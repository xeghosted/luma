// src/net/path_guard.h
#pragma once
#include <stddef.h>

// Resolving a client-supplied path under the plugin's own directory. Anything
// arriving over the wire is untrusted, and this is the only place that decides
// what a path is allowed to be — so it is a pure function with no filesystem
// access, and it is tested on a host.
//
// This check is purely textual: it never touches the filesystem, which is the
// whole reason it can be unit-tested on a host at all. That means a symlink
// planted under /data/rdr2lua that points back out of it would defeat it — a
// resolved path can look sandboxed and still land somewhere else. That is
// accepted, not missed: the threat model here is a home LAN and a jailbroken
// console that already has FTP access equivalent to this channel, so a
// symlink attack buys an attacker nothing they didn't already have. Fixing it
// would mean lstat-ing every component, which requires ORBIS and would end
// this unit's host-testability — not worth it for a threat that isn't in
// scope. If the threat model ever changes, revisit this.

namespace net {

    // The sandbox root every resolved path lands under. Exported so callers
    // that need to name the root (e.g. to open it directly for a listing)
    // don't hardcode a second copy that could drift from this one.
    // The sandbox root, from the detected game's profile.
const char* root();

    // Join `rel` onto ROOT into `out`. False if rel is empty, absolute,
    // contains a ".." segment or a backslash, or the result would not fit.
    bool safe_path(const char* rel, char* out, size_t out_len);

}  // namespace net
