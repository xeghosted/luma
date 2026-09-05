// src/net/net_server_reply.h
#pragma once
#include <stdint.h>

// The game thread's way to answer a request. Frames are queued, not sent: a
// blocking send() inside the frame hook would freeze the game.
//
// Replies carry their payload on the heap: net_server.cpp's reply() mallocs
// and copies it in, and flush() frees it after sending. There is no fixed
// per-reply size limit and nothing here truncates a reply.

namespace net {

    void reply(uint8_t op, uint16_t seq, const void* payload, uint32_t len);

    // Convenience for the common text replies.
    void reply_ok(uint16_t seq, const char* text);
    void reply_err(uint16_t seq, const char* text);

}  // namespace net
