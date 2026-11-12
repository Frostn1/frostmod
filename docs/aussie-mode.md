# Aussie mode

`aussiemode=1` in `frostmod_radar.cfg`, or **Settings → Game Integration → Aussie mode** in MXB
App. Off by default. It takes effect the next time the game starts.

## What it changes

At high ping, the game guesses where every other rider is now. It works from their newest
snapshot and adds velocity × dt plus ½ × acceleration × dt². The acceleration is a finite
difference of 30 ms snapshots, so it is noisy. At 150–250 ms the dt² term blows that noise
up into metres of overshoot, then the next real snapshot snaps the rider back. That is the
warping players describe.

Aussie mode sets the factor in front of that term to 0, so other riders are guessed forward at
their last speed. It is one instruction in the remote-rider extrapolation (`src/aussie.h`),
repointed at a value FrostMod owns, the same mechanism as the antifreeze patch.

| | For the Aussie-mode rider | For everyone else |
|---|---|---|
| How other riders are drawn | Far fewer warps, and closer to where they really are on average | unchanged |
| What they collide with | The drawn pose, which is now closer to the real one | unchanged |
| Own bike physics, own state on the wire, how hard they hit anyone | unchanged | unchanged |

When someone brakes hard, Aussie mode draws them running on at their last speed for one
snapshot gap, instead of overshooting by the squared term. On average that is the smaller
error (table below).

## How it was measured

- **Truth.** 36 minutes of real riding: Sean's MXB Coach telemetry, 50 Hz, 7 sessions.
- **Sender.** One snapshot every 30 ms, quantised.
- **Wire.** Proxy semantics: fixed delay plus uniform 0..J jitter per datagram, and loss.
  Server forwarding is 0–30 ms.
- **Receiver.** The game's playback rules as read from the 21e binary:
  - 8-deep ring, sorted insert, stale drop
  - 0.3 s gap gate (2.0 s with antifreeze)
  - ±800 ms window
  - p + v·dt + k·a·dt² beyond the newest sample
  - evaluated once per 30 ms net step, which is when the game runs it
- **Clock.** Client clock = server time.
- **Metrics:**
  - position error = distance from drawn to true position, every step
  - warp = a 30 ms step where the drawn motion differs from the true motion by more than 0.5 m
  - step jump p99 = 99th percentile of that per-step difference

Then the same rules were run on **real wire arrivals**:
- A local `mxbserver` on the stock club track.
- One `mxbclient --ride` replaying 6 minutes of the real trajectory.
- One `mxbclient --watch` joined through `mxbproxy` at 100 ms + 0–40 ms jitter each way and 1%
  loss (≈ 200 ms RTT). The measured sender-to-watcher age was a median of 155 ms and a p95 of
  171 ms.

### 200 ms, real wire (mxbproxy, 5.8 min)

| | mean error | p99 error | warps/min | step jump p99 |
|---|---|---|---|---|
| stock 21e | 0.28 m | 1.12 m | 333 | 1.40 m |
| **Aussie mode** | **0.22 m** | **0.76 m** | **18** | **0.48 m** |
| 100 ms interpolation delay | 1.58 m | 2.51 m | 8 | 0.37 m |
| 150 ms interpolation delay | 2.36 m | 3.76 m | 0.7 | 0.21 m |

### Replay of all 36 minutes

| network | | mean error | p99 error | warps/min | step jump p99 |
|---|---|---|---|---|---|
| LAN (10+10 ms) | stock | 0.05 m | 0.18 m | 2.0 | 0.23 m |
| | Aussie | 0.04 m | 0.17 m | 0.7 | 0.15 m |
| 200 ms (25 up, 100+0..40 down, 1% loss) | stock | 0.37 m | 1.30 m | 651 | 1.60 m |
| | Aussie | 0.25 m | 0.85 m | 22 | 0.51 m |
| 250 ms, heavy jitter (125+0..80, 2% loss) | stock | 0.54 m | 2.01 m | 913 | 2.36 m |
| | Aussie | 0.35 m | 1.18 m | 47 | 0.66 m |
| both riders far (100+0..40 each side) | stock | 0.83 m | 2.92 m | 1298 | 3.51 m |
| | Aussie | 0.49 m | 1.55 m | 74 | 0.80 m |

A quarter factor (0.25) was also tried. It landed between stock and 0 on every row, so 0 is
what ships.

## What was tried and not shipped

- **Interpolation delay.** Drawing everyone 60–150 ms in the past removes warps, but at race
  speed it puts each rider 1–2.5 m behind where they really are (tables above). You would hit
  riders who have already moved on.
- **Beta 20 rider-body stiffness.** Beta 21 softened the rider-to-bike spring from 200·m to
  20·m. Putting it back cannot be limited to contacts with other riders: it changes the
  player's own bike handling everywhere, which other riders do not get. Not shipped, for
  fairness.
- **Wider gap gate.** Already in FrostMod as antifreeze (on by default, 2 s). Neither run
  above lost a rider to the gate.

## Limits

- The receiver is a model of the game's playback, not the game itself. The finite-difference
  details of `dead_reckon` were approximated from the disassembly.
- The client clock was taken to equal server time.
- The live feel check is a real race at high ping, done by a rider.
