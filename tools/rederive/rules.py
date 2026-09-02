"""The rule vocabulary anchors are written in.

Every rule answers one question — "which function owns these strings", "which global
does this function multiply an index through" — against an `Image`, and returns a
`Found` with the evidence that justified it. Nothing here knows a single address;
that is the whole point, because a new build moves all of them.
"""
from __future__ import annotations

from dataclasses import dataclass, field


@dataclass
class Found:
    value: int | None
    confidence: str          # "high" | "medium" | "low" | "none"
    evidence: str
    extra: dict = field(default_factory=dict)


class Rule:
    #: keys of other anchors this rule needs resolved first
    deps: tuple[str, ...] = ()

    def run(self, img, resolved: dict[str, int | None]) -> Found:  # pragma: no cover
        raise NotImplementedError

    def __repr__(self):
        return f"{type(self).__name__}()"


def _pick(cands, what):
    cands = sorted(cands)
    if len(cands) == 1:
        return Found(cands[0], "high", what)
    if not cands:
        return Found(None, "none", f"no match for {what}")
    return Found(None, "none", f"ambiguous {what}: " + ", ".join(hex(c) for c in cands))


# ---- code anchors -----------------------------------------------------------
class Strings(Rule):
    """The one function that references every one of these C strings.

    The strongest anchor available: literals are what a build keeps stable even as it
    moves every address, and requiring an intersection makes an accidental match
    vanishingly unlikely.
    """

    def __init__(self, *texts: str, without: tuple[str, ...] = (), allow_many: bool = False):
        self.texts, self.without, self.allow_many = texts, without, allow_many

    def run(self, img, resolved):
        sets = [img.string_owners(t) for t in self.texts]
        missing = [t for t, s in zip(self.texts, sets) if not s]
        if missing:
            return Found(None, "none", "string(s) not in image: " + ", ".join(map(repr, missing)))
        common = set.intersection(*sets)
        for t in self.without:
            common -= img.string_owners(t)
        what = "owner of " + " + ".join(map(repr, self.texts))
        if self.allow_many and common:
            return Found(min(common), "medium", f"{what} (first of {len(common)})")
        return _pick(common, what)

    def __repr__(self):
        return f"Strings({', '.join(map(repr, self.texts))})"


class Aob(Rule):
    """A masked byte pattern that must match exactly once in .text."""

    def __init__(self, pattern: str, delta: int = 0):
        self.pattern, self.delta = pattern, delta

    def run(self, img, resolved):
        hits = img.pe.scan(self.pattern)
        r = _pick(hits, f"AOB {self.pattern[:32]}…")
        if r.value is not None:
            r.value += self.delta
            r.confidence = "medium"  # a pattern is bytes, not meaning
        return r


class ImportUser(Rule):
    """The function that calls an imported API, optionally narrowed by a second import."""

    def __init__(self, name: str, also: tuple[str, ...] = (), without: tuple[str, ...] = (),
                 without_strings: tuple[str, ...] = ()):
        self.name, self.also, self.without = name, also, without
        self.without_strings = without_strings

    def run(self, img, resolved):
        slot = img.pe.imports.get(self.name)
        if slot is None:
            return Found(None, "none", f"{self.name} is not imported")
        cands = set(img.ref_owners.get(slot, ()))
        for extra in self.also:
            s = img.pe.imports.get(extra)
            cands &= set(img.ref_owners.get(s, ())) if s else set()
        for extra in self.without:
            s = img.pe.imports.get(extra)
            if s:
                cands -= set(img.ref_owners.get(s, ()))
        for t in self.without_strings:
            cands -= img.string_owners(t)
        return _pick(cands, f"caller of {self.name}")


class CallerOwning(Rule):
    """A caller of `dep` that also references all of `strings` / `globals`."""

    def __init__(self, dep: str, strings: tuple[str, ...] = (), refs: tuple[str, ...] = (),
                 without: tuple[str, ...] = ()):
        self.dep, self.strings, self.refs, self.without = dep, strings, refs, without
        self.deps = (dep,) + refs

    def run(self, img, resolved):
        target = resolved.get(self.dep)
        if target is None:
            return Found(None, "none", f"{self.dep} unresolved")
        cands = {img.pe.func_at(s) for s in img.pe.callers_of(target)} - {None, target}
        for t in self.strings:
            cands &= img.string_owners(t)
        for key in self.refs:
            g = resolved.get(key)
            cands &= {f for f in cands if g in img.func_refs.get(f, ())}
        for t in self.without:
            cands -= img.string_owners(t)
        return _pick(cands, f"caller of {self.dep} owning {self.strings or self.refs}")


