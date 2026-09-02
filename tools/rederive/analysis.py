"""Function-scoped indices over a PE: what each function references, which functions
own a string, and the array/stride pairs the code multiplies through.

Built once per image and reused by every anchor rule. `.pdata` chunking is folded in
via PE.func_at, so a function split across several RUNTIME_FUNCTION entries still
reports as one owner.
"""
from __future__ import annotations

import struct
from collections import Counter, defaultdict
from functools import cached_property


class Image:
    def __init__(self, pe):
        self.pe = pe

    # ---- ownership ----------------------------------------------------------
    @cached_property
    def func_refs(self) -> dict[int, set[int]]:
        """function start RVA -> set of RVAs it references (data or code)."""
        out: dict[int, set[int]] = defaultdict(set)
        for target, sites in self.pe.refs.items():
            for s in sites:
                f = self.pe.func_at(s)
                if f is not None:
                    out[f].add(target)
        return dict(out)

    @cached_property
    def ref_owners(self) -> dict[int, set[int]]:
        """referenced RVA -> set of function starts that reference it."""
        out: dict[int, set[int]] = defaultdict(set)
        for f, targets in self.func_refs.items():
            for t in targets:
                out[t].add(f)
        return dict(out)

    def fanout(self, rva: int) -> int:
        """How many distinct functions touch this address. High = ubiquitous helper."""
        return len(self.ref_owners.get(rva, ()))

    # ---- strings ------------------------------------------------------------
    def string_rvas(self, text: str) -> list[int]:
        return self.pe.find_cstr(text)

    def string_owners(self, text: str) -> set[int]:
        """Functions that reference the exact C string `text`."""
        owners: set[int] = set()
        for rva in self.string_rvas(text):
            owners |= self.ref_owners.get(rva, set())
        return owners

    def strings_of(self, func: int, minlen: int = 3) -> list[tuple[int, str]]:
        """(rva, text) for every printable C string the function references."""
        out = []
        for t in sorted(self.func_refs.get(func, ())):
            sec = self.pe.section_of(t)
            if not sec or sec.name not in (".rdata", ".data"):
                continue
            s = self.pe.cstr(t, 128)
            if len(s) >= minlen and all(0x20 <= ord(c) < 0x7F for c in s):
                out.append((t, s))
        return out

    def data_globals_of(self, func: int, sections=(".data",)) -> list[int]:
        return [
            t for t in sorted(self.func_refs.get(func, ()))
            if (sec := self.pe.section_of(t)) and sec.name in sections
        ]

    # ---- array/stride discovery --------------------------------------------
    @cached_property
    def stride_pairs(self) -> Counter:
        """Counter[(array_global_rva, stride)] over `mov r64,[rip+g]` … `imul r,r,imm`.

        Content lists in these games are a heap pointer in .data indexed by
        `base + i*sizeof(entry)`, and the compiler emits the multiply as a literal
        imul. Counting how often each (pointer, immediate) pair appears within a few
        instructions recovers both the list global and its stride with no prior
        knowledge of either — which is the point, since a new build can move the
        global *and* grow the struct.
        """
        pe = self.pe
        t = pe.section(".text")
        blob = pe.data[t.rawptr:t.rawptr + t.rawsize]
        pairs: Counter = Counter()
        for i in range(len(blob) - 24):
            if blob[i] not in (0x48, 0x4C) or blob[i + 1] != 0x8B or (blob[i + 2] & 0xC7) != 0x05:
                continue
            disp = struct.unpack_from("<i", blob, i + 3)[0]
            target = t.rva + i + 7 + disp
            if not (0 < target < pe.size_of_image):
                continue
            imm = self._imul_imm_after(blob, i + 7, window=20)
            if imm is not None and 0x40 <= imm < 0x100000:
                pairs[(target, imm)] += 1
        return pairs

    @staticmethod
    def _imul_imm_after(blob: bytes, start: int, window: int) -> int | None:
        for j in range(start, min(start + window, len(blob) - 8)):
            if blob[j] == 0x69 and (blob[j + 1] & 0xC0) == 0xC0:
                return struct.unpack_from("<i", blob, j + 2)[0]
            if blob[j] in (0x48, 0x4C) and blob[j + 1] == 0x69 and (blob[j + 2] & 0xC0) == 0xC0:
                return struct.unpack_from("<i", blob, j + 3)[0]
        return None

    def array_for(self, owner_funcs: set[int], top: int = 60) -> tuple[int, int] | None:
        """The best (list global, stride) pair for a set of functions that handle it.

        Ranked by how many of the owners touch the global first, then by how often the
        image multiplies through it. Ranking by the global count alone picks whichever
        list is busiest image-wide, which is the wrong answer whenever the owner set
        also happens to touch it in passing.
        """
        best, best_key = None, None
        for (global_rva, stride), hits in self.stride_pairs.most_common(top):
            owners = sum(1 for f in owner_funcs if global_rva in self.func_refs.get(f, ()))
            if not owners:
                continue
            key = (owners / max(len(owner_funcs), 1), hits)
            if best_key is None or key > best_key:
                best, best_key = (global_rva, stride), key
        return best

    # ---- counts -------------------------------------------------------------
    def count_for(self, list_rva: int, window: int = 0x60, max_fanout: int = 200) -> int | None:
        """The loop-bound dword that sits next to loads of `list_rva`.

        Excludes globals the whole image touches (the engine command bus is referenced
        from everywhere and would otherwise win every vote).
        """
        pe = self.pe
        votes: Counter = Counter()
        for site in pe.xrefs_to(list_rva):
            lo, hi = site - window, site + window
            for target, sites in pe.refs.items():
                if target == list_rva:
                    continue
                sec = pe.section_of(target)
                if not sec or sec.name != ".data":
                    continue
                if any(lo <= s < hi for s in sites):
                    votes[target] += 1
        for target, _ in votes.most_common(16):
            if self.fanout(target) <= max_fanout and pe.u32(target) == 0:
                return target
        return None
