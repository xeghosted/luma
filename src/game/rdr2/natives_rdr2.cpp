#include "game/rdr2/natives_rdr2.h"
#include "game/rdr2/natives_registry.h"
#include "rage/invoker/invoker.h"
#include "platform/log.h"

// RDR2's half of native resolution.
//
// Nothing is walked and nothing is decoded: RDR2 emits every registration into
// the eboot in the clear, so tools/extract_registry.py recovers all 7,072
// hash/handler pairs before the plugin is even built and natives_registry.h is
// that result. What is left is a binary search.
//
// Contrast game/gta5/natives_gta5.cpp, which has to walk a masked table in the
// live process because more than half of GTA V's natives have no address
// obtainable any other way.

namespace game::rdr2 {


    using rage::natives_registry::k_entries;
    using rage::natives_registry::k_count;

    static bool g_usable = false;

    uint32_t count() { return k_count; }
    bool usable() { return g_usable; }

    void* find_hash(uint64_t hash) {
        uintptr_t base = rage::invoker::g_eboot_base;
        if (!base)
            return nullptr;

        // k_entries is emitted sorted by hash, which is what makes this a
        // binary search rather than the open-addressed table GTALua needed.
        uint32_t lo = 0, hi = k_count;
        while (lo < hi) {
            uint32_t mid = lo + (hi - lo) / 2;
            uint64_t probe = k_entries[mid].hash;
            if (probe == hash)
                return (void*)(base + k_entries[mid].rva);
            if (probe < hash)
                lo = mid + 1;
            else
                hi = mid;
        }
        return nullptr;
    }

    bool build() {
        if (g_usable)
            return true;
        if (!rage::invoker::g_eboot_base) {
            platform::logf("hash_natives", "eboot base not resolved yet");
            return false;
        }

        // The table itself is a build artefact and cannot be wrong at runtime,
        // but the base + RVA arithmetic can be, and a wrong base produces
        // pointers that look perfectly plausible right up until they are called.
        // So prove it: resolve GET_HASH_KEY by hash and make it compute a value
        // we already know.
        //
        // 0x3DBB3D48 is joaat("rdr2lua"). The joaat implementation that produced
        // it was itself checked against this console: GET_HASH_KEY("gtalua")
        // returned 0x72B71C36 there, and the same code computes 0x72B71C36 for
        // that string -- so the algorithm matches this build's, and the constant
        // below is not merely asserted.
        static constexpr uint64_t HASH_GET_HASH_KEY = 0xFD340785ADF8CFB7ULL;
        static constexpr uint32_t EXPECTED = 0x3DBB3D48u;

        void* fn = find_hash(HASH_GET_HASH_KEY);
        if (!fn) {
            platform::logf("hash_natives", "GET_HASH_KEY missing from the compiled table");
            return false;
        }

        rage::invoker::native_setup ctx;
        ctx.push("rdr2lua");
        ((rage::invoker::native_handler)fn)(&ctx);
        uint32_t got = ctx.get_return<uint32_t>();

        g_usable = (got == EXPECTED);
        platform::logf("hash_natives", "%u natives compiled in; GET_HASH_KEY -> 0x%08X (%s)",
                       (unsigned)k_count, (unsigned)got,
                       g_usable ? "OK" : "MISMATCH - base or table is wrong");
        return g_usable;
    }

}  // namespace game::rdr2
