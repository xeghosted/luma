#include "rage/invoker/hash_natives.h"
#include "game/profile.h"

// Native-by-hash, routed to whichever backend the detected game uses.
//
// The two are not variations on a theme: GTA V walks a XOR-masked table in the
// live process because most of its natives have no address obtainable
// otherwise, while RDR2's are all known from its eboot before boot. Callers
// -- the loader, the control channel, the Lua bridge -- care about neither.

namespace rage::hash_natives {

    bool build() {
        const game::profile* p = game::current();
        return p && p->natives_build ? p->natives_build() : false;
    }

    void* find(uint64_t hash) {
        const game::profile* p = game::current();
        return p && p->natives_find ? p->natives_find(hash) : nullptr;
    }

    uint32_t entry_count() {
        const game::profile* p = game::current();
        return p && p->natives_count ? p->natives_count() : 0;
    }

    bool usable() {
        const game::profile* p = game::current();
        return p && p->natives_usable ? p->natives_usable() : false;
    }
}
