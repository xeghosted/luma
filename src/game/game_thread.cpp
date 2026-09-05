#include "game_thread.h"
#include "../rage/invoker/invoker.h"
#include "platform/log.h"
#include "game/profile.h"

#include <GoldHEN/Detour.h>
#include <stdint.h>

// No STL: the plugin links SceLibcInternal without libc++. Synchronisation uses
// clang's __atomic builtins (word-sized, lock-free on x86-64, no runtime lib).

namespace game {

	// GET_PLAYER_PED impl (hash 0x275F255ED201B937, address from this build's
	// own registry -- see tools/native_registry.json). The base game's scripts
	// call it on the script thread, so detouring it gives a per-frame,
	// correct-thread callback without hooking the engine itself.
	//
	// **The target must be detourable, and most natives are not.** GoldHEN's
	// Detour overwrites 14 bytes and copies the displaced prologue into an
	// mmap'd stub with memcpy, relocating nothing -- HDE64 gives it instruction
	// lengths and nothing more. Any position-dependent instruction inside those
	// 14 bytes is therefore broken in the copy.
	//
	// This was PLAYER_PED_ID (0x21BDAC0), RDR2's idiomatic equivalent, and it
	// crashed the game. Its prologue is
	//
	//     push rbp / mov rbp,rsp / push rbx / push rax   ; 6 bytes
	//     mov  rax, [rip + 0x62dcbbb]                    ; 7 bytes, ends at 13
	//     mov  rdi, [rax + 8]                            ; dereferences it
	//
	// so the stub loaded a pointer from the wrong address and faulted on the
	// first return through the trampoline. Everything before that worked, which
	// is why the log showed a clean boot and then nothing.
	//
	// 47% of this build's natives share that shape, so a target chosen by name
	// is a coin flip. tools/check_detour_target.py checks the value below
	// against the eboot and tests/run.sh fails if it is not relocatable.
	// Address and signature both come from the detected profile now; see
	// src/game/profile.cpp for the two builds and why the prologue doubles
	// as the game fingerprint.

	// The 14 bytes the detour will displace, as they must appear in the eboot.
	// Verified offline; asserted here so a different game build refuses the hook
	// instead of patching over instructions nobody checked.


	typedef int64_t (*native_fn)(rage::invoker::native_context*);
	typedef void (*task_fn)();

	static const int QUEUE_CAP = 64;

	static Detour g_detour;
	static bool g_installed = false;

	static task_fn g_queue[QUEUE_CAP];
	static int g_queue_count = 0;
	static volatile int g_lock = 0;      // spinlock guarding the queue
	static volatile int g_draining = 0;  // re-entrancy guard for the detour

	// Cached at install time: the hook runs many times per frame and has no
	// business looking the profile up on each one.
	static uint64_t g_frame_count_rva = 0;

	static frame_fn g_frame_cb = nullptr;
	static int g_last_frame = -1;

	static inline void lock() {
		while (__atomic_exchange_n(&g_lock, 1, __ATOMIC_ACQUIRE)) { }
	}
	static inline void unlock() {
		__atomic_store_n(&g_lock, 0, __ATOMIC_RELEASE);
	}

	static void drain_queue() {
		// One drainer at a time: queued work may call the hooked native and
		// re-enter this detour on the same thread.
		if (__atomic_exchange_n(&g_draining, 1, __ATOMIC_ACQ_REL))
			return;

		task_fn local[QUEUE_CAP];
		int n;
		lock();
		n = g_queue_count;
		for (int i = 0; i < n; ++i)
			local[i] = g_queue[i];
		g_queue_count = 0;
		unlock();

		for (int i = 0; i < n; ++i)
			local[i]();

		__atomic_store_n(&g_draining, 0, __ATOMIC_RELEASE);
	}

	static int64_t player_ped_hook(rage::invoker::native_context* ctx) {
		drain_queue();

		// g_last_frame is updated BEFORE g_frame_cb() runs, so if the callback
		// itself calls the hooked native (get_player_ped) the resulting
		// re-entrant call to this detour sees frame == g_last_frame and skips
		// calling g_frame_cb() again - one bounded extra recursion, not a loop.
		if (g_frame_cb) {
			int frame = rage::invoker::invoke<int>(g_frame_count_rva);
			if (frame != g_last_frame) {
				g_last_frame = frame;
				g_frame_cb();
			}
		}

		return Detour_Stub(&g_detour, native_fn, ctx);
	}

	void run_on_game_thread(task_fn fn) {
		lock();
		if (g_queue_count < QUEUE_CAP)
			g_queue[g_queue_count++] = fn;
		unlock();
	}

	void set_frame_callback(frame_fn fn) { g_frame_cb = fn; }

	bool install_frame_hook() {
		if (g_installed)
			return true;
		if (!rage::invoker::g_eboot_base)
			return false;

		// Refuse to patch bytes we have not checked. The detour cannot relocate
		// a position-dependent instruction (see HOOK_RVA above), so the whole
		// safety argument rests on these exact 14 bytes being what they were
		// when tools/check_detour_target.py verified them. On a different game
		// build they are something else, and patching anyway is how you get a
		// crash that looks like a clean boot.
		const game::profile* p = game::current();
		if (!p) {
			platform::logf("game_thread", "frame hook REFUSED: the game was not identified");
			return false;
		}
		const uint8_t* target = (const uint8_t*)(rage::invoker::g_eboot_base + p->hook_rva);
		for (unsigned i = 0; i < p->hook_prologue_len; ++i) {
			if (target[i] != p->hook_prologue[i]) {
				platform::logf("game_thread",
				               "frame hook REFUSED: byte %u at rva 0x%llX is 0x%02X, expected 0x%02X "
				               "- this is not the build the hook was verified against",
				               i, (unsigned long long)p->hook_rva, target[i], p->hook_prologue[i]);
				return false;
			}
		}

		g_frame_count_rva = p->frame_count_rva;

		Detour_Construct(&g_detour, DetourMode_x64);
		void* stub = Detour_DetourFunction(
			&g_detour,
			rage::invoker::g_eboot_base + p->hook_rva,
			(void*)&player_ped_hook);

		g_installed = (stub != nullptr);
		return g_installed;
	}
}
