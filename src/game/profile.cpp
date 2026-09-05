#include "game/profile.h"
#include "rage/invoker/invoker.h"
#include "platform/log.h"
#include "game/gta5/natives_gta5.h"
#include "game/rdr2/natives_rdr2.h"

namespace game {

	// GTA V, CUSA00411 v1.57 -- GET_PLAYER_PED at 0xAA9310.
	//   push rbp / mov rbp,rsp / push rbx / push rax / mov rbx,rdi
	//   mov rax,[rbx+0x10] / mov edi,[rax]
	// Read out of the 1.57 database; 15 bytes of whole instructions cover the
	// 14-byte patch and none of them is position-dependent.
	static const uint8_t GTA5_HOOK[14] = {
		0x55, 0x48, 0x89, 0xE5, 0x53, 0x50, 0x48, 0x89, 0xFB, 0x48, 0x8B, 0x43, 0x10, 0x8B
	};

	// RDR2, CUSA03041 v1.32 -- GET_PLAYER_PED at 0x21B9140.
	//   push rbp / mov rbp,rsp / push r14 / push rbx / mov r14,rdi
	//   mov rax,[r14+0x10]
	// NOT PLAYER_PED_ID (0x21BDAC0), which is RDR2's idiomatic equivalent and
	// carries `mov rax,[rip+0x62dcbbb]` at offset 6. Hooking that one crashed
	// the game after a boot log that looked completely healthy.
	static const uint8_t RDR2_HOOK[14] = {
		0x55, 0x48, 0x89, 0xE5, 0x41, 0x56, 0x53, 0x49, 0x89, 0xFE, 0x49, 0x8B, 0x46, 0x10
	};

	static const profile PROFILES[] = {
		{
			"gta5", "CUSA00411", "Grand Theft Auto V",
			"/data/gtalua/",
			"/data/gtalua/gtalua.log",
			"/data/gtalua/scripts",
			"/data/gtalua/natives.lua",
			"/data/gtalua/resources",
			"/data/gtalua/autostart.cfg",
			"/data/gtalua/token",
			9615,
			0xAA9310, GTA5_HOOK, sizeof(GTA5_HOOK),
			0x9F6430,
			gta5::build, gta5::find_hash, gta5::count, gta5::usable,
			0x3E0CFB8,
		},
		{
			"rdr2", "CUSA03041", "Red Dead Redemption 2",
			"/data/rdr2lua/",
			"/data/rdr2lua/rdr2lua.log",
			"/data/rdr2lua/scripts",
			"/data/rdr2lua/natives.lua",
			"/data/rdr2lua/resources",
			"/data/rdr2lua/autostart.cfg",
			"/data/rdr2lua/token",
			9616,
			0x21B9140, RDR2_HOOK, sizeof(RDR2_HOOK),
			0x1F7F9A0,
			rdr2::build, rdr2::find_hash, rdr2::count, rdr2::usable,
			0,   // the block table has not been located in this build
		},
	};
	static const unsigned PROFILE_COUNT = sizeof(PROFILES) / sizeof(PROFILES[0]);

	static const profile* g_current = nullptr;

	const profile* current() { return g_current; }

	const profile* detect() {
		if (g_current)
			return g_current;

		uintptr_t base = rage::invoker::g_eboot_base;
		if (!base) {
			platform::logf("profile", "cannot identify the game before the eboot base is resolved");
			return nullptr;
		}

		// Every candidate is checked, not just until the first hit. A signature
		// that matched two profiles would mean the fingerprints are not
		// distinguishing, and quietly taking the first would pick a game by
		// array order. Both eboots were compared in all four combinations
		// offline, so this cannot currently happen -- which is exactly why it
		// is worth noticing if it ever does.
		const profile* found = nullptr;
		for (unsigned p = 0; p < PROFILE_COUNT; ++p) {
			const profile& c = PROFILES[p];
			const uint8_t* at = (const uint8_t*)(base + c.hook_rva);
			bool match = true;
			for (unsigned i = 0; i < c.hook_prologue_len; ++i) {
				if (at[i] != c.hook_prologue[i]) { match = false; break; }
			}
			if (!match)
				continue;
			if (found) {
				platform::logf("profile",
				               "AMBIGUOUS: both %s and %s match; refusing to guess",
				               found->id, c.id);
				return nullptr;
			}
			found = &c;
		}

		if (!found) {
			// Say what was actually there. "Unknown game" alone sends whoever
			// hits this looking in the wrong place; the bytes name the build.
			const uint8_t* at = (const uint8_t*)(base + PROFILES[0].hook_rva);
			platform::logf("profile",
			               "no known build matched. At rva 0x%llX this image has "
			               "%02X %02X %02X %02X %02X %02X %02X %02X - Luma supports "
			               "CUSA00411 v1.57 and CUSA03041 v1.32 only",
			               (unsigned long long)PROFILES[0].hook_rva,
			               at[0], at[1], at[2], at[3], at[4], at[5], at[6], at[7]);
			return nullptr;
		}

		g_current = found;
		platform::logf("profile", "%s (%s), data root %s, port %d",
		               found->name, found->title_id, found->data_root, found->port);
		return g_current;
	}

}  // namespace game
