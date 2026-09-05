#include "script/resource.h"
#include "game/profile.h"
#include "script/lua_runtime.h"
#include "script/embedded_lua.h"
#include "platform/log.h"
#include "lua.hpp"

#include <orbis/libkernel.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ORBIS_O_RDONLY 0x0000
#define ORBIS_SEEK_SET 0
#define ORBIS_SEEK_END 2

namespace script {
namespace {

    const char* const RT_KEY = "rdr2lua.rt";
    const size_t MAX_SCRIPT = 1 << 20;   // same cap the control channel uses

    // --- host primitives -----------------------------------------------------
    // Injected into a __host table that boot.lua captures and then removes from
    // _G. Resource sandboxes read through the shared globals, so anything left
    // in _G would be reachable by every resource — file I/O most of all.

    int l_read_file(lua_State* L) {
        const char* path = luaL_checkstring(L, 1);
        int fd = sceKernelOpen(path, ORBIS_O_RDONLY, 0);
        if (fd < 0) { lua_pushnil(L); return 1; }
        off_t size = sceKernelLseek(fd, 0, ORBIS_SEEK_END);
        if (size < 0 || (size_t)size > MAX_SCRIPT) { sceKernelClose(fd); lua_pushnil(L); return 1; }
        if (sceKernelLseek(fd, 0, ORBIS_SEEK_SET) < 0) { sceKernelClose(fd); lua_pushnil(L); return 1; }
        char* buf = (char*)malloc((size_t)size + 1);
        if (!buf) { sceKernelClose(fd); lua_pushnil(L); return 1; }
        ssize_t got = sceKernelRead(fd, buf, (size_t)size);
        sceKernelClose(fd);
        if (got != (ssize_t)size) { free(buf); lua_pushnil(L); return 1; }
        lua_pushlstring(L, buf, (size_t)size);
        free(buf);
        return 1;
    }

    int l_list_dir(lua_State* L) {
        const char* path = luaL_checkstring(L, 1);
        int dfd = sceKernelOpen(path, ORBIS_O_RDONLY, 0);
        if (dfd < 0) { lua_pushnil(L); return 1; }

        // sceKernelOpen succeeds on a plain file too; that fd's first getdents
        // then fails, with nothing read yet. Check that BEFORE creating the
        // table, so a failure and a genuinely empty directory (first call
        // returns 0, not negative) stay distinguishable to Lua instead of both
        // producing an empty table.
        char dents[4096];
        int nb = sceKernelGetdents(dfd, dents, sizeof(dents));
        if (nb < 0) {
            sceKernelClose(dfd);
            lua_pushnil(L);
            return 1;
        }

        lua_newtable(L);
        int n = 0;
        bool truncated = false;
        while (nb > 0) {
            int off = 0;
            while (off < nb) {
                struct dirent* de = (struct dirent*)(dents + off);
                if (de->d_reclen == 0) break;
                if (strcmp(de->d_name, ".") != 0 && strcmp(de->d_name, "..") != 0) {
                    lua_pushstring(L, de->d_name);
                    lua_rawseti(L, -2, ++n);
                }
                off += de->d_reclen;
            }
            nb = sceKernelGetdents(dfd, dents, sizeof(dents));
            if (nb < 0) truncated = true;
        }
        sceKernelClose(dfd);

        // A later page failing partway through is a different failure than the
        // first-call case above: entries already collected are still real, and
        // the caller (R.list/expand) has no "partial" idiom to hand them back
        // through, so keep the table but say so in the log rather than letting
        // a cut-short listing pass for a complete one silently.
        if (truncated) {
            platform::logf("resource", "list_dir(%s): getdents failed after %d entr%s, "
                                        "listing may be incomplete",
                            path, n, n == 1 ? "y" : "ies");
        }
        return 1;
    }

    int l_now_ms(lua_State* L) {
        lua_pushinteger(L, (lua_Integer)(sceKernelGetProcessTime() / 1000));
        return 1;
    }

    int l_log(lua_State* L) {
        const char* s = luaL_optstring(L, 1, "");
        platform::log_line(nullptr, s);
        return 0;
    }

    // boot.lua calls this with the table of entry points; we stash it in the
    // registry so C++ can reach it after __rt has been removed from _G.
    int l_publish(lua_State* L) {
        luaL_checktype(L, 1, LUA_TTABLE);
        lua_pushvalue(L, 1);
        lua_setfield(L, LUA_REGISTRYINDEX, RT_KEY);
        return 0;
    }

    // --- calling into the runtime -------------------------------------------

