-- tests/lua/manifest_test.lua
return function(h)
    -- scheduler.lua owns the instruction budget the parser arms on its
    -- evaluation coroutine (rt.sched.arm_budget), so it has to be loaded even
    -- though nothing here schedules anything -- otherwise this file only
    -- passes when it happens to run after scheduler_test.
    package.loaded["scheduler"] = nil
    package.loaded["manifest"] = nil
    __rt = __rt or {}
    require("scheduler"); require("manifest")
    local M = __rt.manifest

    local m, err = M.parse([[
fx_version 'cerulean'
game 'gta5'
name 'vehiclespawner'
author 'dominik'
version '1.0.0'
client_scripts { 'lib/util.lua', 'main.lua' }
dependency 'hello'
]], "fxmanifest.lua")

    h.check(m ~= nil, "a well-formed manifest parses")
    h.check(m and m.name == "vehiclespawner", "  name is read")
    h.check(m and m.author == "dominik", "  author is read")
    h.check(m and m.version == "1.0.0", "  version is read")
    h.check(m and #m.scripts == 2, "  client_scripts collects both entries")
    h.check(m and m.scripts[1] == "lib/util.lua", "  in order")
    h.check(m and #m.dependencies == 1 and m.dependencies[1] == "hello", "  dependency is read")

    -- The three script directives are synonyms: there is no client/server split
    -- on a single-player console, but the synonyms let pasted FiveM code run.
    local m2 = M.parse("script 'a.lua'\nshared_script 'b.lua'\nclient_script 'c.lua'", "t")
    h.check(m2 and #m2.scripts == 3, "script/shared_script/client_script are synonyms")

    -- server_script is accepted and warned about, not silently dropped.
    local m3 = M.parse("client_script 'a.lua'\nserver_script 'b.lua'", "t")
    h.check(m3 ~= nil, "a manifest with server_script still parses")
    h.check(m3 and #m3.scripts == 1, "  the server script is not loaded")
    h.check(m3 and #m3.warnings == 1, "  and a warning is recorded")
    h.check(m3 and m3.warnings[1]:find("server_script") ~= nil, "  naming the directive")

    -- fx_version and game are accepted and ignored.
    local m4 = M.parse("fx_version 'cerulean'\ngame 'gta5'\nclient_script 'a.lua'", "t")
    h.check(m4 ~= nil and #m4.warnings == 0, "fx_version and game are accepted silently")

    -- A syntax error is an error, not a crash.
    local bad, berr = M.parse("client_script (", "t")
    h.check(bad == nil, "a malformed manifest is rejected")
    h.check(berr ~= nil and #berr > 0, "  with a message")

    -- An unknown directive is a warning, not a failure: FiveM manifests carry
    -- plenty we do not implement, and refusing them would reject real scripts.
    local m5 = M.parse("this_is_not_a_directive 'x'\nclient_script 'a.lua'", "t")
    h.check(m5 ~= nil, "an unknown directive does not fail the manifest")
    h.check(m5 and #m5.warnings == 1, "  but is warned about")

    -- The manifest sandbox must not reach the host.
    --
    -- `name(v)` stores tostring(v) using manifest.lua's OWN tostring --
    -- manifest.lua is host Lua, not sandboxed, so this is the real
    -- function -- which means m.name reports exactly what the directive
    -- received. That makes it a real probe: if `__rt` genuinely leaked into
    -- the sandbox (e.g. env's __index falling back to the true globals for
    -- an unknown key instead of a stub), name(__rt) would report a string
    -- naming a table. It does not, because `__rt` is not a key of `env`
    -- either and resolves through __index to a stub like anything else.
    --
    -- (The old version of this check called `tostring(__rt)` INSIDE the
    -- manifest, where `tostring` is itself a stub that always returns nil
    -- regardless of its argument -- so the check passed whether or not
    -- __rt leaked, and proved nothing.)
    local leak = M.parse("name(__rt)", "t")
    h.check(leak ~= nil, "referencing __rt from the manifest does not fail it")
    h.check(leak and leak.name and leak.name:find("table") == nil,
            "the manifest sandbox cannot see the real __rt: name(__rt) does not report a table")

    -- Pinned independently of the leak check above, so the two facts do not
    -- prop each other up: any name that is not a known directive -- a
    -- standard-library name like `tostring` is neither known nor special --
    -- resolves through __index to a callable stub, not a runtime error.
    local stub = M.parse("name(tostring)", "t")
    h.check(stub ~= nil, "referencing an unknown name (tostring) does not fail the manifest")
    h.check(stub and stub.name and stub.name:find("function") ~= nil,
            "  it resolves to a callable stub function, not an error and not the real tostring")

    -- A manifest cannot be allowed to hang the game: it runs on the game
    -- thread, inside the frame hook. A bare infinite loop has no directive
    -- call for anything to intercept, so only the general instruction
    -- budget can catch it -- it must still come back as a clean, prompt
    -- `nil, message` rather than freezing the test (and the game).
    local loopy, loopy_err = M.parse("while true do end", "t")
    h.check(loopy == nil, "an infinite loop in the manifest is rejected, not run forever")
    h.check(loopy_err ~= nil and loopy_err:find("budget") ~= nil,
            "  and the message says the manifest exceeded its instruction budget")

    -- _ENV is a magic upvalue, not a name lookup: it does NOT go through
    -- __index, so client_script(_ENV) hands the manifest the sandbox
    -- environment table itself. That table's __index returns a fresh
    -- non-nil stub for every key, including integers, so an ipairs-based
    -- add_scripts would spin forever walking it. The hardened, specific
    -- fix catches this immediately (a table carrying its own metatable is
    -- refused with a warning, never walked), so this never needs to reach
    -- the general budget above -- it must still resolve promptly either way.
    local envleak = M.parse("client_script(_ENV)", "t")
    h.check(envleak ~= nil, "client_script(_ENV) does not hang or crash the parser")
    h.check(envleak and #envleak.scripts == 0, "  and nothing from _ENV is added to scripts")
    h.check(envleak and #envleak.warnings == 1, "  a warning is recorded instead")

    -- Globbing is per-segment.
    h.check(M.match_glob("*.lua", "main.lua") == true, "*.lua matches main.lua")
    h.check(M.match_glob("*.lua", "main.txt") == false, "*.lua does not match main.txt")
    h.check(M.match_glob("main.lua", "main.lua") == true, "an exact name matches itself")
    h.check(M.match_glob("a*b", "axxb") == true, "a * matches in the middle")
    h.check(M.match_glob("*", "anything") == true, "a bare * matches anything")
    h.check(M.match_glob("*.lua", "sub/main.lua") == false, "* does not cross a separator")
end
