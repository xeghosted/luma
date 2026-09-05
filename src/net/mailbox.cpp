// src/net/mailbox.cpp
#include "net/mailbox.h"

#include <stdio.h>
#include <string.h>

namespace net {
namespace {

    // Same spinlock shape as src/game/game_thread.cpp: word-sized clang atomics,
    // lock-free on x86-64, no runtime library. There is no STL here.
    volatile int g_in_lock  = 0;
    volatile int g_out_lock = 0;

    inline void lock(volatile int* l) {
        while (__atomic_exchange_n(l, 1, __ATOMIC_ACQUIRE)) { }
    }
    inline void unlock(volatile int* l) {
        __atomic_store_n(l, 0, __ATOMIC_RELEASE);
    }

    message  g_in[INBOX_CAP];
    int      g_in_head = 0;
    int      g_in_count = 0;

    char     g_out[OUTBOX_SLOTS][OUTBOX_LINE];
    int      g_out_head = 0;
    int      g_out_count = 0;
    int      g_dropped = 0;

}  // namespace

bool inbox_push(const message& m) {
    lock(&g_in_lock);
    if (g_in_count >= INBOX_CAP) {
        unlock(&g_in_lock);
        return false;
    }
    g_in[(g_in_head + g_in_count) % INBOX_CAP] = m;
    g_in_count++;
    unlock(&g_in_lock);
    return true;
}

bool inbox_pop(message* out) {
    lock(&g_in_lock);
    if (g_in_count == 0) {
        unlock(&g_in_lock);
        return false;
    }
    *out = g_in[g_in_head];
    g_in_head = (g_in_head + 1) % INBOX_CAP;
    g_in_count--;
    unlock(&g_in_lock);
    return true;
}

const char OUTBOX_TRUNC_MARK[] = " -- truncated: log line incomplete --";

// A marker that did not leave room for any of the line would turn every long
// line into nothing but the marker, which is a different way of losing the
// text. Checked at compile time because the slot size and the marker are
// edited independently.
static_assert(sizeof(OUTBOX_TRUNC_MARK) < (size_t)OUTBOX_LINE / 2,
              "the truncation marker must leave most of a slot for the line itself");

void outbox_push_line(const char* s) {
    const size_t MARK_LEN = sizeof(OUTBOX_TRUNC_MARK) - 1;

    lock(&g_out_lock);
    if (g_out_count >= OUTBOX_SLOTS) {
        g_out_head = (g_out_head + 1) % OUTBOX_SLOTS;   // drop the oldest
        g_out_count--;
        g_dropped++;
    }
    char* slot = g_out[(g_out_head + g_out_count) % OUTBOX_SLOTS];
    size_t n = strlen(s);
    if (n > (size_t)OUTBOX_LINE - 1) {
        // Room for the marker is reserved up front rather than discovered not
        // to fit once the slot is already full -- the same rule on_ls and
        // resource_list_pack follow for their listings.
        n = (size_t)OUTBOX_LINE - 1 - MARK_LEN;
        memcpy(slot, s, n);
        memcpy(slot + n, OUTBOX_TRUNC_MARK, MARK_LEN);
        n += MARK_LEN;
    } else {
        memcpy(slot, s, n);
    }
    slot[n] = 0;
    g_out_count++;
    unlock(&g_out_lock);
}

bool outbox_pop_line(char out[OUTBOX_LINE]) {
    lock(&g_out_lock);
    if (g_dropped > 0) {
        int d = g_dropped;
        g_dropped = 0;
        unlock(&g_out_lock);
        snprintf(out, OUTBOX_LINE, "[rdr2lua] %d log line(s) dropped", d);
        return true;
    }
    if (g_out_count == 0) {
        unlock(&g_out_lock);
        return false;
    }
    memcpy(out, g_out[g_out_head], OUTBOX_LINE);
    g_out_head = (g_out_head + 1) % OUTBOX_SLOTS;
    g_out_count--;
    unlock(&g_out_lock);
    return true;
}

}  // namespace net
