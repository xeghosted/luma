-- src/script/lua/boot.lua
-- Wires the runtime's own Lua together. Loaded last; every other chunk has
-- already populated __rt by the time this runs.
--
-- Chunks cannot require() each other: this Lua has no package library. They
-- share one table, __rt, which the C++ layer creates before loading anything.

-- __rt is created by the C++ layer before any chunk loads. Bind it locally:
-- the C++ layer removes the global later in this same file, once the host has
-- taken what it needs from it (see the scrub at the bottom), and a chunk that
-- reached for the global at runtime would then find nil.
local rt = __rt
rt.version = "m2a"

-- The shared globals every resource reads through. Resources write into their
-- own table; reads fall through to here, so they see the stdlib, the natives
-- and anything else already global. By the time any resource actually starts,
-- that no longer includes __rt and __host themselves -- the scrub at the
-- bottom of this file strips them from _G once the host has taken what it
-- needs. Nothing here gives a resource any special reach INTO either table
-- beyond that ordinary fallthrough, so a resource reading __rt sees nil like
-- any other cleared global.
local shared = _G

-- Build a resource's environment. Registration APIs are bound as closures over
-- the record rather than resolved through an ambient "current resource"
-- pointer: no ambient state to maintain around every resume, and
-- GetCurrentResourceName() inside an exported function then names the DEFINING
-- resource, which is what FiveM does.
function rt.make_env(rec)
    -- __metatable closes the casual escape: without it, getmetatable(_G) (_G
    -- inside a resource IS this env, see the assignment at the bottom of this
    -- function) hands back the real metatable, and
    -- getmetatable(_G).__index.FOO = 1 writes straight into `shared` from
    -- inside the sandbox. This is not a lockdown against determined code --
    -- debug.getmetatable walks straight past __metatable, and debug is itself
    -- one of the shared globals -- and it does not need to be: the isolation
    -- here exists so two resources that each forget a `local` do not clobber
    -- each other, not to defend against a script that goes looking for
    -- debug.getmetatable on purpose. These are the console owner's own
    -- scripts; accidents, not adversaries.
    local env = setmetatable({}, {
        __index = shared,
        __metatable = "the resource sandbox metatable is not accessible",
    })

    env.Citizen = {}

    -- Every registration below is refused once this record has been torn
    -- down, and refused BEFORE the underlying registration happens -- not
    -- after. "The resource has stopped" is not the same as "no more of its
    -- code runs": a callback that stopped its own resource is still running
    -- inside its own coroutine when teardown returns, and it keeps a live
    -- reference to this env. Refusing only at rt.res.track (which also
    -- refuses, as a backstop) would be too late for the ones with a side
    -- effect outside the record -- commands.add has already overwritten the
    -- global registry by then, and nothing will ever hold a token to undo it.
    -- That is the shape that left the command table permanently non-empty.
    --
    -- Logged, always. A registration that silently evaporates is its own
    -- undetectable failure: the script has no way to tell it apart from one
    -- that worked.
    local function refused(what)
        if not rec.dead then return false end
        local log = rt.log
        if log then
            log("[" .. tostring(rec.name) .. "] " .. what ..
                " ignored: this resource has already been torn down")
        end
        return true
    end

    local function create_thread(fn)
        if refused("CreateThread") then return nil end
        return rt.res.track(rec, "threads", rt.sched.create(fn, rec))
    end
    local function create_thread_now(fn)
        if refused("CreateThreadNow") then return false end
        -- create_now's second return is the scheduler handle for a body that
        -- yielded instead of finishing -- track it against this resource so
        -- teardown (and ClearTimeout) can reach it too, exactly like
        -- create_thread and set_timeout below. There is nothing to track when
        -- the body already ran to completion (handle is nil then). Scripts
        -- see only `ok`, matching FiveM's CreateThreadNow, which returns a
        -- plain boolean.
        local ok, handle = rt.sched.create_now(fn, rec)
        if handle then rt.res.track(rec, "threads", handle) end
        return ok
    end
    local function set_timeout(ms, fn)
        if refused("SetTimeout") then return nil end
        return rt.res.track(rec, "threads", rt.sched.set_timeout(ms, fn, rec))
    end

    env.CreateThread    = create_thread
    env.CreateThreadNow = create_thread_now
    env.Wait            = rt.sched.wait
    env.SetTimeout      = set_timeout
    env.ClearTimeout    = rt.sched.clear
    env.Citizen.CreateThread    = create_thread
    env.Citizen.CreateThreadNow = create_thread_now
    env.Citizen.Wait            = rt.sched.wait
    env.Citizen.SetTimeout      = set_timeout
    env.Citizen.ClearTimeout    = rt.sched.clear

    env.GetCurrentResourceName = function() return rec.name end

    -- on_tick survives as an alias so scripts written before the resource model
    -- keep working unchanged, exactly as the README promises.
    env.on_tick = function(fn)
        return create_thread(function()
            while true do fn(); rt.sched.wait(0) end
        end)
    end

    -- Events. Every registration is recorded against this resource, which is
    -- what makes stop() able to undo it.
    env.AddEventHandler = function(name, cb)
        if refused("AddEventHandler('" .. tostring(name) .. "')") then return nil end
        return rt.res.track(rec, "handlers", rt.events.add(name, cb, rec))
    end
    env.RemoveEventHandler = function(entry)
        rt.events.remove(entry)
    end
    env.TriggerEvent = function(name, ...) return rt.events.trigger(name, ...) end
    -- There is no network here. RegisterNetEvent exists so pasted FiveM code
    -- runs; it registers an ordinary local handler.
    env.RegisterNetEvent = function(name, cb)
        if cb then return env.AddEventHandler(name, cb) end
    end

    -- Exports. exports('name', fn) defines; exports.other.fn(...) calls. One
    -- lua_State means an export is a direct function reference, with no
    -- marshalling boundary to cross.
    local function define_export(fn_name, fn)
        if refused("exports('" .. tostring(fn_name) .. "')") then return end
        local t = rt.exports_registry[rec.name]
        if not t then t = {}; rt.exports_registry[rec.name] = t end
        t[tostring(fn_name)] = fn
        rt.res.track(rec, "exports", tostring(fn_name))
    end
    env.exports = setmetatable({}, {
        __call = function(_, fn_name, fn) define_export(fn_name, fn) end,
        __index = function(_, other)
            -- Built per access rather than cached: a resource can be restarted
            -- between two calls, and a cached table would keep resolving
            -- against the stopped one.
            local proxy
            proxy = setmetatable({}, {
                __index = function(_, fn_name)
                    local t = rt.exports_registry[other]
                    local fn = t and t[fn_name]
                    if not fn then
                        error(("no export '%s' on resource '%s' (is it started?)")
                              :format(tostring(fn_name), tostring(other)), 2)
                    end
                    -- Both exports.x.fn(a) and exports.x:fn(a) must work. The
                    -- colon form passes the proxy as the first argument; drop
                    -- it, and only it, by identity.
                    return function(first, ...)
                        if rawequal(first, proxy) then return fn(...) end
                        return fn(first, ...)
                    end
                end,
            })
            return proxy
        end,
    })

    -- Commands. The PS4 has no chat box, but the control channel is a console:
    -- the editor sends a line and it reaches the handler here.
    env.RegisterCommand = function(name, handler)
        if refused("RegisterCommand('" .. tostring(name) .. "')") then return nil end
        return rt.res.track(rec, "commands", rt.commands.add(name, handler, rec))
    end

    -- Lifecycle.
    env.GetResourceState = function(name) return rt.res.state(name) end
    env.StartResource    = function(name) return rt.res.start(name) end
    env.StopResource     = function(name) return rt.res.stop(name) end

    env._G = env
    return env
end

-- The host primitives, captured before they are removed from _G. A resource's
-- environment reads through _G, so anything left there would be reachable by
-- every resource -- file I/O most of all.
--
-- Guarded on __host being present, not run unconditionally: the pure-Lua
-- tests have no real host and instead set rt.host/rt.log directly, before
-- ever requiring this file, so that a resource's file reads reach their fake
-- one -- this must not stomp that back to nil.
if __host then
    rt.host = __host
    rt.log  = __host.log
end

-- Hand the C++ side its entry points, then scrub. After this the globals are
-- gone; every chunk already bound `local rt = __rt` at load time, so they are
-- unaffected, and no resource sandbox can reach either table.
--
-- The scrub is conditional on there being a real host to publish to. Under the
-- pure-Lua tests there is none, and the tests drive __rt directly.
if __host and __host.publish then
    __host.publish(rt)
    __host = nil
    __rt = nil
end