    // Push __rt.<field1>.<field2> (field2 optional). False if absent.
    bool push_entry(lua_State* L, const char* a, const char* b) {
        lua_getfield(L, LUA_REGISTRYINDEX, RT_KEY);
        if (!lua_istable(L, -1)) { lua_pop(L, 1); return false; }
        lua_getfield(L, -1, a);
        lua_remove(L, -2);
        if (b) {
            if (!lua_istable(L, -1)) { lua_pop(L, 1); return false; }
            lua_getfield(L, -1, b);
            lua_remove(L, -2);
        }
        if (!lua_isfunction(L, -1)) { lua_pop(L, 1); return false; }
        return true;
    }

    // Call __rt.res.<fn>(name) -> ok, err. Shared by start/stop/restart, which
    // differ only in the function name.
    bool call_lifecycle(const char* fn, const char* name, char* err, size_t err_len) {
        if (err && err_len) err[0] = 0;
        lua_State* L = runtime_state();
        if (!L) { if (err && err_len) snprintf(err, err_len, "runtime not initialised"); return false; }
        int top = lua_gettop(L);
        if (!push_entry(L, "res", fn)) {
            lua_settop(L, top);
            if (err && err_len) snprintf(err, err_len, "resource runtime not booted");
            return false;
        }
        lua_pushstring(L, name ? name : "");
        if (lua_pcall(L, 1, 2, 0) != LUA_OK) {
            const char* e = lua_tostring(L, -1);
            if (err && err_len) snprintf(err, err_len, "%s", e ? e : "(error)");
            lua_settop(L, top);
            return false;
        }
        bool ok = lua_toboolean(L, -2) != 0;
        if (!ok && err && err_len) {
            const char* e = lua_tostring(L, -1);
            snprintf(err, err_len, "%s", e ? e : "failed");
        }
        lua_settop(L, top);
        return ok;
    }

