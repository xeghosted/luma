-- Minimal example. Everything here comes from natives.lua, which binds every
-- native this build has under its canonical name — no RVAs in script code.

local frames = 0

on_tick(function()
    frames = frames + 1
    if frames < 1800 then return end    -- about a minute at 30 fps, half that at 60
    frames = 0

    local ped = GET_PLAYER_PED(-1)
    local c   = GET_ENTITY_COORDS(ped, true)      -- a Vector3 return: {x, y, z}
    local spd = GET_ENTITY_SPEED(ped)             -- a float return

    log(string.format("hello.lua: ped=%d at %.1f, %.1f, %.1f (%.1f m/s)",
                      ped, c[1], c[2], c[3], spd))

    -- An out-parameter native: give it scratch memory, then read it back.
    -- A Vector3 out-param wants **24** bytes, not 12: the game reads and writes
    -- x, y and z at an 8-byte stride. read_vector3 knows that layout, which is
    -- why it exists -- this example used to allocate 12 and read +8 as z, when
    -- +8 is y. (It reported 0.00 m regardless, because the invoker was not
    -- copying the native's answer back at all; see invoker.h.)
    local lo, hi = mem_alloc(24), mem_alloc(24)
    GET_MODEL_DIMENSIONS(GET_ENTITY_MODEL(ped), lo, hi)
    local lo_v, hi_v = read_vector3(lo), read_vector3(hi)
    log(string.format("player model is %.2f m tall", hi_v[3] - lo_v[3]))
    mem_free(lo)
    mem_free(hi)
end)

-- Roughly half the natives are reached through the hash registry read out of the
-- game at boot. It is up before scripts run, so this is a sanity check rather
-- than something you normally have to wait on.
-- log() and print() go to gtalua.log and out to the editor, and deliberately
-- NOT to the screen: one notification per logged line filled the PS4's queue
-- faster than it drains, so the messages worth reading arrived behind a
-- backlog. notify() is the opt-in for a line worth interrupting someone over.
-- It logs as well, so nothing is lost to the display cutting long text.
notify("hello.lua loaded; hash registry ready: " .. tostring(natives_ready()))
