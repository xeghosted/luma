// src/net/dispatch.cpp
#include "net/dispatch.h"
#include "net/auth.h"
#include "net/mailbox.h"
#include "net/frame.h"
#include "net/net_server_reply.h"
#include "net/path_guard.h"
#include "rage/invoker/invoker.h"
#include "rage/invoker/hash_natives.h"
#include "game/profile.h"
#include "platform/log.h"
#include "script/lua_runtime.h"
#include "script/loader.h"
#include "script/resource.h"
#include "version.h"

#include <orbis/libkernel.h>
#include <dirent.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// PLUGIN_VERSION comes from version.h, shared with main.cpp, so the version
// reported here in the HELLO reply (and displayed by the editor) can never
// drift from GoldHEN's own plugin metadata.
#define PROTO_VERSION  1

// FreeBSD open(2) flags — the OpenOrbis headers do not expose them. Same set the
// loader already defines; kept local rather than shared so neither unit has to
// include the other.
#define ORBIS_O_RDONLY 0x0000
#define ORBIS_O_WRONLY 0x0001
#define ORBIS_O_CREAT  0x0200
#define ORBIS_O_TRUNC  0x0400
#define ORBIS_SEEK_SET 0
#define ORBIS_SEEK_END 2

// Optional, and absent by default. Its mere existence turns the channel from
// open to closed, which is the whole interface: no setting, no rebuild, just
// a file dropped in over FTP.
// The token file lives in the detected game's data root.
#define TOKEN_PATH (game::current() ? game::current()->token_path : "")
#define TOKEN_MAX  256

namespace net {
namespace {

    // One static handler per op, so a future task adds a case here by adding
    // a handler function above and one `case` line below — nothing else in
    // this file needs to change.

    // Which connection has authenticated, rather than whether one has.
    //
    // A flag would be enough if authentication only ever became true on the
    // connection asking for it. It does not: the game thread can be part-way
    // through an old connection's HELLO while the net thread accepts the next
    // one, and clearing a flag at accept does not stop that in-flight handler
    // from setting it again a moment later -- handing the new, unauthenticated
    // connection someone else's answer.
    //
    // So it is a connection number, in the shape net_server.cpp's g_generation
    // already uses for the same class of problem. on_hello reads the current
    // number BEFORE checking the token and stamps it afterwards, so a
    // connection accepted in between makes that stamp stale rather than
    // granting access. Both are touched by both threads, hence the same
    // acquire/release atomics used for g_client and g_generation; they start
    // unequal, which is what leaves the first connection unauthenticated.
    volatile uint32_t g_conn_gen = 0;
    volatile uint32_t g_authed_gen = 0xFFFFFFFFu;

    inline uint32_t conn_gen()   { return __atomic_load_n(&g_conn_gen, __ATOMIC_ACQUIRE); }
    inline uint32_t authed_gen() { return __atomic_load_n(&g_authed_gen, __ATOMIC_ACQUIRE); }
    inline bool authed_get()     { return authed_gen() == conn_gen(); }
    inline void authed_stamp(uint32_t gen) { __atomic_store_n(&g_authed_gen, gen, __ATOMIC_RELEASE); }

    // What the game's token file holds, resolved once per CONNECTION.
    //
    // Not per message: this runs on the game thread inside the frame hook, and
    // an open() there for every frame that carries traffic is the cost this
    // file caps PUT at 1 MiB to avoid. Not at boot either: the file arrives
    // over FTP, and "drop it in and reconnect" is the only workflow the feature
    // has, so auth_on_connect() throws the answer away and the next connection
    // reads it again.
    volatile int g_token_state = TOKEN_UNKNOWN;
    char   g_token[TOKEN_MAX];
    size_t g_token_len = 0;