class MostRefsAmong(Rule):
    """Of the functions owning `text`, the one that touches `ref_key` most often.

    Used where a format string is shared by a shallow wrapper and the real worker: the
    worker is the one that actually walks the array.
    """

    def __init__(self, text: str, ref_key: str):
        self.text, self.ref_key, self.deps = text, ref_key, (ref_key,)

    def run(self, img, resolved):
        g = resolved.get(self.ref_key)
        owners = img.string_owners(self.text)
        if g is None or not owners:
            return Found(None, "none", f"{self.text!r} owners={len(owners)} {self.ref_key}={g}")
        scored = sorted(
            ((sum(1 for s in img.pe.xrefs_to(g) if img.pe.func_at(s) == f), f) for f in owners),
            reverse=True,
        )
        if len(scored) > 1 and scored[0][0] == scored[1][0]:
            return Found(None, "none", f"tie among {[hex(f) for _, f in scored]}")
        return Found(scored[0][1], "high",
                     f"owner of {self.text!r} with the most {self.ref_key} refs ({scored[0][0]})")


# ---- data anchors -----------------------------------------------------------
class Array(Rule):
    """The (heap-pointer global, entry stride) pair a set of functions indexes through.

    Emits the stride in `extra["stride"]` so a sibling anchor can pick it up: a build
    that grows the entry struct moves both, and deriving them together is the only way
    to notice.
    """

    def __init__(self, *strings: str):
        self.strings = strings

    def run(self, img, resolved):
        owners: set[int] = set()
        for t in self.strings:
            owners |= img.string_owners(t)
        if not owners:
            return Found(None, "none", f"no owner for {self.strings}")
        pair = img.array_for(owners)
        if pair is None:
            return Found(None, "none", f"no indexed array in {len(owners)} owner(s)")
        g, stride = pair
        return Found(g, "high", f"indexed by imul {stride:#x} in {len(owners)} owner(s)",
                     {"stride": stride})


class ArrayOf(Rule):
    """Like Array, but the owner is an anchor already resolved by other means."""

    def __init__(self, *func_keys: str):
        self.func_keys, self.deps = func_keys, func_keys

    def run(self, img, resolved):
        owners = {resolved.get(k) for k in self.func_keys} - {None}
        if not owners:
            return Found(None, "none", f"none of {self.func_keys} resolved")
        pair = img.array_for(owners)
        if pair is None:
            return Found(None, "none", f"no indexed array in {self.func_keys}")
        g, stride = pair
        return Found(g, "high", f"indexed by imul {stride:#x} in {self.func_keys}",
                     {"stride": stride})


class StrideOf(Rule):
    """The stride the `dep` array anchor already recovered."""

    def __init__(self, dep: str):
        self.dep, self.deps = dep, (dep,)

    def run(self, img, resolved):
        extra = resolved.get("__extra__", {}).get(self.dep, {})
        s = extra.get("stride")
        if s is None:
            return Found(None, "none", f"{self.dep} produced no stride")
        return Found(s, "high", f"imul immediate beside {self.dep}")


class CountOf(Rule):
    """The loop-bound dword that lives beside loads of the `dep` list pointer."""

    def __init__(self, dep: str):
        self.dep, self.deps = dep, (dep,)

    def run(self, img, resolved):
        g = resolved.get(self.dep)
        if g is None:
            return Found(None, "none", f"{self.dep} unresolved")
        c = img.count_for(g)
        if c is None:
            return Found(None, "none", f"no loop bound found beside {self.dep}")
        return Found(c, "medium", f"dword voted beside {self.dep} loads (fanout {img.fanout(c)})")


class GlobalNearString(Rule):
    """The .data global referenced closest to where a function handles `text`.

    Ubiquitous globals (the stack cookie, the engine command bus) are excluded by
    fanout, since they sit near everything.
    """

    def __init__(self, owner: str, text: str, max_fanout: int = 100, window: int = 0x200):
        self.owner, self.text = owner, text
        self.max_fanout, self.window = max_fanout, window
        self.deps = (owner,)

    def run(self, img, resolved):
        f = resolved.get(self.owner)
        if f is None:
            return Found(None, "none", f"{self.owner} unresolved")
        sites = [s for r in img.string_rvas(self.text) for s in img.pe.xrefs_to(r)
                 if img.pe.func_at(s) == f]
        if not sites:
            return Found(None, "none", f"{self.text!r} not referenced from {self.owner}")
        best = None
        for target, refs in img.pe.refs.items():
            sec = img.pe.section_of(target)
            if not sec or sec.name != ".data" or img.fanout(target) > self.max_fanout:
                continue
            for s in refs:
                if img.pe.func_at(s) != f:
                    continue
                d = min(abs(s - a) for a in sites)
                if d <= self.window and (best is None or d < best[0]):
                    best = (d, target)
        if best is None:
            return Found(None, "none", f"no local global within {self.window:#x} of {self.text!r}")
        return Found(best[1], "medium", f"nearest local global to {self.text!r} (+{best[0]:#x})")


