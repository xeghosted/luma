-- tests/lua/reentrancy_test.lua
-- Re-entrancy: what happens when a resource's own code drives the lifecycle
-- from inside a callback that the lifecycle is itself running.
--
-- All of this happens on the game thread, inside the frame hook. "It recursed
-- two hundred levels and then reported success" is therefore not a nuisance:
-- it is a frozen console plus a lie on the wire. Two Criticals lived here for
-- twelve task reviews because nothing in the suite ever re-entered anything.
--
--   C1  A resource that stops itself leaks the coroutine it is running in.
--       commands.run / events.trigger resume the handler through
--       sched.create_now, so while the body runs its coroutine is in neither
--       `threads` nor `staged` and teardown's kill_owner cannot see it.
--       create_now then stages the continuation *after* the body returns, and
--       the zombie runs a frame later against the dead record's still-captured
--       env -- re-registering into the global command table, permanently.
--
--   C2  R.start and R.stop had no in-progress guard. R.stop fired
--       onResourceStop while rec.state was still "started", and R.start left
--       the record "stopped" for the whole chunk run, so a handler that
--       stopped or started its own resource recursed until the C stack gave
--       out -- and the outermost call still returned true.
return function(h)
    for _, m in ipairs({ "scheduler", "manifest", "resource", "api", "boot" }) do package.loaded[m] = nil end
    __rt = {}
    require("scheduler"); require("manifest")
    local host = require("harness").fake_host()
    -- The host hands the resource root in: it is per game, so resource.lua
    -- cannot bake one in. The tests must supply it like the real host does.
    __rt.resource_root = "/data/rdr2lua/resources"
    __rt.host = host; __rt.log = host.log
    require("api"); require("resource"); require("boot")
    local R = __rt.res

    -- The injected clock, advanced only through here so `now` stays monotone
    -- across every section of this file (the scheduler keeps its previous
    -- value on a nil argument and would otherwise fire every pending timer at
    -- once if a later section rewound it).
    local clock = 0
    local function frames(n)
        for _ = 1, n do clock = clock + 16; __rt.sched.tick(clock) end
    end

    -- Scans the log tail for a substring, so a section can assert that a
    -- refusal was actually reported rather than silently swallowed -- and,
    -- inversely, that nothing blew the C stack.
    local function logged_since(mark, needle)
        for i = mark + 1, #host.logged do
            if tostring(host.logged[i]):find(needle, 1, true) then return true end
        end
        return false
    end

    local function mk(name, body)
        host.files["/data/rdr2lua/resources/" .. name .. "/fxmanifest.lua"] =
            "name '" .. name .. "'\nclient_script 'main.lua'\n"
        host.files["/data/rdr2lua/resources/" .. name .. "/main.lua"] = body
    end

    RE = {}

    ------------------------------------------------------------------ C1: command
    -- The reviewer's exact reproduction: a command handler that stops its own
    -- resource, yields, and then re-registers.
    mk("selfcmd", [[
        RegisterCommand("bye", function()
            StopResource("selfcmd")
            Wait(0)
            RegisterCommand("ghost", function() end)
        end)
    ]])

    h.check(R.start("selfcmd") == true, "selfcmd starts")
    h.check(__rt.commands.count() == 1, "  with its one command registered")

    local mark = #host.logged
    h.check(__rt.commands.run("bye") == true, "the self-stopping command dispatches")
    h.check(R.state("selfcmd") == "stopped", "  and the resource reports stopped")
    h.check(__rt.commands.count() == 0, "  its command is unregistered by teardown")
    -- The bug: create_now stages the continuation after the body returns, so
    -- the handler's own coroutine outlives the teardown that was supposed to
    -- have swept it.
    h.check(__rt.sched.count() == 0,
        "a handler that stopped its own resource leaves no live thread behind")
    h.check(logged_since(mark, "refus"),
        "  and the abandoned continuation is reported, not dropped in silence")

    -- The zombie only bites on the NEXT tick, where it re-registers from the
    -- dead record's env.
    frames(3)
    h.check(__rt.commands.count() == 0,
        "  nothing re-registers into the command table after teardown")
    h.check(__rt.sched.count() == 0, "  and the scheduler is still empty a few frames later")

    -- This is the half that makes C1 permanent rather than transient: a later
    -- clean start+stop cannot return the command table to zero while a ghost
    -- entry from the previous life is still sitting in it.
    h.check(R.start("selfcmd") == true, "selfcmd starts again cleanly")
    h.check(__rt.commands.count() == 1, "  registering exactly its own one command")
    h.check(R.stop("selfcmd") == true, "  and stops")
    h.check(__rt.commands.count() == 0, "  leaving the command table empty again")
    h.check(__rt.sched.count() == 0, "  and the scheduler empty")

    ------------------------------------------------------------------ C1: event
    -- Same hole through api.lua's event dispatch, which uses the same
    -- create_now path.
    mk("selfevt", [[
        AddEventHandler("boom", function()
            StopResource("selfevt")
            Wait(0)
            RegisterCommand("evtghost", function() end)
        end)
    ]])

    h.check(R.start("selfevt") == true, "selfevt starts")
    h.check(__rt.events.count() == 1, "  with its one handler registered")
    __rt.events.trigger("boom")
    h.check(R.state("selfevt") == "stopped", "an event handler can stop its own resource")
    h.check(__rt.events.count() == 0, "  and its handler is gone")
    h.check(__rt.sched.count() == 0, "  leaving no live thread behind either")
    frames(3)
    h.check(__rt.commands.count() == 0, "  and nothing re-registers a frame later")
    h.check(__rt.sched.count() == 0, "  the scheduler stays empty")

    ------------------------------------------------------------------ C2: start self
    -- StartResource on itself. The record stays "stopped" for the whole chunk
    -- run, so nothing refuses the re-entry; it recurses until the C stack
    -- gives out, and the outermost call still returns ok=true, err=nil.
    RE.n = 0
    mk("selfstart", [[
        RE.n = RE.n + 1
        RE.ok, RE.err = StartResource("selfstart")
    ]])

    local ssok, sserr = R.start("selfstart")
    h.check(RE.n == 1, string.format(
        "a resource that starts itself runs its chunk exactly once (ran %d times)", RE.n))
    h.check(RE.ok == false, "  the re-entrant StartResource is refused, not recursed into")
    h.check(type(RE.err) == "string" and #RE.err > 0,
        "  and the refusal reaches the caller as a real reason, not a silent success")
    h.check(ssok == true, "  while the outermost start still succeeds" ..
        (sserr and (" (" .. tostring(sserr) .. ")") or ""))
    h.check(R.state("selfstart") == "started", "  leaving it started")
    h.check(R.stop("selfstart") == true, "  and it stops again afterwards")

    ------------------------------------------------------------------ C2: A/B pair
    -- Two resources that start each other. Unbounded before the guard: ~196
    -- levels, ~400 file reads and 196 manifest parses in one frame.
    RE.a, RE.b = 0, 0
    mk("mutA", [[
        RE.a = RE.a + 1
        RE.aok, RE.aerr = StartResource("mutB")
    ]])
    mk("mutB", [[
        RE.b = RE.b + 1
        RE.bok, RE.berr = StartResource("mutA")
    ]])

    local reads = 0
    local real_read = host.read_file
    host.read_file = function(path) reads = reads + 1; return real_read(path) end
    local mok, merr = R.start("mutA")
    host.read_file = real_read

    h.check(mok == true, "the outer start of a mutually-starting pair succeeds" ..
        (merr and (" (" .. tostring(merr) .. ")") or ""))
    h.check(RE.a == 1 and RE.b == 1, string.format(
        "  each side's chunk runs exactly once (a=%d, b=%d)", RE.a, RE.b))
    -- Bounded in the only unit the frame hook cares about: work done. Four
    -- reads are the honest total (two manifests, two scripts); the pre-guard
    -- runs measured in the hundreds.
    h.check(reads <= 8, string.format(
        "  the whole pair costs a bounded number of file reads (%d)", reads))
    h.check(RE.aok == true, "  A really did start B")
    h.check(RE.bok == false and type(RE.berr) == "string" and #RE.berr > 0,
        "  and B starting A back is refused with a reason, not silently recursed")
    R.stop("mutB"); R.stop("mutA")

    ------------------------------------------------------------------ C2: stop re-entry
    -- R.stop fires onResourceStop BEFORE teardown, and rec.state is still
    -- "started" while those handlers run: a handler that stops its own
    -- resource re-enters stop, and the guard never fires.
    RE.s = 0
    mk("stopper", [[
        AddEventHandler("onResourceStop", function(who)
            if who ~= "stopper" then return end
            RE.s = RE.s + 1
            RE.sok, RE.serr = StopResource("stopper")
        end)
    ]])

    h.check(R.start("stopper") == true, "stopper starts")
    mark = #host.logged
    local pok, sok2 = pcall(R.stop, "stopper")
    h.check(pok == true, "stopping a resource whose onResourceStop stops it again does not raise")
    h.check(sok2 == true, "  the outer stop reports success")
    h.check(RE.s == 1, string.format(
        "  and its onResourceStop handler runs exactly once (ran %d times)", RE.s))
    h.check(RE.sok == false and type(RE.serr) == "string" and #RE.serr > 0,
        "  the re-entrant StopResource is refused with a reason")
    h.check(not logged_since(mark, "stack overflow"),
        "  and nothing blew the C stack inside the frame hook")
    h.check(R.state("stopper") == "stopped", "  leaving it stopped")
    h.check(__rt.events.count() == 0, "  with its handler removed")
    h.check(__rt.sched.count() == 0, "  and no thread left behind")

    ------------------------------------------------- a teardown mutating the list being dispatched
    -- Two resources both handling onResourceStop, and A's handler stops B.
    -- events.trigger walked the LIVE handler list with a cached #list, and B's
    -- teardown table.remove'd B's entry out of that same list mid-walk, so the
    -- last index indexed off the end: "attempt to index a nil value" raised
    -- out of trigger, out of R.stop, past `rec.state = "stopped"`.
    --
    -- Wave A is what made that permanent rather than merely noisy: the raise
    -- now escapes with the record sitting in "stopping", which BOTH R.start
    -- and R.stop refuse -- so the resource is unstartable for the rest of the
    -- session. That is the assertion this section is really about.
    RE.ta, RE.tb = 0, 0
    mk("tandemA", [[
        AddEventHandler("onResourceStop", function(who)
            RE.ta = RE.ta + 1
            if who == "tandemA" then StopResource("tandemB") end
        end)
    ]])
    mk("tandemB", [[
        AddEventHandler("onResourceStop", function(who) RE.tb = RE.tb + 1 end)
    ]])
    h.check(R.start("tandemA") == true, "tandemA starts")
    h.check(R.start("tandemB") == true, "  and tandemB, both handling onResourceStop")
    h.check(__rt.events.count() == 2, "  two handlers on the one event name")

    local dok, derr = pcall(R.stop, "tandemA")
    h.check(dok == true, "stopping A, whose handler stops B, does not raise out of R.stop" ..
        (dok and "" or (" (" .. tostring(derr) .. ")")))
    h.check(R.state("tandemA") == "stopped",
        "  A lands on a terminal state, not wedged in \"stopping\"")
    h.check(R.state("tandemB") == "stopped", "  and B is stopped by the handler as asked")
    h.check(__rt.events.count() == 0, "  with both handlers swept")
    -- The half that makes it a brick rather than a blemish.
    h.check(R.start("tandemA") == true, "  A starts again afterwards, not refused forever")
    h.check(R.stop("tandemA") == true, "  and stops again")

    -- A handler removed while the event is dispatching must not be called by
    -- that dispatch: the snapshot hands trigger an entry B's teardown has
    -- already retired, and calling it would run a torn-down resource's code.
    h.check(RE.tb == 1, string.format(
        "  B's handler ran exactly once, for B's own stop (%d)", RE.tb))

    ------------------------------------------------- no raise may strand a transient state
    -- (1) fixes the raise we know about. This is the guarantee for the ones we
    -- do not: whatever raises inside R.stop's transient window, the record
    -- still has to come out at a terminal state and the error still has to be
    -- reported. Forced by replacing the dispatch itself, so the test does not
    -- depend on any particular handler bug staying reproducible.
    mk("brick", "AddEventHandler('onResourceStop', function() end)")
    h.check(R.start("brick") == true, "brick starts")
    local real_trigger = __rt.events.trigger
    __rt.events.trigger = function() error("kaboom from the dispatch machinery", 0) end
    mark = #host.logged
    local bok, sok, serr = pcall(R.stop, "brick")
    h.check(bok == true, "a raise from inside the stop's own dispatch does not escape R.stop" ..
        (bok and "" or (" (" .. tostring(sok) .. ")")))
    h.check(sok == true, "  R.stop still reports the stop itself as successful")
    h.check(type(serr) == "string" and serr:find("kaboom", 1, true) ~= nil,
        "  and its second value names the handler that raised, for a caller that looks")
    h.check(R.state("brick") == "stopped", "  the record still reaches a terminal state")
    h.check(logged_since(mark, "kaboom"), "  and the error is reported, not swallowed")
    h.check(__rt.events.count() == 0, "  the teardown still ran despite the raise")
    h.check(R.start("brick") == true, "  and the resource is startable again, not bricked")

    -- R.restart reads only R.stop's first value (see R.restart in
    -- resource.lua), so the same raise must not stop a restart from going on
    -- to actually restart -- the second value is there for a caller that
    -- looks, not one that merely propagates R.stop's own answer.
    mark = #host.logged
    local rrok, rrerr = R.restart("brick")
    h.check(rrok == true, "R.restart still succeeds when the stop half's handler raised" ..
        (rrok and "" or (" (" .. tostring(rrerr) .. ")")))
    h.check(R.state("brick") == "started", "  leaving the resource started, not half-restarted")
    h.check(logged_since(mark, "kaboom"), "  with the handler's raise still reported")
    __rt.events.trigger = real_trigger
    h.check(R.stop("brick") == true, "  and stops, cleanly this time")

    -- The backstop under that one. A pcall only helps if the statement AFTER
    -- it gets to run, and the instruction budget's hook can re-raise between
    -- two statements once it has escalated (see scheduler.lua). So the record
    -- also remembers a ticket cut from a counter that only ever counts up:
    -- while the call is really in flight nothing has taken a newer one, and
    -- once something has, the ticket proves the operation that issued it is
    -- over -- with no need to know which coroutine, if any, it ran on. Forced
    -- here, because a raise that outruns a pcall is not reproducible on
    -- demand. -1 stands in for "gone": next_op_token only ever counts up from
    -- 0, and dozens of real lifecycle calls have already run earlier in this
    -- file, so whatever it is now is well past -1 already.
    for _, forced in ipairs({ { "stopping", "stopped" }, { "starting", "error" } }) do
        h.check(R.start("brick") == true, "brick starts, to be left " .. forced[1] .. " by hand")
        local rec = R.get("brick")
        rec.state, rec.op_token = forced[1], -1
        mark = #host.logged
        h.check(R.state("brick") == forced[2],
            "  a record left \"" .. forced[1] .. "\" by a call that is gone settles to \"" ..
            forced[2] .. "\"")
        h.check(logged_since(mark, "no longer running"), "  saying so in the log")
        h.check(__rt.events.count() == 0, "  having finished the teardown it interrupted")
        h.check(R.start("brick") == true, "  and the resource starts rather than being unstartable")
        h.check(R.stop("brick") == true, "  and stops")
    end

    -- ...and the same check must not undo a transient state that IS in
    -- flight, or the re-entry guards above stop working. The stopper section
    -- already proves the refusal from within a handler's own re-entrant call;
    -- this pins the reason it still holds.
    h.check(R.start("stopper") == true, "stopper starts again")
    RE.s, RE.sok, RE.serr = 0, nil, nil
    h.check(R.stop("stopper") == true, "  and stops")
    h.check(RE.sok == false and type(RE.serr) == "string",
        "  its re-entrant StopResource is still refused, not settled away")

    ------------------------------------------------- list()/info() never show a transient state
    -- Both used to read rec.state (or the counts it gates) raw, so a record
    -- stuck "starting"/"stopping" by a call that has since died -- exactly
    -- the shape the token section above forces by hand -- would show up on
    -- the wire and in the editor tree as still mid-lifecycle, when R.state
    -- asked in the same breath would already report it settled.
    h.check(R.start("brick") == true, "brick starts, to be caught mid-teardown by info()")
    do
        local rec = R.get("brick")
        rec.state, rec.op_token = "stopping", -1
        local info = R.info("brick")
        h.check(info ~= nil and info.handlers == 0,
            "info() settles a stale \"stopping\" record before reading its counts" ..
            (info and (" (handlers=" .. tostring(info.handlers) .. ")") or ""))
    end

    h.check(R.start("brick") == true, "brick starts again, to be caught mid-teardown by list()")
    do
        local rec = R.get("brick")
        rec.state, rec.op_token = "stopping", -1
        local seen_state
        for _, e in ipairs(R.list()) do if e.name == "brick" then seen_state = e.state end end
        h.check(seen_state == "stopped",
            "list() settles a stale \"stopping\" record instead of reporting it" ..
            (seen_state and (" (got \"" .. tostring(seen_state) .. "\")") or " (missing from the listing)"))
        -- Settling it is what list() just did as a side effect of reading it
        -- -- confirmed independently, through the state R.list() cannot fake.
        h.check(R.state("brick") == "stopped", "  which really did finish the teardown")
    end

    ------------------------------------------------------------------ restart
    -- The guards must not cost restart its own stop-then-start: R.restart's
    -- stop has to leave the record in a state R.restart's start will accept.
    RE.r, RE.rstopped = 0, 0
    mk("restarter", [[
        RE.r = RE.r + 1
        AddEventHandler("onResourceStop", function() RE.rstopped = RE.rstopped + 1 end)
        RegisterCommand("restartcmd", function() end)
        CreateThread(function() while true do Wait(0) end end)
    ]])

    h.check(R.start("restarter") == true, "restarter starts")
    frames(2)   -- promote its thread out of `staged` so teardown sweeps the real case
    h.check(__rt.sched.count() == 1, "  with its thread live in the scheduler")

    local rok, rerr = R.restart("restarter")
    h.check(rok == true, "restart still works end to end with the in-progress guards in place" ..
        (rerr and (" (" .. tostring(rerr) .. ")") or ""))
    h.check(R.state("restarter") == "started", "  leaving it started")
    h.check(RE.r == 2, string.format("  having re-run its chunk exactly once more (%d)", RE.r))
    h.check(RE.rstopped == 1, string.format(
        "  and fired onResourceStop exactly once (%d)", RE.rstopped))
    h.check(__rt.commands.count() == 1, "  with exactly one command registered, not two")
    h.check(__rt.sched.count() == 1, "  and exactly one live thread, not two")

    h.check(R.stop("restarter") == true, "restarter stops")
    h.check(__rt.commands.count() == 0, "  with no command left registered")
    h.check(__rt.events.count() == 0, "  no handler left registered")
    h.check(__rt.sched.count() == 0, "  and no thread left in the scheduler")

    ------------------------------------------------- stop-and-restart from a callback
    -- The one path where the promised refusal did not fire. The scheduler
    -- asked "is the resource called X dead?" -- by NAME -- and a callback that
    -- stopped and then RESTARTED its own resource had already put a brand-new,
    -- live record under that name by the time its continuation was staged. So
    -- the dead incarnation's body was staged against the new record. Contained
    -- everywhere downstream (every registration it could make is refused, the
    -- new record's teardown sweeps it), but contained is not reported, and a
    -- silently abandoned body is precisely what that log line exists to make
    -- visible.
    RE.z = 0
    mk("phoenix", [[
        RegisterCommand("rise", function()
            StopResource("phoenix")
            StartResource("phoenix")
            Wait(0)
            RE.z = RE.z + 1
        end)
    ]])
    h.check(R.start("phoenix") == true, "phoenix starts")
    mark = #host.logged
    h.check(__rt.commands.run("rise") == true, "its command dispatches")
    h.check(R.state("phoenix") == "started", "  the resource is running again, as the handler asked")
    h.check(__rt.commands.count() == 1, "  with the new incarnation's one command registered")
    h.check(logged_since(mark, "refusing to schedule"),
        "  and the dead incarnation's continuation is refused, with the log to say so")
    frames(3)
    h.check(RE.z == 0, "  that body never resumes against the record that replaced it")
    h.check(__rt.sched.count() == 0, "  and nothing of it is left in the scheduler")
    h.check(R.stop("phoenix") == true, "  the new incarnation stops cleanly")

    -- A restart driven from inside the resource's own command handler: the
    -- same self-stop shape as C1, but through the path the console actually
    -- uses. It must be refused rather than recursed, and must not leave the
    -- record wedged in a transient state.
    RE.q = 0
    mk("quitter", [[
        RegisterCommand("selfrestart", function()
            RE.q = RE.q + 1
            RE.qok, RE.qerr = StartResource("quitter")
        end)
    ]])
    h.check(R.start("quitter") == true, "quitter starts")
    __rt.commands.run("selfrestart")
    h.check(RE.q == 1, string.format("a command that starts its own live resource runs once (%d)", RE.q))
    h.check(RE.qok == false and type(RE.qerr) == "string",
        "  and is refused, since the resource is already started")
    h.check(R.state("quitter") == "started", "  leaving the record started, not wedged")
    h.check(R.stop("quitter") == true, "  and it still stops cleanly afterwards")
    h.check(__rt.sched.count() == 0, "  with nothing left in the scheduler")
    h.check(__rt.commands.count() == 0, "  and nothing left in the command table")

    RE = nil
end
