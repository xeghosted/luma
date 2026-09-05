-- src/script/lua/scheduler.lua
-- The coroutine scheduler. Wait(ms) yields the DELAY and the scheduler computes
-- the wake time, matching FiveM's own implementation
-- (code/components/citizen-scripting-lua/src/LuaScriptRuntime.cpp: Lua_Wait
-- pushes its argument and yields it; SetTimeout takes the delay FIRST).
--
-- Threads created during a tick are staged and run on the NEXT tick. That is
-- not a detail: without it, a CreateThread from inside a running callback
-- mutates the table being iterated.

-- __rt is created by the C++ layer before any chunk loads, and removed from
-- _G once boot.lua has run. Bind it locally NOW: report() below reaches for
-- rt.log at runtime, long after the global is gone.
local rt = __rt

local co_create, co_resume, co_status, co_yield, co_running =
    coroutine.create, coroutine.resume, coroutine.status, coroutine.yield, coroutine.running
local traceback = debug.traceback
local sethook = debug.sethook

local threads = {}      -- handle -> { co, wake, owner }
local staged = {}       -- handle -> same, promoted at the top of the next tick
local next_handle = 1
local now = 0

local S = {}

local function report(err)
    local log = rt.log
    if log then log(err) end
end

-- The instruction budget. Every coroutine this runtime resumes -- a scheduler
-- thread, an event or command callback, a resource's own script body, an
-- fxmanifest -- runs on the game thread inside the frame hook, and none of
-- them may run forever. Manifest evaluation has been budgeted since Task 4
-- for exactly that reason; the constant and the mechanism live here, once,
-- so manifest.lua (which reads them back off rt.sched) and resource.lua
-- share one number rather than drifting apart.
--
-- PER RESUME SLICE, never cumulative. debug.sethook resets a coroutine's
-- instruction counter every time it is called, so arming immediately before
-- each resume is what makes the budget a bound on one uninterrupted slice
-- rather than on a thread's whole life. That distinction is the whole
-- design: CreateThread(function() while true do Wait(0) end end) is ordinary
-- FiveM code and must keep running for as long as the game does. What the
-- budget catches is a slice that never yields -- `while true do end` -- which
-- is the one shape that cannot be recovered from any other way.
--
-- A few dozen directives, or one frame's worth of script work, need nowhere
-- near this many instructions, so the bound turns a frozen console that only
-- a hard restart clears into an imperceptible pause and a log line.
local INSTRUCTION_BUDGET = 200000
S.INSTRUCTION_BUDGET = INSTRUCTION_BUDGET

-- Coroutines that have already had a budget error raised in them during the
-- slice they are currently running. Weak keys: the ordinary case is that the
-- error escapes and the coroutine dies, and this table must not be what keeps
-- it alive.
local struck = setmetatable({}, { __mode = "k" })

-- Raised inside the offending coroutine, so it comes back through
-- coroutine.resume as an ordinary `false, message` -- no separate failure
-- path to thread through every caller. Level 0: the position of the
-- instruction that happened to trip the counter says nothing useful about
-- where the loop is.
--
-- "An ordinary error" is also the hole, though: ordinary Lua code can catch
-- one. The counter resets on every raise, so
--
--     CreateThread(function() while true do
--         pcall(function() while true do end end)
--     end end)
--
-- buys itself a fresh 200k budget every time round and never stops -- three
-- seconds of frozen game thread, measured, and it only ended because the
-- prober had its own escape hatch. `while true do end`, the shape this budget
-- was written for, was genuinely bounded; this one was not.
--
-- So the SECOND strike within one slice re-arms at a single instruction.
-- Every instruction after that raises, which means each enclosing pcall
-- absorbs exactly one raise and the thread unwinds in a number of raises
-- bounded by its own pcall nesting rather than never. Two budgets' worth of
-- work is the ceiling instead of an unbounded number of them.
local function budget_exceeded()
    local co = co_running()
    if struck[co] then
        sethook(co, budget_exceeded, "", 1)
    else
        struck[co] = true
    end
    error("exceeded its instruction budget (" .. INSTRUCTION_BUDGET ..
          " instructions) -- likely an infinite loop", 0)
