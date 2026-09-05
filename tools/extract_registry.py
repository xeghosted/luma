#!/usr/bin/env python3
"""Recover the complete native registry from an RDR2 PS4 eboot.

RDR2 registers its natives INLINE, and without obfuscation. There is no
RegisterCommand call to hook or to scan for -- which is why looking for GTA V's
`movabs hash / lea fn / call RegisterCommand` shape finds nothing here. The
compiler emitted the whole registry straight into the registering functions, as
pairs of stores into a node:

    mov     [r14 + rax*8 + 0x48], rcx   ; node->hashes[i] = hash
    mov     [r14 + rax*8 + 8],   rdx    ; node->funcs[i]  = handler
    mov     [r14 + 0x40], ebx           ; node->count = i + 1

**The store pair is the anchor, not the hash literal.** An earlier version of
this tool keyed on `movabs r64, imm64` instead, and that was wrong twice over:

  - It admitted ordinary constants. `movabs rcx, 0x100000000` followed by an
    unrelated lea produced three bogus handlers for one hash.
  - It missed every native whose hash fits in 32 bits. The compiler does not
    emit `movabs` for those -- it folds the value straight into the store, as
    `mov qword ptr [r12 + rax*8 + 0x48], 0x50597EE2` (that one is TIMESTEP).
    Keying on a 64-bit immediate cannot see them at all.

So this walks the two stores and traces each operand back to its source: the
hash from an immediate (in the store itself, or from a preceding movabs/mov),
the handler from a preceding rip-relative lea. Both encodings fall out of the
same logic.

Nothing here needs a booted console and nothing needs decoding, so the result is
reproducible from the file alone.
"""
import argparse
import json
import re
import struct
import sys

try:
    import capstone
except ImportError:
    sys.exit("capstone is required:  pip install capstone")

# ph00 of the RDR2 eboot: file offset 0x4000 maps to vaddr 0.
TEXT_FILE_OFFSET = 0x4000

HASH_SLOT = 0x48   # node->hashes[i], disp8 on a [base + index*8] address
FUNC_SLOT = 0x08   # node->funcs[i], same base and index

# Candidate anchor bytes: a REX.W store with SIB addressing and disp8 == 0x48,
# in either the register form (89 /r) or the immediate form (C7 /0). Used only
# to pick windows worth disassembling -- correctness comes from the decode.
RE_ANCHOR = re.compile(
    rb"[\x48-\x4F](?:\x89[\x44\x4C\x54\x5C\x64\x6C\x74\x7C]|\xC7\x44)[\x00-\xFF]\x48",
    re.S,
)

# How far back an operand's source may sit. The registering functions interleave
# a few instructions between the lea and the stores, and a handful separate them
# by considerably more; at 64 bytes ten real natives were lost, among them
# COMPENDIUM_FISH_CAUGHT and _WRITE_DATA_BOOL.
LOOKBEHIND = 192
LOOKAHEAD = 48


def read_text_segment(path):
    with open(path, "rb") as handle:
        data = handle.read()
    if data[:4] != b"\x7fELF":
        sys.exit(f"{path}: not an ELF (GoldHEN FTP serves RDR2 eboots already decrypted)")

    phoff = struct.unpack_from("<Q", data, 0x20)[0]
    phentsize = struct.unpack_from("<H", data, 0x36)[0]
    phnum = struct.unpack_from("<H", data, 0x38)[0]
    for i in range(phnum):
        off = phoff + i * phentsize
        p_type, p_flags = struct.unpack_from("<II", data, off)
        p_offset, p_vaddr, _, p_filesz, _, _ = struct.unpack_from("<QQQQQQ", data, off + 8)
        if p_type == 1 and (p_flags & 1):
            if p_offset != TEXT_FILE_OFFSET or p_vaddr != 0:
                sys.exit(
                    f"{path}: text segment is off=0x{p_offset:x} va=0x{p_vaddr:x}; this build "
                    "does not use the RVA convention the extractor assumes"
                )
            return data[p_offset:p_offset + p_filesz], p_filesz
    sys.exit(f"{path}: no executable PT_LOAD segment")


