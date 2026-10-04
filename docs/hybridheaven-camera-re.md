# Hybrid Heaven (USA) camera reverse engineering — for the Analog Camera patch

Derived 2026-07-29 from this repo's `HybridHeaven-RecompSyms/` plus the decomp at
`~/projects/hybridheaven` (splat-split asm under `asm/usa/`, decompiled on demand with
`tools/m2c`). Every claim below is traceable to a named function in that asm; where a
statement is inference rather than a direct read, it says so.

Reproduce any of it with, e.g.:

```
cd ~/projects/hybridheaven
python3 tools/m2c/m2c.py --target mips-ido-c -f func_80119F9C_4F96AC asm/usa/file_7.s
```

## Big picture

Hybrid Heaven's renderer is fed by a per-frame list of typed draw objects ("tasks").
Object kind 1 is *the perspective camera*, and its state is a 0x5C-byte **camera
parameter block**. Everything camera-related in the game — the view matrix, the
movement basis, the HUD compass, the skybox — reads that one block. There is no
separate "camera struct" the game keeps privately: the parameter block **is** the
camera.

That single fact is what makes the analog camera cheap here. Rotate the eye in that
block and the game's own consumers follow, because they all derive their angles from
it rather than from a stored yaw.

## The camera parameter block

Reached as `(*(void**)0x801BBCD8)->0x2C`. Layout, read out of `func_80007BD0_87D0`
(the setup that copies a request into it) and `func_80007DE4_89E4` (the emitter that
turns it into a display list):

| Offset | Type | Meaning |
|---|---|---|
| +0x00..0x06 | s16 x4 | scissor ulx, uly, lrx, lry (emitted as `G_SETSCISSOR`, ×4.0) |
| +0x08..0x14 | s32 x4 | `Vp` (vscale/vtrans), copied to a viewport slot for `G_MOVEMEM` |
| +0x18 | u16 | perspNorm, also emitted via `G_MOVEWORD`/`G_MW_PERSPNORM` |
| +0x1C | f32 | **fovy** |
| +0x20 | f32 | aspect |
| +0x24 | f32 | near |
| +0x28 | f32 | far |
| +0x2C | f32 | scale |
| +0x30/34/38 | f32 | **eye x, y, z** |
| +0x3C/40/44 | f32 | **look-at x, y, z** |
| +0x48/4C/50 | f32 | **up x, y, z** |
| +0x54 | f32 | (unread by the emitter) |
| +0x58 | s32 | (unread by the emitter) |

`func_80007DE4_89E4` feeds +0x1C..0x2C to `guPerspective` and +0x30..0x50 to
`guLookAt`, in that order, then emits the matrices. It is in `.main`, so it is
resident for the whole run.

`func_8011723C_4F694C` clones the whole block field-for-field from the main camera,
which is where the 0x54/0x58 tail comes from.

## Angle convention

Angles are **13-bit binary**: `0x2000` is a full turn, and every producer masks with
`0x1FFF`. Signed angles (pitch) are folded into ±0x1000 by subtracting 0x2000 when
bit 0x1000 is set.

Trig helpers, all in `.main`:

- `f32 func_8001EAD0_1F6D0(s16 angle)` — **sin**
- `f32 func_8001EB64_1F764(s16 angle)` — **cos**
- `s16 func_8001EF38_1FB38(f32 y, f32 x)` — **atan2**, returns a 13-bit angle
- `f32 _nsqrtf(f32)` — sqrt

The spherical convention the game uses, read straight out of `func_8011913C_4F884C`:

```
forward = (cos(pitch)*cos(yaw), sin(pitch), cos(pitch)*sin(yaw))
eye     = at - radius * forward
```

so `yaw = atan2(at.z - eye.z, at.x - eye.x)` and
`pitch = atan2(at.y - eye.y, hypot(at.x - eye.x, at.z - eye.z))`.

## The two derived getters — why hooking the camera is enough

`func_80119F9C_4F96AC(s16* yaw, s16* pitch)` computes both angles from the parameter
block every time it is called. It stores nothing. Roughly 30 call sites in `.file_7`
alone use it as *the* source of truth for "where is the camera looking".