    // Read the whole file, or decide it is too big to be a token. sceKernelRead
    // is looped rather than trusted to return everything at once.
    void load_token() {
        if (__atomic_load_n(&g_token_state, __ATOMIC_ACQUIRE) != TOKEN_UNKNOWN)
            return;

        // Same guard as on_hello's, for the same reason: a connection accepted
        // while this reads has already reset the state, and publishing an
        // answer read for the PREVIOUS one would hide a token file dropped in
        // between -- leaving that connection ungated for its whole life.
        const uint32_t gen = conn_gen();
        int state = TOKEN_NONE;
        int fd = sceKernelOpen(TOKEN_PATH, ORBIS_O_RDONLY, 0);
        if (fd >= 0) {
            // Room for the longest token this accepts (TOKEN_MAX - 1, so the
            // NUL fits in g_token), its line ending, and one byte more -- that
            // last byte is what makes a too-long file DETECTABLE rather than
            // silently truncated. Comparing a truncated token would
            // authenticate anyone who sends the first 255 bytes of the secret
            // and reject anyone who sends all of it.
            char buf[TOKEN_MAX + 2];
            size_t got = 0;
            for (;;) {
                ssize_t n = (ssize_t)sceKernelRead(fd, buf + got, sizeof(buf) - got);
                if (n <= 0) break;
                got += (size_t)n;
                if (got >= sizeof(buf)) break;
            }
            sceKernelClose(fd);

            // Trimming, the empty case and the length limit all live in
            // net/auth.h, where tests/net_test.cpp can reach them: every
            // defect this has had was in that decision, not in the read.
            size_t secret = 0;
            switch (token_classify(buf, got, TOKEN_MAX - 1, &secret)) {
            case TOKEN_TOO_LONG:
                // Refused, and the channel stays SHUT: a token nobody can
                // send is the safe reading of "the operator meant to close
                // this". Recoverable without the channel -- rewrite the file
                // over FTP.
                platform::klogf("net: %s is longer than %d bytes; channel closed",
                                TOKEN_PATH, (int)(TOKEN_MAX - 1));
                state = TOKEN_UNUSABLE;
                break;
            case TOKEN_NO_SECRET:
                // An empty file leaves the channel OPEN, which is the
                // opposite of what "the file's existence is the switch"
                // suggests -- so it is said out loud rather than left to be
                // discovered. Closing instead would lock everyone out behind
                // a secret that nothing could ever match.
                platform::klogf("net: %s is empty; channel left OPEN", TOKEN_PATH);
                break;
            case TOKEN_OK:
                memcpy(g_token, buf, secret);
                g_token[secret] = 0;   // secret <= TOKEN_MAX - 1, so this fits
                g_token_len = secret;
                state = TOKEN_PRESENT;
                break;
            }
        }
        if (conn_gen() == gen)
            __atomic_store_n(&g_token_state, state, __ATOMIC_RELEASE);
    }

    // True when a token file is in force -- including one we refuse to use, and
    // including a state we have not managed to establish.
    //
    // Only TOKEN_NONE opens the channel, and that is the whole point: a load
    // that raced with an accept leaves the state UNKNOWN, and reading UNKNOWN as
    // "no token file" would let every op through on exactly the message that
    // lost the race. A client looping connect / EXEC / disconnect could retry
    // that window until it hit. Unknown means shut, which also makes this agree
    // with on_hello, where nothing but TOKEN_PRESENT can authenticate.
    //
    // The cost of being wrong this way is a spurious "unauthenticated" on a
    // console with no token file at all, and only for the one message that
    // raced; the next one retries the load and gets TOKEN_NONE.
    bool token_gate_active() {
        load_token();
        return token_gates(__atomic_load_n(&g_token_state, __ATOMIC_ACQUIRE));
    }

