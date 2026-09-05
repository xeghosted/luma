---@meta
-- LuaLS definitions for the GTALua runtime API. Hand-written: unlike the
-- natives, this surface is small and changes when we change it — and unlike
-- natives.def.lua, it cannot be regenerated. Whenever `bridge_open()` in
-- src/script/native_bridge.cpp, or the globals set up in
-- src/script/lua_runtime.cpp, gain, lose or reshape a global, update this
-- file to match, function-for-function and constant-for-constant. It is the
-- only thing keeping this file honest.

-- --- lifecycle -------------------------------------------------------------

---Run `fn` every frame on the game thread. A raising callback is logged once
---and removed; the rest keep running.
---@param fn fun()
function on_tick(fn) end

---Write to gtalua.log and an on-screen notification. Every argument is
---stringified (`tostring`-style) and joined with tabs, so any value works,
---not just a single string.
---@vararg any
function log(...) end

---Same C function as `log` — see above.
---@vararg any
function print(...) end

---Write to gtalua.log AND raise a PS4 on-screen notification. This is the
---opt-in: `log` and `print` deliberately stay off the screen, because one
---notification per logged line fills a queue that never drains and buries
---the plugin's own load messages behind it.
---@vararg any
function notify(...) end

-- --- native registry ---------------------------------------------------

---Has the hash registry been walked and verified? RVA-bound natives work
---regardless of this; it only gates the hash-bound half.
---@return boolean
function natives_ready() end

---Does this build register a native at `hash`, whether or not it has been
---called yet?
---@param hash integer
---@return boolean
function native_exists(hash) end

-- --- calling and binding natives yourself -------------------------------
--
-- natives.def.lua's 6,487 generated bindings cover the normal case; these
-- are the primitives they are built from, for a native this build does not
-- name or a one-off call you don't want a permanent binding for.

---Bind a native at `rva` (by implementation address) into a reusable
---callable, parsing `sig` once up front rather than on every call — this is
---what every generated natives.def.lua binding compiles down to.
---A signature is one character per slot, return type first: `v` void,
---`i` int, `f` float, `b` bool, `s` string, `p` pointer, `3` vector3.
---@param rva integer
---@param sig string
---@return fun(...): any
function native(rva, sig) end

---Same as `native`, but reached by 64-bit hash through the registry
---instead of by RVA. Raises if the registry is not ready or does not
---register `hash`, rather than silently returning 0.
---@param hash integer
---@param sig string
---@return fun(...): any
function native_hash(hash, sig) end

---One-off call to the native at `rva`, with explicit types: `types` is
---`{ret_tag, arg_tag, arg_tag, ...}` built from the `ARG_*` constants below,
---and the trailing varargs are the native's own arguments.
---@param rva integer
---@param types integer[]
---@vararg any
---@return any
function invoke(rva, types, ...) end

---Same as `invoke`, but reached by 64-bit hash instead of by RVA.
---@param hash integer
---@param types integer[]
---@vararg any
---@return any
function invoke_hash(hash, types, ...) end

---One-off call to the native at `rva` with argument types inferred from the
---Lua values passed. Always returns a plain 32-bit int — RAGE natives only
---ever write a 32-bit return, so a float- or vector-returning native needs
---`invoke` (explicit `ARG_FLOAT`/`ARG_VEC3`) or a natives.def.lua binding
---instead of this.
---@param rva integer
---@vararg any
---@return integer
function invoke_raw(rva, ...) end

---Same as `invoke_raw`, but reached by 64-bit hash instead of by RVA.
---@param hash integer
---@vararg any
---@return integer
function invoke_hash_raw(hash, ...) end

-- --- argument/return tags for invoke()/invoke_hash()/native()/native_hash() -
--
-- Values match src/script/native_marshal.h's arg_tag enum exactly (VOID=0
-- .. PTR=6) - these are the real runtime values, not placeholders.

---@type integer
ARG_VOID = 0
---@type integer
ARG_INT = 1
---@type integer
ARG_FLOAT = 2
---@type integer
ARG_BOOL = 3
---@type integer
ARG_VEC3 = 4
---@type integer
ARG_STRING = 5
---@type integer
ARG_PTR = 6

-- --- memory ----------------------------------------------------------------

---Zeroed scratch memory for out-parameters, capped at 64 KiB. Returns nil
---only if the underlying allocation itself fails (an out-of-range size
---raises instead).
---@param n integer
---@return integer? address
function mem_alloc(n) end

---@param address integer
function mem_free(address) end

---Read `size` bytes (1, 2, 4 or anything else -> 8; default 8) at `address`
---as an unsigned integer.
---@param address integer
---@param size? integer
---@return integer
function read_mem(address, size) end

---Write the low `size` bytes (1, 2, 4 or anything else -> 8; default 8) of
---`value` to `address`.
---@param address integer
---@param value integer
---@param size? integer
function write_mem(address, value, size) end

---@param address integer
---@return number
function read_float(address) end