`func_8011A0F0_4F9800(s16* roll)` builds on it for the roll/up axis.

The one that matters most for feel:

```c
// func_8011A878_4F9F88 — the movement basis
p = gCamera->params;
gState->0x232 = func_8001EF38_1FB38(p->at.z - p->eye.z, p->at.x - p->eye.x);
```

`gState->0x232` is the yaw the player's stick vector is rotated by. It is recomputed
from the live eye/at **every frame**, so a patch that moves the eye gets a correct
movement basis natively — no counter-rotation hack, the same property Quest 64 has
and Goemon 64 does not.

## The per-frame camera integrator

`func_8011A3D4_4F9AE4` (`.file_7`), called once per frame. In order:

1. Removes last frame's vertical shake from `eye.y` and `at.y`.
2. Advances `eye` (+0x30/34/38) toward the goal at `gState->0x2C0/0x2CC/0x2D8`,
   at speed `gState->0x2B8` — `func_80119C20_4F9330`.
3. Advances `at` (+0x3C/40/44) toward the goal at `gState->0x2E8/0x2F4/0x300`,
   at speed `gState->0x2E0`.
4. Advances `fovy` (+0x1C) toward `gState->0x310` at speed `gState->0x308`
   — `func_80119D38_4F9448`.
5. **Calls `func_80119F9C_4F96AC`**, then rebuilds the up vector (+0x48/4C/50) from
   the yaw/pitch/roll it returns — `func_8011A148_4F9858`.
6. Applies this frame's vertical shake (`gState->0x324`).

Step 5 is the analog camera's hook point: it is the one place per frame that sits
*after* the eye and look-at are final and *before* anything consumes them.

Camera goals are set by `func_8011AAF4_4FA204(...)` — 386 call sites across sixteen
overlays, i.e. the game's general "aim the camera here over this long" API. It
early-outs when a different task owns the camera (`gState->0x2B0`).

## The camera mode machine — the game's own answer to "may the player look around"

Added 2026-07-30. This is the most important section in this document, and it was
missed for the feature's first two days: **the game has an explicit camera state
machine, and it already decides when a player is allowed to move the camera.**

`D_80163740_542E50` holds a camera **mode**, a signed byte in −1..8.
`func_8010DD4C_4ED45C` — the camera task's per-frame exec — copies it to
**`gState+0x29A`** and dispatches on it through `jtbl_80185CD8_5653E8`:

```
8010E4EC  lb   $a1, %lo(D_80163740_542E50)($a1)   mode
8010E4F4  addiu $t5, $a1, 1                       index = mode + 1
8010E4F8  sltiu $at, $t5, 0xA                     ten entries
8010E500  sb   $a1, 0x29A($v1)                    mirror into gState
8010E510  lw   $t5, %lo(jtbl_80185CD8_5653E8)($at)
8010E514  jr   $t5
```

Each jump-table stub installs that mode's handler into the camera task's exec slot
(`task->0x1C`). Mode −1 installs one directly; modes 0–8 pass a descriptor to
`func_800057DC_63DC` and the handler is the descriptor's `+0xC`. Each mode is a
**two-phase sub-machine** — an `init` handler that swaps itself for a `main`
handler once running:

| mode | descriptor | init | main (observed) |
|---|---|---|---|
| −1 | — | `func_8010E668_4EDD78` | — |
| 0 | `D_80163750_542E60` | `func_8010E680_4EDD90` | `func_8010E794_4EDEA4` |
| 1 | `D_801637D4_542EE4` | `func_8010E890_4EDFA0` | `func_8010EA78_4EE188` |
| **2** | `D_801638A0_542FB0` | `func_8010F6E0_4EEDF0` | `func_8010FABC_4EF1CC` |
| 3 | `D_80163990_5430A0` | `func_80113148_4F2858` | `func_80113AB4_4F31C4` |
| 4 | `D_80163A24_543134` | `func_801147C0_4F3ED0` | — |
| 5 | `D_80163AD0_5431E0` | `func_80115390_4F4AA0` | `func_801154A4_4F4BB4` |
| 6 | `D_80163B40_543250` | `func_80115620_4F4D30` | — |
| 7 | `D_80163BB0_5432C0` | `func_80115B50_4F5260` | — |
| 8 | `D_80163C2C_54333C` | `func_80115F10_4F5620` | — |

