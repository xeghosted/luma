// src/net/mailbox.h
#pragma once
#include <stdint.h>
#include <stddef.h>

// The two buffers between the net thread and the game thread. No ORBIS, no Lua:
// this is the seam, and it is host-tested.
//
// Inbox (net -> game): the net thread reads a whole frame into a malloc'd buffer
// and pushes only the pointer. Ownership passes to the popper, which must free
// it. Bounded, because an unbounded queue in front of a frame-rate consumer is
// just a slower way to run out of memory.
//
// Outbox (game -> net): a fixed ring of fixed-width lines. The game thread must
// never malloc and never send() on the logging path, so both are impossible
// here by construction. On overflow the oldest line is dropped and the drop is
// reported, because a log that silently loses lines is worse than no log.

namespace net {

    struct message {
        uint8_t  op;
        uint16_t seq;
        uint32_t len;
        uint8_t* data;      // malloc'd; null when len == 0
    };

    const int INBOX_CAP    = 16;
    const int OUTBOX_SLOTS = 256;
    const int OUTBOX_LINE  = 256;

    // Appended, in place of the tail it displaces, to a line too long for a
    // slot. The ring already refuses to lose a whole line silently -- that is
    // what the drop counter is for -- and losing the TAIL of one silently is
    // the same defect one level down. Until this existed it was the only
    // truncation in the system a receiver could not detect: every other cut
    // reply (LS, EXEC, RESLIST) already carries a marker, and a console
    // session found print() being cut at 255 characters with nothing to say
    // so. Named here rather than written out twice, so the suite asserts the
    // same string the code appends.
    extern const char OUTBOX_TRUNC_MARK[];

    // False when full: the caller still owns m.data and should reply BUSY.
    bool inbox_push(const message& m);

    // False when empty. On true the caller owns out->data and must free it.
    bool inbox_pop(message* out);

    // Game thread. Never blocks, never allocates. Truncates past OUTBOX_LINE-1.
    void outbox_push_line(const char* s);

    // Net thread. False when empty.
    bool outbox_pop_line(char out[OUTBOX_LINE]);

}  // namespace net