    // resource_list_entry_fn adapter for resource_list_pack (resource.h):
    // walks the { {name=, state=}, ... } table that __rt.res.list() returns,
    // left on top of the Lua stack by the caller (resource_list below).
    // Copies name/state out to the caller's buffers -- rather than handing
    // back the raw lua_tostring() pointers -- BEFORE popping, since those
    // pointers are only guaranteed valid while the strings are still
    // referenced on the stack.
    bool lua_list_entry(void* ctx, int index, char* name, size_t name_cap, char* state, size_t state_cap) {
        lua_State* L = (lua_State*)ctx;
        lua_rawgeti(L, -1, index + 1);
        bool ok = false;
        if (lua_istable(L, -1)) {
            lua_getfield(L, -1, "name");
            lua_getfield(L, -2, "state");
            const char* nm = lua_tostring(L, -2);
            const char* st = lua_tostring(L, -1);
            if (nm && st) {
                snprintf(name, name_cap, "%s", nm);
                snprintf(state, state_cap, "%s", st);
                ok = true;
            }
            lua_pop(L, 2);
        }
        lua_pop(L, 1);
        return ok;
    }

}  // namespace

bool resource_boot() {
    lua_State* L = runtime_state();
    if (!L) return false;

    // __rt: the table the chunks share, since this Lua has no package library
    // and they cannot require() each other. It MUST exist before the first
    // chunk loads — every one of them binds `local rt = __rt` at its top, and
    // would otherwise capture nil and fail on its first runtime use.
    lua_newtable(L);
    // The resource root is per-game and only known once the profile is
    // detected, so it is handed in here rather than baked into resource.lua.
    // Baking it would mean one embedded chunk per game, which is exactly the
    // duplication this plugin exists to remove.
    lua_pushstring(L, game::current() ? game::current()->resource_root : "");
    lua_setfield(L, -2, "resource_root");
    lua_setglobal(L, "__rt");

    // __host: the primitives Lua cannot provide itself. boot.lua captures this
    // table and removes it from _G, so no resource sandbox can reach file I/O.
    lua_newtable(L);
    lua_pushcfunction(L, l_read_file); lua_setfield(L, -2, "read_file");
    lua_pushcfunction(L, l_list_dir);  lua_setfield(L, -2, "list_dir");
    lua_pushcfunction(L, l_now_ms);    lua_setfield(L, -2, "now_ms");
    lua_pushcfunction(L, l_log);       lua_setfield(L, -2, "log");
    lua_pushcfunction(L, l_publish);   lua_setfield(L, -2, "publish");
    lua_setglobal(L, "__host");

    for (const embedded_chunk* c = EMBEDDED_LUA; c->name; ++c) {
        char err[512];
        int kind = 0;
        if (!runtime_exec_capture(c->name, c->src, strlen(c->src), err, sizeof(err), &kind)) {
            platform::logf("resource", "embedded chunk %s failed: %s", c->name, err);
            // A half-built sandbox must not stay open: the plugin keeps running
            // after this (the control channel is still up, scripts can still be
            // pushed), and __host left sitting in _G would hand any of them
            // read_file/list_dir on an arbitrary path, straight past the path
            // guard that has confined file access from the console side since
            // the previous milestone.
            lua_pushnil(L); lua_setglobal(L, "__rt");
            lua_pushnil(L); lua_setglobal(L, "__host");
            return false;
        }
    }

    lua_getfield(L, LUA_REGISTRYINDEX, RT_KEY);
    bool ok = lua_istable(L, -1);
    lua_pop(L, 1);
    // Every chunk ran without raising, but boot.lua never published its
    // entry points. The only way that happens is if the scrub at the end of
    // boot.lua -- "if __host and __host.publish then publish(rt); __host =
    // nil; __rt = nil end" -- never ran, since publishing and scrubbing
    // happen inside that one guarded block. So reaching here with ok ==
    // false is exactly the path where __rt and __host are still sitting in
    // _G: nil them here, for the same reason the mid-loop failure above does.
    if (!ok) {
        platform::log_line("resource", "boot.lua did not publish its entry points");
        lua_pushnil(L); lua_setglobal(L, "__rt");
        lua_pushnil(L); lua_setglobal(L, "__host");
    }
    return ok;
}

bool resource_is_booted() {
    lua_State* L = runtime_state();
    if (!L) return false;
    // Same check resource_boot() makes right after the embedded chunks run:
    // the entry points are only reachable once boot.lua's publish/scrub
    // guard actually ran.
    lua_getfield(L, LUA_REGISTRYINDEX, RT_KEY);
    bool ok = lua_istable(L, -1);
    lua_pop(L, 1);
    return ok;
}

void resource_tick() {
    lua_State* L = runtime_state();
    if (!L) return;
    int top = lua_gettop(L);
    if (!push_entry(L, "sched", "tick")) { lua_settop(L, top); return; }
    lua_pushinteger(L, (lua_Integer)(sceKernelGetProcessTime() / 1000));
    if (lua_pcall(L, 1, 0, 0) != LUA_OK) {
        const char* e = lua_tostring(L, -1);
        platform::logf("resource", "scheduler tick failed: %s", e ? e : "(error)");
    }
    lua_settop(L, top);
}

bool resource_start(const char* name, char* err, size_t err_len)   { return call_lifecycle("start", name, err, err_len); }
bool resource_stop(const char* name, char* err, size_t err_len)    { return call_lifecycle("stop", name, err, err_len); }
bool resource_restart(const char* name, char* err, size_t err_len) { return call_lifecycle("restart", name, err, err_len); }

int resource_list(char* out, size_t out_len) {
    if (!out || out_len == 0) return 0;
    out[0] = 0;
    lua_State* L = runtime_state();
    if (!L) return 0;
    int top = lua_gettop(L);
    if (!push_entry(L, "res", "list")) { lua_settop(L, top); return 0; }
    if (lua_pcall(L, 0, 1, 0) != LUA_OK || !lua_istable(L, -1)) { lua_settop(L, top); return 0; }

    // The packing itself -- including the "stop at the first entry that
    // doesn't fit, don't skip past it" truncation rule -- lives in
    // resource_list_pack (resource.h) so it can be exercised on a host
    // without booting this Lua runtime.
    int n = (int)lua_rawlen(L, -1);
    int used = resource_list_pack(out, out_len, n, lua_list_entry, L);
    lua_settop(L, top);
    return used;
}

bool resource_info(const char* name, char* out, size_t out_len, char* err, size_t err_len) {
    if (err && err_len) err[0] = 0;
    if (!out || out_len == 0) {
        if (err && err_len) snprintf(err, err_len, "no output buffer");
        return false;
    }
    out[0] = 0;
    lua_State* L = runtime_state();
    if (!L) {
        if (err && err_len) snprintf(err, err_len, "runtime not initialised");
        return false;
    }
    int top = lua_gettop(L);
    if (!push_entry(L, "res", "info")) {
        lua_settop(L, top);
        if (err && err_len) snprintf(err, err_len, "resource runtime not booted");
        return false;
    }
    lua_pushstring(L, name ? name : "");
    // A raise and a nil answer are different failures, and used to reach the
    // control channel as the same "no such resource": one is a bug in the
    // runtime, the other is the user mistyping a name.
    if (lua_pcall(L, 1, 1, 0) != LUA_OK) {
        const char* e = lua_tostring(L, -1);
        if (err && err_len) snprintf(err, err_len, "%s", e ? e : "(error)");
        lua_settop(L, top);
        return false;
    }
    if (!lua_istable(L, -1)) {
        lua_settop(L, top);
        if (err && err_len) snprintf(err, err_len, "no such resource");
        return false;
    }
    lua_getfield(L, -1, "threads");
    lua_getfield(L, -2, "handlers");
    lua_getfield(L, -3, "exports");
    lua_getfield(L, -4, "commands");
    int w = snprintf(out, out_len, "threads=%d handlers=%d exports=%d commands=%d",
                     (int)lua_tointeger(L, -4), (int)lua_tointeger(L, -3),
                     (int)lua_tointeger(L, -2), (int)lua_tointeger(L, -1));
    lua_settop(L, top);
    // Unreachable at the caller's 128 bytes -- four ints is at most ~81 --
    // but the length was previously computed and thrown away with `(void)w`,
    // and a smaller buffer would have handed back a silently cut string as a
    // complete answer. That exact pattern has been caught four times on this
    // branch; checking it costs three lines.
    if (w < 0 || (size_t)w >= out_len) {
        out[0] = 0;
        if (err && err_len) snprintf(err, err_len, "info did not fit the reply buffer");
        return false;
    }
    return true;
}

bool resource_command(const char* raw, char* err, size_t err_len) {
    if (err && err_len) err[0] = 0;
    lua_State* L = runtime_state();
    if (!L) { if (err && err_len) snprintf(err, err_len, "runtime not initialised"); return false; }
    int top = lua_gettop(L);
    if (!push_entry(L, "commands", "run")) {
        lua_settop(L, top);
        if (err && err_len) snprintf(err, err_len, "resource runtime not booted");
        return false;
    }
    lua_pushstring(L, raw ? raw : "");
    if (lua_pcall(L, 1, 2, 0) != LUA_OK) {
        const char* e = lua_tostring(L, -1);
        if (err && err_len) snprintf(err, err_len, "%s", e ? e : "(error)");
        lua_settop(L, top);
        return false;
    }
    bool ok = lua_toboolean(L, -2) != 0;
    if (!ok && err && err_len) {
        const char* e = lua_tostring(L, -1);
        snprintf(err, err_len, "%s", e ? e : "unknown command");
    }
    lua_settop(L, top);
    return ok;
}

int resource_autostart() {
    // One resource per line; '#' starts a comment. Listed rather than "start
    // everything present" so a resource can sit on the console and be off.
    int fd = sceKernelOpen("/data/rdr2lua/autostart.cfg", ORBIS_O_RDONLY, 0);
    if (fd < 0) return 0;
    char buf[4096];
    ssize_t got = sceKernelRead(fd, buf, sizeof(buf) - 1);

    // Probe for one further byte at the current file position rather than
    // comparing `got` to the buffer size: that would only catch the case
    // where the main read filled the buffer exactly, and miss a SHORT read
    // that still left data unread (unlikely for a request this small on a
    // local file, but free to close off this way rather than assume it away
    // -- a file that exactly fills `got` bytes and truly ends there still
    // reads 0 from the probe, so a config that just fits is unaffected).
    bool truncated = false;
    if (got > 0) {
        char probe;
        if (sceKernelRead(fd, &probe, 1) > 0) truncated = true;
    }

    sceKernelClose(fd);
    if (got <= 0) return 0;
    buf[got] = 0;

    if (truncated) {
        // Cut back to the last complete line rather than act on half of one:
        // a name split by the buffer boundary is not a real resource name,
        // and starting whatever the split left is worse than skipping the
        // truncated tail entirely.
        char* last_nl = strrchr(buf, '\n');
        if (last_nl) *last_nl = 0; else buf[0] = 0;
        platform::logf("resource", "autostart.cfg exceeds the %zu-byte read buffer; "
                                    "the tail past the last complete line was ignored",
                        sizeof(buf) - 1);
    }

    int started = 0;
    char* line = buf;
    while (line && *line) {
        char* nl = strchr(line, '\n');
        if (nl) *nl = 0;
        char* hash = strchr(line, '#');
        if (hash) *hash = 0;
        while (*line == ' ' || *line == '\t' || *line == '\r') ++line;
        size_t len = strlen(line);
        while (len && (line[len - 1] == ' ' || line[len - 1] == '\t' || line[len - 1] == '\r'))
            line[--len] = 0;
        if (len) {
            char err[256];
            if (resource_start(line, err, sizeof(err))) {
                ++started;
                platform::logf("resource", "started %s", line);
            } else {
                platform::logf("resource", "failed to start %s: %s", line, err);
            }
        }
        line = nl ? nl + 1 : nullptr;
    }
    return started;
}

}  // namespace script