**The developers' own debug strings are still in the ROM**, immediately after that
jump table at `0x80185D00`: `process(cam_fix_init1)`, `process(cam_fix_main)`,
`process(cam_free_init1)`, `process(cam_free_main)`, plus EUC-JP Japanese for
"instant switch" / "smooth switch" and an entire `< CAMERA DEBUG INFO >` readout
(`SP %.2f`, `P/PR/C/CA/DO % 5d`), a free-camera collision toggle and a camera-speed
up/down display. So the game's own vocabulary for these is **fix** and **free**.

### The gate

Two predicates, both in `.file_7`:

```c
// func_8011B254_4FA964 — three instructions
s8 mode_mirror(void) { return *(s8*)(gState + 0x29A); }

// func_8011B260_4FA970 — a packed scene id, zero being the permissive answer
u32 scene_gate(void) {
    u8 a = gState[0x254], b = gState[0x255], c = gState[0x256];
    if (a == 2 && b == 8 && c == 1) return 0;
    return a * 10000 + b * 100 + c;
}
```

`func_801E5010_596010` in `.file_9` combines them before it will put the player
into a camera state:

- **C-left** (0x0002, the orbit camera): `scene_gate() == 0 && mode_mirror() == 2`
- **C-down** (0x0004, the close look): `scene_gate() == 0 || mode_mirror() == 2`

Note the conjunction versus the disjunction — it is not a typo in the asm, and it
matters: C-down's is satisfied by the scene gate alone.

### What the modes are, measured

From ~40 minutes of traced play (`HH_TRACE_CAMMODE=1`, 95k records over two
sessions). The scene triple was **`0-0-0` in every sample**, so `scene_gate()`
returned 0 throughout and the rule reduced in practice to `mirror == 2`.

| mode | what it is | fingerprint |
|---|---|---|
| **0** | cutscenes, elevators, room transitions | **the only mode that changes fovy** (30–50); least player movement (12%) |
| 1 | brief, transitional | fovy 35–55, 9 s total |
| **2** | **exploration — the only mode a player may drive** | fovy pinned 35, most movement (58%) |
| **3** | **battle** | fovy pinned 35; four clusters matched four battle-overlay loads to within 1 s |
| **5** | ladder / climbing | fovy 33–50, radius to 428; main handler is `func_801154A4_4F4BB4` |

Modes 4, 6, 7, 8 and −1 have not been observed in play yet.

Direct confirmation of the gate, from the same trace: **C-left was accepted on all
5 presses inside mode 2 and refused on all 5 inside mode 0**, and one C-down press
inside mode 0 was accepted — exactly as the disjunction predicts.

### Two things this corrects elsewhere in this document

- **`gState+0x2AE`, the goal-write "type" byte, IS the camera mode.** It is
  described under "The goal's provenance" as meaning unknown and "always 0". It
  read 2 in mode 2 (7289 of 7348 samples) and 3 in mode 3 (11809 of 11838). It was
  always 0 before because every earlier trace happened to sit in mode 0.
- **The four functions that refresh the movement basis are camera mode `main`
  handlers, not player states.** `func_8010E794_4EDEA4` is mode 0's,
  `func_801154A4_4F4BB4` is mode 5's; the other two sit beside modes 6 and 7's
  entries.

### The goal is not where the camera ends up

Also measured, and it invalidates a natural reading of the next section. Inside
mode 2, split by whether the analog camera was driving:

```
goal radius                    median 37.9
analog camera, engaged         median 37.9   <- tracks the goal exactly
the game's own camera          median 45.5   <- where it actually settles
```

The game eases its **eye position in three dimensions** toward a goal that keeps
moving with the player, so it trails and settles further out than the goal radius.
Anything that eases a **scalar radius** toward that number converges onto the goal
instead, and frames closer than the game does. The goal is still the right source
for *occlusion* (see below) — it is simply not the framing distance.

