// src/net/net_server.cpp
#include "net/net_server.h"
#include "game/profile.h"
#include "net/frame.h"
#include "net/dispatch.h"
#include "net/mailbox.h"
#include "net/net_server_reply.h"
#include "platform/log.h"

#include <orbis/libkernel.h>
#include <orbis/Net.h>
#include <stdlib.h>
#include <string.h>

// No STL: synchronisation on the shared socket descriptors uses clang's
// __atomic builtins, matching src/game/game_thread.cpp's spinlock idiom.

namespace net {
namespace {

    // From the profile: 9615 for GTA V, 9616 for RDR2, so both plugins can
    // listen on one console without the editor reaching the wrong game.
    inline int port() { return game::current() ? game::current()->port : 0; }
    const size_t   STACK_SIZE = 256 * 1024;

    // Not exposed by orbis/Net.h. Verified on console in this task's Step 6.
    const int32_t NET_SOL_SOCKET  = 0xffff;
    const int32_t NET_SO_REUSEADDR = 0x0004;
    const int32_t NET_SO_NBIO      = 0x1200;

    // orbis/Net.h declares this as `void sceNetErrnoLoc()` -- a generated-
    // header placeholder, not the real ABI. The actual libSceNet symbol
    // returns a pointer to the calling thread's socket-library errno cell
    // (BSD's __error() shape). Re-declared here under a local name bound to
    // the same linker symbol via an asm label, so we get the correct type
    // without conflicting with the header's own (wrong) declaration.
    extern "C" int* net_sce_errno_loc(void) asm("sceNetErrnoLoc");

    // Errno values this code positively recognises as fatal for a connection.
    // Deliberately an ALLOWLIST, not a blocklist: sceNetErrnoLoc()'s numbering
    // is unverified on this firmware two ways over. First, whether it uses
    // POSIX errno numbering at all -- Sony's networking layer can instead
    // report errors in its own 0x8041xxxx SCE_NET_ERROR_* space. Second, even
    // if it is POSIX-style, these particular FreeBSD values (this toolchain's
    // own <errno.h> agrees with them) were, at the time this list was
    // written, a guess rather than a confirmed fact about the running
    // kernel. Getting the "retry" set wrong (the previous version of this
    // fix) makes every ordinary would-block look fatal and drops the
    // connection after the first request; getting this allowlist wrong just
    // means an unrecognised value -- POSIX or 0x8041xxxx alike -- falls
    // through to "retry" and is caught by the deadline below instead, which
    // is a bounded, recoverable delay rather than a channel that looks
    // permanently broken. That fallthrough is intentional, not an oversight.
    //
    // 54 (ECONNRESET) below has since been CONFIRMED against this firmware in
    // a live console session (see NET_RETRY_ERRNOS just below, confirmed the
    // same way); the rest of this table is still the original inference.
    const int NET_FATAL_ERRNOS[] = {
        9,    // EBADF
        32,   // EPIPE
        53,   // ECONNABORTED
        54,   // ECONNRESET -- confirmed against this firmware in a live console session
        57,   // ENOTCONN
        60,   // ETIMEDOUT
    };
    const int NET_FATAL_ERRNO_COUNT = sizeof(NET_FATAL_ERRNOS) / sizeof(NET_FATAL_ERRNOS[0]);

    // Errno values this code positively recognises as retryable -- i.e.
    // ordinary, expected outcomes of a non-blocking recv()/send() that must
    // never be treated as the "unrecognised" case below. Kept as its own
    // named table, next to the fatal one above, rather than folded into the
    // fallthrough: without it, EWOULDBLOCK -- the single most common errno
    // this code ever sees, since every non-blocking socket reports it
    // whenever there is simply nothing to read yet -- burns the one-shot
    // "unrecognised" log latch (g_retry_errno_logged_recv/_send) on the very
    // first recv() of a connection, and a genuinely unknown errno later on
    // that same connection then goes unreported. Recognising it here instead
    // means the latch stays available for what it exists to catch.
    //
    // 35 (EWOULDBLOCK, same value as EAGAIN on this ABI) was CONFIRMED
    // against this firmware in a live console session -- this is the FreeBSD
    // numbering the fatal table above also uses, and 54 (ECONNRESET) was
    // confirmed the same way. 4 (EINTR) has not been independently observed
    // here; it is standard FreeBSD numbering and a signal interrupting the
    // call is unambiguously a retry regardless of numbering scheme, so it is
    // listed as recognised-but-inferred rather than confirmed.
    const int NET_RETRY_ERRNOS[] = {
        4,    // EINTR -- inferred (standard FreeBSD numbering, not independently measured here)
        35,   // EWOULDBLOCK / EAGAIN -- confirmed against this firmware in a live console session
    };
    const int NET_RETRY_ERRNO_COUNT = sizeof(NET_RETRY_ERRNOS) / sizeof(NET_RETRY_ERRNOS[0]);

