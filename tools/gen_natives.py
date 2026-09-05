#!/usr/bin/env python3
"""Generate the native bindings for RDR2Lua.

Two inputs, joined on the native hash:

  tools/native_registry.json    hash -> handler RVA, extracted from OUR eboot
                                (tools/extract_registry.py)
  tools/rdr2_native_signatures.h  name + C signature -> hash, vendored
                                (see the .PROVENANCE.md beside it)

The join is on the hash, never on position. That matters: GTALua's equivalent
generator paired an ordered name list against an ordered address list, and a
one-slot slip in eleven places gave eleven natives both a neighbour's address
AND a neighbour's signature -- silently, because both halves moved together.
A hash join cannot produce that failure at all. It can only produce a native
with no address (dropped, and counted) or an address with no name (emitted as
_0xHASH, and counted).

Every output is written with newline="
". The generated files are committed
and tests/run.sh gates on a regeneration matching them byte for byte; without
that, Python's default newline translation makes the check fail purely because
it ran on Windows rather than in WSL, which says nothing about the bindings.

Outputs:
  src/game/rdr2/natives.h      C++ wrappers calling _i<R>(0xRVA, ...)
  scripts/natives.lua          Lua bindings, name -> (rva, signature)
  editor/lua-defs/natives.def.lua  LSP definitions for autocomplete
"""
import argparse
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# A vendored line looks like:
#   static <ret> <NAME>(<params>) { [return] invoke<T>(0xHASH, a, b); } // ...
RE_NATIVE = re.compile(
    r"^static\s+(?P<ret>[A-Za-z_][\w:]*\s*\**)\s+"
    r"(?P<name>[A-Za-z_]\w*)\s*"
    r"\((?P<params>[^)]*)\)\s*\{[^}]*?invoke<[^>]*>\s*\(\s*0x(?P<hash>[0-9A-Fa-f]{16})",
    re.M,
)

# Signature characters, as understood by the Lua bridge: return type first.
#   v void   i int   f float   b bool   s string   p pointer   3 vector3
SIG_BY_TYPE = {
    "void": "v",
    "Void": "v",
    "float": "f",
    "BOOL": "b",
    "Vector3": "3",
}


def sig_char(ctype):
    """Map one C type from the vendored header to a bridge signature char."""
    ctype = ctype.strip()
    if ctype.endswith("*"):
        # const char* is a string; every other pointer is an out-param address.
        return "s" if "char" in ctype else "p"
    return SIG_BY_TYPE.get(ctype, "i")  # Any/Hash/Ped/Entity/... are all int32


def cpp_type(ctype):
    """The C++ type to emit in natives.h for a vendored type."""
    ctype = ctype.strip()
    if ctype in ("void", "Void"):
        return "Void"
    # A Vector3 out-param must be the PADDED 24-byte layout, not a packed
    # {x,y,z}: the natives read and write the caller's buffer at an 8-byte
    # stride (see invoker.h). Emitting a packed type here would compile fine
    # and then drop y into x's padding at runtime.
    if ctype.replace(" ", "") == "Vector3*":
        return "rage::invoker::script_vec3*"
    if ctype == "Vector3":
        return "rage::invoker::script_vec3"
    return ctype


def parse_params(params):
    """Return [(ctype, name)] for a vendored parameter list."""
    params = params.strip()
    if not params or params == "void":
        return []
    out = []
    for index, raw in enumerate(params.split(",")):
        raw = raw.strip()
        match = re.match(r"^(.*?)([A-Za-z_]\w*)$", raw)
        if match and match.group(1).strip():
            ctype, name = match.group(1).strip(), match.group(2)
        else:
            ctype, name = raw, f"p{index}"
        if name in ("char", "int", "float", "void"):   # unnamed pointer params
            ctype, name = raw, f"p{index}"
        out.append((ctype, name))
    return out


