-- tests/lua/resource_test.lua
return function(h)
    for _, m in ipairs({ "scheduler", "manifest", "resource", "boot" }) do package.loaded[m] = nil end
    __rt = {}
    require("scheduler"); require("manifest")

    local host = require("harness").fake_host()
    -- The host hands the resource root in: it is per game now, so
    -- resource.lua cannot bake one in.
    __rt.resource_root = "/data/rdr2lua/resources"
    __rt.host = host
    __rt.log = host.log
    require("resource"); require("boot")   -- make_env lives in boot.lua
    local R = __rt.res

    host.dirs["/data/rdr2lua/resources"] = { "demo" }
    host.files["/data/rdr2lua/resources/demo/fxmanifest.lua"] =
        "name 'demo'\nclient_script 'main.lua'\n"
    host.files["/data/rdr2lua/resources/demo/main.lua"] = [[
        DEMO_RAN = (DEMO_RAN or 0) + 1
        CreateThread(function() while true do Wait(0) end end)
    ]]

    h.check(R.state("demo") == "stopped", "a present but unstarted resource reads as stopped")
    h.check(R.state("nope") == "missing", "an absent resource reads as missing")

    local ok, err = R.start("demo")
    h.check(ok == true, "the resource starts" .. (err and (" (" .. tostring(err) .. ")") or ""))
    h.check(R.state("demo") == "started", "  and reports started")

    local info = R.info("demo")
    h.check(info ~= nil and info.threads == 1, "its CreateThread is tracked against it")

    -- The sandbox: a resource's globals are its own, not shared.
    local ok2 = R.start("demo")
    h.check(ok2 == false, "starting an already-started resource is refused")

    R.stop("demo")
    h.check(R.state("demo") == "stopped", "stop returns it to stopped")
    h.check(__rt.sched.count() == 0, "and its threads are gone from the scheduler")
    local after = R.info("demo")
    h.check(after ~= nil and after.threads == 0, "its tracked thread count is zero")

    -- Writes from a resource do not reach the shared globals.
    h.check(rawget(_G, "DEMO_RAN") == nil, "a resource's global write stays in its sandbox")

    -- Reads fall through to the shared globals.
    host.files["/data/rdr2lua/resources/demo/main.lua"] =
        "SAW_TYPE = type(print)\nCreateThread(function() Wait(0) end)"
    R.start("demo")
    h.check(R.get("demo").env.SAW_TYPE == "function", "a resource can read the shared globals")

    -- The metatable guard: getmetatable() must not hand back the real
    -- metatable, or getmetatable(_G).__index.FOO = 1 would write straight
    -- into the shared globals from inside the sandbox (_G inside a resource
    -- IS its env -- see the assignment at the bottom of make_env).
    local demo_env = R.get("demo").env
    h.check(type(getmetatable(demo_env)) ~= "table",
        "getmetatable on a resource's env does not hand back the real metatable table")
    local mt_ok = pcall(setmetatable, demo_env, {})
    h.check(mt_ok == false, "the sandbox's metatable cannot be replaced from inside a resource")

    -- Task 8 will have the C++ layer strip __rt (and __host) from the shared
    -- globals once boot.lua has run; nothing built so far does that. Pin what
    -- should already be true regardless: an env has no special path to __rt
    -- beyond the ordinary shared-globals fallthrough, so clearing the real
    -- global here previews exactly what that future scrub will produce.
    local saved_rt = __rt
    __rt = nil
    h.check(R.get("demo").env.__rt == nil,
        "clearing the shared __rt global leaves a resource's env unable to see it")
    __rt = saved_rt

    R.stop("demo")

    -- A resource that fails halfway leaves nothing behind: start is
    -- transactional, or a half-registered resource would keep ticking with no
    -- way to stop it by name.
    host.files["/data/rdr2lua/resources/broken/fxmanifest.lua"] =
        "name 'broken'\nclient_scripts { 'a.lua', 'b.lua' }\n"
    host.files["/data/rdr2lua/resources/broken/a.lua"] =
        "CreateThread(function() while true do Wait(0) end end)"
    host.files["/data/rdr2lua/resources/broken/b.lua"] = "this is not lua"
    local bok, berr = R.start("broken")
    h.check(bok == false, "a resource whose second file fails does not start")
    h.check(berr ~= nil and #berr > 0, "  and says why")
    h.check(R.state("broken") == "error", "  its state is error")
    h.check(__rt.sched.count() == 0, "  and the thread its first file created is gone")

    -- A missing resource is an error, not a crash.
    local mok, merr = R.start("nope")
    h.check(mok == false and merr ~= nil, "starting a missing resource errors cleanly")

    -- A manifest entry naming a file that just isn't there is its own failure
    -- mode, distinct from "broken" above (whose file exists but fails to
    -- compile): read_file returns nil and R.start must say which file.
    host.files["/data/rdr2lua/resources/missingfile/fxmanifest.lua"] =
        "name 'missingfile'\nclient_script 'ghost.lua'\n"
    local mfok, mferr = R.start("missingfile")
    h.check(mfok == false, "a manifest referencing a script file that doesn't exist does not start")
    h.check(mferr == "missing file: ghost.lua", "  and names the missing file")
    h.check(R.state("missingfile") == "error", "  its state is error")

    -- A manifest whose only script pattern matches nothing at all -- as
    -- opposed to one glob among several coming up empty, covered below --
    -- leaves no files to run and must fail cleanly rather than "start"
    -- something with nothing in it.
    host.files["/data/rdr2lua/resources/noscripts/fxmanifest.lua"] =
        "name 'noscripts'\nclient_script 'nope/*.lua'\n"
    local nsok, nserr = R.start("noscripts")
    h.check(nsok == false, "a manifest whose only script pattern matches nothing does not start")
    h.check(nserr == "manifest lists no script files", "  and says there were no script files")
    h.check(R.state("noscripts") == "error", "  its state is error, not stopped")
    -- ...and the record is torn down, like every other start failure. This
    -- was the one failure path in R.start that skipped teardown, so rec.env
    -- -- the entire per-resource sandbox table -- stayed alive on a record
    -- nothing would ever sweep again. Nothing above this point runs a line of
    -- resource code, so there is nothing registered to undo and no counter
    -- that moves: the env is the whole leak, and only an env check sees it.
    h.check(R.get("noscripts").env == nil, "  and its sandbox is torn down, not left alive")

    -- A manifest that exists but fails to PARSE (as opposed to being absent)
    -- must also read back as "error", not "stopped" -- state() must not fall
    -- back to its disk probe, which only knows the file is there, not that it
    -- is broken.
    host.files["/data/rdr2lua/resources/badmanifest/fxmanifest.lua"] = "client_script ("
    local pok, perr = R.start("badmanifest")
    h.check(pok == false, "a resource whose manifest fails to parse does not start")
    h.check(perr ~= nil and #perr > 0, "  and says why")
    h.check(R.state("badmanifest") == "error",
        "  and its state is error, not stopped, even though fxmanifest.lua exists on disk")

    -- Globbing: several files match, in sorted order (not directory-listing
    -- order, which the fake host deliberately scrambles); a second pattern in
    -- the same manifest that matches nothing contributes nothing, but does
    -- not fail the whole resource.
    GLOB_ORDER = {}
    host.dirs["/data/rdr2lua/resources/globby/scripts"] = { "c.lua", "a.lua", "b.lua" }
    host.files["/data/rdr2lua/resources/globby/fxmanifest.lua"] =
        "name 'globby'\nclient_scripts { 'scripts/*.lua', 'missing/*.lua' }\n"
    host.files["/data/rdr2lua/resources/globby/scripts/a.lua"] = "GLOB_ORDER[#GLOB_ORDER + 1] = 'a'"
    host.files["/data/rdr2lua/resources/globby/scripts/b.lua"] = "GLOB_ORDER[#GLOB_ORDER + 1] = 'b'"
    host.files["/data/rdr2lua/resources/globby/scripts/c.lua"] = "GLOB_ORDER[#GLOB_ORDER + 1] = 'c'"
    local gok, gerr = R.start("globby")
    h.check(gok == true, "a resource with a glob pattern matching several files starts" ..
        (gerr and (" (" .. tostring(gerr) .. ")") or ""))
    h.check(#GLOB_ORDER == 3 and GLOB_ORDER[1] == "a" and GLOB_ORDER[2] == "b" and GLOB_ORDER[3] == "c",
        "the matched files load in sorted order, not directory-listing order")
    R.stop("globby")
    GLOB_ORDER = nil

    -- A real directory listing is not guaranteed to omit '.' and '..' --
    -- R.list() below already has to filter both out itself for exactly that
    -- reason. expand() must defend against them too: without that, a plain
    -- 'client_script '*'' over a listing of {"..", ".", "main.lua"} fails
    -- with "missing file: .", even though nothing about the manifest itself
    -- was unsafe.
    host.dirs["/data/rdr2lua/resources/dotdot"] = { "..", ".", "main.lua" }
    host.files["/data/rdr2lua/resources/dotdot/fxmanifest.lua"] = [[
        name 'dotdot'
        client_script '*'
    ]]
    host.files["/data/rdr2lua/resources/dotdot/main.lua"] = "DOTDOT_RAN = true"
    local ddok, dderr = R.start("dotdot")
    h.check(ddok == true, "a '*' pattern over a listing containing '.' and '..' still starts" ..
        (dderr and (" (" .. tostring(dderr) .. ")") or ""))
    local dotdot_env = R.get("dotdot") and R.get("dotdot").env
    h.check(dotdot_env ~= nil and dotdot_env.DOTDOT_RAN == true,
        "  loading only the real file, not '.' or '..'")
    R.stop("dotdot")

    -- A manifest cannot read outside its own resource directory. '..' as a
    -- whole path segment is an escape (whether in a literal entry or in a
    -- glob's directory prefix); a name that merely starts with dots is not.
    host.files["/data/rdr2lua/resources/escapee/../../GoldHEN/plugins.ini"] = "ESCAPED = true"
    host.dirs["/data/rdr2lua/resources/escapee/.."] = { "secret.lua" }
    host.files["/data/rdr2lua/resources/escapee/../secret.lua"] = "SECRET_RAN = true"
    host.files["/data/rdr2lua/resources/escapee/fxmanifest.lua"] =
        "name 'escapee'\nclient_scripts { '../../GoldHEN/plugins.ini', '../*.lua', 'main.lua' }\n"
    host.files["/data/rdr2lua/resources/escapee/main.lua"] = "ESCAPEE_RAN = true"
    local eok, eerr = R.start("escapee")
    h.check(eok == true, "a resource with escaping script entries still starts, using only its safe entries" ..
        (eerr and (" (" .. tostring(eerr) .. ")") or ""))
    local escapee_env = R.get("escapee") and R.get("escapee").env
    h.check(escapee_env ~= nil and escapee_env.ESCAPEE_RAN == true, "  and its legitimate script still ran")
    h.check(escapee_env ~= nil and escapee_env.ESCAPED == nil,
        "a '..'-prefixed literal script entry is not read, even though the file exists on disk")
    h.check(escapee_env ~= nil and escapee_env.SECRET_RAN == nil,
        "a glob pattern whose directory segment is '..' is not listed either")
    local saw_literal_warning, saw_glob_warning = false, false
    for _, msg in ipairs(host.logged) do
        if msg:find("GoldHEN/plugins.ini", 1, true) then saw_literal_warning = true end
        if msg:find("../*.lua", 1, true) then saw_glob_warning = true end
    end
    h.check(saw_literal_warning, "the literal escape attempt is named in a warning")
    h.check(saw_glob_warning, "the glob escape attempt is named in a warning too")
    R.stop("escapee")

    -- SetTimeout and on_tick both funnel through create_thread/track like
    -- CreateThread does, so they are torn down the same way.
    host.files["/data/rdr2lua/resources/timers/fxmanifest.lua"] =
        "name 'timers'\nclient_script 'main.lua'\n"
    host.files["/data/rdr2lua/resources/timers/main.lua"] =
        "SetTimeout(1000, function() end)\non_tick(function() end)"
    local tok = R.start("timers")
    h.check(tok == true, "a resource using SetTimeout and on_tick starts")
    local tinfo = R.info("timers")
    h.check(tinfo ~= nil and tinfo.threads == 2, "SetTimeout and on_tick are both tracked against the resource")
    R.stop("timers")
    h.check(R.info("timers").threads == 0, "  and torn down along with everything else")

    -- CreateThreadNow must be tracked too: a leaked path here is harmless for
    -- threads (teardown sweeps by owner), but the same wrapper shape is what
    -- events/exports/commands will rely on next task, where there is no
    -- equivalent sweep.
    host.files["/data/rdr2lua/resources/nowres/fxmanifest.lua"] =
        "name 'nowres'\nclient_script 'main.lua'\n"
    host.files["/data/rdr2lua/resources/nowres/main.lua"] =
        "CreateThreadNow(function() Wait(50) end)"
    local nwok = R.start("nowres")
    h.check(nwok == true, "a resource using CreateThreadNow starts")
    h.check(R.info("nowres").threads == 1, "a CreateThreadNow body that yields is tracked against the resource")
    R.stop("nowres")
    h.check(__rt.sched.count() == 0, "  and its continuation is gone from the scheduler too")

    -- A CreateThreadNow body that finishes immediately has no handle to
    -- track (and must not add a bogus entry for one).
    host.files["/data/rdr2lua/resources/nowres2/fxmanifest.lua"] =
        "name 'nowres2'\nclient_script 'main.lua'\n"
    host.files["/data/rdr2lua/resources/nowres2/main.lua"] =
        "CreateThreadNow(function() end)"
    local nw2ok = R.start("nowres2")
    h.check(nw2ok == true, "a resource using CreateThreadNow with a body that finishes immediately also starts")
    h.check(R.info("nowres2").threads == 0, "  and there is nothing to track since the body never yielded")
    R.stop("nowres2")

    -- restart is stop-then-start and works from either state.
    local sok, serr = R.start("demo")
    h.check(sok == true, "demo starts again ahead of the restart checks" ..
        (serr and (" (" .. tostring(serr) .. ")") or ""))
    local rok = R.restart("demo")
    h.check(rok == true, "restart works on a started resource")
    h.check(R.state("demo") == "started", "  leaving it started")
    R.stop("demo")
    h.check(R.restart("demo") == true, "restart also works on a stopped resource")
    R.stop("demo")

    -- list() reports every resource on disk with its state.
    --
    -- "broken" is added to the listing here rather than left out of it. Every
    -- resource in this file has its FILES in host.files but only "demo" was
    -- ever in host.dirs, which no real console can reproduce: a directory
    -- holding an fxmanifest.lua is a directory getdents returns. This check
    -- used to lean on that inconsistency to assert that a record with no
    -- directory entry is listed anyway -- and that is now deliberately no
    -- longer true for a record in a terminal state, which R.list forgets. The
    -- orphaned-record rules have their own checks at the end of this file;
    -- what belongs here is that a resource on disk whose manifest failed to
    -- parse is reported with its real state rather than hidden.
    host.dirs["/data/rdr2lua/resources"] = { "demo", "broken" }
    local l = R.list()
    local seen = {}
    for _, e in ipairs(l) do seen[e.name] = e.state end
    h.check(seen["demo"] == "stopped", "list reports a stopped resource")
    h.check(seen["broken"] == "error", "  and one whose manifest failed to parse, as error")

    -- A resource NAME is one path segment, not a path: resource_dir()
    -- concatenates it straight into ROOT .. "/" .. name, so a name containing
    -- '/', '\', or equal to '..' would read (and, via start, execute) outside
    -- the resources directory the same way an unguarded file path would.
    -- Fixed in this round: previously nothing validated the name at all, and
    -- the very next task wires start/stop/restart to the control channel,
    -- where a name arrives over the network unvetted. Checked at every entry
    -- point that takes one, not just start, since state()/info() also build
    -- a path or a lookup from it.
    local esok, eserr = R.start("../../GoldHEN")
    h.check(esok == false and eserr == "invalid resource name",
        "a name containing '/' is refused by start, with a clear reason")
    h.check(R.state("../../GoldHEN") == "missing",
        "  state() refuses it too, reading it as missing rather than probing the escaped path")
    h.check(R.info("../../GoldHEN") == nil, "  info() refuses it too")
    local stok, sterr = R.stop("../../GoldHEN")
    h.check(stok == false and sterr == "invalid resource name", "  stop() refuses it too")
    local rsok, rserr = R.restart("../../GoldHEN")
    h.check(rsok == false and rserr == "invalid resource name", "  restart() refuses it too")

    local dotok = R.start(".")
    h.check(dotok == false, "a bare '.' is refused too, alongside '..'")
    h.check(R.state(".") == "missing", "  and reads as missing")

    -- valid_name rejects "." and ".." exactly, not by pattern: a name that
    -- merely contains dots, or an unusual character that is not a path
    -- separator, is a real directory name and must not be over-rejected.
    host.files["/data/rdr2lua/resources/.../fxmanifest.lua"] =
        "name '...'\nclient_script 'main.lua'\n"
    host.files["/data/rdr2lua/resources/.../main.lua"] = "DOTS_RAN = true"
    local dotsok, dotserr = R.start("...")
    h.check(dotsok == true, "a name of three dots is a real name, not an escape, and still starts" ..
        (dotserr and (" (" .. tostring(dotserr) .. ")") or ""))
    R.stop("...")

    host.files["/data/rdr2lua/resources/trail /fxmanifest.lua"] =
        "name 'trail '\nclient_script 'main.lua'\n"
    host.files["/data/rdr2lua/resources/trail /main.lua"] = "TRAIL_RAN = true"
    local spok, sperr = R.start("trail ")
    h.check(spok == true, "a name with a trailing space is unusual but not an escape, and still starts" ..
        (sperr and (" (" .. tostring(sperr) .. ")") or ""))
    R.stop("trail ")

    local ddok2 = R.start("..")
    h.check(ddok2 == false, "a bare '..' is refused, not resolved one directory up")
    h.check(R.state("..") == "missing", "  and reads as missing")

    local bsok = R.start("a\\b")
    h.check(bsok == false, "a name containing a backslash is refused")

    local emok = R.start("")
    h.check(emok == false, "an empty name is refused")

    local nlok = R.start(nil)
    h.check(nlok == false, "a non-string name is refused rather than erroring")
    h.check(R.state(nil) == "missing", "  and state() on a non-string name reads as missing rather than erroring")

    -- None of the above should have touched the sandbox for an ordinary name.
    local nmok, nmerr = R.start("demo")
    h.check(nmok == true, "an ordinary resource name still starts" ..
        (nmerr and (" (" .. tostring(nmerr) .. ")") or ""))
    h.check(R.state("demo") == "started", "  and reports started")
    R.stop("demo")

    -- ------------------------------------------------------------------
    -- The wire's name limit, and the Lua side agreeing with it.
    -- ------------------------------------------------------------------
    -- Found by running it: a 183-character resource directory was listed by
    -- RESLIST as "stopped" -- that is, as something you could start -- while
    -- START, STOP, RESTART and RESINFO every one of them answered "payload
    -- too long", because their handlers decode a name into a fixed buffer.
    -- The runtime must not advertise what it cannot act on, so the limit
    -- belongs with the other name rules rather than only at the wire.
    local NAME_MAX = 127

    -- Bound to net::RESOURCE_NAME_MAX, not merely equal to it today: both
    -- suites read this one number out of the fixture file, the same rule the
    -- opcode tables follow. Changing one side alone turns the other red
    -- instead of quietly reintroducing the disagreement.
    local fx = io.open("tests/protocol_vectors.json", "r")
    h.check(fx ~= nil, "protocol_vectors.json is readable from the repo root")
    if fx then
        local fxsrc = fx:read("a"); fx:close()
        local fixture_max = tonumber(fxsrc:match('"resource_name_max"%s*:%s*(%d+)'))
        h.check(fixture_max == NAME_MAX,
            "  and its resource_name_max is the limit valid_name enforces")
    end

    local at_limit   = string.rep("n", NAME_MAX)
    local over_limit = string.rep("n", NAME_MAX + 1)

    host.files["/data/rdr2lua/resources/" .. at_limit .. "/fxmanifest.lua"] =
        "name 'atlimit'\nclient_script 'main.lua'\n"
    host.files["/data/rdr2lua/resources/" .. at_limit .. "/main.lua"] = "AT_LIMIT_RAN = true"
    local alok, alerr = R.start(at_limit)
    h.check(alok == true, "a name of exactly the wire limit still starts" ..
        (alerr and (" (" .. tostring(alerr) .. ")") or ""))
    R.stop(at_limit)

    host.files["/data/rdr2lua/resources/" .. over_limit .. "/fxmanifest.lua"] =
        "name 'over'\nclient_script 'main.lua'\n"
    host.files["/data/rdr2lua/resources/" .. over_limit .. "/main.lua"] = "OVER_RAN = true"
    local olok, olerr = R.start(over_limit)
    h.check(olok == false and olerr == "invalid resource name",
        "a name one byte past the wire limit is refused rather than started")
    h.check(R.info(over_limit) == nil, "  info() refuses it too")

    -- The listing is the actual defect: this is what the console saw.
    host.dirs["/data/rdr2lua/resources"] = { over_limit }
    local over_state
    for _, e in ipairs(R.list()) do
        if e.name == over_limit then over_state = e.state end
    end
    h.check(over_state == "missing",
        "  and it is never listed as a startable 'stopped'")

    -- ------------------------------------------------------------------
    -- A record whose directory disappeared underneath it.
    -- ------------------------------------------------------------------
    local function listed(name)
        for _, e in ipairs(R.list()) do if e.name == name then return e.state end end
        return nil
    end

    host.dirs["/data/rdr2lua/resources"] = { "ghost" }
    host.files["/data/rdr2lua/resources/ghost/fxmanifest.lua"] =
        "name 'ghost'\nclient_script 'main.lua'\n"
    host.files["/data/rdr2lua/resources/ghost/main.lua"] =
        "CreateThread(function() while true do Wait(0) end end)"
    h.check(R.start("ghost") == true, "a resource that is about to be orphaned starts")

    -- Deleted while it is STILL RUNNING.
    host.dirs["/data/rdr2lua/resources"] = {}
    host.files["/data/rdr2lua/resources/ghost/fxmanifest.lua"] = nil
    host.files["/data/rdr2lua/resources/ghost/main.lua"] = nil

    h.check(listed("ghost") == "started",
        "a started resource whose directory is deleted is still listed")
    h.check(__rt.sched.count() > 0, "  because its threads really are still running")

    R.stop("ghost")
    h.check(listed("ghost") == nil,
        "once stopped, the orphaned record is forgotten instead of listed forever")
    h.check(R.get("ghost") == nil, "  and the record is gone, not merely hidden from the listing")

    -- A FAILED listing is not an empty one. list_dir answers nil when the
    -- directory cannot be read at all, and reading that as "every resource
    -- was deleted" would forget every record there is on one bad call.
    host.dirs["/data/rdr2lua/resources"] = { "keep" }
    host.files["/data/rdr2lua/resources/keep/fxmanifest.lua"] =
        "name 'keep'\nclient_script 'main.lua'\n"
    host.files["/data/rdr2lua/resources/keep/main.lua"] = "KEEP_RAN = true"
    h.check(R.start("keep") == true, "a resource to survive an unreadable listing starts")
    R.stop("keep")
    h.check(R.get("keep") ~= nil, "  and is stopped but still recorded")

    host.dirs["/data/rdr2lua/resources"] = nil      -- list_dir now answers nil
    R.list()
    h.check(R.get("keep") ~= nil,
        "an unreadable resource directory forgets nothing")
    host.dirs["/data/rdr2lua/resources"] = { "keep" }
end
