// src/net/frame.h
#pragma once
#include <stdint.h>
#include <stddef.h>

// The control channel's wire format. Deliberately free of any ORBIS or game
// dependency so it can be tested on a host against the same fixture vectors the
// TypeScript client uses (tests/protocol_vectors.json).
//
//   magic 'GLUA' (4) | op (1) | flags (1) | seq (2) | len (4) | payload[len]
//
// Integers are little-endian. seq correlates a reply with its request; server
// frames nobody asked for (LOG, EVENT) carry seq = 0.

namespace net {

    const size_t   HEADER_SIZE  = 12;
    const uint32_t MAX_PAYLOAD  = 8u << 20;

    enum op : uint8_t {
        OP_HELLO  = 1,
        OP_PING   = 2,
        OP_PONG   = 3,
        OP_PUT    = 4,
        OP_GET    = 5,
        OP_LS     = 6,
        OP_RM     = 7,
        OP_EXEC   = 8,
        OP_RELOAD = 9,
        OP_LOG    = 10,
        OP_EVENT  = 11,
        OP_OK     = 12,
        OP_ERR    = 13,
        OP_BUSY   = 14,
        OP_START   = 15,
        OP_STOP    = 16,
        OP_RESTART = 17,
        OP_RESLIST = 18,
        OP_RESINFO = 19,
        OP_CMD     = 20,

        // Not a wire value, and never sent or matched: one past the last
        // opcode, so a test can assert that its own name table covers every
        // member of this enum. C++ cannot enumerate an enum, and the opcode
        // table in tests/net_test.cpp is hand-written -- an opcode added here
        // and to the TypeScript Op but to neither name table nor
        // tests/protocol_vectors.json would leave all three suites green,
        // agreeing perfectly about the twenty they knew about. Keep last.
        OP__COUNT
    };

    struct header {
        uint8_t  op;
        uint8_t  flags;
        uint16_t seq;
        uint32_t len;
    };

    void encode_header(uint8_t out[HEADER_SIZE], const header& h);

    // False on a bad magic or a length past MAX_PAYLOAD — both mean the stream
    // is not one of ours and the connection should close.
    bool decode_header(const uint8_t in[HEADER_SIZE], header* out);

}  // namespace net
