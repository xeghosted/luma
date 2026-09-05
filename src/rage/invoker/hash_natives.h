#pragma once
#include <stdint.h>
#include "rage/invoker/invoker.h"

// Call natives by their 64-bit hash.
//
// GTALua had to recover this mapping from the running game: more than half of
// GTA V's natives had no known RVA, so their addresses existed only inside the
// process, in a table of XOR-masked nodes that had to be walked and then
// verified before anything could trust it. That walk could fail, so the Lua API
// grew a `natives_ready()` for scripts to guard on.
//
// None of that applies here. RDR2 registers its natives inline and unmasked, so
// tools/extract_registry.py recovers every hash and address from the eboot
// before the plugin is even built, and natives_registry.h is that result
// compiled in. What is left is a binary search.
//
// The interface is unchanged from GTALua's so the loader and the control
// channel need no special cases, but the semantics are simpler: build() cannot
// fail for want of the game's cooperation, and usable() is true as soon as the
// image base is known.
namespace rage::hash_natives {

    // Validates the compiled-in table against the resolved image base. Cheap
    // and idempotent; safe to call before any script runs.
    bool build();

    // How many hash -> handler pairs are compiled in.
    uint32_t entry_count();

    // True once build() has succeeded.
    bool usable();

    // Absolute address of the handler for this hash, or nullptr when the hash
    // is not registered in this build.
    void* find(uint64_t hash);

    // Call a native by hash. Same shape as rage::invoker::invoke. Returns a
    // default-constructed R when the hash is not registered -- check find(hash)
    // when the difference matters.
    template<typename R, typename... Args>
    R invoke_hash(uint64_t hash, Args&&... args) {
        void* fn = find(hash);
        if (!fn)
            return R();
        rage::invoker::native_setup ctx;
        rage::invoker::pass{ ([&]() { ctx.push(args); }(), 1)... };
        ((rage::invoker::native_handler)fn)(&ctx);
        // The Vector3 write-back every native-calling path needs; see the
        // header comment in invoker.h.
        ctx.set_vector_results();
        return ctx.get_return<R>();
    }
}
