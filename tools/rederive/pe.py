"""Minimal read-only PE64 reader: sections, RVA math, .pdata functions, imports,
strings and rip-relative xrefs. Enough to re-derive offsets without a disassembler.

Everything here is expressed in RVAs (the offsets.h currency), never file offsets.
"""
from __future__ import annotations

import re
import struct
from bisect import bisect_right
from dataclasses import dataclass


# ws2_32 exports every socket call by ordinal only, so an import-by-ordinal has no
# name in the file. These are the fixed, documented ordinals.
WS2_ORDINALS = {("ws2_32.dll", o): n for o, n in {
    1: "accept", 2: "bind", 3: "closesocket", 4: "connect", 5: "getpeername",
    6: "getsockname", 7: "getsockopt", 8: "htonl", 9: "htons", 10: "ioctlsocket",
    11: "inet_addr", 12: "inet_ntoa", 13: "listen", 14: "ntohl", 15: "ntohs",
    16: "recv", 17: "recvfrom", 18: "select", 19: "send", 20: "sendto",
    21: "setsockopt", 22: "shutdown", 23: "socket", 51: "gethostbyname",
    52: "gethostname", 111: "WSAGetLastError", 115: "WSAStartup", 116: "WSACleanup",
}.items()}


@dataclass
class Section:
    name: str
    vsize: int
    rva: int
    rawsize: int
    rawptr: int

    def holds(self, rva: int) -> bool:
        return self.rva <= rva < self.rva + max(self.vsize, self.rawsize)


