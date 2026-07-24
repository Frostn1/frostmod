#!/usr/bin/env python3
"""
find_track_loader.py  --  pin RVA_TRACK_LOADER for the surgical/incremental reload.

Goal (the one TODO in offsets.h:48):
    constexpr uintptr_t RVA_TRACK_LOADER = 0x000000;  // TODO: xref writer of 0x140f43298

The track list is  qword_14109de98  (ptr to a heap array, stride 1220) and its
COUNT is  dword_140f43298 . The per-category track loader is the sub inside
fcn.1400ef210 that writes that count. This script does the "capstone xref sweep on
the raw file" that recovered every other RVA in offsets.h (see offsets.h:160-161),
but pointed at 0xf43298, then:

  1. finds every instruction that WRITES the track count,
  2. walks back to the enclosing function prologue      -> candidate RVA_TRACK_LOADER,
  3. disassembles that function and flags, inside it:
       * the CLEAR   (where it zeroes the count / list-head globals), and
       * the inner SCAN calls to the generic walker fcn.140158be0 (RVA 0x158be0),
         with the `lea` that sets the directory arg (game dir vs mods dir).

That {clear; scan(gameDir); scan(modsDir)} shape is exactly what RL_Dir already
exploits for DIR-style loaders (frostmod.cpp:1285). Pinning it for tracks lets us
call the inner SCAN on a directory scoped to only the NEW .pkz, skipping the clear
-> append just the new track, everything else stays loaded.

USAGE
    pip install capstone pefile
    python find_track_loader.py path\\to\\mxbikes.exe
    python find_track_loader.py dump.bin --base 0x140000000 --flat   # decrypted mem dump

IMPORTANT (SteamStub): the shipping exe is DRM-wrapped and .text is decrypted in
place at runtime (offsets.h:6-8). The script SELF-VALIDATES by disassembling the
known scanner at 0x158be0 and checking it against SIG_SCAN_FOLDER. If that check
FAILS, the .text you fed it is still encrypted -> feed a runtime dump instead
(x64dbg/Scylla dump of mxbikes.exe while the game is running), or run this against
whatever "raw file" produced the existing offsets.

Paste the whole report back and I'll finalize RVA_TRACK_LOADER + the RL_AddDir step.
"""
import argparse
import sys

try:
    from capstone import Cs, CS_ARCH_X86, CS_MODE_64, CS_AC_WRITE
    from capstone.x86 import X86_REG_RIP, X86_OP_MEM, X86_OP_IMM
except ImportError:
    sys.exit("need capstone:  pip install capstone")

# ---- known RVAs from offsets.h (base 0x140000000) --------------------------------
RVA_TRACK_COUNT  = 0xf43298    # dword_140f43298  -- the value the track loader writes
RVA_TRACK_LIST   = 0x109de98   # qword_14109de98  -- ptr to the heap track array
RVA_SCAN_FOLDER  = 0x158be0    # fcn.140158be0    -- generic VFS dir walker (the inner scan)
RVA_CONTENT_INIT = 0xef210     # fcn.1400ef210    -- boot content-load (the enclosing routine)

# SIG_SCAN_FOLDER from offsets.h:100 -- used to prove .text is decrypted & base is right.
SIG_SCAN_FOLDER = bytes([0x40,0x53,0x56,0x57,0x41,0x54,0x41,0x55,0x41,0x56,
                         0x48,0x81,0xEC,0xF8,0x07,0x00,0x00,0x48,0x8B,0x05])

INT3 = 0xCC


def load_text(path, base_override, flat):
    """Return (code_bytes, text_base_rva=0-relative offset into image, image_base)."""
    data = open(path, "rb").read()
    if flat:
        base = base_override if base_override is not None else 0x140000000
        # flat = a full-image memory dump: file offset == RVA. Whole file is the image.
        return data, 0, base
    try:
        import pefile
    except ImportError:
        sys.exit("PE parsing needs pefile (or pass --flat for a raw dump):  pip install pefile")
    pe = pefile.PE(path, fast_load=True)
    base = base_override if base_override is not None else pe.OPTIONAL_HEADER.ImageBase
    for s in pe.sections:
        if s.Name.rstrip(b"\x00") == b".text":
            # Build an RVA-indexed image slice so insn.address == image_base + RVA.
            start_rva = s.VirtualAddress
            raw = s.get_data()
            return raw, start_rva, base
    sys.exit(".text section not found")


def rip_target(insn):
    """Absolute address a RIP-relative memory operand resolves to, or None."""
    for op in insn.operands:
        if op.type == X86_OP_MEM and op.mem.base == X86_REG_RIP:
            return insn.address + insn.size + op.mem.disp
    return None


def op_writes_mem(insn):
    """True if this instruction writes its memory operand (store/inc/add to [mem])."""
    for op in insn.operands:
        if op.type == X86_OP_MEM:
            # capstone >= 4 exposes per-operand access; fall back to mnemonic heuristic.
            acc = getattr(op, "access", None)
            if acc is not None:
                return bool(acc & CS_AC_WRITE)
    m = insn.mnemonic
    return m in ("mov", "inc", "dec", "add", "sub", "and", "or", "xor", "lea") and \
        insn.operands and insn.operands[0].type == X86_OP_MEM


def call_target(insn, image_base):
    """Absolute target of a direct `call rel32`, else None."""
    if insn.mnemonic == "call" and insn.operands and insn.operands[0].type == X86_OP_IMM:
        return insn.operands[0].imm
    return None


