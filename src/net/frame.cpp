// src/net/frame.cpp
#include "net/frame.h"

namespace net {
namespace {
    const uint8_t MAGIC[4] = { 'G', 'L', 'U', 'A' };
}

void encode_header(uint8_t out[HEADER_SIZE], const header& h) {
    out[0] = MAGIC[0]; out[1] = MAGIC[1]; out[2] = MAGIC[2]; out[3] = MAGIC[3];
    out[4] = h.op;
    out[5] = h.flags;
    out[6] = (uint8_t)(h.seq & 0xff);
    out[7] = (uint8_t)(h.seq >> 8);
    out[8]  = (uint8_t)(h.len & 0xff);
    out[9]  = (uint8_t)((h.len >> 8) & 0xff);
    out[10] = (uint8_t)((h.len >> 16) & 0xff);
    out[11] = (uint8_t)((h.len >> 24) & 0xff);
}

bool decode_header(const uint8_t in[HEADER_SIZE], header* out) {
    if (in[0] != MAGIC[0] || in[1] != MAGIC[1] || in[2] != MAGIC[2] || in[3] != MAGIC[3])
        return false;
    uint32_t len = (uint32_t)in[8] | ((uint32_t)in[9] << 8)
                 | ((uint32_t)in[10] << 16) | ((uint32_t)in[11] << 24);
    if (len > MAX_PAYLOAD)
        return false;
    out->op    = in[4];
    out->flags = in[5];
    out->seq   = (uint16_t)((uint16_t)in[6] | ((uint16_t)in[7] << 8));
    out->len   = len;
    return true;
}

}  // namespace net