end

-- Arm (or re-arm) the budget on one coroutine. Call this immediately before
-- every resume, not once at creation: see the per-slice note above.
--
-- Clearing the strike mark is part of that same per-slice rule, and it is
-- what keeps the escalation above from becoming a life sentence: a thread
-- that swallowed a budget error and then YIELDED is a cooperative thread
-- having one bad slice, and the next resume is a new slice with a whole
-- fresh budget. Only a slice that never gives control back escalates.
function S.arm_budget(co)
    struck[co] = nil
    sethook(co, budget_exceeded, "", INSTRUCTION_BUDGET)
end

-- An owner is opaque here. The scheduler only ever compares one for identity
-- (kill_owner) and asks whether it is dead; everything resource-owned passes
-- the resource RECORD, which boot.lua's make_env already closes over. This is
-- for the log lines, which want something a human can read.
local function owner_label(owner)
    if type(owner) == "table" and owner.name ~= nil then return tostring(owner.name) end
    return tostring(owner)
end

-- True once the owning resource's teardown has run.
--
-- Why the scheduler has to ask at all: a resource that stops itself does so
-- from inside a coroutine this module is in the middle of running, and that
-- coroutine is in neither `threads` nor `staged` while its body executes --
-- kill_owner has nothing to find. The continuation is staged AFTER the body
-- returns (see create_now), i.e. after the teardown, and would otherwise run
-- a frame later against a dead record's still-captured env.
--
-- Asked of the RECORD, not of the name. Those are different questions, and
-- they diverge on exactly one shape: a callback that stops and then RESTARTS
-- its own resource. By the time its continuation is staged there is a new,
-- live record under that name, so "is the resource called X dead" answered
-- no and the dead incarnation's body was staged against its replacement.
-- Everything downstream contained it -- every registration it could make is
-- refused, and the new record's teardown sweeps it -- but nothing REPORTED
-- it, and a body abandoned in silence is indistinguishable from one that
-- never ran, which is the whole reason the refusal below is logged.
--
-- The name lookup stays as the fallback for a plain string owner (the
-- scheduler's own tests use one, and nothing stops a future caller). `rt.res`
-- is resolved at CALL time, not bound at load time: resource.lua loads after
-- this chunk, so the field does not exist yet while this file is being read.
local function owner_dead(owner)
    if owner == nil then return false end
    if type(owner) == "table" then return owner.dead == true end
    local res = rt.res
    return res ~= nil and res.is_dead ~= nil and res.is_dead(owner) == true
end

-- Staged entries carry a DELAY, not a resolved wake time: they are promoted
-- into threads[] at the top of the NEXT tick (see the module comment), and it
-- is that promotion's `now` -- not whatever `now` happened to be left over
-- from the last tick when add() was called -- that the delay is relative to.
-- Resolving eagerly here would let a SetTimeout called between two ticks (or
-- from a thread mid-tick, for the same reason CreateThread stages) fire
-- against a stale clock and go off early.
local function add(co, delay, owner)
    -- Refused, not staged: nothing would ever sweep this entry again. Its
    -- owner's teardown has already been and gone, so kill_owner will not
    -- revisit it, and the body it would resume closes over an env that no
    -- longer belongs to anything. Reported rather than dropped in silence --
    -- abandoning the rest of a handler is a real, if correct, loss, and a
    -- caller who cannot see it has no way to tell this apart from code that
    -- simply did not run.
    if owner_dead(owner) then
        report("[" .. owner_label(owner) .. "] refusing to schedule a thread for a resource " ..
               "that has already been torn down; the rest of that body is abandoned")
        return nil
    end
    local handle = next_handle
    next_handle = next_handle + 1
    staged[handle] = { co = co, delay = delay, owner = owner }
    return handle
end

function S.wait(ms)
    return co_yield(ms or 0)
end

function S.create(fn, owner)
    return add(co_create(fn), 0, owner)
end

function S.set_timeout(ms, fn, owner)
    return add(co_create(fn), ms or 0, owner)
end

function S.clear(handle)
    threads[handle] = nil
    staged[handle] = nil
end

-- Runs the body immediately, up to its first yield. Used for event handlers,
-- which FiveM also dispatches this way so a TriggerEvent's effects are visible
-- to the caller rather than a frame later.
--
-- A later task dispatches event handlers through this from inside a thread
-- that tick() is already resuming -- i.e. from inside tick()'s own
-- `pairs(threads)` traversal. If the body doesn't finish, its continuation
-- MUST go through add() into `staged`, exactly like create()/set_timeout():
-- Lua allows nil-ing an existing key mid-traversal but never adding a new
-- one, so writing straight into `threads` here would corrupt that
-- traversal. In practice that can silently skip or double-resume a thread
-- (verified: with the direct-into-threads version, a create_now'd thread
-- whose body immediately does wait(0) gets visited and resumed a second time
-- within the very same tick, before ever returning to the caller) or raise
-- "invalid key to 'next'" out of tick(), uncaught. Routing through add() also
-- resolves the continuation's wake against the NEXT promotion's `now`
-- instead of whatever `now` happens to be right now, for the same
-- stale-clock reason set_timeout() does.
-- Returns ok, handle. handle is nil when the body ran to completion without
-- yielding -- there is nothing left to track. When the body yielded instead,
-- handle names the staged continuation, so a caller that wants to register it
-- against something (make_env's CreateThreadNow wrapper, against the owning
-- resource's record) can. FiveM's CreateThreadNow returns a plain boolean, so
-- this second value is for internal callers only; a script-facing wrapper
-- must still return just `ok`.
function S.create_now(fn, owner)
    -- Nothing to dispatch to: the owner is already torn down, so running the
    -- body would run it against a dead env. In practice teardown has already
    -- unregistered whatever would have led here, so this is a backstop --
    -- logged, because a dispatch that quietly does nothing is exactly the
    -- kind of failure a caller cannot detect. The continuation is refused
    -- separately, in add() above: that is the case that actually bites,
    -- because the owner dies WHILE this body runs.
    if owner_dead(owner) then
        report("[" .. owner_label(owner) .. "] refusing to run a callback for a resource " ..
               "that has already been torn down")
        return false, nil
    end
    local co = co_create(fn)
    S.arm_budget(co)
    local ok, delay = co_resume(co)
    if not ok then
        report("[thread error] " .. tostring(delay) .. "\n" .. traceback(co))
        return false, nil
    end
    if co_status(co) ~= "dead" then
        return true, add(co, tonumber(delay) or 0, owner)
    end
    return true, nil
end

function S.kill_owner(owner)
    -- nil is "no owner", not a wildcard: treating it as a real match key
    -- would let a caller that passes nil by mistake (a resource teardown,
    -- once a later task adds one) silently stop every unowned thread in the
    -- system instead of doing nothing.
    if owner == nil then return end
    for handle, t in pairs(threads) do
        if t.owner == owner then threads[handle] = nil end
    end
    for handle, t in pairs(staged) do
        if t.owner == owner then staged[handle] = nil end
    end
end

function S.count()
    local n = 0
    for _ in pairs(threads) do n = n + 1 end
    for _ in pairs(staged) do n = n + 1 end
    return n
end

function S.tick(now_ms)
    -- Keep the previous value on a nil argument rather than rewinding to 0:
    -- the clock must be monotone regardless of what the host passes, or
    -- every pending timer would fire at once the moment it happened.
    now = now_ms or now

    for handle, t in pairs(staged) do
        t.wake = now + t.delay
        t.delay = nil
        threads[handle] = t
        staged[handle] = nil
    end

    for handle, t in pairs(threads) do
        if now >= t.wake then
            -- Re-armed here, on every resume, rather than once when the
            -- thread was created: that is what makes the budget a bound on
            -- this slice instead of on the thread's whole life. A thread that
            -- yields and comes back gets a full budget again, forever.
            S.arm_budget(t.co)
            local ok, delay = co_resume(t.co)
            if not ok then
                report("[thread error] " .. tostring(delay) .. "\n" .. traceback(t.co))
                threads[handle] = nil
            elseif co_status(t.co) == "dead" then
                threads[handle] = nil
            else
                t.wake = now + (tonumber(delay) or 0)
            end
        end
    end
end

rt.sched = S
return S
