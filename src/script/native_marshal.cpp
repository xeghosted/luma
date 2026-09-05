#include "script/native_marshal.h"

namespace script {

int marshal_args(const arg_value* a, int n, uint64_t* slots, int max) {
    int s = 0;
    for (int k = 0; k < n; ++k) {
        if (s >= max) break;
        switch (a[k].tag) {
            case ARG_INT:
                slots[s++] = (uint64_t)a[k].i;
                break;
            case ARG_PTR:
                slots[s++] = a[k].p;
                break;
            case ARG_BOOL:
                slots[s++] = a[k].i ? 1u : 0u;
                break;
            case ARG_FLOAT: {
                uint32_t b;
                __builtin_memcpy(&b, &a[k].f, 4);
                slots[s++] = b;
                break;
            }
            case ARG_STRING:
                slots[s++] = (uint64_t)a[k].s;
                break;
            case ARG_VEC3:
                for (int j = 0; j < 3 && s < max; ++j) {
                    uint32_t b;
                    __builtin_memcpy(&b, &a[k].v3[j], 4);
                    slots[s++] = b;
                }
                break;
            default:
                slots[s++] = 0;
                break;
        }
    }
    return s;
}

arg_value read_result(arg_tag ret, const uint64_t* r) {
    arg_value v{};
    v.tag = ret;
    switch (ret) {
        case ARG_INT:
        case ARG_PTR:
            v.i = (int64_t)r[0];
            v.p = r[0];
            break;
        case ARG_BOOL:
            v.i = r[0] & 1;
            break;
        case ARG_FLOAT: {
            uint32_t b = (uint32_t)r[0];
            __builtin_memcpy(&v.f, &b, 4);
            break;
        }
        case ARG_VEC3:
            for (int j = 0; j < 3; ++j) {
                uint32_t b = (uint32_t)r[j];
                __builtin_memcpy(&v.v3[j], &b, 4);
            }
            break;
        default:
            break;
    }
    return v;
}

}  // namespace script
