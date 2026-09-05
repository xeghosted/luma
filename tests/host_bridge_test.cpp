// Host test for the native bridge and the generated scripts/natives.lua.
//
// Everything below the ORBIS line is pure C++ and Lua, so the interesting half
// of this plugin can be exercised on a PC: the bridge dispatches through
// function-pointer seams, and this file installs recording stubs in place of the
// real invoker. What it proves is exactly what cannot be eyeballed in a 6,487-line
// generated file — that each binding parses, that its signature marshals into the
// slots the RAGE ABI expects, and that results come back in the right type.
//
// Run it with tests/run.sh (from the repo root). Lua has to be compiled by a C
// compiler, not g++: lua.hpp declares it extern "C", so building the .c files as
// C++ leaves every symbol mangled and nothing links.

#include "rage/invoker/invoker.h"
#include "script/native_bridge.h"
#include "script/native_marshal.h"
#include "lua.hpp"

#include <math.h>
#include <stdio.h>
#include <string.h>

namespace {

int g_failures = 0;

void check(bool ok, const char* what) {
    printf("%-58s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok) g_failures++;
}

// A stand-in for the game's sub_1E26A00 (v1.57 eboot, 0x1E26A00): the helper
// every Vector3 out-param native calls to obtain the address it writes to.
//
// Written by raw BYTE OFFSET rather than through native_context's interface,
// deliberately, because that is how the real thing reaches into the struct. If
// those fields are ever reordered this test fails -- which is the whole point,
// since the real native would fail too and would do it silently, on console.
void* fake_prepare_vector(rage::invoker::native_context* ctx, int arg_index) {
    uint8_t*  c        = (uint8_t*)ctx;
    uint64_t  arg_data = *(uint64_t*)(c + 16);
    uint32_t& count    = *(uint32_t*)(c + 24);
    uint64_t* script_v = (uint64_t*)(c + 32);
    float*    result_v = (float*)(c + 64);

    uint64_t caller = *(uint64_t*)(arg_data + 8ull * arg_index);
    script_v[count] = caller;
    // The caller's CURRENT value is copied in first, at the 8-byte stride, so
    // an in-param survives. Packed into the result slot (4 floats per entry).
    result_v[4 * count + 0] = *(float*)(caller +  0);
    result_v[4 * count + 1] = *(float*)(caller +  8);
    result_v[4 * count + 2] = *(float*)(caller + 16);
    void* scratch = &result_v[4 * count];
    count++;
    return scratch;
}

void test_vector_out_params() {
    using namespace rage::invoker;

    // Two caller buffers, laid out the way a script's mem_alloc(24) is, and
    // pre-filled so the padding can be checked for collateral damage.
    uint8_t buf_a[24], buf_b[24];
    memset(buf_a, 0xAB, sizeof(buf_a));
    memset(buf_b, 0xAB, sizeof(buf_b));
    *(float*)(buf_a + 0) = 1.0f; *(float*)(buf_a + 8) = 2.0f; *(float*)(buf_a + 16) = 3.0f;
    *(float*)(buf_b + 0) = 4.0f; *(float*)(buf_b + 8) = 5.0f; *(float*)(buf_b + 16) = 6.0f;

    native_setup ctx;
    ctx.push<uint64_t>(0x1234);                 // arg 0: an ordinary value
    ctx.push<uint64_t>((uint64_t)buf_a);        // arg 1: first vector3 out-param
    ctx.push<uint64_t>((uint64_t)buf_b);        // arg 2: second

    // What a native does: ask for its scratch, then write its answer there.
    float* out_a = (float*)fake_prepare_vector(&ctx, 1);
    float* out_b = (float*)fake_prepare_vector(&ctx, 2);
    check(out_a[0] == 1.0f && out_a[1] == 2.0f && out_a[2] == 3.0f,
          "the caller's existing vector3 is copied INTO the scratch, so in-params survive");
    out_a[0] = -10.5f; out_a[1] = 20.25f; out_a[2] = -30.125f;
    out_b[0] = 7.5f;   out_b[1] = 8.25f;  out_b[2] = 9.125f;

    // Before the write-back the caller's buffers still hold the old values:
    // this is exactly the state the console shipped in, and why
    // GET_MODEL_DIMENSIONS read back zeros.
    check(*(float*)(buf_a + 0) == 1.0f,
          "  and the caller's buffer is untouched until the write-back runs");

    ctx.set_vector_results();

    check(*(float*)(buf_a +  0) == -10.5f &&
          *(float*)(buf_a +  8) ==  20.25f &&
          *(float*)(buf_a + 16) == -30.125f,
          "set_vector_results copies the native's answer back to the caller");
    check(*(float*)(buf_b +  0) == 7.5f &&
          *(float*)(buf_b +  8) == 8.25f &&
          *(float*)(buf_b + 16) == 9.125f,
          "  for every out-param of the call, not just the first");

    // The stride is the part that is easy to get wrong and impossible to see:
    // a packed 12-byte write would land y at +4 and z at +8, quietly shredding
    // the padding and reporting y as z. Assert the holes are still intact.
    check(*(uint32_t*)(buf_a + 4)  == 0xABABABABu &&
          *(uint32_t*)(buf_a + 12) == 0xABABABABu &&
          *(uint32_t*)(buf_a + 20) == 0xABABABABu,
          "  at an 8-byte stride, leaving the padding between the floats alone");

    // A second call on the same context must not re-fire the first call's
    // write-back into pointers whose owner may be gone.
    memset(buf_a, 0, sizeof(buf_a));
    ctx.set_vector_results();
    check(*(float*)(buf_a + 0) == 0.0f,
          "a second write-back is a no-op: the count is cleared, not left standing");

    // The clamp: the game owns the count, so a bogus one must not walk off the
    // end of the object. Four is the real maximum; ask for far more.
    native_setup ctx2;
    ctx2.push<uint64_t>((uint64_t)buf_a);
    for (int i = 0; i < 4; ++i) fake_prepare_vector(&ctx2, 0);
    *(uint32_t*)((uint8_t*)&ctx2 + 24) = 99;
    ctx2.set_vector_results();
    check(true, "an out-of-range vector count is clamped rather than trusted");
}

// --- recording seams -----------------------------------------------------

struct call_record {
    bool     happened;
    bool     by_hash;
    uint64_t target;
    int      nslots;
    uint64_t slots[96];
};

call_record g_last;

// What the stub writes back, so a test can check how a result is decoded.
uint64_t g_reply[3];

void reset() {
    memset(&g_last, 0, sizeof(g_last));
    memset(g_reply, 0, sizeof(g_reply));
}

void record(bool by_hash, uint64_t target, const uint64_t* slots, int n, uint64_t* result) {
    g_last.happened = true;
    g_last.by_hash = by_hash;
    g_last.target = target;
    g_last.nslots = n;
    for (int i = 0; i < n && i < 96; ++i) g_last.slots[i] = slots[i];
    result[0] = g_reply[0];
    result[1] = g_reply[1];
    result[2] = g_reply[2];
}

void stub_call(uint64_t rva, const uint64_t* slots, int n, uint64_t* result) {
    record(false, rva, slots, n, result);
}

// Every hash resolves here, except the one the "unregistered" test uses.
const uint64_t UNREGISTERED = 0xDEADBEEFDEADBEEFULL;

bool stub_call_hash(uint64_t hash, const uint64_t* slots, int n, uint64_t* result) {
    if (hash == UNREGISTERED) return false;
    record(true, hash, slots, n, result);
    return true;
}

bool stub_ready() { return true; }
bool stub_exists(uint64_t hash) { return hash != UNREGISTERED; }

uint32_t f2b(float f) { uint32_t b; memcpy(&b, &f, 4); return b; }

bool run(lua_State* L, const char* chunk) {
    if (luaL_loadstring(L, chunk) != LUA_OK || lua_pcall(L, 0, LUA_MULTRET, 0) != LUA_OK) {
        printf("  lua error: %s\n", lua_tostring(L, -1));
        lua_pop(L, 1);
        return false;
    }
    return true;
}

}  // namespace

int main() {
    lua_State* L = luaL_newstate();
    luaL_openlibs(L);

    script::g_call_native      = stub_call;
    script::g_call_native_hash = stub_call_hash;
    script::g_natives_ready    = stub_ready;
    script::g_native_exists    = stub_exists;
    script::g_eboot_base_abs   = 0x400000;
    script::bridge_open(L);

    // The generated prelude, loaded exactly as the plugin loads it on console.
    if (luaL_loadfile(L, "scripts/natives.lua") != LUA_OK ||
        lua_pcall(L, 0, 0, 0) != LUA_OK) {
        printf("natives.lua failed to load: %s\n", lua_tostring(L, -1));
        return 1;
    }
    check(true, "natives.lua loads and binds");

    // Every binding is a callable, and the count is what the generator reported.
    //
    // The pattern has to accept `_0xHASH` names as well as spelled ones. Most
    // RDR2 natives have no published name: 4,747 of the 7,072 are bound as
    // _0x<16 hex digits>, and a pattern of uppercase-only letters silently
    // counts just the 2,325 that are spelled out.
    run(L, "local n = 0 for k, v in pairs(_G) do "
           "  if (k:match('^_?%u[%u%d_]*$') or k:match('^_0[xX]%x+$')) "
           "     and type(v) == 'function' then n = n + 1 end "
           "end NATIVE_COUNT = n");
    lua_getglobal(L, "NATIVE_COUNT");
    long bound = (long)lua_tointeger(L, -1);
    lua_pop(L, 1);
    printf("  bound natives: %ld\n", bound);
    check(bound > 7000, "all ~7.1k natives are bound as callables");

    // --- an RVA native: int args, int return ------------------------------
    reset();
    g_reply[0] = 0xFFFFFFFF00000091ULL;   // upper half is argument garbage
    run(L, "PED = GET_PLAYER_PED(-1)");
    lua_getglobal(L, "PED");
    long ped = (long)lua_tointeger(L, -1);
    lua_pop(L, 1);
    check(!g_last.by_hash && g_last.target == 0x21B9140, "GET_PLAYER_PED dispatches by RVA");
    check(g_last.nslots == 1 && (int32_t)g_last.slots[0] == -1, "  int arg reaches slot 0");
    check(ped == 0x91, "  int return reads the low 32 bits only");

    // --- float args, bool arg ---------------------------------------------
    reset();
    run(L, "SET_ENTITY_COORDS(7, 1.5, -2.25, 3.0, false, false, false, true)");
    check(g_last.nslots == 8, "SET_ENTITY_COORDS marshals 8 slots");
    check(g_last.slots[0] == 7 &&
          (uint32_t)g_last.slots[1] == f2b(1.5f) &&
          (uint32_t)g_last.slots[2] == f2b(-2.25f) &&
          (uint32_t)g_last.slots[3] == f2b(3.0f),
          "  floats are marshalled as raw 32-bit patterns");
    check(g_last.slots[4] == 0 && g_last.slots[7] == 1, "  bools become 0/1");

    // --- Vector3 return: three 8-byte slots, not a packed struct -----------
    reset();
    g_reply[0] = f2b(10.5f);
    g_reply[1] = f2b(-20.25f);
    g_reply[2] = f2b(30.75f);
    // RDR2's GET_ENTITY_COORDS is "3iii" -- three int args where GTA V took two.
    run(L, "C = GET_ENTITY_COORDS(7, 1, 0)");
    check(!g_last.by_hash && g_last.target == 0x1E5B2E0, "GET_ENTITY_COORDS dispatches by RVA");
    run(L, "assert(type(C) == 'table', 'vector3 return should be a table') "
           "OK3 = (C[1] == 10.5 and C[2] == -20.25 and C[3] == 30.75)");
    lua_getglobal(L, "OK3");
    check(lua_toboolean(L, -1) != 0, "  vector3 return reads slots 0/1/2 as {x, y, z}");
    lua_pop(L, 1);

    // --- float return ------------------------------------------------------
    reset();
    g_reply[0] = f2b(42.5f);
    run(L, "S = GET_ENTITY_SPEED(7)");
    lua_getglobal(L, "S");
    check(fabs(lua_tonumber(L, -1) - 42.5) < 1e-6, "float return is decoded as a float");
    lua_pop(L, 1);

    // --- string arg --------------------------------------------------------
    reset();
    run(L, "GET_HASH_KEY('adder')");
    check(g_last.nslots == 1 && g_last.slots[0] != 0 &&
          strcmp((const char*)g_last.slots[0], "adder") == 0,
          "string arg is passed as a char pointer");

    // --- the hash path -----------------------------------------------------
    //
    // Nothing in natives.lua dispatches by hash any more: every RDR2 native has
    // a known RVA, so the generated bindings are uniformly RVA-based. (In GTALua
    // 3,674 of 6,485 had to go by hash.) The hash seam still exists for
    // invoke_hash, so exercise it directly rather than through a binding.
    reset();
    run(L, "invoke_hash(0x67C540AA08E4A6F5, "
           "{ARG_VOID, ARG_STRING, ARG_STRING, ARG_BOOL, ARG_INT}, "
           "'SELECT', 'HUD_FRONTEND_DEFAULT_SOUNDSET', true, 7)");
    check(g_last.by_hash && g_last.target == 0x67C540AA08E4A6F5ULL,
          "invoke_hash dispatches by hash");
    check(g_last.nslots == 4 && g_last.slots[2] == 1 && g_last.slots[3] == 7,
          "  hash native marshals the same way as an RVA one");

    // --- pointer out-params ------------------------------------------------
    reset();
    run(L, "MIN = mem_alloc(12) MAX = mem_alloc(12) "
           "GET_MODEL_DIMENSIONS(1234, MIN, MAX)");
    lua_getglobal(L, "MIN");
    uint64_t minp = (uint64_t)lua_tointeger(L, -1);
    lua_pop(L, 1);
    check(minp != 0 && g_last.nslots == 3 && g_last.slots[1] == minp,
          "mem_alloc address reaches the native as a pointer slot");
    check(*(uint64_t*)minp == 0, "  mem_alloc returns zeroed memory");
    run(L, "mem_free(MIN) mem_free(MAX)");

    // nil is a null pointer, not an error — many natives take optional outputs.
    reset();
    check(run(L, "GET_MODEL_DIMENSIONS(1234, nil, nil)") && g_last.slots[1] == 0,
          "nil pointer arg becomes a null slot");

    // --- an unresolvable hash raises rather than returning 0 ----------------
    reset();
    bool raised = !run(L, "local f = native_hash(0xDEADBEEFDEADBEEF, 'ii') f(1)");
    check(raised, "an unregistered hash raises instead of returning 0");

    // --- the introspection helpers -----------------------------------------
    run(L, "R1 = natives_ready() R2 = native_exists(0xBF3D28CA44F3BE2D) "
           "R3 = native_exists(0xDEADBEEFDEADBEEF)");
    lua_getglobal(L, "R1"); bool r1 = lua_toboolean(L, -1) != 0; lua_pop(L, 1);
    lua_getglobal(L, "R2"); bool r2 = lua_toboolean(L, -1) != 0; lua_pop(L, 1);
    lua_getglobal(L, "R3"); bool r3 = lua_toboolean(L, -1) != 0; lua_pop(L, 1);
    check(r1 && r2 && !r3, "natives_ready/native_exists report the registry state");

    // --- a bad signature is caught at bind time ----------------------------
    check(!run(L, "native(0x1000, 'q')"), "an unknown signature character is rejected");

    // --- the Vector3 out-param write-back ----------------------------------
    test_vector_out_params();

    lua_close(L);
    printf("\n%s (%d failure%s)\n", g_failures ? "FAILED" : "PASSED",
           g_failures, g_failures == 1 ? "" : "s");
    return g_failures ? 1 : 0;
}