    void on_hello(const message& m) {
        // Read BEFORE the token check, stamped after it: if a new connection is
        // accepted while this handler runs, the stamp lands on a generation
        // that is no longer current and grants that connection nothing.
        const uint32_t gen = conn_gen();

        if (token_gate_active()) {
            // Checked BEFORE the version line is built, so a wrong token
            // learns nothing at all about the build it is talking to.
            // A HELLO with no payload at all is the ordinary case for a
            // client that does not know a token is wanted; m.data is null
            // there, so it is never dereferenced.
            const bool ok = __atomic_load_n(&g_token_state, __ATOMIC_ACQUIRE) == TOKEN_PRESENT
                         && m.data
                         && token_matches(g_token, g_token_len,
                                          (const char*)m.data, m.len);
            if (!ok) {
                reply_err(m.seq, "bad token");
                return;
            }
            authed_stamp(gen);
        }
        char buf[256];
        int n = snprintf(buf, sizeof(buf),
                         "proto=%d\nplugin=%s\nbase=0x%llx\nnatives_ready=%d\n",
                         PROTO_VERSION, PLUGIN_VERSION,
                         (unsigned long long)rage::invoker::g_eboot_base,
                         rage::hash_natives::usable() ? 1 : 0);
        // snprintf returns the length that WOULD have been written, not the
        // (possibly truncated) length actually sitting in buf -- passing that
        // raw value on to reply() as a length is a read-past-the-buffer
        // pattern. Unreachable at the field widths above today, but this is
        // the third time this exact pattern has been flagged in review, so
        // clamp it here once and for all rather than relying on the fields
        // never growing.
        if (n < 0) n = 0;
        else if ((size_t)n >= sizeof(buf)) n = (int)sizeof(buf) - 1;
        reply(OP_HELLO, m.seq, buf, (uint32_t)n);
    }

    void on_ping(const message& m) {
        reply(OP_PONG, m.seq, nullptr, 0);
    }

    // Shared by PUT and GET: scripts are text, and 1 MiB is generous for one.
    // PUT used to be bounded only by the protocol's 8 MB MAX_PAYLOAD and wrote
    // synchronously on the game thread inside a frame hook — a multi-MB write
    // there can stall the game for seconds. GET already capped at this size;
    // PUT now matches it.
    const size_t MAX_FILE_SIZE = 1u << 20;

    // Create every missing directory component between the sandbox root and
    // the file itself. `path` must already be a safe_path()-resolved path —
    // never the raw client string — so every directory created here is
    // guaranteed by safe_path() to land under net::root(). Mutates `path` to
    // carve out each prefix in turn but always restores it before returning.
    // "Already exists" counts as success: the common case is a directory an
    // earlier PUT already created.
    bool ensure_parent_dirs(char* path) {
        size_t root_len = strlen(root());
        size_t plen = strlen(path);
        if (plen <= root_len) return true;  // nothing under the root to create

        for (size_t i = root_len; i < plen; ++i) {
            if (path[i] != '/') continue;
            path[i] = 0;
            int32_t mk = sceKernelMkdir(path, 0777);
            path[i] = '/';
            if (mk < 0 && (uint32_t)mk != ORBIS_KERNEL_ERROR_EEXIST) return false;
        }
        return true;
    }

    // "path\0<bytes>" — the payload's first NUL splits name from content.
    void on_put(const message& m) {
        if (!m.len) { reply_err(m.seq, "empty payload"); return; }
        const char* name = (const char*)m.data;
        size_t nlen = strnlen(name, m.len);
        if (nlen == m.len) { reply_err(m.seq, "payload has no NUL separator"); return; }

        char path[512], tmp[520];
        if (!safe_path(name, path, sizeof(path))) { reply_err(m.seq, "bad path"); return; }

        const uint8_t* body = m.data + nlen + 1;
        size_t         blen = m.len - nlen - 1;
        if (blen > MAX_FILE_SIZE) { reply_err(m.seq, "payload too large (max 1 MiB)"); return; }

        if (!ensure_parent_dirs(path)) { reply_err(m.seq, "mkdir failed"); return; }
        snprintf(tmp, sizeof(tmp), "%s.tmp", path);

        int fd = sceKernelOpen(tmp, ORBIS_O_WRONLY | ORBIS_O_CREAT | ORBIS_O_TRUNC, 0666);
        if (fd < 0) { reply_err(m.seq, "open failed"); return; }
        bool ok = blen == 0 || sceKernelWrite(fd, body, blen) == (ssize_t)blen;
        sceKernelClose(fd);
        if (!ok) { sceKernelUnlink(tmp); reply_err(m.seq, "write failed"); return; }
        if (sceKernelRename(tmp, path) < 0) { sceKernelUnlink(tmp); reply_err(m.seq, "rename failed"); return; }

        char msg[64];
        snprintf(msg, sizeof(msg), "wrote %u bytes", (unsigned)blen);
        reply_ok(m.seq, msg);
    }

