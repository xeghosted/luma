#pragma once
#include <stdint.h>

// RDR2 native resolution: a table compiled in from the eboot before boot.
// See natives_rdr2.cpp; RDR2 does not obfuscate its registry, so no walk.
namespace game::rdr2 {
    bool     build();
    void*    find_hash(uint64_t hash);
    uint32_t count();
    bool     usable();
}