    // Bounds on how long serve()/send_all() wait for progress before giving
    // up on a connection -- the actual guarantee that nothing can wedge the
    // channel open forever, whether that is a genuine but unrecognised fatal
    // error, or libSceNet reporting would-block in a way the allowlist above
    // was never meant to catch. The allowlist is only a fast path for an
    // error positively identified as fatal; everything else waits out one of
    // these deadlines instead.
    //
    // IDLE_US covers traffic in EITHER direction: a successful recv and a
    // successful send both reset it. The channel's main client is a log
    // tailer that mostly just receives OP_LOG frames while otherwise silent;
    // resetting only on inbound bytes closed that connection every 30s even
    // though the channel was working and data was flowing the whole time.
    // This does not weaken the guarantee against a genuinely dead peer: once
    // a peer stops reading, the kernel send buffer fills, sends stop making
    // progress, and SEND_STALL_US -- which only recv cannot reset -- fires
    // on its own. Do not narrow this back to recv-only; that was tried and
    // it drops healthy long-lived connections.
    const uint64_t IDLE_US        = 30ull * 1000000ull;   // 30s with no traffic either way
    const uint64_t SEND_STALL_US  = 10ull * 1000000ull;   // 10s with no bytes out

    // Set false at the start of each connection (thread_main, just before
    // serve()) and latched true the first time a retry-classified errno is
    // logged for that direction on that connection -- see
    // net_error_is_retryable() below. Two separate latches (recv and send)
    // so one direction firing first does not permanently silence the
    // other diagnostic -- that log line is the only way we will ever learn
    // which errno numbering this firmware actually uses. Only ever touched
    // by the net thread, so neither needs atomic treatment.
    bool g_retry_errno_logged_recv = false;
    bool g_retry_errno_logged_send = false;

    struct sockaddr_in_be {
        uint8_t  len;
        uint8_t  family;
        uint16_t port;          // network order
        uint32_t addr;          // network order
        uint8_t  zero[8];
    };

    // g_listen and g_client are written on the net thread (on accept/close) and
    // read from whichever thread calls server_stop, with nothing else between
    // them - every cross-thread touch goes through the fd_get/fd_set helpers
    // below so neither is a torn or stale read.
    volatile OrbisNetId g_listen = -1;
    volatile OrbisNetId g_client = -1;
    // OrbisPthread (pthread_t) is an integer handle on this toolchain, not a
    // pointer, so 0 (never a real handle) is the "no thread" sentinel. Only
    // touched by the control side (server_start/server_stop callers), never
    // the net thread itself, so it does not need the same treatment.
    OrbisPthread  g_thread = 0;
    volatile int  g_stop   = 0;

    inline OrbisNetId fd_get(volatile OrbisNetId* p) {
        return (OrbisNetId)__atomic_load_n(p, __ATOMIC_ACQUIRE);
    }
    inline void fd_set(volatile OrbisNetId* p, OrbisNetId v) {
        __atomic_store_n(p, v, __ATOMIC_RELEASE);
    }

    // Session generation: bumped once per accepted connection, in
    // thread_main, before serve() runs for that connection. reply() -- on
    // the game thread, whenever a dispatched request completes -- stamps
    // whatever generation is current AT THAT MOMENT into the reply it
    // queues; flush() -- on the net thread -- compares each entry's stamp
    // against the current generation and drops (frees, counts) anything
    // that does not match, instead of sending it.
    //
    // This exists because a request can sit in the inbox for roughly a
    // frame before the game thread's dispatch_pending() picks it up: a
    // client can send a request, disconnect, and have a different client
    // already connected and reading replies by the time the first
    // client's reply is finally queued. Both clients start their own seq
    // numbering at 1, so without generation tagging that stale reply could
    // silently resolve the new session's first request with the old
    // session's answer, rather than being merely dropped.
    //
    // Cross-thread (the net thread writes it and reads it in flush(); the
    // game thread reads it in reply()), so every touch goes through the
    // same acquire/release atomic idiom as g_listen/g_client.
    volatile uint32_t g_generation = 0;

