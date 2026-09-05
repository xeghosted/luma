// C++ runtime pieces the GHPLUGIN lacks (no libc++). The menu and all static
// local singletons are constructed by menu::build() on the module-start
// thread, before the frame callback is registered on the script thread, so
// no concurrent access races the guard variables during construction.
#include <stddef.h>
#include <stdlib.h>

void* operator new(size_t n) { return malloc(n); }
void* operator new[](size_t n) { return malloc(n); }
void  operator delete(void* p) noexcept { free(p); }
void  operator delete[](void* p) noexcept { free(p); }
void  operator delete(void* p, size_t) noexcept { free(p); }
void  operator delete[](void* p, size_t) noexcept { free(p); }

extern "C" {
    // Itanium ABI guard variable is 8 bytes; byte 0 = "initialized".
    int __cxa_guard_acquire(long long* g) {
        return *((char*)g) == 0;   // acquire if not yet initialized
    }
    void __cxa_guard_release(long long* g) {
        *((char*)g) = 1;
    }
    void __cxa_guard_abort(long long*) {}
    void __cxa_pure_virtual() { for (;;) {} }   // never call a pure virtual

    // Vendored Lua's lauxlib file helpers (luaL_fileresult/loadfilex) reference
    // the glibc-style errno accessor, which SceLibcInternal does not export.
    // Those helpers are never called (scripts are loaded via buffers), so a
    // trivial single-threaded errno cell satisfies the linker.
    int* __errno_location() {
        static int e = 0;
        return &e;
    }
}