    // Shared by GET, LS and RM: their payload is nothing but a path, copied
    // into a caller-provided stack buffer (`name`, conventionally char[256])
    // and NUL-terminated. A payload that does not fit is an ERROR, not
    // something to clamp: silently truncating it would hand the handler a
    // different, syntactically valid path than the one the client actually
    // asked about -- turning e.g. an RM for the truncated name into a
    // deletion of the wrong file, with the client never told. Already
    // replies (reply_err) and returns false on that path, so every call site
    // can just check the return value and return.
    bool decode_short_name(const message& m, char* name, size_t name_cap) {
        if (m.len >= name_cap) { reply_err(m.seq, "payload too long"); return false; }
        if (m.len && m.data) memcpy(name, m.data, m.len);
        name[m.len] = 0;
        return true;
    }

    // The three lifecycle ops and RESINFO all carry a bare resource name, and
    // all four used to refuse an oversized one with decode_short_name's
    // generic "payload too long". True, but unhelpful when the payload IS the
    // name: the console session that hit this with a 183-character resource
    // got a message about payloads for what is really a naming rule. Say
    // which limit was crossed, and read the number from the constant rather
    // than keep a second copy of it inside a string.
    bool decode_resource_name(const message& m, char* name, size_t name_cap) {
        if (m.len > (uint32_t)RESOURCE_NAME_MAX) {
            char msg[64];
            snprintf(msg, sizeof(msg), "resource name too long (max %d bytes)", RESOURCE_NAME_MAX);
            reply_err(m.seq, msg);
            return false;
        }
        return decode_short_name(m, name, name_cap);
    }

    void on_get(const message& m) {
        char name[256], path[512];
        if (!decode_short_name(m, name, sizeof(name))) return;
        if (!safe_path(name, path, sizeof(path))) { reply_err(m.seq, "bad path"); return; }

        int fd = sceKernelOpen(path, ORBIS_O_RDONLY, 0);
        if (fd < 0) { reply_err(m.seq, "not found"); return; }

        off_t size = sceKernelLseek(fd, 0, ORBIS_SEEK_END);
        if (size < 0) { sceKernelClose(fd); reply_err(m.seq, "seek failed"); return; }
        if (sceKernelLseek(fd, 0, ORBIS_SEEK_SET) < 0) {
            sceKernelClose(fd); reply_err(m.seq, "seek failed"); return;
        }
        if ((size_t)size > MAX_FILE_SIZE) { sceKernelClose(fd); reply_err(m.seq, "too large"); return; }

        char* buf = (char*)malloc((size_t)size + 1);
        if (!buf) { sceKernelClose(fd); reply_err(m.seq, "oom"); return; }
        ssize_t got = sceKernelRead(fd, buf, (size_t)size);
        sceKernelClose(fd);
        // A short read must not be handed back as a complete file: the client
        // would write a truncated script to disk believing it intact.
        if (got < 0 || (size_t)got != (size_t)size) { free(buf); reply_err(m.seq, "read failed"); return; }
        reply(OP_OK, m.seq, buf, (uint32_t)got);
        free(buf);
    }

