-- Minimal example, and the first-boot probe for this port.
--
-- Nothing here has run on a console yet, so it is written to answer the one
-- open question first — does the frame hook fire in RDR2? — and to do the
-- interesting native calls only after it has. If the log shows the load line
-- and nothing else, the plugin loaded and the tick never came: that is the
-- PLAYER_PED_ID detour in src/game/game_thread.cpp, not your script.
--
-- Every name comes from natives.lua, which binds all 7,072 natives this build
-- registers. No RVAs in script code.

notify("rdr2lua: hello.lua loaded")

local ticked = false
local frames = 0

on_tick(function()
    -- Say so exactly once, the moment the hook proves itself.
    if not ticked then
        ticked = true
        notify(string.format("rdr2lua: frame hook is alive (frame %d)", GET_FRAME_COUNT()))
    end

    frames = frames + 1
    if frames < 1800 then return end    -- about a minute at 30 fps
    frames = 0

    local ped = PLAYER_PED_ID()         -- RDR2's idiom; GTA V's GET_PLAYER_PED(-1)
    if not DOES_ENTITY_EXIST(ped) then  -- is a different native here and takes an index
        log("hello.lua: no player ped yet")
        return
    end

    -- RDR2's GET_ENTITY_COORDS is "3iii" — three int args, where GTA V took two.
    -- Passing two would leave the third slot at zero and read it anyway.
    local c   = GET_ENTITY_COORDS(ped, 1, 0)   -- a Vector3 return: {x, y, z}
    local spd = GET_ENTITY_SPEED(ped)          -- a float return

    log(string.format("hello.lua: ped=%d at %.1f, %.1f, %.1f (%.1f m/s)",
                      ped, c[1], c[2], c[3], spd))

    -- An out-parameter native: give it scratch memory, then read it back.
    -- A Vector3 out-param wants **24** bytes, not 12: RDR2 reads and writes x,
    -- y and z at an 8-byte stride, which is why read_vector3 exists. The
    -- disassembly that establishes it is quoted in src/rage/invoker/invoker.h.
    local lo, hi = mem_alloc(24), mem_alloc(24)
    GET_MODEL_DIMENSIONS(GET_ENTITY_MODEL(ped), lo, hi)
    local lo_v, hi_v = read_vector3(lo), read_vector3(hi)
    log(string.format("player model is %.2f m tall", hi_v[3] - lo_v[3]))
    mem_free(lo)
    mem_free(hi)
end)

-- log() and print() go to rdr2lua.log and out to the editor, and deliberately
-- NOT to the screen: one notification per logged line filled the PS4's queue
-- faster than it drains, so the messages worth reading arrived behind a
-- backlog. notify() is the opt-in for a line worth interrupting someone over,
-- which on a first boot is "it loaded" and "the tick works" and nothing else.