    inline uint32_t gen_get() {
        return __atomic_load_n(&g_generation, __ATOMIC_ACQUIRE);
    }
    inline void gen_bump() {
        // Only ever called from the net thread, one connection at a time,
        // so this read-modify-write has no concurrent writer to race with;
        // the atomic store is what makes the new value visible to reply()
        // on the game thread.
        __atomic_store_n(&g_generation, gen_get() + 1, __ATOMIC_RELEASE);
    }

    // Reply queue, drained by the connection loop. Separate from the outbox
    // because a reply carries an op and a seq, while the outbox is only
    // lines. Payloads are heap-carried (mirroring the inbox message's
    // ownership shape) rather than copied into a fixed-size slot: a reply
    // too big for a fixed slot would have to be truncated with no way for
    // the client to ask for the rest, which made GET unimplementable.
    //
    // Declared here, with the other file-scope statics, so both flush()
    // below and net::reply()/reply_ok()/reply_err() after the anonymous
    // namespace can see them.
    struct pending_reply {
        uint8_t  op;
        uint8_t  flags;
        uint16_t seq;
        uint32_t len;
        uint32_t generation;   // stamped by reply() at queue time; flush() drops a mismatch
        uint8_t* data;         // malloc'd; null when len == 0. Queue-owned until flush() sends and frees it.
    };
    // At most INBOX_CAP (16) requests can be in flight at once, so one
    // dispatch pass produces at most 16 replies; 32 makes overflow
    // structurally unreachable from request/reply pairing rather than merely
    // unlikely. Slots are now a header plus one pointer, so the larger cap
    // costs less memory than the old fixed-1KB-per-slot version did.
    const int REPLY_CAP = 32;
    pending_reply g_replies[REPLY_CAP];
    int           g_reply_head = 0;
    int           g_reply_count = 0;
    int           g_reply_dropped = 0;
    volatile int  g_reply_lock = 0;

    // NET_SO_NBIO is one of the three unverified constants above; a failure
    // here is logged (naming which socket it was for) but not fatal, so the
    // console gate this task exists for still runs far enough to be seen.
    void set_nonblocking(OrbisNetId s, const char* which) {
        int on = 1;
        int r = sceNetSetsockopt(s, NET_SOL_SOCKET, NET_SO_NBIO, &on, sizeof(on));
        if (r < 0)
            platform::klogf("net: setsockopt(SO_NBIO, %s) failed %d", which, r);
    }

    bool net_errno_is_fatal(int err) {
        for (int i = 0; i < NET_FATAL_ERRNO_COUNT; i++)
            if (err == NET_FATAL_ERRNOS[i]) return true;
        return false;
    }

    bool net_errno_is_recognised_retry(int err) {
        for (int i = 0; i < NET_RETRY_ERRNO_COUNT; i++)
            if (err == NET_RETRY_ERRNOS[i]) return true;
        return false;
    }

    // Classifies a negative sceNetRecv()/sceNetSend() result against the
    // fatal allowlist and the retryable allowlist above. A recognised-fatal
    // errno closes the connection immediately, logged with its raw value. A
    // recognised-retryable errno (EWOULDBLOCK/EAGAIN, EINTR) retries
    // silently, exactly as expected of an ordinary non-blocking call, and
    // never touches *logged. Only a value in NEITHER table -- genuinely
    // unrecognised -- retries AND spends the one-shot latch: the first such
    // value seen on this connection for this direction is logged once
    // through *logged (recv and send each own latch, so one firing does not
    // silence the other), not on every iteration (this sits in a ~200 Hz
    // loop and must not flood the kernel log). Without the retryable table,
    // EWOULDBLOCK -- the single most common errno this code ever sees --
    // would burn that latch on the very first recv() of a connection, and a
    // genuinely unknown errno arriving later on that same connection would
    // then never be reported; that defeats the entire point of the latch, so
    // recognised-retryable values must never trip it.
    //
    // net_sce_errno_loc() is a diagnostic helper; a null return must not
    // crash the connection it is trying to help debug, so it is guarded and
    // treated the same as any other unrecognised value: retry.
    bool net_error_is_retryable(const char* what, bool* logged) {
        int* loc = net_sce_errno_loc();
        if (!loc) {
            if (!*logged) {
                *logged = true;
                platform::klogf("net: %s: sceNetErrnoLoc() returned null, treating as retry", what);
            }
            return true;
        }
        int err = *loc;
        if (net_errno_is_fatal(err)) {
            platform::klogf("net: %s failed, errno %d, closing", what, err);
            return false;
        }
        if (net_errno_is_recognised_retry(err)) {
            return true;
        }
        if (!*logged) {
            *logged = true;
            platform::klogf("net: %s returned errno %d, unrecognised, treating as retry", what, err);
        }
        return true;
    }

