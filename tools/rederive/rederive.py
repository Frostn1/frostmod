#!/usr/bin/env python3
"""Re-derive MX Bikes offsets from a new game build.

    ./rederive.py /path/to/mxbikes.exe                 # report against the baseline
    ./rederive.py mxbikes.exe --json new.json          # machine-readable
    ./rederive.py mxbikes.exe --header                 # an offsets.h block to paste
    ./rederive.py mxbikes.exe --check baselines/mxb-beta21e.json   # regression test
    ./rederive.py mxbikes.exe --unpack-to out.exe      # just decrypt the SteamStub

Takes the shipping (SteamStub-packed) exe or an already-unpacked one; it detects which.

The point is not that every offset comes out automatically — some are struct fields
that no amount of analysis can name. The point is that nothing is ever emitted as if
it were re-derived when it wasn't: each row says how it was found and how far it moved.
"""
from __future__ import annotations

import argparse
import json
import sys
from dataclasses import asdict, dataclass
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import steamstub  # noqa: E402
from analysis import Image  # noqa: E402
from anchors import TITLES  # noqa: E402
from pe import PE  # noqa: E402

HERE = Path(__file__).resolve().parent


@dataclass
class Row:
    key: str
    what: str
    group: str
    consumer: str
    kind: str
    value: int | None
    confidence: str
    evidence: str
    baseline: int | None = None
    status: str = ""       # same | moved | carried | UNRESOLVED

    @property
    def display(self) -> str:
        return "-" if self.value is None else f"{self.value:#x}"


def load(path: Path) -> tuple[PE, dict]:
    """Read an exe, unpacking it if it is still SteamStub-wrapped."""
    pe = PE(path.read_bytes(), str(path))
    meta = {"path": str(path), "timestamp": pe.timestamp, "image_base": pe.image_base}
    try:
        h = steamstub.read_header(pe)
        meta["steamstub"] = steamstub.describe(h)
        meta["steam_app_id"] = h.steam_app_id
        pe = PE(steamstub.unpack(pe), str(path))
    except steamstub.NotPacked as why:
        meta["steamstub"] = f"already unpacked ({why})"
    meta["entry"] = pe.entry_rva
    meta["functions"] = len(pe.functions)
    return pe, meta


def resolve(pe: PE, title: str, baseline: dict) -> list[Row]:
    img = Image(pe)
    table = TITLES[title]
    by_key = {a.key: a for a in table}

    order, seen = [], set()

    def visit(key, stack=()):
        if key in seen or key not in by_key:
            return
        if key in stack:
            raise SystemExit(f"anchor dependency cycle at {key}")
        for d in by_key[key].rule.deps:
            visit(d, stack + (key,))
        seen.add(key)
        order.append(key)

    for a in table:
        visit(a.key)

    resolved: dict[str, int | None] = {"__extra__": {}}
    rows: list[Row] = []
    base_vals = baseline.get("values", {})

    for key in order:
        a = by_key[key]
        found = a.rule.run(img, resolved)
        resolved[key] = found.value
        resolved["__extra__"][key] = found.extra

        b = base_vals.get(key)
        b = int(b, 0) if isinstance(b, str) else b
        row = Row(key, a.what, a.group, a.consumer, a.kind,
                  found.value, found.confidence, found.evidence, b)

        if found.value is None:
            row.status = "carried" if b is not None and found.confidence == "low" else "UNRESOLVED"
            if row.status == "carried":
                row.value = b
        elif b is None:
            row.status = "new"
        elif found.value == b:
            row.status = "same"
        else:
            row.status = "moved"
        rows.append(row)

    return rows


STATUS_MARK = {"same": "  ", "moved": "->", "new": "+ ", "carried": "~ ", "UNRESOLVED": "!!"}


def report(rows: list[Row], meta: dict) -> str:
    out = [f"image      {meta['path']}",
           f"build      TimeDateStamp {meta['timestamp']:#010x}",
           f"packing    {meta['steamstub']}",
           f"entry      {meta['entry']:#x}   ({meta['functions']} functions in .pdata)",
           ""]
    tally = {}
    for r in rows:
        tally[r.status] = tally.get(r.status, 0) + 1
    order = list(dict.fromkeys(r.group for r in rows))
    rows = sorted(rows, key=lambda r: order.index(r.group))
    group = None
    for r in rows:
        if r.group != group:
            group = r.group
            out.append(f"[{group}]")
        moved = ""
        if r.status == "moved":
            moved = f"  (was {r.baseline:#x})"
        elif r.status == "carried":
            moved = "  (baseline value kept)"
        out.append(f" {STATUS_MARK[r.status]} {r.key:<26} {r.display:>12}"
                   f"  {r.confidence:<6}{moved}")
        if r.status in ("UNRESOLVED", "moved", "new"):
            out.append(f"        {r.evidence}")
    out += ["", "  ".join(f"{k}={v}" for k, v in sorted(tally.items()))]
    unresolved = [r.key for r in rows if r.status == "UNRESOLVED"]
    if unresolved:
        out += ["", "needs a look by hand: " + ", ".join(unresolved)]
    return "\n".join(out)