`D_801BBBB4_59B2C4`, the radius the game's own orbit camera uses, is not an
alternative: `func_80117E58_4F7568` computes it, and is called only from the two
orbit-camera *entry* functions, so it is a snapshot taken when C-left is pressed.

## The game's own camera controls — what the C-buttons actually do

The user-facing model is "the C-buttons control the camera". Mechanically it is
narrower than that: **a C-button puts the player into a camera state, and the analog
stick then drives the camera** — so you cannot move and look at the same time. That
is the limitation the analog camera removes.

The two states, both player-task exec functions in `.file_7`:

| Function | Gate | Behaviour |
|---|---|---|
| `func_8011913C_4F884C` | `heldButtons & 0x0002` (**C-left**) | orbit around the player at radius `D_801BBBB4`, pitch clamped to ±0x600 |
| `func_80118768_4F7E78` | `heldButtons & 0x0004` (**C-down**) | close look; also snaps the player's facing to `0x800 - camYaw` |

Both read their input through one handler:

```c
// func_80118038_4F7748(s16* yaw, s16* pitch)
//   camX = gState->0xA2, camY = gState->0xA4  (the analog stick, deadzone |v| >= 11)
//   whichever axis dominates wins that frame
*yaw   += 16.0f * camX / 20.0;                          // ~0.8 units/frame at full tilt
*pitch  = lerp(*pitch, base + 2048*camY/K, 0.125);      // damped, |pitch| < 0x800
```

`D_801BBB90_59B2A0` (s16) and `D_801BBB92_59B2A2` (s16) hold that yaw and pitch. They
are touched by exactly six functions, all of them part of these two states — **the
normal exploration camera does not use them**, so they are not a usable injection
point for a free-look camera.

## Controller state

`func_800021B4_2DB4` (`.main`) polls all four ports into 0x20-byte records based at
`D_80089474_8A074`:

| Offset | Meaning |
|---|---|
| +0x02 | held buttons (u16, standard N64 bit values) |
| +0x04 | newly-pressed edge |
| +0x06 | stick X (s16) |
| +0x08 | stick Y (s16) |
| +0x0A | direction word **synthesised from the stick** at thresholds +0x29/−0x28, reusing the D-pad bit values |
| +0x0C | its newly-pressed edge |

Every direction test in the game reads +0x0A/+0x0C; no site reads a hardware D-pad
bit. C-button bits are the standard ones: C-up 0x0008, C-down 0x0004, C-left 0x0002,
C-right 0x0001.

`gState` (`D_801BBBF0_59B300`, `.file_7`) keeps a copy of the primary record at
+0x9C, hence the `gState->0x9E` (held), `gState->0xA2` (stick X) and `gState->0xA4`
(stick Y) reads above.

## Global map

| Symbol | Meaning |
|---|---|
| `D_801BBCD8_59B3E8` | pointer to the camera task; `->0x2C` is the parameter block |
| `D_801BBBF0_59B300` | `gState`, the gameplay engine's master struct (`.file_7` bss) |
| `gState->0x9C` | 0x20-byte copy of the primary controller record |
| `gState->0x232` | movement-basis yaw, recomputed from the camera every frame |
| `gState->0x2B0` | task that currently owns the camera (0 = nobody) |
| `gState->0x2B8..0x310` | camera goal eye/look-at/fovy and their approach speeds |
| `gState->0xE0` | player task; `->0x2C` has pos at +0x4/8/C and facing at +0x12 |
| `gState->0xE8` | camera task — the same pointer as `D_801BBCD8` |
| `D_801BBB90/92` | C-button camera state's yaw/pitch (not the general camera) |

## The camera goal, and when it is a lie

`func_8011AAF4_4FA204` stores a camera GOAL into gState and the integrator eases the
live camera toward it:

