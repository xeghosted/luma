#include "script/native_bridge.h"
#include "script/native_marshal.h"
#include "script/script_mem.h"
#include "lua.hpp"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace script {

call_native_fn      g_call_native      = nullptr;
call_native_hash_fn g_call_native_hash = nullptr;
natives_ready_fn    g_natives_ready    = nullptr;
native_exists_fn    g_native_exists    = nullptr;
native_addr_fn      g_native_addr      = nullptr;
uint64_t            g_eboot_base_abs   = 0;

namespace {

// The RAGE call context holds 32 argument slots, so 32 is the engine's own
// ceiling rather than ours. RDR2's widest native, _DRAW_MARKER, takes 24, so
// this leaves headroom rather than sitting exactly on the limit.
constexpr int kMaxArgs = 32;

// Convert the Lua value at `sp` into a typed arg_value under an explicit tag.
arg_value coerce(lua_State* L, int sp, arg_tag tag) {
    arg_value v{};
    v.tag = tag;
    switch (tag) {
        case ARG_INT:
            v.i = (int64_t)luaL_checkinteger(L, sp);
            break;
        case ARG_PTR:
            // nil is a legitimate null pointer: a great many natives take an
            // optional out-param that scripts simply do not want.
            v.p = (uint64_t)luaL_optinteger(L, sp, 0);
            break;
        case ARG_BOOL:
            v.i = lua_toboolean(L, sp) ? 1 : 0;
            break;
        case ARG_FLOAT:
            v.f = (float)luaL_checknumber(L, sp);
            break;
        case ARG_STRING:
            // Same reasoning as ARG_PTR: nil means "no string", not an error.
            v.s = luaL_optstring(L, sp, nullptr);
            break;
        case ARG_VEC3: {
            luaL_checktype(L, sp, LUA_TTABLE);
            for (int j = 0; j < 3; ++j) {
                lua_rawgeti(L, sp, j + 1);
                v.v3[j] = (float)lua_tonumber(L, -1);
                lua_pop(L, 1);
            }
            break;
        }
        default:
            break;
    }
    return v;
}

// Infer an arg tag from the runtime Lua type of the value at `sp` (invoke_raw).
arg_value infer(lua_State* L, int sp) {
    arg_value v{};
    switch (lua_type(L, sp)) {
        case LUA_TNUMBER:
            if (lua_isinteger(L, sp)) {
                v.tag = ARG_INT;
                v.i = (int64_t)lua_tointeger(L, sp);
            } else {
                v.tag = ARG_FLOAT;
                v.f = (float)lua_tonumber(L, sp);
            }
            break;
        case LUA_TBOOLEAN:
            v.tag = ARG_BOOL;
            v.i = lua_toboolean(L, sp) ? 1 : 0;
            break;
        case LUA_TSTRING:
            v.tag = ARG_STRING;
            v.s = lua_tostring(L, sp);
            break;
        case LUA_TTABLE:
            v.tag = ARG_VEC3;
            for (int j = 0; j < 3; ++j) {
                lua_rawgeti(L, sp, j + 1);
                v.v3[j] = (float)lua_tonumber(L, -1);
                lua_pop(L, 1);
            }
            break;
        default:
            v.tag = ARG_INT;
            v.i = 0;
            break;
    }
    return v;
}

// Push the native result (per ret tag) onto the Lua stack; returns value count.
int push_result(lua_State* L, arg_tag ret, const uint64_t* result) {
    switch (ret) {
        case ARG_VOID:
            return 0;
        case ARG_INT:
            // RAGE natives write a 32-bit return; the slot's upper 32 bits keep
            // garbage from whichever argument occupied it. Read 32-bit signed,
            // matching the C++ invoker's *(int*) semantics.
            lua_pushinteger(L, (lua_Integer)(int32_t)result[0]);
            return 1;
        case ARG_PTR:
            lua_pushinteger(L, (lua_Integer)result[0]);
            return 1;
        case ARG_BOOL:
            lua_pushboolean(L, (int)(result[0] & 1));
            return 1;
        case ARG_FLOAT: {
            arg_value v = read_result(ARG_FLOAT, result);
            lua_pushnumber(L, v.f);
            return 1;
        }
        case ARG_VEC3: {
            arg_value v = read_result(ARG_VEC3, result);
            lua_createtable(L, 3, 0);
            for (int j = 0; j < 3; ++j) {
                lua_pushnumber(L, v.v3[j]);
                lua_rawseti(L, -2, j + 1);
            }
            return 1;
        }
        case ARG_STRING: {
            const char* s = (const char*)result[0];
            if (s) lua_pushstring(L, s);
            else lua_pushnil(L);
            return 1;
        }
        default:
            lua_pushinteger(L, (lua_Integer)result[0]);
            return 1;
    }
}

int dispatch(lua_State* L, uint64_t rva, const arg_value* av, int nargs, arg_tag ret) {
    if (!g_call_native)
        return luaL_error(L, "native call seam not installed");
    uint64_t slots[kMaxArgs * 3] = {0};
    int nslots = marshal_args(av, nargs, slots, kMaxArgs * 3);
    uint64_t result[3] = {0};
    g_call_native(rva, slots, nslots, result);
    return push_result(L, ret, result);
}

// Same, by hash. A hash that the registry cannot resolve is an error rather
// than a zero result: "the registry is not up yet" and "this native returned 0"
// are very different things, and silently conflating them is the kind of bug
// that costs an afternoon on console.
int dispatch_hash(lua_State* L, uint64_t hash, const arg_value* av, int nargs, arg_tag ret) {
    if (!g_call_native_hash)
        return luaL_error(L, "hash native call seam not installed");
    uint64_t slots[kMaxArgs * 3] = {0};
    int nslots = marshal_args(av, nargs, slots, kMaxArgs * 3);
    uint64_t result[3] = {0};
    if (!g_call_native_hash(hash, slots, nslots, result)) {
        // luaL_error goes through lua_pushfstring, which understands only
        // %s/%d/%c/%f/%p/%I/%U - a %llX there is a runtime error about the
        // format, not the message you wanted. Render the hash first.
        char id[24];
        snprintf(id, sizeof(id), "0x%016llX", (unsigned long long)hash);
        if (g_natives_ready && !g_natives_ready())
            return luaL_error(L, "native %s: the hash registry is not ready yet", id);
        return luaL_error(L, "native %s is not registered in this build", id);
    }
    return push_result(L, ret, result);
}

// invoke(rva, {ret_tag, arg_tag...}, arg1, arg2, ...)
int l_invoke(lua_State* L) {
    uint64_t rva = (uint64_t)luaL_checkinteger(L, 1);
    luaL_checktype(L, 2, LUA_TTABLE);
    int ntypes = (int)lua_rawlen(L, 2);
    if (ntypes < 1)
        return luaL_error(L, "invoke: types table needs at least a return tag");

    lua_rawgeti(L, 2, 1);
    arg_tag ret = (arg_tag)lua_tointeger(L, -1);
    lua_pop(L, 1);

    int nargs = ntypes - 1;
    if (nargs > kMaxArgs)
        return luaL_error(L, "invoke: too many args (%d)", nargs);

    arg_value av[kMaxArgs];
    for (int i = 0; i < nargs; ++i) {
        lua_rawgeti(L, 2, i + 2);
        arg_tag tag = (arg_tag)lua_tointeger(L, -1);
        lua_pop(L, 1);
        av[i] = coerce(L, 3 + i, tag);
    }
    return dispatch(L, rva, av, nargs, ret);
}

// invoke_hash(hash, {ret_tag, arg_tag...}, arg1, ...) — invoke's twin.
int l_invoke_hash(lua_State* L) {
    uint64_t hash = (uint64_t)luaL_checkinteger(L, 1);
    luaL_checktype(L, 2, LUA_TTABLE);
    int ntypes = (int)lua_rawlen(L, 2);
    if (ntypes < 1)
        return luaL_error(L, "invoke_hash: types table needs at least a return tag");

    lua_rawgeti(L, 2, 1);
    arg_tag ret = (arg_tag)lua_tointeger(L, -1);
    lua_pop(L, 1);

    int nargs = ntypes - 1;
    if (nargs > kMaxArgs)
        return luaL_error(L, "invoke_hash: too many args (%d)", nargs);

    arg_value av[kMaxArgs];
    for (int i = 0; i < nargs; ++i) {
        lua_rawgeti(L, 2, i + 2);
        arg_tag tag = (arg_tag)lua_tointeger(L, -1);
        lua_pop(L, 1);
        av[i] = coerce(L, 3 + i, tag);
    }
    return dispatch_hash(L, hash, av, nargs, ret);
}

// invoke_raw(rva, arg1, arg2, ...) -> inferred arg types, integer result
int l_invoke_raw(lua_State* L) {
    uint64_t rva = (uint64_t)luaL_checkinteger(L, 1);
    int nargs = lua_gettop(L) - 1;
    if (nargs < 0) nargs = 0;
    if (nargs > kMaxArgs)
        return luaL_error(L, "invoke_raw: too many args (%d)", nargs);

    arg_value av[kMaxArgs];
    for (int i = 0; i < nargs; ++i)
        av[i] = infer(L, 2 + i);
    return dispatch(L, rva, av, nargs, ARG_INT);
}

// invoke_hash_raw(hash, arg1, ...) -> inferred arg types, integer result
int l_invoke_hash_raw(lua_State* L) {
    uint64_t hash = (uint64_t)luaL_checkinteger(L, 1);
    int nargs = lua_gettop(L) - 1;
    if (nargs < 0) nargs = 0;
    if (nargs > kMaxArgs)
        return luaL_error(L, "invoke_hash_raw: too many args (%d)", nargs);

    arg_value av[kMaxArgs];
    for (int i = 0; i < nargs; ++i)
        av[i] = infer(L, 2 + i);
    return dispatch_hash(L, hash, av, nargs, ARG_INT);
}

// --- bound natives -------------------------------------------------------
//
// scripts/natives.lua binds ~6,400 names, so the per-call cost of the binding
// form is what decides whether a script can call natives in a tick loop. A
// `function NAME(a, b) return invoke(rva, {tags}, a, b) end` wrapper builds a
// fresh types table on every single call; binding instead hands Lua a C closure
// whose upvalue already holds the parsed signature, so a call allocates nothing
// and crosses into C exactly once.
//
// The signature is one character per slot, return type first:
//   v void   i int   f float   b bool   s string   p pointer   3 vector3
// so GET_ENTITY_COORDS(entity, alive) is "3ib".

constexpr uint8_t KIND_RVA  = 0;
constexpr uint8_t KIND_HASH = 1;

struct binding {
    uint64_t target;            // RVA, or the 64-bit hash when kind is KIND_HASH
    uint8_t  kind;
    uint8_t  ret;               // arg_tag
    uint8_t  nargs;
    uint8_t  tags[kMaxArgs];    // arg_tag per argument
};

bool tag_from_char(char c, arg_tag* out) {
    switch (c) {
        case 'v': *out = ARG_VOID;   return true;
        case 'i': *out = ARG_INT;    return true;
        case 'f': *out = ARG_FLOAT;  return true;
        case 'b': *out = ARG_BOOL;   return true;
        case 's': *out = ARG_STRING; return true;
        case 'p': *out = ARG_PTR;    return true;
        case '3': *out = ARG_VEC3;   return true;
        default:                     return false;
    }
}

int l_native_call(lua_State* L) {
    const binding* b = (const binding*)lua_touserdata(L, lua_upvalueindex(1));
    arg_value av[kMaxArgs];
    for (int i = 0; i < b->nargs; ++i)
        av[i] = coerce(L, i + 1, (arg_tag)b->tags[i]);
    if (b->kind == KIND_RVA)
        return dispatch(L, b->target, av, b->nargs, (arg_tag)b->ret);
    return dispatch_hash(L, b->target, av, b->nargs, (arg_tag)b->ret);
}

int bind(lua_State* L, uint8_t kind) {
    uint64_t target = (uint64_t)luaL_checkinteger(L, 1);
    size_t len = 0;
    const char* sig = luaL_checklstring(L, 2, &len);
    if (len < 1)
        return luaL_error(L, "native: signature needs at least a return tag");
    if (len - 1 > (size_t)kMaxArgs)
        return luaL_error(L, "native: too many args (%d)", (int)(len - 1));

    binding* b = (binding*)lua_newuserdatauv(L, sizeof(binding), 0);
    memset(b, 0, sizeof(*b));
    b->target = target;
    b->kind = kind;
    b->nargs = (uint8_t)(len - 1);

    arg_tag t;
    if (!tag_from_char(sig[0], &t))
        return luaL_error(L, "native: bad return tag '%c' in \"%s\"", sig[0], sig);
    b->ret = (uint8_t)t;
    for (size_t i = 1; i < len; ++i) {
        if (!tag_from_char(sig[i], &t))
            return luaL_error(L, "native: bad arg tag '%c' in \"%s\"", sig[i], sig);
        b->tags[i - 1] = (uint8_t)t;
    }

    lua_pushcclosure(L, l_native_call, 1);
    return 1;
}

// native(rva, sig) -> callable
int l_native(lua_State* L) { return bind(L, KIND_RVA); }

// native_hash(hash, sig) -> callable
int l_native_hash(lua_State* L) { return bind(L, KIND_HASH); }

// natives_ready() -> true once the hash registry has been walked and verified.
// RVA-bound natives work regardless; this only gates the hash-bound half.
int l_natives_ready(lua_State* L) {
    lua_pushboolean(L, g_natives_ready ? (g_natives_ready() ? 1 : 0) : 0);
    return 1;
}

// native_addr(hash) -> the address the game's own registry resolves it to, or
// nil. That registry IS the game's dispatch table, so its answer is the
// authority: comparing it against rva(off) for the same native is what turns
// "are the 2,813 RVAs correct?" from a hand check per native into one sweep.
int l_native_addr(lua_State* L) {
    uint64_t hash = (uint64_t)luaL_checkinteger(L, 1);
    uint64_t addr = g_native_addr ? g_native_addr(hash) : 0;
    if (!addr) { lua_pushnil(L); return 1; }
    lua_pushinteger(L, (lua_Integer)addr);
    return 1;
}

// native_exists(hash) -> is this native registered in this build?
int l_native_exists(lua_State* L) {
    uint64_t hash = (uint64_t)luaL_checkinteger(L, 1);
    lua_pushboolean(L, g_native_exists ? (g_native_exists(hash) ? 1 : 0) : 0);
    return 1;
}

uint64_t global_addr(uint32_t idx) {
    uint64_t table = g_eboot_base_abs + script_globals_rva();
    return script_global_addr(table, idx);
}

// The block table has not been located in the RDR2 v1.32 eboot yet (see
// script_mem.h). Raising here is the whole point: with the RVA at 0 the address
// arithmetic would dereference near-null garbage on a read, and on a write it
// would scribble into whatever the game keeps at a low address. An error naming
// the reason costs a script one line; a silent wrong global costs an afternoon.
int l_script_global(lua_State* L) {
    uint32_t idx = (uint32_t)luaL_checkinteger(L, 1);
    if (!script_globals_available())
        return luaL_error(L, "script_global(%d): the script-global block table "
                             "has not been located for this game build yet", (int)idx);
    lua_pushinteger(L, (lua_Integer)*(int64_t*)global_addr(idx));
    return 1;
}

int l_set_script_global(lua_State* L) {
    uint32_t idx = (uint32_t)luaL_checkinteger(L, 1);
    int64_t val = (int64_t)luaL_checkinteger(L, 2);
    if (!script_globals_available())
        return luaL_error(L, "set_script_global(%d): the script-global block table "
                             "has not been located for this game build yet", (int)idx);
    *(int64_t*)global_addr(idx) = val;
    return 0;
}

int l_read_mem(lua_State* L) {
    uint64_t addr = (uint64_t)luaL_checkinteger(L, 1);
    int size = (int)luaL_optinteger(L, 2, 8);
    uint64_t out = 0;
    switch (size) {
        case 1: out = *(uint8_t*)addr;  break;
        case 2: out = *(uint16_t*)addr; break;
        case 4: out = *(uint32_t*)addr; break;
        default: out = *(uint64_t*)addr; break;
    }
    lua_pushinteger(L, (lua_Integer)out);
    return 1;
}

int l_write_mem(lua_State* L) {
    uint64_t addr = (uint64_t)luaL_checkinteger(L, 1);
    uint64_t val = (uint64_t)luaL_checkinteger(L, 2);
    int size = (int)luaL_optinteger(L, 3, 8);
    switch (size) {
        case 1: *(uint8_t*)addr  = (uint8_t)val;  break;
        case 2: *(uint16_t*)addr = (uint16_t)val; break;
        case 4: *(uint32_t*)addr = (uint32_t)val; break;
        default: *(uint64_t*)addr = val; break;
    }
    return 0;
}

int l_read_float(lua_State* L) {
    uint64_t addr = (uint64_t)luaL_checkinteger(L, 1);
    lua_pushnumber(L, *(float*)addr);
    return 1;
}

int l_write_float(lua_State* L) {
    uint64_t addr = (uint64_t)luaL_checkinteger(L, 1);
    *(float*)addr = (float)luaL_checknumber(L, 2);
    return 0;
}

// read_vector3(p) -> {x, y, z}
//
// A Vector3 out-param's buffer is 24 bytes with the floats at +0, +8 and +16,
// not a packed 12 (see rage::invoker::script_vec3). Leaving every caller to get
// that stride right by hand is a trap that has already been sprung once: the
// shipped example read +8 and called it z, when +8 is y. So the layout is
// spelled out once, here. Returns the same shape a Vector3 RETURN arrives in,
// so a script need not care which of the two it is looking at.
int l_read_vector3(lua_State* L) {
    uint64_t addr = (uint64_t)luaL_checkinteger(L, 1);
    if (!addr) { lua_pushnil(L); return 1; }
    lua_createtable(L, 3, 0);
    lua_pushnumber(L, *(float*)(addr +  0)); lua_rawseti(L, -2, 1);
    lua_pushnumber(L, *(float*)(addr +  8)); lua_rawseti(L, -2, 2);
    lua_pushnumber(L, *(float*)(addr + 16)); lua_rawseti(L, -2, 3);
    return 1;
}

int l_read_string(lua_State* L) {
    uint64_t addr = (uint64_t)luaL_checkinteger(L, 1);
    if (!addr) { lua_pushnil(L); return 1; }
    lua_pushstring(L, (const char*)addr);
    return 1;
}

// Scratch memory for the out-parameters that ~700 natives take. Zeroed, because
// a native handed an uninitialised in/out slot behaves unpredictably, and capped
// so a typo'd size fails here rather than in the allocator.
constexpr lua_Integer kMaxAlloc = 64 * 1024;

int l_mem_alloc(lua_State* L) {
    lua_Integer size = luaL_checkinteger(L, 1);
    if (size <= 0 || size > kMaxAlloc)
        return luaL_error(L, "mem_alloc: size must be 1..%d (got %I)", (int)kMaxAlloc, size);
    void* p = malloc((size_t)size);
    if (!p) { lua_pushnil(L); return 1; }
    memset(p, 0, (size_t)size);
    lua_pushinteger(L, (lua_Integer)(uint64_t)p);
    return 1;
}

int l_mem_free(lua_State* L) {
    uint64_t p = (uint64_t)luaL_checkinteger(L, 1);
    if (p) free((void*)p);
    return 0;
}

int l_rva(lua_State* L) {
    uint64_t off = (uint64_t)luaL_checkinteger(L, 1);
    lua_pushinteger(L, (lua_Integer)(g_eboot_base_abs + off));
    return 1;
}

void set_int(lua_State* L, const char* name, lua_Integer v) {
    lua_pushinteger(L, v);
    lua_setglobal(L, name);
}

void set_fn(lua_State* L, const char* name, lua_CFunction f) {
    lua_pushcfunction(L, f);
    lua_setglobal(L, name);
}

}  // namespace

