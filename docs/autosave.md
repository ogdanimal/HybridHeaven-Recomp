# Autosave — implementation rundown

**STATUS: FUNCTIONALLY COMPLETE — DEVICE-VERIFIED 2026-08-01.** Manual combo and
2-minute timer both commit real saves on a Retroid Pocket 5, and a resulting save
**loads through the game's own load menu** (user-confirmed). Two bugs were found
by playing it on hardware and both are fixed — a settled check that refused every
save, and a wrong function signature that segfaulted the game. See "Two bugs
found on device"; neither was visible to any automated run.

Ported from Goemon64Recomp's `patches/autosave.c`, which is itself a port of
Zelda64Recomp's `patches/autosaving.c`. This document is the authoritative
rollup.

## What it does

Commits your progress through Hybrid Heaven's own save routine and save-slot
format, to whichever slot the save menu last selected. A resulting save loads
through the normal load path. No custom format, no separate slot, no bespoke
checksum.

Both a **manual trigger** (`L + R + Z`) and a **2-minute timer** commit saves.
The whole feature defaults to **Off**.

## The one structural difference from Goemon, and it is the whole design

Goemon *reimplements* its game's save routine, because that routine lives in an
overlay that is not resident during gameplay. Hybrid Heaven does not need that,
and reimplementing here would be actively wrong.

**`.file_7` IS the gameplay engine.** It spans `0x80107830`–`0x80191520` and
holds gState, the camera state machine, the player code *and* the whole
save/load layer. `patches/camera.c` already runs from inside it every frame. So
the game's own save code is callable directly, and `patches/autosave.c` calls
it:

| | |
|---|---|
| `func_80141628_520D38(device, slot)` | marshal live state into a 0xD00 image and write the slot |
| `func_80141568_520C78(device, dir_base + 4)` | write the 0x100-byte directory |

That is the body of the game's own save routine `func_80141268_520978`, minus
its final call to `func_8014307C_52278C` — 616 instructions of save-menu message
UI, the one part that must not run over live gameplay. The directory-entry
fields written between the two calls are reproduced in the original's order.

The consequence is that a resulting save is not "indistinguishable from" a real
one by argument — it **is** one, produced by the same code.

### The pak layout, for reference

The game creates a `0x3500`-byte pak file (`func_80142240_521950` passes that
size to `func_80002EF0_3AF0`). The layout is a `0x100` directory followed by
`0xD00` slots: `0x100 + 4 * 0xD00 == 0x3500` exactly, hence **four slots**. The
byte offset of slot *n* is `n * 0xD00 + 0x100`, computed identically at all five
call sites in `.file_7`.

## Why the marshal is never run speculatively

The obvious way to build the settled/changed check would be to run the marshal
`func_80141F28_521638` into a scratch buffer and hash the 0xD00 it produces —
that is exactly the payload, so it would be the perfect measure.

It is not available. **The marshal is not idempotent.** Its bulk serializer
`func_80144C40_524350` opens by folding counters back into game state:

```
8017_DC40[0x88] += 8017_DDA4[0x2]        (and the same for 0x89 / 0x8A / 0x9C)
8017_DDA4[0x0..0x7] = <reset>
```

i.e. "add the elapsed-since-last-save counters to the totals, then reset them".
Running it to *look* at the result would bank those counters without a save
behind it, and the next real save would under-count. So the settled check hashes
the marshal's **sources** instead.

The same fact makes the timed save harmless, which is worth stating because it
looks like the opposite: those counters are a running total plus a delta, so
folding the delta in more often does not change the total.

## The safe-state gate

Zelda64Recomp uses `gCanPause`; Goemon re-evaluates its game's pause-gate
conjunction. Hybrid Heaven has something better already measured — the gate
`patches/camera.c` hangs off, which is the game's own answer to "may the player
drive right now":

- `func_8011B254_4FA964` returns gState `+0x29A`, the mirror of the camera mode
  selector that `func_8010DD4C_4ED45C` dispatches on once a frame.
- `func_8011B260_4FA970` returns 0 for scene 2-8-1 and otherwise packs the scene
  triple as decimal; zero is the permissive answer.

The game requires both — scene gate zero **and** mirror exactly 2 — before it
hands the player the orbit camera. From camera.c's measurements over a 19-minute
session (34,653 samples): **mode 2 during exploration accepted every time, mode
0 during cutscenes and elevators refused every time, mode 3 for the whole of
four battles.**

So the conjunction excludes cutscenes, elevators and combat without this feature
enumerating any of them — the same trick as `gCanPause`, using a gate this port
has already validated on hardware.

The load flag `D_8008DC18_8E818` is checked on top of it, because a file load is
the one unsafe state that has nothing to do with the camera.

