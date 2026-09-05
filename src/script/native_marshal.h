#pragma once
#include <stdint.h>

// Pure arg marshalling for the RAGE scrNativeCallContext ABI. No Lua, no ORBIS,
// no game deps -> host-testable. arg_tag values MUST match the Lua ARG_* globals
// and the generator (tools/gen_natives_lua.py): VOID=0..PTR=6.
namespace script {

enum arg_tag {
    ARG_VOID = 0,
    ARG_INT = 1,
    ARG_FLOAT = 2,
    ARG_BOOL = 3,
    ARG_VEC3 = 4,
    ARG_STRING = 5,
    ARG_PTR = 6,
};

struct arg_value {
    arg_tag tag;
    int64_t i;
    float f;
    uint64_t p;
    const char* s;
    float v3[3];
};

// Write native_context argument slots (8 bytes each) from typed args.
// Vec3 consumes 3 slots. Returns the number of slots written (<= max).
int marshal_args(const arg_value* a, int n, uint64_t* slots, int max);

// Read a result of tag `ret` from the native_context result slots.
arg_value read_result(arg_tag ret, const uint64_t* result);

}  // namespace script
