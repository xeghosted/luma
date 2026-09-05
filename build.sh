#!/bin/bash
# Build under WSL/Linux — the OpenOrbis toolchain is a Linux toolchain. Running
# this from Git Bash picks up the Windows MSVC generator and fails in try_compile.
set -e
cd "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
if [ -z "$OO_PS4_TOOLCHAIN" ]; then
    echo "OO_PS4_TOOLCHAIN is not set. See the OpenOrbis PS4 Toolchain README." >&2
    exit 1
fi
if [ "$1" = "clean" ]; then rm -rf build; echo "Cleaned."; exit 0; fi
cmake -B build -S . -DCMAKE_TOOLCHAIN_FILE=cmake/oo-ps4-toolchain.cmake
cmake --build build

# --- post-link: no static initialisers -------------------------------------
#
# Nothing in this plugin runs global constructors. GoldHEN enters at _init,
# which crtprx.o forwards straight to module_start; nobody walks .init_array.
# A file-scope object with a constructor therefore links perfectly cleanly and
# then simply never runs — which on console looks like a null pointer in a
# subsystem that "should" have been initialised, with nothing in the log to say
# why. The whole codebase is written around that constraint and nothing checked
# it. It passes today; the point of this is that it keeps passing.
#
# Scope is deliberately .init_array and nothing else. The ELF does carry a
# .gcc_except_table (CMakeLists.txt sets neither -fno-exceptions nor -fno-rtti);
# whether to add those flags is a separate decision and is NOT what this guards.
elf=build/Luma
if [ ! -f "$elf" ]; then
    echo "FATAL: post-link check cannot find $elf — the link produced no ELF." >&2
    exit 1
fi
if ! command -v readelf >/dev/null 2>&1; then
    echo "FATAL: readelf not found; cannot verify $elf runs no static initialisers." >&2
    exit 1
fi

found=$(readelf -SW "$elf" | grep -oE '\.(preinit_array|init_array|ctors)\b' | sort -u | tr '\n' ' ')
if [ -n "$found" ]; then
    echo "" >&2
    echo "FATAL: $elf carries ${found}— this plugin runs no static initialisers," >&2
    echo "       so whatever is in there is dead code that silently never runs." >&2
    echo "       Find the file-scope object with a constructor and make it a" >&2
    echo "       function-local or an explicitly-initialised POD." >&2
    exit 1
fi

# The section can be stripped while the dynamic tag still points the loader at
# a non-empty array, so check the size the loader would actually act on too.
# A zero-sized INIT_ARRAY tag is normal and present in a clean build.
for sz in $(readelf -dW "$elf" | awk '/\((PRE)?INIT_ARRAYSZ\)/ { print $(NF-1) }'); do
    if [ "$sz" != "0" ]; then
        echo "" >&2
        echo "FATAL: $elf declares an INIT_ARRAY of $sz bytes — static initialisers" >&2
        echo "       that will never be run. See above." >&2
        exit 1
    fi
done
echo "post-link: no .init_array / .preinit_array / .ctors        ok"
