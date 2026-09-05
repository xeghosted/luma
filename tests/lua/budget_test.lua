-- tests/lua/budget_test.lua
-- The instruction budget, applied to the two places that were running
-- unbounded on the game thread: a resource's script bodies (resource.lua's
-- chunk run) and the scheduler's resumes.
--
-- manifest.lua has had a debug.sethook instruction budget since Task 4,
-- precisely so a `while true do end` in an fxmanifest could not freeze the
-- console. The chunk one function later, and every scheduler resume, had
-- none: `while true do end` at the top of client.lua, or inside a
-- CreateThread, hard-froze the game -- the exact failure that ruling was
-- meant to prevent.
--
-- The subtlety these tests exist to pin: the budget is PER RESUME SLICE and
-- is re-armed on every resume. `CreateThread(function() while true do Wait(0)
-- end end)` is ordinary FiveM code and must keep running forever; the bug is
-- a slice that never yields, not a thread that has been alive a long time. A
-- cumulative budget would look identical on the two runaway tests below and
-- kill the legitimate loop -- which is what the first section is for.
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

    local function mk(name, body)
        host.files["/data/rdr2lua/resources/" .. name .. "/fxmanifest.lua"] =
            "name '" .. name .. "'\nclient_script 'main.lua'\n"
        host.files["/data/rdr2lua/resources/" .. name .. "/main.lua"] = body
    end

    local function logged_since(mark, needle)
        for i = mark + 1, #host.logged do
            if tostring(host.logged[i]):find(needle, 1, true) then return true end
        end
        return false
    end

    -- Every runaway below used to hang the frame hook outright, and "it did
    -- not hang" was asserted with h.check(true, ...) -- which cannot report
    -- FAIL, only fail to finish, leaving a regression looking like a stuck
    -- terminal rather than a red line. run_bounded gives those a real
    -- assertion: the call has to come back, AND come back inside a wall-clock
    -- bound generous enough that no healthy machine trips it. It still cannot
    -- interrupt a true hang (nothing in-process can), but it does catch the
    -- shape this file actually keeps meeting -- a budget that fires but only
    -- after the thread has bought itself another one, and another.
    local SLICE_LIMIT = 2.0     -- seconds; one budgeted slice costs ~0.01s
    local function run_bounded(what, fn)
        local t0 = os.clock()
        local ok, res = pcall(fn)
        local spent = os.clock() - t0
        h.check(ok and spent < SLICE_LIMIT, string.format(
            "%s (returned in %.3fs%s)", what, spent,
            ok and "" or (", raised: " .. tostring(res))))
        return res
    end

    ------------------------------------------------------- per-slice, not cumulative
    -- A Wait(0) loop that does real work in every slice: ~40k VM instructions
    -- per resume, well inside one budget, but far past it once a few dozen
    -- slices are added together. Sixty frames is therefore ~2.4M instructions
    -- of lifetime against a 200k budget -- a cumulative implementation kills
    -- this thread within the first handful of frames, a per-slice one never
    -- touches it. This is the test that tells the two apart.
    BUD = { n = 0 }
    mk("waiter", [[
        CreateThread(function()
            while true do
                local s = 0
                for i = 1, 20000 do s = s + i end
                BUD.n = BUD.n + 1
                Wait(0)
            end
        end)
    ]])

    local wok, werr = R.start("waiter")
    h.check(wok == true, "a resource whose thread loops on Wait(0) starts" ..
        (werr and (" (" .. tostring(werr) .. ")") or ""))
    for i = 1, 60 do __rt.sched.tick(i * 16) end
    h.check(BUD.n == 60, string.format(
        "  and its slice runs on all 60 frames, untouched by the budget (%d)", BUD.n))
    h.check(__rt.sched.count() == 1, "  the thread is still alive after 60 frames")
    R.stop("waiter")
    h.check(__rt.sched.count() == 0, "  and stops with its resource")
    BUD = nil

    ------------------------------------------------------- runaway chunk body
    -- resource.lua's chunk run had no budget at all: this froze the console
    -- outright, with only a hard restart to clear it.
    mk("runaway", "while true do end")
    local mark = #host.logged
    local rok, rerr = R.start("runaway")
    h.check(rok == false, "a resource whose chunk body never returns does not hang the frame hook")
    h.check(type(rerr) == "string" and rerr:find("budget", 1, true) ~= nil,
        "  and the failure names the instruction budget" ..
        (rerr and (" (" .. tostring(rerr) .. ")") or ""))
    h.check(R.state("runaway") == "error", "  its state is error")
    h.check(__rt.sched.count() == 0, "  and it left nothing running")

    -- A chunk that fails halfway is still transactional: the file before the
    -- runaway one already registered, and must be swept.
    host.files["/data/rdr2lua/resources/runaway2/fxmanifest.lua"] =
        "name 'runaway2'\nclient_scripts { 'a.lua', 'b.lua' }\n"
    host.files["/data/rdr2lua/resources/runaway2/a.lua"] =
        "RegisterCommand('doomed', function() end)\nCreateThread(function() while true do Wait(0) end end)"
    host.files["/data/rdr2lua/resources/runaway2/b.lua"] = "while true do end"
    local r2ok = R.start("runaway2")
    h.check(r2ok == false, "a runaway in the SECOND script file fails the whole resource")
    h.check(__rt.sched.count() == 0, "  and the first file's thread is torn down with it")
    h.check(__rt.commands.count() == 0, "  along with its command")

    ------------------------------------------------------- runaway in a thread
    -- The scheduler's own resume had no budget either, so a CreateThread that
    -- never yields froze the same way -- one frame later.
    mk("runthread", "CreateThread(function() while true do end end)")
    h.check(R.start("runthread") == true, "a resource whose CreateThread never yields still starts")
    mark = #host.logged
    run_bounded("  the tick that resumes it returns instead of freezing",
        function() __rt.sched.tick(100000) end)
    h.check(__rt.sched.count() == 0, "  the runaway thread is dropped from the scheduler")
    h.check(logged_since(mark, "budget"), "  and the drop is reported, naming the budget")
    h.check(R.stop("runthread") == true, "  and the resource still stops cleanly")

    -- The same, through create_now: an event handler that never yields is
    -- resumed immediately inside trigger(), not from tick().
    mk("runevt", [[
        AddEventHandler("spin", function() while true do end end)
    ]])
    h.check(R.start("runevt") == true, "a resource with a never-yielding event handler starts")
    mark = #host.logged
    local dispatched = run_bounded("  triggering it returns instead of freezing",
        function() return __rt.events.trigger("spin") end)
    h.check(dispatched == 1, "  having dispatched to the one handler")
    h.check(logged_since(mark, "budget"), "  and the budget is named in the report")
    h.check(__rt.sched.count() == 0, "  with nothing left staged")
    R.stop("runevt")

    ------------------------------------------------------- a runaway wrapped in pcall
    -- The budget error is an ordinary Lua error, so ordinary Lua code can
    -- catch it -- and then go round again on a freshly re-armed budget. The
    -- reported `while true do end` shape is genuinely fixed; this is the
    -- variant that was not, and it held the game thread for a measured three
    -- seconds, returning only because the prober had its own escape hatch.
    --
    -- The escape hatch in the body below is that failure signal, not the
    -- mechanism under test: it is what keeps this file terminating (and so
    -- able to print a red line) when the bound stops working, since nothing
    -- in-process can interrupt a spinning coroutine.
    NEST = { n = 0 }
    mk("nested", [[
        CreateThread(function()
            while true do
                NEST.n = NEST.n + 1
                if NEST.n > 200 then
                    error("escape hatch: the budget never stopped this thread", 0)
                end
                pcall(function() while true do end end)
            end
        end)
    ]])
    h.check(R.start("nested") == true, "a resource whose thread pcalls a runaway starts")
    mark = #host.logged
    run_bounded("  the tick that resumes it still returns",
        function() __rt.sched.tick(200000) end)
    h.check(NEST.n <= 5, string.format(
        "  and catching the budget error does not buy another whole budget (%d rounds)", NEST.n))
    h.check(__rt.sched.count() == 0, "  the thread is dropped despite the pcall")
    h.check(logged_since(mark, "instruction budget"),
        "  and the drop is reported, naming the budget")
    h.check(not logged_since(mark, "escape hatch"),
        "  by the budget itself, not by the test's own escape hatch")
    R.stop("nested")
    NEST = nil

    -- ...and the escalation is a bound on ONE slice, not a life sentence. A
    -- thread that swallows a budget error and then yields properly is a
    -- thread on a new slice, and gets a whole fresh budget -- the same
    -- distinction the very first section of this file exists to protect.
    NEST2 = { n = 0, caught = 0 }
    mk("recovers", [[
        CreateThread(function()
            while true do
                NEST2.n = NEST2.n + 1
                if NEST2.n <= 2 then
                    local ok = pcall(function() while true do end end)
                    if not ok then NEST2.caught = NEST2.caught + 1 end
                end
                Wait(0)
            end
        end)
    ]])
    h.check(R.start("recovers") == true, "a thread that swallows a budget error and then yields starts")
    run_bounded("  ten frames of it return", function()
        for i = 1, 10 do __rt.sched.tick(300000 + i * 16) end
    end)
    h.check(NEST2.caught == 2, string.format(
        "  it caught the budget error on each of its two runaway slices (%d)", NEST2.caught))
    h.check(NEST2.n == 10, string.format(
        "  and still ran on all ten frames, one slice each (%d)", NEST2.n))
    h.check(__rt.sched.count() == 1, "  with the thread still alive")
    R.stop("recovers")
    h.check(__rt.sched.count() == 0, "  until its resource stops")
    NEST2 = nil
end