def slot_operand(insn):
    """If insn stores to [base + index*8 + disp8], return (disp, base, index, src).

    src is the source register id, or None when the source is an immediate.
    """
    if insn.mnemonic != "mov" or len(insn.operands) != 2:
        return None
    dst, src = insn.operands
    if dst.type != capstone.x86.X86_OP_MEM:
        return None
    mem = dst.mem
    if mem.base == 0 or mem.index == 0 or mem.scale != 8:
        return None
    if src.type == capstone.x86.X86_OP_REG:
        return mem.disp, mem.base, mem.index, src.reg, None
    if src.type == capstone.x86.X86_OP_IMM:
        return mem.disp, mem.base, mem.index, None, src.imm
    return None


# 32-bit / 16-bit / 8-bit names mapped onto their 64-bit parent.
PARENT = {}
for wide, parts in {
    "rax": ("eax", "ax", "al", "ah"), "rbx": ("ebx", "bx", "bl", "bh"),
    "rcx": ("ecx", "cx", "cl", "ch"), "rdx": ("edx", "dx", "dl", "dh"),
    "rsi": ("esi", "si", "sil"), "rdi": ("edi", "di", "dil"),
    "rbp": ("ebp", "bp", "bpl"), "rsp": ("esp", "sp", "spl"),
}.items():
    PARENT[wide] = wide
    for part in parts:
        PARENT[part] = wide
for n in range(8, 16):
    PARENT[f"r{n}"] = f"r{n}"
    for suffix in ("d", "w", "b"):
        PARENT[f"r{n}{suffix}"] = f"r{n}"


def canon(md, reg_id):
    name = md.reg_name(reg_id) or ""
    return PARENT.get(name, name)


def trace_immediate(md, window, target, anchor_index, depth=0):
    """Constant value of `target` just before anchor_index, or None.

    Two shapes produce a hash. The plain one is an immediate:

        movabs r13, 0x725D52F21A5E9E00

    The other is the compiler noticing that consecutive natives have hashes a
    few counts apart, keeping one in a register and deriving its neighbours:

        lea    rcx, [r13 + 0x22]        ; = 0x725D52F21A5E9E22

    Following that `lea` is not optional. Reading the register's own value and
    ignoring the displacement yields a hash that is *almost* right, which is the
    worst kind of wrong: it is a real native's hash, just not this one's, so it
    binds a valid name to the wrong address without anything looking amiss.
    """
    if depth > 4:
        return None
    for offset, insn in enumerate(reversed(window[:anchor_index])):
        if len(insn.operands) != 2:
            continue
        dst, src = insn.operands
        if dst.type != capstone.x86.X86_OP_REG or canon(md, dst.reg) != target:
            continue
        index_here = anchor_index - 1 - offset

        if insn.mnemonic in ("mov", "movabs"):
            if src.type == capstone.x86.X86_OP_IMM:
                return src.imm & 0xFFFFFFFFFFFFFFFF
            if src.type == capstone.x86.X86_OP_REG:
                return trace_immediate(md, window, canon(md, src.reg), index_here, depth + 1)
            return None

        if insn.mnemonic == "lea" and src.type == capstone.x86.X86_OP_MEM:
            mem = src.mem
            if mem.index != 0 or mem.base == 0:
                return None
            base = canon(md, mem.base)
            if base == "rip":
                return None
            value = trace_immediate(md, window, base, index_here, depth + 1)
            return None if value is None else (value + mem.disp) & 0xFFFFFFFFFFFFFFFF

        return None  # target was clobbered by something that is not a constant
    return None


def trace_lea(md, window, target, anchor_index, text_size):
    """Most recent rip-relative lea into .text that wrote `target`."""
    for insn in reversed(window[:anchor_index]):
        if insn.mnemonic != "lea" or len(insn.operands) != 2:
            continue
        dst, src = insn.operands
        if dst.type != capstone.x86.X86_OP_REG or canon(md, dst.reg) != target:
            continue
        if src.type != capstone.x86.X86_OP_MEM:
            return None
        if canon(md, src.mem.base) != "rip" and md.reg_name(src.mem.base) != "rip":
            return None
        addr = insn.address + insn.size + src.mem.disp
        return addr if 0x1000 <= addr < text_size else None
    return None


