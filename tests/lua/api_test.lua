-- tests/lua/api_test.lua
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

    host.dirs["/data/rdr2lua/resources"] = { "a", "b" }
    host.files["/data/rdr2lua/resources/a/fxmanifest.lua"] = "name 'a'\nclient_script 'a.lua'\n"
    host.files["/data/rdr2lua/resources/b/fxmanifest.lua"] = "name 'b'\nclient_script 'b.lua'\n"

    -- Events: an event fires every handler, and a handler runs in its own
    -- thread so a slow one cannot block the trigger. Both resources also
    -- register a command under the SAME name ("shared"): b's is a later
    -- registration than a's, and re-registration replaces rather than
    -- refuses (see api.lua's commands.add), so "shared" resolves to b's
    -- handler from the moment b starts, before anything ever calls it.
    host.files["/data/rdr2lua/resources/a/a.lua"] = [[
        SEEN = {}
        AddEventHandler("ping", function(x) SEEN[#SEEN+1] = "a:" .. tostring(x) end)
        exports("double", function(n) return n * 2 end)
        RegisterCommand("greet", function(source, args, raw) LAST_CMD = args[1] end)
        RegisterCommand("shared", function(source, args, raw) A_SHARED = true end)
    ]]
    host.files["/data/rdr2lua/resources/b/b.lua"] = [[
        AddEventHandler("ping", function(x) B_SAW = x end)
        CALLED = exports.a.double(21)
        COLON  = exports.a:double(10)
        WHO    = GetCurrentResourceName()
        RegisterCommand("shared", function(source, args, raw) B_SHARED = true end)
    ]]

    h.check(__rt.res.start("a") == true, "resource a starts")
    h.check(__rt.res.start("b") == true, "resource b starts")

    -- Exports are direct function references: one lua_State, no marshalling.
    local benv = __rt.res.get("b").env
    h.check(benv.CALLED == 42, "exports.a.double(21) returns 42")
    h.check(benv.COLON == 20, "the colon call form also works")
    h.check(benv.WHO == "b", "GetCurrentResourceName names the calling resource")

    -- GetCurrentResourceName inside an EXPORTED function names the DEFINING
    -- resource, because the API is bound as a closure over the record.
    local aenv = __rt.res.get("a").env
    h.check(aenv.GetCurrentResourceName() == "a", "  and the defining resource inside its own env")

    __rt.events.trigger("ping", 7)
    -- Handlers run immediately inside trigger(), via create_now, with no
    -- intervening tick: a TriggerEvent's effects must be visible to the code
    -- that triggered it, not a frame later, which is what FiveM does and what
    -- callers write against. Every OTHER event check in this file calls
    -- tick(0) before asserting, which would stay green even if a future
    -- refactor swapped create_now for create (fire on the next tick instead)
    -- -- this pair, with no tick in between, is what would catch that.
    h.check(#aenv.SEEN == 1 and aenv.SEEN[1] == "a:7", "an event's handler runs immediately, before any tick")
    h.check(benv.B_SAW == 7, "  every handler on the event runs immediately, not just the first")
    __rt.sched.tick(0)
    h.check(#aenv.SEEN == 1 and aenv.SEEN[1] == "a:7", "an event reaches resource a")
    h.check(benv.B_SAW == 7, "and resource b")

    -- RemoveEventHandler removes only the targeted registration, not every
    -- handler on the event.
    local dup1_ran, dup2_ran = false, false
    local dup1 = benv.AddEventHandler("dup", function() dup1_ran = true end)
    local dup2 = benv.AddEventHandler("dup", function() dup2_ran = true end)
    benv.RemoveEventHandler(dup1)
    __rt.events.trigger("dup")
    h.check(dup1_ran == false and dup2_ran == true, "RemoveEventHandler removes only the targeted handler")

    -- Commands.
    local ok = __rt.commands.run("greet world")
    __rt.sched.tick(0)
    h.check(ok == true, "a registered command runs")
    h.check(aenv.LAST_CMD == "world", "  and receives its arguments split")
    h.check(__rt.commands.run("nosuch") == false, "an unknown command reports failure")

    h.check(__rt.commands.run("GREET case") == true, "command names are case-insensitive")
    __rt.sched.tick(0)
    h.check(aenv.LAST_CMD == "case", "  and dispatch reaches the same handler regardless of case")

    h.check(__rt.commands.run("") == false, "an empty command reports failure")
    h.check(__rt.commands.run(nil) == false, "a non-string argument reports failure")

    -- A command handler that raises still reports success from run(): run()'s
    -- answer means "there was such a command to dispatch to", not "the
    -- handler completed without error" -- create_now already makes exactly
    -- this distinction for thread and event-handler errors (logged, not
    -- raised at the call site), and commands.run keeps it rather than
    -- changing it.
    __rt.commands.add("boom", function() error("boom") end, "test")
    h.check(__rt.commands.run("boom") == true, "a command whose handler raises still reports success")
    __rt.sched.tick(0)

    -- Stopping a resource takes its registrations with it.
    local handlers_before = __rt.events.count()
    __rt.res.stop("a")
    h.check(__rt.events.count() == handlers_before - 1, "stopping a resource removes exactly its handlers, no more and no less")
    h.check(__rt.commands.run("greet x") == false, "and its commands")
    h.check(__rt.exports_registry["a"] == nil, "and its exports")

    -- Commands are keyed by name but removed by entry identity: b's "shared"
    -- registration replaced a's at load time, before "shared" was ever run,
    -- so a's teardown token is stale and stopping a must not be able to
    -- reach into a live command it no longer owns.
    h.check(__rt.commands.run("shared") == true, "a command name a stopped resource once held still runs, if another resource re-registered it")
    __rt.sched.tick(0)
    h.check(benv.B_SHARED == true, "  and it is the surviving resource's handler that ran")

    -- An export of a stopped resource is a clear error, not a nil call.
    local okc, errc = pcall(function() return benv.exports.a.double(1) end)
    h.check(okc == false, "calling an export of a stopped resource raises")
    h.check(tostring(errc):find("export 'double' on resource 'a'", 1, true) ~= nil, "  naming the resource and the export together")

    -- RegisterNetEvent exists for pasted FiveM code and behaves locally.
    host.files["/data/rdr2lua/resources/a/a.lua"] = [[
        NET_SEEN = 0
        RegisterNetEvent("netping", function() NET_SEEN = NET_SEEN + 1 end)
    ]]
    __rt.res.start("a")
    __rt.events.trigger("netping")
    __rt.sched.tick(0)
    h.check(__rt.res.get("a").env.NET_SEEN == 1, "RegisterNetEvent registers a local handler")

    -- A raising handler does not stop the others on the same event.
    host.files["/data/rdr2lua/resources/b/b.lua"] = [[
        AddEventHandler("mixed", function() error("handler boom") end)
        AddEventHandler("mixed", function() MIXED_OK = true end)
    ]]
    __rt.res.restart("b")
    __rt.events.trigger("mixed")
    __rt.sched.tick(0)
    h.check(__rt.res.get("b").env.MIXED_OK == true, "a raising handler does not stop the next one")
end
