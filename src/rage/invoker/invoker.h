#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "rage/types/base_types.h"   // math::vector3 for the padded return read

// RDR2 native invoker for the PS4 CUSA03041 v1.32 eboot.
//
// The generated natives.h calls _i<R>(0xRVA, args...), where the literal is the
// native handler's RVA (offset from the eboot's ELF base 0), NOT a hash. There
// is no hash->function crossmap to consult at runtime: tools/extract_registry.py
// recovers every address from the eboot before the build, so we add the runtime
// image base (resolved once at load) and call the handler directly.
//
// The call context layout below IS the RAGE scrNativeCallContext ABI: return
// pointer @0, argument count @8, argument array @16, each argument and the
// return value one 8-byte slot. Do not reorder these fields.
//
// This layout was inherited from GTALua's GTA V invoker, and then checked
// against RDR2 rather than assumed -- the two engines agree byte for byte. The
// evidence is below, and every address cited is from THIS eboot.
//
// Vector3 OUT-params do NOT reach the caller's pointer directly. GTA V routes
// this through a shared helper; RDR2 inlines the same logic into each native.
// From GET_MODEL_DIMENSIONS (0x1F80B80), which is exactly the native whose
// {0,0,0} answer exposed the mechanism on GTA V:
//
//     mov       rax, [rdi + 0x10]              ; the argument array
//     movsxd    rcx, dword ptr [rdi + 0x18]    ; m_data_count
//     mov       rax, [rax + 8]                 ; the caller's Vector3*
//     mov       [rdi + rcx*8 + 0x20], rax      ; m_script_vectors[n] = it
//     shl       rcx, 4
//     vmovss    xmm0, [rax]                    ; caller's x, at +0
//     vinsertps xmm1, xmm0, [rax + 8],    0x10 ; caller's y, at +8
//     vinsertps xmm1, xmm1, [rax + 0x10], 0x20 ; caller's z, at +0x10
//     vmovups   [rdi + rcx + 0x40], xmm0       ; m_result_vectors[n]
//     mov       [rdi + 0x18], eax              ; m_data_count = n + 1
//
// So: m_data_count @0x18, m_script_vectors @0x20 (8-byte entries),
// m_result_vectors @0x40 (16-byte entries) -- the three offsets the
// static_asserts in set_vector_results() pin at 24, 32 and 64.
//
// The answer therefore lands in scratch inside this context and the caller has
// to copy it back. set_vector_results() does that, and calling it after every
// native is not optional -- without it the scratch is simply discarded and the
// out-param reads back as whatever the caller put there.
//
// The redirect cannot corrupt a Vector3 INPUT, because it is the GAME's rather
// than ours: the native copies the caller's existing x/y/z in before it runs,
// so an in-param arrives intact and copying it back returns it unchanged.
//
// Note the 8-BYTE STRIDE in the caller's buffer ([p+0], [p+8], [p+0x10]). A
// caller's Vector3 is 24 bytes with a four-byte hole after each float, which is
// why script_vec3 below is padded and why a Lua caller must mem_alloc(24).
//
// Vector3 RETURN VALUES use the same padded layout, in the return buffer.
// GET_ENTITY_COORDS (0x1E5B2E0) ends:
//
//     mov     rax, [rbx]                       ; the return pointer
//     vmovss  dword ptr [rax],        xmm0     ; x @ +0
//     vpextrd dword ptr [rax + 8],    xmm0, 1  ; y @ +8
//     vpextrd dword ptr [rax + 0x10], xmm0, 2  ; z @ +0x10
//
// A packed 12-byte read of math::vector3<float> would yield {x, 0, native's y}
// -- the "entity spawns nowhere" bug. get_return is therefore specialised below
// to read the padded layout.

namespace rage::invoker {

	struct vec3f { float x, y, z; };
	struct vec4f { float x, y, z, w; };

	// The SCRIPT side's vector3 is not packed. The natives read x, y and z at
	// [p+0], [p+8], [p+10h] (see the header comment), so a caller's buffer is 24
	// bytes with a four-byte hole after each float, and the write-back must use
	// the same stride: a packed vec3f here would drop y into the first hole and
	// never write z. This is also why a Lua caller must mem_alloc(24), not 12.
	struct script_vec3 {
		float x; uint32_t _pad_x;
		float y; uint32_t _pad_y;
		float z; uint32_t _pad_z;
	};