def load_signatures(path):
    """name/signature records keyed by hash, from the vendored header."""
    with open(path, encoding="utf-8", errors="replace") as handle:
        source = handle.read()

    by_hash = {}
    for match in RE_NATIVE.finditer(source):
        native_hash = int(match.group("hash"), 16)
        if native_hash in by_hash:
            continue  # first spelling wins; the header repeats a few
        by_hash[native_hash] = {
            "name": match.group("name"),
            "ret": match.group("ret").strip(),
            "params": parse_params(match.group("params")),
        }
    return by_hash


def lua_escape(name):
    return name


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--registry", default=os.path.join(HERE, "tools", "native_registry.json"))
    parser.add_argument("--signatures", default=os.path.join(HERE, "tools", "rdr2_native_signatures.h"))
    parser.add_argument("--out-dir", default=HERE,
                        help="repo root to write into; point it at a temp tree to "
                             "diff a fresh generation against the committed files")
    args = parser.parse_args()
    root = args.out_dir

    with open(args.registry, encoding="utf-8") as handle:
        registry_doc = json.load(handle)
    registry = {int(k, 16): int(v, 16) for k, v in registry_doc["natives"].items()}
    signatures = load_signatures(args.signatures)

    # Three populations, and conflating them overstates the result badly. The
    # vendored header covers most hashes, but for most of those its "name" is
    # itself a _0xHASH placeholder -- so "has an entry in the reference" is not
    # the same as "has a name a human can use", and reporting the former as the
    # latter turns 2.3k spelled natives into a claimed 6.7k.
    spelled = 0      # a real name, with a signature
    placeholder = 0  # in the reference, but only as _0xHASH
    absent = 0       # not in the reference at all
    rows = []
    for native_hash in sorted(registry):
        rva = registry[native_hash]
        info = signatures.get(native_hash)
        if info is None:
            absent += 1
            info = {"name": f"_0x{native_hash:016X}", "ret": "Any", "params": []}
        elif re.match(r"^_0[xX][0-9A-Fa-f]+$", info["name"]):
            placeholder += 1
        else:
            spelled += 1
        rows.append((native_hash, rva, info))
    named = spelled
    unnamed = placeholder + absent

    missing = [h for h in signatures if h not in registry]

    for sub in (("src", "game", "rdr2"), ("scripts",), ("editor", "lua-defs")):
        os.makedirs(os.path.join(root, *sub), exist_ok=True)

    # ---- natives.h -------------------------------------------------------
    out_h = os.path.join(root, "src", "game", "rdr2", "natives.h")
    with open(out_h, "w", encoding="utf-8", newline="\n") as handle:
        handle.write(f"""#pragma once
// GENERATED by tools/gen_natives.py -- do not edit by hand.
//
// Native wrappers for RDR2 PS4 CUSA03041 v1.32. The literal in each _i<> call
// is the handler's RVA into eboot.bin, not a hash; the invoker adds the runtime
// image base. Names and signatures come from tools/rdr2_native_signatures.h,
// addresses from this build's own registry -- joined on the hash, so a wrong
// name can never drag a wrong address along with it.
//
// {len(rows)} natives: {spelled} with real names, {unnamed} bound as _0xHASH.
#include "invoker.h"
#include "rage/types/base_types.h"
#define _i rage::invoker::invoke

namespace native {{

""")
        for native_hash, rva, info in rows:
            ret = cpp_type(info["ret"])
            params = info["params"]
            decl = ", ".join(f"{cpp_type(t)} {n}" for t, n in params)
            call = "".join(f", {n}" for _, n in params)
            body = f"_i<{ret}>(0x{rva:X}{call})"
            # `Void` is `typedef int Void` -- the RAGE ABI always produces a
            # return slot, even for natives that mean nothing by it. So a
            # Void-returning wrapper still has to return: omitting it leaves a
            # non-void function falling off the end, which is UB, not a no-op.
            lead = "" if ret == "void" else "return "
            handle.write(
                f"\tstatic {ret} {info['name'].lower()}({decl}) {{ {lead}{body}; }}"
                f"  // 0x{native_hash:016X}\n"
            )
        handle.write("}\n")

    # ---- natives.lua -----------------------------------------------------
    out_lua = os.path.join(root, "scripts", "natives.lua")
    with open(out_lua, "w", encoding="utf-8", newline="\n") as handle:
        handle.write(f"""-- GENERATED by tools/gen_natives.py -- do not edit by hand.
-- RDR2 PS4 CUSA03041 v1.32: {len(rows)} natives; {spelled} have real names,
-- the other {unnamed} are bound as _0xHASH because no public name exists for them.
--
-- Every native here is reached by RVA. RDR2Lua needs no runtime hash-table walk
-- -- unlike GTALua, where 3,674 of 6,485 natives had no address and had to be
-- resolved out of the running game -- because RDR2's registrations sit in the
-- eboot unobfuscated and all {len(rows)} addresses are known before boot.
local native = native

""")
        for native_hash, rva, info in rows:
            sig = sig_char(info["ret"]) + "".join(sig_char(t) for t, _ in info["params"])
            args = ", ".join(n for _, n in info["params"])
            handle.write(
                f"{lua_escape(info['name'])} = native(0x{rva:X}, \"{sig}\")"
                f"  -- 0x{native_hash:016X}\n"
            )

    # ---- LSP defs --------------------------------------------------------
    out_def = os.path.join(root, "editor", "lua-defs", "natives.def.lua")
    LUA_TYPE = {"v": "nil", "i": "integer", "f": "number", "b": "boolean",
                "s": "string", "p": "integer", "3": "table"}
    with open(out_def, "w", encoding="utf-8", newline="\n") as handle:
        handle.write("---@meta\n-- GENERATED by tools/gen_natives.py -- do not edit by hand.\n\n")
        for native_hash, rva, info in rows:
            for ctype, name in info["params"]:
                handle.write(f"---@param {name} {LUA_TYPE[sig_char(ctype)]}\n")
            ret = sig_char(info["ret"])
            if ret != "v":
                handle.write(f"---@return {LUA_TYPE[ret]}\n")
            names = ", ".join(n for _, n in info["params"])
            handle.write(f"function {info['name']}({names}) end\n\n")

    # ---- natives_registry.h ---------------------------------------------
    # Backs invoke_hash()/native_exists() with a compiled-in table instead of
    # GTALua's runtime walk of the game's own hash table. GTALua had to walk:
    # more than half its natives had no RVA, so the addresses only existed
    # inside the running game, behind XOR-masked nodes. Here every address is
    # known before boot, so the whole mechanism -- the walk, its verification
    # step, and the "registry not ready yet" state a script had to guard for --
    # collapses into a binary search over a sorted array.
    out_reg = os.path.join(root, "src", "game", "rdr2", "natives_registry.h")
    with open(out_reg, "w", encoding="utf-8", newline="\n") as handle:
        handle.write(f"""#pragma once
#include <stdint.h>
// GENERATED by tools/gen_natives.py -- do not edit by hand.
//
// hash -> handler RVA for RDR2 PS4 CUSA03041 v1.32, sorted by hash so a lookup
// is a binary search. {len(rows)} entries.
namespace rage::natives_registry {{

struct entry {{ uint64_t hash; uint32_t rva; }};

static const entry k_entries[] = {{
""")
        for native_hash, rva, _ in rows:
            handle.write(f"\t{{ 0x{native_hash:016X}ULL, 0x{rva:X} }},\n")
        handle.write(f"""}};

static const uint32_t k_count = {len(rows)};

}}  // namespace rage::natives_registry
""")

    print(f"natives with an address : {len(rows)}")
    print(f"  spelled-out names     : {spelled}")
    print(f"  _0xHASH placeholders  : {placeholder} (named that way by the reference)")
    print(f"  absent from reference : {absent} (signature unknown, callable by hash)")
    print(f"signatures with no address in this build: {len(missing)}")
    print(f"wrote {out_h}")
    print(f"wrote {out_lua}")
    print(f"wrote {out_def}")


if __name__ == "__main__":
    sys.exit(main())
