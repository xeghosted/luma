#!/usr/bin/env python3
"""Verify that the frame hook's target can actually be detoured.

This exists because getting it wrong crashed the game, and did so in a way that
looked like anything *but* a bad hook target: the plugin booted, resolved the
native registry, loaded 7,072 bindings, started a resource, and logged that the
frame hook was alive — and then died. Every line in the log was written by our
code before the crash, so the log looked like a success.

The cause is that GoldHEN's Detour relocates nothing. Reading the SDK's
relocation records for Detour_DetourFunction64 shows the whole algorithm:

    Detour_GetInstructionSize   -> how many bytes make whole instructions (>= 14)
    sceKernelMmap               -> allocate the stub
    memcpy                      -> copy the original bytes into it
    Detour_WriteJump64          -> append a jump back

HDE64 is a *length* disassembler: it returns instruction sizes and nothing else.
So the original prologue is copied byte-for-byte to a different address, and any
instruction whose meaning depends on where it sits is silently broken:

  - `mov rax, [rip + disp32]` now resolves against the stub's address.
  - a relative jmp/call now lands somewhere else entirely.

PLAYER_PED_ID (0x21BDAC0) begins:

    push rbp / mov rbp,rsp / push rbx / push rax   ; 6 bytes
    mov  rax, [rip + 0x62dcbbb]                    ; 7 bytes, ends at 13
    mov  rdi, [rax + 8]                            ; dereferences it immediately

A 14-byte patch cannot avoid that instruction, so the stub loaded a wrong
pointer and the first return through the trampoline faulted.

47% of this build's natives have the same problem, so picking a hook target by
name is a coin flip. This checks the one we picked, and fails the suite if
anyone changes it to a target that cannot survive the copy.
The check has two halves, and they have different dependencies on purpose:

  1. Do HOOK_RVA and HOOK_PROLOGUE in game_thread.cpp still agree with the
     eboot? Pure byte comparison, no disassembler. This is the everyday
     regression: change the RVA to another native and the recorded prologue
     stops matching, so the suite goes red until someone re-derives it.

  2. Is that prologue actually relocatable? Needs a length-aware decoder, so it
     runs only where capstone is importable. It only *needs* re-running when
     the target changes -- which half 1 is what forces.

Splitting them means the gate always does something real. Requiring capstone
outright would have made the suite fail on a machine without it in a way that
reads identically to a genuinely unsafe target, which is the same
"can't-check looks like failed" confusion this tool exists to remove.
"""
import argparse
import os
import re
import struct
import sys

try:
    import capstone
except ImportError:
    capstone = None

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TEXT_FILE_OFFSET = 0x4000

# GoldHEN's Detour writes JumpInstructions64[14] over the target, then copies
# whole instructions covering at least that many bytes into the stub.
PATCH_BYTES = 14


def read_text(path):
    with open(path, "rb") as handle:
        data = handle.read()
    if data[:4] != b"\x7fELF":
        sys.exit(f"{path}: not an ELF")
    phoff = struct.unpack_from("<Q", data, 0x20)[0]
    phentsize = struct.unpack_from("<H", data, 0x36)[0]
    phnum = struct.unpack_from("<H", data, 0x38)[0]
    for i in range(phnum):
        off = phoff + i * phentsize
        p_type, p_flags = struct.unpack_from("<II", data, off)
        p_offset, _, _, p_filesz, _, _ = struct.unpack_from("<QQQQQQ", data, off + 8)
        if p_type == 1 and (p_flags & 1):
            return data[p_offset:p_offset + p_filesz]
    sys.exit(f"{path}: no executable segment")


def detour_safe(md, text, rva, need=PATCH_BYTES):
    """(ok, reason, instructions) for detouring `rva` with a `need`-byte patch."""
    total = 0
    covered = []
    for insn in md.disasm(text[rva:rva + 64], rva):
        covered.append(insn)
        total += insn.size
        if total >= need:
            break

    if total < need:
        return False, f"prologue is only {total} bytes; a {need}-byte patch would " \
                      f"overwrite past the end of the function", covered

    for insn in covered:
        offset = insn.address - rva
        if insn.mnemonic == "ret":
            return False, f"`ret` at +{offset} sits inside the patched region", covered
        for op in insn.operands:
            if op.type == capstone.x86.X86_OP_MEM and op.mem.base == capstone.x86.X86_REG_RIP:
                return False, (f"rip-relative `{insn.mnemonic} {insn.op_str}` at +{offset}: "
                               "memcpy'd to the stub it resolves against the wrong address"), covered
        if set(insn.groups) & {capstone.CS_GRP_JUMP, capstone.CS_GRP_CALL}:
            return False, (f"relative `{insn.mnemonic} {insn.op_str}` at +{offset}: "
                           "its target is encoded as a displacement from here"), covered

    return True, f"{total} bytes copied, all position-independent", covered


