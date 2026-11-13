# Aussie mode

Turn it on with `aussiemode=1` in `frostmod_radar.cfg`, or with **Settings → Game Integration →
Aussie mode** in MXB App. It is off by default and takes effect the next time the game starts.

## What it changes

On your screen, every other rider has a physical collision body. It is a rigid body with the mass
of bike plus rider (184.5 kg for an MX2 OEM KTM), and a spring pulls it toward where that rider
is drawn. Your bike collides with that body.

Beta 21 made that spring ten times softer horizontally. That was its "harsh multiplayer
collisions" fix.

| | Beta 20d | Beta 21e | Aussie mode |
|---|---|---|---|
| horizontal stiffness | 200 × m | 20 × m | 200 × m |
| vertical stiffness | 0.1 × 200 × m = 20 × m | 20 × m | 20 × m |
| damping | critical | critical | critical |
| force limit per axis | 1.5 × k | 1.5 × k | 1.5 × k |

Aussie mode puts the Beta 20 spring back on other riders' collision bodies, exactly. The vertical
axis was already 20 × m in both builds, so it is kept there. It patches three sites in the
stepper `0x166A10` (see `src/aussie.h`):

- the 20 × m load is pointed at a 200.0 FrostMod owns, and the pooled 20.0 is not touched;
- the vertical spring term gets one extra multiply, × 0.1, in a small code cave;
- the vertical damper term gets one extra multiply, × √0.1, in the same cave.

Nothing else is touched. The netcode is not, and neither is the 300 ms gate.

## Whose bodies it affects (measured in-engine)

A probe hooked all four world-step list steppers while the game was joined to a local server.
The game went through `mxbproxy` at 100 ms + 0–40 ms each way. One headless rider was on track.

- **With me in the garage:** the stepper ran for exactly one body. When that remote rider
  disconnected, the body's target froze at the same moment.
- **With me on track:** still one body. My own bike never went through this stepper, or through
  `0x1867D0`.
- **Conclusion:** `0x166A10` drives only other riders' collision bodies.

**For the Aussie-mode rider:** other riders feel as solid as in Beta 20 when you touch them. Your
own bike's physics are unchanged.

**For everyone else:** nothing changes. Each client resolves its own bike's contacts against the
bodies on its own screen, and the server does not simulate collisions. So other riders' games,
and how their bikes respond to you, are not affected. Aussie mode cannot make you hit anyone
harder. It only makes you take a contact as firmly as Beta 20 did.

## Measured: no contact, 200 ms

The same 2.5-minute stretch of real riding was replayed through `mxbproxy` (100+0..40 ms each
way, 1% loss). The source is Sean's MXB Coach telemetry, rotated and scaled 0.86 to fit the club
track, median 15.7 m/s.

The table is the distance from the remote rider's collision body to the pose it follows, with
the body's fixed centre-of-mass offset removed:

| | median | p90 | p99 | max |
|---|---|---|---|---|
| stock 21e | 0.003 m | 0.011 m | 0.089 m | 0.57 m |
| Aussie mode | 0.003 m | 0.012 m | 0.053 m | 0.65 m |

The game drives these bodies along with the pose, so with no contact they sit on the drawn rider
at any ping, and stiffness makes no difference. This run also confirms the patch applies and runs
in-engine (`[aussie] on`, a 4-minute session, no faults).

## Contacts: model with the game's constants

The spring only matters when something pushes the body. An in-engine contact A/B was not
possible in this rig. The headless rider passed straight through my parked bike in the pits, so
collisions are off there. Driving my own bike out needs gamepad input.

So these numbers come from a model, not the engine:
- the measured mass and the exact stepper law, `F = clamp(k·e + c·ė, ±1.5k)` at 2 ms;
- my bike at the same mass, closing on the other rider, with a rigid inelastic contact.

| closing speed | | other rider gives way | contact lasts | my peak deceleration | speed I lose in 100 ms |
|---|---|---|---|---|---|
| 3 m/s | 21e | 0.22 m | 350 ms | 6.7 m/s² | 2.1 of 3.0 m/s |
| | **Aussie** | **0.07 m** | **110 ms** | 21 m/s² | 2.9 of 3.0 m/s |
| 6 m/s | 21e | 0.43 m | 350 ms | 13 m/s² | 4.2 of 6.0 m/s |
| | **Aussie** | **0.13 m** | **110 ms** | 42 m/s² | 5.8 of 6.0 m/s |

In 21e the other rider gives way three times as far, and the contact drags on for a third of a
second. That reads as "floaty". In Beta 20, and in Aussie mode, the contact is short and firm.

## How this relates to ping

The spring behaves the same at any ping, as the no-contact table shows. Ping changes how often
you touch a rider whose drawn pose is not where they really are. The playback pipeline is
unchanged since 20d. At 150–250 ms those contacts are more frequent and less expected, and the
soft Beta 21 response is what they feel like. That matches what AU riders report.

The parked draft that changed extrapolation instead (#173's first commit) is not part of this
mode.

## Not done

- **In-engine contact A/B:** needs a real rider. That is the "Sean tries the local build" step.
- **The other 20·m site (`0x186DFC`, stepper `0x1867D0`):** never ran in any session here. Its
  objects are unknown, so it is left alone.
- **Beta 21's limb damping (0.4×) and 100 Hz limb sub-step:** visual, not ping-related, so left
  alone.
