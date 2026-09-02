#!/usr/bin/env python3
"""Regression test: the anchors must still reproduce the baseline on a known build.

This is the whole reason to trust the tool on a build nobody has seen yet. Run it
after touching any rule; a rule that gets cleverer at the cost of the known answer is
a rule that will be wrong on the next build too.

Game binaries are not in the repo, so each check skips (loudly) when its input is
missing. Point them elsewhere with MXB_EXE / MXB_UNPACKED / GPB_EXE.
"""
from __future__ import annotations

import contextlib
import io
import os
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import rederive  # noqa: E402
import steamstub  # noqa: E402
from pe import PE  # noqa: E402

DL = Path.home() / "Downloads"
MXB = Path(os.environ.get("MXB_EXE", DL / "mxbikes.exe"))
MXB_REF = Path(os.environ.get("MXB_UNPACKED", DL / "mxbikes.exe.unpacked.exe"))
GPB = Path(os.environ.get("GPB_EXE", DL / "gpbikes.exe.unpacked.exe"))

results: list[tuple[str, str, str]] = []


def record(name, ok, detail=""):
    results.append((name, "PASS" if ok else "FAIL", detail))


def quiet(*argv) -> int:
    """Run the CLI without its report; --check still writes failures to stderr."""
    with contextlib.redirect_stdout(io.StringIO()):
        return rederive.main(list(argv))


def skip(name, why):
    results.append((name, "SKIP", why))


def test_unpack_matches_steamless():
    """Our SteamStub decrypt must equal what Steamless produced, byte for byte."""
    if not (MXB.exists() and MXB_REF.exists()):
        return skip("unpack == steamless", f"need {MXB.name} and {MXB_REF.name}")
    pe = PE(MXB.read_bytes(), str(MXB))
    ours = steamstub.unpack(pe)
    ref = MXB_REF.read_bytes()
    q = PE(ours)
    t = q.section(".text")
    a = ours[t.rawptr:t.rawptr + t.rawsize]
    b = ref[t.rawptr:t.rawptr + t.rawsize]
    record("unpack == steamless", a == b,
           f"{sum(x != y for x, y in zip(a, b))} differing bytes of {len(a)}")


def test_mxb_baseline():
    """Every baselined MX Bikes value must come back out of the anchors."""
    if not MXB.exists():
        return skip("mxb baseline", f"need {MXB}")
    code = quiet(str(MXB), "--check")
    record("mxb baseline", code == 0, "see the MISMATCH/LOST lines above")


def test_gpb_known_values():
    """The two GP Bikes offsets the header states, found by the same rules."""
    if not GPB.exists():
        return skip("gpb known values", f"need {GPB}")
    code = quiet(str(GPB), "--title", "gpb", "--check")
    record("gpb known values", code == 0, "see the MISMATCH/LOST lines above")


def test_packed_and_unpacked_agree():
    """Feeding the packed exe and the unpacked one must give the same answers."""
    if not (MXB.exists() and MXB_REF.exists()):
        return skip("packed == unpacked", "need both forms")
    out = {}
    for tag, p in (("packed", MXB), ("unpacked", MXB_REF)):
        pe, _ = rederive.load(p)
        out[tag] = {r.key: r.value for r in rederive.resolve(pe, "mxb", {})}
    diff = [k for k in out["packed"] if out["packed"][k] != out["unpacked"][k]]
    record("packed == unpacked", not diff, f"disagree on {diff}")


if __name__ == "__main__":
    for fn in (test_unpack_matches_steamless, test_mxb_baseline,
               test_gpb_known_values, test_packed_and_unpacked_agree):
        try:
            fn()
        except Exception as exc:  # a crashing rule is a failing test, not a traceback
            record(fn.__name__, False, f"{type(exc).__name__}: {exc}")
    print()
    for name, status, detail in results:
        print(f"{status:<5} {name}" + (f"   ({detail})" if status != "PASS" else ""))
    raise SystemExit(1 if any(s == "FAIL" for _, s, _ in results) else 0)
