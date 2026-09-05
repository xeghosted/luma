#include "script/lua_runtime.h"
#include "script/native_bridge.h"
#include "lua.hpp"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

namespace script {

namespace {

lua_State*  g_L = nullptr;
log_sink_fn g_sink = nullptr;
log_sink_fn g_notify_sink = nullptr;

// Registry key holding the array of on_tick callbacks.
const char* const TICKS_KEY = "insulin.ticks";

void emit(const char* msg) {
    if (g_sink) g_sink(msg);
}

void emit_notify(const char* msg) {
    if (g_notify_sink) g_notify_sink(msg);
}

// Printf-style internal log (load/run/tick errors) -- and the screen.
//
// Ordinary output went quiet when log/print stopped notifying, but errors did
// not, and that is the distinction the split exists to draw: a notification is
// for the rare and important, and a script that failed to load is exactly
// that. Losing it from the screen as a side effect of quietening print() would
// have traded one silent problem for another, since without the editor
// attached the only other copy is in a file you fetch over FTP.
//
// Bounded by construction, both of its two callers: runtime_load_buffer runs
// once per .lua file per RELOAD, and runtime_tick removes a raising callback
// from the tick list after its first error rather than logging it every frame.
void logf(const char* fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    emit(buf);
    emit_notify(buf);
}

// Join the `n` values starting at stack index `base` into one tab-separated
// string and push it. This is the ONE stringification in the runtime: both
// log/print and the EXEC reply come through here, so the same value can never
// render two different ways depending on which door it left by.
//
// `limit` bounds the WORK, not merely the answer: a chunk that returns a
// 100 MB string must not have that string concatenated into a second 100 MB
// string just to be cut down to a 4 KB reply. Zero means unbounded, which is
// what log/print want (the log sink truncates per line itself). Otherwise the
// join stops as soon as the text passes `limit`, and it deliberately
// overshoots by exactly one byte -- that is what lets the caller tell "filled
// the buffer exactly" from "was cut" by length alone.
//
// luaL_addvalue, NOT luaL_addlstring, and that is not a style choice.
// luaL_tolstring leaves its result ON THE STACK, and every luaL_Buffer call
// except luaL_addvalue assumes the buffer's box (or, before the first grow,
// the light-userdata placeholder luaL_buffinit pushed) is what sits on top:
// they pass -1 to lauxlib's prepbuffsize. With the string in the way, the
// first grow past LUAL_BUFFERSIZE did `lua_remove` on the STRING, put a
// to-be-closed box where it had been, and the `lua_pop` below then CLOSED
// that box -- freeing the buffer luaL_pushresult goes on to read. A 2 KB
// EXEC reply (`=("x"):rep(2000)` in the editor console, against on_exec's
// 4 KB buffer) was a straight heap-use-after-free, and a multi-value join
// crossing the same boundary segfaulted outright in resizebox. luaL_addvalue
// exists for exactly this position: it passes -2, and it pops the value
// itself.
//
// The clamp therefore has to become a real Lua string rather than a shorter
// length argument. lua_pushlstring is safe to call here: the full-length
// result is still anchored on the stack, so nothing it points at can be
// collected while the copy is made.
void push_joined(lua_State* L, int base, int n, size_t limit) {
    luaL_Buffer b;
    luaL_buffinit(L, &b);
    for (int i = 0; i < n; ++i) {
        if (i) luaL_addchar(&b, '\t');    // buffer on top here: -1 is correct
        size_t len;
        const char* s = luaL_tolstring(L, base + i, &len);  // pushes the string
        if (limit) {
            size_t have = luaL_bufflen(&b);
            if (have > limit) { lua_pop(L, 1); break; }
            size_t room = limit + 1 - have;   // >= 1: have <= limit above
            if (len > room) {
                lua_pushlstring(L, s, room);
                lua_remove(L, -2);            // drop the un-clamped original
            }
        }
        luaL_addvalue(&b);                    // consumes the value on top
        if (limit && luaL_bufflen(&b) > limit) break;
    }
    luaL_pushresult(&b);
}

// log(...) / print(...): stringify every argument (tab-separated) -> sink.
int l_log(lua_State* L) {
    push_joined(L, 1, lua_gettop(L), 0);
    emit(lua_tostring(L, -1));
    lua_pop(L, 1);
    return 0;
}

// notify(...): the same line, to the same log, PLUS the screen. Stringified by
// the identical call rather than a second rule to remember -- notify is log
// with one extra destination, and a script that wants to be seen says so.
//
// It logs as well as notifies deliberately: the on-screen buffer is small and
// cuts long text to fit, and this is what keeps that cut from losing anything.
int l_notify(lua_State* L) {
    push_joined(L, 1, lua_gettop(L), 0);
    const char* line = lua_tostring(L, -1);
    emit(line);
    emit_notify(line);
    lua_pop(L, 1);
    return 0;
}

// push_joined over every argument, as a Lua function so it can be run under
// lua_pcall -- see render_results below for why that protection is not
// optional. The byte limit rides in an upvalue rather than as an argument so
// that every value on the stack when this runs IS one of the chunk's results,
// with no off-by-one to get wrong; file-static state was rejected here for
// the same reason exec_kind is an out-parameter (see lua_runtime.h).
int l_join_results(lua_State* L) {
    size_t limit = (size_t)lua_tointeger(L, lua_upvalueindex(1));
    push_joined(L, 1, lua_gettop(L), limit);
    return 1;
}

// Render the `n` values sitting above `base` into the caller's buffer.
// Only called with a non-null, non-empty buffer and n > 0.
void render_results(lua_State* L, int base, int n, exec_result* r) {
    const size_t mark_len = strlen(EXEC_RESULT_TRUNC_MARK);
    const size_t usable   = r->buf_len - 1;      // one byte reserved for the NUL
    // A zero limit means "unbounded" to push_joined, so a one-byte buffer
    // must not pass one: ask for 1 instead. Everything is truncated at that
    // size anyway, and the truncated branch below writes nothing but the NUL.
    const size_t limit    = usable ? usable : 1;

    // The join runs under lua_pcall because luaL_tolstring can invoke a
    // __tostring metamethod, and that is resource-written Lua which is free
    // to raise. Unprotected, such an error reaches lua_atpanic and takes the
    // process down -- on this platform, the game. A raise here is not the
    // chunk failing, so it is reported through value_error, not as an error
    // of the exec.
    if (!lua_checkstack(L, 3)) {
        r->value_error = true;
        r->len = (size_t)snprintf(r->buf, r->buf_len, "(result: out of Lua stack)");
        if (r->len >= r->buf_len) r->len = r->buf_len - 1;
        return;
    }
    lua_pushinteger(L, (lua_Integer)limit);
    lua_pushcclosure(L, l_join_results, 1);
    lua_insert(L, base + 1);                     // below the results it consumes
    if (lua_pcall(L, n, 1, 0) != LUA_OK) {
        const char* e = lua_tostring(L, -1);
        int w = snprintf(r->buf, r->buf_len, "(result could not be rendered: %s)",
                         e ? e : "(error)");
        r->value_error = true;
        r->len = (w < 0) ? 0 : ((size_t)w >= r->buf_len ? r->buf_len - 1 : (size_t)w);
        lua_pop(L, 1);
        return;
    }

    size_t      jlen   = 0;
    const char* joined = lua_tolstring(L, -1, &jlen);
    if (!joined) { jlen = 0; joined = ""; }      // l_join_results always returns a string
    if (jlen <= usable) {
        memcpy(r->buf, joined, jlen);
        r->len = jlen;
    } else {
        // Cut, and say so in the bytes themselves. The marker wins the tail
        // of the buffer: a caller that can only see the payload must never
        // be handed a piece of a value that reads like the whole of one.
        r->truncated = true;
        size_t keep = usable > mark_len ? usable - mark_len : 0;
        size_t room = usable - keep;
        size_t m    = mark_len < room ? mark_len : room;
        memcpy(r->buf, joined, keep);
        memcpy(r->buf + keep, EXEC_RESULT_TRUNC_MARK, m);
        r->len = keep + m;
    }
    r->buf[r->len] = 0;
    lua_pop(L, 1);
}

// on_tick(fn): append fn to the registry tick array.
int l_on_tick(lua_State* L) {
    luaL_checktype(L, 1, LUA_TFUNCTION);
    lua_getfield(L, LUA_REGISTRYINDEX, TICKS_KEY);
    int n = (int)lua_rawlen(L, -1);
    lua_pushvalue(L, 1);
    lua_rawseti(L, -2, n + 1);
    lua_pop(L, 1);
    return 0;
}

}  // namespace

void runtime_set_log_sink(log_sink_fn fn) { g_sink = fn; }
void runtime_set_notify_sink(log_sink_fn fn) { g_notify_sink = fn; }

bool runtime_init() {
    if (g_L) return true;
    lua_State* L = luaL_newstate();
    if (!L) return false;
    luaL_openlibs(L);   // trimmed set (see linit.c): base/coroutine/table/string/math/utf8/debug
    bridge_open(L);

    lua_newtable(L);
    lua_setfield(L, LUA_REGISTRYINDEX, TICKS_KEY);

    lua_pushcfunction(L, l_on_tick);
    lua_setglobal(L, "on_tick");
    lua_pushcfunction(L, l_log);
    lua_setglobal(L, "log");
    lua_pushcfunction(L, l_log);
    lua_setglobal(L, "print");
    lua_pushcfunction(L, l_notify);
    lua_setglobal(L, "notify");

    g_L = L;
    return true;
}

bool runtime_exec_capture(const char* name, const char* src, size_t len,
                          char* err, size_t err_len, int* kind, exec_result* result) {
    if (err && err_len) err[0] = 0;
    if (kind) *kind = EXEC_SUCCEEDED;
    if (result) {
        result->nresults = 0;
        result->len = 0;
        result->truncated = false;
        result->value_error = false;
        if (result->buf && result->buf_len) result->buf[0] = 0;
    }
    if (!g_L) {
        if (err && err_len) snprintf(err, err_len, "runtime not initialised");
        if (kind) *kind = EXEC_NOT_INITIALISED;
        return false;
    }
    lua_State* L = g_L;
    // Captured before the chunk is pushed, so every exit below can restore the
    // stack with one settop -- necessary now that the call returns an unknown
    // number of results rather than a fixed zero.
    const int base = lua_gettop(L);
    if (luaL_loadbuffer(L, src, len, name) != LUA_OK) {
        const char* e = lua_tostring(L, -1);
        if (err && err_len) snprintf(err, err_len, "%s", e ? e : "(load error)");
        if (kind) *kind = EXEC_COMPILE_ERROR;
        lua_settop(L, base);
        return false;
    }
    // LUA_MULTRET, not 0. The chunk's return values ARE the EXEC reply: the
    // editor's console rewrites "=expr" into "return expr" precisely so the
    // value comes back as the reply payload, and asking for zero results here
    // is what made it answer "ok" to everything instead.
    if (lua_pcall(L, 0, LUA_MULTRET, 0) != LUA_OK) {
        const char* e = lua_tostring(L, -1);
        if (err && err_len) snprintf(err, err_len, "%s", e ? e : "(run error)");
        if (kind) *kind = EXEC_RUNTIME_ERROR;
        lua_settop(L, base);
        return false;
    }
    int nres = lua_gettop(L) - base;
    if (result) {
        result->nresults = nres;
        if (nres > 0 && result->buf && result->buf_len)
            render_results(L, base, nres, result);
    }
    lua_settop(L, base);
    return true;
}

bool runtime_load_buffer(const char* name, const char* src, size_t len) {
    // Not initialised stays silent, exactly as the pre-capture code did: this
    // runs once per .lua file on every RELOAD, and script_frame() calls into
    // here every frame regardless of whether runtime_init() ever succeeded —
    // a log line here would spam one per file per RELOAD attempt.
    if (!g_L) return false;

    char err[512];
    int  kind = EXEC_SUCCEEDED;
    if (runtime_exec_capture(name, src, len, err, sizeof(err), &kind))
        return true;
    // Preserve the old distinction: a chunk that failed to compile logged as
    // "[load error]", one that compiled and then raised logged as
    // "[run error]" — two different investigations for whoever reads the log.
    const char* tag = (kind == EXEC_RUNTIME_ERROR) ? "[run error]" : "[load error]";
    logf("%s %s: %s", tag, name, err);
    return false;
}

void runtime_tick() {
    if (!g_L) return;
    lua_State* L = g_L;
    lua_getfield(L, LUA_REGISTRYINDEX, TICKS_KEY);
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        return;
    }
    int n = (int)lua_rawlen(L, -1);
    int i = 1;
    while (i <= n) {
        lua_rawgeti(L, -1, i);           // push callback
        if (lua_pcall(L, 0, 0, 0) != LUA_OK) {
            const char* err = lua_tostring(L, -1);
            logf("[tick error] %s", err ? err : "(non-string error)");
            lua_pop(L, 1);               // pop error -> ticks table at -1
            lua_rawgeti(L, -1, n);       // swap-remove: t[i] = t[n]
            lua_rawseti(L, -2, i);
            lua_pushnil(L);
            lua_rawseti(L, -2, n);       // t[n] = nil
            --n;                         // re-check the element now at i
        } else {
            ++i;
        }
    }
    lua_pop(L, 1);                       // pop ticks table
}

void runtime_clear_ticks() {
    if (!g_L) return;
    // A FRESH table rather than emptying the existing one in place. runtime_tick
    // holds the list on its own Lua stack while it iterates, so an in-place
    // clear from inside a callback would pull the array out from under the loop
    // it is running in; replacing the registry entry leaves that iteration
    // walking the old table to a clean finish. No caller does this today --
    // reloads are dispatched before runtime_tick in script_frame -- and this is
    // the cheaper of the two ways to make sure none ever can.
    lua_newtable(g_L);
    lua_setfield(g_L, LUA_REGISTRYINDEX, TICKS_KEY);
}

bool runtime_exec(const char* name, const char* src) {
    return runtime_load_buffer(name, src, strlen(src));
}

void runtime_shutdown() {
    if (g_L) {
        lua_close(g_L);
        g_L = nullptr;
    }
}

lua_State* runtime_state() { return g_L; }

}  // namespace script