	class native_context {
	protected:
		void* m_return_data;          // @0
		uint32_t m_argument_count;    // @8
		void* m_argument_data;        // @16
		uint32_t m_data_count;
		script_vec3* m_script_vectors[4];
		vec4f m_result_vectors[4];
	public:
		// Copy what the native wrote into this context's scratch back out to
		// the pointers the caller passed. Runs after every native call; see the
		// header comment for why it is the only reason a Vector3 out-param ever
		// arrives.
		void set_vector_results() {
			// The offsets the game reaches into, asserted rather than trusted:
			// this layout IS the ABI, and reordering these fields would not
			// fail to compile, it would corrupt whatever ends up at +24 and
			// +32 while the native runs.
			static_assert(offsetof(native_context, m_data_count)     == 24, "ABI: m_data_count @24");
			static_assert(offsetof(native_context, m_script_vectors) == 32, "ABI: m_script_vectors @32");
			static_assert(offsetof(native_context, m_result_vectors) == 64, "ABI: m_result_vectors @64");

			// The GAME writes m_data_count, so it is not ours to trust. Four is
			// its own maximum -- GET_ENTITY_MATRIX is the one RDR2 native that
			// uses all four Vector3 out-params, and nothing uses more -- and the
			// arrays are sized for that. Clamping means a wrong count cannot walk
			// off the end of this object; it cannot undo an overrun the native
			// already made, but it stops us compounding one.
			uint32_t n = m_data_count;
			if (n > 4) n = 4;
			for (uint32_t i = 0; i < n; ++i) {
				if (!m_script_vectors[i]) continue;
				m_script_vectors[i]->x = m_result_vectors[i].x;
				m_script_vectors[i]->y = m_result_vectors[i].y;
				m_script_vectors[i]->z = m_result_vectors[i].z;
			}
			// Reset so a reused context cannot write a stale vector a second
			// time, to a pointer whose owner may be long gone.
			m_data_count = 0;
		}
		template<typename T>
		T get_argument(int index) {
			return *(T*)&((uint64_t*)m_argument_data)[index];
		}
		template<typename T>
		void set_argument(int index, T value) {
			*(T*)&((uint64_t*)m_argument_data)[index] = value;
		}
		template<typename T>
		T get_return_result() {
			return *(T*)m_return_data;
		}
		template<typename T>
		void set_return(T value) {
			*(T*)m_return_data = value;
		}
		void* get_return_data() { return m_return_data; }
		uint32_t get_argument_count() { return m_argument_count; }
	};

	// Args and the return value share one 8-byte-slot buffer: arguments are read
	// by the native before it writes the result, exactly as ScriptHookV's
	// nativeInit/nativePush/nativeCall does.
	class native_setup : public native_context {
	private:
		uint8_t m_temp_buffer[256];
	public:
		native_setup() {
			m_argument_data = m_temp_buffer;
			m_return_data = m_temp_buffer;
			m_argument_count = 0;
			m_data_count = 0;
			// Both vector arrays, not just the count: set_vector_results skips
			// a null m_script_vectors[i], and that guard is worth nothing if
			// the slot holds whatever was on the stack. The game sets these
			// itself when it uses them, so this only ever matters on the paths
			// where it does not.
			memset(m_script_vectors, 0, sizeof(m_script_vectors));
			memset(m_result_vectors, 0, sizeof(m_result_vectors));
			memset(m_temp_buffer, 0, sizeof(m_temp_buffer));
		}
		template<typename T>
		void push(T value) {
			memset(&m_temp_buffer[8 * m_argument_count], 0, 8);
			*(T*)&m_temp_buffer[8 * m_argument_count] = value;
			m_argument_count++;
		}
		template<typename T>
		T get_return() { return *(T*)m_temp_buffer; }
	};

	// Vector3 returns occupy three 8-byte slots (see the header comment). Reading
	// them packed silently drops y into z and zeroes y.
	template<>
	inline math::vector3<float> native_setup::get_return<math::vector3<float>>() {
		return math::vector3<float>(
			*(float*)&m_temp_buffer[0],
			*(float*)&m_temp_buffer[8],
			*(float*)&m_temp_buffer[16]);
	}

	typedef void(*native_handler)(native_context*);

	// Runtime base of the GTA eboot's first segment; RVA + this = absolute impl.
	// 0 until resolve_base() succeeds; invoke() refuses to call while it is 0.
	extern uintptr_t g_eboot_base;

	bool resolve_base();

	struct pass { template<typename... T> pass(T...) {} };

	template<typename R, typename... Args>
	R invoke(uint64_t rva, Args&&... args) {
		native_setup ctx;
		pass{ ([&]() { ctx.push(args); }(), 1)... };
		if (!g_eboot_base)
			return R();
		native_handler handler = (native_handler)(g_eboot_base + rva);
		handler(&ctx);
		ctx.set_vector_results();
		return ctx.get_return<R>();
	}
}
