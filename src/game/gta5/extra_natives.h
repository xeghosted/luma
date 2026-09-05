#pragma once
#include "rage/invoker/invoker.h"
#include "rage/types/base_types.h"

// Natives that the generated natives.h omits, bound by the RVA the CUSA00411
// v1.57 IDB gives for them (E:\Projects\IDA\PS4\GTA5\eboot_named.i64,
// imagebase 0, so an impl address IS its RVA). Hand-written, so keep it
// small: everything else missing from natives.h is reached by hash through
// natives_hash.h.
//
// "Verified" used to be claimed here for all of them. It was not true of
// get_control_instructional_buttons_string, which pointed at ROPE_LOAD_TEXTURES
// until the registry audit caught it: the IDB name that vouched for the address
// was itself a guess, marked confidence=aligned inside the database. An IDB
// name cannot verify an address. What can is rage::scrThread::RegisterCommand's
// own call sites -- the pairing the game dispatches through -- which is what
// tools/native_crosscheck_offline.py now checks every one of these against.
//
// The point of wrapping these by RVA rather than leaving them to the hash path
// is independence. A hash native needs rage::hash_natives::build() to have
// walked the game's registry first; these work from the first frame, and they
// keep working if that walk ever fails on a build whose table layout differs.
// So this file is for the handful that a script most wants to be certain of.
//
// Names and signatures are the canonical ones from the v1.57 hash header, so a
// script calling GET_ENTITY_COORDS gets the same thing here as it would from any
// other GTA V scripting environment.
//
// Included before natives_hash.h is generated: tools/gen_natives_lua.py and
// tools/gen_hash_natives.py both treat a name defined here as already covered,
// which is what keeps the hash header from redefining it.
namespace native {

    // Vector3 returns land in three 8-byte slots, not a packed struct — see the
    // header comment in invoker.h, which cites these two natives as the pair the
    // layout was read off.
    static math::vector3<float> get_entity_coords(Entity entity, bool alive) {
        return rage::invoker::invoke<math::vector3<float>>(0x9B2BC0, entity, alive);
    }
    static math::vector3<float> get_offset_from_entity_in_world_coords(Entity entity, float offsetX, float offsetY, float offsetZ) {
        return rage::invoker::invoke<math::vector3<float>>(0x9B3470, entity, offsetX, offsetY, offsetZ);
    }

    // Text measurement — the begin/end pairs a script needs to lay anything out.
    static Void begin_text_command_get_screen_width_of_display_text(const char* text) {
        return rage::invoker::invoke<Void>(0x9E0710, text);
    }
    static float end_text_command_get_screen_width_of_display_text(bool p0) {
        return rage::invoker::invoke<float>(0x9E0720, p0);
    }
    static Void begin_text_command_get_number_of_lines_for_string(const char* entry) {
        return rage::invoker::invoke<Void>(0x9E0750, entry);
    }
    static int end_text_command_get_number_of_lines_for_string(float x, float y) {
        return rage::invoker::invoke<int>(0x9E0760, x, y);
    }

    static const char* get_control_instructional_buttons_string(int control, int action, bool allowXOSwap) {
        // [rva-fix] was 0xAA0FD0, which is ROPE_LOAD_TEXTURES and takes no
        // arguments at all. See the registry audit in docs/.
        return rage::invoker::invoke<const char*>(0xA6AD80, control, action, allowXOSwap);
    }
}