    void on_ls(const message& m) {
        char name[256], path[512];
        if (!decode_short_name(m, name, sizeof(name))) return;
        if (name[0] == 0) { snprintf(path, sizeof(path), "%s", root()); }
        else if (!safe_path(name, path, sizeof(path))) { reply_err(m.seq, "bad path"); return; }

        int dfd = sceKernelOpen(path, ORBIS_O_RDONLY, 0);
        if (dfd < 0) { reply_err(m.seq, "not found"); return; }

        // Heap, not a 2048-byte stack buffer: a directory of roughly a hundred
        // scripts already overflows that, silently dropping entries while
        // still answering OP_OK. net::reply() copies the payload internally,
        // so there's no reason to size this tighter than comfortable — 64 KB
        // covers any script directory this plugin will realistically see.
        const size_t LS_CAP     = 64 * 1024;
        const char*  TRUNC_MARK = "-- truncated: listing incomplete --\n";
        const size_t TRUNC_LEN  = strlen(TRUNC_MARK);
        // Room for the marker is reserved up front so it can always be
        // appended when needed, rather than discovered not to fit after the
        // buffer is already completely full.
        const size_t LS_LIMIT = LS_CAP - TRUNC_LEN;

        char* out = (char*)malloc(LS_CAP);
        if (!out) { sceKernelClose(dfd); reply_err(m.seq, "oom"); return; }

        size_t used = 0;
        bool truncated = false;
        char dents[4096]; int nb;
        while (!truncated && (nb = sceKernelGetdents(dfd, dents, sizeof(dents))) > 0) {
            int off = 0;
            while (off < nb) {
                struct dirent* de = (struct dirent*)(dents + off);
                if (de->d_reclen == 0) break;
                size_t l = strlen(de->d_name);
                if (used + l + 1 <= LS_LIMIT) {
                    memcpy(out + used, de->d_name, l); used += l;
                    out[used++] = '\n';
                } else {
                    truncated = true;
                    break;
                }
                off += de->d_reclen;
            }
        }
        sceKernelClose(dfd);

        // Never let the client mistake a partial answer for a complete one.
        if (truncated) {
            memcpy(out + used, TRUNC_MARK, TRUNC_LEN);
            used += TRUNC_LEN;
        }

        reply(OP_OK, m.seq, out, (uint32_t)used);
        free(out);
    }

    void on_rm(const message& m) {
        char name[256], path[512];
        if (!decode_short_name(m, name, sizeof(name))) return;
        if (!safe_path(name, path, sizeof(path))) { reply_err(m.seq, "bad path"); return; }
        if (sceKernelUnlink(path) < 0) { reply_err(m.seq, "unlink failed"); return; }
        reply_ok(m.seq, "removed");
    }

    // "chunkname\0<lua source>"
    void on_exec(const message& m) {
        if (!m.len) { reply_err(m.seq, "empty payload"); return; }
        const char* name = (const char*)m.data;
        size_t nlen = strnlen(name, m.len);
        if (nlen == m.len) { reply_err(m.seq, "payload has no NUL separator"); return; }

        const char* src = (const char*)m.data + nlen + 1;
        size_t      slen = m.len - nlen - 1;

        // Spec 4.2: "run a chunk now; reply carries the result or the error".
        // The console's REPL depends on it -- consoleinput.ts rewrites "=expr"
        // into "return expr" specifically so the value lands in this payload.
        // 4 KB matches on_reslist's reply buffer; anything longer comes back
        // cut with EXEC_RESULT_TRUNC_MARK on the end, so the client is never
        // handed a piece of a value that reads like the whole of one.
        char err[512];
        char val[4096];
        script::exec_result res = { val, sizeof(val), 0, 0, false, false };
        if (!script::runtime_exec_capture(name, src, slen, err, sizeof(err), nullptr, &res)) {
            reply_err(m.seq, err);
            return;
        }
        // A chunk that genuinely returns nothing still answers "ok": a
        // successful statement ("x = 1", or a whole pushed file) must not
        // come back as an empty reply the user cannot tell from a dropped
        // one. Zero bytes here means the chunk returned an empty string,
        // which is a value, not a silence.
        if (res.nresults == 0) reply_ok(m.seq, "ok");
        else                   reply(OP_OK, m.seq, val, (uint32_t)res.len);
    }

    void on_reload(const message& m) {
        int n = script::script_reload();
        char msg[64];
        snprintf(msg, sizeof(msg), "%d script(s) reloaded", n);
        reply_ok(m.seq, msg);
    }

    // The three lifecycle ops differ only in which façade call they make, so
    // they share a body rather than triplicating the reply handling.
    typedef bool (*lifecycle_fn)(const char*, char*, size_t);

    void on_lifecycle(const message& m, lifecycle_fn fn, const char* verb) {
        char name[RESOURCE_NAME_MAX + 1];
        if (!decode_resource_name(m, name, sizeof(name))) return;
        char err[256];
        if (fn(name, err, sizeof(err))) {
            char msg[192];
            int w = snprintf(msg, sizeof(msg), "%s %s", verb, name);
            if (w < 0) w = 0;
            if ((size_t)w > sizeof(msg) - 1) w = (int)sizeof(msg) - 1;
            reply(OP_OK, m.seq, msg, (uint32_t)w);
        } else {
            reply_err(m.seq, err);
        }
    }

