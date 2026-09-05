#include "game/profile.h"

// A fixed profile for the host tests.
//
// The real one (src/game/profile.cpp) identifies the game by reading a byte
// signature out of the loaded eboot, which needs a PS4 and an ORBIS toolchain.
// The host suite exists precisely to exercise the parts that do NOT need
// either, so it gets a profile that simply exists: path_guard needs a sandbox
// root and native_bridge needs to know whether script globals are available.
//
// script_globals_rva is 0 on purpose. That is RDR2's real state, and it is the
// case worth having in a test: script_global() must refuse rather than
// dereference an address derived from nothing.
namespace game {

    static const profile TEST_PROFILE = {
        "test", "CUSA00000", "Host test",
        "/data/rdr2lua/",
        "/data/rdr2lua/rdr2lua.log",
        "/data/rdr2lua/scripts",
        "/data/rdr2lua/natives.lua",
        "/data/rdr2lua/resources",
        "/data/rdr2lua/autostart.cfg",
        "/data/rdr2lua/token",
        9616,
        0, nullptr, 0,
        0,
        nullptr, nullptr, nullptr, nullptr,
        0,
    };

    const profile* current() { return &TEST_PROFILE; }

}  // namespace game