def find_func_start(code, text_base, image_base, hit_off):
    """Walk back from a code offset to the nearest INT3-padding gap -> function start."""
    i = hit_off
    while i > 0:
        if code[i - 1] == INT3:
            # skip the run of padding
            while i < len(code) and code[i] == INT3:
                i += 1
            return i
        i -= 1
    return 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("binary", help="mxbikes.exe (or a decrypted --flat memory dump)")
    ap.add_argument("--base", type=lambda x: int(x, 0), default=None,
                    help="image base override (default: PE header, or 0x140000000 for --flat)")
    ap.add_argument("--flat", action="store_true",
                    help="treat file as a full memory dump (file offset == RVA)")
    ap.add_argument("--window", type=lambda x: int(x, 0), default=0x600,
                    help="max bytes to disassemble per candidate function (default 0x600)")
    args = ap.parse_args()

    code, text_base, image_base = load_text(args.binary, args.base, args.flat)
    md = Cs(CS_ARCH_X86, CS_MODE_64)
    md.detail = True

    def off_of(rva):          # file/slice offset of an RVA
        return rva - text_base

    def rva_of(addr):         # RVA of an absolute VA
        return addr - image_base

    print(f"image base = 0x{image_base:x} | .text starts at RVA 0x{text_base:x} "
          f"| {len(code)} bytes")

    # ---- 0) self-validate: is .text actually decrypted & base correct? -------------
    probe = code[off_of(RVA_SCAN_FOLDER): off_of(RVA_SCAN_FOLDER) + len(SIG_SCAN_FOLDER)]
    ok = probe == SIG_SCAN_FOLDER
    print(f"\n[validate] scanner @0x{RVA_SCAN_FOLDER:x} matches SIG_SCAN_FOLDER: "
          f"{'YES' if ok else 'NO'}")
    if not ok:
        print(f"           expected {SIG_SCAN_FOLDER.hex()}")
        print(f"           got      {probe.hex()}")
        print("           -> .text is still ENCRYPTED (or base is wrong). Feed a "
              "runtime dump; results below are unreliable.")

    count_abs = image_base + RVA_TRACK_COUNT
    list_abs  = image_base + RVA_TRACK_LIST
    scan_abs  = image_base + RVA_SCAN_FOLDER

    # ---- 1) sweep .text for refs to the track count / list -------------------------
    writes, reads = [], []
    for insn in md.disasm(code, image_base + text_base):
        tgt = rip_target(insn)
        if tgt in (count_abs, list_abs):
            (writes if op_writes_mem(insn) else reads).append((insn, tgt))

    def label(tgt):
        return "COUNT" if tgt == count_abs else "LIST "

    print(f"\n[xref] writers of track count/list ({len(writes)}):")
    for insn, tgt in writes:
        print(f"   W {label(tgt)} @ RVA 0x{rva_of(insn.address):06x}   "
              f"{insn.mnemonic:6} {insn.op_str}")
    print(f"[xref] readers ({len(reads)}):")
    for insn, tgt in reads:
        print(f"   R {label(tgt)} @ RVA 0x{rva_of(insn.address):06x}   "
              f"{insn.mnemonic:6} {insn.op_str}")

    if not writes:
        print("\n!! no writers found. If [validate] said NO, that's why. Otherwise the "
              "count may be written via a computed pointer (not RIP-rel) -> tell me and "
              "we switch to a runtime hardware-write breakpoint in x64dbg.")
        return

    # ---- 2) for each writer, dump the enclosing function ---------------------------
    seen_starts = set()
    for insn, tgt in writes:
        start_off = find_func_start(code, text_base, image_base, off_of(rva_of(insn.address)))
        start_rva = start_off + text_base
        if start_rva in seen_starts:
            continue
        seen_starts.add(start_rva)

        in_content_init = "  (inside fcn.1400ef210 boot content-load)" \
            if RVA_CONTENT_INIT <= start_rva < RVA_CONTENT_INIT + 0x20000 else ""
        print("\n" + "=" * 78)
        print(f"CANDIDATE RVA_TRACK_LOADER = 0x{start_rva:06x}{in_content_init}")
        print(f"  (writer of {label(tgt).strip()} at 0x{rva_of(insn.address):06x})")
        print("=" * 78)

        end_off = start_off
        limit = min(len(code), start_off + args.window)
        clears, scans = [], []
        for ins in md.disasm(code[start_off:limit], start_rva):
            # stop at the next padding gap (next function)
            o = off_of(rva_of(ins.address))
            if o > start_off and code[o - 1] == INT3 and code[o] == INT3:
                break
            end_off = o + ins.size

            note = ""
            t = rip_target(ins)
            if t in (count_abs, list_abs) and op_writes_mem(ins):
                is_zero = (ins.mnemonic == "mov" and ins.operands[-1].type == X86_OP_IMM
                           and ins.operands[-1].imm == 0)
                if is_zero:
                    note = "   <== CLEAR (zero)"
                    clears.append(ins)
                else:
                    note = "   <== WRITE"
            ct = call_target(ins, image_base)
            if ct == scan_abs:
                note = "   <== INNER SCAN call (fcn.140158be0)"
                scans.append(ins)
            elif ct is not None:
                note = f"   -> call 0x{rva_of(ct):06x}"

            print(f"  0x{rva_of(ins.address):06x}: {ins.mnemonic:7} {ins.op_str}{note}")

        print(f"\n  summary: clears={len(clears)}  inner-scan-calls={len(scans)}")
        if scans:
            print("  -> incremental add = call the inner scan on a dir with only the new "
                  ".pkz, SKIP the clear(s) above.")
        # raw bytes so the design can be re-checked against a different capstone build
        blob = code[start_off:end_off]
        print(f"\n  function bytes (RVA 0x{start_rva:06x}, {len(blob)} bytes):")
        print("  " + blob.hex())


if __name__ == "__main__":
    main()
