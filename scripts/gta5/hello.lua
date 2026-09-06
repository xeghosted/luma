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

-- ---------------------------------------------------------------------------
-- A test menu.
--
-- Drawn from on_tick, which is the only place it can be drawn from: DRAW_RECT
-- and the text commands are frame-scoped. They do not paint something that
-- stays on screen, they contribute to the frame being built right now, so
-- anything that stops calling them every frame simply disappears.
--
-- Open it with the button below, or from the editor console: `=menu()`.

local MENU = { open = false, index = 1, x = 0.16, y = 0.20, w = 0.23, h = 0.035 }

-- Standard GTA V control indices, control group 0. They are just numbers the
-- game looks up, so if one of these is taken on your setup, change it here.
--
-- On a DualShock several of these land on the SAME physical button: control 20
-- is D-pad Down, and so is INPUT_CELLPHONE_DOWN. That is why the toggle is only
-- read while the menu is closed (see tick below) -- otherwise the press that
-- opens the menu also moves the cursor, and the next one closes it again
-- instead of navigating. Splitting it by state makes the binding table
-- irrelevant: whatever TOGGLE turns out to be, it opens, and the menu owns the
-- pad from then until you back out of it.
local TOGGLE = 20   -- INPUT_MULTIPLAYER_INFO  D-pad down on a pad, Z on a keyboard
local UP     = 172  -- INPUT_CELLPHONE_UP      D-pad up / arrow up
local DOWN   = 173  -- INPUT_CELLPHONE_DOWN    D-pad down / arrow down
local SELECT = 176  -- INPUT_CELLPHONE_SELECT  cross / Enter
local BACK   = 177  -- INPUT_CELLPHONE_CANCEL  circle / Backspace
local PHONE  = 27   -- INPUT_PHONE -- shares D-pad up, so it gets held off too

-- Held off for one frame at a time while the menu is open, so that walking the
-- list does not also answer the phone. IS_DISABLED_CONTROL_JUST_PRESSED still
-- sees them: "disabled" stops the *game* acting on a button, not us reading it.
local BORROWED = { PHONE, UP, DOWN, SELECT, BACK }

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

-- Registered unconditionally, and that is deliberate.
--
-- script_reload() clears the runtime's tick list before re-running the files, so
-- re-running this one cannot leave a second copy of the callback behind. It also
-- means the opposite mistake is the dangerous one now: guarding registration
-- behind a global flag would survive the reload while the tick list did not, so
-- the file would run, skip the registration it thinks it already did, and leave
-- nothing driving the menu at all. Register every time.
local function tick()
    if not MENU.open then
        if IS_DISABLED_CONTROL_JUST_PRESSED(0, TOGGLE) then MENU.open = true end
        -- Return either way. The press that opened the menu must not be read a
        -- second time this frame as navigation -- on a pad it is the same
        -- button -- and with the menu closed there is nothing to draw anyway.
        return
    end

    for i = 1, #BORROWED do
        DISABLE_CONTROL_ACTION(0, BORROWED[i], true)
    end

    local n = #items
    if IS_DISABLED_CONTROL_JUST_PRESSED(0, UP) then
        MENU.index = (MENU.index - 2) % n + 1
    elseif IS_DISABLED_CONTROL_JUST_PRESSED(0, DOWN) then
        MENU.index = MENU.index % n + 1
    elseif IS_DISABLED_CONTROL_JUST_PRESSED(0, BACK) then
        MENU.open = false
        return
    elseif IS_DISABLED_CONTROL_JUST_PRESSED(0, SELECT) then
        -- pcall, because on_tick drops a callback that raises. Without this a
        -- single bad menu action would not misbehave once, it would take the
        -- whole menu off the screen for good and leave you guessing why.
        local ok, err = pcall(items[MENU.index].run)
        if not ok then log("menu: item " .. MENU.index .. " failed: " .. tostring(err)) end
    end

    local cx = MENU.x + MENU.w / 2

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
    text(string.format("%d / %d   \xe2\x97\x8b back", MENU.index, n),
         cx, foot + 0.008, 0.35, 190, 190, 190, true)
end

on_tick(tick)
