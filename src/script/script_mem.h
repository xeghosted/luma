#pragma once
#include <stdint.h>
#include "game/profile.h"

// RAGE script-global addressing.
//
// The arithmetic is the RAGE VM's GLOBAL opcode and is the same in both engines:
//
//   global_addr = *(table_base + 8 * (idx >> 26)) + 8 * ((idx >> 8) & 0x3FFFF)
//
// where table_base is an array of block base pointers with 8-byte entries. Pure
// math, so it is host-testable without a console.
//
// WHAT IS NOT KNOWN YET: where that array lives in the RDR2 v1.32 eboot.
// GTALua's value (0x3E0CFB8) is GTA V CUSA00411 v1.57's and is meaningless
// here -- in this build that offset is simply some other data, and using it
// would read and WRITE arbitrary game memory.
//
// So the RVA is deliberately left at 0 and script_globals_available() gates
// every caller. script_global()/set_script_global() report the feature as
// unavailable rather than returning plausible-looking numbers, because a
// silently wrong global is the failure mode that costs the most to find.
// Everything else in RDR2Lua -- all 7,072 natives, the frame hook, resources --
// works without it.
//
// To fill it in: locate the VM's GLOBAL opcode handler, read the array it
// indexes, and put the RVA below. Then record it in the ps4-offsets catalog
// under CUSA03041/1.32 with the evidence, the way every other address here was.
namespace script {

// The RVA now comes from the detected game's profile: GTA V has it, RDR2 does
// not, and one build serves both. 0 still means "not located", and every caller
// still refuses rather than dereferencing an address derived from nothing.
inline uint64_t script_globals_rva() {
    const game::profile* p = game::current();
    return p ? p->script_globals_rva : 0;
}

inline bool script_globals_available() {
    return script_globals_rva() != 0;
}

inline uint64_t script_global_addr(uint64_t table_base_abs, uint32_t idx) {
    uint64_t block_ptr = *(uint64_t*)(table_base_abs + 8ull * (idx >> 26));
    return block_ptr + 8ull * ((idx >> 8) & 0x3FFFF);
}

}  // namespace script
