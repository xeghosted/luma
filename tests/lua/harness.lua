-- tests/lua/harness.lua
-- Test harness for the runtime's own Lua. Deliberately mirrors the C++ suites'
-- style: a check() that prints a padded label, a failure counter, and a
-- PASSED/FAILED summary with a non-zero exit.
--
-- The important part is the fake host: the real runtime gets file I/O and a
-- clock from C++, so the tests supply their own. Time is injected rather than
-- read, which is what makes scheduler tests deterministic instead of flaky.

local M = { failures = 0 }

function M.check(ok, what)
    print(string.format("%-58s %s", what, ok and "ok" or "FAIL"))
    if not ok then M.failures = M.failures + 1 end
end

function M.summary()
    print("")
    print(string.format("%s (%d failures)", M.failures == 0 and "PASSED" or "FAILED", M.failures))
    return M.failures == 0 and 0 or 1
end

function M.fake_host()
    local h = { files = {}, dirs = {}, logged = {}, _now = 0 }
    function h.set_now(ms) h._now = ms end
    function h.now_ms() return h._now end
    function h.read_file(path) return h.files[path] end
    function h.list_dir(path) return h.dirs[path] end
    function h.log(msg) h.logged[#h.logged + 1] = msg end
    return h
end

return M
