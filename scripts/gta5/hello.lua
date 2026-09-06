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
    -- mem_alloc returns nil if the allocation fails. Checking is not ceremony:
    -- a native called with a nil address runs on the GAME thread, where a bad
    -- pointer takes the game down instead of raising a Lua error you can read.
    if lo and hi then
        GET_MODEL_DIMENSIONS(GET_ENTITY_MODEL(ped), lo, hi)
        local lo_v, hi_v = read_vector3(lo), read_vector3(hi)
        log(string.format("player model is %.2f m tall", hi_v[3] - lo_v[3]))
    end
    -- Freed separately, because if only one of the two succeeded the other
    -- still has to go back.
    if lo then mem_free(lo) end
    if hi then mem_free(hi) end
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

-- ---------------------------------------------------------------------------
-- A test menu.
--
-- Drawn from on_tick, which is the only place it can be drawn from: DRAW_RECT
-- and the text commands are frame-scoped. They do not paint something that
-- stays on screen, they contribute to the frame being built right now, so
-- anything that stops calling them every frame simply disappears.
--
-- L1 + CIRCLE opens and closes it. D-pad up/down move, CROSS activates, CIRCLE
-- backs out. Those are not guesses: the control ids, the open bind, the held
-- repeat and its 300 ms lockout all come from the C++ menu in
-- InsulinEngine/InsulinGTAV (menu/base/util/input.cpp, platform/compat.h), so
-- both menus on this console answer to the same buttons. An earlier version of
-- this file used control 20 for the toggle on the assumption that it was the
-- touchpad; it is D-pad Down, which is also the button that moves the
-- selection.
--
-- From the editor console: `=menu()` toggles, `menu(false)` closes.

local MENU = { open = false, index = 1, x = 0.16, y = 0.20, w = 0.23, h = 0.035 }

-- platform/compat.h, verbatim.
local UP     = 188  -- ControlFrontendUp        D-pad up
local DOWN   = 187  -- ControlFrontendDown      D-pad down
local ACCEPT = 201  -- ControlFrontendAccept    cross
local CANCEL = 202  -- ControlFrontendCancel    circle
local LB     = 205  -- ControlFrontendLb        L1
local PHONE  = 27   -- INPUT_PHONE -- shares D-pad up, so it gets held off too

-- Held off for one frame at a time while the menu is open, so that walking the
-- list does not also answer the phone or pick a weapon. The game stops acting
-- on them; IS_DISABLED_CONTROL_* still reads them, which is the whole point of
-- the "disabled" family.
local BORROWED = { PHONE, UP, DOWN, ACCEPT, CANCEL, LB }

local function sound(name)
    -- PLAY_SOUND_FRONTEND is one of the hash-bound half on this build, so it is
    -- the only call here that can raise if the boot-time table walk failed.
    if natives_ready() then
        PLAY_SOUND_FRONTEND(-1, name, "HUD_FRONTEND_DEFAULT_SOUNDSET", false)
    end
end

local function text(s, x, y, scale, r, g, b, centre)
    SET_TEXT_FONT(0)
    SET_TEXT_SCALE(0.0, scale)
    SET_TEXT_COLOUR(r, g, b, 255)
    SET_TEXT_CENTRE(centre == true)
    SET_TEXT_DROP_SHADOW()
    SET_TEXT_OUTLINE()
    -- Two calls rather than one: the game takes strings through a text-command
    -- buffer, and "STRING" is the format entry meaning "the substring I am
    -- about to hand you is the entire message".
    BEGIN_TEXT_COMMAND_DISPLAY_TEXT("STRING")
    ADD_TEXT_COMPONENT_SUBSTRING_PLAYER_NAME(s)
    END_TEXT_COMMAND_DISPLAY_TEXT(x, y, 0)
end

-- Labels are functions, not strings, so a row can read live game state every
-- frame -- which is the more interesting half of the test: it proves the menu
-- is reading the game, not just painting over it.
local items = {
    {
        label = function()
            local c = GET_ENTITY_COORDS(GET_PLAYER_PED(-1), true)
            return string.format("Position  %.0f %.0f %.0f", c[1], c[2], c[3])
        end,
        run = function()
            local ped = GET_PLAYER_PED(-1)
            local c   = GET_ENTITY_COORDS(ped, true)
            log(string.format("menu: %.2f, %.2f, %.2f heading %.1f",
                              c[1], c[2], c[3], GET_ENTITY_HEADING(ped)))
        end,
    },
    {
        label = function()
            local ped = GET_PLAYER_PED(-1)
            return string.format("Health  %d / %d",
                                 GET_ENTITY_HEALTH(ped), GET_ENTITY_MAX_HEALTH(ped))
        end,
        run = function()
            local ped = GET_PLAYER_PED(-1)
            SET_ENTITY_HEALTH(ped, GET_ENTITY_MAX_HEALTH(ped))
        end,
    },
    {
        label = function()
            return "Invincible  " .. (GET_PLAYER_INVINCIBLE(PLAYER_ID()) and "on" or "off")
        end,
        run = function()
            local p = PLAYER_ID()
            SET_PLAYER_INVINCIBLE(p, not GET_PLAYER_INVINCIBLE(p))
        end,
    },
    {
        label = function() return "Clear wanted level" end,
        run   = function() CLEAR_PLAYER_WANTED_LEVEL(PLAYER_ID()) end,
    },
    {
        label = function()
            local ped = GET_PLAYER_PED(-1)
            return IS_PED_IN_ANY_VEHICLE(ped, false) and "Repair vehicle" or "Repair vehicle  --"
        end,
        run = function()
            local ped = GET_PLAYER_PED(-1)
            if IS_PED_IN_ANY_VEHICLE(ped, false) then
                SET_VEHICLE_FIXED(GET_VEHICLE_PED_IS_IN(ped, false))
            end
        end,
    },
    {
        label = function() return "Close" end,
        run   = function() MENU.open = false end,
    },
}