**What this gate does not do**, stated so it is not mistaken for cover it does
not give: it says nothing about the pause menu, and it is not a claim that every
mode-2 frame is transaction-free. The settled check covers the second. The first
is untested, and is the first thing to look at if a save ever lands somewhere it
should not.

## The settled check

Requires the save-relevant state to hold still for **167 ms** before a save may
commit, so a write never lands mid-transaction.

**Measured in time, not frames, and that is not a stylistic choice.** Goemon
counts frames because its poll is called from a patched main loop exactly once
per frame. This one is called from `camera.c`'s `func_80119F9C_4F96AC` patch,
which has 30+ call sites across the camera state handlers and can run more than
once in a frame. A frame counter here would be a call counter wearing a frame
counter's name, and would settle in a third of the time it claimed to. 167 ms is
ten frames at 60 Hz, i.e. Zelda64Recomp's window expressed in the unit this poll
can actually measure.

### What is watched

Seven ranges, every one of them read by the marshal or by the bulk serializer it
calls:

| | range | |
|---|---|---|
| A | gState `+0x02`, 0x0A | the halfwords at `+0x2/+0x4/+0x6/+0x8/+0xA` |
| B | gState `+0x0E`, 0x10 | `+0xE..+0x1D` |
| C | gState `+0x39D`, 0x01 | the lone far byte the marshal reads |
| D | `8008_DC18 +0x08`, 0x64 | copied to image `+0x300` |
| E | `801B_ED38`, 0x200 | copied verbatim |
| F | `8017_DC40`, 0x9E | the bulk serializer's first block |
| G | `8018_3CE0`, 0x204 | its 0x56 six-byte records |

Range E's base and length are not guesses: `func_8014C294_52B9A4` and
`func_8014C2A0_52B9B0` are two- and three-instruction functions returning
`&D_801BED38_59E448` and `0x200` respectively.

**A–C are the marshal's exact field list, not a convenient block around it**, and
that distinction is the whole of a shipped bug — see "Two bugs found on device".
`+0x00`, `+0x01`, `+0x0C`, `+0x0D`, `+0x1E` and `+0x1F` are excluded because the
marshal does not read them. Do not widen these back to a tidy block without
checking each added byte for volatility first.

**This is not complete, and saying so is the point.** `func_80144C40_524350`
continues past range G into several more blocks that were not traced. So the
check can miss a transaction confined entirely to an unwatched block — the
direction that lets a save land slightly early, not the one that refuses
forever. Widening it is a matter of tracing the rest of that function and adding
ranges; nothing else changes.

> An earlier version of this paragraph claimed the check "cannot produce a false
> unsettled". It could, and it did: one unread volatile byte inside the old
> contiguous range A refused every save for as long as the feature existed. The
> claim is removed rather than softened, because it is what stopped the first
> device log from being read as the smoking gun it was.

**Still open:** range A (`gs-head`) does move periodically — plausibly a
play-time counter, which Goemon excluded from its own ranges as noise. At the
42 s of stability now measured it is nowhere near blocking, but a refusal
reporting `last change: gs-head` with a small age is the next thing to bisect.

Player world position is deliberately absent — it is in no watched range — so
walking around does not prevent settling. A check that only settled while
standing still would refuse almost every save.

## The timer

Every `AUTOSAVE_INTERVAL_US` (2 minutes), when the gate passes, the state has
settled, and something has actually changed. Three behaviours, none incidental:

1. **An elapsed interval is not consumed.** If the gate or the settled check
   refuses, the timer retries on every poll instead of skipping to the next
   period. Otherwise an interval elapsing during a cutscene would silently lose
   that save.
2. **Nothing is written if nothing changed.** The settled hashes are snapshotted
   at each save and compared. Every flush rotates the runtime's `.bak`, so a
   timer that wrote unconditionally would churn it forever for no benefit.
3. **A failed save still resets the timer**, so a persistently failing save
   cannot retry on every poll.

The manual combo also feeds the timer, so a timed save cannot land moments after
the player saved by hand.

## The manual trigger: `L + R + Z`

Same three buttons as Goemon's, chosen here for a Hybrid Heaven reason rather
than by inheritance. The README's input scan established what this game reads:
**A, B, Z, Start, L, R and the four C-buttons, and no D-pad bit anywhere** — so
the D-pad combos other ports use are unavailable, not merely unfashionable. Of
what is left, every C-button drives an action, Start opens the menu, and A/B are
attack and guard. L, R and Z are the only three that can be held together
without committing the player to anything: R raises the gun
(`func_801E5010_596010` enters the aim state) but firing needs a separate press,
and the save is edge-triggered so the combo resolves in one poll.

All three are read from the **game's** button word (gState `+0x9E`), so the combo
follows whatever the player has bound. Unlike Goemon this needs no
physical-trigger special case: HH's analog camera masks C-DOWN, not R, so none of
L/R/Z is ever masked out from under the combo.

