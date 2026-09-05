-- src/script/lua/resource.lua
-- Resource records, the per-resource sandbox, and the one registration factory
-- everything must go through.
--
-- Isolation is one lua_State with a per-resource _ENV whose __index falls
-- through to the shared globals: reads see the stdlib, the natives and the API;
-- writes land in the resource's own table. Stop is "walk the record and undo
-- it", which is only correct if every registration was written into the record
-- in the first place -- hence track(), and hence no second path.

local rt = __rt

local R = {}
local records = {}      -- name -> record
-- Handed in by the host: the data root differs per game, and one embedded
-- copy of this chunk has to serve both.
local ROOT = rt.resource_root

local function host() return rt.host end
local function report(msg)
    local log = rt.log
    if log then log(msg) end
end

-- The single registration factory. Every API that registers something for a
-- resource calls this and nothing else; stop() undoes exactly what it recorded.
--
-- A dead record refuses. Nothing sweeps a torn-down record's buckets ever
-- again, so anything recorded here after teardown is recorded into a table
-- no stop will revisit. This is the LAST line of that defence, not the first:
-- every real caller is make_env, which refuses before the underlying
-- registration happens (refusing only here would leave the command already
-- in the global table with nothing holding a token for it). If this branch
-- ever fires it means a registration path was added without its own guard --
-- which is precisely what the log line is for.
function R.track(rec, kind, entry)
    if rec.dead then
        report("[" .. tostring(rec.name) .. "] refusing to track a " .. tostring(kind) ..
               " registration: this resource has already been torn down")
        return nil
    end
    local bucket = rec[kind]
    bucket[#bucket + 1] = entry
    return entry
end

function R.get(name) return records[name] end

-- Whether the record currently held under this NAME has been torn down --
-- which is a different question from "is this record dead", and the weaker
-- one. Everything the scheduler owns now carries the record itself (boot.lua's
-- make_env closes over it), so it asks by identity and this is only its
-- fallback for a plain string owner; see owner_dead in scheduler.lua for the
-- one shape the two answers disagreed on. make_env's own guards have always
-- read rec.dead directly, for the same reason.
function R.is_dead(name)
    local rec = records[name]
    return rec ~= nil and rec.dead == true
end

local function resource_dir(name) return ROOT .. "/" .. name end

-- Must equal net::RESOURCE_NAME_MAX (src/net/dispatch.h). The control
-- channel's lifecycle ops decode a name into a buffer of that size and refuse
-- anything longer, so without the same bound here a longer directory was
-- listed by R.list as "stopped" -- offered as startable -- while START, STOP,
-- RESTART and RESINFO every one of them refused it. The runtime must not
-- advertise what it cannot act on. Bound to the C++ constant through
-- tests/protocol_vectors.json, which both suites assert against.
local NAME_MAX = 127

-- A resource name becomes a path segment (resource_dir above concatenates it
-- straight into ROOT .. "/" .. name), so it must be exactly that: one
-- directory name, nothing that could turn into a traversal once concatenated.
-- Checked here, at every entry point that takes a name, rather than trusted
-- to the caller: today only autostart.cfg (a file the console owner writes)
-- supplies one, but a name reaching here from the control channel is not
-- vetted before it arrives, and a bare ".." or a name containing a separator
-- resolves outside the resource directory the same way an unguarded
-- file path would. A bare "." is rejected too: it stays inside ROOT rather
-- than escaping it, but names nothing real, and a resource that "started"
-- would be confusing rather than dangerous. Rejected outright rather than
-- sanitised -- a caller that gets a clear error can retry with a real name;
-- one that got a silently edited name would not know its request changed.
local function valid_name(name)
    return type(name) == "string" and name ~= "" and name ~= ".." and name ~= "."
        and #name <= NAME_MAX
        and not name:find("/", 1, true) and not name:find("\\", 1, true)
end

-- Third return is whether fxmanifest.lua itself was found: a missing manifest
-- and a manifest that was found but failed to parse are different states
-- (R.state must answer "missing" for the former and "error" for the latter,
-- see R.start below), and both look identical from `not m` alone.
local function read_manifest(name)
    local src = host().read_file(resource_dir(name) .. "/fxmanifest.lua")
    if not src then return nil, "no fxmanifest.lua", false end
    local m, err = rt.manifest.parse(src, name .. "/fxmanifest.lua")
    return m, err, true
end

-- Declared here, defined below teardown (which it calls). R.state sits above
-- both because it is part of the module's public shape, not because it runs
-- first.
local settle

function R.state(name)
    if not valid_name(name) then return "missing" end
    local rec = records[name]
    if rec then settle(rec); return rec.state end
    local src = host().read_file(resource_dir(name) .. "/fxmanifest.lua")
    return src and "stopped" or "missing"
end

-- True if `p` is absolute or contains ".." as a whole path segment -- mirrors
-- net::path_guard's rule on the C++ side (src/net/path_guard.cpp), which has
-- confined file access from the console side since M1. The Lua side had no
-- equivalent: a manifest entry is written by whoever wrote the resource, and
-- without this a script directive like '../../GoldHEN/plugins.ini' gets
-- concatenated straight into the read path and read (and run) as Lua. As with
-- the C++ rule, "..hidden.lua" is a filename, not an escape -- only a segment
-- that IS exactly ".." counts.
local function is_unsafe_path(p)
    if p == "" or p:sub(1, 1) == "/" then return true end
    local start = 1
    while true do
        local slash = p:find("/", start, true)
        local seg = slash and p:sub(start, slash - 1) or p:sub(start)
        if seg == ".." then return true end
        if not slash then return false end
        start = slash + 1
    end
end

-- Expand a manifest entry, which may contain a '*' in its last segment. The
-- unsafe-path check runs on the whole pattern before either branch, which
-- also covers the glob branch's directory prefix: '../*.lua' is rejected
-- before list_dir ever sees the parent directory, not just a literal
-- '../x.lua' entry. It runs again on each entry the glob branch matches,
-- separately from the pattern check above: a pattern can be perfectly safe
-- while a name a real directory listing hands back is not (list_dir is not
-- guaranteed to omit '.' and '..' -- R.list() below already has to filter
-- those itself for exactly that reason -- and a future host is one detail
-- away from handing either straight through).
local function expand(name, pattern)
    if is_unsafe_path(pattern) then
        report("[" .. name .. "] " .. pattern .. ": path escapes the resource directory, ignoring")
        return {}
    end
    if not pattern:find("*", 1, true) then return { pattern } end
    local dir, glob = pattern:match("^(.*)/([^/]*)$")
    if not dir then dir, glob = "", pattern end
    local listing = host().list_dir(resource_dir(name) .. (dir ~= "" and ("/" .. dir) or ""))
    local out = {}
    if listing then
        table.sort(listing)
        for _, entry in ipairs(listing) do
            if entry ~= "." and entry ~= ".." and rt.manifest.match_glob(glob, entry) then
                local rel = (dir ~= "" and (dir .. "/") or "") .. entry
                if is_unsafe_path(rel) then
                    report("[" .. name .. "] " .. rel .. ": path escapes the resource directory, ignoring")
                else
                    out[#out + 1] = rel
                end
            end
        end
    end
    return out
end

local function new_record(name)
    return {
        name = name, state = "stopped",
        threads = {}, handlers = {}, exports = {}, commands = {},
        env = nil,
    }
end

-- Undo everything the record holds. Deliberately tolerant: teardown runs on the
-- failure path too, where the record may be only partly filled.
local function teardown(rec)
    -- Dead FIRST, before anything is swept. A resource can stop itself from
    -- inside one of its own callbacks, and that callback's coroutine is in
    -- neither the scheduler's `threads` nor its `staged` while it runs -- so
    -- kill_owner below cannot see it, and it comes back after this function
    -- returns, still holding rec.env. Marking the record here is what lets
    -- every registration path (make_env's guards, R.track above, the
    -- scheduler's owner_dead) refuse it from this instant on; setting the
    -- flag after the sweep would leave exactly the window that lets a zombie
    -- body re-register into the global command table.
    rec.dead = true
    rt.sched.kill_owner(rec)
    if rt.events then
        for _, entry in ipairs(rec.handlers) do rt.events.remove(entry) end
    end
    if rt.exports_registry then rt.exports_registry[rec.name] = nil end
    if rt.commands then
        for _, cmd in ipairs(rec.commands) do rt.commands.remove(cmd) end
    end
    rec.threads, rec.handlers, rec.exports, rec.commands = {}, {}, {}, {}
    rec.env = nil
end

-- Every enter() hands out one of these, cut from a counter that only ever
-- counts up and is shared by every record, and open_ops[token] is true for
-- exactly as long as that specific enter()/leave() bracket is on the stack.
-- See settle() below for why a ticket replaced a coroutine handle, and why
-- membership in this set rather than a plain "still the newest one" compare.
local next_op_token = 0
local open_ops = {}

-- Mark a record as being inside a lifecycle call, and remember WHICH one.
local function enter(rec, state)
    next_op_token = next_op_token + 1
    rec.op_token = next_op_token
    open_ops[rec.op_token] = true
    rec.state = state
end

local function leave(rec, state)
    open_ops[rec.op_token] = nil
    rec.op_token = nil
    rec.state = state
end

-- A transient state is only true while the call that set it is still on the
-- stack. This decides whether it is, and finishes the job when it is not.
--
-- Used to ask coroutine.status(rec.op_co) instead of comparing a token. That
-- was wrong, not just narrow: a lifecycle call driven straight from C++ runs
-- on the MAIN coroutine, and coroutine.status of the one coroutine you are
-- currently running on, asked from a point that is not nested inside any
-- other coroutine, is "running" -- always, unconditionally, whether or not
-- the call that set rec.state is still the one on the stack. That is a
-- tautology, not a check: op_co being the main coroutine made this function
-- unable to ever answer "gone", so a record left "starting"/"stopping" by a
-- call that died on the main coroutine (an exhausted stack, an allocation
-- failure) stayed that way forever -- and BOTH R.start and R.stop refuse a
-- transient record, so "forever" meant unstartable for the rest of the
-- session, contrary to what the comment here used to promise.
--
-- op_token has no such alias: it is a ticket cut from a counter that never
-- resets and never repeats, so membership in open_ops is a real question with
-- a real answer instead of one that always comes back the same way regardless
-- of which physical coroutine happens to be asking. Membership, not "is it
-- still the newest ticket issued": mutA and mutB starting each other (see the
-- C2 A/B pair section in reentrancy_test.lua) is a call for a DIFFERENT
-- record genuinely nested inside this one, and it hands out a newer ticket
-- while mutA's own call is still legitimately on the stack -- "newest ticket"
-- would call mutA stale right then and tear it down mid-start, which is
-- exactly the unbounded-recursion bug the in-progress guard exists to
-- prevent, just reintroduced from the settle side. Set membership does not
-- have that failure: mutA's ticket stays a member for as long as mutA's own
-- enter()/leave() bracket is open, no matter how many unrelated tickets are
-- handed out and closed while it waits, because leave() only ever clears the
-- one entry its own token names.
--
-- This is the backstop under R.stop/R.start's own pcalls, not a replacement
-- for them: a pcall only helps if the statement after it gets to run, and the
-- instruction budget's hook re-raises on every instruction once it has
-- escalated (scheduler.lua).
function settle(rec)
    local was = rec.state
    if was ~= "starting" and was ~= "stopping" then return end
    if rec.op_token and open_ops[rec.op_token] then return end
    report("[" .. tostring(rec.name) .. "] was left \"" .. was ..
           "\" by a call that is no longer running; completing its teardown")
    pcall(teardown, rec)
    leave(rec, was == "starting" and "error" or "stopped")
end

-- "starting" and "stopping" are transient states that exist for exactly one
-- purpose: to be visible to a re-entrant call. Everything here runs on the
-- game thread inside the frame hook, single-threaded and synchronous, so the
-- ONLY way anybody observes one of them is from inside code that start/stop
-- is itself running -- a script body, an onResourceStart/onResourceStop
-- handler, a command handler. That is the case they refuse. Nothing on the
-- wire can ever see one: a lifecycle call has always reached a terminal
-- state by the time it returns to C++.
--
-- Refused with a real `false, reason`, never swallowed. Before this guard,
-- StopResource(GetCurrentResourceName()) from inside onResourceStop recursed
-- ~200 levels until the C stack gave out, and R.stop still returned true, so
-- the console was told "OK stopped r". Reporting an unbounded recursion as a
-- success is the failure; the recursion is only how it got there.
local function in_progress(rec)
    settle(rec)
    if rec.state == "starting" then return "already starting" end
    if rec.state == "stopping" then return "currently stopping" end
    return nil
end

-- Run every script file into the record's env, in manifest order. Answers
-- true, or nil plus the message the caller should report.
--
-- Deliberately does NOT touch rec.state and does NOT tear down: R.start owns
-- the transient window, and "every path out of it lands on a terminal state,
-- including a raise" is only checkable while exactly one place does that.
local function run_scripts(rec, name, files)
    for _, rel in ipairs(files) do
        local path = resource_dir(name) .. "/" .. rel
        local src = host().read_file(path)
        if not src then return nil, "missing file: " .. rel end
        local chunk, lerr = load(src, "@" .. name .. "/" .. rel, "t", rec.env)
        if not chunk then return nil, tostring(lerr) end
        -- Run under the same instruction budget manifest evaluation has had
        -- since Task 4, which means running in a coroutine: a debug hook can
        -- only be armed on a thread. The bare pcall(chunk) this replaces had
        -- no bound at all, so `while true do end` at the top of client.lua
        -- froze the console outright -- the exact failure the manifest budget
        -- exists to prevent, one function later.
        --
        -- coroutine.resume subsumes the pcall: it returns false, message for
        -- a raising chunk exactly as pcall did. A chunk that YIELDS rather
        -- than finishing is its own failure, refused the same way and for
        -- the same reason manifest.lua refuses a yielding manifest: a
        -- top-level Wait() has no thread to park in, and a suspended body
        -- nothing tracks is a leak with no owner. It failed before this
        -- change too ("attempt to yield from outside a coroutine"), so
        -- nothing that used to work stops working.
        --
        -- Both messages name the file. The budget error carries no position
        -- of its own -- the instruction that happens to trip the counter says
        -- nothing about where the loop is -- and "which of my five scripts is
        -- spinning" is the only question worth answering here. It costs a
        -- little redundancy on the errors that do carry a position.
        local co = coroutine.create(chunk)
        rt.sched.arm_budget(co)
        local ok, rerr = coroutine.resume(co)
        if not ok then return nil, rel .. ": " .. tostring(rerr) end
        if coroutine.status(co) ~= "dead" then
            return nil, rel .. ": script yielded at top level (a Wait() outside a thread?)"
        end
    end
    return true
end

function R.start(name)
    if not valid_name(name) then return false, "invalid resource name" end
    local rec = records[name]
    if rec then
        local busy = in_progress(rec)
        if busy then return false, busy end
        if rec.state == "started" then return false, "already started" end
    end

    local m, err, present = read_manifest(name)
    if not m then
        if present then
            -- fxmanifest.lua exists but failed to parse (a syntax error, or an
            -- exhausted instruction budget): record the failure so state()
            -- returns "error" rather than falling back to its disk probe,
            -- which would otherwise report "stopped" for a resource that is
            -- actually broken -- a later task's editor tree shows this string
            -- to the user verbatim.
            records[name] = new_record(name)
            records[name].state = "error"
        end
        return false, err or "manifest failed to parse"
    end
    for _, w in ipairs(m.warnings) do report("[" .. name .. "] " .. w) end

    rec = new_record(name)
    records[name] = rec
    rec.env = rt.make_env(rec)

    local files = {}
    for _, pattern in ipairs(m.scripts) do
        for _, f in ipairs(expand(name, pattern)) do files[#files + 1] = f end
    end
    if #files == 0 then
        -- Through teardown like every other failure past make_env. Nothing
        -- above here has run a line of resource code, so there is nothing
        -- registered to undo -- but rec.env is already built, and this was
        -- the ONE exit that left it alive on a record no stop will ever
        -- revisit. Being the harmless-looking case is exactly why it was the
        -- one that got missed.
        teardown(rec); rec.state = "error"
        return false, "manifest lists no script files"
    end

    -- Armed as late as possible, and unconditionally cleared below: nothing
    -- above this line can run a line of resource code, and every exit path
    -- past it -- a missing file, a chunk that fails to compile, a chunk that
    -- raises, a chunk that runs away or yields, a raise from anywhere at all,
    -- and success -- assigns a terminal state before returning. A start that
    -- failed while leaving the record wedged in "starting" would make the
    -- resource permanently unstartable, which is a worse bug than the one
    -- this guard fixes.
    enter(rec, "starting")

    -- Under pcall because "every exit path" has to include the ones nobody
    -- listed. run_scripts calls out to host().read_file, to load(), and to
    -- resource-written code; the file it runs can install a metamethod that
    -- raises on the next table access here. None of that is allowed to skip
    -- the terminal state below. What is caught is reported, never swallowed:
    -- it comes back as this start's own failure reason.
    local pok, ran, why = pcall(run_scripts, rec, name, files)
    if not pok then ran, why = nil, "start raised: " .. tostring(ran) end
    if not ran then
        pcall(teardown, rec)
        leave(rec, "error")
        return false, why
    end

    leave(rec, "started")
    -- After the terminal state, so a handler that re-enters sees "started"
    -- and is refused by the ordinary rule rather than by the transient guard
    -- -- and so a raise out of the dispatch cannot wedge anything. It is
    -- still pcall'd: R.start is called from C++ and from resource code, and
    -- neither is a place for an onResourceStart handler's error to surface as
    -- a raise. Reported, the same way create_now reports a thread error.
    if rt.events then
        local eok, eerr = pcall(rt.events.trigger, "onResourceStart", name)
        if not eok then
            report("[" .. name .. "] onResourceStart dispatch failed: " .. tostring(eerr))
        end
    end
    return true
end

function R.stop(name)
    if not valid_name(name) then return false, "invalid resource name" end
    local rec = records[name]
    if not rec then return false, "not started" end
    local busy = in_progress(rec)
    if busy then return false, busy end
    if rec.state ~= "started" then return false, "not started" end
    -- Before the trigger, not after it. onResourceStop handlers run while
    -- this stop is still in flight, and a handler that stops its own resource
    -- re-enters here: the guard above has to already be true by then, or it
    -- never fires at all. That ordering IS the fix -- moving this line below
    -- the trigger restores the recursion exactly.
    enter(rec, "stopping")

    -- Both halves under their own pcall, and the terminal state after both,
    -- unconditionally. A raise anywhere in this window used to escape past
    -- `rec.state = "stopped"` and leave the record in "stopping", which
    -- R.start and R.stop BOTH refuse -- an unstartable resource for the rest
    -- of the session, out of a single bad handler. (The known raise came from
    -- events.trigger walking a list a nested teardown was mutating; api.lua
    -- fixes that one. These pcalls are for the ones nobody has found yet.)
    --
    -- Teardown separately from the trigger, not inside it: a handler that
    -- raised must not also cost the resource its sweep. Before this, such a
    -- raise skipped teardown entirely and left every thread, handler and
    -- command of a "stopped" resource still registered.
    local dok, derr = true, nil
    if rt.events then dok, derr = pcall(rt.events.trigger, "onResourceStop", name) end
    local tok, terr = pcall(teardown, rec)
    leave(rec, "stopped")

    -- ok stays true. The stop itself succeeded -- the resource is stopped and
    -- swept -- so answering false would be a second lie on top of the first,
    -- and would break R.restart, which reads that first value alone to decide
    -- whether to go on and start (see R.restart below). This is the same
    -- ruling commands.run already makes for a raising command handler, and
    -- the same channel create_now reports a thread error on.
    --
    -- A caller that DOES look past the first value should not have to go read
    -- the log channel to learn a handler failed, though, so the second value
    -- carries it -- costs nothing here, since every other caller of R.stop
    -- (R.restart, and StopResource's forwarding in boot.lua) either ignores
    -- it or, like R.restart, only ever inspects it on the false path this
    -- never takes.
    local err
    if not dok then
        report("[" .. name .. "] onResourceStop dispatch failed: " .. tostring(derr))
        err = "onResourceStop dispatch failed: " .. tostring(derr)
    end
    if not tok then
        report("[" .. name .. "] teardown failed: " .. tostring(terr))
        err = err and (err .. "; teardown failed: " .. tostring(terr))
            or ("teardown failed: " .. tostring(terr))
    end
    return true, err
end

-- restart is stop-then-start, and the in-progress guards are what make that
-- safe rather than what breaks it: R.stop's last act is rec.state =
-- "stopped", which is precisely the state R.start accepts. A restart that is
-- itself re-entrant (called from inside a lifecycle callback) sees "starting"
-- or "stopping" here, skips the stop -- the state is not "started" -- and
-- hands the refusal back from R.start with its reason intact.
function R.restart(name)
    if not valid_name(name) then return false, "invalid resource name" end
    if R.state(name) == "started" then
        local ok, err = R.stop(name)
        if not ok then return false, err end
    end
    return R.start(name)
end

function R.info(name)
    if not valid_name(name) then return nil end
    local rec = records[name]
    if not rec then return nil end
    -- Settled first, same as R.state: a record stuck "starting"/"stopping" by
    -- a call that has since died still holds its pre-teardown thread/handler/
    -- export/command counts until something settles it, and this must not
    -- hand those stale counts to a caller when R.state, asked in the same
    -- breath, would already report the record torn down.
    settle(rec)
    return {
        threads = #rec.threads, handlers = #rec.handlers,
        exports = #rec.exports, commands = #rec.commands,
    }
end

function R.list()
    local out = {}
    local seen = {}
    -- nil from list_dir is a FAILED listing, not an empty directory, and the
    -- difference is what makes the prune below safe. With the old `or {}`
    -- swallowing it, one unreadable ROOT would look exactly like "every
    -- resource directory was deleted at once" and forget every record there
    -- is. An empty table is an answer; nil is the absence of one.
    local listing = host().list_dir(ROOT)
    local listed_ok = listing ~= nil
    listing = listing or {}
    table.sort(listing)
    for _, name in ipairs(listing) do
        if name ~= "." and name ~= ".." then
            out[#out + 1] = { name = name, state = R.state(name) }
            seen[name] = true
        end
    end
    for name, rec in pairs(records) do
        -- Unlike the loop above (which goes through R.state, and so already
        -- settles), a record with no directory entry has no other path
        -- through here -- settle it directly, or a "starting"/"stopping" left
        -- behind by a call that has since died would show up on the wire and
        -- in the editor tree exactly like a genuinely in-flight one.
        if not seen[name] then
            settle(rec)
            -- The directory is gone. A record in a TERMINAL state holds
            -- nothing by this point -- teardown has emptied its four tables
            -- and dropped its env -- so forgetting it loses no information,
            -- and it is the only way the listing ever shrinks again: there is
            -- no wire op to forget one by hand, so a console that had
            -- resources deleted underneath it went on listing them until the
            -- game was restarted.
            --
            -- A record that is still "started", or mid-lifecycle, is NOT
            -- forgotten: its chunk is loaded and its threads are running, and
            -- hiding a resource that is still executing would be a worse lie
            -- than listing one whose directory someone deleted underneath it.
            --
            -- Setting an existing key to nil during pairs() is explicitly
            -- allowed by the Lua manual -- only ADDING keys mid-traversal is
            -- undefined -- so this prunes in place rather than needing a
            -- second pass over a collected list.
            if listed_ok and (rec.state == "stopped" or rec.state == "error") then
                records[name] = nil
            else
                out[#out + 1] = { name = name, state = rec.state }
            end
        end
    end
    return out
end

rt.res = R
return R
