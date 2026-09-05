// src/net/net_server.h
#pragma once

// The control channel's socket side. Owns one listening socket on TCP 9616 and
// at most one client, on its own thread.
//
// This thread must never touch Lua and never call a native: both are only safe
// on the game's script thread. Everything it receives goes through the mailbox
// (src/net/mailbox.h) for the game thread to pick up.

namespace net {

    // Bind, listen, and start the thread. False if the socket could not be set
    // up; the plugin stays usable either way.
    bool server_start();

    // Abort the listening socket and join the thread. Safe when not started.
    void server_stop();

}  // namespace net
