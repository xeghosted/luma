-- tests/lua/run_lua_tests.lua
-- Runs every *_test.lua in this directory. Each test file returns a function
-- taking the harness; it registers its own checks.
--
--   lua5.4 tests/lua/run_lua_tests.lua

package.path = "tests/lua/?.lua;src/script/lua/?.lua;" .. package.path

local harness = require("harness")

local TESTS = {
    "scheduler_test",
    "manifest_test",
    "resource_test",
    "api_test",
    "leak_test",
    "reentrancy_test",
    -- Last on purpose: every check in here is a bound on something that used
    -- to run forever, so a regression shows up as this file never finishing.
    -- Everything else has already printed its result by then.
    "budget_test",
}

for _, name in ipairs(TESTS) do
    print("=== " .. name .. " ===")
    -- A test file can throw while loading (require, e.g. a syntax error) or
    -- while running (the returned function, e.g. a nil call halfway through).
    -- Both are "this file did not complete" from the outside, so both are
    -- caught here and reported as one failed check rather than aborting the
    -- run: five test files land across the next five tasks, and a typo in
    -- one should not hide whether the other four pass.
    local ok, err = pcall(function() require(name)(harness) end)
    if not ok then
        harness.check(false, name .. ": " .. tostring(err))
    end
    print("")
end

os.exit(harness.summary())