def window_at(md, text, pos, text_size):
    """Disassemble around `pos`, returning (instructions, index of the one at pos).

    x86 has no way to decode backwards, so the window has to be decoded forward
    from some earlier byte -- and an arbitrary earlier byte is usually the middle
    of an instruction, which desynchronises the decode so that it never lands on
    `pos` at all. That is not hypothetical: fixing `start` at `pos - LOOKBEHIND`
    silently dropped 34 real natives, ROUND and CEIL among them, whose
    registrations are perfectly ordinary. Retry from successively closer starts
    until one synchronises on `pos`; the first almost always does.
    """
    end = min(text_size, pos + LOOKAHEAD)
    for delta in range(LOOKBEHIND, 7, -1):
        start = pos - delta
        if start < 0:
            continue
        window = list(md.disasm(text[start:end], start))
        for i, insn in enumerate(window):
            if insn.address == pos:
                return window, i
    window = list(md.disasm(text[pos:end], pos))
    return (window, 0) if window else (None, None)


def extract(text, text_size):
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
    md.detail = True

    pairs = []
    for anchor in RE_ANCHOR.finditer(text):
        pos = anchor.start()
        window, anchor_index = window_at(md, text, pos, text_size)
        if anchor_index is None:
            continue

        hash_store = slot_operand(window[anchor_index])
        if hash_store is None or hash_store[0] != HASH_SLOT:
            continue
        _, base, index, src_reg, imm = hash_store

        # The handler store: same base, same index, disp 8. It sits next to the
        # hash store in every registration -- requiring it is what rejects an
        # ordinary constant that merely looks like a hash.
        handler = None
        for follow in window[anchor_index + 1:anchor_index + 5]:
            other = slot_operand(follow)
            if other is None:
                continue
            disp, obase, oindex, oreg, _ = other
            if disp != FUNC_SLOT or obase != base or oindex != index or oreg is None:
                continue
            handler = trace_lea(md, window, canon(md, oreg), anchor_index, text_size)
            break
        if handler is None:
            continue

        if imm is not None:
            native_hash = imm & 0xFFFFFFFFFFFFFFFF
        else:
            native_hash = trace_immediate(md, window, canon(md, src_reg), anchor_index)
        if not native_hash:
            continue

        pairs.append((native_hash, handler))
    return pairs


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("eboot", help="path to the decrypted RDR2 eboot.bin")
    parser.add_argument("-o", "--out", default="tools/native_registry.json")
    args = parser.parse_args()

    text, text_size = read_text_segment(args.eboot)
    pairs = extract(text, text_size)

    # A hash that decodes to two different handlers is an ambiguity we cannot
    # resolve here, and guessing is the one outcome worth avoiding: a plausible
    # wrong address binds a real name to the wrong code and fails silently at
    # runtime. Such hashes are reported and then dropped, so a native is missing
    # rather than lying. (The trace does not model control flow, so a wide
    # LOOKBEHIND can read a register value across a branch where it is not
    # actually live -- which is where most of these come from.)
    candidates = {}
    for native_hash, handler in pairs:
        candidates.setdefault(native_hash, set()).add(handler)

    registry = {h: next(iter(v)) for h, v in candidates.items() if len(v) == 1}
    conflicts = {h: v for h, v in candidates.items() if len(v) > 1}

    # Every handler must begin a function. 16-byte alignment is the cheap form
    # of that check and is total on a correct extraction, so anything less means
    # the pattern has drifted and the output should be rejected rather than
    # quietly trusted.
    misaligned = [h for h in registry.values() if h % 16]

    payload = {
        "source": args.eboot,
        "count": len(registry),
        "natives": {f"0x{k:016X}": f"0x{v:X}" for k, v in sorted(registry.items())},
        "conflicts": {f"0x{k:016X}": sorted(f"0x{a:X}" for a in v)
                      for k, v in sorted(conflicts.items())},
    }
    with open(args.out, "w", encoding="utf-8", newline="\n") as handle:
        json.dump(payload, handle, indent=1)

    print(f"registrations decoded : {len(pairs)}")
    print(f"distinct hashes       : {len(registry)}")
    print(f"hashes w/ 2 handlers  : {len(conflicts)}")
    print(f"handlers misaligned   : {len(misaligned)}  (must be 0)")
    print(f"written               : {args.out}")
    return 1 if misaligned else 0


if __name__ == "__main__":
    sys.exit(main())
