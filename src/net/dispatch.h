// src/net/dispatch.h
#pragma once

// The game-thread half of the control channel: pops whatever the net thread
// queued and acts on it. Everything here runs inside the frame hook, so it may
// touch Lua and call natives — and must not block.

namespace net {

    // The longest resource name START/STOP/RESTART/RESINFO can carry, in
    // bytes. Their handlers decode into a stack buffer of this size plus a
    // NUL, and a payload that does not fit is refused rather than clamped --
    // truncating a name would operate on a different resource than the one
    // asked about, with the caller never told.
    //
    // The runtime's valid_name() (src/script/lua/resource.lua) enforces the
    // same number, which is the point: a console session found a
    // 183-character directory that RESLIST advertised as "stopped" while all
    // four ops refused it, because only the wire had a limit. Both sides
    // assert it against tests/protocol_vectors.json in their own suite, the
    // same rule the opcode tables follow, so changing one alone turns the
    // other red.
    const int RESOURCE_NAME_MAX = 127;

    // Drain the inbox. Call once per frame from the game thread.
    void dispatch_pending();

    // Called by the NET thread when a client is accepted: forgets that the
    // previous connection had authenticated. Without it a token would guard
    // only the first connection after boot, and every one after that would
    // inherit the flag from whoever got there first.
    void auth_on_connect();

}  // namespace net