    // `traffic` is the connection-wide "progress in either direction" clock
    // (see IDLE_US above); every successful partial send refreshes it, on
    // top of this function's own local stall clock.
    bool send_all(OrbisNetId c, const void* p, size_t n, uint64_t* traffic) {
        const uint8_t* b = (const uint8_t*)p;
        size_t sent = 0;
        uint64_t last_progress = sceKernelGetProcessTime();
        while (sent < n) {
            if (g_stop) return false;
            int r = sceNetSend(c, b + sent, n - sent, 0);
            if (r > 0) {
                sent += (size_t)r;
                last_progress = sceKernelGetProcessTime();
                *traffic = last_progress;
            } else if (r == 0) {
                return false;
            } else {
                if (!net_error_is_retryable("send()", &g_retry_errno_logged_send)) return false;
                uint64_t now = sceKernelGetProcessTime();
                if (now - last_progress > SEND_STALL_US) {
                    platform::klogf("net: send() stalled for %llu us, closing",
                                    (unsigned long long)(now - last_progress));
                    return false;
                }
                sceKernelUsleep(1000);   // would-block; try again
            }
        }
        return true;
    }

    bool send_frame(OrbisNetId c, uint8_t op, uint8_t flags, uint16_t seq,
                    const void* payload, uint32_t len, uint64_t* traffic) {
        uint8_t hdr[net::HEADER_SIZE];
        net::header h = { op, flags, seq, len };
        net::encode_header(hdr, h);
        if (!send_all(c, hdr, sizeof(hdr), traffic)) return false;
        if (len && !send_all(c, payload, len, traffic)) return false;
        return true;
    }

    // Appends to the reply ring, taking ownership of `data` (already
    // malloc'd, or null). On overflow -- structurally unreachable from normal
    // request/reply pairing, see REPLY_CAP above, but handled rather than
    // assumed away -- frees data and counts and logs the drop instead of
    // leaving the caller waiting on a seq that will never come.
    void push_reply(uint8_t op, uint16_t seq, uint8_t* data, uint32_t len, uint32_t generation) {
        while (__atomic_exchange_n(&g_reply_lock, 1, __ATOMIC_ACQUIRE)) { }
        if (g_reply_count < REPLY_CAP) {
            pending_reply* r = &g_replies[(g_reply_head + g_reply_count) % REPLY_CAP];
            r->op = op; r->flags = 0; r->seq = seq; r->len = len; r->generation = generation; r->data = data;
            g_reply_count++;
            __atomic_store_n(&g_reply_lock, 0, __ATOMIC_RELEASE);
        } else {
            g_reply_dropped++;
            __atomic_store_n(&g_reply_lock, 0, __ATOMIC_RELEASE);
            platform::klogf("net: reply queue full (%d dropped so far), op %u seq %u",
                            g_reply_dropped, op, seq);
            if (data) free(data);
        }
    }

    // Frees every payload still queued and empties the ring. Called after
    // serve() returns (so the next accepted connection always starts with an
    // empty queue -- a reply queued for a session that just ended must never
    // be delivered to whoever connects next, since it would carry that dead
    // session's seq numbers) and again after server_stop()'s join (so a
    // stopped server does not leak up to REPLY_CAP payloads for the rest of
    // the process's life).
    //
    // Concurrency: flush() cannot be running at either call site. The net
    // thread is single-threaded and flush() is only ever called from inside
    // serve()'s loop, which has already returned by the first call site; by
    // the second call site the thread has been joined and no longer exists.
    // The game thread's reply()/push_reply() can still run concurrently with
    // either call, but both take g_reply_lock for their entire critical
    // section, so there is no torn read and no double free. What this does
    // NOT close: a reply pushed in the narrow window right after a drain
    // completes still waits for whichever connection comes next (or leaks
    // permanently after server_stop(), since nothing will ever pop it
    // again). That is a consequence of draining at a wall-clock connection
    // boundary rather than fencing replies by connection identity, not a
    // new memory-safety issue, and closing it is out of scope here.
    void drain_replies() {
        while (__atomic_exchange_n(&g_reply_lock, 1, __ATOMIC_ACQUIRE)) { }
        for (int i = 0; i < g_reply_count; i++) {
            pending_reply* r = &g_replies[(g_reply_head + i) % REPLY_CAP];
            if (r->data) free(r->data);
            r->data = nullptr;
        }
        g_reply_head  = 0;
        g_reply_count = 0;
        __atomic_store_n(&g_reply_lock, 0, __ATOMIC_RELEASE);
    }