## The slot cursor, and why it is guarded

`D_801BEC00_59E310` is a six-byte struct in `.file_7` bss; `[2]` is the device
selector (1 = controller 1, 2 = controller 2) and `[5]` is the slot.

Both are validated before anything is written, and the validation is
load-bearing rather than defensive tidiness:

- The **device** selector is the harmless one. The game's own save routine simply
  falls through when it is neither 1 nor 2, returning an uninitialised stack
  byte. Refusing here turns that into a reportable status
  (`AUTOSAVE_ERR_NO_DEVICE`).
- The **slot** cursor is the dangerous one. It is owned by the save/load menu, so
  on a session that has never opened that menu it may be stale, and nothing
  downstream re-checks it: the pak offset is `slot * 0xD00 + 0x100` with no bound
  between here and the write. An in-range wrong slot silently overwrites a
  different save; an out-of-range one writes past the pak file.

> **Known consequence, not yet measured on a real session:** because the cursor
> is menu-owned, autosave may refuse with `no save device selected` until the
> player has visited the save/load menu once in that session. The load path
> reads `[5]` for controller 1 and `[1]` for controller 2 while the game's save
> routine reads `[5]` for *both*; that asymmetry is reproduced here rather than
> corrected, because saving to a slot the game would not have saved to is
> precisely the failure this feature must not have.

## The `.manual.bak` rollback point

`src/game/save_rollback.cpp`, ported from Goemon unchanged in policy because the
policy is a property of librecomp rather than of either game.

librecomp rotates `current -> .bak` on every flush, and every autosave is a
flush. That rotation used to mean, incidentally, that `.bak` held your last
*deliberate* save — purely because flushes were rare. Autosave destroys that
property, so a save-class rollback point is maintained deliberately: a guest pak
write that is **not** bracketed as the autosave's own arms a one-shot, and the
next flush copies the save file to `.manual.bak` (via a temp file and an atomic
rename).

**A Hybrid Heaven specific, and the reason the bracket wraps the whole save
body:** this game commits a save as *two* pak writes, the 0xD00 slot and then
the 0x100 directory. An unbracketed directory write would arm the one-shot and
copy autosave content into `.manual.bak` — precisely the state it exists to roll
back from.

The rollback point is maintained **unconditionally**, not gated on the autosave
setting, so it already exists at the moment autosave is switched on.

## Diagnostics

`recomp_autosave_log` formats host-side (patch code has no `printf`). Not behind
an `HH_TRACE_*` switch, unlike every other trace in the port: these fire at most
once per save or per combo press, and the one way this feature fails is by
refusing silently — which is indistinguishable from a gate working correctly
unless the refusal names the guard that rejected it.

```
[autosave] manual save -> status 0: committed (device 1, slot 0)
[autosave] refused: unsafe state (cam mode 0 want 2 | scene 0-0-0 | loading 0)
[autosave] refused: save data not settled (43/167 ms | last change: gstate bulk)
[autosave] interval elapsed, suppressed: unsafe state (cam mode 0 | last change mask 0x01)
```

The last one exists because the timed path's silence is otherwise ambiguous in
exactly the way that matters: **a timer that fires and is correctly suppressed,
and a timer that fired once and never re-armed, produce the identical empty
log.** Goemon's notes record that ambiguity costing three separate debugging
cycles on this feature, in three different places. It is rate-limited by
*reason*, not by time — the timer retries on every poll once its interval has
elapsed, so a repeated line means the reason actually changed.

## What is proven, and what is not

Verified by a 175-second `HH_AUTOSTART` run on the Linux build with
`autosave_mode` forced to `On`, which produced exactly one line:

```
[autosave] interval elapsed, suppressed: unsafe state (cam mode 0 | last change mask 0x01)
```

That single line is load-bearing in five ways at once:

| it proves | because |
|---|---|
| the poll is reached at all | nothing else calls `recomp_autosave_log` |
| the setting is read | the line only exists past the `recomp_get_autosave_enabled` early return |
| the settle hashing runs and does not fault | the mask is populated |
| the hashing is discriminating, not stuck | mask `0x01` means range A moved and B–E did not; all-set or all-clear would both be suspicious |
| the 2-minute wall-clock timer fires and re-arms | the line appears once, at the boundary, and the rate limiter holds it to one |

The control on that control: the same run with `HH_TRACE_CAMMODE=1` produced
7,368 `[cammode]` lines, all `MODE 0`, confirming the hook function runs
continuously and that mode 0 — attract/title, not gameplay — is the honest reason
for the refusal. (An earlier attempt used `HH_TRACE_CAM`, which is a different
switch and produced zero lines; a "0 hits" that came from a wrong variable name
proves nothing, and nearly read as "the hook never runs".)

### Then proven on hardware (2026-08-01)

