#pragma once
#include <stddef.h>
#include <stdio.h>
#include <string.h>

// The C++ side of the resource model. The model itself is Lua (see
// src/script/lua/) — this is the façade: it supplies the host primitives that
// Lua cannot provide for itself (file I/O, a clock), loads the embedded chunks,
// and exposes start/stop/list/info to the control channel's dispatcher.
//
// Everything here runs on the game thread, inside the frame hook. Nothing here
// may block.

namespace script {

    // Load the embedded Lua and publish its entry points. Call once, after
    // runtime_init(), on the game thread. False if a chunk failed to load —
    // the message is written to the log.
    bool resource_boot();

    // Drive the scheduler. Call once per frame from the game thread.
    void resource_tick();

    // Lifecycle. `err` receives the reason on failure (NUL-terminated,
    // truncated to err_len).
    bool resource_start(const char* name, char* err, size_t err_len);
    bool resource_stop(const char* name, char* err, size_t err_len);
    bool resource_restart(const char* name, char* err, size_t err_len);

    // True once resource_boot() has published the runtime's entry points and
    // they are still reachable (i.e. resource_list/_info/_command etc. have
    // something to call into). Cheap: a registry lookup, no Lua call.
    bool resource_is_booted();

    // "name state\n" per resource. Returns the number of bytes written --
    // which is 0 both when there are genuinely no resources AND when the
    // runtime isn't booted yet. Callers that must tell those apart (e.g. the
    // control channel, so it can reply with an error instead of an empty
    // list) should check resource_is_booted() first rather than infer it
    // from a 0 return here.
    int resource_list(char* out, size_t out_len);

    // Appended to a resource_list_pack() reply the moment an entry does not
    // fit, so the client can always tell a truncated listing from a complete
    // one -- mirrors on_ls's TRUNC_MARK in src/net/dispatch.cpp.
    constexpr const char* RESOURCE_LIST_TRUNC_MARK = "-- truncated: listing incomplete --\n";

    // One entry for resource_list_pack below: called for index in [0, n).
    // Returns false to mean "nothing to print for this index" (skipped, not
    // a truncation -- e.g. a malformed table entry); otherwise it must have
    // copied a NUL-terminated name into `name` (capacity name_cap) and a
    // state into `state` (capacity state_cap).
    typedef bool (*resource_list_entry_fn)(void* ctx, int index,
                                            char* name, size_t name_cap,
                                            char* state, size_t state_cap);

    // The truncation-safe entry-packing loop resource_list uses to build its
    // reply, factored out as a pure function -- no Lua, no ORBIS -- so it can
    // be exercised by a host test without booting the runtime the rest of
    // resource_list depends on (see tests/net_test.cpp). Appends "<name>
    // <state>\n" for each entry `next` supplies in turn; the FIRST entry that
    // would not fit inside out_len ends the listing right there, with
    // RESOURCE_LIST_TRUNC_MARK appended, rather than being skipped while
    // shorter entries after it get packed instead -- that would leave a reply
    // that isn't even a prefix of the real list, with no way for the client
    // to tell. Returns the number of bytes written.
    inline int resource_list_pack(char* out, size_t out_len, int n,
                                   resource_list_entry_fn next, void* ctx) {
        if (!out || out_len == 0) return 0;
        out[0] = 0;
        if (!next || n <= 0) return 0;

        const size_t trunc_len = strlen(RESOURCE_LIST_TRUNC_MARK);
        if (out_len <= trunc_len) return 0;  // no room to ever say "truncated"
        const size_t limit = out_len - trunc_len;

        size_t used = 0;
        bool truncated = false;
        for (int i = 0; i < n; ++i) {
            char name[256], state[64];
            if (!next(ctx, i, name, sizeof(name), state, sizeof(state))) continue;
            int w = snprintf(out + used, limit - used, "%s %s\n", name, state);
            if (w < 0 || (size_t)w >= limit - used) { truncated = true; break; }
            used += (size_t)w;
        }

        if (truncated) {
            memcpy(out + used, RESOURCE_LIST_TRUNC_MARK, trunc_len);
            used += trunc_len;
        }
        return (int)used;
    }

    // "threads=N handlers=N exports=N commands=N". False on failure, with the
    // reason written to `err` if one was supplied -- and the reason matters:
    // "the resource runtime never booted", "the Lua call raised" and "there
    // is no such resource" are three different problems, and collapsing them
    // into one message sends whoever reads it hunting for a missing resource
    // when the runtime itself is down. resource_list makes the same
    // distinction (via resource_is_booted, which its caller must check first);
    // this reports it directly, since unlike list it already has an error
    // channel of its own.
    bool resource_info(const char* name, char* out, size_t out_len,
                       char* err = nullptr, size_t err_len = 0);

    // Run a console command line. False if no such command.
    bool resource_command(const char* raw, char* err, size_t err_len);

    // Start everything listed in /data/rdr2lua/autostart.cfg, in order.
    // Returns the number started.
    int resource_autostart();

}  // namespace script