bool bridge_open(lua_State* L) {
    if (!L) return false;

    set_fn(L, "native", l_native);
    set_fn(L, "native_hash", l_native_hash);
    set_fn(L, "natives_ready", l_natives_ready);
    set_fn(L, "native_exists", l_native_exists);
    set_fn(L, "native_addr", l_native_addr);
    set_fn(L, "invoke", l_invoke);
    set_fn(L, "invoke_raw", l_invoke_raw);
    set_fn(L, "invoke_hash", l_invoke_hash);
    set_fn(L, "invoke_hash_raw", l_invoke_hash_raw);
    set_fn(L, "script_global", l_script_global);
    set_fn(L, "set_script_global", l_set_script_global);
    set_fn(L, "read_mem", l_read_mem);
    set_fn(L, "write_mem", l_write_mem);
    set_fn(L, "read_float", l_read_float);
    set_fn(L, "write_float", l_write_float);
    set_fn(L, "read_string", l_read_string);
    set_fn(L, "read_vector3", l_read_vector3);
    set_fn(L, "mem_alloc", l_mem_alloc);
    set_fn(L, "mem_free", l_mem_free);
    set_fn(L, "rva", l_rva);

    set_int(L, "ARG_VOID", ARG_VOID);
    set_int(L, "ARG_INT", ARG_INT);
    set_int(L, "ARG_FLOAT", ARG_FLOAT);
    set_int(L, "ARG_BOOL", ARG_BOOL);
    set_int(L, "ARG_VEC3", ARG_VEC3);
    set_int(L, "ARG_STRING", ARG_STRING);
    set_int(L, "ARG_PTR", ARG_PTR);
    return true;
}

}  // namespace script
