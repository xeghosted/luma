#pragma once

// Console-side script loading. Boots the Lua runtime on the game thread, installs
// the real native-call seam, loads every .lua from the flat script directory,
// then boots the resource model (see resource.h) alongside it.
//
// Layout on the console:
//   /data/rdr2lua/natives.lua       optional prelude of generated native bindings
//   /data/rdr2lua/scripts/*.lua     your v1-style scripts, loaded in directory order
//   /data/rdr2lua/resources/*/      resources, each with its own fxmanifest.lua
//   /data/rdr2lua/autostart.cfg     resources to start automatically, one per line
//   /data/rdr2lua/rdr2lua.log        everything log()/print() produced
//
// Keeping this separate from lua_runtime keeps that unit free of any ORBIS or
// filesystem dependency, so it stays testable on a host.

namespace script {

// Boot the runtime and load the scripts. Returns the number of scripts loaded,
// or -1 if the runtime itself failed to start. Call on the game thread: script
// top-level code may call natives.
int script_boot();

// Drive the per-frame callbacks. Call once per frame from the game thread.
void script_frame();

// Re-run every .lua in the flat script directory against the live runtime.
// Returns the number that loaded. This is the M1 reload, unchanged now that
// the resource model (see resource.h) boots alongside it: a flat script was
// never a resource with something to unload, so re-pushing one still just
// redefines what it defines rather than tearing anything down first. Real
// start/stop/restart for resources lives in resource.h.
int script_reload();

}  // namespace script