    void on_reslist(const message& m) {
        // resource_list() alone can't tell the editor's tree apart from an
        // empty runtime: 0 bytes back means either "no resources" or "not
        // booted yet", and both would otherwise render as an empty list with
        // no way to tell a broken boot from a clean one. Ask the façade
        // directly rather than guess from the byte count.
        if (!script::resource_is_booted()) { reply_err(m.seq, "resource runtime not booted"); return; }
        char out[4096];
        int n = script::resource_list(out, sizeof(out));
        reply(OP_OK, m.seq, out, (uint32_t)(n < 0 ? 0 : n));
    }

    void on_resinfo(const message& m) {
        char name[RESOURCE_NAME_MAX + 1];
        if (!decode_resource_name(m, name, sizeof(name))) return;
        // "no such resource" used to be the answer to all three of: the
        // resource runtime never booted, the Lua call raised, and the name
        // genuinely isn't there. on_reslist two functions above deliberately
        // separates the not-booted case for exactly this reason -- an empty
        // or missing answer that has more than one cause needs to say which
        // one -- and resource_info now carries the reason out itself, so the
        // two handlers finally agree.
        char out[128], err[192];
        if (script::resource_info(name, out, sizeof(out), err, sizeof(err)))
            reply_ok(m.seq, out);
        else
            reply_err(m.seq, err);
    }

    void on_cmd(const message& m) {
        char raw[512];
        if (!decode_short_name(m, raw, sizeof(raw))) return;
        char err[256];
        if (script::resource_command(raw, err, sizeof(err)))
            reply_ok(m.seq, "ok");
        else
            reply_err(m.seq, err);
    }

}  // namespace

// Called by the net thread when a client is accepted, so a new connection
// never inherits the previous one's authentication -- not even from a HELLO
// the game thread is still processing as this runs, which a plain flag could
// not have prevented (see the generation comment above g_conn_gen).
//
// The cached token goes with it, so a file dropped in over FTP takes effect on
// the next connection rather than the next boot.
void auth_on_connect() {
    __atomic_add_fetch(&g_conn_gen, 1, __ATOMIC_ACQ_REL);
    __atomic_store_n(&g_token_state, TOKEN_UNKNOWN, __ATOMIC_RELEASE);
}

void dispatch_pending() {
    message m;
    while (inbox_pop(&m)) {
        // The gate, ahead of every handler: with a token file present, an
        // unauthenticated connection may say hello and ping, and nothing
        // else. See net/auth.h for why gating HELLO alone would not be enough.
        // token_gate_active() reads the file at most once per CONNECTION, and
        // an authenticated one never asks at all, so a frame carrying traffic
        // costs no syscall here.
        const bool authed = authed_get();
        if (!authed) {
            if (!auth_allows(m.op, token_gate_active(), authed)) {
                reply_err(m.seq, "unauthenticated");
                if (m.data) free(m.data);
                continue;
            }
        }
        switch (m.op) {
        case OP_HELLO:  on_hello(m); break;
        case OP_PING:   on_ping(m);  break;
        case OP_PUT:    on_put(m);   break;
        case OP_GET:    on_get(m);   break;
        case OP_LS:     on_ls(m);    break;
        case OP_RM:     on_rm(m);    break;
        case OP_EXEC:   on_exec(m);   break;
        case OP_RELOAD: on_reload(m); break;
        case OP_START:   on_lifecycle(m, script::resource_start,   "started");   break;
        case OP_STOP:    on_lifecycle(m, script::resource_stop,    "stopped");   break;
        case OP_RESTART: on_lifecycle(m, script::resource_restart, "restarted"); break;
        case OP_RESLIST: on_reslist(m);  break;
        case OP_RESINFO: on_resinfo(m);  break;
        case OP_CMD:     on_cmd(m);      break;
        default:
            reply_err(m.seq, "unknown op");
            break;
        }
        // m.data is ours from inbox_pop; free it on every path out of the
        // loop body, including a handler that bails early.
        if (m.data) free(m.data);
    }
}

}  // namespace net
