-- src/script/lua/manifest.lua
-- fxmanifest.lua is Lua, evaluated in a sandbox where each directive is a
-- function -- FiveM's own trick, which is why a real FiveM manifest works here.
--
-- The sandbox is deliberately tiny: a manifest is data written by whoever wrote
-- the resource, and it must not be able to reach the runtime.

local rt = __rt

local M = {}

-- Glob over ONE path segment: '*' matches any run of characters except '/'.
function M.match_glob(pattern, name)
    if pattern == name then return true end
    if not pattern:find("*", 1, true) then return false end
    local lua_pat = "^" .. pattern:gsub("[%^%$%(%)%%%.%[%]%+%-%?]", "%%%1"):gsub("%*", "[^/]*") .. "$"
    return name:match(lua_pat) ~= nil
end

-- A manifest runs on the game thread, inside the frame hook: it must not be
-- able to run forever. The budget that bounds it now lives in scheduler.lua
-- (rt.sched.arm_budget), where the resource script bodies and the scheduler's
-- own resumes -- which had no bound at all until this round -- share the same
-- mechanism and the same constant instead of each carrying a copy. Read off
-- rt at call time, not bound here: scheduler.lua loads first, but the tests
-- rebuild __rt between files.

function M.parse(src, chunkname)
    local m = { scripts = {}, dependencies = {}, warnings = {} }

    -- Collects a string, or a table of strings, into `list`. `ipairs` is
    -- unsafe here: the sandbox's __index (below) returns a fresh, non-nil
    -- stub for EVERY key it is asked for, including integers, so `ipairs`
    -- over a table sharing that metatable never finds a nil and never
    -- terminates. `_ENV` is exactly such a table, and it is reachable
    -- directly: `_ENV` is a magic upvalue, not a name lookup, so a manifest
    -- doing `client_script(_ENV)` hands this function the sandbox
    -- environment itself without ever going through __index.
    --
    -- `#v` reads a table's raw array part, which no __index metamethod can
    -- influence, so it terminates regardless of what __index would return.
    -- A table that carries its own metatable (again, _ENV, but this also
    -- catches any manifest-constructed table that somehow acquired one) is
    -- refused outright with a warning rather than walked at all -- its raw
    -- part says nothing trustworthy about what the manifest intended.
    local function collect_strings(list, v, directive_name)
        if type(v) == "string" then
            list[#list + 1] = v
            return
        end
        if type(v) == "table" then
            if getmetatable(v) ~= nil then
                m.warnings[#m.warnings + 1] = directive_name ..
                    ": expected a string or a table of strings, got a table with a metatable"
                return
            end
            for i = 1, #v do
                local s = v[i]
                if type(s) == "string" then
                    list[#list + 1] = s
                else
                    m.warnings[#m.warnings + 1] = directive_name ..
                        ": entry " .. i .. " is not a string, ignoring"
                end
            end
            return
        end
        if v ~= nil then
            m.warnings[#m.warnings + 1] = directive_name .. ": expected a string or a table of strings"
        end
    end

    local directives = {
        name        = function(v) m.name = tostring(v) end,
        author      = function(v) m.author = tostring(v) end,
        version     = function(v) m.version = tostring(v) end,
        description = function(v) m.description = tostring(v) end,
        -- Accepted and ignored: they exist so a FiveM manifest parses unchanged.
        fx_version  = function() end,
        game        = function() end,
        games       = function() end,
    }
    for _, k in ipairs({ "script", "scripts", "client_script", "client_scripts",
                         "shared_script", "shared_scripts" }) do
        directives[k] = function(v) collect_strings(m.scripts, v, k) end
    end
    for _, k in ipairs({ "dependency", "dependencies" }) do
        directives[k] = function(v) collect_strings(m.dependencies, v, k) end
    end
    for _, k in ipairs({ "server_script", "server_scripts" }) do
        directives[k] = function()
            m.warnings[#m.warnings + 1] =
                "server_script ignored: there is no server on a single-player console"
        end
    end

    -- `env` never gains a raw key of its own: every directive call, known or
    -- not, is dispatched through __index below, and every assignment --
    -- including a manifest trying to reassign one of its own directives,
    -- e.g. `client_script = function() end` -- is silently dropped by
    -- __newindex. Blocking that reassignment is deliberate: __newindex only
    -- fires for keys a table does not already have, so if directives were
    -- pre-populated as raw fields (as an earlier version of this file did),
    -- a manifest could quietly overwrite one and disable it for the rest of
    -- that manifest's evaluation with no sign anything went wrong. A real
    -- FiveM manifest never does this, and routing every lookup through
    -- __index instead of pre-populating raw keys closes it off entirely --
    -- both known and unknown keys are handled by the same two metamethods.
    --
    -- Anything that is not a known directive -- including standard-library
    -- names like `tostring` or `type`, and `__rt` itself, none of which are
    -- keys of `env` -- falls through to a warning stub instead of the real
    -- thing or a runtime error. That is what keeps the sandbox a sandbox: a
    -- manifest is data, and nothing inside it can reach the runtime.
    -- `error` falls through the same way, on purpose: a manifest cannot
    -- deliberately abort parsing with a custom message, but it does not
    -- need to -- every failure this parser recognises (a syntax error, a
    -- run past the instruction budget) is already reported as a clean
    -- `nil, message`.
    local env = setmetatable({}, {
        __index = function(_, k)
            local fn = directives[k]
            if fn then return fn end
            return function()
                m.warnings[#m.warnings + 1] = "unknown manifest directive: " .. tostring(k)
            end
        end,
        __newindex = function() end,
    })

    local chunk, err = load(src, "@" .. (chunkname or "fxmanifest.lua"), "t", env)
    if not chunk then return nil, err end

    -- Run inside a coroutine, under an instruction-count hook, so a runaway
    -- manifest is stopped after INSTRUCTION_BUDGET instructions instead of
    -- hanging the thread that is evaluating it. This is the general net:
    -- collect_strings above is the specific, comprehensible answer for the
    -- one runaway shape this parser can name in advance (an ipairs-unsafe
    -- table reaching a script directive); the budget covers everything else,
    -- named or not -- most simply, `while true do end` at manifest top level.
    local co = coroutine.create(chunk)
    rt.sched.arm_budget(co)
    local ok, rerr = coroutine.resume(co)
    if not ok then return nil, "manifest " .. tostring(rerr) end
    if coroutine.status(co) ~= "dead" then
        return nil, "manifest did not run to completion (yielded)"
    end
    return m
end

rt.manifest = M
return M