class PE:
    def __init__(self, data: bytes, path: str = "<mem>"):
        self.data = data
        self.path = path
        if data[:2] != b"MZ":
            raise ValueError(f"{path}: not a PE (no MZ)")
        self.pe = struct.unpack_from("<I", data, 0x3C)[0]
        if data[self.pe:self.pe + 4] != b"PE\0\0":
            raise ValueError(f"{path}: bad PE signature")
        self.machine, self.nsec = struct.unpack_from("<HH", data, self.pe + 4)
        self.timestamp = struct.unpack_from("<I", data, self.pe + 8)[0]
        self.opt_size = struct.unpack_from("<H", data, self.pe + 20)[0]
        self.opt = self.pe + 24
        magic = struct.unpack_from("<H", data, self.opt)[0]
        if magic != 0x20B:
            raise ValueError(f"{path}: not PE32+ (magic {magic:#x})")
        self.entry_rva = struct.unpack_from("<I", data, self.opt + 16)[0]
        self.image_base = struct.unpack_from("<Q", data, self.opt + 24)[0]
        self.size_of_image = struct.unpack_from("<I", data, self.opt + 56)[0]
        self.n_datadir = struct.unpack_from("<I", data, self.opt + 108)[0]
        self.datadir_off = self.opt + 112

        so = self.opt + self.opt_size
        self.sections: list[Section] = []
        for i in range(self.nsec):
            o = so + i * 40
            name = data[o:o + 8].rstrip(b"\0").decode("latin-1")
            vsize, rva, rawsize, rawptr = struct.unpack_from("<IIII", data, o + 8)
            self.sections.append(Section(name, vsize, rva, rawsize, rawptr))
        self.section_table_off = so

        self._text = self.section(".text")
        self._pdata_cache: list[tuple[int, int, int]] | None = None
        self._starts_cache: list[int] | None = None

    # ---- lookup -------------------------------------------------------------
    def section(self, name: str) -> Section | None:
        for s in self.sections:
            if s.name == name:
                return s
        return None

    def section_of(self, rva: int) -> Section | None:
        for s in self.sections:
            if s.holds(rva):
                return s
        return None

    def datadir(self, index: int) -> tuple[int, int]:
        if index >= self.n_datadir:
            return (0, 0)
        return struct.unpack_from("<II", self.data, self.datadir_off + index * 8)

    def off(self, rva: int) -> int | None:
        """RVA -> file offset, or None if it lives only in virtual space."""
        s = self.section_of(rva)
        if s is None:
            return None
        d = rva - s.rva
        if d >= s.rawsize:
            return None
        return s.rawptr + d

    def read(self, rva: int, n: int) -> bytes:
        """Bytes at `rva`, zero-padded. Addresses past raw data are BSS, which the
        loader zero-fills, so reading zeros there is the truthful answer."""
        o = self.off(rva)
        b = b"" if o is None else self.data[o:o + n]
        return b + b"\0" * (n - len(b))

    def u8(self, rva):  return struct.unpack_from("<B", self.read(rva, 1))[0]
    def u32(self, rva): return struct.unpack_from("<I", self.read(rva, 4))[0]
    def u64(self, rva): return struct.unpack_from("<Q", self.read(rva, 8))[0]

    def cstr(self, rva: int, limit: int = 512) -> str:
        b = self.read(rva, limit)
        end = b.find(b"\0")
        return b[:end if end >= 0 else len(b)].decode("latin-1")

    # ---- .pdata: the authoritative function table ---------------------------
    @property
    def functions(self) -> list[tuple[int, int, int]]:
        """RUNTIME_FUNCTION table as (start_rva, end_rva, unwind_rva), sorted.

        x64 PEs are required to describe every non-leaf function here, so this is a
        far better function boundary source than walking prologues.
        """
        if self._pdata_cache is None:
            rva, size = self.datadir(3)  # IMAGE_DIRECTORY_ENTRY_EXCEPTION
            out = []
            if rva and size:
                blob = self.read(rva, size)
                for i in range(0, len(blob) - 11, 12):
                    s, e, u = struct.unpack_from("<III", blob, i)
                    if s and e > s:
                        out.append((s, e, u))
            out.sort()
            self._pdata_cache = out
            self._starts_cache = [f[0] for f in out]
        return self._pdata_cache

    def func_at(self, rva: int) -> int | None:
        """Start RVA of the function containing `rva` (chunked entries folded in)."""
        self.functions
        i = bisect_right(self._starts_cache, rva) - 1
        while i >= 0:
            s, e, u = self._pdata_cache[i]
            if s <= rva < e:
                # An unwind info with the CHAININFO flag points at the parent function.
                parent = self._chain_parent(u)
                return parent if parent is not None else s
            if s + 0x20000 < rva:
                break
            i -= 1
        return None

    def _chain_parent(self, unwind_rva: int, depth: int = 0) -> int | None:
        if not unwind_rva or depth > 8:
            return None
        b = self.read(unwind_rva, 4)
        if len(b) < 4:
            return None
        ver_flags, _, count, _ = b[0], b[1], b[2], b[3]
        if (ver_flags >> 3) & 0x4 == 0:  # UNW_FLAG_CHAININFO
            return None
        tail = unwind_rva + 4 + ((count + 1) & ~1) * 2
        s, e, u = struct.unpack_from("<III", self.read(tail, 12))
        return self._chain_parent(u, depth + 1) or s

    # ---- strings ------------------------------------------------------------
    def find_bytes(self, needle: bytes, sections: tuple[str, ...] = ()) -> list[int]:
        """Every RVA at which `needle` appears, optionally restricted to sections."""
        hits = []
        for s in self.sections:
            if sections and s.name not in sections:
                continue
            blob = self.data[s.rawptr:s.rawptr + s.rawsize]
            start = 0
            while True:
                i = blob.find(needle, start)
                if i < 0:
                    break
                hits.append(s.rva + i)
                start = i + 1
        return hits

    def find_cstr(self, text: str, exact: bool = True) -> list[int]:
        """RVAs of NUL-terminated ASCII `text`. `exact` also requires a clean start."""
        needle = text.encode("latin-1") + b"\0"
        hits = []
        for rva in self.find_bytes(needle):
            if exact:
                o = self.off(rva)
                if o and o > 0 and 0x20 <= self.data[o - 1] < 0x7F:
                    continue  # mid-string match
            hits.append(rva)
        return hits

    def find_wstr(self, text: str) -> list[int]:
        return self.find_bytes(text.encode("utf-16-le") + b"\0\0")

    # ---- rip-relative xrefs -------------------------------------------------
    # Every reference we care about is one of these forms; the modrm byte's low 3
    # bits are 0b101 (RIP-relative) and the 4-byte displacement follows.
    _REF_FORMS = (
        (b"\x48\x8d", 3, 7),   # lea  r64, [rip+d]
        (b"\x4c\x8d", 3, 7),   # lea  r64(ext), [rip+d]
        (b"\x48\x8b", 3, 7),   # mov  r64, [rip+d]
        (b"\x4c\x8b", 3, 7),   # mov  r64(ext), [rip+d]
        (b"\x48\x89", 3, 7),   # mov  [rip+d], r64
        (b"\x8b", 2, 6),       # mov  r32, [rip+d]
        (b"\x89", 2, 6),       # mov  [rip+d], r32
        (b"\x83", 2, 7),       # cmp/add [rip+d], imm8   (imm follows disp)
        (b"\xc7", 2, 10),      # mov  [rip+d], imm32
        (b"\xff", 2, 6),       # call/jmp/inc [rip+d]
        (b"\x0f", 3, 7),       # movzx/movss family
        (b"\xf3\x0f", 4, 8),   # movss/movsd
        (b"\x8d", 2, 6),       # lea  r32, [rip+d]
    )

    def _iter_refs(self):
        """Yield (site_rva, target_rva, kind) for every rip-relative operand in .text.

        A linear byte sweep, so it over-produces: a displacement that happens to land
        inside the image is emitted even when the bytes were really operand data. Every
        caller filters by an expected target, which is what makes the noise harmless.
        """
        t = self._text
        blob = self.data[t.rawptr:t.rawptr + t.rawsize]
        n = len(blob)
        for i in range(n - 10):
            b0 = blob[i]
            if b0 in (0x48, 0x4C):
                op = blob[i + 1]
                if op not in (0x8B, 0x8D, 0x89, 0x63, 0x3B, 0x39, 0x83, 0xC7):
                    continue
                modrm_i = i + 2
                head = 3
            elif b0 in (0x8B, 0x89, 0x8D, 0x3B, 0x39, 0x38, 0x84, 0xFF, 0xC7, 0x83, 0x80, 0x81):
                # A REX prefix in front means this byte is the opcode of the 3-byte form
                # the branch above already emitted; counting it again doubles every ref.
                if i and 0x40 <= blob[i - 1] <= 0x4F:
                    continue
                modrm_i = i + 1
                head = 2
            else:
                continue
            modrm = blob[modrm_i]
            if (modrm & 0xC7) != 0x05:
                continue
            disp = struct.unpack_from("<i", blob, modrm_i + 1)[0]
            tail = 0
            if blob[modrm_i - 1] in (0xC7, 0x81):
                tail = 4
            elif blob[modrm_i - 1] in (0x83, 0x80):
                tail = 1
            nxt = modrm_i + 5 + tail
            target = t.rva + nxt + disp
            if 0 < target < self.size_of_image:
                yield (t.rva + i, target, blob[modrm_i - 1])

    @property
    def refs(self) -> dict[int, list[int]]:
        """target RVA -> [site RVAs]. Built once; ~1s for a 3MB .text."""
        if not hasattr(self, "_refs"):
            m: dict[int, list[int]] = {}
            for site, target, _ in self._iter_refs():
                m.setdefault(target, []).append(site)
            self._refs = m
        return self._refs

    def xrefs_to(self, rva: int) -> list[int]:
        return sorted(self.refs.get(rva, []))

    # ---- direct calls -------------------------------------------------------
    @property
    def calls(self) -> dict[int, list[int]]:
        """callee RVA -> [call site RVAs] for `E8 rel32`."""
        if not hasattr(self, "_calls"):
            t = self._text
            blob = self.data[t.rawptr:t.rawptr + t.rawsize]
            m: dict[int, list[int]] = {}
            for i in range(len(blob) - 5):
                if blob[i] != 0xE8:
                    continue
                rel = struct.unpack_from("<i", blob, i + 1)[0]
                tgt = t.rva + i + 5 + rel
                if t.rva <= tgt < t.rva + t.vsize:
                    m.setdefault(tgt, []).append(t.rva + i)
            self._calls = m
        return self._calls

    def callers_of(self, rva: int) -> list[int]:
        return sorted(self.calls.get(rva, []))

    def callees_of(self, start: int, end: int | None = None) -> list[int]:
        """Direct callees of the function starting at `start`, in address order."""
        if end is None:
            end = next((e for s, e, _ in self.functions if s == start), start + 0x400)
        t = self._text
        blob = self.data[t.rawptr:t.rawptr + t.rawsize]
        out = []
        for rva in range(start, end - 4):
            i = rva - t.rva
            if i < 0 or i + 5 > len(blob) or blob[i] != 0xE8:
                continue
            rel = struct.unpack_from("<i", blob, i + 1)[0]
            tgt = rva + 5 + rel
            if t.rva <= tgt < t.rva + t.vsize:
                out.append((rva, tgt))
        return out

    # ---- imports ------------------------------------------------------------
    @property
    def imports(self) -> dict[str, int]:
        """`dll!name` and bare `name` -> IAT slot RVA."""
        if not hasattr(self, "_imports"):
            out: dict[str, int] = {}
            rva, size = self.datadir(1)
            i = 0
            while rva and True:
                d = self.read(rva + i * 20, 20)
                if len(d) < 20:
                    break
                oft, _, _, name_rva, first = struct.unpack("<IIIII", d)
                if not (oft or first):
                    break
                dll = self.cstr(name_rva, 64).lower()
                thunk = oft or first
                j = 0
                while True:
                    e = self.u64(thunk + j * 8)
                    if not e:
                        break
                    slot = first + j * 8
                    if e >> 63:
                        nm = WS2_ORDINALS.get((dll, e & 0xFFFF))
                        out[f"{dll}#{e & 0xFFFF}"] = slot
                    else:
                        nm = self.cstr(e + 2, 128)
                    if nm:
                        out[f"{dll}!{nm}"] = slot
                        out.setdefault(nm, slot)
                    j += 1
                i += 1
            self._imports = out
        return self._imports

    # ---- AOB ----------------------------------------------------------------
    def scan(self, pattern: str, section: str = ".text") -> list[int]:
        """IDA-style pattern: '40 53 ?? 48 8B 05 ? ? ? ?'. Returns match RVAs."""
        toks = pattern.split()
        rx = b""
        for t in toks:
            rx += b"." if t in ("?", "??") else re.escape(bytes([int(t, 16)]))
        s = self.section(section)
        blob = self.data[s.rawptr:s.rawptr + s.rawsize]
        return [s.rva + m.start() for m in re.finditer(rx, blob, re.DOTALL)]