class ClusterBase(Rule):
    """Lowest address of the dense .data run a function works through.

    A screen's state lives in one contiguous block; pinning the block and expressing
    each field as a delta means a rebased build only has to move one number.
    """

    def __init__(self, owner: str, gap: int = 0x40, min_members: int = 4, max_fanout: int = 200):
        self.owner, self.gap = owner, gap
        self.min_members, self.max_fanout = min_members, max_fanout
        self.deps = (owner,)

    def run(self, img, resolved):
        f = resolved.get(self.owner)
        if f is None:
            return Found(None, "none", f"{self.owner} unresolved")
        gs = [g for g in img.data_globals_of(f) if img.fanout(g) <= self.max_fanout]
        runs, cur = [], []
        for g in gs:
            if cur and g - cur[-1] > self.gap:
                runs.append(cur); cur = []
            cur.append(g)
        if cur:
            runs.append(cur)
        runs = [r for r in runs if len(r) >= self.min_members]
        if not runs:
            return Found(None, "none", "no dense global cluster")
        best = max(runs, key=len)
        return Found(best[0], "medium",
                     f"cluster of {len(best)} globals {best[0]:#x}..{best[-1]:#x}",
                     {"members": best})


class Rel(Rule):
    """`base + delta` — a field whose position inside a resolved cluster is known."""

    def __init__(self, base: str, delta: int):
        self.base, self.delta, self.deps = base, delta, (base,)

    def run(self, img, resolved):
        b = resolved.get(self.base)
        if b is None:
            return Found(None, "none", f"{self.base} unresolved")
        return Found(b + self.delta, "medium", f"{self.base} + {self.delta:#x}")


class BusPointer(Rule):
    """The engine command-bus function pointer: the .data qword called indirectly by
    more of the image than any other."""

    def run(self, img, resolved):
        from collections import Counter
        import struct
        pe = img.pe
        t = pe.section(".text")
        blob = pe.data[t.rawptr:t.rawptr + t.rawsize]
        votes: Counter = Counter()
        for i in range(len(blob) - 6):
            if blob[i] != 0xFF:
                continue
            modrm = blob[i + 1]
            if (modrm & 0xC7) != 0x05 or ((modrm >> 3) & 7) not in (2, 3):
                continue  # not a rip-relative call/jmp through memory
            disp = struct.unpack_from("<i", blob, i + 2)[0]
            target = t.rva + i + 6 + disp
            sec = pe.section_of(target)
            if sec and sec.name == ".data":
                votes[target] += 1
        if not votes:
            return Found(None, "none", "no indirect calls through .data")
        g, n = votes.most_common(1)[0]
        return Found(g, "high", f"called indirectly from {n} sites")


class Const(Rule):
    """A value the tool cannot derive: carried from the baseline and always flagged.

    Struct field offsets and protocol ids are meaning, not addresses — nothing in the
    image announces that `+0x4C0` is the cfg name. Carrying them forward with a loud
    label is honest; silently emitting them as if re-derived is not.
    """

    def __init__(self, why: str, near: str | None = None):
        self.why, self.near = why, near
        self.deps = (near,) if near else ()

    def run(self, img, resolved):
        hint = ""
        if self.near:
            v = resolved.get(self.near)
            hint = f"; start from {self.near} = {v:#x}" if v is not None else ""
        return Found(None, "low", f"not derivable: {self.why}{hint}")


class GlobalsOfExcept(Rule):
    """Pick from a function's own .data globals, ordered by address.

    `index` selects which one; `max_fanout` drops the stack cookie and the command bus,
    which every function touches and which would otherwise always come first.
    """

    def __init__(self, owner: str, index: int, max_fanout: int = 100, expect: int | None = None):
        self.owner, self.index, self.max_fanout = owner, index, max_fanout
        self.expect, self.deps = expect, (owner,)

    def run(self, img, resolved):
        f = resolved.get(self.owner)
        if f is None:
            return Found(None, "none", f"{self.owner} unresolved")
        gs = [g for g in img.data_globals_of(f) if img.fanout(g) <= self.max_fanout]
        if self.expect is not None and len(gs) != self.expect:
            return Found(None, "none",
                         f"{self.owner} has {len(gs)} local globals, expected {self.expect}")
        if not -len(gs) <= self.index < len(gs):
            return Found(None, "none", f"{self.owner} has only {len(gs)} local globals")
        return Found(gs[self.index], "medium",
                     f"local global #{self.index} of {len(gs)} in {self.owner}")


