# Overjump crash — probe, then patch

## Why

beta21e's changelog adds `new: overjump crash option`, and it does not stick: players
uncheck it, join, and the crash is back ("when you get into the server it turns it back on
itself"). Same story on servers that set `overjump_crash = 0`.

The ini side did **not** change: beta21d parses `[hardcore] overjump_crash` with
byte-identical code — same section, same default (`1` = crash on), same inverted store into
the same slot. So the value the client ends up simulating with is what we have to look at,
not the key.

## What is already pinned (static, from the beta21e exe)

All addresses are RVAs into `mxbikes.exe` beta21e (base `0x140000000`), and they are in
`src/offsets.h` with the derivation:

- `0x02546B` — dedicated-server config loader reads `[hardcore] overjump_crash` (bus cmd
  `0x27` = read int, default `1`).
- `0x02549D` — stores `value == 0` into the session-settings block: the flag is **inverted**.
- `0x026FCC` — hands the block to bus cmd `0x310`, `arg13 = &settings`, `arg14 = 0x21C`.
- `0x0921B7`, `0x111F5E` — the client / single-player dispatches of the same command, same
  `(ptr, 0x21C)` pair.

So: `settings[+0x190] != 0` means the overjump crash is **disabled**.

## What this branch adds

- [x] `offsets.h`: the block above, plus `CMD_SESSION_START`, `CMD_CFG_READ_INT`,
      `SESSION_CFG_SIZE`, `OFF_OVERJUMP_DISABLED`.
- [x] `frostmod.cpp`: `hkBus` — a swap of the command-bus fn-ptr (`RVA_CMD_BUS_PTR`), not a
      code patch, so one aligned qword covers all 140+ dispatch sites. On cmd `0x310` it logs
      the settings pointer, the size, the decoded flag and a hex dump of the block; on cmd
      `0x27` it logs the `overjump_crash` value the ini gave (dedicated-server runs).
- [x] `launcher.cpp`: `--probe-overjump`, and `--force-overjump-off` which implies it.
- [x] `USAGE.md`: both flags.
- [x] Windows run — done, and it went badly. See "What the first run actually found".

## What the first run actually found (v0.16.1, 2026-09-01)

The probe shipped in v0.16.1 **armed by default**, and this checklist's Windows run was
still unticked when it did. The first player logs back answered a question nobody asked:

- 22 launches, 22 `[overjump] session start #0` lines, 22 crashes. Every one an access
  violation reading `0x0` at `mxbikes.exe+0x297b20`, 0–1s after the probe's own line. Not
  one session survived.
- The same machine, same game build, on v0.9.8 (no probe): nine runs across four hours,
  content reloads, menus, server browsing — zero faults.
- The minidump puts `hkBus` on the crashing stack twice over (`frostmod.dll+0x170b7` and
  `+0x170c6`), and the `settings=` pointer the probe logs is the same object the faulting
  code is walking when it reads a null member.

So the swap is not free, and "the bus only reads what its own command needs" was the wrong
assumption to ship on. v0.16.2 puts the probe back behind `--probe-overjump`.

If it is ever armed by default again, `hkBus` has to stop re-marshalling: a naked thunk that
tests `cmd` in `ecx` and `jmp`s straight to `g_origBus` for anything but `0x310` keeps the
caller's stack, the integer registers, `xmm0-3` and every stack arg exactly as they were,
which is the only safe way to sit in front of a function whose signature we inferred. The
`0x27` branch needs a structural check too — `offsets.h` warns the id itself is build-specific
(21d numbers it `0x25`), and unlike the `0x310` branch it validates nothing before acting.

## The run that settles it

`frostmod.exe --probe-overjump`, then three sessions, sending `frostmod.log` after each:

1. **Testing, single player.** Expect one `[overjump] session start` line. `ON` here with the
   menu option unchecked means the client never applies its own toggle.
2. **Host with the option unchecked.** Compare the flag against what the menu was set to.
3. **Join a ded server with `overjump_crash = 0`.** Run that server with the probe too — its
   `[overjump] ini read [hardcore] overjump_crash = 0` line proves the server side read it —
   then check what the joining client's `0x310` line says.

Reading the results:

- Client says `ON` where the server/menu said off → the client is overriding the setting.
  The fix is one dword in `hkBus`, which is what `--force-overjump-off` already does; then
  the question is only where we gate it.
- Client says `OFF` and riders still crash → the flag is not what drives the crash in 21e.
  Next step then is a read watchpoint on `settings+0x190` (or an AOB hunt) to find the
  physics reader, and the patch moves there.
- `size is not 0x21C` in the log → the block moved; re-derive the offset before anything else.

## Gating (decide before this ships to anyone)

`--force-overjump-off` is offline/testing only. Forcing the flag on a server that wants the
crash is a client-side cheat, and MXB App cannot be the thing that ships one. If the probe
shows the client is overriding a server that asked for `0`, the defensible shape is
"make the client honour the session setting" — hold the server's value, not our own.