def emit_header(rows: list[Row]) -> str:
    out = ["// Re-derived by tools/rederive. Values marked CARRIED were not re-derived.",
           "// Anything missing here failed to resolve and is still whatever offsets.h says."]
    group = None
    for r in rows:
        if r.value is None:
            continue
        if r.group != group:
            group = r.group
            out.append(f"\n// ---- {group} " + "-" * max(0, 60 - len(group)))
        ty = "int" if r.kind == "value" else "uintptr_t"
        note = "  // CARRIED, not re-derived" if r.status == "carried" else f"  // {r.what}"
        out.append(f"constexpr {ty} {r.key:<24} = {r.value:#x};{note}")
    return "\n".join(out)


def emit_rust(rows: list[Row], meta: dict) -> str:
    """The two constants and the build stamp mxb-app's gameproc.rs holds."""
    by = {r.key: r for r in rows if r.value is not None}
    out = ["// src-tauri/src/gameproc.rs — re-derived by tools/rederive."]
    for key, name in (("APP_LOADER_OFFSET", "LOADER_OFFSET"),
                      ("APP_GUID_OFFSET", "GUID_OFFSET")):
        r = by.get(key)
        if r:
            out.append(f"const {name}: usize = 0x{r.value:09_x};   // {r.confidence}")
    out.append(f"const KNOWN_GOOD_BUILDS: &[u32] = &[{meta['timestamp']:#010x}];")
    return "\n".join(out)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("exe", type=Path, help="mxbikes.exe, packed or unpacked")
    ap.add_argument("--title", default="mxb", choices=sorted(TITLES))
    ap.add_argument("--baseline", type=Path, default=None,
                    help="known-good values to diff against (default: baselines/<title>.json)")
    ap.add_argument("--json", type=Path, help="write the full result here")
    ap.add_argument("--header", action="store_true", help="print an offsets.h block")
    ap.add_argument("--rust", action="store_true", help="print mxb-app's constants")
    ap.add_argument("--unpack-to", type=Path, help="write the decrypted image and stop")
    ap.add_argument("--check", action="store_true",
                    help="exit non-zero unless every baselined value is reproduced")
    args = ap.parse_args(argv)

    if args.unpack_to:
        pe = PE(args.exe.read_bytes(), str(args.exe))
        try:
            h = steamstub.read_header(pe)
        except steamstub.NotPacked as why:
            raise SystemExit(f"{args.exe}: nothing to unpack ({why})")
        args.unpack_to.write_bytes(steamstub.unpack(pe))
        print(f"{steamstub.describe(h)}\nwrote {args.unpack_to}")
        return 0

    baseline_path = args.baseline or HERE / "baselines" / f"{args.title}.json"
    baseline = json.loads(baseline_path.read_text()) if baseline_path.exists() else {}

    pe, meta = load(args.exe)
    rows = resolve(pe, args.title, baseline)

    if args.header:
        print(emit_header(rows))
    elif args.rust:
        print(emit_rust(rows, meta))
    else:
        print(report(rows, meta))

    if args.json:
        args.json.write_text(json.dumps({
            "meta": meta,
            "values": {r.key: r.value for r in rows if r.value is not None},
            "rows": [asdict(r) for r in rows],
        }, indent=2) + "\n")

    if args.check:
        bad = [r for r in rows if r.baseline is not None and r.value != r.baseline]
        missing = [r for r in rows if r.status == "UNRESOLVED" and r.baseline is not None]
        for r in bad:
            print(f"MISMATCH {r.key}: got {r.display}, baseline {r.baseline:#x} — {r.evidence}",
                  file=sys.stderr)
        for r in missing:
            print(f"LOST {r.key}: {r.evidence}", file=sys.stderr)
        return 1 if bad or missing else 0
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