| gState offset | |
|---|---|
| +0x2B8 | eye approach speed |
| +0x2C0 / +0x2CC / +0x2D8 | **goal eye** x / y / z (note the 12-byte stride — not a contiguous vector) |
| +0x2E0 | look-at approach speed |
| +0x2E8 / +0x2F4 / +0x300 | **goal look-at** x / y / z |
| +0x308 / +0x310 | fovy approach speed / goal |
| +0x318 / +0x31C | roll goal and its speed (−1 when unused) |
| +0x324 | this frame's vertical shake |
| +0x380 / +0x384 / +0x388 | last collision hit point |
| +0x2B0 | task that has claimed the camera, 0 for none — **non-null during ordinary play**, so it does not mark a scripted shot |
| +0x2AE | **write type** — `arg4 & 0xFF`, stored on every goal write |
| +0x2B4 | **mode lock** — while non-zero, +0x2B5 cannot be rewritten |
| +0x2B5 | **write mode** — `arg5`, stored only while +0x2B4 is clear; the setter branches on this being 2 |

### The goal's provenance — three bytes nobody was reading

Before `func_8011AAF4_4FA204` stores a goal it records where the write came from,
and this was missed entirely for a long time: every staleness question the analog
camera answered by measuring distances had a direct answer sitting in gState.

```
8011AB08  andi  $a3, $a3, 0xFF
8011AB38  sb    $a3, 0x2AE($v1)      write type, on every call
8011AB3C  bne   $t7, 1                arg5 == 1 ?
8011AB44  sb    $zero, 0x2B4($v1)     ... then UNLOCK
8011AB48  lb    $t8, 0x2B4($v1)
8011AB50  bnel  $t8, $zero            locked ?
8011AB58  sb    $t9, 0x2B5($v1)       ... else mode := arg5
8011AB64  bne   $t0, 2                mode == 2 drives the rest of the function
```

So +0x2B5 is a **latched** mode rather than a plain record: a write with
`arg5 == 1` clears the lock, and only while the lock is clear does the mode take
the incoming `arg5`. Something later in the function sets the lock again — the
traced state `mode 2, lock 1` holds steady for minutes — so **mode 2 is sticky,
and only an explicit `arg5 == 1` write releases it.** That is the shape of "a
special camera state has taken over and holds until dismissed", which is exactly
the signal the analog camera's lifecycle gating lacks; `gState->0x2B0` was tried
for that job and is useless, being non-null in ordinary play.

Traced under `HH_TRACE_CAM` by `recomp_analog_cam_provenance`. Observed so far:
type is always 0, mode takes 0, 1 and 2, and the lock is 1 exactly when mode is 2.
**What the values MEAN is still unknown, so nothing reads them for a decision
yet** — they have to be pinned against what the camera is visibly doing first, and
that needs a player.

**Two things the goal encodes that are not obvious, and one trap.**

- **It is the game's occlusion handling.** When something comes between the camera
  and the player, the game pulls the goal distance in. Measured walking through a
  doorway: **17**, against **35** for the same room clear, flicking back to 65 as it
  re-clears. Following that distance inherits the game's own occlusion solution
  without any raycast.
- **It is transient.** That 17 is a temporary cap, not the room's framing. Anything
  that adopts it as *the* distance will come out of the doorway still tight.
- **The trap: not every camera mode writes it.** `func_801154A4_4F4BB4` snaps the
  look-at straight onto the player each frame and never touches the goal fields, so
  the goal keeps whatever the last mode that did use them left there. Measured
  leftovers: **561** on a ladder and **630** in attract mode. A live goal always
  aims within 14–19 units of the player, so `|goal_look_at − player|` is a usable
  staleness test.

## Camera collision

Two wrappers around one raycast worker `func_80108868_4E7F78`, differing only in the
surface filter they install at `D_801BBAD4_59B1E4`:

- `func_8010843C_4E7B4C` — filter `func_80108664_4E7D74`, which returns 1
  unconditionally: **collides with everything**.
- `func_801084C4_4E7BD4` — filter `func_801086C4_4E7DD4`, which skips surface types
  listed in `D_80163570_542C80`. This is the one the game's camera states use.

Signature is six floats `(x0,y0,z0, x1,y1,z1)`, returning non-zero on a hit with the
hit point in gState+0x380. Convention read off the game's own call site at
`0x801104DC`: **arg1 in `$f12`, arg2 in `$f14`, args 3–6 in `a2`/`a3`/stack**. The
worker walks a collision list from `gState->0x50 → +0x8`.

The game's own use is not a line-of-sight test: it probes a short way along the view
direction and, on a hit, translates the eye by `hit − probe_end` — a "don't let the
camera get within *d* of a wall in front of it" rule.

