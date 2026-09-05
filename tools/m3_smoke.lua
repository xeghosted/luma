-- The M3 console check, in one chunk. Run it once the game is up:
--
--   pwsh tools/console.ps1 execfile tools/m3_smoke.lua
--
-- Everything in M3 was built and tested on a PC. This is the part a PC cannot
-- answer: whether the eleven corrected native bindings behave on hardware, and
-- whether the runtime still boots clean under the build that carries them.
--
-- Almost everything here reads. Three calls do not, and pretending otherwise
-- would be the wrong kind of comment: STOP_CUTSCENE_CAM_SHAKING(false),
-- USE_PLAYER_COLOUR_INSTEAD_OF_TEAM_COLOUR(false) and
-- SET_PARTICLE_FX_BLOOD_SCALE(1.0) each write something, because those three
-- had the WRONG ARGUMENT COUNT before the fix and a call is the only way to
-- exercise that. Each is set to its default, none spawns or moves anything,
-- and nothing here writes a file -- so it is still safe to run in the middle of
-- whatever is on screen, which is the point: no arranging anything first.
--
-- Each line prints PASS, FAIL or INFO. INFO is for answers that are correct
-- for a reason this script cannot check by itself (a value that depends on
-- where the player is standing), and they still have to be READ: a plausible
-- number is the evidence, not the absence of a FAIL.

local pass, fail = 0, 0
local function ok(cond, what, detail)
    if cond then pass = pass + 1 else fail = fail + 1 end
    print(string.format("%-4s %s%s", cond and "PASS" or "FAIL", what,
                        detail and ("  -- " .. tostring(detail)) or ""))
end
local function info(what, detail)
    print(string.format("INFO %s  -- %s", what, tostring(detail)))
end

print("== M3 smoke ==")
info("plugin sees the hash registry", natives_ready() and "ready" or "PENDING")

local ped = GET_PLAYER_PED(-1)
ok(ped ~= 0, "GET_PLAYER_PED answers a handle", ped)

-- --- the eleven corrected bindings ----------------------------------------
--
-- Each one used to reach a DIFFERENT native. The check is not "does it return
-- something" but "does it return something the wrong native could not have
-- returned".

-- Was GET_ENTITY_TYPE, which answers 1, 2 or 3 for ped/vehicle/object. A
-- population type is a different scale entirely (0..10), and for a player ped
-- it is not 1.
local pop = GET_ENTITY_POPULATION_TYPE(ped)
ok(pop ~= 1, "GET_ENTITY_POPULATION_TYPE is not GET_ENTITY_TYPE's answer", pop)
ok(pop >= 0 and pop <= 10, "  and is inside the population-type range", pop)

-- Was ROPE_LOAD_TEXTURES, which takes no arguments and returns nothing useful.
-- 51 is INPUT_CONTEXT; the string is a button glyph, so its content depends on
-- the pad -- but a non-empty string at all is the thing the old binding could
-- never produce.
local btn = GET_CONTROL_INSTRUCTIONAL_BUTTONS_STRING(2, 51, true)
ok(type(btn) == "string" and #btn > 0,
   "GET_CONTROL_INSTRUCTIONAL_BUTTONS_STRING returns a string", btn)

-- Was AUDIO_IS_MUSIC_PLAYING. Both decompile to the same body in this build,
-- so this cannot fail on a wrong address -- it is here to catch a wrong
-- SIGNATURE, which the fix also changed (Any -> bool).
local music = AUDIO_IS_SCRIPTED_MUSIC_PLAYING()
ok(type(music) == "boolean", "AUDIO_IS_SCRIPTED_MUSIC_PLAYING returns a bool", music)

-- NETWORK_CAN_COMMUNICATE_WITH_GAMER is deliberately NOT called. It takes a
-- gamer handle, there is no valid one in story mode, and handing a native a
-- zeroed buffer to walk is the one thing this script must not risk: a crash
-- costs a four-minute reboot and proves nothing the offline audit did not
-- already settle from the decompiled body. Both candidates took the same
-- argument shape, so a call could not have distinguished them anyway.
info("NETWORK_CAN_COMMUNICATE_WITH_GAMER", "not called on purpose -- see the comment")

-- The write-back fix from the previous session, re-checked under this build:
-- these are the natives that returned zeros before set_vector_results().
local lo, hi = mem_alloc(24), mem_alloc(24)
GET_MODEL_DIMENSIONS(GET_ENTITY_MODEL(ped), lo, hi)
local a, b = read_vector3(lo), read_vector3(hi)
local height = b[3] - a[3]
ok(height > 0.5 and height < 4.0, "GET_MODEL_DIMENSIONS still writes a real box",
   string.format("%.2f m tall", height))
mem_free(lo); mem_free(hi)

local c = GET_ENTITY_COORDS(ped, true)
info("player position", string.format("%.1f, %.1f, %.1f", c[1], c[2], c[3]))

-- --- signature changes that the address alone would not catch --------------
--
-- These three had the WRONG ARGUMENT COUNT before the fix. A call that returns
-- at all is the evidence: the old binding pushed a different number of slots.

STOP_CUTSCENE_CAM_SHAKING(false)
ok(true, "STOP_CUTSCENE_CAM_SHAKING accepts its one argument")

USE_PLAYER_COLOUR_INSTEAD_OF_TEAM_COLOUR(false)
ok(true, "USE_PLAYER_COLOUR_INSTEAD_OF_TEAM_COLOUR accepts its one argument")

SET_PARTICLE_FX_BLOOD_SCALE(1.0)
ok(true, "SET_PARTICLE_FX_BLOOD_SCALE accepts a float")

-- --- the resource model still works under this build -----------------------
--
-- Not from here. This line used to read GetResourceState("hello"), and it
-- answered "n/a" on every run it ever had: GetResourceState is installed on a
-- RESOURCE's environment (boot.lua's make_env), and an EXEC chunk is not a
-- resource -- so the guard fell through to its default and the check reported
-- nothing while looking like it had passed. The resource model is checked over
-- the wire instead, where the answer comes from the console rather than from a
-- global this chunk cannot see:
--
--   pwsh tools/console.ps1 reslist        -> "hello started"
--   pwsh tools/console.ps1 resinfo hello  -> threads/handlers/exports/commands
info("resource state", "not visible from EXEC -- use console.ps1 reslist")

print(string.format("== %d passed, %d failed ==", pass, fail))
return string.format("m3 smoke: %d passed, %d failed", pass, fail)
