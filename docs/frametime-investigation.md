# The periodic frametime hitch — investigation handover

**Status (2026-07-28, gameplay round): BOTH BUGS ARE FIXED, measured in gameplay
on the Windows build and confirmed by ear and eye by the user ("played and
sounded beautiful"). The frametime hitch was the guest skipping renders because
external messages were delivered late; `HH_OSGETTIME_YIELD_US=100` removes it
entirely (843 periodic spikes -> 0 over an 8.5 minute session). The audio
oscillation was fixed earlier by `HH_AUDIO_INLINE_RSP=1`, which the same switch
has since made redundant.**

**`HH_OSGETTIME_YIELD_US=1000` is now the DEFAULT** — the one deliberate default
change this investigation produced, made after the yield-only path was judged
good by ear in gameplay. `HH_OSGETTIME_YIELD_US=0` disables it and restores the
pre-fix behaviour exactly (verified on one binary: 2 signature spikes by default,
316 with `=0`). Every other switch still ships off.

**Decided: this will NOT be offered upstream.** It stays a fork patch. Anyone who
wants it can fork. Do not reopen this as a task — the technical case for
upstreaming is real and is written up below, and it was considered and declined.

> ### Read in this order — the sections below are NEWEST FIRST
>
> This document was written for review and then revised as measurements landed,
> with each round prepended. **Later rounds retract earlier ones.** Do not quote
> a figure from a lower section without checking whether a higher one retracts
> it.
>
> | round | what it established |
> |---|---|
> | **gameplay** | bug 2 FIXED: late external delivery -> guest render skip; 843 spikes -> 0; the governor is a symptom, not the cause |
> | **fresh eyes** | two separate bugs; audio fixed by inline RSP; hitch = guest render skip; step-3/4/5 mechanism corrected |
> | **step 5** | guest send failures matched the deficit and were **not** the cause |
> | **step 4** | the port delivers 59.9 retraces/s and loses **nothing** |
> | **step 3** | the retrace→audio chain; VI is driven correctly at 60 Hz |
> | **step 2** | the task rate is not audio-controlled; kills the "FIFO-sized report" fix |
> | **after review** | the report-cap line of work is retired — the failure never occurs |
> | **review** | the compensation theory and the 4031 count retracted |
> | §1–§8 | the original write-up; **§4/§6 figures are all n=1 and none is an effect** |

### 2026-07-28, gameplay: bug 2 is fixed — the render skip was late delivery

**Measured on the Windows build (RTX 5080) in real gameplay, ~8.5 minutes,
against the previous 11-minute gameplay run as control. Judged by the user by
eye and ear: "the game played and sounded beautiful."**

#### What bug 2 actually was

The guest's frame handler skips its render when a scheduler field says the
previous frame's graphics task is still in flight:

```
0x80001A14  flag = MEM_BU(D_8008D0E5)
0x80001A24  if (flag != 1) goto render
0x80001A2C  v0 = func_80000EC8_1AC8(&D_8005C050)   // one insn: return sc->field_0x89C
0x80001A34  if (v0 != 0)   goto skip               // no render, no framebuffer swap
```

`sc+0x89C` is written by **`func_80000BF0_17F0`, the gfx dispatcher** (verified
by finding every writer in the recompiled output, not by assuming an SDK struct
layout — this scheduler is custom and does NOT use the stock field order).

The chain: ultramodern delivers external messages only at guest scheduling
points. The main thread's frame governor busy-waits on `osGetTime` for most of
every frame **without making a single blocking call**, and it is the LOWEST
priority guest thread, so nothing is delivered and nothing else runs. A retrace
delayed up to 33 ms delays the gfx dispatcher, the previous frame's task is
still in flight when the game polls, and the game drops the render.

**The governor was the symptom, not the cause.** Earlier rounds chased it as a
timing bug; `base` is resampled at the top of every frame (0x80001938) so the
deadline is relative and cannot drift. Retuning it (`HH_FRAME_GOVERNOR_US`) was
correctly a null result. Disabling it (`HH_FRAME_GOVERNOR_OFF`) "worked" only by
removing the wait that exposed the delivery latency.

#### The fix: `HH_OSGETTIME_YIELD_US=<us>` — **now the DEFAULT at 1000**

*(This section was written while it was still opt-in. It became the default later
the same day, after the yield-only path was judged good by ear. `=0` disables.)*

Turns `osGetTime` into a throttled scheduling point: deliver pending externals,
then `check_running_queue`. **Both halves are required** — delivering alone does
not help, because ultramodern emulates N64 priority scheduling and a
higher-priority thread made runnable does not run until the current thread
yields to it.

Throttled on the counter value `osGetTime` already computed (no extra clock read
on a path taken ~19M times/second); guarded on `is_game_thread()`; hooked in
`osGetTime_recomp` rather than `osGetTime` because delivery needs RDRAM.

| | control | `HH_OSGETTIME_YIELD_US=100` |
|---|---|---|
| **67 ms periodic spikes** | **843** (over 660 s) | **0** (over 511 s) |
| audio tasks/s | 60.00 | 60.01 |
| `consumed` | 1.000 | 1.000 |
| ext delivery | 7.93 ms mean / 33.45 ms worst | **0.016 ms mean / 1.61 ms worst** |
| dropped frames / empty-queue windows | 0 / 0 | 0 / 0 |

Linux title-path A/B, same build, n=1 per arm: **364 spikes -> 6**, gfx
**24.82 -> 31.73 frames/s**, delivery 6.150 ms -> 0.025 ms mean.

**The control that rules out a false positive is audio tasks/s staying at 60.**
That is tied to the game's logic rate, so a `HH_FRAME_GOVERNOR_OFF`-style
speedup would read ~120. Game speed is unchanged; the game simply stops
discarding renders and moves toward its 30 fps target.

#### Upstream comparison — asked for explicitly, and it paid off

`ultramodern` is shared by every N64Recomp port, and `hybridheaven-port`
descends from `goemon-android` with **instrumentation-only** divergence. The
`sp_complete()`-early / `dp_complete()`-after-the-whole-DL-walk shape is
upstream's, and upstream's own comment at `events.cpp` anticipates this class of
problem:

> // Games usually preserve the RSP inputs until the RDP is finished as well, so
> // sending this early shouldn't be an issue in most cases.
> // If this causes issues then the logic can be replaced with responding to
> // yield requests.

Most games **block** on DP-done and lose nothing. Hybrid Heaven **polls** and
converts a late completion into a dropped frame. Same shape as the audio
finding: not a broken port, a game unusually sensitive to a shared shortcut.

#### Open

- ~~100 us is a first guess, not tuned.~~ **Swept. Recommended value: 1000 us.**
  Linux title path, inline RSP off in every arm so the throttle is the only
  variable. Shape first (n=1, 50/100/250/500/1000/2000/5000 us), then n=3 on the
  candidates:

  | arm | signature spikes (n=3) | yields/s | deliv mean | tasks/s | consumed |
  |---|---|---|---|---|---|
  | off | 352 / 323 / 333 | 0 | 8.29-8.38 ms | 53.5-54.2 | 0.908-0.914 |
  | 100 us | 2 / 2 / 2 | 8657-8711 | 0.040-0.045 ms | 60.02 | 0.968-0.972 |
  | 1000 us | 3 / 2 / 3 | 886-889 | 0.296-0.301 ms | 60.01-60.02 | 0.966-0.970 |

  **The whole 50 us - 5 ms range is equally effective** (2-3 spikes v 352 off)
  while cost varies **93x**, so the value is a cost decision, not a correctness
  one — a good property, and worth knowing before anyone "tunes" it chasing a
  regression. 1000 us is ~10x cheaper than 100 us for identical outcomes, and its
  0.30 ms delivery latency is still ~56x under the 16.7 ms VI period that
  actually constrains it. Run-to-run spread is <1% on yields and <5% on latency,
  so unlike the rest of this document these three arms **are** effects.

  **Do not quote a yields/s figure sampled during boot.** The main thread barely
  spins there: an early-window read gave ~60/s against a full-run 9621/s on the
  same Windows gameplay run — the boot-window trap (method note #6) recurring for
  the third time in this investigation, on a different statistic each time.
- ~~Whether `HH_AUDIO_INLINE_RSP` is now redundant.~~ **Answered: it is.** Full
  2x2 on the Linux title path, 60 s per arm, n=1 each:

  | arm | 67 ms spikes | audio tasks/s | bimodal windows (<50/s, >70/s) |
  |---|---|---|---|
  | neither | 135 | mean **51.57**, min 29.6, max 141.1 | **56, 25** |
  | inline RSP only | 364 | 60.00 (57.9-62.1) | 0, 0 |
  | **yield only** | **6** | **60.02** (59.4-61.9) | **0, 0** |
  | both | 6 | 60.02 (59.3-62.0) | 0, 0 |

  The baseline reproduces the audio bug, so the comparison discriminates.
  `HH_OSGETTIME_YIELD_US` **alone fixes both bugs** — one switch addressing the
  root cause instead of two bespoke ones, and a far better upstream candidate.
  **Before dropping `HH_AUDIO_INLINE_RSP`, the user must judge a yield-only run
  by ear** — every audio verdict that mattered here came from the ear, not from
  `consumed`, which is blind to what a fix ADDS.

  Do not read anything into inline-only showing more spikes than neither (364 vs
  135): with audio bimodal the game's frame pacing differs, so those two arms do
  not measure the same thing.
- Multi-second no-render periods (audio perfect at 60/s, `gfx 0.0/s`) appear at
  heavy overlay-load points. The user reports no visible freeze, so these read
  as loading screens — but the one at t=137.78 s had **0 overlay loads**, so it
  is the one to re-examine if anything resembling a freeze is ever reported.

### 2026-07-28, fresh eyes: two bugs, one fixed, the architecture corrected

Everything below in this section is **measured on the Linux build in the
autostart title/attract scene** unless stated. New instrumentation and switches
are listed at the end.

#### The guest architecture, corrected

Earlier rounds modeled "the scheduler" as one thread that holds the RSP through
the DL walk. That is wrong. `func_80000774_1374` (thread 0x13, pri 130) is a
pure event fan-out: retrace 0x29A → notify every client with `sc+0`. The RSP is
run by two OTHER threads created in `func_80000460_1060`:

- **audio dispatcher** `func_80000A5C_165C` — thread 0x12, **pri 120**, reads
  audio tasks from `sc+8`, yields a running gfx task (`osSpTaskYield`), starts
  the audio task, waits SP-done on **`sc+0xE8`**, hands the RSP back.
- **gfx dispatcher** `func_80000BF0_17F0` — thread 0x11, **pri 100**, reads
  `sc+0x40`, waits for the target framebuffer to leave scanout (as a transient
  retrace client on `sc+0x158`), starts the task, waits SP-done on the **same
  `sc+0xE8`**, then DP-done on `sc+0x120`.
- swap thread `func_80000DC8_19C8` — 0x10, pri 110, on `sc+0x78`.
- The **audio thread** (`func_8001FBA8_207A8`) is **pri 12**; the **main game
  thread** (`func_800011B0_1DB0`, id 5) is **pri 10**.

Consequences: `dp_complete()` gates only the gfx dispatcher's frame completion —
it never blocks retrace fan-out, so step 3's "decouple dp_complete" fix
direction is moot. The port ignores yields (`librecomp/sp.cpp`), so when the
audio dispatcher takes the yield path it consumes the gfx task's SP-done off the
shared typeless `sc+0xE8` queue and re-posts a substitute — measured benign
(yields ~0–8/s, nowhere near the task rate).

#### Bug 1 — the audio trough/burst: external-delivery quantization. FIXED.

ultramodern delivers external messages (retrace, SP-done, DP-done) **only when a
running guest thread reaches a scheduling point** (`osSendMesg`/`osRecvMesg`) or
when every guest thread is blocked. Measured with the new delivery-latency
counter: **mean 8–16 ms, max ~33 ms in every gameplay window** — events sit for
up to a whole main-loop frame. The audio chain pays that latency twice per frame
(retrace in, SP-done back), and the audio thread (pri 12) plus dispatcher spend
the gap blocked. That is the 29.7/s trough; the burst is the backlog draining
whenever delivery flows. This also retires step 5's open question: the ~4–5
frames/s "lost inside the dispatch path" were never lost, they were *deferred* —
the task rate is delivery-limited, not message-limited.

**Fix, measured: `HH_AUDIO_INLINE_RSP=1`** (ultramodern `events.cpp`,
`submit_rsp_task`) runs audio RSP tasks synchronously on the submitting guest
thread. The SP-done is already in the external queue when the dispatcher's own
`osRecvMesg` drains it — the round trip through the main thread's schedule
vanishes. Result: task rate **60.0/s in every 500 ms window** (was 29.7↔86
bimodal), `consumed` 0.97–0.99 steady, SDL queue parked stable instead of
swinging 0↔27 000. Needs: gameplay verification by ear, then consider default.

#### Bug 2 — the frametime spike is NOT audio, and it is not the port's timing

With audio fully fixed the spike is unchanged: **67–70 ms, every 1.47–1.50 s,
metronomic**, ~100 % idle. Forensics chain, each step measured:

1. `HH_TRACE_MESG_BLOCK` (new): during the spike every other guest thread is
   blocked and the **main thread makes no blocking OS call for the whole 67 ms**
   — external delivery freezes with it (which is what made bug 1 worse).
2. perf on the main guest thread: the time goes to `osGetTime` +
   `__ull_div/__ll_mul` + `func_80001454_2054` — the **title-path frame
   governor**, which busy-spins on `osGetTime` until `frame-step`
   (`D_8017AA90`, normally 2) frames of 16666.666 µs (`D_8004B908`) have
   elapsed (`.L80001A88_2688`).
3. SPIKE lines now print the osGetTime-call delta and frame-step: **~1.25 M
   calls per spike, frame-step 2**. A normal frame's spin is ~0.6 M calls, so a
   spike is **two full spins with no display list between** — the game runs its
   per-frame handler twice and *skips one render* every ~44–45 frames.
4. **Null result, kept:** `HH_FRAME_GOVERNOR_US=16000` (retune the governor 4 %
   short so the retrace queue paces the loop) did NOT remove the spike — same
   cadence, same double-spin. So the spike is not a governor-vs-VI beat; the
   skip is the game's own frame accounting (the 60-in-64 counter block at
   `2454`–`2534` is the prime unread suspect).
5. `HH_FRAME_GOVERNOR_OFF=1` (poke frame-step 2→0, diagnostic only) removes the
   spike completely and the game submits every retrace — **60 fps, p50 16.7 ms,
   zero spikes** — proving the governor/skip is the entire spike. Almost
   certainly also doubles title game-speed; do not ship.

**Open questions, in order:** (a) whether real GAMEPLAY (handler
`func_80001BB0_27B0`, which has **no** osGetTime spin) shows the same skip —
autostart only ever exercised the title path, so one traced gameplay run with
`HH_TRACE_FRAME=1 HH_TRACE_FRAME_MS=50` + `HH_AUDIO_INLINE_RSP=1` decides
whether bug 2 even exists where the user plays; (b) if it does, read the
counter block at `2454`–`2534` and the demo scheduler behind
`func_80133AA0_5131B0` to find the skip's trigger; (c) whether the title skip
reproduces on real hardware (it may be a game quirk the port merely makes
audible through bug 1 — in which case inline RSP alone is the user-visible fix).

#### New instrumentation and switches (all kept; all off/gated *as of this round* — `HH_OSGETTIME_YIELD_US` became the default later the same day)

| thing | where | what |
|---|---|---|
| `HH_AUDIO_INLINE_RSP=1` | ultramodern `events.cpp` | run audio RSP tasks inline; the measured audio fix |
| `HH_FRAME_GOVERNOR_OFF=1` | `main.cpp` + `send_dl` | diagnostic: frame-step 2→0; 60 fps, do not ship |
| `HH_FRAME_GOVERNOR_US=<µs>` | `main.cpp` | retune `D_8004B908`; measured null for the spike |
| `HH_TRACE_MESG_BLOCK=1` | ultramodern `mesgqueue.cpp` | timestamped block/wake log per guest thread/queue |
| ext delivery latency | `mesgqueue.cpp` → `[audio]` report | enqueue→delivery µs mean/max per window |
| sp-task yields | `sp.cpp`/`events.cpp` → `[audio]` report | audio dispatcher yield-path rate |
| osGetTime count + frame-step | `timer.cpp` → SPIKE lines | spin-vs-work discriminator per spike |

Thread-id legend for the `[mq]` log: 5 = main, 3 = audio thread, 0x13/19 =
fan-out, 0x12/18 = audio dispatcher, 0x11/17 = gfx dispatcher, 0x10/16 = swap.

Written 2026-07-28 for adversarial review. **Revised after that review** — three
reviewers (code verification, methodology, N64/libultra domain) went through this
document and the source. Their corrections are folded in below and marked
**[RETRACTED]** or **[REVISED]**. Attack the refutations in §5 first.

### 2026-07-28, step 5: a matching number that was NOT the cause

Counting guest-side `osSendMesg` failures (ultramodern implements the guest's
`osSendMesg`, so this is measurable from the port) gave **5.02/s** against an
audio deficit of **59.9 − 55.6 = 4.3/s**. The books appeared to close.

**They did not. The match was a coincidence, and this is the fourth time this
investigation has been misled by a number that fit.** Logging *which* queue was
rejecting settled it: every single failure is queue **`0x8005C288`, full at
64/64**, message `0x8005C4B0` — which is the scheduler base, i.e. `sc+0`, the
**retrace** message (the scheduler notifies clients with `sc+0`, `sc+2`, `sc+4`,
whose leading s16s are the types 1 / 3 / 0x20 the audio thread compares against).

But `0x8005C288` is created and consumed by `func_800011B0_1DB0`, which registers
it with `osScAddClient` and then blocks on `osRecvMesg` on it — **that is the
main game thread's client queue, not the audio thread's.** And
`func_80000A0C_160C` keeps walking the client list after a failed `NOBLOCK` send,
so a rejection there costs the audio client nothing.

It is also probably not a defect: a 30 fps main loop consuming one retrace per
frame from a 60 Hz feed must overflow a bounded queue eventually. **The audio
client's queue (`0x80091DA0`) never appears in the failure log at all.**

**So the audio loss is still unaccounted for**, and it is now cornered precisely:
the audio thread receives ~60 notifications/s and runs ~55 audio frames/s, with
nothing dropped anywhere between. The remaining suspect is inside the audio
thread's own dispatch — the `.L8001FC70_20870` path in `func_8001FBA8_207A8`
has work between receiving a type-1 message and calling `func_8001FD14_20914`,
and that is where ~4/s go. **That is the next thing to read, and it is guest
code, not port code.**

### 2026-07-28, step 4: the port's side is exonerated — the loss is inside the guest

Two counters added to `ultramodern` (branch `hybridheaven-port`), both because
earlier claims cited evidence that could not support them:

- **`debug_external_message_drops()`** — the real drop counter. The reviewers
  were right that `debug_external_message_count()` cannot see a drop: a message
  is popped by `try_dequeue` *before* `do_send` is attempted, so the queue is as
  empty after a drop as after a delivery.
- **`debug_retrace_messages_sent()`** — retrace *messages*, not VI *iterations*.
  The retrace branch is gated by `retrace_count`, so those are different numbers
  and my step-3 VI counter was answering a near-miss question.

**Measured in gameplay, Windows build:**

| | rate |
|---|---|
| retrace messages sent to the guest | **59.8 /s** (so `retrace_count == 1`) |
| external messages dropped by the port | **0, cumulative, whole run** |
| audio tasks reaching the guest's audio thread | **55.3 /s** |
| ratio | **0.925** |

That ratio matches the measured `consumed` of ~0.92. **The audio shortfall IS the
retrace-to-audio-frame conversion loss**, and the port's message layer is
exonerated: it delivers 60 retraces a second and loses nothing.

So ~7.5% of retraces never become audio frames, entirely inside the guest, even
though the scheduler notifies clients unconditionally on `0x29A`. The remaining
loss point is the guest's own `osSendMesg(client->msgQ, msg, NOBLOCK)` in
`func_80000A0C_160C` — which discards when the audio thread's 64-deep queue is
full, i.e. when the audio thread is not draining. Which returns to the same
suspect as step 3: the audio thread cannot run its frame while the scheduler is
holding the RSP for a graphics task, and in this port the scheduler holds it for
the whole display-list walk because `dp_complete()` is sent only at the end.

**This is the handoff point.** Confirming it needs guest-side instrumentation
(audio-queue depth at notify time, and RSP occupancy), which is a fresh cycle.

### 2026-07-28, step 3: the full causal chain, and the port drives VI correctly

The guest chain, read end to end from the ROM:

```
VI retrace -> osViSetEvent posts 0x29A          (main.s, func_80000460_1060; ONE call site)
           -> scheduler thread func_80000774_1374 matches 0x29A
           -> func_80000A0C_160C walks sc->clientList (sc+0x888),
              osSendMesg(client->msgQ, msg=1, NOBLOCK)
           -> audio thread func_8001FBA8_207A8 wakes on msg 1
           -> ONE audio frame (func_8001FD14_20914)
```

`func_80000934_1534` is `osScAddClient` (links the client at `sc+0x888` under an
interrupt mask), and the audio thread registers itself with it. **So the audio
task rate IS the retrace notification rate**, and the game's `target = 736
= 44100/60` is its statement that it expects 60 of them a second.

**Measured, on the Windows build in gameplay:**

| | rate |
|---|---|
| VI iterations driven by the port | **59.9 – 60.1 /s, rock steady** |
| gfx tasks (the game renders at 30 fps — normal) | 29.7 /s |
| audio tasks reaching the guest's audio thread | **~55 /s mean, swinging 45 – 84 /s** |

**The port drives the retrace correctly at 60 Hz. The loss is entirely inside the
guest**, between the VI message and the audio thread — ~8% of audio frames never
happen, which matches the ~0.92 `consumed` almost exactly, and the swing is the
burst/trough cycle.

**The mechanism is already written down in this repo**, in
`src/main/rt64_render_context.cpp` above `send_dl`:

> *"the game's scheduler waits on `dp_complete()`, which the gfx thread sends
> only after RT64 has rendered, so the queue backs up while the retrace-driven
> state machine runs on."*

The scheduler thread is the same thread that must service retraces and notify
the audio client. While it is blocked on `dp_complete()`, retrace messages back
up in its 8-deep queue; it then drains them in a burst, and anything past 8
pending is dropped by the `NOBLOCK`/no-requeue path. **That is the burst, the
trough, and the ~8% shortfall, from one cause.**

It also explains the single most confusing fact in this whole investigation —
that llvmpipe and an RTX 5080 behave identically. The stall is not the *duration*
of the display-list walk (2–17% of wall on both); it is that `dp_complete` is
sent only *after* it, so the scheduler is structurally a full DL-walk behind
every frame no matter how fast that walk is.

**Fix direction (untested):** `sp_complete()` is already sent early, deliberately
— `events.cpp:451-455` explains why. `dp_complete()` is not. Decoupling it so the
scheduler is not blocked for the whole DL walk is the obvious A/B, and it is in
the port's **graphics** path, not the audio path where this investigation spent
its time. Treat as a hypothesis: it needs a switch and a measured run.

### 2026-07-28, step 2: the task rate is NOT audio-controlled, and that kills step 3

Static read of the guest's audio thread, no new instrumentation:

- **Only two `osAiGetLength` call sites exist in the whole ROM**, both in
  `main_20420.s`: one in `func_8001FD14_20914` (sizes the buffer) and one in
  `func_8001FEBC_20ABC` (a tiny underrun check — reads the length, tests
  `queued == 0`, touches a flag). That is exactly the measured **2.00 reads per
  task**, fully accounted for.
- **Both are called from `func_8001FBA8_207A8`**, which is the audio thread's
  main loop: a *blocking* `osRecvMesg` on `D_80091DA0_929A0` (`a2 = 1`),
  dispatching on the message value — type 1 is the audio-frame path. Stock
  `audiomgr.c` shape. **There is no polling loop and no
  `while (osAiGetLength() < threshold)` catch-up anywhere.**

**Therefore the audio backlog does not control the task rate.** One message in,
one audio frame out. The task rate is set entirely by how many type-1 messages
the game's scheduler posts, which is upstream of the audio code and upstream of
anything this port reports.

**This rules out review step 3 as a fix.** Reporting a FIFO-sized backlog would
raise the trough's size from the 720 floor to at most 992 — real, but
`29.8 × 992 = 29 562 samples/s against 44 100 needed is still 33% short`. No
value returned from `get_frames_remaining()` can close a gap that is set by the
task rate. It is worth doing as a correctness matter; it is not the fix.

**And it names the bug precisely.** The game's own `target` is **736 = 44100/60**
— its design point is **60 audio tasks per second at 736 samples each**, which is
the device rate exactly. Hardware drove those from the retrace at 60 Hz,
independent of a 30 fps render. In this port gameplay delivers **~30/s**, so the
game is structurally starved of audio frames and the bursts are it catching up
when the scheduler runs long. **The defect is in how often the guest's audio
message is posted, not in what the audio path is told.**

### 2026-07-28, after the review: the cap line of work is RETIRED by measurement

Following review step 1 (log ground truth per task), `queue_samples` now records
the size the game actually synthesised — `sample_count / 2` **is** the value
`func_8001FD14_20914` computed, so this is ground truth with no model in the way.
Over **209 gameplay windows** on the Windows build:

| | result |
|---|---|
| reports above 992 handed to the game | **10 708** |
| windows where the size fell below the floor (720) | **0** |
| windows where the size rose above the ceiling (992) | **0** |
| `silent tasks` (a task producing no buffer — the `blez $s3` skip) | **0, every window** |
| reads of `osAiGetLength` per audio task | **exactly 2.00** |

**The failure the cap exists to prevent does not happen.** Reporting 16 384
yields a 720-frame buffer, not a negative one. So:

- **`over-992` is not a defect counter** and nothing should be read as one.
- **The cap-992 experiment was treating a non-problem**, which is the simplest
  explanation for it showing no benefit — no compensation story needed.
- The crash regime past ~33 760 is equally unobserved on this path.
- **Open question, no longer blocking:** the static reading of `sltu` against a
  sign-extended `lh` (§3) says a negative size should survive the clamp, and it
  demonstrably does not. Most likely `func_8001FD14_20914` is not the only
  audio-task builder and the one on the hot path clamps differently. Worth
  knowing, but the answer either way is that capping fixes nothing that occurs.

A third pairing error, caught before it became a finding: `audio_last_reported`
lags the buffer by ~1 task (the function queues the *previous* buffer before
reading and sizing the next), so individual `MISMATCH` lines prove nothing. Only
the aggregate above is valid, and it is, because the drain phase holds the report
above 992 for many consecutive tasks.

### What the review changed — read this before anything else

1. **[RETRACTED] "The wrap was accidentally compensating" (old §6.1) is false.**
   A negative size synthesises *nothing*: `func_8002C4D0_2D0D0` reaches
   `blez $s3` at `2D1C8` with the size in `$s3` and skips the synthesis loop,
   having already skipped the loop above it on a signed `slt`. It cannot make the
   game generate more.
2. **[RETRACTED] The "4031 over-992 events" figure was measured at the wrong
   moment.** The probe sampled at RSP *dispatch* — after the game had already
   read `osAiGetLength`, sized its buffer and synthesised into it, so the queue
   had grown in between. This document's own data refutes it: drain windows
   showed >992 on nearly every task while mean size was exactly **720**, and a
   game generating 720 did not see a value above 992. Fixed: the counter now
   lives inside `get_frames_remaining()`, which *is* the callback.
3. **Consequently the §6.1 "regression" is probably noise.** With the defect rate
   unknown and n=1 per configuration, 0.936 → 0.876 is within content variance.
4. **[REVISED] The burst is load-bearing, not a symptom.** See §4.1 — the
   arithmetic this document never ran.
5. **[REVISED] 29.8/59.8 tasks/s is 30 Hz / 60 Hz**, the game's main loop. My
   "strongest lead" was not a mystery.

---

## 0. One-line status

> **SUPERSEDED — this section is wrong and is kept only so the error is
> traceable. For the current status read the top of this document.** Despite the
> heading, do not quote this paragraph: "one event" is refuted, and a section
> called "One-line status" is the most quotable thing in the file.
>
> **Current one-line status:** the hitch and the dropout are **two bugs sharing
> one root cause** — ultramodern delivers external messages only at guest
> scheduling points, and the guest's frame governor busy-waits on `osGetTime`
> without ever reaching one. **Both are fixed by `HH_OSGETTIME_YIELD_US=100`**,
> measured in gameplay and confirmed by the user by eye and ear.

The original claim, as written: the periodic frametime hitch and the periodic
audio dropout are **one event**: the game's audio **task rate** oscillates
between 29.8/s and 90–120/s on a ~1.5 s cycle. The burst is guest-thread CPU work
(the hitch); the trough drains the audio queue to zero (the dropout). It is not
performance, not the renderer, not a slow audio task, and not dropped interrupts
— each ruled out by measurement below.

**What survived:** the four "not performance / not the renderer / not a slow audio
task / not dropped interrupts" refutations all still hold. **What did not:** that
the two symptoms were one event, and that the burst was guest CPU work — the
stall is the guest *spinning on `osGetTime`*, and the "burst" was a backlog
draining once delivery resumed.

---

## 1. Where things live

Five sibling repos under `~/projects`, all private and local-only.
Only the first two were touched.

| Path | Branch | What it is |
|---|---|---|
| `hybridheaven/` | `review-remediation` | **The decomp.** ROM-format tooling, `asm/usa/*.s`, `PLAN.md`. Source of truth for what the *game* does — §3's control law was read out of `asm/usa/main_20420.s`. |
| `HybridHeaven-Recomp/` | `review-remediation` | **The port.** Everything in this document that is code lives here. |
| `HybridHeaven-RecompSyms/` | `main` | Generated symbol/section tables, consumed by the port as a submodule. |
| `Quest64-Recomp/` | `android-port-step3` | Upstream port #1, for diffing. |
| `Goemon64Recomp/` | `dev` | Upstream port #2, for diffing. Source of this port's UI/input layer. |

**Diff two upstream ports, not one.** Quest64, Goemon and Zelda64Recomp agreeing
is what previously proved this port's bespoke audio was the outlier. Goemon also
carries `docs/re-notes/menu-framerate-handover.md` and an RT64 branch
`origin/diag/menu-framerate` with `g64prof` instrumentation — reusable, but that
was a *sustained* Android menu framerate problem, not this periodic hitch.

Inside `HybridHeaven-Recomp/`:

| Path | Relevance here |
|---|---|
| `src/main/main.cpp` | **The audio path.** `get_frames_remaining()` (the `osAiGetLength` callback — §3), `queue_samples()`, the `HH_TRACE_AUDIO` report, and the new per-task accounting. |
| `src/main/rt64_render_context.cpp` | The renderer. **New: the `HH_TRACE_FRAME` probe** (§2) in `send_dl`/`update_screen`. |
| `src/main/null_render_context.cpp` | `-DHH_RT64=OFF` bisect build — the right thing to build against when a failure might be the renderer. |
| `src/game/recomp_api.cpp` | Overlay loader/eviction hooks, `hh_overlay_load_count()`. |
| `patches/required.c` | N64Recomp patches, incl. the overlay loader hook. |
| `lib/N64ModernRuntime/` | Submodule, branch `hybridheaven-port`, carries local patches. `ultramodern/src/events.cpp` is the VI thread and the gfx thread; `librecomp/src/` is overlays/pi/rsp. **Not modified.** |
| `lib/rt64/` | Submodule, branch `goemon-android`. **Not modified.** |
| `build/`, `build-win/` | Linux build and the cross-compiled Windows exe. |

---

## 2. Two measurement environments — never conflate

| | Linux (`build/`) | Windows (`build-win/`) |
|---|---|---|
| Reports | `via x11` | `via windows` |
| GPU | llvmpipe (software) | NVIDIA RTX 5080, native Vulkan |
| Launch | `HH_AUTOSTART=1 ... ./build/HybridHeavenRecompiled ./hybridheaven.z64` | see trap below |
| Stop | `timeout -k` | PowerShell `Stop-Process` (`timeout` kills only the interop wrapper) |

**Trap that cost a run: `HH_*` env vars do not reach the `.exe`.** binfmt interop
does not forward the Linux environment. You must name them in `WSLENV`:

```
WSLENV=HH_AUTOSTART:HH_TRACE_AUDIO:HH_AUDIO_REPORT_MS HH_AUTOSTART=1 HH_TRACE_AUDIO=1 ... ./build-win/HybridHeavenRecompiled.exe
```

The failure is silent and looks exactly like a broken feature: the process comes
up, `Responding` is True, the window has its title, and it sits in the launcher
emitting no trace lines. **A careless A/B across the two builds silently compares
a traced run against an untraced one.** Also copy the exe to a fresh filename per
run — launching over the WSL UNC path can execute a cached pre-rebuild image.

**Sampling rate matters more than it looks.** The `HH_TRACE_AUDIO` window was
2 s, which **aliased** the real ~1.5 s period into a bogus 4–6 s one. The tell was
`qmax` sitting at ~25 000 in windows whose sampled `queue` read 0. `HH_AUDIO_REPORT_MS`
now sets the window; use 500 ms or finer. **The "every 4–6 seconds" figure from
the first pass of this investigation is wrong — do not quote it.**

### New instrumentation (all kept)

- **`HH_TRACE_FRAME=1`** — `src/main/rt64_render_context.cpp`. Decomposes the
  interval between consecutive display lists into `dl` (RT64 walking the list) +
  `present` (`updateScreen`) + **`idle`** (the gfx thread had nothing to do).
  The idle split is the whole point: a hitch that is idle time is **not a
  graphics bug**, it is the guest stalled. `HH_TRACE_FRAME_MS` sets the spike
  threshold (default 30 ms — raise it above the game's own 33.7 ms frame period
  or every frame is a "spike"), `HH_TRACE_FRAME_REPORT` the summary interval.
- **`HH_AUDIO_REPORT_MS=<ms>`** — audio report window.
- **Per-task audio accounting** added to the `HH_TRACE_AUDIO` line: `tasks/s`,
  `mean size`, `reported mean/max`, `over-992`, `aspMain ms/task and % of wall`,
  and the external-message backlog. The `reported` accounting now samples inside
  `get_frames_remaining()` — the `osAiGetLength` callback itself, the only point
  that sees what the game reads — and is **gated on `HH_TRACE_AUDIO`**. It
  previously ran at RSP dispatch, unconditionally, which both measured the wrong
  moment (§6.1) and cost an `SDL_GetQueuedAudioSize` on the audio path in every
  build, traced or not: a permanent perturbation of the path under investigation.
  The external-message backlog is retained but **does not support refutation #4**
  — see §5.

---

## 3. The game's control law (read from the ROM, not inferred)

`func_8001FD14_20914` in `asm/usa/main_20420.s`:

```
v0 = osAiGetLength()          t7 = v0 >> 2            ; bytes -> frames
t8 = D_80096340(736) - t7     t9 = t8 + 0x100         ; = 992 - reported
t1 = t9 & 0xFFF0              sh t1 -> size           ; stored s16
a3 = (s16)size                v1 = D_8009633C(720)
sltu $at, $a3, $v1            ; UNSIGNED compare
beqz $at, skip                sh $v1 -> size          ; clamp UP to floor
```

So `size = 992 - reported`, floored at 720 — **but the floor is an unsigned
compare against a sign-extended halfword**, so it only rescues a *small positive*
result. Modelled exactly:

| reported | 0 | 128 | 272 | 992 | **1008** | **16384** | **~33 760+** |
|---|---|---|---|---|---|---|---|
| size | 992 | 864 | 720 | 720 | **−16** | **−15392** | **large POSITIVE** |

The controller's entire linear authority is `reported ∈ [0, 272]` — about **6 ms**
at 44.1 kHz. Past 992 the arithmetic breaks, in **two** regimes:

- **Negative (992 → ~33 760).** The size is ≤ 0 and the game synthesises
  **nothing** — `func_8002C4D0_2D0D0` skips its first loop on a signed `slt` and
  then its synthesis loop on `blez $s3` (`2D1C8`). Silence, not damage.
- **Positive wrap (~33 760+).** The s16 wraps back to a large positive count and
  the game synthesises ~32 700 samples in one task, overrunning its `0xA000`
  command list. **This is the regime that caused the port's actual crashes**, and
  it is why a report cap has to exist at all. The current `0x4000` cap sits
  between the two regimes: past the silence edge, well short of the crash edge.

**`736`, `720` and `992` are not ROM literals.** They are `.main_bss` cells
(`D_80096340`, `D_8009633C`) written at runtime by `func_8001F8A0_204A0` from
whatever `osAiSetFrequency` actually returned — and the two builds here opened at
different rates (44 095 vs 48 000). Log them from a live run rather than
asserting them; `HH_TRACE_RSP=1` already prints both.

The port answers `osAiGetLength` from `get_frames_remaining()` in
`src/main/main.cpp`: the whole SDL queue, minus one VI (upstream's
`buffer_offset_frames = 1.0f`), capped at `max_backlog_frames`.

---

## 4. What was measured

All figures: Windows build, RTX 5080, native Vulkan, gameplay windows only
(`peak > 0`), 500 ms windows, one ~120 s run per configuration.

**Baseline** (`HH_AUDIO_REPORT_CAP=0x4000`, `HH_AUDIO_DAMP=1` — i.e. shipped):

- `consumed` **0.936** — the device is fed well *on average*.
- Queue swings **0 ↔ 27 000 frames** (0 ↔ ~570 ms). **20% of windows end EMPTY.**
- Task rate is **bimodal**: 104 windows below 45/s, 54 above 75/s, almost nothing
  between. Mean 55.1/s.
- Audio work per rendered frame swings **1141 ↔ 1745 samples** — this is the
  frametime hitch.
- ~~`over-992` fires in **99% of windows, 4031 events in 81 s**.~~ **[RETRACTED]**
  — measured at RSP dispatch rather than at the game's read, and contradicted by
  the mean size of 720 in those same windows. See §6.1.
- **Cycle: 2 windows draining at 29.8 tasks/s, 1 window bursting at ~100/s →
  ~1.5 s period.**

**Boot phase**: same binary, same code — queue **315–1047 frames, 59.8 tasks/s,
ratio ~1.0, no oscillation.** I called this "the biggest unexplained clue"; the
review points out **29.8/59.8 is simply 30 Hz / 60 Hz**, the game's main loop,
with audio submission coupled to it. Boot's *rendered* rate is only 12–16/s, so
the gfx rate is not the tell — the loop rate is. Not a mystery.

### 4.1 The arithmetic this document should have run first

At the trough: **29.8 tasks/s × 992 samples (the maximum the game can ask for)
≈ 29 500 samples/s, against 44 100 needed — 33% short.** The trough can never
feed the device, however the size half of the loop is tuned.

So **the burst is not instability; it is load-bearing catch-up.** That is the
stock SDK `audiomgr.c` shape: build one task per retrace, and drain the backlog
in a tight loop when behind. The measured mean of **55.1 tasks/s × ~800 samples
lands at 44.1 k**, consistent with trough + burst averaging to real time.

This reframes the target. **Do not try to remove the burst — it is what feeds the
device.** The problem is that the trough is deep enough and long enough that the
queue reaches zero before the next burst arrives.

---

## 5. Refuted hypotheses — do not re-litigate; attack the refutations

1. **"It's performance / the host can't keep up."** Refuted: identical
   oscillation on llvmpipe and on an RTX 5080 at `consumed` 0.89–1.00. Host speed
   does not move it.
2. **"It's the renderer / RT64 / the GPU."** Refuted by `HH_TRACE_FRAME`: RT64's
   display-list walk is **2–17% of wall**, present **0%**, the rest idle waiting
   on the guest. Every multi-second stall was ~100% idle.
   **Caveat the review raised, and it is fair:** the probe's own spike `fprintf`
   lands in the *next* interval's `idle` residual — the exact bucket this
   refutation rests on — and these runs used the default
   `HH_TRACE_FRAME_MS=30`, which is *below* the game's own 33.7 ms frame period,
   so **every frame logged a line**. The refutation survives only because it
   rests on `dl` and `present` being small, which logging overhead inflates in
   the opposite direction. Re-measure with the threshold above 33.7 ms before
   quoting the idle percentages.
3. **"The audio task is too slow, so interrupts are missed."** Refuted:
   `aspMain` is **0.16–0.27 ms/task, 1–2% of wall**, against a 16.7 ms VI period.
4. **"ultramodern is dropping AI interrupts"** (plausible — the AI enqueue passes
   `requeue_if_blocked=false`, `ultramodern/src/events.cpp:332`, so a full guest
   queue silently drops). **[REVISED] The conclusion holds; the citation I first
   gave does not.** `debug_external_message_count()` **cannot see drops**: a
   message is popped by `try_dequeue` *before* `do_send` is attempted
   (`mesgqueue.cpp:29-31`), so a drop is indistinguishable from a delivery and
   count==0 is equally consistent with dropping every one. The valid evidence was
   already in the tree and I failed to cite it: the **`requeue_if_blocked=true`
   A/B** recorded in `hybridheaven/PLAN.md` measured **no effect at all — 0.901
   v 0.905, task rate 50.8 → 51.3/s**. That is what refutes this.
5. **"The buffer-size controller is oscillating."** This was my main hypothesis
   and it is wrong — see §6.2.

---

## 6. The two fixes that measured worse

Both are **off by default**. Three-way A/B, same metrics:

| config | `consumed` | windows at empty queue | tasks/s | over-992 |
|---|---|---|---|---|
| baseline (cap 0x4000, damp 1) | **0.936** | **20%** | 55.1 | ~~4031~~ invalid |
| cap 992, damp 1 | 0.876 | 36% | 50.0 | 0 (tautological) |
| cap 992, damp 4 | 0.915 | 26% | 51.6 | 0 (tautological) |

**Every number in this table is n=1, from runs that were not the same gameplay.**
None of the differences exceeds what content variance alone could produce. The
`over-992` column is retracted outright (§6.1).

### 6.1 Capping the reported backlog at 992 — `HH_AUDIO_REPORT_CAP`

Arithmetically correct (§3): 992 is the largest value the game survives, and the
shipped 0x4000 sits 16× past it.

**[RETRACTED] My explanation of the regression was wrong twice over.**

I wrote that "the wrap was accidentally compensating" — that a negative length
pushes the game to generate more buffers. **It cannot.** A negative size
synthesises *nothing* (§3): `blez $s3` at `2D1C8` skips the synthesis loop, and
the loop above it is skipped by a signed `slt`. The theory is refutable at the
instruction level from a file this document already cited.

**And the defect count that motivated the fix was measured at the wrong moment.**
`over-992 → 0` under the cap is in any case tautological — `get_frames_remaining`
clamps before the counter sampled it — but the baseline figure is worse than
tautological, it is invalid: the probe ran at RSP *dispatch*, after the game had
read `osAiGetLength`, sized its buffer and synthesised into it. **This document's
own §4 refutes it**: drain windows showed >992 on nearly every task while mean
size was exactly **720**, and a game generating 720 did not see a value above 992.

So the true rate at which the game is handed unusable arithmetic is **unknown**,
and with n=1 per configuration the 0.936 → 0.876 swing is within content
variance. **Treat this row as unmeasured, not as a regression.** The counter has
been moved inside `get_frames_remaining()`; re-run per §7.

### 6.2 Damping the size report — `HH_AUDIO_DAMP=<K>`

Theory: the game computes `size[n]` for a buffer queued behind the one playing,
so the correction lands one task late; with `reported = Q[n-1] − offset` the
characteristic polynomial is `z² − z + 1/K`, whose roots at K=1 have magnitude
**exactly 1** — sustained oscillation by construction, which would explain why
host speed never moves it. Dividing by K gives `|z| = 1/√K`.

**It did not work** (0.915, 26% empty). The derivation is correct about the loop
it models; that loop is not the dominant one. **The game varies its task RATE
(29.8 ↔ 120/s) far more than its buffer size (720 ↔ 992), and the size report
cannot reach the task rate.**

**The null result was available from the algebra before spending a run**, and the
review is right to call this out: the same roots (`e^±iπ/3`) predict a **6-task**
period — 50–200 ms at observed rates — an order of magnitude off the measured
1.5 s cycle. A model that mispredicts the period by 10× is not describing the
oscillation. I noticed the discrepancy mid-investigation and did not act on it.

### 6.3 Also tried: capping the SDL queue to a hardware-like depth

`HH_AUDIO_QUEUE_CAP=2400` (~54 ms, roughly what the AI's own FIFO would hold).
Clearly worse: `consumed` **0.64–0.71**, **23–31k frames dropped per 500 ms
window**, queue pinned at 0. Refusing chunks throws away the burst that would
have fed the device. **This also refuted my "deep queue makes the game idle"
story: the task rate still alternated 29.8 ↔ 100+/s with the queue pinned at
zero the entire run.**

---

## 7. Where to go next — reordered by the review

1. ~~**Log ground truth per task.**~~ **DONE — see the box at the top.** The size
   never leaves [720, 992], `silent tasks` is 0, and the game reads
   `osAiGetLength` exactly twice per task. This retires the cap line of work and
   resolves the "709–720 frames every time" contradiction in favour of the
   709–720 observation. **The remaining steps below are unchanged and now have
   one fewer distraction in front of them.**
2. ~~**Walk `func_8001FBA8_207A8`'s dispatch loop statically.**~~ **DONE — see
   the box at the top.** It is stock `audiomgr.c`: blocking `osRecvMesg`, one
   message per audio frame, no polling. The audio backlog does not control the
   task rate.
3. ~~**Prototype the reframed fix (report something FIFO-sized).**~~ **RULED OUT
   as the fix by step 2**, though still worth doing for correctness: it can only
   raise the trough's size from 720 to ≤992, and `29.8 × 992` is still 33% short
   of 44 100. No value returned from `get_frames_remaining()` can close a gap set
   by the task rate.

**NEW step 3 — find why the guest gets ~30 audio messages/s instead of 60.**
This is now the whole problem. The game's `target = 736 = 44100/60` is its own
statement that it expects 60 tasks/s; hardware drove them from the retrace,
independent of a 30 fps render. Trace who posts type-1 messages to
`D_80091DA0_929A0` and at what rate — that is the game's scheduler
(`D_8005C4B0`, the object `inspect_gfx_task` already reads), and the question is
whether the port's retrace delivery is making it run at the render rate instead
of 60 Hz. Note the `requeue_if_blocked` A/B already says this is **not** simple
interrupt dropping.
4. **Re-run §6 with fixed content.** `HH_AUTOSTART` leaves the game wherever it
   lands, so no two runs are the same gameplay. Use a save state or scripted
   input, n≥5, and report median/IQR — every comparative number in §4 and §6 is
   n=1 and none of the differences exceed plausible content variance.
5. **Ask the user's ear.** Every figure here is `consumed` and queue depth.
   `consumed` counts what was *removed* and is blind to what was *added* — an
   earlier decimation change scored well and was audibly inventing sound. No
   configuration in §6 has been judged by ear.

### Measurement changes to make alongside

- **Per-event timestamps, not windowed aggregates.** 500 ms is exactly ⅓ of the
  1.5 s period — phase-locking risk — and a window cannot recover cycle shape.
  Autocorrelation over a timestamp ring buffer removes the window dependence
  that already produced one wrong period in §2.
- **Measure what the ear hears.** The metric for an audible dropout is the SDL
  callback's requested-vs-available shortfall plus a gap-length histogram.
  `consumed` and point-sampled depth can both miss it.
- **Silent-misconfiguration classes**, on top of the WSLENV and stale-exe traps
  in §2: the boolean switches are presence-gated, so `HH_TRACE_FRAME=0` still
  *enables* (this is the existing codebase convention — changing it is a
  separate decision); and an unparseable numeric override falls back to the
  default with no warning.

## 7.1 Methodology debt, and the method note

**The n=1 debt still stands.** It applies to every comparative number in §4 and
§6, and to every arm of the Linux A/Bs in the gameplay section. Repair: fixed
content (save state or scripted input), n≥5, median/IQR. **One exception now
exists:** the hitch fix was confirmed in gameplay by the user by eye and ear, and
the gameplay arms differ by ~100x (843 spikes v 0), which is far outside anything
run-to-run variation produces. Nothing else here should be quoted as an effect.

### The method note — seven wrong inferences, plus two lessons about comparisons

Every one came from a probe at subtly the wrong place, or a number read from the
wrong scope. Kept in full because the cost was paid and the failure modes repeat:

1. Sampling the guest's **input at dispatch** instead of at the callback.
   → **Measure the guest's OUTPUT.** `queue_samples` receives the game's own answer.
2. Pairing a report with a buffer **across a one-task lag**.
3. Counting VI **iterations** when the retrace branch is gated by `retrace_count`.
   → Count the thing that was *sent*, not the loop that might have sent it.
4. A **2 s window aliasing a 1.5 s cycle** into an apparent 4–6 s one.
   → **Sample finer than the cycle.** `qmax` high in a window whose `queue` reads
   0 is the tell that you are aliasing.
5. A guest send-failure rate that **matched the deficit but was on a different
   object** (the main thread's client queue, not audio's).
   → **Log WHICH object, not just how many.** A rate that matches is not a mechanism.
6. Quoting a **boot-phase** delivery latency (0.01 ms) as if it were the run's
   (7.93 ms), which invented a 150x improvement that did not exist.
   → **Never quote an early window as a run figure.** Boot is not gameplay.
7. A `max ...ms` grep that **silently swept in frame-report lines**, producing a
   "delivery latency" that belonged to another object entirely.
   → **Scope every grep to the line that owns the field.** This is #5 again, in
   the analysis rather than the instrumentation — the trap survives being named.

Two more this round paid for directly, both about *comparisons* rather than probes:

8. **Two symptoms sharing a ~1.5 s period were assumed to be one bug** for an
   entire investigation. → **Fix one and see whether the other moves.** That is
   the cheap discriminator, and it should have been step 1 rather than step N.
9. **A fix that removes the symptom may have removed the game instead.** Rendered
   frames rising is ambiguous alone; **audio tasks/s pinned at 60.0 is what
   proves game speed did not change** (a speedup reads ~120). Relatedly: **never
   compare event counts across arms where the bug itself changed the pacing** —
   inline-RSP-only showed *more* spikes than the neither-arm (364 v 135), and it
   means nothing.

And the standing limit on the whole approach: **`consumed` counts what was
REMOVED and is blind to what was ADDED.** Unfiltered decimation once scored well
and was audibly inventing sound. Ask the ear whether artefacts are *gone* or
merely quieter.

## 8. What is in the tree

> **OUT OF DATE — this section describes only the first round.** In particular
> "no submodule was modified" is **no longer true**: the fix for both bugs lives
> in the `lib/N64ModernRuntime` submodule. Current inventory below.

- `src/main/rt64_render_context.cpp` — `HH_TRACE_FRAME` probe (new).
- `src/main/main.cpp` — per-task audio accounting, `aspMain` timing, message
  backlog, `HH_AUDIO_REPORT_MS`, and the two switches
  (`HH_AUDIO_REPORT_CAP`, `HH_AUDIO_DAMP`) with defaults at the **old** behaviour
  and the full reasoning, including both negative results, in comments.
- ~~No submodule was modified.~~ ~~No default behaviour changed.~~ **Both are now
  false.** The submodule is where the fix lives, and
  `HH_OSGETTIME_YIELD_US=1000` is the port's default as of 2026-07-28. That is
  the **only** intentional default change; everything else here still ships off.

### Current inventory (2026-07-28, after the gameplay round)

In `lib/N64ModernRuntime` (branch `hybridheaven-port`) — **the submodule IS
modified; this is where the fix lives**:

- `ultramodern/src/mesgqueue.cpp` — `osgettime_scheduling_point()` and its
  throttle, the engagement counter, external-delivery latency accounting, guest
  send-failure counting, `HH_TRACE_MESG_BLOCK`.
- `ultramodern/src/events.cpp` — `HH_AUDIO_INLINE_RSP`, sp-task yield counting.
- `ultramodern/src/timer.cpp` — the `osGetTime` call counter.
- `ultramodern/include/ultramodern/ultramodern.hpp` — declarations for all of the
  above.
- `librecomp/src/ultra_translation.cpp` — the `osGetTime_recomp` hook. Note it
  passes `PASS_RDRAM`, not a literal `rdram`, or global-RDRAM builds break.
- `librecomp/src/sp.cpp` — sp-task yield instrumentation (the port ignores guest
  `osSpTaskYield`; measured benign at 0–8 yields/s).

In the port proper:

- `src/main/main.cpp` — `HH_FRAME_GOVERNOR_OFF`, `HH_FRAME_GOVERNOR_US`, and the
  osgettime-yield counter in the `[audio]` report.
- `src/main/rt64_render_context.cpp` — osGetTime count and frame-step on SPIKE
  lines.
- `docs/development-notes.md` — all switches documented, with
  `HH_FRAME_GOVERNOR_OFF` marked **diagnostic only, do not ship**.

Every switch ships **off**. Both the Linux and Windows builds are current.

---

## Appendix: the V-Sync setting (2026-07-28) — and the second patched submodule

Unrelated to the hitch, but recorded here because it introduces a **new fork
patch in `lib/rt64`**, which until now tracked `goemon-android` clean. This
project has already lost patches silently three times; a second patched submodule
doubles that exposure.

**What existed already:** plume (RT64's RHI) fully implements vsync —
`RenderSwapChain::setVsyncEnabled()` is pure virtual with Vulkan
(FIFO <-> IMMEDIATE), D3D12 (`syncInterval` + `ALLOW_TEARING`) and Metal
implementations. **RT64 never called it.** The only invocation anywhere was
plume's own `setVsyncEnabled(true)` inside Vulkan swap-chain construction, so
vsync was hardcoded on and `UserConfiguration` had no field for it.

**The patch, in `lib/rt64`:**

- `src/common/rt64_user_configuration.h` — `bool vsync;`
- `src/common/rt64_user_configuration.cpp` — `to_json`, `from_json` (absent key
  -> default), and `vsync = true` in the constructor.
- `src/hle/rt64_application.cpp` — `swapChain->setVsyncEnabled(userConfig.vsync)`
  after `createSwapChain`, and again in `updateUserConfig()` for live changes.
  `RenderSwapChainDesc` has no vsync field, so it must be set post-creation;
  Vulkan then picks it up because `needsResize()` returns true while
  `requiredPresentMode != createdPresentMode`.

**Port side:** `VSync` enum + `vsync_option` in ultramodern's `config.hpp`,
default/serialise/reset in `src/game/config.cpp`, `bind_option` in
`src/ui/ui_config.cpp`, the control and its help text in
`assets/config_menu/graphics.rml`, and the apply + `[gfx] vsync requested ...`
line in `src/main/rt64_render_context.cpp`.

**Verified:** `Off` -> `[gfx] vsync requested Off`; `On` -> `On`; **key absent ->
`On`**, so existing `graphics.json` files are unaffected on upgrade.
`graphics.rml` validates as well-formed. (`config_menu.rml` does *not* validate
as strict XML — it uses valueless attributes like `autofocus`. That is
pre-existing and expected for RML, not a defect.)

**Expectation management:** this will not change the frame rate. The game paces
itself and the port forces `PresentEarly`, so `present` measured **0.0 ms** in
every frame trace taken during the hitch investigation. It is a latency/tearing
preference. The apply line states what was **requested** — Vulkan silently falls
back to FIFO where IMMEDIATE is unsupported, so a log saying `Off` is not proof
the device granted it.