def hook_target_from_source(path, game):
    """(HOOK_RVA, HOOK_PROLOGUE) as the plugin itself declares them."""
    with open(path, encoding="utf-8") as handle:
        source = handle.read()

    # One .prx serves several games, so the source now declares one signature
    # per game and this checks the one for the eboot it was handed.
    pro = re.search(r"%s_HOOK\[\s*(\d+)\s*\]\s*=\s*\{(.*?)\}" % game.upper(), source, re.S)
    if not pro:
        sys.exit(f"{path}: no {game.upper()}_HOOK signature found")
    declared_len = int(pro.group(1))
    prologue = bytes(int(b, 16) for b in re.findall(r"0x([0-9A-Fa-f]{2})", pro.group(2)))
    if len(prologue) != declared_len:
        sys.exit(f"{path}: {game.upper()}_HOOK declares {declared_len} bytes but lists {len(prologue)}")

    rvas = re.findall(r"0x([0-9A-Fa-f]+),\s*%s_HOOK" % game.upper(), source)
    if not rvas:
        sys.exit(f"{path}: no hook rva paired with {game.upper()}_HOOK")
    return int(rvas[0], 16), prologue


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("eboot", nargs="?",
                        help="the decrypted RDR2 eboot the RVAs belong to")
    parser.add_argument("--game", default="rdr2", help="which profile to check: gta5 or rdr2")
    parser.add_argument("--source", default=os.path.join(HERE, "src", "game", "profile.cpp"))
    args = parser.parse_args()

    # tests/run.sh runs under WSL while the eboot is named by a Windows path, so
    # try both spellings before giving up. Getting this wrong would make the gate
    # SKIP silently on the machine that actually runs it -- a check that always
    # passes is worse than no check, because it reads as coverage.
    # Each game's signature only means anything against THAT game's eboot.
    # Checking the gta5 profile against RDR2's image would report a mismatch
    # that says nothing about the gta5 profile, so the eboots are per game and
    # a missing one is a skip rather than a failure.
    EBOOTS = {
        "rdr2": [r"E:\Projects\IDA\PS4\RDR2\eboot.bin",
                 "/mnt/e/Projects/IDA/PS4/RDR2/eboot.bin"],
        # No raw 1.57 image is on hand -- only an IDA database, which this tool
        # cannot read. The gta5 signature was taken from that database once and
        # cross-checked against RDR2's eboot in all four combinations; it is
        # simply not re-derivable here until a 1.57 eboot.bin turns up.
        "gta5": [r"E:\Projects\IDA\PS4\GTA5\eboot.bin",
                 "/mnt/e/Projects/IDA/PS4/GTA5/eboot.bin"],
    }
    candidates = [args.eboot] if args.eboot else EBOOTS.get(args.game, [])
    eboot = next((p for p in candidates if p and os.path.exists(p)), None)
    if eboot is None:
        print(f"SKIP: no eboot for {args.game}; cannot re-derive its hook signature")
        print("      looked in: " + ", ".join(p for p in candidates if p))
        return 0

    text = read_text(eboot)
    rva, declared = hook_target_from_source(args.source, args.game)
    actual = text[rva:rva + len(declared)]
    print(f"frame hook target: 0x{rva:X}")

    # --- half 1: the recorded prologue against the binary (no disassembler) ---
    if actual != declared:
        print("", file=sys.stderr)
        print("FAILED: HOOK_PROLOGUE does not match the eboot at HOOK_RVA.", file=sys.stderr)
        print(f"        declared: {declared.hex(' ')}", file=sys.stderr)
        print(f"        eboot:    {actual.hex(' ')}", file=sys.stderr)
        print("        Either the target moved, or the hook was pointed at a different", file=sys.stderr)
        print("        native without re-deriving whether its prologue can be relocated.", file=sys.stderr)
        print("        Re-run this on a machine with capstone before trusting a new one.", file=sys.stderr)
        return 1
    print(f"  recorded prologue matches the eboot ({len(declared)} bytes)")

    # --- half 2: is it actually relocatable? (needs capstone) ---
    if capstone is None:
        print("  relocatability NOT re-derived here: capstone is not installed.")
        print("  (The bytes are unchanged since it was, so this is a weaker but real check.)")
        return 0

    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
    md.detail = True
    ok, reason, covered = detour_safe(md, text, rva)
    for insn in covered:
        print(f"    +{insn.address - rva:<3} {insn.size:<2}  {insn.mnemonic:<9} {insn.op_str}")
    print(f"  -> {'SAFE' if ok else 'UNSAFE'}: {reason}")

    if not ok:
        print("", file=sys.stderr)
        print("FAILED: this target cannot be detoured by GoldHEN's Detour.", file=sys.stderr)
        print("        It copies the prologue with memcpy and relocates nothing, so the", file=sys.stderr)
        print("        stub would run a position-dependent instruction at the wrong", file=sys.stderr)
        print("        address. The game boots, logs happily, and then crashes on the", file=sys.stderr)
        print("        first return through the trampoline.", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
