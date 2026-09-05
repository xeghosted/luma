// Host test for the wire codec, the mailbox and the path sandbox. None of these
// touch ORBIS, which is the point: the parts of the control channel that can be
// wrong in a subtle way are the parts that run on a PC.
#include "net/auth.h"
#include "net/dispatch.h"
#include "net/frame.h"
#include "net/mailbox.h"
#include "net/path_guard.h"
#include "script/lua_runtime.h"
#include "script/resource.h"
#include "lua.hpp"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace {

int g_failures = 0;

void check(bool ok, const char* what) {
    printf("%-58s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok) g_failures++;
}

void hex(const uint8_t* b, size_t n, char* out) {
    static const char* d = "0123456789abcdef";
    for (size_t i = 0; i < n; ++i) { out[i*2] = d[b[i] >> 4]; out[i*2+1] = d[b[i] & 15]; }
    out[n*2] = 0;
}

// --- a deliberately dumb scanner for tests/protocol_vectors.json ---------
//
// No JSON library: it is twelve vectors and two rejects, and a dependency here
// would have to be vendored into a freestanding build. Every helper below is
// bounded to a [begin, end) range so a field missing from one object is not
// accidentally picked up from its neighbour.

const char* find_in(const char* begin, const char* end, const char* needle) {
    size_t nlen = strlen(needle);
    if (begin >= end || (size_t)(end - begin) < nlen) return nullptr;
    for (const char* p = begin; p + nlen <= end; ++p) {
        if (strncmp(p, needle, nlen) == 0) return p;
    }
    return nullptr;
}

bool scan_int(const char* begin, const char* end, const char* key, long* out) {
    char pat[32];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char* p = find_in(begin, end, pat);
    if (!p) return false;
    p = strchr(p, ':');
    if (!p || p >= end) return false;
    *out = strtol(p + 1, nullptr, 10);
    return true;
}

bool scan_str(const char* begin, const char* end, const char* key, char* out, size_t outsz) {
    char pat[32];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char* p = find_in(begin, end, pat);
    if (!p) return false;
    p = strchr(p, ':');
    if (!p || p >= end) return false;
    p = strchr(p, '"');            // opening quote of the value
    if (!p || p >= end) return false;
    const char* start = p + 1;
    const char* vend = strchr(start, '"');
    if (!vend || vend >= end) return false;
    size_t len = (size_t)(vend - start);
    if (len >= outsz) return false;
    memcpy(out, start, len);
    out[len] = 0;
    return true;
}

// Every opcode by name, paired with the enum constant it must equal.
//
// The fixture vectors alone never bound the two languages' opcode tables
// together, though this file's header said they did: both suites read a
// vector's op NUMBER out of the JSON, so renumbering OP_CMD in src/net/frame.h
// left all three suites green while the C++ and TypeScript enums silently
// disagreed. This table plus the fixture's "opcodes" object is what closes
// that: each side asserts its own enum against the shared file, so a rename or
// a renumber on one side turns that side red immediately.
struct named_op { const char* name; int value; };

const named_op OPCODES[] = {
    { "HELLO",   net::OP_HELLO   }, { "PING",    net::OP_PING    },
    { "PONG",    net::OP_PONG    }, { "PUT",     net::OP_PUT     },
    { "GET",     net::OP_GET     }, { "LS",      net::OP_LS      },
    { "RM",      net::OP_RM      }, { "EXEC",    net::OP_EXEC    },
    { "RELOAD",  net::OP_RELOAD  }, { "LOG",     net::OP_LOG     },
    { "EVENT",   net::OP_EVENT   }, { "OK",      net::OP_OK      },
    { "ERR",     net::OP_ERR     }, { "BUSY",    net::OP_BUSY    },
    { "START",   net::OP_START   }, { "STOP",    net::OP_STOP    },
    { "RESTART", net::OP_RESTART }, { "RESLIST", net::OP_RESLIST },
    { "RESINFO", net::OP_RESINFO }, { "CMD",     net::OP_CMD     },
};
const int NOPCODES = (int)(sizeof(OPCODES) / sizeof(OPCODES[0]));

void test_frame() {
    uint8_t buf[net::HEADER_SIZE];
    char    got[64];

    net::header h = { net::OP_HELLO, 0, 1, 0 };
    net::encode_header(buf, h);
    hex(buf, net::HEADER_SIZE, got);
    check(strcmp(got, "474c55410100010000000000") == 0, "HELLO seq=1 len=0 encodes to the spec bytes");

    net::header e = { net::OP_EXEC, 0, 258, 5 };
    net::encode_header(buf, e);
    hex(buf, net::HEADER_SIZE, got);
    check(strcmp(got, "474c55410800020105000000") == 0, "EXEC seq=258 len=5 encodes little-endian");

    net::header back;
    check(net::decode_header(buf, &back), "a well-formed header decodes");
    check(back.op == net::OP_EXEC && back.seq == 258 && back.len == 5, "  round-trips op, seq and len");

    uint8_t bad[net::HEADER_SIZE];
    memcpy(bad, buf, sizeof(bad));
    bad[0] = 'X';
    check(!net::decode_header(bad, &back), "a bad magic is rejected");

    uint8_t big[net::HEADER_SIZE];
    net::header h2 = { net::OP_PUT, 0, 1, net::MAX_PAYLOAD + 1 };
    net::encode_header(big, h2);
    check(!net::decode_header(big, &back), "a length over the 8 MB cap is rejected");

    // The cap is inclusive: MAX_PAYLOAD itself must still decode. An off-by-one
    // that turned the check into `>=` would sail past the case above unnoticed.
    uint8_t atcap[net::HEADER_SIZE];
    net::header h3 = { net::OP_PUT, 0, 1, net::MAX_PAYLOAD };
    net::encode_header(atcap, h3);
    net::header capback;
    check(net::decode_header(atcap, &capback) && capback.len == net::MAX_PAYLOAD,
          "a length exactly at the 8 MB cap decodes and round-trips");

    // Before any of the fixture work: does OPCODES above cover net::op at
    // all? Everything below compares that table to the shared file, in both
    // directions -- but the table itself is hand-written, so an opcode added
    // to net::op and to the TypeScript Op, and to neither name table nor the
    // fixture, is invisible to all three suites. They would agree perfectly
    // about the twenty they knew about. The enum's trailing OP__COUNT is what
    // makes this checkable in a language with no enum reflection.
    check(NOPCODES == (int)net::OP__COUNT - 1,
          "the opcode name table covers every member of net::op");
    bool bijective = true;
    for (int v = 1; v < (int)net::OP__COUNT; ++v) {
        int seen = 0;
        for (int i = 0; i < NOPCODES; ++i) if (OPCODES[i].value == v) ++seen;
        if (seen != 1) bijective = false;
    }
    check(bijective, "  naming each of its values exactly once, with no gaps");

    // Parse tests/protocol_vectors.json and drive every fixture through the
    // real codec, so this test catches a codec regression instead of only
    // detecting drift between two hardcoded constants in this file.
    FILE* f = fopen("tests/protocol_vectors.json", "rb");
    check(f != nullptr, "protocol_vectors.json is readable from the repo root");
    if (f) {
        char js[8192];
        size_t n = fread(js, 1, sizeof(js) - 1, f);
        js[n] = 0;
        fclose(f);
        const char* js_end = js + n;

        // A fixture file that outgrew this buffer would be scanned in part,
        // and whatever fell off the end would simply never be asserted. The
        // "all twelve were found" counts below catch a vector going missing
        // from the file; nothing would catch the file being read short.
        check(n < sizeof(js) - 1, "the whole fixture file fits the scanner's buffer");

        // The resource-name limit, bound the same way the opcode table is:
        // tests/lua/resource_test.lua asserts valid_name's own bound against
        // this very number, so the wire's buffer and the runtime's naming
        // rule cannot drift apart again. They did once -- only the wire had a
        // limit, and RESLIST advertised a 183-character directory as
        // "stopped" while all four ops that take a name refused it.
        long name_max = -1;
        check(scan_int(js, js_end, "resource_name_max", &name_max) &&
              name_max == net::RESOURCE_NAME_MAX,
              "net::RESOURCE_NAME_MAX is the fixture's resource_name_max");

        // The opcode TABLE, before any encoding: each name in the fixture's
        // "opcodes" object must equal the net::op constant of the same name,
        // and the fixture must list no opcode this file does not name. Both
        // directions matter -- one alone lets an opcode be added on one side
        // only. The VS Code client asserts the identical object (src/protocol.test.ts
        // in https://github.com/xeghosted/rage-script-manager)
        // against its own Op enum, which is what makes the two enums unable
        // to drift from each other.
        const char* ops     = find_in(js, js_end, "\"opcodes\"");
        const char* ops_beg = ops ? find_in(ops, js_end, "{") : nullptr;
        const char* ops_end = ops_beg ? find_in(ops_beg, js_end, "}") : nullptr;
        check(ops_beg != nullptr && ops_end != nullptr, "the fixture file carries an \"opcodes\" table");
        if (ops_beg && ops_end) {
            for (int i = 0; i < NOPCODES; ++i) {
                long v = -1;
                char msg[96];
                snprintf(msg, sizeof(msg), "  net::OP_%s is the fixture's \"%s\"",
                         OPCODES[i].name, OPCODES[i].name);
                check(scan_int(ops_beg, ops_end, OPCODES[i].name, &v) && v == OPCODES[i].value, msg);
            }
            // Values in that object are bare integers, so every quote inside
            // it belongs to a key: two per opcode. An opcode added to the
            // fixture (and to the TypeScript enum) but not to net::op fails
            // right here rather than passing for want of being looked for.
            int quotes = 0;
            for (const char* p = ops_beg; p < ops_end; ++p) if (*p == '"') ++quotes;
            check(quotes == NOPCODES * 2,
                  "  and the fixture lists exactly the opcodes net::op has, no more");
        }

        // Every fixture vector must encode, through the real net::encode_header,
        // to exactly its own "hex" field.
        const char* vectors     = strstr(js, "\"vectors\"");
        const char* vectors_end = vectors ? strchr(vectors, ']') : nullptr;
        int nvectors = 0;
        if (vectors && vectors_end) {
            const char* p = vectors;
            for (;;) {
                const char* obj = find_in(p, vectors_end, "{");
                if (!obj) break;
                const char* obj_end = find_in(obj, vectors_end, "}");
                if (!obj_end) break;

                long op = 0, flags = 0, seq = 0, len = 0;
                char name[32] = "?";
                char want[64];
                scan_str(obj, obj_end, "name", name, sizeof(name));
                if (scan_int(obj, obj_end, "op", &op) &&
                    scan_int(obj, obj_end, "flags", &flags) &&
                    scan_int(obj, obj_end, "seq", &seq) &&
                    scan_int(obj, obj_end, "len", &len) &&
                    scan_str(obj, obj_end, "hex", want, sizeof(want))) {
                    net::header vh = { (uint8_t)op, (uint8_t)flags, (uint16_t)seq, (uint32_t)len };
                    uint8_t vbuf[net::HEADER_SIZE];
                    net::encode_header(vbuf, vh);
                    char vgot[64];
                    hex(vbuf, net::HEADER_SIZE, vgot);
                    char msg[96];
                    snprintf(msg, sizeof(msg), "fixture vector %s round-trips through net::encode_header", name);
                    check(strcmp(vgot, want) == 0, msg);
                    ++nvectors;
                }
                p = obj_end + 1;
            }
        }
        // A scanner that silently finds nothing must fail loudly, not pass.
        check(nvectors == 12, "all twelve fixture vectors were found and parsed");

        // Every reject fixture's raw bytes must be rejected by the real decoder.
        const char* rejects     = strstr(js, "\"rejects\"");
        const char* rejects_end = rejects ? strchr(rejects, ']') : nullptr;
        int nrejects = 0;
        if (rejects && rejects_end) {
            const char* p = rejects;
            for (;;) {
                const char* obj = find_in(p, rejects_end, "{");
                if (!obj) break;
                const char* obj_end = find_in(obj, rejects_end, "}");
                if (!obj_end) break;

                char name[32] = "?";
                char rhex[64];
                scan_str(obj, obj_end, "name", name, sizeof(name));
                if (scan_str(obj, obj_end, "hex", rhex, sizeof(rhex)) &&
                    strlen(rhex) == net::HEADER_SIZE * 2) {
                    uint8_t rbuf[net::HEADER_SIZE];
                    for (size_t i = 0; i < net::HEADER_SIZE; ++i) {
                        char byte_str[3] = { rhex[i * 2], rhex[i * 2 + 1], 0 };
                        rbuf[i] = (uint8_t)strtol(byte_str, nullptr, 16);
                    }
                    net::header rh;
                    char msg[96];
                    snprintf(msg, sizeof(msg), "fixture reject %s is rejected by net::decode_header", name);
                    check(!net::decode_header(rbuf, &rh), msg);
                    ++nrejects;
                }
                p = obj_end + 1;
            }
        }
        check(nrejects == 2, "both fixture reject vectors were found and parsed");
    }
}

void test_mailbox() {
    // Inbox: FIFO, bounded, ownership of data passes to the popper.
    net::message m;
    m.op = net::OP_EXEC; m.seq = 7; m.len = 4;
    m.data = (uint8_t*)malloc(4);
    memcpy(m.data, "abcd", 4);
    check(net::inbox_push(m), "a message pushes into an empty inbox");

    net::message got;
    check(net::inbox_pop(&got), "and pops back out");
    check(got.op == net::OP_EXEC && got.seq == 7 && got.len == 4, "  with op, seq and len intact");
    check(memcmp(got.data, "abcd", 4) == 0, "  and the payload pointer still valid");
    free(got.data);
    check(!net::inbox_pop(&got), "an empty inbox reports empty");

    net::message filler;
    filler.op = net::OP_PING; filler.seq = 0; filler.len = 0; filler.data = nullptr;
    int pushed = 0;
    for (int i = 0; i < net::INBOX_CAP + 4; ++i)
        if (net::inbox_push(filler)) pushed++;
    check(pushed == net::INBOX_CAP, "the inbox refuses past its capacity rather than growing");
    while (net::inbox_pop(&got)) { }

    // Outbox: fixed ring, drop-oldest, and the drop is reported not hidden.
    net::outbox_push_line("first");
    net::outbox_push_line("second");
    char line[net::OUTBOX_LINE];
    check(net::outbox_pop_line(line) && strcmp(line, "first") == 0,  "outbox pops in order");
    check(net::outbox_pop_line(line) && strcmp(line, "second") == 0, "  second line follows");
    check(!net::outbox_pop_line(line), "an empty outbox reports empty");

    for (int i = 0; i < net::OUTBOX_SLOTS + 5; ++i) {
        char s[32];
        snprintf(s, sizeof(s), "line%d", i);
        net::outbox_push_line(s);
    }
    check(net::outbox_pop_line(line) && strncmp(line, "[rdr2lua] 5 log line", 19) == 0,
          "overflow surfaces a drop marker before the surviving lines");
    check(net::outbox_pop_line(line) && strcmp(line, "line5") == 0,
          "  and the oldest survivor is the 6th line pushed");

    // A line longer than a slot is truncated, never overflowed -- and SAYS
    // so. The ring already refuses to lose a whole line silently (the drop
    // marker above); losing the tail of one silently was the same defect one
    // level down, and it was the one truncation in the system a receiver
    // could not detect. A console session found print() cut at 255 characters
    // with nothing to mark it.
    char big[net::OUTBOX_LINE * 2];
    memset(big, 'x', sizeof(big) - 1);
    big[sizeof(big) - 1] = 0;
    while (net::outbox_pop_line(line)) { }
    net::outbox_push_line(big);
    const size_t MARK_LEN = strlen(net::OUTBOX_TRUNC_MARK);
    check(net::outbox_pop_line(line) && strlen(line) == net::OUTBOX_LINE - 1,
          "an oversized line still fills the slot exactly, never past it");
    check(strlen(line) > MARK_LEN &&
          strcmp(line + strlen(line) - MARK_LEN, net::OUTBOX_TRUNC_MARK) == 0,
          "  and ends with the truncation marker, so the cut is detectable");
    check(strncmp(line, "xxxxxxxx", 8) == 0,
          "  with the surviving prefix still the line's own text");

    // The marker must not turn every line into a truncated one.
    while (net::outbox_pop_line(line)) { }
    net::outbox_push_line("a short line");
    check(net::outbox_pop_line(line) && strcmp(line, "a short line") == 0,
          "a line that fits is stored whole, with no marker");

    // The exact boundary: OUTBOX_LINE-1 bytes is the longest line that fits,
    // and it must come back unmarked. An off-by-one here would mark a line
    // that was never cut, which is the same lie in the other direction.
    char exact[net::OUTBOX_LINE];
    memset(exact, 'y', sizeof(exact) - 1);
    exact[sizeof(exact) - 1] = 0;
    while (net::outbox_pop_line(line)) { }
    net::outbox_push_line(exact);
    check(net::outbox_pop_line(line) && strcmp(line, exact) == 0,
          "a line of exactly the slot's capacity is stored whole and unmarked");

    // One byte past it is the shortest line that IS cut.
    char over[net::OUTBOX_LINE + 1];
    memset(over, 'z', sizeof(over) - 1);
    over[sizeof(over) - 1] = 0;
    while (net::outbox_pop_line(line)) { }
    net::outbox_push_line(over);
    check(net::outbox_pop_line(line) && strlen(line) > MARK_LEN &&
          strcmp(line + strlen(line) - MARK_LEN, net::OUTBOX_TRUNC_MARK) == 0,
          "a line one byte over the slot is marked, not silently shortened");
}

void test_token_classify() {
    // The caller stores max_len bytes plus a NUL. Every bug this logic has
    // had lived at one of these boundaries, including a one-byte overflow
    // that no test would have caught because there was no test.
    const size_t MAX = 255;
    size_t len = 0;

    check(net::token_classify("secret", 6, MAX, &len) == net::TOKEN_OK &&  len == 6,
          "a plain secret is accepted whole");
    check(net::token_classify("secret\n", 7, MAX, &len) == net::TOKEN_OK && len == 6,
          "  and an editor's trailing newline is not part of it");
    check(net::token_classify("secret\r\n", 8, MAX, &len) == net::TOKEN_OK && len == 6,
          "  nor a CRLF");
    check(net::token_classify("with space", 10, MAX, &len) == net::TOKEN_OK && len == 10,
          "  while a space INSIDE it is kept");

    check(net::token_classify("", 0, MAX, &len) == net::TOKEN_NO_SECRET,
          "an empty file has no secret");
    check(net::token_classify("\n\n", 2, MAX, &len) == net::TOKEN_NO_SECRET,
          "  and neither does one holding only newlines");

    char big[300];
    memset(big, 'x', sizeof(big));
    check(net::token_classify(big, MAX, MAX, &len) == net::TOKEN_OK && len == MAX,
          "a secret of exactly the maximum length fits");
    // The case that produced the overflow: at the limit, plus the newline the
    // editor added. Trimming BEFORE the length is judged is what saves it.
    char atmax[MAX + 1];
    memset(atmax, 'x', MAX);
    atmax[MAX] = '\n';
    check(net::token_classify(atmax, MAX + 1, MAX, &len) == net::TOKEN_OK && len == MAX,
          "  and so does that same secret with a newline after it");
    check(net::token_classify(big, MAX + 1, MAX, &len) == net::TOKEN_TOO_LONG,
          "one byte over the maximum is refused, not truncated");
    check(net::token_classify(big, sizeof(big), MAX, &len) == net::TOKEN_TOO_LONG,
          "  and so is a file far over it");

    // Only 'no usable token file' opens the channel. UNKNOWN -- a load that
    // raced with a connection being accepted -- must read as SHUT: reading it
    // as open would let every op through on exactly the message that lost the
    // race, and a client could retry that window until it hit.
    check(!net::token_gates(net::TOKEN_NONE), "no token file leaves the channel open");
    check(net::token_gates(net::TOKEN_PRESENT), "a loaded secret closes it");
    check(net::token_gates(net::TOKEN_UNUSABLE), "  and so does a file we refuse to use");
    check(net::token_gates(net::TOKEN_UNKNOWN), "  and so does not knowing yet");

    // A truncated compare would authenticate a prefix. It does not.
    check(net::token_matches("secret", 6, "secret", 6), "an exact token matches");
    check(!net::token_matches("secret", 6, "secre", 5), "  a prefix of it does not");
    check(!net::token_matches("secret", 6, "secretx", 7), "  nor an extension of it");
    check(!net::token_matches("secret", 6, "secreT", 6), "  nor one wrong byte");
    check(!net::token_matches("secret", 6, "", 0), "  and an empty payload never matches");
}

void test_auth_policy() {
    // No token file on the console: nothing changes. This is the documented
    // default (spec 4.4) and the whole LAN can already reach FTP, so a channel
    // that started refusing work here would break every existing setup.
    check(net::auth_allows(net::OP_EXEC, false, false),
          "with no token file, EXEC is allowed unauthenticated");
    check(net::auth_allows(net::OP_PUT, false, false), "  and so is PUT");

    // With a token file, a connection that has not passed HELLO may try to,
    // and may check the channel is alive, and nothing else.
    check(net::auth_allows(net::OP_HELLO, true, false),
          "with a token file, HELLO is allowed unauthenticated");
    check(net::auth_allows(net::OP_PING, true, false),
          "  and PING, so a client can still tell the channel is alive");
    check(!net::auth_allows(net::OP_EXEC, true, false), "  but EXEC is refused");
    check(!net::auth_allows(net::OP_PUT, true, false), "  and PUT is refused");
    // Read-only is not harmless: RESLIST and GET tell a stranger what is on
    // the console, and GET hands them the contents.
    check(!net::auth_allows(net::OP_RESLIST, true, false), "  and so is a read-only RESLIST");
    check(!net::auth_allows(net::OP_GET, true, false), "  and GET, which would hand over a file");

    check(net::auth_allows(net::OP_EXEC, true, true), "an authenticated connection may EXEC");
    check(net::auth_allows(net::OP_PUT, true, true), "  and PUT");

    // The gate is a whitelist, so an op this table has never heard of is
    // refused rather than allowed through by omission.
    check(!net::auth_allows(net::OP__COUNT, true, false),
          "an unknown op is refused rather than defaulting open");
}

void test_path_guard() {
    char out[512];

    check(net::safe_path("scripts/a.lua", out, sizeof(out)), "a plain relative path is accepted");
    check(strcmp(out, "/data/rdr2lua/scripts/a.lua") == 0, "  and lands under /data/rdr2lua");

    check(net::safe_path("a.lua", out, sizeof(out)), "a bare filename is accepted");
    check(strcmp(out, "/data/rdr2lua/a.lua") == 0, "  directly under the root");

    check(!net::safe_path("../etc/passwd", out, sizeof(out)), "a leading .. is rejected");
    check(!net::safe_path("scripts/../../x", out, sizeof(out)), "an embedded .. segment is rejected");
    check(!net::safe_path("/etc/passwd", out, sizeof(out)), "an absolute path is rejected");
    check(!net::safe_path("", out, sizeof(out)), "an empty path is rejected");
    check(!net::safe_path("a\\..\\b", out, sizeof(out)), "a backslash is rejected");

    // A name that merely starts with dots is a legitimate filename, not an escape.
    check(net::safe_path("..hidden.lua", out, sizeof(out)), "'..hidden.lua' is a filename, not an escape");

    char tiny[8];
    check(!net::safe_path("scripts/a.lua", tiny, sizeof(tiny)), "a path that would not fit is rejected");

    // The exact boundary of the fit check: "scripts/a.lua" (13) onto
    // net::ROOT (14, "/data/rdr2lua/") is 27 bytes plus a NUL. A buffer sized
    // to hold exactly that must be accepted; one byte less must not. This
    // pins the boundary tiny[8] above misses by 20 bytes — an off-by-one in
    // `need + 1 > out_len` would slip past that case unnoticed.
    //
    // These sizes are tied to strlen(net::ROOT), so they move when the root
    // does: renaming /data/gtalua/ to /data/rdr2lua/ added one byte and this
    // test caught it. If you change ROOT again, change 28/27 with it.
    char exact[28];
    check(net::safe_path("scripts/a.lua", exact, sizeof(exact)), "a buffer sized to fit exactly is accepted");
    char one_short[27];
    check(!net::safe_path("scripts/a.lua", one_short, sizeof(one_short)), "one byte less than exact is rejected");

    check(!net::safe_path("..", out, sizeof(out)), "a bare '..' is rejected");
    check(!net::safe_path("a/..", out, sizeof(out)), "a trailing '..' segment is rejected");

    // "." and "./x" are no-ops, not escapes: they never leave the sandbox.
    check(net::safe_path(".", out, sizeof(out)), "a lone '.' is accepted");
    check(net::safe_path("./x", out, sizeof(out)), "'./x' is accepted");

    // A trailing or doubled slash does not let anything out of the sandbox
    // either; the OS collapses both when the path is actually opened.
    check(net::safe_path("scripts/", out, sizeof(out)), "a trailing slash is accepted");
    check(net::safe_path("scripts//a.lua", out, sizeof(out)), "a doubled slash is accepted");

    check(!net::safe_path(nullptr, out, sizeof(out)), "a null rel pointer is rejected");
}

// Every out-field of an exec_result is seeded with a deliberately WRONG value
// before each call. A zeroed struct would let a function that forgot to write
// a field pass by inheriting the right answer from the initialiser.
script::exec_result seeded(char* buf, size_t cap) {
    script::exec_result r = { buf, cap, -1, 999, true, true };
    return r;
}

// The last line handed to the log sink, for the print/log half of the
// stringification path -- push_joined serves both, and only the EXEC half is
// reachable through runtime_exec_capture's out-parameter.
char   g_log_line[262144];
size_t g_log_len   = 0;
int    g_log_count = 0;

void log_capture(const char* msg) {
    ++g_log_count;
    size_t n = msg ? strlen(msg) : 0;
    g_log_len = n;                                  // the TRUE length, uncapped
    if (n >= sizeof(g_log_line)) n = sizeof(g_log_line) - 1;
    if (msg) memcpy(g_log_line, msg, n);
    g_log_line[n] = 0;
}

// The same, for the screen channel. Separate counters are the whole point of
// these tests: the two used to be one sink.
char g_notify_line[4096];
int  g_notify_count = 0;

void notify_capture(const char* msg) {
    ++g_notify_count;
    size_t n = msg ? strlen(msg) : 0;
    if (n >= sizeof(g_notify_line)) n = sizeof(g_notify_line) - 1;
    if (msg) memcpy(g_notify_line, msg, n);
    g_notify_line[n] = 0;
}

bool all_bytes(const char* p, size_t n, char c) {
    for (size_t i = 0; i < n; ++i) if (p[i] != c) return false;
    return true;
}

// EXEC's reply has to carry what the chunk RETURNED (spec 4.2: "reply carries
// the result or the error"). It used to ask lua_pcall for zero results and
// answer the literal string "ok" to everything, so the editor's console --
// which rewrites "=expr" into "return expr" for exactly this reason --
// could never print a value. Called from test_exec_capture, between
// runtime_init and runtime_shutdown.
void test_exec_results() {
    char err[256];
    char val[256];

    script::exec_result r = seeded(val, sizeof(val));
    check(script::runtime_exec_capture("t", "return 1 + 1", 12, err, sizeof(err), nullptr, &r),
          "a chunk with a return value runs");
    check(r.nresults == 1, "  reporting one result");
    check(strcmp(val, "2") == 0, "  and the payload is the value itself, not \"ok\"");
    check(r.len == 1 && !r.truncated && !r.value_error, "  one byte, whole, rendered");

    // Multiple results, and every non-string kind the REPL will actually
    // meet: integer, string, boolean, nil, float. Tab-separated, because
    // that is what log/print already do -- the same push_joined, not a
    // second stringification that could drift from it.
    const char* multi = "return 1, 'two', true, nil, 3.5";
    r = seeded(val, sizeof(val));
    check(script::runtime_exec_capture("t", multi, strlen(multi), err, sizeof(err), nullptr, &r),
          "a chunk returning several values runs");
    check(r.nresults == 5, "  reporting five results");
    check(strcmp(val, "1\ttwo\ttrue\tnil\t3.5") == 0,
          "  rendered tab-separated, the same way log/print render arguments");

    r = seeded(val, sizeof(val));
    check(script::runtime_exec_capture("t", "x = 1", 5, err, sizeof(err), nullptr, &r),
          "a chunk that returns nothing still succeeds");
    check(r.nresults == 0, "  reporting no results");
    check(r.len == 0 && val[0] == 0, "  and writing nothing, so on_exec can answer \"ok\"");

    // "no result" and "the result nil" are different answers and must not
    // collapse into each other: nresults is what separates them.
    r = seeded(val, sizeof(val));
    check(script::runtime_exec_capture("t", "return nil", 10, err, sizeof(err), nullptr, &r),
          "'return nil' runs");
    check(r.nresults == 1 && strcmp(val, "nil") == 0,
          "  and is one result rendering as \"nil\", not silence");

    r = seeded(val, sizeof(val));
    check(script::runtime_exec_capture("t", "return {}", 9, err, sizeof(err), nullptr, &r),
          "a chunk returning a table runs");
    check(r.nresults == 1 && strncmp(val, "table: ", 7) == 0,
          "  and renders like tostring does, address and all");

    // A caller from before this existed passes neither kind nor result; a
    // returning chunk must still succeed for it, with the values dropped.
    check(script::runtime_exec_capture("t", "return 1", 8, err, sizeof(err)),
          "a caller that asks for no result still succeeds on a returning chunk");

    script::exec_result nobuf = { nullptr, 0, -1, 999, true, true };
    check(script::runtime_exec_capture("t", "return 1", 8, err, sizeof(err), nullptr, &nobuf) &&
          nobuf.nresults == 1 && nobuf.len == 0,
          "an exec_result with no buffer still reports the result count");

    // Bounded output. A chunk returning 100 KB must not blow the buffer, and
    // the cut must be visible in the BYTES -- a client holding nothing but
    // the reply payload has no struct to read `truncated` out of. Guards
    // flank the buffer so an overrun in either direction is caught rather
    // than merely inspected for.
    {
        const size_t GUARD = 8, BUFN = 64;
        char region[GUARD + BUFN + GUARD];
        memset(region, 0x5a, sizeof(region));
        char* buf = region + GUARD;
        memset(buf, 0x7e, BUFN);

        const char* big = "return ('x'):rep(100000)";
        script::exec_result t = seeded(buf, BUFN);
        check(script::runtime_exec_capture("t", big, strlen(big), err, sizeof(err), nullptr, &t),
              "a chunk returning 100 KB still succeeds");
        check(t.truncated, "  and reports that the result was cut");
        check(t.len < BUFN && strlen(buf) == t.len,
              "  the text fits the buffer and is NUL-terminated at the reported length");
        check(strstr(buf, "truncated") != nullptr,
              "  the payload itself says so, so a bytes-only client can tell");

        bool guards_intact = true;
        for (size_t i = 0; i < GUARD; ++i) {
            if ((unsigned char)region[i] != 0x5a) guards_intact = false;
            if ((unsigned char)region[GUARD + BUFN + i] != 0x5a) guards_intact = false;
        }
        check(guards_intact, "  and nothing was written past the buffer in either direction");
    }

    // The exact boundary of "fits". A result of exactly 10 bytes into a
    // buffer sized 10 + the NUL must NOT be reported as truncated; one byte
    // less must be. An off-by-one in the `jlen <= usable` test would mark
    // every full buffer truncated (or, worse, mark a cut one whole) and the
    // 100 KB case above would not notice either way.
    {
        const char* ten = "return ('y'):rep(10)";
        char exact[11];
        script::exec_result e = seeded(exact, sizeof(exact));
        check(script::runtime_exec_capture("t", ten, strlen(ten), err, sizeof(err), nullptr, &e) &&
              !e.truncated && e.len == 10 && strcmp(exact, "yyyyyyyyyy") == 0,
              "a result that exactly fills the buffer is not reported as truncated");
        char one_short[10];
        script::exec_result s = seeded(one_short, sizeof(one_short));
        check(script::runtime_exec_capture("t", ten, strlen(ten), err, sizeof(err), nullptr, &s) &&
              s.truncated && strlen(one_short) == s.len && s.len < sizeof(one_short),
              "  one byte less and it is, still NUL-terminated inside the buffer");
    }

    // A __tostring metamethod is resource-written Lua and may raise. Rendered
    // unprotected that error would reach lua_atpanic and abort the process --
    // on console, the game. The chunk itself ran to completion, so this is
    // NOT an exec failure; it is reported through value_error instead, and
    // the reason still reaches the payload.
    {
        const char* bad = "return setmetatable({}, { __tostring = function() error('nope') end })";
        script::exec_result v = seeded(val, sizeof(val));
        check(script::runtime_exec_capture("t", bad, strlen(bad), err, sizeof(err), nullptr, &v),
              "a result whose __tostring raises does not fail the exec");
        check(v.value_error, "  but the caller is told the value could not be rendered");
        check(strstr(val, "nope") != nullptr, "  and the reason reaches the payload");
    }

    // Stack balance again, this time for the succeeding path: LUA_MULTRET
    // leaves an unknown number of values behind, and one leaked per EXEC
    // would accumulate silently over an editing session.
    {
        lua_State* L = script::runtime_state();
        int before = lua_gettop(L);
        for (int i = 0; i < 50; ++i) {
            script::exec_result b = seeded(val, sizeof(val));
            script::runtime_exec_capture("t", "return 1, 2, 3", 14, err, sizeof(err), nullptr, &b);
        }
        check(lua_gettop(L) == before,
              "50 result-returning execs leave the Lua stack depth unchanged");
    }
}

// Results that outgrow push_joined's luaL_Buffer.
//
// A luaL_Buffer starts life in a LUAL_BUFFERSIZE (1 KB) array inside the
// luaL_Buffer struct and moves to a heap "box" the first time it needs more
// than that. Every case in test_exec_results renders into a 64-byte buffer or
// less, so the buffer never grew, so the entire grow path was structurally
// invisible to this suite -- which is how a heap-use-after-free lived on the
// one code path every EXEC reply and every print() goes through.
//
// on_exec (src/net/dispatch.cpp) hands runtime_exec_capture a 4096-byte
// buffer, which makes the join limit 4095, which GUARANTEES the grow for any
// reply over ~1 KB. `=("x"):rep(2000)` typed into the editor console is the
// whole trigger, so BUFN here is on_exec's own 4096 rather than a size chosen
// to make the test comfortable.
void test_exec_results_past_the_buffer() {
    char err[256];

    const size_t GUARD = 8, BUFN = 4096;
    char region[GUARD + BUFN + GUARD];
    memset(region, 0x5a, sizeof(region));
    char* buf = region + GUARD;
    memset(buf, 0x7e, BUFN);

    check(BUFN > (size_t)LUAL_BUFFERSIZE + 1,
          "the reply buffer is bigger than a luaL_Buffer, so the join must grow");

    // One value, comfortably past LUAL_BUFFERSIZE and comfortably inside the
    // reply buffer: nothing here is truncated, so every byte of the answer is
    // checkable against what the chunk actually returned.
    {
        const char* two_k = "return ('x'):rep(2000)";
        script::exec_result g = seeded(buf, BUFN);
        check(script::runtime_exec_capture("t", two_k, strlen(two_k), err, sizeof(err), nullptr, &g),
              "a 2 KB result -- past the 1 KB join buffer -- renders");
        check(g.nresults == 1 && !g.truncated && !g.value_error && g.len == 2000,
              "  whole, at its full length, with nothing to report");
        check(strlen(buf) == 2000 && all_bytes(buf, 2000, 'x'),
              "  and every byte of it is the value, not the contents of freed memory");
    }

    // The grow happening mid-loop, with values still to come after it. This
    // is the harsher shape: the buffer moves to the heap while value 2 is
    // being added, and values 3 and 4 are appended afterwards.
    {
        const char* many = "return ('a'):rep(600), ('b'):rep(600), ('c'):rep(600), ('d'):rep(600)";
        script::exec_result g = seeded(buf, BUFN);
        check(script::runtime_exec_capture("t", many, strlen(many), err, sizeof(err), nullptr, &g),
              "four 600-byte values, crossing the join buffer part-way through");
        const size_t want = 4 * 600 + 3;                 // three tab separators
        bool shape = g.nresults == 4 && !g.truncated && g.len == want && strlen(buf) == want;
        check(shape, "  render to one 2403-byte string");
        check(shape && all_bytes(buf, 600, 'a') && buf[600] == '\t' &&
              all_bytes(buf + 601, 600, 'b') && buf[1201] == '\t' &&
              all_bytes(buf + 1202, 600, 'c') && buf[1802] == '\t' &&
              all_bytes(buf + 1803, 600, 'd'),
              "  with each value intact and tab-separated across the move");
    }

    // Truncation, but now on the far side of a grow: the 100 KB case in
    // test_exec_results caps the join at 63 bytes and never leaves the
    // stack-allocated array. Here the join runs to 4096 bytes first.
    {
        const size_t mark = strlen(script::EXEC_RESULT_TRUNC_MARK);
        const char*  big  = "return ('x'):rep(100000)";
        script::exec_result g = seeded(buf, BUFN);
        check(script::runtime_exec_capture("t", big, strlen(big), err, sizeof(err), nullptr, &g),
              "a 100 KB result into a 4 KB reply buffer succeeds");
        check(g.truncated && g.len == BUFN - 1 && strlen(buf) == g.len,
              "  cut to exactly the buffer, NUL-terminated");
        check(memcmp(buf + g.len - mark, script::EXEC_RESULT_TRUNC_MARK, mark) == 0,
              "  with the marker on the end");
        check(all_bytes(buf, g.len - mark, 'x'),
              "  and the kept prefix is all value, with no freed bytes in it");
    }

    // The other door out of push_joined. log/print pass limit = 0, so they
    // never clamp and a long line grows the buffer just the same -- this
    // shape predates the EXEC reply entirely, and `print(('p'):rep(2000))`
    // from any resource script hit it.
    {
        script::runtime_set_log_sink(log_capture);
        g_log_count = 0;
        g_log_len   = 0;
        g_log_line[0] = 0;
        const char* p = "print(('p'):rep(2000))";
        check(script::runtime_exec_capture("t", p, strlen(p), err, sizeof(err)),
              "print() of a 2 KB line runs");
        check(g_log_count == 1 && g_log_len == 2000,
              "  and reaches the sink at its full length");
        check(all_bytes(g_log_line, g_log_len < 2000 ? g_log_len : 2000, 'p'),
              "  every byte of it the value print was given");

        // Two arguments that only cross the boundary together, so the grow
        // lands between them rather than inside the first.
        g_log_count = 0;
        const char* p2 = "print(('q'):rep(700), ('r'):rep(700))";
        check(script::runtime_exec_capture("t", p2, strlen(p2), err, sizeof(err)),
              "print() of two arguments that together outgrow the join buffer runs");
        check(g_log_count == 1 && g_log_len == 1401 &&
              all_bytes(g_log_line, 700, 'q') && g_log_line[700] == '\t' &&
              all_bytes(g_log_line + 701, 700, 'r'),
              "  and both reach the sink whole, tab-separated");
        script::runtime_set_log_sink(nullptr);
    }

    // log/print must not reach the screen; notify() must reach both.
    //
    // These were one channel: log_sink raised a PS4 notification per logged
    // line, so a script logging on a timer -- both shipped hello examples do,
    // three lines every few seconds between them -- kept the notification
    // queue permanently full and buried the plugin's own load/ready/FAILED
    // messages behind a backlog. Output is the quiet default now.
    {
        script::runtime_set_log_sink(log_capture);
        script::runtime_set_notify_sink(notify_capture);
        g_log_count = 0; g_notify_count = 0; g_notify_line[0] = 0;

        const char* pl = "print('quiet')";
        check(script::runtime_exec_capture("t", pl, strlen(pl), err, sizeof(err)),
              "print() runs");
        check(g_log_count == 1, "  and reaches the log");
        check(g_notify_count == 0, "  but raises no on-screen notification");

        const char* nt = "notify('loud')";
        check(script::runtime_exec_capture("t", nt, strlen(nt), err, sizeof(err)),
              "notify() runs");
        check(g_notify_count == 1 && strcmp(g_notify_line, "loud") == 0,
              "  and reaches the screen");
        check(g_log_count == 2,
              "  and is logged too, so nothing shown on screen goes unrecorded");

        // The same stringification as print, not a second rule to remember.
        g_notify_count = 0;
        const char* nt2 = "notify('a', 1, true)";
        check(script::runtime_exec_capture("t", nt2, strlen(nt2), err, sizeof(err)) &&
              g_notify_count == 1 && strcmp(g_notify_line, "a	1	true") == 0,
              "notify() joins its arguments exactly as print does");

        // Errors still reach the screen. Quietening print() must not quietly
        // take script failures off it too: without the editor attached the
        // only other copy is a file you fetch over FTP. Bounded by
        // construction -- once per file per RELOAD, and a raising tick
        // callback is dropped from the list after its first error.
        g_notify_count = 0;
        const char* bad = "this is not lua";
        check(!script::runtime_load_buffer("bad.lua", bad, strlen(bad)),
              "a script that fails to compile reports failure");
        check(g_notify_count == 1 && strstr(g_notify_line, "[load error]") != nullptr,
              "  and says so on screen, not only in the log");

        // A runtime with no screen attached (every host suite, and the console
        // before the loader installs its sink) must not lose the line or crash.
        script::runtime_set_notify_sink(nullptr);
        g_log_count = 0;
        const char* nt3 = "notify('no screen here')";
        check(script::runtime_exec_capture("t", nt3, strlen(nt3), err, sizeof(err)) &&
              g_log_count == 1,
              "notify() with no sink installed still logs, and does not raise");

        script::runtime_set_log_sink(nullptr);
    }

    bool guards_intact = true;
    for (size_t i = 0; i < GUARD; ++i) {
        if ((unsigned char)region[i] != 0x5a) guards_intact = false;
        if ((unsigned char)region[GUARD + BUFN + i] != 0x5a) guards_intact = false;
    }
    check(guards_intact, "  and nothing was written past the 4 KB buffer in either direction");

    // Stack balance across the grow path too: the box is a to-be-closed slot
    // and the placeholder is a real stack value, so an unbalanced grow leaks
    // one of them per EXEC.
    {
        lua_State* L = script::runtime_state();
        int before = lua_gettop(L);
        for (int i = 0; i < 50; ++i) {
            script::exec_result g = seeded(buf, BUFN);
            script::runtime_exec_capture("t", "return ('z'):rep(3000)", 22,
                                         err, sizeof(err), nullptr, &g);
        }
        check(lua_gettop(L) == before,
              "50 execs whose results outgrow the join buffer leave the stack depth unchanged");
    }
}

void test_exec_capture() {
    char err[256];
    int  kind;

    // Not-initialised is one of the four documented outcomes; pin it from
    // both directions (before init, and again after shutdown, below) rather
    // than leaving it verified only by inspection.
    err[0] = 0;
    kind = -1;
    check(!script::runtime_exec_capture("t", "x = 1", 5, err, sizeof(err), &kind),
          "exec before runtime_init reports failure");
    check(kind == script::EXEC_NOT_INITIALISED, "  tagged as not-initialised");
    check(err[0] != 0, "  and a non-empty message");

    check(script::runtime_init(), "the runtime starts");

    // A recognisable sentinel, not a zeroed buffer: zeroing before the call
    // would make this assertion pass even if the function's own clearing
    // line were deleted. Overwriting a sentinel proves the function did it.
    strcpy(err, "SENTINEL");
    check(script::runtime_exec_capture("t", "x = 1 + 1", 9, err, sizeof(err)),
          "a valid chunk runs and reports success");
    check(err[0] == 0, "  and overwrites the sentinel with an empty error buffer");

    err[0] = 0;
    kind = -1;
    check(!script::runtime_exec_capture("t", "x = (", 5, err, sizeof(err), &kind),
          "a syntax error is reported as failure");
    check(kind == script::EXEC_COMPILE_ERROR, "  tagged as a compile error");
    // luaL_loadbuffer renders a plain chunk name as [string "t"], not "t", so
    // assert on what Lua actually produces: the name and a line number.
    check(strstr(err, "\"t\"") != nullptr && strstr(err, ":1:") != nullptr,
          "  the message names the chunk and the line");

    err[0] = 0;
    kind = -1;
    check(!script::runtime_exec_capture("t", "error('boom')", 13, err, sizeof(err), &kind),
          "a raising chunk is reported as failure");
    check(kind == script::EXEC_RUNTIME_ERROR, "  tagged as a runtime error");
    check(strstr(err, "boom") != nullptr, "  and the message carries the error text");

    // Truncation: a message far longer than err_len must still come back
    // truncated, NUL-terminated, and must never write past the buffer. Guard
    // bytes flank a tiny buffer inside one larger array so an overrun in
    // either direction is caught rather than merely inspected for.
    {
        const size_t GUARD = 8, BUFN = 16;
        char region[GUARD + BUFN + GUARD];
        memset(region, 0x5a, sizeof(region));
        char* buf = region + GUARD;
        memset(buf, 0x7e, BUFN);

        // A long chunk name makes the rendered "[string "..."]" prefix alone
        // exceed the buffer, so truncation is forced regardless of exactly
        // how Lua's own internal chunk-id truncation behaves.
        char long_name[300];
        memset(long_name, 'n', sizeof(long_name) - 1);
        long_name[sizeof(long_name) - 1] = 0;

        check(!script::runtime_exec_capture(long_name, "error('boom')", 13, buf, BUFN),
              "a message longer than err_len still reports failure");
        check(strlen(buf) < BUFN, "  the message is truncated to fit, NUL-terminated");

        bool guards_intact = true;
        for (size_t i = 0; i < GUARD; ++i) {
            if ((unsigned char)region[i] != 0x5a) guards_intact = false;
            if ((unsigned char)region[GUARD + BUFN + i] != 0x5a) guards_intact = false;
        }
        check(guards_intact, "  and nothing was written past the buffer");
    }

    // Stack balance: every failed exec must leave the Lua stack exactly where
    // it found it. A leaked error object per failed EXEC would accumulate
    // silently over a long editing session rather than fail loudly here.
    {
        lua_State* L = script::runtime_state();
        int before = lua_gettop(L);
        for (int i = 0; i < 50; ++i) {
            script::runtime_exec_capture("t", "x = (", 5, err, sizeof(err));
            script::runtime_exec_capture("t", "error('boom')", 13, err, sizeof(err));
        }
        check(lua_gettop(L) == before,
              "50 rounds of failing execs leave the Lua stack depth unchanged");
    }

    test_exec_results();
    test_exec_results_past_the_buffer();

    script::runtime_shutdown();

    err[0] = 0;
    kind = -1;
    check(!script::runtime_exec_capture("t", "x = 1", 5, err, sizeof(err), &kind),
          "exec after runtime_shutdown reports failure");
    check(kind == script::EXEC_NOT_INITIALISED, "  tagged as not-initialised");
    check(err[0] != 0, "  and a non-empty message");
}

// resource_list's packing loop used to skip an entry that didn't fit the
// output buffer and keep going -- so a long name got dropped while a later,
// shorter one slipped in right after it, and the reply wasn't even a prefix
// of the real list, with no way for the client to tell an entry was missing.
// This drives the real script::resource_list_pack -- the exact function
// resource_list itself calls to do the packing -- directly, without booting
// the Lua runtime or touching ORBIS, both of which the rest of resource_list
// depends on.
struct list_pack_entry { const char* name; const char* state; };

bool list_pack_next(void* ctx, int i, char* name, size_t name_cap, char* state, size_t state_cap) {
    const list_pack_entry* e = (const list_pack_entry*)ctx;
    snprintf(name, name_cap, "%s", e[i].name);
    snprintf(state, state_cap, "%s", e[i].state);
    return true;
}

void test_resource_list_pack() {
    // Entry 2's name alone is long enough that entry 1 + entry 2 cannot both
    // fit in the small buffer below, forcing a break right there. Entry 3
    // would fit on its own -- the defect this guards against is exactly it
    // sneaking into the reply after entry 2 was dropped.
    char long_name[100];
    memset(long_name, 'b', sizeof(long_name) - 1);
    long_name[sizeof(long_name) - 1] = 0;

    list_pack_entry entries[3] = {
        { "alpha",   "started" },
        { long_name, "started" },
        { "charlie", "stopped" },
    };

    const size_t TRUNC_LEN = strlen(script::RESOURCE_LIST_TRUNC_MARK);
    const size_t LIMIT     = 50;                  // fits "alpha started\n" (14), not +100 more
    const size_t OUT_LEN   = LIMIT + TRUNC_LEN;

    // Guard bytes flank the buffer on both sides so a write past either end
    // is caught, not just inspected for. BUFN matches OUT_LEN exactly -- the
    // same size passed to resource_list_pack as out_len -- so the guards sit
    // immediately after the caller's actual buffer; sized any larger, an
    // overflow would have to clear that slack before it could ever touch a
    // guard byte, and the check below would pass regardless of whether the
    // packing loop honoured out_len.
    const size_t GUARD = 8, BUFN = OUT_LEN;
    char region[GUARD + BUFN + GUARD];
    memset(region, 0x5a, sizeof(region));
    char* out = region + GUARD;

    int used = script::resource_list_pack(out, OUT_LEN, 3, list_pack_next, entries);

    check(used > 0, "an overflowing list still returns a non-empty reply");
    check((size_t)used <= OUT_LEN, "  and never exceeds the caller's buffer length");

    // The real caller (net::reply) works off this returned length, not a NUL
    // terminator -- resource_list_pack never promises one past `used`, same
    // as on_ls. Cap it here purely so the checks below can use ordinary
    // string functions; well within OUT_LEN, so this exercises nothing the
    // assertion above didn't already cover.
    out[used] = 0;
    check(strncmp(out, "alpha started\n", 14) == 0,
          "  the entry that fits is written first, intact");
    check(strstr(out, "charlie") == nullptr,
          "  an entry after a dropped one is never smuggled into the reply");
    // memcmp, not strcmp: the marker is appended by length, not NUL-terminated
    // (the caller gets an explicit byte count back, same as on_ls's reply).
    check((size_t)used > TRUNC_LEN &&
          memcmp(out + used - TRUNC_LEN, script::RESOURCE_LIST_TRUNC_MARK, TRUNC_LEN) == 0,
          "  and the reply ends with the truncation marker");

    bool guards_intact = true;
    for (size_t i = 0; i < GUARD; ++i) {
        if ((unsigned char)region[i] != 0x5a) guards_intact = false;
        if ((unsigned char)region[GUARD + BUFN + i] != 0x5a) guards_intact = false;
    }
    check(guards_intact, "  and nothing was written past the caller's buffer in either direction");

    // A list that fits entirely comes back whole, with no marker at all: the
    // fix must not turn every reply into a truncated one.
    list_pack_entry small[2] = { { "alpha", "started" }, { "charlie", "stopped" } };
    char out2[256];
    int used2 = script::resource_list_pack(out2, sizeof(out2), 2, list_pack_next, small);
    check(used2 == (int)strlen("alpha started\ncharlie stopped\n") &&
          strcmp(out2, "alpha started\ncharlie stopped\n") == 0,
          "a list that fits entirely is returned whole, with no truncation marker");
}

}  // namespace

int main() {
    test_frame();
    test_mailbox();
    test_path_guard();
    test_auth_policy();
    test_token_classify();
    test_exec_capture();
    test_resource_list_pack();
    printf("\n%s (%d failures)\n", g_failures ? "FAILED" : "PASSED", g_failures);
    return g_failures ? 1 : 0;
}
