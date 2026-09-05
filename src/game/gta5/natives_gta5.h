#pragma once
#include <stdint.h>

// GTA V native resolution: recovered from the game's own registry at runtime.
// See natives_gta5.cpp for why this game needs a walk and RDR2 does not.
namespace game::gta5 {
    bool     build();
    void*    find_hash(uint64_t hash);
    uint32_t count();
    bool     usable();
}
