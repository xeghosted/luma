#!/bin/bash
# Host test for the native bridge — no console needed. Run from anywhere:
#   tests/run.sh
#
# Lua is built with the C compiler and the bridge with the C++ one, because
# lua.hpp declares Lua extern "C"; compiling the .c files as C++ mangles every
# symbol and the link fails.
set -e
cd "$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

# ONE scratch directory for the whole run — the generated-header check below
# and the host tests' object files both live in it. A second `trap ... EXIT`
# would silently REPLACE this one rather than add to it, leaking whichever
# directory lost, so there is exactly one trap and it is set here.
out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT

# The runtime's own Lua is compiled into the plugin; a syntax error there would
# only surface on the console, so gate it here.
for f in src/script/lua/*.lua; do
    luac5.4 -p "$f" || { echo "SYNTAX ERROR in $f"; exit 1; }
done
echo "embedded lua parses                                        ok"

# ...and the plugin does not read those .lua files: it runs the C string
# literals in src/script/embedded_lua.h, generated from them. Nothing tied the
# two together. A stale header leaves every Lua suite below green while the
# console runs different code — which does not merely miss a bug, it voids the
# whole suite's claim to be testing the shipped artifact. So regenerate to a
# temp path and diff. Deliberately NOT over the committed file: a gate that
# can silently repair the mismatch it is looking for is not a gate.
if ! command -v python3 >/dev/null 2>&1; then
    echo "python3 is required to check src/script/embedded_lua.h against src/script/lua/*.lua." >&2
    echo "(The header is committed so that BUILDING needs no Python. Verifying it does.)" >&2
    exit 1
fi
python3 tools/gen_embedded_lua.py --out "$out/embedded_lua.h" >/dev/null
if ! diff -u src/script/embedded_lua.h "$out/embedded_lua.h"; then
    echo "" >&2
    echo "STALE: src/script/embedded_lua.h does not match src/script/lua/*.lua." >&2
    echo "       The console would run the header, not the files these tests check." >&2
    echo "       Regenerate it:  python tools/gen_embedded_lua.py" >&2
    exit 1
fi
echo "embedded_lua.h is regenerated from those same files        ok"

# --- the 2,811 addresses, against the table the game dispatches through -----
#
# Every other check in this file proves a binding PARSES and MARSHALS. None of
# them can tell whether an RVA points at the native whose name it carries: on a
# host there is nothing to ask.
#
# RDR2Lua cannot make that mistake in the first place. GTALua's generator joined
# an ordered name list against an ordered address list, and an eleven-slot slip
# gave eleven natives a neighbour's address AND its signature. Here names and
# addresses are joined on the 64-bit hash, so a name and an address can only ever
# meet if they belong to the same native.
#
# What CAN still go wrong is staleness: the committed natives.h / natives.lua /
# natives_registry.h / natives.def.lua drifting from tools/native_registry.json,
# which would ship one set of addresses to the compiler and another to scripts.
# So regenerate all four into a temp tree and diff. Same shape as the
# embedded_lua.h gate below: never regenerate over the committed files.
python3 tools/gen_natives.py --out-dir "$out/regen" >/dev/null || {
    echo "FAILED: tools/gen_natives.py could not run." >&2
    exit 1
}
for f in src/game/rdr2/natives.h          src/game/rdr2/natives_registry.h          scripts/rdr2/natives.lua          editor/lua-defs/rdr2/natives.def.lua; do
    if ! diff -q "$f" "$out/regen/$f" >/dev/null; then
        echo "" >&2
        echo "STALE: $f does not match a fresh generation." >&2
        echo "       The committed bindings and tools/native_registry.json disagree," >&2
        echo "       so the compiler and scripts would see different addresses." >&2
        echo "       Regenerate:  python tools/gen_natives.py" >&2
        exit 1
    fi
done
echo "generated native bindings match the registry               ok"

# --- ...and those diffs only mean anything if the files cannot be converted ---
#
# Every file compared above is byte-diffed against a generator that pins its
# output to LF. On a Windows checkout with core.autocrlf=true, a file NOT
# covered by an eol=lf rule in .gitattributes comes out with CRLF -- and the
# diff then reports a perfectly correct file as stale, on every line, sending
# you off to regenerate something that was never wrong.
#
# This happened. The rules named the pre-split paths (scripts/natives.lua,
# src/rage/invoker/natives.h) and went on naming them after the files moved, so
# they matched nothing, and a fresh clone of this repository failed its own test
# suite. Locally everything passed, because the generator had written those
# files with LF and git never rewrote them -- which is the worst shape a bug can
# have: invisible to the person who could fix it.
#
# So check the attribute, not the bytes. git check-attr answers what a checkout
# WOULD do, which is the thing that actually differs between machines.
unpinned=""
for f in src/script/embedded_lua.h tools/native_registry.json tests/run.sh \
         src/game/rdr2/natives.h src/game/rdr2/natives_registry.h \
         scripts/rdr2/natives.lua editor/lua-defs/rdr2/natives.def.lua; do
    eol=$(git check-attr eol -- "$f" 2>/dev/null | sed 's/.*eol: //')
    if [ "$eol" != "lf" ]; then unpinned="$unpinned $f($eol)"; fi
done
if [ -n "$unpinned" ]; then
    echo "" >&2
    echo "NOT PINNED TO LF in .gitattributes:$unpinned" >&2
    echo "       These are byte-diffed above. A Windows checkout would convert them" >&2
    echo "       and report correct files as stale. Add an eol=lf rule that matches" >&2
    echo "       the path they live at NOW." >&2
    exit 1
fi
echo "every byte-diffed file is pinned to LF                    ok"

# --- the frame hook's target must actually be detourable --------------------
#
# GoldHEN's Detour overwrites 14 bytes and memcpy's the displaced prologue into
# a stub, relocating nothing. A rip-relative instruction inside those 14 bytes
# is silently broken in the copy, and the failure looks like a clean boot
# followed by a crash -- which is exactly what PLAYER_PED_ID did. 47% of this
# build's natives have that shape, so the target cannot be chosen by name.
for g in rdr2 gta5; do
python3 tools/check_detour_target.py --game "$g" || {
    echo "" >&2
    echo "       src/game/game_thread.cpp picks a hook target whose prologue" >&2
    echo "       cannot survive being copied to the detour stub." >&2
    exit 1
}
done
echo "every profile hooks the target it was verified against     ok"

# --- the editor's definitions, against the globals that actually exist ------
#
# editor/lua-defs/rdr2/runtime.def.lua is what LuaLS reads, and it is hand-written
# because it cannot be generated -- its own header says to update it whenever
# the bridge gains a global. Nothing enforced that, and it drifted: the whole
# resource API (CreateThread, Wait, RegisterCommand, exports, ...) was missing,
# so every resource script showed undefined-global warnings on the API the
# resource model exists to provide, and read_vector3/native_addr/notify were
# missing too.
#
# This is a spelling check, not a type check: a name the runtime installs must
# appear in the definitions. That is enough to catch the drift that actually
# happens, which is a global added on one side and forgotten on the other.
defs=editor/lua-defs/rdr2/runtime.def.lua
undefined=""
bridge_globals=$(grep -ohE 'set_fn\(L, "[a-zA-Z_0-9]+|lua_setglobal\(L, "[a-zA-Z_0-9]+|set_int\(L, "[a-zA-Z_0-9]+' \
    src/script/native_bridge.cpp src/script/lua_runtime.cpp | sed 's/.*"//' | sort -u)
# A pattern that stopped matching would make every check below pass with
# nothing to check, which is the one way a gate like this dies silently.
if [ -z "$bridge_globals" ]; then
    echo "FATAL: found no globals in src/script/native_bridge.cpp at all." >&2
    echo "       The check below would pass vacuously, so it fails instead." >&2
    exit 1
fi
for g in $bridge_globals; do
    grep -qw "$g" "$defs" || undefined="$undefined $g"
done
# ...and the per-resource environment, which boot.lua installs onto `env`.
# _G is Lua's own and needs no definition.
for g in $(grep -ohE '^[[:space:]]*env\.[A-Za-z_0-9]+' src/script/lua/boot.lua |
          sed 's/.*env\.//' | sort -u | grep -v '^_G$'); do
    grep -qw "$g" "$defs" || undefined="$undefined $g"
done
if [ -n "$undefined" ]; then
    echo "" >&2
    echo "MISSING from $defs:$undefined" >&2
    echo "       The runtime installs those globals and the editor does not know" >&2
    echo "       them, so writing them looks like an error to whoever types them." >&2
    exit 1
fi
echo "the editor knows every global the runtime installs         ok"

# (natives.def.lua is covered by the regeneration diff above.)

# --- the sink wiring, which no host test can reach --------------------------
#
# src/script/loader.cpp is ORBIS-only, so net_test cannot link it: putting
# notify(msg) back into log_sink leaves every suite in this file green while
# restoring the exact defect the channels were split to fix -- one PS4
# notification per logged line, a queue that never drains, and the plugin's own
# load/ready/FAILED messages buried behind it. Gated at the source level for the
# same reason build.sh gates .init_array: check what no unit test can see.
sink_body=$(sed -n '/^void log_sink(/,/^}/p' src/script/loader.cpp)
if [ -z "$sink_body" ]; then
    echo "FATAL: log_sink() not found in src/script/loader.cpp." >&2
    echo "       Renaming it silently disables this gate, so it fails instead." >&2
    exit 1
fi
if printf '%s' "$sink_body" | grep -q 'notify'; then
    echo "" >&2
    echo "log_sink() in src/script/loader.cpp calls notify()." >&2
    echo "       log/print must not reach the screen -- notify() is the opt-in." >&2
    exit 1
fi
echo "log_sink keeps log and screen separate                     ok"

# ...and the other half: notify() reaching nothing at all would be just as
# invisible, and just as green.
if ! grep -q 'runtime_set_notify_sink(notify)' src/script/loader.cpp; then
    echo "src/script/loader.cpp never installs the notify sink." >&2
    echo "       notify() would silently do nothing on the console." >&2
    exit 1
fi
echo "the notify sink is installed at boot                       ok"

# --- ...and a reload must not stack up tick callbacks -----------------------
#
# on_tick APPENDS to the runtime's tick list. Nothing removed an entry until
# runtime_clear_ticks() existed, so every script_reload() left the previous
# run's callbacks registered next to the new ones -- a second copy of every
# callback, then a third. Both copies run in the same frame and see the same
# input, so a script that toggles state on a button press flipped it twice and
# stopped responding after its first reload, which is the normal editing loop.
#
# net_test covers runtime_clear_ticks() itself. What it cannot reach is the
# caller: src/script/loader.cpp is ORBIS-only and never links into a host test.
# So the call is gated at the source level, ORDER INCLUDED -- clearing after
# load_dir would throw away the callbacks the reload just registered, which is
# a worse failure than the one being fixed and would pass a presence-only check.
reload_body=$(sed -n '/^int script_reload(/,/^}/p' src/script/loader.cpp)
if [ -z "$reload_body" ]; then
    echo "FATAL: script_reload() not found in src/script/loader.cpp." >&2
    echo "       Renaming it silently disables this gate, so it fails instead." >&2
    exit 1
fi
# Comments stripped first. The body explains itself, and both names appear in
# that prose -- a gate a comment can flip is not a gate, so it reads the code.
reload_code=$(printf '%s' "$reload_body" | sed 's://.*::')
clear_at=$(printf '%s' "$reload_code" | grep -n 'runtime_clear_ticks' | head -1 | cut -d: -f1)
load_at=$(printf '%s'  "$reload_code" | grep -n 'load_dir'            | head -1 | cut -d: -f1)
if [ -z "$clear_at" ] || [ -z "$load_at" ] || [ "$clear_at" -ge "$load_at" ]; then
    echo "" >&2
    echo "script_reload() in src/script/loader.cpp must call runtime_clear_ticks()" >&2
    echo "       BEFORE load_dir(). Without it every reload registers a second copy" >&2
    echo "       of every on_tick callback; after it, the reload throws away its own." >&2
    exit 1
fi
echo "a reload clears the previous run's tick callbacks         ok"

lua5.4 tests/lua/run_lua_tests.lua

(cd "$out" && gcc -std=c99 -O1 -w -I "$OLDPWD/src/lua" -c "$OLDPWD"/src/lua/*.c)
g++ -std=c++17 -I src -I src/lua -o "$out/net_test" \
    tests/net_test.cpp \
    tests/host_profile_stub.cpp \
    src/net/frame.cpp \
    src/net/mailbox.cpp \
    src/net/path_guard.cpp \
    src/script/lua_runtime.cpp \
    src/script/native_bridge.cpp \
    src/script/native_marshal.cpp \
    "$out"/*.o -lm
"$out/net_test"

g++ -std=c++17 -I src -I src/lua -o "$out/host_bridge_test" \
    tests/host_bridge_test.cpp \
    tests/host_profile_stub.cpp \
    src/script/native_bridge.cpp \
    src/script/native_marshal.cpp \
    "$out"/*.o -lm

# Run from the repo root: the test loads the real scripts/rdr2/natives.lua.
"$out/host_bridge_test"