    // Pops and frees every message still sitting in the inbox. Called right
    // after drain_replies() at both of its call sites: a request that
    // arrived on a connection that has already ended is worthless the
    // moment that connection ends, since nobody is waiting for its answer
    // any more. Dispatching it late and replying to it is exactly what let
    // a stale reply reach a later connection (see g_generation's comment);
    // removing the request itself, rather than only tagging the reply it
    // would eventually produce, closes that at the source instead of racing
    // it. Uses net::inbox_pop()'s documented ownership contract (true means
    // the caller now owns out->data) -- no new mailbox API needed.
    //
    // This leaves one genuinely tiny window, accepted rather than chased
    // further: the game thread already inside dispatch_pending(), holding a
    // message it popped microseconds before this drain runs. That message
    // is gone from the inbox by the time this returns, but the reply it
    // produces is still handled by generation tagging in flush(), same as
    // any other in-flight reply.
    void drain_inbox() {
        net::message m;
        while (net::inbox_pop(&m)) {
            if (m.data) free(m.data);
        }
    }

    // Drain both queues onto the wire. Returns false if the connection died.
    // *did_work is set to true when anything was actually sent, so serve()
    // can skip its idle sleep on a busy connection. *traffic is refreshed by
    // every successful send (see send_all()), so a connection that only ever
    // sends (a log tailer) still counts as alive for IDLE_US. *stale_count is
    // incremented for every entry dropped because it was generated for a
    // connection other than this one (see g_generation above) -- the caller
    // logs the total once per connection rather than once per entry.
    bool flush(OrbisNetId c, bool* did_work, uint64_t* traffic, uint32_t* stale_count) {
        uint32_t generation = gen_get();
        for (;;) {
            pending_reply r;
            while (__atomic_exchange_n(&g_reply_lock, 1, __ATOMIC_ACQUIRE)) { }
            bool have = g_reply_count > 0;
            if (have) {
                r = g_replies[g_reply_head];
                g_reply_head = (g_reply_head + 1) % REPLY_CAP;
                g_reply_count--;
            }
            __atomic_store_n(&g_reply_lock, 0, __ATOMIC_RELEASE);
            if (!have) break;

            if (r.generation != generation) {
                // Generated for a connection other than this one -- most
                // often a session that has already ended, per the comment on
                // g_generation above. Delivering it here could resolve this
                // connection's own request with someone else's answer, so it
                // is dropped rather than sent.
                if (r.data) free(r.data);
                (*stale_count)++;
                continue;
            }

            *did_work = true;
            bool ok = send_frame(c, r.op, r.flags, r.seq, r.data, r.len, traffic);
            if (r.data) free(r.data);
            if (!ok) return false;
        }

        char line[net::OUTBOX_LINE];
        while (net::outbox_pop_line(line)) {
            *did_work = true;
            uint32_t n = (uint32_t)strlen(line);
            if (!send_frame(c, net::OP_LOG, 0, 0, line, n, traffic)) return false;
        }
        return true;
    }

    // Shared by both recv sites in serve(): true means keep waiting, false
    // means close the connection (already logged by whichever check failed).
    // *traffic is the same clock send_all() refreshes, so this checks "no
    // traffic in either direction", not "no bytes received" -- see IDLE_US.
    bool recv_should_retry(uint64_t* traffic) {
        if (!net_error_is_retryable("recv()", &g_retry_errno_logged_recv)) return false;
        uint64_t now = sceKernelGetProcessTime();
        if (now - *traffic > IDLE_US) {
            platform::klogf("net: no traffic for %llu us, closing",
                            (unsigned long long)(now - *traffic));
            return false;
        }
        return true;
    }

