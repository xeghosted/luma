-- src/script/lua/api.lua
-- Events, exports and commands: the three things a resource registers besides
-- threads. All three are undone by resource.lua's teardown, and all three are
-- recorded through __rt.res.track so there is exactly one registration path.

local rt = __rt

--------------------------------------------------------------------------- events

local events = {}
local handlers = {}     -- name -> { entry, ... }; entry = { name, cb, owner }

function events.add(name, cb, owner)
    local list = handlers[name]
    if not list then list = {}; handlers[name] = list end
    local entry = { name = name, cb = cb, owner = owner }
    list[#list + 1] = entry
    return entry
end

function events.remove(entry)
    if not entry then return end
    -- Retired by IDENTITY, before the list surgery and whether or not the
    -- entry is still in a list at all. events.trigger has to dispatch over a
    -- snapshot (see below), and a snapshot can hold an entry that a nested
    -- teardown unregistered a moment ago; this flag is the only thing that
    -- lets it tell. It is the same token rule the rest of this file follows:
    -- a removal only ever affects the entry its caller holds.
    entry.removed = true
    local list = handlers[entry.name]
    if not list then return end
    for i = #list, 1, -1 do
        if list[i] == entry then table.remove(list, i) end
    end
    -- Drop the emptied list rather than leave a dead table parked under its
    -- name forever. `handlers` is keyed by event NAME and nothing else ever
    -- removes a key from it, so without this a console grows one entry per
    -- distinct name a resource has ever handled and never gives one back --
    -- and events.count() sums list LENGTHS, so it reads 0 the whole time.
    -- events.name_count() is what can see this, and leak_test.lua now asserts
    -- on it every cycle.
    if #list == 0 then handlers[entry.name] = nil end
end

-- Each handler runs in its own thread, started immediately: FiveM dispatches
-- events this way, so a handler's effects are visible to the trigger rather
-- than a frame later, while a handler that Waits cannot stall the others.
--
-- Over a SNAPSHOT, never the live list. A handler runs synchronously, and it
-- is allowed to stop a resource -- which table.removes that resource's entries
-- from this very list, part-way through the walk. With the live list and a
-- cached #list this indexed off the end: `list[i].cb` on a nil, raised out of
-- trigger, out of R.stop, and (since the in-progress guards landed) past the
-- line that returns the record to a terminal state, leaving it permanently
-- unstartable. Two resources both handling onResourceStop, one of them
-- stopping the other, was the whole reproduction.
--
-- The snapshot also settles the two questions the live list left ambiguous. A
-- handler ADDED while this event is dispatching does not run in this dispatch
-- -- otherwise a handler that re-registers its own event never terminates.
-- One REMOVED while it is dispatching does not run either: its resource has
-- been torn down and its callback closes over a dead env. That is checked off
-- the entry's own retired flag rather than by searching the list back, so it
-- stays the same removal-by-identity rule events.remove uses.
function events.trigger(name, ...)
    local list = handlers[name]
    if not list then return 0 end
    local count = #list
    local snapshot = {}
    for i = 1, count do snapshot[i] = list[i] end
    local args = table.pack(...)
    local n = 0
    for i = 1, count do
        local entry = snapshot[i]
        if not entry.removed then
            local cb = entry.cb
            local owner = entry.owner
            rt.sched.create_now(function() cb(table.unpack(args, 1, args.n)) end, owner)
            n = n + 1
        end
    end
    return n
end

function events.count()
    local n = 0
    for _, list in pairs(handlers) do n = n + #list end
    return n
end

-- Distinct event NAMES currently held, which is a different number from
-- events.count() above and the only one that can see a leak in the `handlers`
-- table itself: count() sums list lengths, so an empty list parked under a
-- name forever reads as zero no matter how many of them pile up. Exposed so
-- tests/lua/leak_test.lua can assert on keys and not only on entries.
function events.name_count()
    local n = 0
    for _ in pairs(handlers) do n = n + 1 end
    return n
end

--------------------------------------------------------------------------- exports

local exports_registry = {}     -- resource name -> { export name -> fn }

--------------------------------------------------------------------------- commands

local commands = {}
local registered = {}           -- lowercase name -> entry; entry = { name, handler, owner }

-- Commands are keyed by name, and a name can be re-registered: a resource
-- restarting re-runs RegisterCommand, and nothing stops two different
-- resources from picking the same name. A later registration REPLACES the
-- earlier one at that name -- the same policy FiveM's own RegisterCommand
-- has. What must not happen is the earlier resource's own teardown deleting
-- the replacement out from under whoever registered it (a command belonging
-- to a resource that is still running must not die because some OTHER,
-- unrelated resource stopped). That is why remove() takes the entry add()
-- returned, not the name: a stop can only ever remove the entry it holds a
-- token for, and if that entry is no longer the one `registered[key]` points
-- at -- because a later registration replaced it -- the removal is a no-op,
-- exactly like events.remove() above.
function commands.add(name, handler, owner)
    local key = tostring(name):lower()
    local entry = { name = key, handler = handler, owner = owner }
    registered[key] = entry
    return entry
end

function commands.remove(entry)
    if not entry then return end
    if registered[entry.name] == entry then registered[entry.name] = nil end
end

function commands.run(raw)
    if type(raw) ~= "string" then return false, "not a command" end
    local parts = {}
    for word in raw:gmatch("%S+") do parts[#parts + 1] = word end
    if #parts == 0 then return false, "empty command" end
    local key = parts[1]:lower()
    local entry = registered[key]
    if not entry then return false, "unknown command: " .. parts[1] end
    local args = {}
    for i = 2, #parts do args[#args + 1] = parts[i] end
    -- run()'s answer means "there was such a command to dispatch to", not
    -- "the handler completed without error": a raising handler is caught and
    -- logged inside create_now, the same as any other thread error, and does
    -- not turn a successful dispatch into a reported failure.
    rt.sched.create_now(function() entry.handler(0, args, raw) end, entry.owner)
    return true
end

function commands.count()
    local n = 0
    for _ in pairs(registered) do n = n + 1 end
    return n
end

rt.events = events
rt.exports_registry = exports_registry
rt.commands = commands
return { events = events, commands = commands }