-- Callable from the editor console: `=menu()` toggles, `menu(true)` opens.
function menu(state)
    if state == nil then MENU.open = not MENU.open
    else                 MENU.open = state and true or false end
    return MENU.open
end

-- Input and drawing are separate, the way menu.cpp separates its input phase
-- from its render phase. It matters: the open bind is HELD, so it needs a
-- lockout to stop it toggling every frame it is down -- and if drawing shared
-- that lockout the menu would blink out for 300 ms every time you opened it.
local next_input_at = 0     -- game-timer ms
local repeat_delay  = 150

local function step(delta)
    local n = #items
    MENU.index = (MENU.index - 1 + delta) % n + 1
    sound("NAV_UP_DOWN")
end

local function handle_input(now)
    if now < next_input_at then return end

    -- The open bind first, so that CIRCLE-as-part-of-the-combo is never also
    -- read as CIRCLE-alone-means-back below.
    if IS_DISABLED_CONTROL_PRESSED(0, LB) and IS_DISABLED_CONTROL_PRESSED(0, CANCEL) then
        MENU.open = not MENU.open
        sound("Back")
        next_input_at = now + 300
        return
    end

    if not MENU.open then return end

    for i = 1, #BORROWED do
        DISABLE_CONTROL_ACTION(0, BORROWED[i], true)
    end

    if IS_DISABLED_CONTROL_PRESSED(0, UP) then
        step(-1)
    elseif IS_DISABLED_CONTROL_PRESSED(0, DOWN) then
        step(1)
    elseif IS_DISABLED_CONTROL_JUST_RELEASED(0, ACCEPT) then
        -- pcall, because on_tick drops a callback that raises. Without this a
        -- single bad menu action would not misbehave once, it would take the
        -- whole menu off the screen for good and leave you guessing why.
        local ok, err = pcall(items[MENU.index].run)
        if not ok then log("menu: item " .. MENU.index .. " failed: " .. tostring(err)) end
        sound("SELECT")
        next_input_at = now + 200
        return
    elseif IS_DISABLED_CONTROL_JUST_RELEASED(0, CANCEL) then
        MENU.open = false
        sound("Back")
        next_input_at = now + 200
        return
    else
        repeat_delay = 150      -- nothing held: the next press starts slow again
        return
    end

    -- Held: accelerate, so a long list does not need a long thumb.
    if repeat_delay > 80 then repeat_delay = repeat_delay - 15 end
    next_input_at = now + repeat_delay
end

local function draw()
    local cx = MENU.x + MENU.w / 2
    local n  = #items

    DRAW_RECT(cx, MENU.y + MENU.h / 2, MENU.w, MENU.h, 0, 90, 150, 225, 0)
    text("Luma test menu", cx, MENU.y + 0.005, 0.45, 255, 255, 255, true)

    for i = 1, n do
        local top = MENU.y + i * MENU.h
        local sel = (i == MENU.index)
        if sel then DRAW_RECT(cx, top + MENU.h / 2, MENU.w, MENU.h, 245, 245, 245, 235, 0)
        else        DRAW_RECT(cx, top + MENU.h / 2, MENU.w, MENU.h, 0, 0, 0, 175, 0) end

        -- A label that raises would take the menu down the same way an action
        -- would, and it runs every frame rather than on a button, so it gets
        -- the same net.
        local ok, s = pcall(items[i].label)
        if not ok then s = "<label error>" end
        if sel then text(s, MENU.x + 0.008, top + 0.008, 0.35, 15, 15, 15, false)
        else        text(s, MENU.x + 0.008, top + 0.008, 0.35, 255, 255, 255, false) end
    end

    local foot = MENU.y + (n + 1) * MENU.h
    DRAW_RECT(cx, foot + MENU.h / 2, MENU.w, MENU.h, 0, 0, 0, 225, 0)
    text(string.format("%d / %d    L1+O close", MENU.index, n),
         cx, foot + 0.008, 0.35, 190, 190, 190, true)
end

-- Registered unconditionally, and that is deliberate.
--
-- script_reload() clears the runtime's tick list before re-running the files, so
-- re-running this one cannot leave a second copy of the callback behind. It also
-- means the opposite mistake is the dangerous one now: guarding registration
-- behind a global flag would survive the reload while the tick list did not, so
-- the file would run, skip the registration it thinks it already did, and leave
-- nothing driving the menu at all. Register every time.
on_tick(function()
    handle_input(GET_GAME_TIMER())
    if MENU.open then draw() end
end)
