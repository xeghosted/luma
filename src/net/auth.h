#pragma once
#include <stdint.h>
#include "net/frame.h"

// Who may send what, before and after HELLO.
//
// Split out of dispatch.cpp because dispatch.cpp is ORBIS-only — it opens the
// token file with sceKernelOpen — and a policy no host test can reach is a
// policy nobody checks. The file read and the per-connection flag stay there;
// the decision lives here, where tests/net_test.cpp compiles it.
//
// The rule, from spec 4.4:
//
//   * No token file: nothing changes. That is the documented default, and on a
//     home LAN with a jailbroken console anyone who can reach 9616 can already
//     reach FTP. A channel that started refusing work here would break every
//     setup that exists today for no gain.
//
//   * A token file exists: a connection that has not passed HELLO may send
//     HELLO (to try) and PING (to see the channel is alive), and nothing else.
//
// Gating HELLO alone would be theatre. Nothing in the protocol requires a
// client to say hello first, so a stranger could simply send EXEC and never
// present a token at all. And "read-only is harmless" does not hold either:
// RESLIST tells them what is on the console and GET hands them the contents.
//
// A whitelist rather than a blacklist, so an op added later is refused until
// somebody decides otherwise, instead of being allowed through by omission.
namespace net {

    inline bool auth_allows(uint16_t op, bool have_token, bool authed) {
        if (!have_token || authed) return true;
        return op == OP_HELLO || op == OP_PING;
    }

    // What is known about /data/rdr2lua/token right now. Resolved per connection
    // by the dispatcher; the states live here because the rule below is the
    // interesting part and it belongs where a test can reach it.
    enum token_file_state : int {
        TOKEN_UNKNOWN = 0,   // not established since the last connection
        TOKEN_NONE,          // no file, or a file with nothing usable in it
        TOKEN_PRESENT,       // a secret is loaded
        TOKEN_UNUSABLE,      // a file we refuse to work with (too long)
    };

    // Only TOKEN_NONE leaves the channel open.
    //
    // UNKNOWN counting as shut is the part worth stating: the load can race with
    // a connection being accepted and leave the state unestablished, and reading
    // that as "no token file" would let every op through on exactly the message
    // that lost the race -- a window a client could retry until it hit. Shut is
    // also what on_hello does with UNKNOWN, where nothing but TOKEN_PRESENT can
    // authenticate, so the two paths agree.
    inline bool token_gates(int state) { return state != TOKEN_NONE; }

    // What a token file's bytes mean, separated from reading them for the same
    // reason as above: the read is ORBIS-only, the decision is not — and every
    // defect this feature has had so far was in the decision, not the read. An
    // empty file, a file one byte too long, a secret whose trailing newline
    // pushed it over the limit, and an off-by-one that wrote past the buffer.
    enum token_verdict {
        TOKEN_NO_SECRET,   // no usable bytes: the channel stays OPEN
        TOKEN_OK,          // *out_len bytes of secret
        TOKEN_TOO_LONG,    // refused, and the channel stays SHUT
    };

    // `max_len` is the longest secret the caller can store (its buffer minus
    // the NUL). The line ending is stripped BEFORE the length is judged, so a
    // secret of exactly max_len bytes written by an editor is not refused for
    // the newline the editor added.
    inline token_verdict token_classify(const char* buf, size_t len,
                                        size_t max_len, size_t* out_len) {
        while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r')) --len;
        *out_len = len;
        if (len == 0)       return TOKEN_NO_SECRET;
        if (len > max_len)  return TOKEN_TOO_LONG;
        return TOKEN_OK;
    }

    // Compares without leaking the length of the match through timing. Not a
    // serious side-channel defence — this is a LAN cable and a jailbroken
    // console — but strcmp would give that away for free and this does not.
    inline bool token_matches(const char* want, size_t want_len,
                              const char* got, size_t got_len) {
        if (want_len != got_len) return false;
        unsigned char diff = 0;
        for (size_t i = 0; i < want_len; ++i)
            diff |= (unsigned char)(want[i] ^ got[i]);
        return diff == 0;
    }

}  // namespace net