---@param address integer
---@param value number
function write_float(address, value) end

---Read a NUL-terminated C string at `address`. Returns nil if `address` is
---0 (a null pointer), not on any other failure.
---@param address integer
---@return string? value
function read_string(address) end

---Absolute address of an eboot offset (`g_eboot_base + offset`).
---@param offset integer
---@return integer
function rva(offset) end

-- --- RAGE script globals ----------------------------------------------------

---@param index integer
---@return integer
function script_global(index) end

---@param index integer
---@param value integer
function set_script_global(index, value) end

-- --- memory: Vector3 out-parameters ----------------------------------------

---Read a Vector3 out-parameter written by a native.
---
---The game stores x, y and z at an **8-byte stride**, not packed, so such a
---buffer is `mem_alloc(24)` and NOT 12 bytes, and reading `p + 4` as y gives
---garbage. This function spells that layout once and returns the same shape a
---Vector3 *return* arrives in.
---@param address integer
---@return number[] xyz `{x, y, z}`
function read_vector3(address) end

-- --- native registry, continued --------------------------------------------

---Where the game's own dispatch table resolves `hash` to, or 0 when this build
---does not register it. This is the game's answer, not ours, which is what
---makes it the authority when auditing an RVA (see
---tools/native_crosscheck_offline.py).
---@param hash integer
---@return integer address 0 when the hash is not registered
function native_addr(hash) end

-- --- the resource API ------------------------------------------------------
--
-- Everything below exists only inside a RESOURCE (a directory under
-- /data/gtalua/resources with an fxmanifest.lua). A plain script in
-- /data/gtalua/scripts gets the globals above and `on_tick`, but not these:
-- they are installed per resource so that stopping one can undo every
-- registration it made. See src/script/lua/boot.lua.

---Start a coroutine that the scheduler resumes once per frame. The body runs
---on the game thread, so it may call natives — and must `Wait()` rather than
---loop, or the runaway budget kills it.
---@param fn fun()
---@return integer? handle nil if the resource is shutting down
function CreateThread(fn) end

---Like `CreateThread`, but runs the body immediately, up to its first `Wait`.
---@param fn fun()
---@return boolean ok false if the resource is shutting down
function CreateThreadNow(fn) end

---Yield for at least `ms` milliseconds. Only legal inside a thread: a `Wait`
---at the top level of a script has nothing to park in and is refused.
---@param ms integer
function Wait(ms) end

---Run `fn` once, after at least `ms` milliseconds.
---@param ms integer milliseconds FIRST, as in FiveM
---@param fn fun()
---@return integer? handle nil if the resource is shutting down
function SetTimeout(ms, fn) end

---Cancel a pending `SetTimeout`, or stop a `CreateThread` thread.
---@param handle integer
function ClearTimeout(handle) end

---The FiveM-compatible aliases. Same functions, reachable under the name
---pasted code expects.
---@class Citizen
---@field CreateThread fun(fn: fun()): integer?
---@field CreateThreadNow fun(fn: fun()): boolean
---@field Wait fun(ms: integer)
---@field SetTimeout fun(ms: integer, fn: fun()): integer?
---@field ClearTimeout fun(handle: integer)
Citizen = {}

---The name of the resource this script belongs to.
---@return string
function GetCurrentResourceName() end

---Register a handler for an event. The handle it returns is what
---`RemoveEventHandler` takes; the registration is also recorded against this
---resource, so stopping the resource removes it.
---@param name string
---@param cb fun(...)
---@return table? handle nil if the resource is shutting down
function AddEventHandler(name, cb) end

---@param handle table the value AddEventHandler returned
function RemoveEventHandler(handle) end

---Call every handler registered for `name`, in registration order. A handler
---that raises is logged and skipped; the rest still run.
---@param name string
---@vararg any
function TriggerEvent(name, ...) end

---There is no network on this console. This exists so pasted FiveM code runs:
---with a callback it registers an ordinary local handler, without one it does
---nothing at all.
---@param name string
---@param cb? fun(...)
---@return table? handle
function RegisterNetEvent(name, cb) end

---Define an export: `exports('where', fn)`.
---Call another resource's: `exports.other.where()` or `exports.other:where()`.
---Both forms work, and an export is a direct function reference — one
---lua_State means there is no marshalling boundary to cross.
---@overload fun(name: string, fn: function)
---@type table<string, table<string, function>>
exports = {}

---Register a console command. The PS4 has no chat box, but the control channel
---is one: a line typed in the editor's GTALua terminal reaches `handler`.
---`source` is always 0, `args` is the line split on spaces, `raw` is the line.
---@param name string
---@param handler fun(source: integer, args: string[], raw: string)
---@return table? handle nil if the resource is shutting down
function RegisterCommand(name, handler) end

---One of "started", "starting", "stopped", "stopping", "error" or "missing".
---@param name string
---@return string
function GetResourceState(name) end

---@param name string
---@return boolean ok
function StartResource(name) end

---@param name string
---@return boolean ok
function StopResource(name) end