    // Replaces the Task 1 echo loop. Reads length-prefixed frames off a
    // non-blocking socket into the mailbox; drains replies and log lines back
    // out each pass. Never calls a native, never touches Lua.
    void serve(OrbisNetId c) {
        uint8_t  hdr[net::HEADER_SIZE];
        size_t   hdr_got = 0;
        net::header h = { 0, 0, 0, 0 };
        uint8_t* body = nullptr;
        size_t   body_got = 0;
        bool     in_body = false;
        uint64_t last_traffic = sceKernelGetProcessTime();
        uint32_t stale_replies = 0;

        for (;;) {
            if (g_stop) break;
            bool did_work = false;

            if (!in_body) {
                int n = sceNetRecv(c, hdr + hdr_got, net::HEADER_SIZE - hdr_got, 0);
                if (n == 0) {
                    break;                       // peer closed
                } else if (n > 0) {
                    did_work = true;
                    last_traffic = sceKernelGetProcessTime();
                    hdr_got += (size_t)n;
                    if (hdr_got == net::HEADER_SIZE) {
                        hdr_got = 0;
                        if (!net::decode_header(hdr, &h)) {
                            platform::klogf("net: malformed frame, closing");
                            break;
                        }
                        if (h.len) {
                            body = (uint8_t*)malloc(h.len);
                            if (!body) { platform::klogf("net: oom on %u bytes", h.len); break; }
                            body_got = 0;
                            in_body = true;
                        } else {
                            net::message m = { h.op, h.seq, 0, nullptr };
                            if (!net::inbox_push(m))
                                send_frame(c, net::OP_BUSY, 0, h.seq, nullptr, 0, &last_traffic);
                        }
                    }
                } else if (!recv_should_retry(&last_traffic)) {
                    break;
                }
            } else {
                int n = sceNetRecv(c, body + body_got, h.len - body_got, 0);
                if (n == 0) {
                    break;                       // peer closed
                } else if (n > 0) {
                    did_work = true;
                    last_traffic = sceKernelGetProcessTime();
                    body_got += (size_t)n;
                    if (body_got == h.len) {
                        in_body = false;
                        net::message m = { h.op, h.seq, h.len, body };
                        if (!net::inbox_push(m)) {
                            free(body);
                            send_frame(c, net::OP_BUSY, 0, h.seq, nullptr, 0, &last_traffic);
                        }
                        body = nullptr;
                    }
                } else if (!recv_should_retry(&last_traffic)) {
                    break;
                }
            }

            if (!flush(c, &did_work, &last_traffic, &stale_replies)) break;
            if (!did_work) sceKernelUsleep(5000);
        }

        if (stale_replies)
            platform::klogf("net: dropped %u stale repl(ies) meant for a different connection",
                            stale_replies);
        if (body) free(body);
    }