**Called from inside `func_80119F9C_4F96AC` it returns 0 for every ray tried** — 250
units, 5000 units, and straight down into a floor. Unresolved whether that is the
hook's phase or the surface filter (the floor test used the *filtered* wrapper, so it
proves nothing). Not worth resolving unless something needs collision the goal
distance cannot provide.

## Where this leaves the patch — as of 2026-07-30

`patches/camera.c` now asks the game's question first. `acam_mode_allows_player()`
replicates the **C-left conjunction** verbatim, and when it is false the patch calls
`acam_release()` — handing back the camera *and* the right stick, so a mode the
feature has no business in behaves exactly as if it were switched off. Re-entering
mode 2 retakes the camera through the ordinary engage path, which captures the live
pose and therefore cannot jump.

The C-left rule rather than C-down's, deliberately: C-down's disjunction is
satisfied by the scene gate alone and was true throughout the traced cutscenes. The
orbit camera is the one that is actually a free camera.

**R (0x0010) closes the gate too.** It fires the gun —
`func_801E5010_596010` puts the player into `func_801EFF78_5A0F78` while it is held
— and the game frames that shot itself. Gated on the game's own held-button word
(`gState+0x9E`) rather than the host pad, so it follows whatever R is bound to.

**Framing distance** is the goal radius scaled by a measured gain: the ratio of the
game's live radius to its goal radius, sampled **only on frames where the camera is
not ours**. That restriction is what keeps it out of the feedback loop the last
section of this document forbids — while the patch is not driving, the eye is
entirely the game's.

Verified in play: **0 engaged samples outside mode 2** across 16.8 minutes covering
8.7 minutes of battle and 3.5 of cutscene; 308 R-held samples with 0 engaged;
framing moved from 0.83× the game's distance to ~1.12×.

A/B switches: `HH_CAM_NO_MODE_GATE`, `HH_CAM_NO_AIM_RELEASE`,
`HH_CAM_NO_FRAMING_GAIN`, `HH_CAM_LEGACY_RADIUS_EASE`. Trace with
`HH_TRACE_CAMMODE=1`.

**Still carried, and probably no longer needed:** the goal-staleness test, both
trust thresholds and the occlusion dwell were built to survive readings taken in
modes the patch now refuses to run in — the "561 on a ladder" leftover is mode 5,
whose main handler is the very `func_801154A4_4F4BB4` named above. They should be
measured inside mode 2 and deleted if they never fire, rather than assumed dead.

## Where this leaves the patch — the original design

`patches/camera.c` replaces `func_80119F9C_4F96AC`. It applies the accumulated
analog-camera rotation to the parameter block's eye, then computes and returns the
yaw/pitch of the result exactly as the original does. Consequences:

- the integrator's up-vector step (5) uses the rotated angles, so the view stays level;
- the emitter renders the rotated eye;
- `func_8011A878_4F9F88` derives the movement basis from the rotated eye, so walking
  matches what you see without any extra work.

The rotation is written as an **absolute override** of the azimuth rather than an
offset added each frame, which makes it idempotent — important because the getter has
~30 call sites and can run several times per frame. Accumulation uses wall-clock dt
for the same reason.

Two further things the patch has to do, neither obvious from the code it replaces:

- **Report roll 0 while engaged** (its second hook, `func_8011A0F0_4F9800`). The
  integrator back-solves roll at frame start from the stored up vector and rebuilds
  the up vector from it at frame end; rotating between those points leaves
  `sin(pitch) × Δyaw` of residue, which the game rebuilds as real roll and compounds
  every frame until the horizon is at 90°.
- **Anchor the look-at to `player.y + 15`.** Rooms with fixed camera angles aim the
  look-at somewhere that is not the player, and an orbit around that point is felt
  as the camera having stopped tracking the player. 15 is the game's own offset,
  from `func_8011913C_4F884C`.

And one it must not do: **never read the framing distance back off the live eye.**
That closes a loop — the integrator eases the eye toward a goal at another azimuth,
the patch resets the azimuth, and the sideways component of that pull returns as a
shortening until the camera is on top of the player.
