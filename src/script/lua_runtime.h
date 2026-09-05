#pragma once
#include <stddef.h>

// Lua runtime lifecycle for the payload. Owns a single lua_State, opens the safe
// standard libs + the native bridge, and exposes a per-frame tick that drives
// script-registered callbacks (on_tick) with per-callback error isolation.
//
// This unit is host-testable: it never touches the filesystem or any ORBIS API.
// Scripts are handed in as buffers (runtime_load_buffer); the console-side file
// loader lives separately and calls into here.

struct lua_State;

namespace script {

using log_sink_fn = void (*)(const char* msg);

// Route runtime/script log output (log/print, load & tick errors) to a sink.
void runtime_set_log_sink(log_sink_fn fn);

// Route notify(...) -- the explicit on-screen channel -- to a sink. Separate
// from the log sink on purpose, and the separation is the point: log/print
// used to raise one PS4 notification per line, so any script that logged on a
// timer (both shipped hello examples do) kept the notification queue full and
// buried the plugin's own load/ready/FAILED messages behind a backlog. Output
// is now the quiet default and the screen is opted into.
//
// A notified line is ALSO logged, so nothing said on screen goes unrecorded --
// which is what lets the notification be cut to fit the display without losing
// anything.
void runtime_set_notify_sink(log_sink_fn fn);

// Create the lua_State, open safe libs, install the native bridge and the
// on_tick/log/print globals. Idempotent. Returns false on allocation failure.
bool runtime_init();

// Load and run one chunk from a buffer. Errors are logged (not fatal); returns
// false if the chunk failed to compile or raised during its top-level run.
bool runtime_load_buffer(const char* name, const char* src, size_t len);

// What kind of thing runtime_exec_capture failed at, for a caller that needs
// to react differently (runtime_load_buffer tags its log line with this;
// on_exec does not need it and passes nullptr). Deliberately returned through
// an out-parameter rather than file-static "last error" state, which two
// concurrent callers would stomp on each other's.
enum exec_kind {
    EXEC_SUCCEEDED = 0,
    EXEC_NOT_INITIALISED,   // g_L is null
    EXEC_COMPILE_ERROR,     // luaL_loadbuffer failed
    EXEC_RUNTIME_ERROR,     // the chunk loaded but lua_pcall raised
};

// Appended to a result that had to be cut, so a client holding nothing but
// the reply bytes can still tell a whole value from a piece of one -- the
// same rule on_ls and resource_list_pack follow for their own replies.
constexpr const char* EXEC_RESULT_TRUNC_MARK = "\n-- truncated: result incomplete --";

// Where runtime_exec_capture puts what the chunk RETURNED, for the caller
// that needs it: the control channel's EXEC reply, which the editor's console
// uses as a REPL -- "=1+1" has to answer 2, not "ok" (spec 4.2: "reply carries
// the result or the error"). Pass nullptr (the default) and the results are
// discarded, exactly as they were before this existed.
//
// `buf` and `buf_len` are inputs the caller supplies; every other field is
// written by the call. A result too long for `buf` is cut, but never
// silently: the text ends with EXEC_RESULT_TRUNC_MARK so a client that only
// ever sees bytes can tell, and `truncated` says the same thing to a caller
// that can read the struct.
struct exec_result {
    char*  buf;          // in:  caller's buffer; NUL-terminated on return
    size_t buf_len;      // in:  its capacity, NUL included
    int    nresults;     // out: how many values the chunk returned (0 = none)
    size_t len;          // out: bytes written to buf, NUL excluded
    bool   truncated;    // out: the text did not fit and was cut
    bool   value_error;  // out: rendering the values raised (a __tostring
                         //      metamethod is resource-written Lua and may
                         //      error); buf then holds that error text
                         //      instead. The chunk itself still ran to
                         //      completion, so the call still returns true --
                         //      reporting it as an exec failure would blame
                         //      the chunk for something it did not do.
};

// Load and run one chunk, writing any compile or runtime error into `err`
// (NUL-terminated, truncated to err_len). Returns false on error. If `kind`
// is non-null, it is set to which of exec_kind above happened. This is what
// the control channel needs: runtime_load_buffer only logs the message, and a
// client waiting on a reply needs the text itself.
//
// If `result` is non-null the chunk's return values are rendered into it (see
// exec_result above); otherwise they are dropped on the floor.
bool runtime_exec_capture(const char* name, const char* src, size_t len,
                          char* err, size_t err_len, int* kind = nullptr,
                          exec_result* result = nullptr);

// Convenience: load+run a NUL-terminated source string (e.g. a menu action
// like "request_spawn('adder')"). Runs on the caller's thread.
bool runtime_exec(const char* name, const char* src);

// Call every registered on_tick callback once. A callback that raises is logged
// and removed so it cannot spam every frame; the rest keep running.
void runtime_tick();

// Destroy the lua_State. Safe to call when uninitialised.
void runtime_shutdown();

// The live lua_State (null before init) — for the console file loader.
lua_State* runtime_state();

}  // namespace script
