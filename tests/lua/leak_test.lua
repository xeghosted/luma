-- tests/lua/leak_test.lua
-- Thirty start/stop cycles on a resource that registers one of everything.
-- Every counter must return to exactly where it started. This is the check the
-- one-lua_State isolation design rests on: anything a resource registers that
-- teardown does not undo shows up here as a number that climbs.
--
-- Fix round 1 (review): the original version only checked once, after the
-- entire loop -- and for commands and exports that is not actually a
-- detection mechanism. commands.add overwrites registered[key] in place on
-- every restart, and a torn-down resource's export sub-table is replaced by a
-- fresh one on the next start rather than accumulating entries, so neither
-- counter climbs the way threads/handlers do when teardown drops something;
-- a broken removal there just sits wrong at a constant count instead of
-- growing. Checking only once, positioned right after the loop's last stop,
-- happened to still catch that -- but only because the loop's last action IS
-- a stop; that is loop shape doing the work, not the test. This version
-- checks after every cycle's stop instead (Fix 1), adds a second,
-- name-colliding resource so a regression of "removed by name instead of
-- identity" has somebody to victimise (Fix 2), covers CreateThreadNow's own
-- tracking branch and proves liveness on every restart, not just the first
-- (Fix 3), and makes the record's own thread-count check exact instead of
-- padded (Fix 4).
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

    host.dirs["/data/rdr2lua/resources"] = { "kitchen" }
    host.files["/data/rdr2lua/resources/kitchen/fxmanifest.lua"] =
        "name 'kitchen'\nclient_script 'main.lua'\n"
    -- CreateThreadNow sits alongside the other four registration kinds
    -- (Fix 3): make_env's create_thread_now wrapper only tracks a handle when
    -- the body yields instead of finishing outright -- a conditional branch
    -- none of CreateThread/SetTimeout/on_tick exercise. Wait(50) makes this
    -- body yield instead of completing immediately, so that branch runs.
    host.files["/data/rdr2lua/resources/kitchen/main.lua"] = [[
        CreateThread(function() while true do Wait(0) end end)
        SetTimeout(1000000, function() end)
        CreateThreadNow(function() Wait(50) end)
        AddEventHandler("evt", function() KITCHEN_EVT_RAN = true end)
        RegisterNetEvent("netevt", function() end)
        exports("thing", function() return 1 end)
        RegisterCommand("kitchencmd", function() KITCHEN_CMD_RAN = true end)
        on_tick(function() end)
    ]]

    local function count_keys(t)
        local n = 0
        if t then for _ in pairs(t) do n = n + 1 end end
        return n
    end

    -- Fix round 2 (review): every counter here used to be an ENTRY count, and
    -- an entry count structurally cannot see a leak in the KEY. events.count()
    -- sums the lengths of the per-name handler lists, so an emptied list left
    -- parked under its event name forever reads as 0 -- one dead table per
    -- distinct event name, invisible to this test for as long as it only
    -- counted entries. handler_names counts the keys instead. The same class
    -- applies to exports_registry, hence export_owners; and `exports` itself
    -- was `... and 1 or 0`, a boolean wearing a number's clothes, which would
    -- read 1 whether kitchen held one export or fifty stale ones.
    local function counts()
        return {
            threads       = __rt.sched.count(),
            handlers      = __rt.events.count(),
            handler_names = __rt.events.name_count(),
            commands      = __rt.commands.count(),
            exports       = count_keys(__rt.exports_registry["kitchen"]),
            export_owners = count_keys(__rt.exports_registry),
        }
    end

    local base = counts()
    h.check(base.threads == 0 and base.handlers == 0 and base.commands == 0 and base.exports == 0,
            "the baseline is empty before anything starts")
    h.check(base.handler_names == 0 and base.export_owners == 0,
            "  including the tables those counters are keyed by")

    __rt.res.start("kitchen")
    local live = counts()
    h.check(live.threads == 4,
            "a started resource holds its four threads (CreateThread, SetTimeout, CreateThreadNow, on_tick)")
    h.check(live.handlers == 2, "  two event handlers")
    h.check(live.handler_names == 2, "  under two distinct event names")
    h.check(live.commands == 1, "  one command")
    h.check(live.exports == 1, "  one export")
    h.check(live.export_owners == 1, "  and one resource holding an export table")
    __rt.res.stop("kitchen")

    -- Fix 1: assert every cycle's stop against the baseline, not only the
    -- last one. commands and exports use overwrite/reuse semantics (see the
    -- header comment), so a dropped removal there does not make the counter
    -- climb across cycles -- it just sits wrong, at a constant count, from
    -- the very first cycle. Checking here means that shows up on cycle 1,
    -- for the reason it actually happened, rather than depending on the
    -- loop's last action being a stop.
    local function check_cycle(cycle, c)
        h.check(c.threads == base.threads,
                string.format("cycle %d: threads at baseline (%d -> %d)", cycle, base.threads, c.threads))
        h.check(c.handlers == base.handlers,
                string.format("cycle %d: handlers at baseline (%d -> %d)", cycle, base.handlers, c.handlers))
        -- The key-count checks: these are the ones that grow when a table is
        -- emptied but its key is never dropped, which no entry count can see.
        h.check(c.handler_names == base.handler_names,
                string.format("cycle %d: distinct event names at baseline (%d -> %d)",
                              cycle, base.handler_names, c.handler_names))
        h.check(c.commands == base.commands,
                string.format("cycle %d: commands at baseline (%d -> %d)", cycle, base.commands, c.commands))
        h.check(c.exports == base.exports,
                string.format("cycle %d: exports at baseline (%d -> %d)", cycle, base.exports, c.exports))
        h.check(c.export_owners == base.export_owners,
                string.format("cycle %d: export owners at baseline (%d -> %d)",
                              cycle, base.export_owners, c.export_owners))
    end

    -- What thirty cycles is actually worth, once the per-cycle checks above
    -- exist: detection happens on the very first cycle that diverges, not on
    -- the thirtieth. The count is not the detection mechanism; it is
    -- belt-and-braces confidence against something that only accumulates
    -- unevenly over a long run -- a console that only starts visibly leaking
    -- after an afternoon of reloads -- not a substitute for checking every
    -- cycle.
    for i = 1, 30 do
        __rt.res.start("kitchen")

        -- Fix 3: liveness proven on a restart, not only on the very first
        -- start before this loop existed. A bug isolated to start #2 onward
        -- (e.g. something that only registers correctly the first time)
        -- would leave every post-loop counter sitting at zero -- correct by
        -- accident -- and pass if liveness were only ever checked once.
        local live_i = counts()
        h.check(live_i.threads == 4, string.format("cycle %d: a restarted resource holds its four threads", i))
        h.check(live_i.handlers == 2, string.format("cycle %d: two event handlers", i))
        h.check(live_i.handler_names == 2, string.format("cycle %d: under two distinct event names", i))
        h.check(live_i.commands == 1, string.format("cycle %d: one command", i))
        h.check(live_i.exports == 1, string.format("cycle %d: one export", i))
        h.check(live_i.export_owners == 1, string.format("cycle %d: one resource holding exports", i))

        __rt.sched.tick(i * 16)
        __rt.res.stop("kitchen")
        check_cycle(i, counts())
    end

    local after = counts()
    h.check(after.threads == base.threads,
            string.format("threads return to baseline after 30 reloads (%d -> %d)",
                          base.threads, after.threads))
    h.check(after.handlers == base.handlers,
            string.format("handlers return to baseline (%d -> %d)", base.handlers, after.handlers))
    h.check(after.handler_names == base.handler_names,
            string.format("distinct event names return to baseline (%d -> %d)",
                          base.handler_names, after.handler_names))
    h.check(after.commands == base.commands,
            string.format("commands return to baseline (%d -> %d)", base.commands, after.commands))
    h.check(after.exports == base.exports,
            string.format("exports return to baseline (%d -> %d)", base.exports, after.exports))
    h.check(after.export_owners == base.export_owners,
            string.format("export owners return to baseline (%d -> %d)",
                          base.export_owners, after.export_owners))

    -- The record itself must not accumulate either. Checked immediately
    -- after start, before any tick runs: none of the four threads registered
    -- above can finish naturally in that window (CreateThread and on_tick
    -- loop forever, SetTimeout's delay is far past any `now` this test ever
    -- reaches, and CreateThreadNow's body is parked on its own Wait(50)), so
    -- the deferred "rec.threads is never pruned when a thread finishes
    -- naturally" imprecision has no room to hide behind here (Fix 4) --
    -- unlike a check taken after ticking, where a thread that legitimately
    -- finished mid-run would still count in rec.threads and require slack.
    -- == is honest at this specific point; it would not be after a tick.
    __rt.res.start("kitchen")
    local info = __rt.res.info("kitchen")
    __rt.res.stop("kitchen")
    h.check(info.threads == 4, "the record's own thread count does not grow across reloads")
    h.check(info.handlers == 2, "  nor its handler count")
    h.check(info.commands == 1, "  nor its command count")

    -- Fix 2: a second resource that collides with the first on both the
    -- event name and the command name. This is the exact shape of the bug
    -- fixed in 5980419 -- commands removed by name rather than the entry's
    -- own identity, so stopping one resource deleted a same-named command a
    -- DIFFERENT, still-running resource had registered. A single-resource
    -- test can never exercise that: there is nobody to victimise. Both
    -- directions are asserted -- the survivor staying intact would also be
    -- true of a teardown that silently does nothing at all, so the stopped
    -- resource's own registrations must be shown gone too.
    host.files["/data/rdr2lua/resources/pantry/fxmanifest.lua"] =
        "name 'pantry'\nclient_script 'main.lua'\n"
    host.files["/data/rdr2lua/resources/pantry/main.lua"] = [[
        AddEventHandler("evt", function() PANTRY_EVT_RAN = true end)
        exports("stuff", function() return 2 end)
        RegisterCommand("kitchencmd", function() PANTRY_CMD_RAN = true end)
    ]]

    __rt.res.start("kitchen")
    __rt.res.start("pantry")   -- registers "kitchencmd" second, replacing kitchen's entry at that key

    h.check(__rt.commands.count() == 1,
            "kitchen and pantry share one command name: there is exactly one live entry, not two")
    __rt.commands.run("kitchencmd")
    h.check(__rt.res.get("pantry").env.PANTRY_CMD_RAN == true,
            "with both running, the shared name dispatches to whoever registered it last (pantry)")

    __rt.res.stop("kitchen")

    -- The survivor: still live, and still actually working, not just present.
    h.check(__rt.res.state("pantry") == "started", "pantry is unaffected by kitchen stopping")
    h.check(__rt.commands.count() == 1, "pantry's command survives kitchen's teardown")
    h.check(__rt.exports_registry["pantry"] ~= nil, "pantry's export survives")
    __rt.res.get("pantry").env.PANTRY_CMD_RAN = false
    __rt.commands.run("kitchencmd")
    h.check(__rt.res.get("pantry").env.PANTRY_CMD_RAN == true,
            "the shared command name still reaches pantry after kitchen's teardown")
    __rt.res.get("pantry").env.PANTRY_EVT_RAN = false
    __rt.events.trigger("evt")
    h.check(__rt.res.get("pantry").env.PANTRY_EVT_RAN == true, "pantry's event handler still fires")

    -- The stopped one: actually torn down, not merely shadowed by pantry.
    h.check(__rt.exports_registry["kitchen"] == nil, "kitchen's export is gone")
    h.check(__rt.res.get("kitchen").env == nil, "kitchen's sandbox is torn down")
    h.check(__rt.events.count() == 1, "kitchen's own event handler is gone; only pantry's remains")

    __rt.res.stop("pantry")
end
