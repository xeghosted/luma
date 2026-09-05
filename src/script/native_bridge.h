#pragma once
#include <stdint.h>

// Lua <-> RAGE native bridge. Registers the invoke/native/memory/global
// primitives and the ARG_* tag constants into a lua_State. The actual native
// dispatch goes through swappable function-pointer seams so the bridge stays
// free of any ORBIS/game dependency and is host-testable with a stub; the
// payload installs the real invoker-backed seams at startup.
//
// Two seams because the game exposes natives two ways. Most are reached by RVA
// (the impl address, added to the runtime image base) and work from the first
// frame. The rest — roughly half of what the build registers, and absent from
// the generated natives.h — are reached by 64-bit hash through the registry
// rage::hash_natives recovers from the game's own table, which has to be walked
// first. See scripts/natives.lua, which binds both under one set of names.

struct lua_State;

namespace script {

// The native-call seam: run the impl at `rva` with `n` pre-marshalled 8-byte
// argument slots, writing up to 3 result slots (scalar in [0], vec3 in [0..2]).
using call_native_fn = void (*)(uint64_t rva, const uint64_t* slots, int n,
                                uint64_t* result);

// Same, by hash. Returns false when the registry is not usable yet or the hash
// is not registered in this build — a distinction the caller must not silently
// turn into a zero, so the Lua side raises an error on false.
using call_native_hash_fn = bool (*)(uint64_t hash, const uint64_t* slots, int n,
                                     uint64_t* result);

// True once the hash registry has been walked and verified.
using natives_ready_fn = bool (*)();

// True if this build registers `hash`, whether or not it has been called.
using native_exists_fn = bool (*)(uint64_t hash);
// The ADDRESS a hash resolves to, or 0. native_exists answers yes/no, which is
// enough for a script guarding a call but not enough to audit the RVA table --
// that needs the game's own answer for a hash put beside our RVA for the same
// native. See tools/gen_native_crosscheck.py.
using native_addr_fn   = uint64_t (*)(uint64_t hash);

// Installed by the payload (real invoker) or a test (stub). Null => invoke errors.
extern call_native_fn      g_call_native;
extern call_native_hash_fn g_call_native_hash;
extern natives_ready_fn    g_natives_ready;
extern native_exists_fn    g_native_exists;
extern native_addr_fn      g_native_addr;

// Absolute runtime base of the eboot's first segment (g_eboot_base). Used by
// rva()/script_global(). 0 on host; set by the payload before running scripts.
extern uint64_t g_eboot_base_abs;

// Register the native, memory and script-global primitives plus the ARG_*
// globals into L. Returns true on success.
bool bridge_open(lua_State* L);

}  // namespace script