    void* thread_main(void*) {
        platform::klogf("net: thread up");
        while (!g_stop) {
            OrbisNetId listen_fd = fd_get(&g_listen);
            OrbisNetId c = sceNetAccept(listen_fd, nullptr, nullptr);
            if (c >= 0) {
                platform::klogf("net: client connected");
                // Before the fd is published, so the game thread cannot pop a
                // message from this connection while the previous one's
                // authentication is still standing.
                auth_on_connect();
                fd_set(&g_client, c);
                set_nonblocking(c, "client");
                gen_bump();
                g_retry_errno_logged_recv = false;
                g_retry_errno_logged_send = false;
                serve(c);
                drain_replies();   // this session's replies must never reach the next client
                drain_inbox();     // ...and neither should anything it never got answered
                sceNetSocketClose(c);
                fd_set(&g_client, -1);
                platform::klogf("net: client gone");
            }
            sceKernelUsleep(5000);
        }
        platform::klogf("net: thread down");
        return nullptr;
    }

}  // namespace

bool server_start() {
    if (g_thread) return true;

    OrbisNetId listen_fd = sceNetSocket("rdr2lua", ORBIS_NET_AF_INET, ORBIS_NET_SOCK_STREAM, 0);
    if (listen_fd < 0) {
        platform::klogf("net: socket() failed %d", listen_fd);
        return false;
    }

    int on = 1;
    int r = sceNetSetsockopt(listen_fd, NET_SOL_SOCKET, NET_SO_REUSEADDR, &on, sizeof(on));
    if (r < 0)
        platform::klogf("net: setsockopt(SO_REUSEADDR) failed %d", r);

    sockaddr_in_be a;
    memset(&a, 0, sizeof(a));
    a.len    = sizeof(a);
    a.family = ORBIS_NET_AF_INET;
    a.port   = sceNetHtons(port());
    a.addr   = 0;                            // INADDR_ANY

    r = sceNetBind(listen_fd, (const OrbisNetSockaddr*)&a, sizeof(a));
    if (r < 0) {
        platform::klogf("net: bind() failed %d", r);
        sceNetSocketClose(listen_fd);
        return false;
    }

    r = sceNetListen(listen_fd, 1);
    if (r < 0) {
        platform::klogf("net: listen() failed %d", r);
        sceNetSocketClose(listen_fd);
        return false;
    }

    set_nonblocking(listen_fd, "listen");
    fd_set(&g_listen, listen_fd);

    OrbisPthreadAttr attr;
    scePthreadAttrInit(&attr);
    scePthreadAttrSetstacksize(&attr, STACK_SIZE);
    g_stop = 0;
    r = scePthreadCreate(&g_thread, &attr, thread_main, nullptr, "rdr2lua_net");
    if (r != 0) {
        platform::klogf("net: pthread_create failed %d", r);
        g_thread = 0;
        sceNetSocketClose(listen_fd);
        fd_set(&g_listen, -1);
        return false;
    }

    platform::klogf("net: listening on %d", port());
    return true;
}

void server_stop() {
    if (!g_thread) return;
    g_stop = 1;

    OrbisNetId client_fd = fd_get(&g_client);
    OrbisNetId listen_fd = fd_get(&g_listen);
    if (client_fd >= 0) sceNetSocketAbort(client_fd, 0);
    if (listen_fd >= 0) sceNetSocketAbort(listen_fd, 0);

    scePthreadJoin(g_thread, nullptr);
    g_thread = 0;

    // Re-read after the join: the net thread may have closed g_client itself
    // (the normal end-of-connection path) while the abort above was in
    // flight, so the pre-join locals above must not be reused here.
    client_fd = fd_get(&g_client);
    listen_fd = fd_get(&g_listen);
    if (client_fd >= 0) { sceNetSocketClose(client_fd); fd_set(&g_client, -1); }
    if (listen_fd >= 0) { sceNetSocketClose(listen_fd); fd_set(&g_listen, -1); }

    // The net thread no longer exists after the join above, so nothing but a
    // still-running game-thread reply() call can touch the ring (see
    // drain_replies()'s own comment for that residual case). Drain it so a
    // stopped server does not leak queued payloads for the rest of the
    // process's life.
    drain_replies();
    drain_inbox();
}

// Declared in net_server_reply.h, called by the game thread through that
// header alone (callers outside this translation unit never see the
// anonymous namespace above). Defined here, after it, so the bodies can still
// reach g_replies/g_reply_lock/push_reply/etc.
void reply(uint8_t op, uint16_t seq, const void* payload, uint32_t len) {
    // Stamped here, on the game thread, at the moment this reply is queued
    // -- not later in flush() on the net thread, which would defeat the
    // whole point (see g_generation's comment above).
    uint32_t generation = gen_get();

    if (!payload) len = 0;   // a null payload is length zero, not garbage to copy

    uint8_t* data = nullptr;
    if (len) {
        data = (uint8_t*)malloc(len);
        if (!data) {
            // Never leave the client waiting on a seq that will never come:
            // report the allocation failure instead of dropping the reply.
            static const char oom_msg[] = "reply alloc failed";
            uint32_t n = (uint32_t)(sizeof(oom_msg) - 1);
            uint8_t* copy = (uint8_t*)malloc(n);
            if (copy) memcpy(copy, oom_msg, n);
            else      n = 0;             // even the fallback alloc failed; send an empty OP_ERR
            push_reply(OP_ERR, seq, copy, n, generation);
            return;
        }
        memcpy(data, payload, len);
    }
    push_reply(op, seq, data, len, generation);
}

void reply_ok(uint16_t seq, const char* text) {
    reply(OP_OK, seq, text, text ? (uint32_t)strlen(text) : 0);
}

void reply_err(uint16_t seq, const char* text) {
    reply(OP_ERR, seq, text, text ? (uint32_t)strlen(text) : 0);
}

}  // namespace net