Four saves on a Retroid Pocket 5 — `manual save -> status 0` at 21:13 and 21:16,
`timed save -> status 0` at 21:15:35 (the 2-minute timer, on hardware), each with
the `indicator shown` / `indicator hidden` pair 2.0 s apart.

The pak was pulled and diffed rather than trusted:

| | |
|---|---|
| slot 0 location | pak offset `0x100` — the layout derived from the disassembly |
| checksum after the write | stored `0xF6`, computed `0xF6` over bytes `0..0xCFE` — **valid** |
| bytes changed by one autosave | 16: two in the directory, fourteen in slot 0 |
| what changed | progress flags at slot0 `+0x1A1/+0x1AD/+0x1C2`, a four-entry structure at `+0x3D4..+0x410`, a counter at `+0x573`, and `+0x57C` mirrored into directory `0x15` |
| `.manual.bak` | still the 21:01 deliberate save, across all four autosaves |

The recomputed checksum is the load-bearing one: a stale echo or a torn write
cannot produce a valid checksum over *changed* content. The directory bytes
moving is the other — that is `func_80141568_520C78`, the call that segfaulted an
hour earlier, working with its second argument.

**A caution about how this was nearly mis-called.** The *first* successful save
produced a pak byte-identical to the previous one (0 differing bytes of 131,072),
because the player had reloaded and changed nothing material. That is
indistinguishable from a write that went nowhere. The verdict was held until a
save with real change behind it could be diffed — which is the only reason the
"it worked" above means anything.

### To verify it

1. Play to somewhere ordinary on foot (camera mode 2), having used the save menu
   at least once so the slot cursor is set.
2. Turn Autosave **On** in the settings menu.
3. Press `L + R + Z`. Expect the "Saved" toast and
   `[autosave] manual save -> status 0: committed (device N, slot M)`.
4. Reload that slot from the game's own load menu and confirm it starts where it
   should.
5. Leave it running for over two minutes on foot and confirm a
   `timed save -> status 0` line, then a
   `suppressed: nothing changed since the last save` once idle.

If step 3 reports a refusal instead, the line names the guard: `unsafe state`
with the camera mode, `not settled` with the range that last moved, or
`no save device selected` if the save menu has not set the cursor this session.


## Two bugs found on device (2026-08-01)

Both were found by playing, not by building, and both are worth recording
because neither was visible to any automated run.

### 1. The settled check refused every save

Every refusal reported `0/167 ms` -- *always exactly zero*, meaning the watched
state changed on the very poll being tested, every time.

Range A was originally gState `+0x00..+0x20`, a contiguous block chosen as a
superset of the marshal's fields on the reasoning that a superset could only be
over-cautious. It cannot be. Bisecting on the desktop build -- first 4-byte
chunks, then single bytes -- pinned it to **gState `+0x0D`**, which changes on
essentially every frame and which **the marshal never reads**. One byte of
collateral, not part of the save at all, held `settle_stable_since_us` at
`now_us` forever.

The check now hashes the marshal's exact field list. After the fix the same run
reports **settled for 42,758 ms** against a 167 ms requirement.

**The symptom was in the very first desktop run, before the feature ever reached
a device** -- `last change mask 0x01` -- and was written off as "plausible". The
mask names *which* range moved but not *whether the window elapses*, and those
are different questions: a range that moves once a second and one that moves
every poll print the identical mask. The settle age is now reported alongside
the mask for exactly that reason.

### 2. A wrong signature segfaulted the game

```
#00 func_80141BD0_5212E0+672     <- faults
#01 func_80142350_521A60+280     <- the directory write
#02 func_80141568_520C78+172     <- our call
#03 hh_save_now
```

`func_80141568_520C78` was declared as taking one argument. Its own body reads
only `a0` -- it dispatches on the device and calls `func_80142350_521A60`
without ever setting `a1` -- so in isolation it looks like a one-argument
function. It is not. **`a1` passes through untouched** to
`func_80142350_521A60`, which hands it to `func_80141BD0_5212E0` as the source
it builds the 0x100 image from. With one declared parameter, `a1` held whatever
the caller left there and the builder dereferenced it.

The game passes the directory array base **plus 4** at both of its call sites
(`D_801BEB84` for device 1, `D_801BEBAC` for device 2). That `+4` is also why
the per-slot fields sit at `+4..+0xB` of an 8-byte entry -- relative to the
pointer the game actually hands over, they are `+0..+7`.

**A function's signature is not what its own body reads, it is what its callees
read.** Recover argument shapes from call sites, and for a dispatch wrapper
check the callee too. The other two calls (`func_80141628_520D38`,
`func_80108280_4E7990`) were re-audited the same way and are correct:
the first explicitly does `andi $a1, $a1, 0xFF`, and the second writes `$a0`
before reading it.
