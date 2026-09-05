#pragma once
#include <stdint.h>

// Which game are we inside, and everything that differs because of it.
//
// Luma is one .prx listed under several title ids in GoldHEN's plugins.ini, so
// the first thing it must do is work out which game actually loaded it. Getting
// that wrong means the wrong address table, which means every native call jumps
// somewhere arbitrary -- so this does not guess. It matches a byte signature
// and, if nothing matches, refuses to run at all.
//
// THE SIGNATURE IS THE FRAME HOOK'S OWN PROLOGUE, which is doing double duty on
// purpose. Those bytes already had to be verified for a second reason: GoldHEN's
// Detour overwrites 14 bytes and copies the displaced prologue into an mmap'd
// stub with memcpy, relocating nothing, so a rip-relative instruction inside
// them is silently broken in the copy. Reusing the same bytes as the game
// fingerprint means one check covers both questions, and there is no second
// constant to keep in step with the first.
//
// The two signatures were checked against BOTH eboots, in all four
// combinations -- each matches its own game and neither matches the other at
// the other's address. That is what makes detection unambiguous rather than
// merely likely; see tools/check_detour_target.py, which re-derives it.
//
// Paths are stored whole rather than built from data_root. There is no STL
// here (the plugin links SceLibcInternal without libc++), so a string that is
// already correct beats one assembled at runtime out of fixed buffers.

namespace game {

	struct profile {
		const char* id;              // "gta5", "rdr2" -- for logs
		const char* title_id;        // CUSA00411, CUSA03041
		const char* name;            // human readable, for the boot notification

		// The console layout. Deliberately per-game: one console can host both
		// plugins' data at once and a script must never land in the other
		// game's scripts/ directory.
		const char* data_root;       // "/data/gtalua/"   (trailing slash)
		const char* log_path;        // "/data/gtalua/gtalua.log"
		const char* script_dir;      // "/data/gtalua/scripts"
		const char* prelude_path;    // "/data/gtalua/natives.lua"
		const char* resource_root;   // "/data/gtalua/resources"
		const char* autostart_path;  // "/data/gtalua/autostart.cfg"
		const char* token_path;      // "/data/gtalua/token"

		int port;                    // control channel: 9615 gta5, 9616 rdr2

		// The per-frame hook, and the fingerprint that identifies the build.
		uint64_t       hook_rva;
		const uint8_t* hook_prologue;
		unsigned       hook_prologue_len;

		// GET_FRAME_COUNT. The frame hook fires many times per frame, so it
		// deduplicates on this; it is the only native the C++ side calls at
		// all, everything else reaches natives through Lua and natives.lua.
		uint64_t frame_count_rva;

		// Native-by-hash resolution, which the two games do completely
		// differently: GTA V walks a masked table in the live process, RDR2
		// binary-searches a table compiled in from its eboot. Behind these
		// pointers the difference stops mattering to every caller.
		bool     (*natives_build)();
		void*    (*natives_find)(uint64_t hash);
		uint32_t (*natives_count)();
		bool     (*natives_usable)();

		// RAGE script-global block table. 0 means it has not been located for
		// this build, and script_global() refuses rather than reading and
		// WRITING an address derived from nothing.
		uint64_t script_globals_rva;
	};

	// Identify the game by reading the signature at each profile's hook_rva.
	// Requires rage::invoker::g_eboot_base. Returns nullptr when no known build
	// matches -- the caller must treat that as fatal, not as a default.
	const profile* detect();

	// The detected profile, or nullptr before detect() has succeeded. Every
	// consumer goes through this; nothing else knows a path or a port.
	const profile* current();

}  // namespace game
