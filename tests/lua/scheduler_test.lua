-- tests/lua/scheduler_test.lua
return function(h)
    package.loaded["scheduler"] = nil
    __rt = {}
    require("scheduler")
    local S = __rt.sched

    -- A thread runs on the first tick at or after its wake time, not before.
    local ran = 0
    S.create(function() ran = ran + 1 end)
    h.check(ran == 0, "CreateThread does not run the body immediately")
    S.tick(0)
    h.check(ran == 1, "the thread runs on the first tick")
    S.tick(0)
    h.check(ran == 1, "a finished thread is not resumed again")
    h.check(S.count() == 0, "and is removed from the scheduler")

    -- Wait yields the DELAY; the host computes the wake time.
    local marks = {}
    S.create(function()
        marks[#marks + 1] = "a"
        __rt.sched.wait(100)
        marks[#marks + 1] = "b"
    end)
    S.tick(1000)
    h.check(#marks == 1, "the thread runs up to its first Wait")
    S.tick(1050)
    h.check(#marks == 1, "and is not resumed before the delay has elapsed")
    S.tick(1100)
    h.check(#marks == 2, "but is resumed once it has")

    -- Wait(0) means next tick, not this one.
    local n = 0
    S.create(function() while true do n = n + 1; __rt.sched.wait(0) end end)
    S.tick(2000)
    S.tick(2000)
    h.check(n == 2, "Wait(0) resumes on the next tick even at the same time")

    -- A thread created during a tick runs on the NEXT tick, not the current
    -- one -- without that staging, creating a thread from a callback would
    -- invalidate the iteration in progress.
    local inner = 0
    S.create(function()
        __rt.sched.create(function() inner = inner + 1 end)
    end)
    S.tick(3000)
    h.check(inner == 0, "a thread created during a tick does not run in it")
    S.tick(3000)
    h.check(inner == 1, "it runs on the next tick")

    -- SetTimeout takes milliseconds FIRST, matching FiveM.
    --
    -- This is also the regression test for the early-fire bug: a SetTimeout
    -- must resolve its wake time against the `now` in effect when it gets
    -- PROMOTED (top of the next tick), never against whatever `now` was left
    -- over from an earlier tick call at the moment set_timeout() was called.
    -- It is pinned with an explicit baseline tick rather than relying on
    -- state left behind by the section above, so it cannot pass by accident
    -- if these sections are ever reordered or this one runs alone.
    S.tick(4000)
    local fired = 0
    S.set_timeout(50, function() fired = fired + 1 end)
    -- A resolve-at-call-time bug would have computed wake = 4000 + 50 = 4050
    -- here, using the baseline tick's `now`. This tick reaches exactly that
    -- stale value; the correct resolution computes wake = 4050 + 50 = 4100
    -- at promotion, so it must not have fired yet.
    S.tick(4050)
    h.check(fired == 0, "SetTimeout is not resolved against a stale now from before it was set")
    S.tick(4100)
    h.check(fired == 1, "SetTimeout fires once its real delay, resolved at promotion, has elapsed")
    S.tick(4150)
    h.check(fired == 1, "and does not fire again")

    -- ClearTimeout cancels before it fires (the target is still staged here,
    -- never having been promoted into `threads`).
    local cancelled = 0
    local handle = S.set_timeout(50, function() cancelled = cancelled + 1 end)
    S.clear(handle)
    S.tick(5100)
    h.check(cancelled == 0, "ClearTimeout cancels a still-staged, pending timeout")

    -- clear() must also work on a thread that has already been promoted into
    -- `threads` -- every check above cleared a target while it was still in
    -- `staged`.
    local promoted_cleared = 0
    local ph = S.create(function()
        __rt.sched.wait(1000)
        promoted_cleared = promoted_cleared + 1
    end)
    S.tick(5200)                          -- promote it and run it up to its Wait; wake becomes 5200 + 1000 = 6200
    local before_clear_count = S.count()
    S.clear(ph)
    h.check(S.count() == before_clear_count - 1, "clear removes an already-promoted thread from threads")
    S.tick(6250)                          -- past the wake time (6200) it would have had if not cleared
    h.check(promoted_cleared == 0, "and a cleared, already-promoted thread never resumes")

    -- An erroring thread dies alone and is reported.
    local survivor = 0
    local logged = {}
    __rt.log = function(m) logged[#logged + 1] = m end
    S.create(function() error("boom") end)
    S.create(function() while true do survivor = survivor + 1; __rt.sched.wait(0) end end)
    S.tick(6500)
    S.tick(6500)
    h.check(survivor == 2, "a raising thread does not stop the others")
    h.check(#logged == 1, "the error is reported exactly once")
    h.check(logged[1]:find("boom") ~= nil, "  and the message names the failure")

    -- CreateThreadNow's error report must be as complete as the tick path's:
    -- a developer debugging a failed event handler needs the stack too.
    local logged_before_cn_error = #logged
    local cn_error_ok = S.create_now(function() error("boom2") end)
    h.check(cn_error_ok == false, "CreateThreadNow returns false when its body errors")
    h.check(#logged == logged_before_cn_error + 1, "the error is reported exactly once")
    h.check(logged[#logged]:find("boom2") ~= nil, "  the message names the failure")
    h.check(logged[#logged]:find("stack traceback") ~= nil, "  and includes a traceback, matching the tick path error report")

    -- CreateThreadNow runs immediately, up to its first yield.
    local now_ran = 0
    local now_ok, now_handle = S.create_now(function() now_ran = 1 end)
    h.check(now_ran == 1, "CreateThreadNow runs the body immediately")
    h.check(now_ok == true, "and returns true when the body finishes without yielding")
    h.check(now_handle == nil,
        "  and its second return is nil: there is no continuation left to track or clear")

    -- create_now's second return is the scheduler handle for a body that
    -- yielded instead of finishing -- an internal contract, not part of the
    -- FiveM-facing boolean: a caller that wants to track it against
    -- something (make_env's CreateThreadNow wrapper, against the owning
    -- resource) can, and can also clear it like any other handle.
    local yield_ok, yield_handle = S.create_now(function() __rt.sched.wait(1000) end)
    h.check(yield_ok == true and yield_handle ~= nil,
        "create_now returns a real handle for a body that yields instead of finishing")
    local before_yield_clear = S.count()
    S.clear(yield_handle)
    h.check(S.count() == before_yield_clear - 1, "  and that handle clears the continuation like any other")

    -- CreateThreadNow with a body that YIELDS instead of finishing: its
    -- continuation must not be resolved against a stale now either -- the
    -- same fix that resolved the SetTimeout regression above, applied to
    -- create_now's own eager-resolution path.
    S.tick(7000)
    local cn_stage = 0
    local cn_ok = S.create_now(function()
        cn_stage = 1
        __rt.sched.wait(50)
        cn_stage = 2
    end)
    h.check(cn_ok == true, "CreateThreadNow returns true when its body yields instead of finishing")
    h.check(cn_stage == 1, "CreateThreadNow still runs the body immediately, up to its first yield")
    -- Same discriminating tick as the SetTimeout regression: a resolve-at-
    -- call-time bug computes wake = 7000 + 50 = 7050 here; the correct
    -- resolution computes wake = 7050 + 50 = 7100 at promotion.
    S.tick(7050)
    h.check(cn_stage == 1, "its continuation is not resolved against a stale now from before it yielded")
    S.tick(7100)
    h.check(cn_stage == 2, "but resumes once its real delay has elapsed")

    -- CRITICAL: create_now called from inside a thread that tick() is
    -- already resuming must not corrupt tick()'s pairs(threads) traversal.
    -- A later task dispatches event handlers through create_now, so this is
    -- not a synthetic case -- a TriggerEvent from a running thread hits this
    -- exact path. Lua permits nil-ing an existing key mid-traversal but never
    -- adding a new one; a direct-into-threads write here was verified (by
    -- hand, against a copy of the pre-fix code) to let the inner thread
    -- resume a SECOND time within this very tick, before ever returning to
    -- its caller -- wait(0) on the inner body makes that same-tick
    -- double-resume observable without relying on pairs happening to raise
    -- an error.
    local mid_tick_ok = false
    local nested = 0
    S.create(function()
        mid_tick_ok = __rt.sched.create_now(function()
            nested = 1
            __rt.sched.wait(0)
            nested = 2
        end)
    end)
    local tick_ok = pcall(S.tick, 7200)
    h.check(tick_ok == true, "create_now called mid-tick does not corrupt tick's traversal of threads")
    h.check(mid_tick_ok == true, "create_now still succeeds when called from a running thread")
    h.check(nested == 1, "its body runs immediately, up to the first yield")
    h.check(nested ~= 2, "  and its continuation does not resume again within that same tick")
    S.tick(7200)
    h.check(nested == 2, "the staged continuation resumes on a later tick, like any other")

    -- kill_owner removes only that owner's threads.
    local a, b = 0, 0
    S.create(function() while true do a = a + 1; __rt.sched.wait(0) end end, "res_a")
    S.create(function() while true do b = b + 1; __rt.sched.wait(0) end end, "res_b")
    S.tick(7500); S.tick(7500)
    local before = S.count()
    S.kill_owner("res_a")
    h.check(S.count() == before - 1, "kill_owner removes exactly that owner's threads")
    local a_at_kill = a
    S.tick(7500)
    h.check(a == a_at_kill, "the killed thread stops running")
    h.check(b > 0, "the other owner's thread keeps running")

    -- kill_owner must also remove a matching entry that is still staged (not
    -- yet promoted into `threads`), and count() must include staged entries.
    local staged_owned = 0
    local before_stage_count = S.count()
    S.create(function() staged_owned = staged_owned + 1 end, "res_c")
    h.check(S.count() == before_stage_count + 1, "count includes a freshly-staged, not-yet-promoted thread")
    S.kill_owner("res_c")
    h.check(S.count() == before_stage_count, "kill_owner removes a matching entry that is still staged")
    S.tick(7600)
    h.check(staged_owned == 0, "and the killed staged entry never runs, even once its promotion tick arrives")

    -- kill_owner(nil) must be a no-op, not a wildcard that sweeps every
    -- unowned thread in the system.
    local unowned = 0
    S.create(function() while true do unowned = unowned + 1; __rt.sched.wait(0) end end)
    S.tick(7700)
    local unowned_mark = unowned
    local before_nil_kill = S.count()
    S.kill_owner(nil)
    h.check(S.count() == before_nil_kill, "kill_owner(nil) does not remove unowned threads")
    S.tick(7700)
    h.check(unowned > unowned_mark, "and an unowned thread keeps running")
end