class FunctionWithGlobals(Rule):
    """The one function whose entire local-global set is exactly these resolved keys.

    Small leaf helpers have no strings and no distinctive imports; what identifies them
    is that they touch a specific pair of globals and nothing else.
    """

    def __init__(self, *keys: str, max_fanout: int = 100):
        self.keys, self.max_fanout, self.deps = keys, max_fanout, keys

    def run(self, img, resolved):
        want = {resolved.get(k) for k in self.keys}
        if None in want:
            return Found(None, "none", f"unresolved dependency in {self.keys}")
        cands = []
        for f in img.func_refs:
            gs = {g for g in img.data_globals_of(f) if img.fanout(g) <= self.max_fanout}
            if gs == want:
                cands.append(f)
        return _pick(cands, f"function whose only globals are {self.keys}")


class SharedGlobals(Rule):
    """The Nth .data global that two resolved functions both touch.

    Two functions in the same subsystem share exactly the state that subsystem owns;
    intersecting them strips the locals each one has of its own, which is what makes
    this survive a build that reshuffles either function.
    """

    def __init__(self, a: str, b: str, index: int, max_fanout: int = 100,
                 expect: int | None = None):
        self.a, self.b, self.index = a, b, index
        self.max_fanout, self.expect, self.deps = max_fanout, expect, (a, b)

    def run(self, img, resolved):
        fa, fb = resolved.get(self.a), resolved.get(self.b)
        if fa is None or fb is None:
            return Found(None, "none", f"{self.a}/{self.b} unresolved")
        shared = sorted(
            set(img.data_globals_of(fa)) & set(img.data_globals_of(fb))
            - {g for g in img.data_globals_of(fa) if img.fanout(g) > self.max_fanout}
        )
        if self.expect is not None and len(shared) != self.expect:
            return Found(None, "none",
                         f"{self.a} n {self.b} share {len(shared)} globals, expected {self.expect}")
        if not -len(shared) <= self.index < len(shared):
            return Found(None, "none", f"only {len(shared)} shared globals")
        return Found(shared[self.index], "medium",
                     f"shared global #{self.index} of {len(shared)} between {self.a} and {self.b}")


class AfterString(Rule):
    """The NUL terminator of a literal — a valid pointer to an empty string.

    The DIR-shaped content loaders are handed `""` for the game directory. Any pointer
    to a zero byte in read-only data does that job, so this anchors on a literal we can
    still find rather than on the particular empty string the old build happened to use.
    """

    def __init__(self, text: str):
        self.text = text

    def run(self, img, resolved):
        hits = img.string_rvas(self.text)
        r = _pick(hits, f"literal {self.text!r}")
        if r.value is not None:
            r.value += len(self.text)
            r.confidence = "high"
            r.evidence = f"NUL terminator of {self.text!r} (any empty string works)"
        return r


class SiteOfStringRef(Rule):
    """The code site inside `owner` that loads `text`. Hook points, not functions."""

    def __init__(self, owner: str, text: str, occurrence: int = 0, delta: int = 0):
        self.owner, self.text = owner, text
        self.occurrence, self.delta, self.deps = occurrence, delta, (owner,)

    def run(self, img, resolved):
        f = resolved.get(self.owner)
        if f is None:
            return Found(None, "none", f"{self.owner} unresolved")
        sites = sorted(s for r in img.string_rvas(self.text)
                       for s in img.pe.xrefs_to(r) if img.pe.func_at(s) == f)
        if not -len(sites) <= self.occurrence < len(sites):
            return Found(None, "none", f"{self.text!r} referenced {len(sites)}x in {self.owner}")
        return Found(sites[self.occurrence] + self.delta, "low",
                     f"{self.text!r} ref #{self.occurrence} in {self.owner}"
                     + (f" {self.delta:+#x}" if self.delta else ""))


class SiteInFunction(Rule):
    """A byte pattern inside a resolved function — the instruction a hook overwrites."""

    def __init__(self, owner: str, pattern: str, occurrence: int = 0):
        self.owner, self.pattern = owner, pattern
        self.occurrence, self.deps = occurrence, (owner,)

    def run(self, img, resolved):
        f = resolved.get(self.owner)
        if f is None:
            return Found(None, "none", f"{self.owner} unresolved")
        hits = [h for h in img.pe.scan(self.pattern) if img.pe.func_at(h) == f]
        if not -len(hits) <= self.occurrence < len(hits):
            return Found(None, "none", f"pattern hits {len(hits)}x inside {self.owner}")
        return Found(hits[self.occurrence], "low",
                     f"{self.pattern} #{self.occurrence} inside {self.owner}")

