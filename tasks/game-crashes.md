# MX Bikes crashes to desktop — what we know, what is fixed, what is next

## The reports

Three symptoms, from players, all of them the game closing to desktop (not the rider
going down — that is the `[hardcore] overjump_crash` setting, which is a different
subject and lives in `tasks/overjump-crash.md`):

1. landing an overjump, or hitting an object
2. clicking **go to track**
3. worse on servers with a lot of people

and one that is not a crash: two riders rendered inside each other, riding along
together ("double bike").

These predate both MXB App and FrostMod. Nothing here assumes we caused them; what
FrostMod can do is be in the process when it happens and say where.

## The corpus (this is the problem)

Two crashes, both from one player's bundle (`mxb-app-logs-mxb534e`, FrostMod v0.17.0):

| where | what |
|---|---|
| `mxbikes.exe+0x11D753` | access violation reading `0x10` — **understood, guarded, see below** |
| `MSVCR90.dll+0x36EDE` | access violation reading `0x000000831B11285C` |

That is all of it. Neither line could say whether the player was on track or in a
menu, and the log around them is 23 MB of server-browser chatter. The second one is a
read of what looks like a stack address inside the CRT — the shape a buffer overrun
walking off a stack has, and the game reads every content file through MSVCR90 stdio —
but with no stack there is nothing to do with that guess.

So v0.28.0 is mostly about the corpus: every crash from here leaves a stack, a session
context, a trail of what happened, and a minidump (`src/crashreport.h`).

## Fixed: the GHS double close (`+0x11D753`)

Derived in `src/offsets.h` under "the GHS handle pool". Short version: a ten-slot pool
of GHS file handles at `0x140E4B380`; the close at `0x11D730` reads `[slot]` and then
`[slot+0x10]` without checking the slot, so closing an already-closed handle is a null
deref. Its own sibling at `0x11D790`, reached from the same bus dispatch, does
`test rsi,rsi; je out` before the identical read — so this is an oversight, not a
design, and the guard FrostMod installs returns the `1` the function already returns
for a handle naming no slot.

Signature-verified before it writes, and the slot table is decoded from the function's
own `lea` rather than from a data RVA (a data RVA does not move by the `.text` delta).
Off, loudly, if either check fails.

**What it does not claim:** there is one sample, and it was not on track. It may have
nothing to do with the three symptoms above. It is fixed because it is a real crash
with a safe fix, not because it is *the* crash.

## Fixed: the terrain query that is not a position (`+0x1F1923`)

Reported from outside, not from our own corpus: about a fifth of all crashes, the one that
takes a race with it, hit "when crashing into a fence".

`0x1F1923` is a corner read in a bilinear sample of the track height grid
(`0x1F1720`). The function is careful - it null-checks the grid, and it checks the query
position against the map's origin and size on both axes. Every one of those checks is
`comiss` + `ja`, and an unordered compare does not take `ja`, so **a NaN passes all four**.
`cvttss2si` then makes it INT_MIN and the index walks off the grid. The upper edge is
clamped; the low side never is, because the float checks were supposed to have covered it.

So the fault is not the heightmap. It is a position that stopped being a number upstream,
and this is where it gets used. The guard returns the 0 the function already returns for a
grid that is not loaded and a position off the map.

Which also explains the other half of the report - a black screen with the game still
running. Same NaN, landing in a transform instead of in this function.

**Unverified:** that the NaN comes from the collision, and the one-in-five figure. Both are
somebody else's numbers. Every catch logs its coordinates, so the first logs back settle it.

## Next

- [ ] **Collect.** Every report from here carries a stack. The `+0x11D753` work took one
      address; ten stacks would take the other crashes the same way.
- [x] **Ship them somewhere.** v0.29.0 writes `frostmod-crash-<stamp>.json` beside the log:
      the same report as JSON, for MXB App to post to the diagnostics endpoint. A file
      rather than a socket, because at that point the game has seconds and a crash that
      happened while the app was closed still has to arrive. The app sends it and renames
      it; the dump stays local until someone asks for it.
- [ ] **`MSVCR90.dll+0x36EDE`.** Needs a stack before it needs a theory.
- [ ] **"go to track".** No sample yet. The report's trail will say what the last thing
      before it was.
- [ ] **Double bike.** Not a crash, and not netcode we can fix — but `RaceAddEntry` +
      `RaceTrackPosition` give us every rider's identity and live position, so FrostMod
      can *detect* two riders holding the same position and say so. That is evidence
      worth having before anyone asks PiBoSo.
- [ ] **Collect the plugin copy's log.** `frostmod_session.log` sits in the game's
      `plugins` folder, which MXB App's log bundle does not sweep. The injected copy's
      report is the one players send today; the plugin copy's has to be asked for by hand.
- [ ] **Quieten the log.** 23 MB of `[srv]`/`[srv.hex]` per bundle buries everything
      above and makes the upload worse. Rate-limit or drop the hex dump by default.
