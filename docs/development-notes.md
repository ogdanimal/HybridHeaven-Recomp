# Hybrid Heaven: Recompiled — development notes

The engineering journal for this port: what was built, what broke, how each
failure was diagnosed and what the evidence was. It is written to be read after
the fact by someone touching the same code, so most entries lead with the wrong
answer that was tried first.

For what the port *is* and how to play it, see [`../README.md`](../README.md);
for how to build it, [`../BUILDING.md`](../BUILDING.md); for the decomp it is
built from, [`../lib/hybridheaven/README.md`](../lib/hybridheaven/README.md).

## Layout

| path | role |
|---|---|
| `hybridheaven.toml` | recompiler config (gitignored ROM input) |
| `hybridheaven.overlays.txt` | the relocatable overlay sections |
| `hybridheaven.dump_context.toml` | config for `--dump-context`, which generates the Syms repo |
| `aspMain.toml` | RSPRecomp config for the audio microcode |
| `rsp/aspMain.cpp` | generated; byte-identical to Goemon64Recomp's |
| `patches.toml` | recompiler config for the patches |
| `patches/` | MIPS C compiled and recompiled to override recompiler output |
| `lib/hybridheaven` | the decomp — supplies the ELF, and headers for patches |
| `HybridHeaven-RecompSyms` | generated `.syms.toml` / `.datasyms.toml` |
| `lib/N64ModernRuntime`, `lib/rt64` | the runtime and renderer |
| `tools/*.patch` | local fixes to pinned dependencies, see below |
| `tools/verify_overlay_hook.py` | asserts the loader hook still agrees with the section table |
| `tools/recomp_symbol_gap.py` | lists libultra the output calls but the runtime lacks |
| `src/main`, `src/game` | the entry layer, host exports, config and input |
| `src/main/rt64_render_context.cpp` | the renderer; `-DHH_RT64=OFF` builds `null_render_context.cpp` instead |
| `include/hh_render.h` | the render context's interface |
| `src/ui/` | the RmlUi launcher and config menus, from Goemon64Recomp — see "The launcher" |
| `assets/` | the launcher's documents, stylesheets, fonts and icons, loaded from disk at runtime |
| `shaders/Interface{VS,PS}.hlsl` | what the UI composites with; compiled by rt64's DXC rules |
| `include/hh_config.h`, `hh_sound.h`, `hh_support.h` | the settings, volume and platform interfaces the UI calls |
| `lib/RmlUi`, `lib/lunasvg` | the UI toolkit and its SVG backend, pinned where Goemon64Recomp pins them |
| `lib/SlotMap`, `lib/GamepadMotionHelpers` | vendored headers the UI and input layer need |
| `cmake/embed_binary.cmake` | turns patches.bin into a C array, replacing rt64's file_to_c |

`hybridheaven.z64` — the **decompressed** ROM, sha1
`61ed3d5d5390c3c17b4026797b93be50a61dd6f2` — is required at the repo root and is
gitignored. Copy it from `lib/hybridheaven/config/usa/baserom.decompressed.z64`.
Goemon64Recomp's `mnsg.z64` is decompressed too, which is what makes ROM offsets
taken from the decomp valid in these configs unchanged.

That is the **recompiler's** input, and it is not the ROM the built port asks a
player for — the port takes a retail cart dump and decompresses it itself (see
"Building"). Two different files, both called a ROM; the decompressed one is a
build-time artifact.

## Status

Working:

- **The launcher.** `src/ui/` is Goemon64Recomp's RmlUi interface: the launcher
  gates startup, so the game boots when the player picks Start Game rather than
  the instant the process does. Config menus, remappable keyboard *and* gamepad
  bindings, a real analog stick, rumble, main volume and the mod menu come with
  it. **Verified drawing, and verified playable**: `HH_TRACE_UI=1` reports the
  launcher handing the render interface ~660 geometry batches and ~20,150
  vertices per second, and a run with no `HH_AUTOSTART` reached the game — which
  only the launcher's Start Game button can do. See "The launcher".
- **Audio microcode.** `RSPRecomp aspMain.toml` reproduces
  `Goemon64Recomp/rsp/aspMain.cpp` byte-for-byte. `aspMain` is the only microcode
  needing recompilation — RT64 implements F3DEX itself. See
  `lib/hybridheaven/PLAN.md` Phase B.
- **Whole-game recompilation.** `N64Recomp hybridheaven.toml` exits 0 over all 92
  overlays and the static segment, producing 319 files in `RecompiledFuncs/`.
  Nothing is skipped except the two rspboot pseudo-functions, which are RSP
  microcode rather than VR4300 code.
- **The overlay loader hook.** `make -C patches && N64Recomp patches.toml` both
  exit 0, producing `RecompiledPatches/`. See "The overlay loader hook" below.
- **The recompiled game builds and boots.** `cmake --build build` produces the
  three static libraries and `HybridHeavenRecompiled`, every `_recomp` symbol
  resolves (`tools/recomp_symbol_gap.py` reports **0 missing**), and running it
  gets through the entry stub, `osInitialize` and the boot function into the
  game's own threads.
- **The port takes a retail cart dump.** `src/game/rom_decompression.cpp` expands
  the Nisitenma-Ichigo/LZKN64 archive in memory, verified byte-identical to the
  decomp's `tools/rommy.py` over the whole 32 MiB, and verified end to end: given
  the retail ROM the port stored 16,777,216 bytes and booted, loading overlays at
  decompressed-ROM offsets. Nothing has to prepare a ROM beforehand any more.
- **An Android build, arm64-v8a.** `android/` produces an APK carrying
  `libHybridHeaven.so`, and the patch override survives the link there too
  (`func_8000469C_529C` resolves at `0x3c4`, PatchesLib's definition). It also
  builds, behind `-PcustomDriver=true`, with support for a **user-supplied Vulkan
  driver** — see [the custom Vulkan driver](#the-custom-vulkan-driver), which is
  built and linked but not yet exercised on a device. See "Building".
- **The overlay loader hook works, and is now verified at runtime rather than
  only statically.** The game loads **10 overlays** in a normal boot, including
  three separate loads into the shared slot at `0x803837E0` — tenants taking
  turns, exactly as Phase A predicted — and it executes recompiled code *inside*
  a loaded overlay (`func_80107830_4E6F40`, in `.file_7`).
- **The port runs the game.** The osPfs subsystem is named, so the Controller Pak
  check passes instead of stopping. CPU sits at roughly one core with the game's
  own loop running, and overlays stream in for as long as the process is left
  alone — **39 loads across 30 distinct sections** in two minutes, well past the
  boot set.
- **Overlay eviction.** A load now drops whatever it is about to overwrite, so
  two tenants of overlapping slots can no longer both be callable. See "Eviction
  on load" below.
- **Quitting is clean.** It was not, and the crash that came of it was mistaken
  for a crash in the game — see "The SIGSEGV that was not a crash".
- **Calls no longer go to overlays that are not loaded.** 489 direct calls in the
  recompiled output were bound to an arbitrary tenant of a shared slot; they are
  runtime lookups now. That was what made a "play sound 125" call land in an
  absent overlay and fault at struct offset `0x111`. See "Ambiguous call
  targets".
- **The Memory Pak screen no longer wedges the port.** Left alone, that screen
  ends in a video-mode change during which the game re-initialises the three
  scheduler task records — including one already queued — so a graphics task
  arrives with `data_ptr = 0` and RT64 interprets rdram from offset 0. `send_dl`
  drops a display list that does not start inside rdram, and the run continues
  into real content instead of leaking 490 MB/s. See "Why the `data_ptr` was
  null".
- **The audio backlog is bounded.** `get_frames_remaining` reported SDL's whole
  queue, which after a stall exceeds what the game's s16 frame-size arithmetic
  can hold. That one number produced *both* remaining failures — the audio
  command list overrunning into the OSTask, and the null dereference in the
  game's DMA callback. See "The audio path".

**Nothing fails any more.** Left alone, the port streams **39 overlay loads
across 30 distinct sections**, runs a live game loop at about one core across
seven game threads, and is still going when the timeout stops it — six
consecutive 150-second runs with no signal, no RSP failure and no diagnostic of
any kind.

What is missing is a picture, and that is deliberate.

The decomp side is settled: the twelve functions this config used to stop at are
now named in `lib/hybridheaven/config/usa/symbol_addrs/symbol_addrs.txt`, and
`lib/hybridheaven/tools/undefined_funcs_report.py` is the check that no more of
them exist. See "What the decomp owed" below.

**There is a picture.** `src/main/rt64_render_context.cpp` drives RT64, and the
game renders: it reaches the "Memory Pak Enhanced" screen and waits there for a
button. Built and verified on Linux against Vulkan, on `llvmpipe` — a software
device, so it is slow, but it is the same path a real GPU takes.

The null renderer has not gone away and should not: `-DHH_RT64=OFF` builds
`src/main/null_render_context.cpp` instead, which implements `RendererContext`
as no-ops. It runs everything below the display list without standing up RT64,
which makes it the right bisector — a failure that survives it is not a renderer
bug.

What this port's render context leaves out of Goemon's, deliberately: the RmlUi
layer (`recompui::set_render_hooks`), the texture-pack action queue, and the
Android window-handoff path. What remains is the part that drives RT64.

**Controls** in the default build are Goemon's whole input layer — keyboard and
gamepad, both remappable, with rumble; see "Controller input". On top of it,
**Analog Camera** gives the right stick free look while you walk, which the
game's own C-button camera cannot do; it is off by default and lives under
Options → General. See "Analog camera". The table below is
the **`-DHH_UI=OFF`** build's fixed keyboard layout, which stays because the
bisect build has to be able to press Start without dragging in RmlUi to do it:

| | |
|---|---|
| Start | Enter |
| A / B / Z | X / C / Space |
| L / R | Q / E |
| analog stick | W A S D |
| D-pad | arrow keys |
| C buttons | I J K L |

### Environment variables

All are bring-up switches; none has to be set for a normal run.

| variable | effect |
|---|---|
| `HH_TRACE_OVERLAYS=1` | log every overlay load and eviction as it happens |
| `HH_NO_EVICT=1` | do not evict overlapping overlays on load (the A/B for eviction) |
| `HH_RELOCATE_OVERLAYS=1` | track each load's address in `section_addresses` instead of keeping the link address (the A/B for the relocation policy) |
| `HH_DUMP_MAPS=1` | append `/proc/self/maps` to a crash report, which is what settles whether a fault address is inside rdram at all |
| `HH_TRACE_RSP=1` | log every audio task with its command-list size, the queued audio backlog and the game's own frame target |
| `HH_TRACE_AUDIO=1` | every 2s, report frames produced per second, **`consumed` — what the device actually took, by accounting** — frames dropped, the queue depth and its high-water mark against the cap, and the **peak sample amplitude** |
| `HH_AUDIO_QUEUE_CAP=<frames>` | the latency ceiling: how much backlog SDL may hold before new chunks are refused (default `0x10000`, ~1.5 s). Distinct from the report cap `max_backlog_frames`, which stays `0x4000` — sharing one constant between the two cost a quarter of the audio. `0x4000` here reproduces the old *cap* but not the old *discard* |
| `HH_AUDIO_CLEAR_ON_OVERFLOW=1` | on overflow, discard the **whole** backlog and enqueue anyway, as the port originally did, instead of refusing the incoming chunk (the A/B for the discard policy — keeps it comparable on one binary at one cap) |
| `HH_AUDIO_REPORT_MS=<ms>` | the `HH_TRACE_AUDIO` window (default 2000). **Use 500 or finer for anything periodic** — the 2 s window aliased a real ~1.5 s audio cycle into an apparent 4–6 s one, and `qmax` sitting high in a window whose `queue` read 0 was the tell |
| `HH_AUDIO_REPORT_CAP=<frames>` | the cap on what `osAiGetLength` reports (default `0x4000`). **`992` is the arithmetically correct bound** — past it the game's unsigned `sltu` clamp is skipped and the size goes negative, which makes the game synthesise **nothing** (`blez $s3`); past ~33760 it wraps positive instead, which is the overrun that caused this port's crashes. `0x4000` sits between the two. The one A/B run at 992 looked worse, but it is **n=1 and not a result** — see `docs/frametime-investigation.md` §6.1, which retracts the reasoning that row was based on |
| `HH_AUDIO_DAMP=<K>` | divide the reported deviation by K to damp the game's rate loop (default 1, off). Sound theory, **no measured effect** — the game varies its task *rate* far more than its buffer *size* |
| `HH_TRACE_FRAME=1` | per-frame pacing, decomposing the interval between display lists into `dl` + `present` + **`idle`**. The idle split is the point: a hitch that is idle time is the **guest** stalled, not the renderer. `HH_TRACE_FRAME_MS` sets the spike threshold (default 30 — raise it above the game's own 33.7 ms frame period), `HH_TRACE_FRAME_REPORT` the summary interval |
| *(setting, not a switch)* `vsync_option` in `graphics.json` / **V-Sync** in the graphics menu | **On** (default) synchronises presentation with the display refresh; **Off** selects `VK_PRESENT_MODE_IMMEDIATE_KHR` under Vulkan — only where the device reports support, plume falls back to FIFO otherwise — and `syncInterval 0` + `ALLOW_TEARING` under D3D12. **This does not change how fast the game runs**: the game paces itself and the port forces RT64's `PresentEarly`, so presentation already measured non-blocking (`present 0.0 ms` in every frame trace). It is a latency/tearing preference. Logs `[gfx] vsync requested On/Off` once and on change, because a graphics setting that silently fails to apply is this port's recurring failure mode. **Requires a local patch to the `lib/rt64` submodule** — see below |
| `HH_OSGETTIME_YIELD_US=<us>` | **ON BY DEFAULT at `1000`. Set `0` to disable and restore the pre-fix behaviour exactly** — the only default-on entry in this table, and the A/B for the fix itself. **The fix for the periodic frametime hitch AND the audio oscillation — one switch for both.** Makes `osGetTime` a throttled scheduling point: deliver pending external messages, then `check_running_queue`. The guest's frame governor busy-waits on `osGetTime` for most of every frame without ever making a blocking call, and the main thread is the **lowest priority** guest thread, so retrace/SP-done/DP-done sat undelivered 7.93 ms mean / 33.45 ms worst. A late retrace delays the gfx dispatcher, leaving the previous frame's task in flight when the game polls `sc+0x89C` — and the game **skips its own render**. `100` measured **843 periodic 67 ms spikes → 0** in gameplay, delivery latency → 0.016 ms, with audio unchanged at 60.0 tasks/s (the control proving game speed did **not** change). Default `1000` — swept 50 µs–5 ms, the whole range is equally effective while cost varies **93×**, so this is a cost decision and not a correctness one; 1000 is ~10× cheaper than 100 for identical spikes and audio (n=3), and its 0.30 ms delivery latency is still ~56× under the 16.7 ms VI period |
| `HH_AUDIO_INLINE_RSP=1` | **SUPERSEDED — you do not need this.** Runs audio RSP tasks synchronously on the submitting guest thread. It fixes the audio oscillation (29.7↔86 bimodal → flat 60.0/s), but `HH_OSGETTIME_YIELD_US` — now the default — already does so by fixing the root cause rather than routing the audio path around it. Proven redundant by a full 2×2 and by a yield-only gameplay run judged good by ear. Kept as an A/B and a fallback; ships **off** |
| `HH_FRAME_GOVERNOR_OFF=1` | **diagnostic only, do not ship.** Pokes the guest's frame-step 2→0, removing the governor wait entirely. Removes the spike and runs the title at 60 fps — but almost certainly doubles game speed. It "worked" only by deleting the wait that *exposed* the delivery latency; the governor was the symptom, not the cause |
| `HH_FRAME_GOVERNOR_US=<us>` | retune the governor's frame period (`D_8004B908`). **Measured null result, kept as one** — the governor's deadline is resampled every frame (`0x80001938`) so it cannot drift, and retuning it changes nothing |
| `HH_TRACE_MESG_BLOCK=1` | timestamped block/wake log per guest thread and queue. This is what proved the main thread makes **no blocking OS call** for the whole 67 ms stall — i.e. it is computing/spinning, not waiting on the port — which moved the investigation from the port to the guest in one step |
| `HH_TRACE_MEM=1` | every 1s, report RSS, `mallinfo2` (`arena`/`hblkhd`/`uordblks`) and the depth of all five unbounded runtime queues; also dumps **every thread's stack** over `SIGUSR2` once the gfx queue passes 180 |
| `HH_TRACE_GFX=1` | one line per graphics task: the game's boot state, the scheduler's record index and in-flight count, all three `OSScTask` records, and the submitted task's `data_ptr`/`data_size`. A task with an unusable `data_ptr` prints the same line **unconditionally** (first 8 only), so a normal run still reports the event |
| `HH_NO_DL_GUARD=1` | hand a null display list to RT64 anyway, so the command ceiling catches it instead (the A/B for the guard in `send_dl`) |
| `HH_EVICT_WHOLE_SECTION=1` | drop an overlapped section entire rather than only the functions a load actually overwrites (the A/B for partial eviction) |
| `HH_SELFTEST_EVICT=1` | at the first overlay load, run the partial-eviction case the game does not reach in a playable run and print what it dropped |
| `HH_NO_DMA_OVERLAY_REGISTER=1` | stop claiming overlays that arrive through the game's *unpatched* asynchronous archive loader, `func_80004838_5438` (the A/B for the battle-transition fix). Without the claim, `.file_56`'s bytes reach rdram and nothing registers them, which is the original `Failed to find function at 0x803757B0` |
| `HH_SELFTEST_DMA_OVERLAY=1` | at the first overlay load, replay `.file_56` as 27 chunks and check five properties of the claim — including that nothing becomes callable mid-load |
| `HH_DROP_TAIL_CLIPPED=1` | drop a function when a load overwrites its **final word** — the delay slot of its returning `jr $ra` — restoring the strict rule (the A/B for the tail-clip fix). This is the rule that cost `func_803757B0_8193D0`: `.file_55` loads at `0x803757E0`, four bytes inside it, and the game calls `0x803757B0` immediately afterwards |
| `HH_NO_RELOAD_REPAIR=1` | when a section's bytes arrive again while it is still listed as loaded, leave its dropped functions missing instead of restoring them (the A/B for the reload repair). Partial eviction leaves a section **loaded but half-callable**, and without this repair the game exits on the first missing function it calls — `0x803758FC`, seven times in an eight-battle run |
| `HH_SELFTEST_TAIL_CLIP=1` | at the first overlay load, replay the loads behind both of the above and check five properties: the tail-clipped function survives, the strict rule really does drop it, a load one word deeper still drops it, a re-load restores the 147 functions an overlapping load took, and without the repair they stay missing |
| `HH_AUTOSTART=1` | skip the launcher and boot the stored ROM immediately. **Set this for any timed run**: 300 seconds of clean play is not a measurement if the run spent them on a menu waiting for a click. Needs an already-validated ROM, since nothing is there to pick one |
| `HH_WINDOW=WxH` | window size, default `1280x720` with the UI and `640x480` without. The A/B for what the window costs the audio — llvmpipe rasterises on the CPU and the default Auto resolution follows the window |
| `HH_TRACE_UI=1` | every 1 s, report whether a UI context is shown and **how much geometry RmlUi handed the render interface** (batches and vertices per second). This is how a run proves the launcher drew without anyone looking at the screen |
| `[gfx] window` (always on) | not a switch: the window's real size and position, the usable desktop bounds, and **which SDL video driver** reported them. A window with no overlap at all with the desktop is flagged `OFF-SCREEN` and moved to the centre |
| `HH_TRACE_CAM=1` | report every take-over and hand-back of the analog camera, a line a second while it is held, and the round-trip check on its own arithmetic. See "Analog camera" |
| `HH_TRACE_CAMMODE=1` | report **the GAME's own camera mode machine** — its mode and installed handler, the scene gate, whether C-left/C-down would be accepted, every C-press with the game's verdict, framing and goal distances. On every mode transition, and once a second otherwise. This is what identified modes 0/2/3/5 as cutscene/exploration/battle/ladder. See "Analog camera" |
| `HH_CAM_NO_MODE_GATE=1` | drive the camera in **every** camera mode, the A/B for the mode gate. With it the analog camera overrides the direction during cutscenes, on elevators and through battles — none of which the game itself permits |
| `HH_CAM_NO_AIM_RELEASE=1` | keep the camera while **R** is held, the A/B for the gun-aim release. With it the right stick overrides the framing the game chose for its own gun shot |
| `HH_CAM_NO_FRAMING_GAIN=1` | follow the camera goal's radius raw, the A/B for the framing gain. The goal is not where the game's camera settles (median 37.9 against the game's 45.5), so this frames ~0.83× the game's distance — "too close behind the character" |
| `HH_CAM_LEGACY_RADIUS_EASE=1` | restore the asymmetric 6.0-in/1.5-out framing ease, which is a valley detector on a flickering goal |
| `HH_CAM_FORCE_X=<v>` / `HH_CAM_FORCE_Y=<v>` | pin the right stick to a constant in [-1, 1]. **Not a cheat — the only way the analog camera can be exercised on this machine at all**, since WSL passes no pad through and every path past the engage threshold is otherwise unreachable. Absent, they change nothing |
| `HH_CAM_FORCE_ZOOM=1` | hold the zoom modifier (RB) down, for the same reason |
| `HH_CAM_NO_ANCHOR=1` | stop pulling the camera's look-at onto the player, i.e. the A/B for anchoring. With it, the orbit circles whatever point a room chose to frame — which is how a fixed-camera room reads as the camera no longer tracking you |
| `HH_CAM_NO_ROLL_FIX=1` | let the analog rotation run inside the game's roll back-solve, the A/B for the roll fix. Turning while pitched then rolls the horizon over: 0° → −24° → −67° → −112° → −156° in eight seconds |
| `HH_NO_WIDESCREEN_SCISSOR=1` | stop rewriting the game's 4:3 safe-rect scissors, i.e. the A/B for widescreen. See "Widescreen" |
| `HH_WIDESCREEN_LEGACY_SCAN=1` | widen only at offsets the periodic full scan has already cached, and only on the call after it cached them — the A/B for the transition fix. See "The transition bug" |
| `HH_TRACE_WIDESCREEN=1` | report every safe-rect-shaped RDP command found in rdram, matched or not — how a rect the table is missing becomes a line rather than silence. Also every frame that reaches RT64 with an un-widened safe rect still in the live list, and a periodic rate for them |
| `HH_TRACE_ASPECT=1` | report the ratio scale RT64 actually applied (`1.000` = no widening, whatever the menu says). This is what separates "the setting is on" from "the picture is wider" |
| `HH_2D_ASPECT=stretch\|native\|legacy` | what a frame with **no 3D geometry in it** does in widescreen — the logo screens. `stretch` (the default) widens the 4:3 art to fill the frame; `native` keeps its proportions and pillarboxes it; `legacy` restores RT64's own per-rect decision, which tears the picture. See "The logo screens" |
| `HH_TRACE_2D_ASPECT=1` | report every entry into and exit from a 2D screen, with a running count of how many render passes of the run they are. The exposure measure for the rule above: 1314 of 15200 passes on a 90-second boot, all of them the logo screens and two brief fades |
| `[input]` (always on) | not a switch. In the UI build: one startup line giving the controller mappings loaded and the joystick count — which is how a log says the gamepad subsystem came up — then a block per pad connect/disconnect with name, GUID and the mapping SDL chose. In `-DHH_UI=OFF` builds it is instead every change in the button word, so a log says whether a keypress reached the game and when |

`HH_TRACE_MEM` is the tool for any memory or hang question here, and it exists
because **`LD_PRELOAD` does not work on this binary**. A tracer hooking
`malloc`, `calloc`, `realloc`, `memalign`, `aligned_alloc`, `posix_memalign`,
all four `operator new` forms, `mmap` and `mmap64` — every one control-tested
against a stub program, and confirmed loaded into the game — logged *nothing*
while 2.5 GB of fresh anonymous mappings appeared. `mallinfo2` then showed
`uordblks` tracking RSS exactly, so the memory really was glibc malloc all
along. Why the interposition misses it is unexplained; do not spend a session
rediscovering that it does.

Read the queue depths as a **consumer** diagnostic. A queue growing at exactly a
known producer's rate — the gfx queue climbing at 60/s, the VI thread's rate,
while every other queue sits at 0 — means the consumer stopped, not that the
queue is leaking. That is how the runaway display list below was found: 590
queued items were never 2.6 GB, they were the symptom.

The `SIGUSR2` thread dump is the substitute for a debugger, since there is no
`gdb`, `strace`, `perf` or `valgrind` on this machine. Each thread prints its
own backtrace; resolve the bare `+0x...` offsets with
`addr2line -f -C -e build/HybridHeavenRecompiled 0x...`. **The trap is that
`SIGUSR2` terminates the process unless the handler is installed before anything
sends it** — an earlier attempt to arm instrumentation with `SIGUSR1` killed
several runs at the exact moment they became interesting.

`HH_TRACE_AUDIO` exists because a draining queue proves only that the device
*consumes* — silence and music are indistinguishable by rate alone. The `peak`
column is the one that matters: a peak pinned at `0` means the game is
synthesising nothing, and no amount of rate analysis will show it. The obtained
SDL spec is reported unconditionally at every device open, because a reopen
mid-run is itself a glitch and the real frequency is the first thing to check.

The crash handler prints the faulting address, `si_code`, and the guest address
it corresponds to. **Read all three.** A `SEGV_MAPERR` inside rdram means the
mapping is gone rather than the access being bad, and a guest address that wraps
— an rdram offset of `0x80000111`, say — means the game dereferenced `0x111`,
i.e. a null pointer plus a struct offset.

Ending a run with `timeout` or Ctrl-C is a **real shutdown** — it runs
`ultramodern::quit()`, which tears the renderer down and joins the saving thread.
Use `timeout -s KILL` when you want the process stopped without running the quit
path.

**How that works changed when the launcher landed, and the old description was
load-bearing.** It used to be SDL's doing: SDL's own handlers turn SIGTERM and
SIGINT into `SDL_QUIT`, and the port quit on `SDL_QUIT`. The UI answers
`SDL_QUIT` differently — mid-game, `sdl_event_filter` opens the *confirm quit
prompt* — and in a headless run nobody is there to answer it, so every timed run
would have needed `-k` to escalate to SIGKILL, losing the orderly shutdown that
took this port a while to get right.

So the port now takes the two signals away from SDL (`SDL_HINT_NO_SIGNAL_HANDLERS`
before `SDL_Init`, its own handlers after) and routes them out of band: the
handler sets one atomic, and `update_gfx` turns it into `ultramodern::quit()` on
the render thread. A signal is not a question. Only the window's close button
reaches the prompt. Measured: SIGTERM to a launcher sitting idle exits in **0.19
s**, with no SIGKILL needed.

### Why the loader hook never fired — two compounding bugs

The hook was correct all along. Two things downstream of it meant the patched
loader was never the code that ran, and neither produced any diagnostic.

**1. The patch was not linked in at all.** A patched function is emitted
*twice* — once by the main recompilation into `RecompiledFuncs/`, once by the
patch recompilation into `RecompiledPatches/` — and the override works by
letting the linker pick the patch's copy. Upstream gets that from two things
together: `PatchesLib` is listed **before** `RecompiledFuncs`, and Clang's
`RECOMP_FUNC` is `extern inline __attribute__((weak,noinline))`, so the base
copy is interposable.

This build had neither. It links with **GCC**, where `RECOMP_FUNC` is
`__attribute__((noipa, ...))` — a *strong* symbol — and it listed the two
libraries in the opposite order. Both definitions were strong, in two static
archives, so the linker simply bound all 52 call sites to the first archive it
reached and never extracted `patches.c.o`. No duplicate-symbol error, because an
archive member that is never needed is never pulled in. The build succeeded and
ran with **no patches applied at all**.

Fixed on both axes, because either alone is insufficient: the link order is
swapped in `CMakeLists.txt`, and `tools/n64recomp-gcc-weak-recomp-func.patch`
makes the GCC branch of `RECOMP_FUNC` weak too. Worth upstreaming — as it
stands, any GCC build of any N64Recomp port silently discards every patch.

**2. The patch's data references used the wrong section index space.** With the
patch finally executing, it faulted immediately. Its generated code read
`section_addresses[1]` for the archive tables, and got 0.

The recompiled code indexes one global `section_addresses` array, but the two
recompilations number sections differently. `hybridheaven.toml` uses `elf_path`
(it has to — see "Why this uses `elf_path`"), and ELF mode keys sections by **ELF
section index**, making `.main` 6. The patch resolves game symbols through the
`.syms.toml` reference context, which numbers sections **densely by position**,
making `.main` 1. Goemon never sees this because both of its recompilations read
the same `.syms.toml`.

Function references are unaffected — those resolve by name and are emitted as
direct calls. Only *data* references carry a section index. So the three archive
symbols now get absolute addresses in `patches/syms.ld`, which N64Recomp folds to
constants with no section lookup (`use_absolute_symbols` was already on). That is
sound here because all three live in `.main`, which is loaded at its link address
and never moves — it is only marked relocatable so its `R_MIPS_26` relocs load.

**Any future patch must follow the same rule**: call game functions by name
freely, but reference game data by absolute address until the two index spaces
are unified.

## Building

```bash
cmake -S . -B build -G Ninja -DHH_N64RECOMP=/path/to/N64Recomp
cmake --build build
./build/HybridHeavenRecompiled "/path/to/Hybrid Heaven (USA).z64"   # the retail rom
```

Two build options, and they are coupled:

| option | default | effect |
|---|---|---|
| `HH_RT64` | `ON` | the renderer. `OFF` builds `null_render_context.cpp`, which runs everything below the display list without standing up RT64 — the bisect build |
| `HH_UI` | `ON` | the launcher and config menus. **Forced `OFF` by `-DHH_RT64=OFF`**, since the UI draws through RT64's render hooks and its shaders are compiled by rt64's DXC rules |

`-DHH_UI=OFF` is also the A/B for what the UI costs (see "The launcher"), and it
is what the audio figures under "The audio path" were measured on. With it off,
`main.cpp` starts the game directly against the stored ROM and polls a fixed
keyboard layout itself — exactly as this port worked before the launcher existed.

The standing checks are a target rather than a habit. Neither needs the build,
so this is cheap to run at any point:

```bash
cmake --build build --target check   # verify_overlay_hook.py + recomp_symbol_gap.py
```

The decomp has its own `make check` for the three on that side. Two further
checks run on their own: `patches/Makefile` fails the link on an undefined
`recomp_*` **or** on any undefined symbol reached by a HI16/LO16 reloc (the
section-index bug — undefined *function* symbols stay legal, since R_MIPS_26
carries no section index), and the port complains at runtime if 60 RSP tasks go
by without the loader hook running once. That last one exists because whether a
patch actually overrode is a link-time property no table lookup reveals, and this
port shipped one build where it had not.

The ROM argument is only needed once; it is validated and copied under the config
directory. It wants the ordinary **retail** cart dump (sha1
`16dbc21620b52deab5c5abf8a309ac60adfbee85`), which is what a player owns.

The recompiled code is still built against the decompressed image, and both the
recompiled section table and the game's own archive table still hold
decompressed-ROM offsets — but that expansion now happens in memory on the way
in. `src/game/rom_decompression.cpp` is registered as the `GameEntry`'s
`decompression_routine`, which librecomp runs *after* matching `rom_hash`, so the
hash that gates entry is the retail one and the file kept on disk stays the
player's own dump.

It is adapted from `Goemon64Recomp/src/game/rom_decompression.cpp`. Both games
are Konami N64 titles using the same Nisitenma-Ichigo file table and the same
LZKN64 compression, so the decoder and the table walk are that port's verbatim
and only the constants differ.

Its output has to be **byte-identical** to what the decomp's `tools/rommy.py`
produces, because that is the image the recompiler consumed — a decompressor that
came out merely almost right would boot and then fail somewhere with no visible
connection to the ROM. So it is checked against the whole 32 MiB rather than a
hash of it:

```bash
c++ -std=c++20 -O2 -I include -o /tmp/verify_rom_decompression \
    tools/verify_rom_decompression.cpp src/game/rom_decompression.cpp
/tmp/verify_rom_decompression "/path/to/Hybrid Heaven (USA).z64" hybridheaven.z64
```

### Android ✅ runs on hardware

`android/` is a Gradle project that cross-builds the same tree with the NDK and
packages it as an APK. The main target becomes `libHybridHeaven.so` rather than
an executable, because SDLActivity loads a library and calls `SDL_main` inside
it.

**Verified on a device 2026-08-01: a Retroid Pocket 5 (Android 13, Snapdragon
865).** It boots, renders on Adreno/Vulkan (`[gfx] window 1920x1080 ... via
Android`), plays the intro screens correctly and reaches the battle system, with
no Android-specific change needed beyond what is described below. Not yet
measured on hardware: sustained framerate, audio by ear, and the physical
controls.

Installing and driving it from this tree uses **Windows' `adb.exe`**, not WSL's,
so stage files on a Windows path first:

```bash
ADB="$LOCALAPPDATA/Microsoft/WinGet/Packages/Google.PlatformTools_*/platform-tools/adb.exe"
cp android/app/build/outputs/apk/debug/app-debug.apk /mnt/c/Users/Public/
"$ADB" install -r 'C:\Users\Public\app-debug.apk'
"$ADB" exec-out screencap -p > shot.png      # captures the game's Vulkan surface
```

Two device states that look exactly like a broken build and are not: **an empty
`adb devices` while the handheld is plainly attached** (a stale adb server —
`kill-server`, `start-server`, and check `Get-PnpDevice` for
`USB\VID_18D1&PID_4EE7` before believing it), and **a black `screencap` with taps
doing nothing** (the screen is dozing — `dumpsys power | grep mWakefulness`, then
`input keyevent KEYCODE_WAKEUP`).

**Saves are portable between platforms** — the desktop `saves/hybridheaven.us.bin`
drops straight into the app's `files/data/saves/`. Push it with the game
**stopped**: librecomp reads the save at boot and writes its in-memory copy back
on exit, so a push under a live session is silently overwritten.

```bash
c++ -std=c++17 -O2 -o build-host-tools/file_to_c_host \
    lib/rt64/src/tools/file_to_c/file_to_c.cpp   # once
cd android && ./gradlew :app:assembleDebug       # -> app/build/outputs/apk/debug/
```

Needs an SDK with NDK 27.1.12297006 and platform 34; `android/local.properties`
points at it and is per-clone. Only `arm64-v8a` is built.

#### The custom Vulkan driver

<a id="the-custom-vulkan-driver"></a>

**Settings → GPU Driver** lets a player run the game on a Vulkan driver they
supply — a Turnip build, on Adreno — instead of the device's own. It is per-app
and needs no root: nothing on the device is modified. This exists because the
system driver is sometimes the thing that is broken, and on that hardware there
is otherwise no way past it.

Off by default. One gradle flag turns it on:

```bash
git submodule update --init --recursive lib/libadrenotools   # note --recursive
cd android && ./gradlew :app:assembleDebug -PcustomDriver=true
```

**`--recursive` is not optional here.** `lib/libadrenotools` is the only
submodule in this tree that has a submodule of its own (`linkernsbypass`), and a
plain `--init` leaves it empty — the failure is then a CMake error from inside
`add_subdirectory(lib/linkernsbypass)`, which does not say "you forgot
`--recursive`". A build without `-PcustomDriver=true` never enters that
directory and so never notices.

**That single flag deliberately drives four things**, because any of them alone
is broken:

- `-DHH_CUSTOM_VULKAN_DRIVER=ON`, which builds `lib/libadrenotools` and compiles
  the loader half of `src/main/android_glue.cpp` rather than its no-op half.
- `PLUME_CUSTOM_VULKAN_LOADER` on the `plume` target, which is what makes plume
  take a host-supplied `vkGetInstanceProcAddr` instead of calling
  `volkInitialize()`. A build without the option compiles the original statement
  unchanged, so the normal Vulkan init path is untouched.
- `jniLibs.useLegacyPackaging = true`, so the four hook libraries exist as **real
  files** in `nativeLibraryDir`. libadrenotools requires that; AGP 8's default
  reads `.so` straight out of the APK and leaves that directory without them, and
  the loader then returns a valid pointer and silently falls back to the system
  driver.
- `BuildConfig.CUSTOM_VULKAN_DRIVER`, which is what hides the settings tab in an
  ordinary build. A control that silently does nothing is worse than a missing
  one.

Most of this arrived with the launcher: the RML tab, `src/ui/ui_gpu_driver.cpp`,
`GpuDriverStore.java`, `SafFiles.java`, the JNI glue and the gradle flag were all
transplanted then, and `lib/rt64` was already on a `goemon-android` commit that
descends from the host-supplied-loader change. What was missing was the part that
makes any of it run: the `lib/libadrenotools` submodule, the CMake option that
builds and links it, and the call that reports which device the renderer actually
came up on.

**The device name is the only authority on whether a custom driver is in use.**
Loading one succeeds even when the system driver ends up being used, so the
loader's own result cannot answer the question. `RT64Context`'s constructor calls
`hybridheaven::report_render_device()` once Vulkan is up and the driver settings
show that name, which is what makes "our bug or their driver?" answerable in a
bug report.

Recovery is the hard part, and it is why the feature is not just a file picker. A
driver that cannot run the game takes the process down before any in-game UI
exists, so a bad pick would otherwise be an unbreakable crash loop. A **boot
latch** is written before launch and cleared only when the user confirms the
picture, or — for an already-confirmed driver — after the renderer has kept going
a while; finding it still set next launch means the attempt died, and the driver
is deselected. Reporting the device deliberately does **not** clear it: reaching
that point only means Vulkan initialised, which is earlier than the Adreno fault
this feature exists for, and clearing there would disarm the latch about a second
before the crash it is meant to catch.

Verified without a device: the flag-on APK carries `libmain_hook.so`,
`libfile_redirect_hook.so`, `libgsl_alloc_hook.so` and `libhook_impl.so`, and
`libHybridHeaven.so` defines `adrenotools_open_libvulkan` and
`plume::SetCustomVulkanLoader`; the flag-off APK has neither the hook libraries
nor those symbols, and still builds and links.

**Confirmed running on the Retroid Pocket 5 (2026-08-01).** The flag-on APK
installs, launches and reaches the launcher, and the loader path logs both of its
outcomes on the device:

```
custom driver: none selected, using the system Vulkan driver
custom driver: renderer came up on 'Adreno (TM) 650'
```

The second line is `report_render_device()`, so the device-name reporting this
feature depends on works end to end. All four hook libraries land as real files in
`nativeLibraryDir`, which is what `libadrenotools` requires and what
`useLegacyPackaging` is there to guarantee.

**What is still unverified: an actual replacement driver.** No Turnip build has
been imported or selected, so nothing here claims a third-party driver renders the
game — only that the machinery to load one is present, runs, and correctly reports
that the system driver is currently in use.

Two things the cross-build needs that the desktop one does not:

- **Host tools.** `file_to_c` and DXC run on the build machine and emit C and
  SPIR-V, so cross-building them would produce arm64 binaries the build cannot
  execute. Gradle passes the prebuilt one as `-DRT64_FILE_TO_C`, and both CMake
  and Gradle fail early with the command to build it rather than part way in.
- **The codegen contract.** `RecompiledFuncs/` and `RecompiledPatches/` are
  gitignored and the NDK build cannot regenerate them — the patch codegen needs
  `make`, `clang` and `ld.lld` as host tools. So a Linux build has to have
  populated them first. CMake checks this up front, because `RecompiledFuncs/` is
  a glob that otherwise just comes back empty and links a `.so` with no game in
  it and no diagnostic anywhere.

Whether the patches actually overrode is a link-time property, exactly as on
Windows. Checking it needs no device:

```bash
llvm-nm -S libHybridHeaven.so | grep func_8000469C_529C
```

The size must match `PatchesLib`'s definition (`0x3c4`), not `RecompiledFuncs`'
(`0x5dc`). It does.

Two things that bit while writing the entry layer:

- **`entrypoint_address` must be sign-extended.** Every `MEM_*` macro subtracts
  `0xFFFFFFFF80000000`, so a plain `0x80000400` lands ~4GB into rdram and faults
  on the first DMA in `recomp::init`. Use the generated
  `get_entrypoint_address()` from `RecompiledFuncs/lookup.cpp` — which is exactly
  why Goemon declares it rather than writing the address out.
- **The three libraries are mutually recursive**, so they are linked inside
  `$<LINK_GROUP:RESCAN,...>` (`--start-group`). No ordering resolves them.

`rsp/aspMain.cpp` needs SSSE3 (`-march=nehalem`), as Goemon's whole executable
does; without it `_mm_shuffle_epi8` fails to inline.

**The renderer is on by default.** `-DHH_RT64=OFF` builds
`src/main/null_render_context.cpp` instead of the RT64 one, which is the right
thing to bisect against — it runs everything below the display list without
standing up RT64, so a failure that survives it is not a renderer bug.

rt64 builds in-tree and needs nothing installed: no Vulkan SDK and no external
shader compiler, because it vendors a DXC that compiles its HLSL to SPIR-V at
build time. Two things it does need from this side — `SDL_WINDOW_VULKAN` on the
window, without which plume cannot make a surface and RT64's setup reports no
graphics API, and `SDL_VULKAN_ENABLED` set as a **CACHE** variable, because plume
asks for CMake 3.12 and a normal variable will not reach it. A full build from
scratch is about 790 targets.

`HH_N64RECOMP` points at the **patched** recompiler (see below); it defaults to
`./N64Recomp`. The build regenerates `RecompiledPatches/` from `patches/` via
`make` and the recompiler, and embeds `patches.bin` with
`cmake/embed_binary.cmake` — done in CMake rather than with rt64's `file_to_c`
so the libraries can be built without configuring rt64.

`RecompiledFuncs/` is *not* generated by this build; run the recompiler first,
as described under "Regenerating".

### The Windows build

Cross-compiled **from WSL**, with no Visual Studio involvement beyond its headers
and import libraries:

```
mkdir -p build-host-tools
c++ -std=c++17 -O2 -o build-host-tools/file_to_c_host lib/rt64/src/tools/file_to_c/file_to_c.cpp
cmake -S . -B build-win -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-windows-clang-cl.cmake \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DRT64_FILE_TO_C=$PWD/build-host-tools/file_to_c_host
ninja -C build-win HybridHeavenRecompiled
```

**`RT64_FILE_TO_C` must point somewhere durable, and `/tmp` is not.** It is
recorded in `build-win/CMakeCache.txt` and consulted on every later build, so a
path under `/tmp` — or worse, a per-session scratch directory — leaves the tree
looking configured while the next build dies with `missing and no known rule to
make it` on a shader. `build-host-tools/` is inside the repo and covered by the
`build*/` gitignore rule.

It produces `build-win/HybridHeavenRecompiled.exe` with `SDL2.dll`, `dxil.dll` and
`dxcompiler.dll` beside it, and **the `.exe` runs straight from WSL** through
binfmt interop — as an ordinary Windows process, which is the point: it reports
`via windows` rather than `via x11` and gets the vendor's real Vulkan driver
instead of the software rasteriser WSL is otherwise limited to.

Ubuntu's `clang` package supplies `clang-cl`, `lld-link` and `llvm-rc`; the MSVC
toolset and Windows SDK are read over `/mnt/c`, whose case-insensitivity is what
lets `<Windows.h>` resolve. The toolchain file expects two symlinks, and says so
if they are missing:

```
ln -sfn '/mnt/c/Program Files (x86)/Microsoft Visual Studio/<VS>/<edition>/VC/Tools/MSVC/<version>' ~/.local/win/vctools
ln -sfn '/mnt/c/Program Files (x86)/Windows Kits/10' ~/.local/win/sdk
```

It **plays** as of 2026-07-28: 180 seconds, no crash, on the vendor's own Vulkan
driver against an RTX 5080, at **0.895** consumed in gameplay with nothing dropped
— against 0.779 on llvmpipe and 0.701 on `dzn`. Getting there took the four fixes
below, and before them it had never run the game at all: it aborted every time
with exit code `0xC0000409` and no output whatsoever.

To run it you still need, by hand: the validated ROM in
`%LOCALAPPDATA%\HybridHeavenRecompiled` (**LocalAppData** — `config.cpp` uses
`FOLDERID_LocalAppData`, so `%APPDATA%` is the wrong directory and
`HH_AUTOSTART` just reports no validated ROM) and `"api_option": "Vulkan"` in
`graphics.json`. **`assets/` is no longer among them**: the build copies it beside
the `.exe` along with `recompcontrollerdb.txt` and the three DLLs, so
`build-win/HybridHeavenRecompiled.exe` runs where it is built. That copy is an
`ALL` custom target, **not** a `POST_BUILD` command — it was the latter until
2026-07-28, and because `POST_BUILD` only fires when the target is *relinked*, an
asset-only edit (a `.rml`, a stylesheet, a font) produced `ninja: no work to do`
and left the **old** file beside the `.exe` while the source showed the change.
If you ever edit an asset and the running build seems to ignore it, `diff` the
source against `build-win/assets/...` before believing the edit was wrong. Hand-copying it was
worth removing because the failure when it is absent misleads — RmlUi reports one
missing stylesheet and then the gfx thread faults inside `init_styling`, which
reads as a renderer bug.

Nine things are worth knowing before touching any of it:

- **clang-cl, never `cl.exe`.** The configure step refuses MSVC outright.
  `recomp.h` picks `RECOMP_FUNC` by compiler, and its MSVC branch is a bare
  `__declspec(noinline)` with no weak linkage — but the patch override works
  *because* the definitions are weak and the linker may prefer PatchesLib's. Two
  strong definitions turn that into whichever archive the linker reaches first,
  which is exactly the failure that once left this port running with no patches
  applied at all. A loud configure error beats a silently patch-less binary.
- **Weak is not enough either: `/WHOLEARCHIVE:PatchesLib` is what applies the
  patches on COFF.** With clang-cl and the libraries in the correct order, the
  build still shipped the *unpatched* `func_8000469C_529C` — from
  `RecompiledFuncs/funcs_293.c`, which `llvm-pdbutil dump -publics` plus
  `llvm-symbolizer` names outright. `weak` becomes a select-any COMDAT and the
  linker keeps whichever copy it **sees** first, not whichever library it reaches
  first; an archive member is only seen once something pulls it in, and
  RecompiledFuncs packs ~50 functions per object, so `funcs_293.c.obj` arrives on
  account of one of its other 49 and the patch's COMDAT is discarded as the
  duplicate. Reordering cannot fix a demand-driven pull. `/WHOLEARCHIVE` adds
  every member of PatchesLib as if it had been named on the command line, which is
  before any archive is searched. The symptom was
  `Failed to find function at 0x80107830` and **zero** overlay loads under
  `HH_TRACE_OVERLAYS=1` where Linux prints 50 — `recomp_load_overlays` never ran.
- **D3D12 cannot be used, because the shaders are unsigned.** `api_option: Auto`
  picks D3D12 on Windows and aborts before it can even name a device: `dxc-linux`
  emits DXIL and cannot sign it, and a D3D12 device is the only thing that checks.
  Set `"api_option": "Vulkan"`. Vulkan takes the same SPIR-V it takes on Linux and
  reports `Using device "NVIDIA GeForce RTX 5080"`.
- **RT64's own `Device Name:` line goes to stdout**, which is block-buffered and
  discarded when the process aborts — the port's `[plume]` lines go to stderr and
  survive. "RT64 prints no device line on Windows" was that, and nothing more.
- **Host tools are chosen by the host, not the target.** DXC and `file_to_c` both
  run during the build and neither ships. Selecting them by target picks
  `dxc.exe`, which fails with exit 126 (the checkout has no executable bit) and,
  once that is fixed, cannot open the Linux absolute paths the build passes it.
  `dxc-linux` emits DXIL perfectly well — it just cannot *sign* it, which only a
  D3D12 device would care about at runtime.
- **`/FIintrin.h` is load-bearing.** Every SDL2 here — rt64's vendored 2.26.3 and
  the 2.30.3 the Windows branch fetches — defines `_m_prefetch` itself, working
  around a Clang 11 bug its own comment calls temporary. Modern clang has it as a
  builtin and defining a builtin is a hard error. Force-including `<intrin.h>`
  lets clang's own header set the `__PRFCHWINTRIN_H` guard first so SDL skips its
  copy. Defining that macro directly also works and is worse: it gags clang's
  real header too.
- **`NOMINMAX` is set at the top level**, not just inside rt64.
  `add_compile_definitions` is directory-scoped, so rt64's own copy never reaches
  this project's targets — and `windows.h`'s `min`/`max` macros break hlslpp,
  which the render context includes.
- **The patch codegen does not run on Windows.** It needs `make`, `clang` and
  `ld.lld` to link a MIPS ELF. Windows consumes `RecompiledPatches/` as static
  sources, the same treatment Android gets, and fails the configure with an
  explanation if they are absent. Since both generated trees are gitignored,
  generate on Linux first and build Windows against the same tree.
- **`timeout` does not kill it.** The signal reaches the Linux interop wrapper,
  not the Windows process, which outlives the harness — three orphans accumulated
  before this was noticed. Stop it from the Windows side instead:
  `powershell.exe -NoProfile -Command "Stop-Process -Name HybridHeavenRecompiled -Force"`,
  and `Get-Process ... | Select-Object Id,Responding,MainWindowTitle` is how to
  ask whether it is alive and its window is up without looking at the screen.
- **Assets resolve against the `.exe`, not the working directory.**
  `get_program_path()` returned `""` here, which is survivable on Linux because
  runs start from the repository root. On Windows it meant a `.exe` started from
  anywhere else found no `assets/` — and the port died inside
  `recompui::init_styling`, which resized a `std::string` by `tellg()` on a stream
  that never opened. `tellg()` is -1, `resize` takes a `size_t`, and 2^64-1 throws
  `std::length_error` on the gfx thread with nothing to catch it. Both are fixed;
  the `tellg()` half was never Windows-specific.
- **Reading a Windows crash needs its own handler, and it is now in `main.cpp`.**
  `install_crash_handler()` was a no-op off Linux, so an abort printed nothing at
  all. It now installs `std::set_terminate`, a `SIGABRT` handler and
  `SetUnhandledExceptionFilter`, and prints frames as RVAs. Two traps in reading
  the result: **MSVC's `set_terminate` is per-thread**, so an uncaught throw on the
  gfx thread never reaches it and `SIGABRT` is the hook that fires; and
  `CaptureStackBackTrace` returns *return* addresses, so a frame must be
  symbolized at `addr - 1` or it names the following function — which is exactly
  how the stylesheet bug first read as a fault inside `Application::setup`.
  Symbolize with the PDB beside the `.exe`:

  ```
  llvm-symbolizer --obj=build-win/HybridHeavenRecompiled.exe $((0x140000000 + RVA - 1))
  ```

  `0x140000000` is the PE `ImageBase`; `llvm-readobj --file-headers` prints it.

## The libultra naming gap

The first attempt to compile `RecompiledFuncs/` found that 18 of 316 files call
`_recomp` symbols N64ModernRuntime does not define — `__osSiGetAccess_recomp`,
`__osViInit_recomp`, `__osSetSR_recomp` and 14 more. 62 distinct `_recomp` symbols
were called; the runtime defined 143, another 40 were `renamed_funcs` that the
output satisfies itself, and 17 were left over.

That last distinction matters, and it cost a wrong count first time round. A
`_recomp` call is not necessarily a call into the runtime: N64Recomp's third
list, `renamed_funcs`, covers `bzero`, `memcpy`, `sqrtf` and friends, which are
still recompiled — just under a `_recomp` name so they do not collide with the
host libc. `bzero_recomp` looks missing and is defined in
`RecompiledFuncs/funcs_31.c`. The tool checks the output's own `funcs.h` for
exactly this reason.

N64Recomp does not recompile a function named in its `ignored_funcs` or
`reimplemented_funcs` lists; where one is *called* it emits
`<name>_recomp(rdram, ctx)` and expects the runtime to define it. Not every
ignored name has an implementation. The gap opens when the decomp still has a
libultra function under a `func_XXXXXXXX` placeholder that calls a named
internal:

```
game code  ->  func_80032FB0_33BB0   unnamed, so recompiled as game code
                   -> __osSiGetAccess   named, so not recompiled -- needs a host impl
```

**The build error is the lesser problem.** An unnamed libultra function is
recompiled and *executed*, so the port runs the game's own hardware drivers —
poking SI, VI and PI registers librecomp does not emulate — instead of the
runtime's native implementation. Goemon64Recomp never hits this because it names
the public libultra API, which leaves the internals unreachable.

```bash
uv run python tools/recomp_symbol_gap.py            # the gap; exits nonzero
uv run python tools/recomp_symbol_gap.py --verbose  # every caller
```

**Twelve of the fourteen are now named** in
`lib/hybridheaven/config/usa/symbol_addrs/symbol_addrs.txt`, each identified from
its own instructions and justified there — `osInitialize`, `osStopThread`,
`osCreateViManager` + `viMgrMain`, `osCreatePiManager` + `__osDevMgrMain`,
`osContStartReadData`, `osPfsInitPak`, `__osMotorAccess`, `__osSiRawReadIo`,
`__osSiRawWriteIo`, `__osSpRawStartDma`. Both ROM hashes still match. The gap
went from 17 symbols to 3, and all 316 files compile.

### Do not read the compile log as the gap

Six `_recomp` names came up as "implicit declaration of function". Three of them
— `__osPiGetAccess`, `__osPiRelAccess`, `osPfsIsPlug` — the runtime defines
perfectly well; they are simply not *declared* anywhere the recompiled C can see.
GCC 14 promotes implicit declarations to errors, so Goemon's
`-Wno-implicit-function-declaration` is no longer sufficient; build the output
with `-Wno-error=implicit-function-declaration` and the real gap appears at link
time instead. Use `recomp_symbol_gap.py`, which distinguishes the two.

### The last three, and how they were closed

The gap now reports 0, but only two of the three were closed properly.
`src/game/ultra_missing.cpp` holds both answers:

- **`__d_to_ull`** — genuinely called from game code at 3 sites, so naming could
  never have helped. Implemented for real, from the game's own copy at vram
  `0x80034AB8`: round toward zero, saturate to all-ones on overflow or a
  negative input, and return o32-style with `$v0` holding the high word and
  `$v1` the low, each sign-extended.
- **`__osSiGetAccess` / `__osSiRelAccess`** — **not implemented, on purpose.**
  They stop the program with a message. Both are only reachable through
  `__osContRamRead` (`0x80034060`) and `__osContRamWrite` (`0x80033E10`), which
  are identified but deliberately still unnamed: about 14 unnamed `osPfs`
  functions call them, so naming the pair alone would only trade these two
  missing symbols for `__osContRamRead_recomp` and `__osContRamWrite_recomp`.

  That was staging, and **the family is now named**, so the stubs are no longer
  reached. They stay in place as a tripwire: if anything ever calls them again,
  something below the public API has become live and that is worth knowing
  loudly.

### The osPfs closure — named as a unit ✅

`librecomp/src/pak.cpp` reimplements the twelve public `osPfs*` entry points and
provides **no SI-level support at all**, so everything beneath the public API is
meant never to run. Naming a subset only moves the boundary, which is why these
went in together.

The closure was computed, not guessed. Walking the call graph *upward* from the
only two functions that actually reach the SI stubs — `func_80034060_34C60`
(`__osContRamRead`) and `func_80033E10_34A10` (`__osContRamWrite`) — gives
exactly **23** unnamed libultra functions, matching the number carried since
Phase D. Of those 23, exactly **ten** have game-code callers, and librecomp
provides exactly **ten** public `osPfs*` entry points not already named
(`osPfsInitPak` and `osPfsIsPlug` were named earlier). That one-to-one fit is
itself part of the evidence.

| vram | name | what identifies it |
|---|---|---|
| `0x8002A350` | `osPfsAllocateFile` | 7 args, `andi $a1, 0xFFFF` types a1 as the u16 company_code; the only entry point that calls two others (`osPfsFindFile`, `osPfsFreeBlocks`) |
| `0x800332C0` | `osPfsChecker` | 1 arg, 0x4C8 frame, 334 instructions — the largest; its only caller is `osPfsInitPak`, which Quest64's `pfsinitPFSPaks.c` confirms calls it |
| `0x80031BC0` | `osPfsDeleteFile` | 5 args (fifth read from `0x180($sp)` past a 0x170 frame), u16 company_code, calls `osPfsFindFile` |
| `0x80032DD0` | `osPfsFileState` | 3 args; a1 is a file number bounds-checked against `pfs->0x50` |
| `0x8002EE40` | `osPfsFindFile` | the one entry point called by *both* AllocateFile and DeleteFile |
| `0x80026450` | `osPfsFreeBlocks` | 2 args, loads `pfs->status` first; called by AllocateFile |
| `0x80027D04` | `osPfsInit` | stores mq into `pfs->0x4`, channel into `pfs->0x8`, clears status, then `__osPfsSelectBank(pfs, 0xFE)` |
| `0x80032AB0` | `osPfsNumFiles` | 3 args, and it writes through *both* pointer arguments at the end |
| `0x80029784` | `osPfsReadWriteFile` | 6 args: file number, `andi $a2, 0xFF` u8 flag, a3 offset, two more from `0x180/0x184($sp)` |
| `0x800308D0` | `osPfsRepairId` | 22 instructions; tests status against `PFS_INITIALIZED\|PFS_ID_BROKEN`, repairs, clears `0x4` |
| `0x80031FF0` | `osMotorInit` | not osPfs but in the same closure — fills a 32-byte buffer with `0xFE` and writes it, libultra's block-detect probe |

Everything *below* the public API stays unnamed on purpose — `__osContRamRead`,
`__osContRamWrite`, `__osPfsSelectBank`, `__osPfsRWInode` and the rest have no
host implementation, so naming them would re-open the symbol gap. Once these
eleven are named nothing reaches them. The gap tool still reports **0 MISSING**,
which is the check that this was the right cut.

Neither mnsg nor Quest64-Decomp could supply the names by fingerprinting — the
best structural match across the 23 peaked at 0.24. They were read out of the
disassembly, using argument signatures as the primary discriminator and
Quest64's `src/libultra/io/pfs*.c` **call graphs** (not its bytes) as
corroboration.

### The measured remainder

Booting the port added three more names — `osGetMemSize`, `osCartRomInit` and
`osViSetSpecialFeatures`. Running it past the loader added five more, each found
by reading the function it stopped in: `osEPiStartDma`, `osSetEventMesg`,
`osContGetReadData`, `__osDisableInt` and `osAiSetFrequency`. **187 libultra
functions are now named, and 25 are still called from game code.**

Two of those five are worth singling out, because they are the ones that show
"touches hardware registers" is the wrong test on its own:

- **`osEPiStartDma`** produced the project's first *hang* rather than a crash.
  The game's DMA wrapper fills an `OSIoMesg`, calls it, then blocks in
  `osRecvMesg`. Unnamed, it queued the request onto the game's own PI command
  queue, which librecomp's native PI manager does not service, so the completion
  never arrived. It touches no registers at all.
- **`osSetEventMesg`** produced a segfault *inside ultramodern*. Its
  `osContStartReadData` ignores the queue argument and posts to
  `events_context.si.mq`, which only `osSetEventMesg(OS_EVENT_SI, ...)` ever
  sets — and its `osContInit` does not set it. Unnamed, the game wrote its own
  `__osEventStateTab` instead, `si.mq` stayed null, and `do_send` dereferenced it.

So the dangerous class is **anything that manipulates state the native runtime
also owns** — queues, event tables, SI buffers — not just anything that pokes
`0xA4xxxxxx`. Exactly one of the remainder touched hardware registers
(`osAiSetFrequency`, now named); the rest are computation or runtime-owned state.

**With the osPfs closure named too, 198 libultra functions are named and only 7
are still called from game code.** Two of those seven account for 18 of the call
sites and were read: both are pure floating-point polynomial evaluation over
double-precision coefficient tables, so they are genuinely harmless and naming
them would buy nothing.

A caution on that number, since this project has published wrong ones before.
It uses the **strict** definition of "game code" — a caller that is still a
`func_XXXXXXXX` placeholder *outside* the libultra text range. A looser sweep
that also counts named libultra and libaudio callers (`alFxPull`, `viMgrMain`,
`alSynNew`) gives a larger figure, and that looser definition is where the
inherited "30" came from. Both are computed from the call graph; they answer
different questions.

The test for whether naming a given function helps is simple and worth stating:
**name it when the runtime provides `<name>_recomp`, not merely because the
recompiler would skip it.** Four unnamed functions touch PI registers
(`func_8002B2B0_2BEB0`, `func_80030C60_31860`, `func_80030E10_31A10`,
`func_80034400_35000` — the raw DMA and IO primitives) and are deliberately left
alone: they are `ignored` but have no host implementation, so naming them would
re-open the gap, and they are dead now that `osCreatePiManager` and
`__osDevMgrMain` are native.

Identify from the instructions, not from the symptom — `lib/hybridheaven/PLAN.md`
Phase C records what symptom-level guessing cost there. Note also that mnsg
cannot supply these by fingerprinting: it has no `osStopThread`, and its
`osInitialize`, `osCreateViManager` and `osPfsInitPak` differ in size from Hybrid
Heaven's, so the two games do not link an identical libultra throughout.

Goemon64Recomp's `src/` is ~18.5k lines, but that number oversells the work.
It splits into `src/main/` (2.5k, 8 files — the entry layer, and the part that
genuinely has to be rederived), `src/game/` (3.9k — game-specific, mostly does
not transfer) and `src/ui/` (12.2k, 62 files — menus and config screens, almost
entirely Goemon content that is reusable or droppable rather than rewritten).

## The audio path — four bugs, three host and one game

> **The whole of this section describes the path now selected by
> `HH_AUDIO_LEGACY=1`.** As of 2026-07-28 the default is upstream's audio path —
> the one Zelda64Recomp, Goemon64Recomp and Quest64-Recomp all share verbatim —
> minus its overflow policy. See "Why this port stopped having its own audio
> path" below. The bugs and reasoning here are still worth reading: they are how
> the units, the channel swap and the report cap were arrived at, and all of that
> carried over.

Naming `osAiSetFrequency` got the game as far as pushing real audio buffers, and
each buffer then hit a different bug.

### Why this port stopped having its own audio path

Three shipped N64Recomp ports share one audio implementation, essentially
character-for-character — Quest64-Recomp's `get_frames_remaining` is identical to
Goemon64Recomp's, comment and all, and both match Zelda64Recomp. This port had
written its own, and measurement said the bespoke one was broken in a specific
way: **it pinned the game's own audio rate control at its floor.**

Hybrid Heaven sizes each audio task as `992 - remaining`, clamped up to a floor
of 720 (`target = 736` at `0x80096340`, `floor = 720` at `0x8009633C`, both read
out of `.main_bss`). That is a controller. It needs to see a backlog under 272 to
generate anything above the minimum, and this port's honest report never was —
across 958 traced tasks it returned 720 or the `0x4000` cap, and the game
generated **709–722 frames every single time, in both phases**. Production
reduced to `~712 x tasks-per-second` with no feedback at all, which is why the
figure 0.980 kept appearing: it is `720 x 60 / 44095`, not drift.

Four changes, three adopted and one rejected:

- **one device at 48 kHz, opened once**, with the game's rate resampled into it
  via `SDL_ConvertAudio`, rather than reopening SDL at the game's 44,095 Hz;
- **a 256-frame device buffer** (`samples = 0x100`), not 1024. The game's setpoint
  is ~257 frames, so a 1024-frame buffer is coarser than the entire range it
  regulates over — the loop cannot settle;
- **report one VI's worth less than is queued.** Upstream's comment names this
  exact case: it "prevents audio popping on games that use the buffered audio byte
  count to determine how many samples to generate";
- **decimation on overflow — REJECTED, and this is the important one.** Upstream
  sheds a long queue by keeping 1 sample in 2^n with no anti-alias filter. Ported
  faithfully at its 100 ms threshold it shed **3,439,310 frames** across three
  runs and delivered 0.712. Worse than the number: unfiltered decimation folds
  everything above the new Nyquist back down as **new content**, and it is plainly
  audible — reported from the room as "additional sounds that are not supposed to
  be there". Nothing in this port's instrumentation could have caught it, because
  every statistic here accounts for *frames* and the fault was *spectral*.
  Retuning the threshold was the wrong instinct; the mechanism does not belong in
  the signal path. It is off by default and `HH_AUDIO_SKIP_MS=<ms>` restores it
  (100 = upstream). Overflow is bounded by refusing a chunk instead: a hole, which
  is worse and honest. **A latency valve must not invent audio.**

Measured, three runs per arm, gameplay windows only:

| | consumed | worst window | stdev | empty-queue | dropped |
|---|---|---|---|---|---|
| this port's own path (`HH_AUDIO_LEGACY=1`) | 0.905 | 0.502 | 0.133 | 19% | 0 |
| upstream, decimation off (default) | **0.955** | **0.660** | **0.069** | **13%** | 0 |

Two hypotheses died before this one worked, recorded so nobody retries them: the
VI offset **alone**, on the old path, was marginal on the mean and made starvation
*worse* (19% → 28%); and requeueing the AI messages ultramodern drops when the
game's queue is full did nothing whatsoever (0.901 against 0.905 with matched
instrumentation, audio task rate 50.8/s → 51.3/s), so those messages were never
being lost in quantity. Note the second of those was measured wrong the first
time, because only one arm had `HH_TRACE_RSP=1` — that tracing costs about 8% of
the task rate on its own. **Match the instrumentation across an A/B or the
instrument is the effect.**

**The host bug: `queue_samples` had its units wrong.** ultramodern's
`queue_audio_buffer` passes `byte_count / sizeof(int16_t)` — the **total** count
of `int16` samples across both channels, not the per-channel frame count. This
port's callback multiplied by 2 again, so it handed SDL twice the length that
existed and `SDL_QueueAudio`'s memcpy ran off the end of rdram. The crash landed
inside `libSDL2`, which is a long way from the mistake.

Note the two audio callbacks genuinely use different units:
`get_frames_remaining` really is frames (ultramodern multiplies its return by
`2 * sizeof(int16_t)` to get bytes), while `queue_samples` really is samples.
That one was already correct and is easy to "fix" into being wrong.

The same callback also now swaps adjacent samples. N64Recomp stores rdram with an
address xor for endianness, so the two 16-bit halves of each word come out
transposed — for interleaved stereo that is exactly left and right.
Goemon64Recomp corrects the same thing in its own `queue_samples`.

**The second host bug: `get_frames_remaining` had no ceiling.** This one caused
*both* of the port's remaining failures, and it took the longest to see because
the symptom was three layers away from the cause.

On hardware `osAiGetLength` reports the length of the DMA the AI is currently
playing -- one buffer, a couple of thousand samples. It physically cannot report
a second of backlog. `SDL_GetQueuedAudioSize` can, and does after any stall.

Hybrid Heaven sizes each audio frame in `func_8001FD14_20914`:

```
t8 = D_80096340 - osAiGetLength()/4      # target 736, minus the backlog
t9 = t8 + 0x100
sh (t9 & 0xFFF0)                          # kept as an s16
slt at, a3, D_8009633C                    # clamp UP to a floor of 720
```

The clamp is a **floor, not a ceiling** -- it is the `sltu`→`slt` patch below,
and that patch is correct as far as it goes. But once the backlog passes 33760
frames, `992 - backlog` wraps inside that s16 to a large *positive* count, which
no clamp catches, and the game synthesises ~32700 samples in a single frame.

Both failures follow from that one number:

- the audio command list is a `0xA000` buffer at `0x800BF9F0` with the **OSTask
  immediately after it**. 32700 samples fills the buffer exactly -- the dump
  showed 5120 valid commands, then the microcode reading the OSTask as commands
  (`800350D0` is opcode `0x80`, which indexes past the jump table). The task's
  own `data_size` said `0x3F990`, four times the buffer, against a healthy range
  of `0x4D8`-`0x2C48`;
- the same frame exhausts the audio DMA free list, and the game's DMA callback
  `func_8001FEFC_20AFC` reads `lw $t1, 0x0($s0)` with `$s0` null. That is the
  SIGSEGV at guest address `0x00000000`, under `alAdpcmPull` / `_decodeChunk`.

The fix is a cap on what the backlog is allowed to look like, in
`get_frames_remaining`. It costs nothing in behaviour: any backlog past the
game's target — the constant is **736**, or 992 once the `0x100` bias above is
added, and both numbers appear in these notes — already produces the same
"generate the minimum"
response, so every value above the cap was already indistinguishable -- the cap
only keeps the number inside the s16 the game stores it in. Goemon64Recomp's
version of this callback is more elaborate than this port's and is also
unbounded, so it has the same exposure.

**The game bug: the `osAiGetLength` underflow, which Goemon also has.**
`lib/hybridheaven/PLAN.md` predicted this one would have to be found rather than
copied, because the target is a game audio function rather than libultra. It is
`func_8001FD14_20914`, and it turned out to be the *same instruction in the same
registers* as Goemon's — `sltu $at, $a3, $v1` at vram `0x8001FD8C`, encoding
`0x00E3082B`, fixed to `slt` (`0x00E3082A`) by an instruction patch in
`hybridheaven.toml`.

The mechanism: `target - osAiGetLength()/4` underflows, the result round-trips
through `sh`/`lh` so it comes back **sign-extended and negative**, and the
unsigned compare then reads it as a huge number and skips the clamp. The negative
length reaches `osAiSetNextBuffer`, and ultramodern halves it into a ~2^31 sample
read. Measured directly before the fix: six good buffers of 1440–1984 samples,
then one of 2147480224.

**The fourth: the cap protected the game's arithmetic and left the real queue
unmanaged.** `get_frames_remaining` caps what the game is shown at 0x4000 frames,
which is what stops the s16 wrap above. It does nothing to SDL's queue, and the
game cannot drain that itself — it has no ceiling on its own frame size and a
floor of 720, so once the backlog is past the cap every response it makes is
identical and the latency never comes back. `queue_samples` now drops the backlog
at the same boundary. That boundary is the one place where discarding audio
cannot change what the game does, because above it the game was already being
told the same number.

Running it turned a precaution into a finding. **The saturation is not
occasional — it is the steady state.** 32 drops in a 90-second run, roughly one
every three seconds, with the queue pinned about 16× above the game's own
992-frame target throughout. The game has therefore been generating its 720-frame
minimum on every audio frame for the whole run and *still* outproducing the
device by roughly a fifth.

### The paragraph above is wrong in both directions ✅ — and the cap was the cause

Everything above was measured on runs that never got past the "Memory Pak
Enhanced" screen, because until the display-list guard the port could not. Past
it, three things are false: the port does not queue silence, the game does not
outproduce the device, and the drain was not a cheap precaution.

Note what this does and does not explain. It explains why the old numbers do not
describe gameplay. It does **not** explain what the old runs were measuring — a
queue pinned at ~16k with a drop every three seconds is not reproduced by the new
boot-screen data either (queue 0–1824, ratio 0.980). Pre-guard, the runaway
display list was stalling the frame loop, which plausibly changed audio pacing;
but that is a hypothesis. The old observation was **superseded, not explained**,
and given that this section reached three wrong conclusions from statistics that
were each individually true, the distinction is worth keeping.

**The game is *behind*, not ahead.** Once it is synthesising, mean production is
**0.88–0.90** of the device rate on six cores (0.882 and 0.899 in two runs) and
**0.96** on twelve, and the queue sat at 0 in most reporting windows — SDL
running dry, not backed up. Production is 0.98 during the boot screens, when
there is nothing to synthesise, and falls to ~0.6 exactly when real music starts,
so the shortfall is throughput in the audio path, not arithmetic.

**And the cap was throwing away a quarter of the audio.** `SDL_ClearQueuedAudio`
discarded the *whole* backlog — ~16,700 frames a time — and at `0x4000` ordinary
jitter reached it **about once every 1.6 seconds** across the producing span
(72 and 77 clears over 118 s in the two runs). It is bursty rather than steady:
a quarter of the 2-second windows saw none at all, most saw one or two, and the
worst saw four. An earlier draft of this section said "twice a second," which
described only those worst windows — at ~16,700 frames a clear, a true 2 Hz rate
would have been ~75% of the device rate, not the ~28% measured.

Over a 150-second run that is **1.21M frames in one run and 1.29M in a second** —
about a quarter of everything the game synthesised — deleted from a stream that
was already short. Worse, the deletion is what caused the silence: bursts got
truncated, so the following trough had nothing banked to draw on.

The giveaway was that **doubling the cores changed nothing audible.** Twelve
cores produced more audio than six (0.96 against 0.88–0.90) and dropped more of
it (1.64M against 1.21–1.29M); delivered audio was 0.65 either way. When more CPU
buys zero improvement, CPU is not the binding constraint.

**Sharing one constant between two unrelated jobs is what did it.**
`max_backlog_frames` bounded both what `get_frames_remaining` *reports* and how
much SDL is allowed to *hold*. Only the first is constrained by the game — it
keeps `target - osAiGetLength()/4` in an s16, so the reported figure must stay
well under 33760. Nothing about SDL's queue follows from that. They are now
separate: the report cap stays `0x4000`, the queue cap is
`audio_queue_cap_frames()`, default `0x10000`, tunable with
`HH_AUDIO_QUEUE_CAP`.

Measured A/B, same binary, 150 s each, averaged over the windows past t=32 s where
the game is actually producing:

| cores | cap | produced | **consumed — what the device took** | frames dropped | peak queue |
|---|---|---|---|---|---|
| 6 | `0x4000` | 0.882 | 0.652 | 1,205,664 | 17088, i.e. at the cap |
| 6 | `0x4000` | 0.899 | 0.649 | 1,286,464 | 16976, at the cap |
| 12 | `0x4000` | — | ~0.64 | 1,636,640 | at the cap |
| 6 | `0x10000` | 0.921 | 0.920 | **0** | 45056, drains to 0 |
| **12** | **`0x10000`** | 0.963 | **0.964** | **0** | 44016, drains to 0 |

The two `0x4000` six-core rows are independent runs of the same configuration —
worth listing separately, because an earlier draft quoted one run's dropped count
against the other's ratio as though they were one measurement.

Peak queue reads slightly *above* the cap (17088 against 16384) because the check
runs before the enqueue: the queue is allowed to sit one ~700-frame chunk over
until the next callback notices. That is arithmetic, not a leak.

The two factors are independent and the cap is much the larger. With the cap
fixed, `dropped` is 0 and the queue drains to 0 within every window — and once
those two hold, `consumed = produced - dropped - Δqueue` makes `consumed ≈
produced` an accounting identity rather than an independent confirmation. The
measured facts are the zero and the drain; the remaining gap is a 3.6% production
shortfall, and the core count only moves that.

A producer averaging below the device rate cannot grow a queue without bound, so
the cap never needed to be tight — bursts drain themselves. It only has to catch
a genuine device stall, and at `0x4000` it was catching ordinary jitter instead.

**`consumed` is the column that settles this, and it did not exist before.**
`peak` answers "is it silent". `ratio` answers "does the game keep up". Neither
answers "does the audio reach the speakers", and this section reached three
different wrong conclusions before that number was added. Everything produced
either left through the device, was dropped by the cap, or is still queued, so
`consumed = produced - dropped - Δqueue` closes the books; it is now reported
unconditionally under `HH_TRACE_AUDIO`.

### The cap sheds one chunk now, instead of demolishing the buffer ✅

Raising the cap fixed the symptom but left the mechanism: at the overflow site the
code still called `SDL_ClearQueuedAudio`, discarding the **entire** backlog, and
then enqueued the new chunk anyway. At `0x10000` that is ~1.5 s of audio destroyed
per trip — a strictly worse artifact than the ~0.4 s holes it replaced, and at
higher latency. The cap and the clear were written in the same change, so how much
of the damage was the clear and how much was the low cap was never separable.

`queue_samples` now refuses the incoming chunk instead — ~700 frames, about 16 ms
— and leaves the backlog intact. The cap becomes a latency ceiling the stream
leans against, shedding excess a chunk at a time until production falls back below
the device rate, rather than a trigger that empties the buffer.

At the default cap this path never fires under llvmpipe: production averages
0.92–0.97, so the queue drains itself and `0x10000` is hit **zero** times in 150 s.
That makes the change unfalsifiable at the default — so `HH_AUDIO_CLEAR_ON_OVERFLOW`
exists to restore the old policy on the same binary, and the comparison was run at
`0x4000`, where the path fires constantly. All four runs below are one binary,
12 cores, no `taskset`, 150 s, averaged over the windows past t=32 s:

| cap | discard policy | produced | **consumed** | frames dropped | peak queue |
|---|---|---|---|---|---|
| `0x4000` | clear whole backlog | 0.934 | 0.674 | 1,340,704 | 17072 |
| `0x4000` | refuse new chunk | 0.955 | **0.742** | 1,098,720 | 17088 |
| `0x8000` | refuse new chunk | 0.938 | **0.936** | 17,280 | 33216 |
| `0x10000` | refuse new chunk | 0.969 | **0.970** | **0** | 44432 |

**Refusing the chunk wins at the same cap** — 0.742 against 0.674, shedding a third
fewer frames. Note that its trip *count* is far higher (thousands of refusals
against ~70 clears): at a cap this low the queue parks against the ceiling and
refuses chunk after chunk instead of emptying and refilling. Compare frames shed,
never trip counts.

**`0x8000` is the interesting row.** Its `consumed` 0.936 is within 0.002 of its own
`produced` 0.938 — the cap cost 17,280 frames, about **0.3%** of production — for
**half** the worst-case latency of `0x10000` (peak 33216 frames against 44432). The
0.936-vs-0.970 gap between those two rows is almost entirely run-to-run production
variance, not the cap. On this evidence `0x8000` is the better default; what it
needs is an ear, not another log, and both are one env var apart.

One caveat is genuine: for a real *device* stall, clear-all resynchronised to
fresh audio immediately, where this replays up to a capful of stale audio first.
That trade is deliberate — overproduction is the case that recurs on faster
hardware, a stalled device is already broken.

**Confirmed by ear ✅.** A 150-second run of the shipped configuration — refuse-the-
chunk at the default `0x10000` — was reported as sounding *very good*. That is the
first listen aimed at **smoothness** rather than mere audibility, and it is what the
`consumed` column could not supply: the numbers said every frame reaches the device,
and the ear says the result is not merely present but clean. The latency the cap buys
is not audibly objectionable at ~1.5 s.

That reframes the remaining tuning. `0x8000` is no longer a fix for anything — it is
an optional refinement worth ~0.75 s of lag at a cost of 0.3% of frames, against a
default already judged good. Worth trying, but the burden is now on it to be
*better*, not on `0x10000` to be acceptable:

```
HH_AUDIO_QUEUE_CAP=0x8000 ./build/HybridHeavenRecompiled hybridheaven.z64   # ~0.75 s
```

The principled version is still to derive the cap from the burst distribution above
— a quarter of windows quiet, worst case four trips in two seconds — rather than
picking a round number over the high-water mark. On a real GPU that distribution
changes completely, so deriving it here may not be worth the effort.

Genuinely still open: the residual ~0.03–0.06 shortfall is real underproduction, and
it is mostly not CPU (0.920 on six cores → 0.970 on twelve).

And all of it is llvmpipe-conditioned: the burstiness driving this is a software
rasteriser's frame-time jitter. On a real GPU, where production can sustain ≥ 1.0,
both the cap and the discard policy should be re-measured — that is the case the
refuse-the-chunk change was made for, and the case this machine cannot produce.

## The launcher

`src/ui/` is Goemon64Recomp's RmlUi interface, brought over whole: the launcher
(Select ROM / Start Game / Controls / Settings / Mods / Exit), the config menus,
the input remapper, the mod menu and installer, and the scripting API a mod would
build its own UI with. About 10,500 lines of C++, plus `assets/` and two shaders.

**Goemon64Recomp is the base, and that is not a preference.** `lib/rt64` here is
the *same commit* on the same `goemon-android` branch Goemon uses, and
`lib/N64ModernRuntime`'s `hybridheaven-port` branch is a direct descendant of
Goemon's `goemon-android` (three commits ahead). Quest64-Recomp has the same UI
but sits on different branches of both submodules, so taking it as the base would
have meant reconciling two runtime forks as well as the UI.

### The artwork, and the silent failure behind it

The launcher shows a full-width backdrop (`assets/launcher_background.png`,
credited above), and the window and the .exe both carry the app icon. Three
things about how, because none of them is guessable from the markup:

- **The UI renderer never loads a texture from disk.** `LoadTexture` looks its
  source string up in a map of images registered as *bytes*
  (`recompui::queue_image_from_bytes_file`), so the art is read from `assets/` in
  `register_launcher_background` and registered **before** the document is
  created — an `<img>` resolves its source the first time it is laid out.
- **A missed lookup returns `true`.** The old code substituted a 1x1 transparent
  texture and said nothing, so a wrong name, or bytes registered a moment too
  late, drew as *nothing at all* and looked exactly like a styling mistake. It now
  logs the missing name once. The registration side logs one line too, so
  "the launcher looks plain" can be split into its two causes without a rebuild.
  Both were confirmed by pointing the `<img>` at a name that does not exist: the
  warning fires, which is what makes its silence in a normal run mean something.
- **The `?/` prefix is load-bearing.** It stops RmlUi resolving the name as a file
  path relative to the document. The mod menu's thumbnails use the same trick.

Styling lives in `launcher.rml`'s own `<style>` block rather than in
`assets/recomp.rcss`, because that file is **generated** from `assets/scss` by a
sass step that only runs under Windows' npm — an edit there is overwritten by the
next build and never reaches the source.

#### Making the background match the launcher's black

The shipped file is **16:9, and the painting is 3.1:1** — the difference is the
fix for a seam. Fitted to the width, the painting left a band with `.launcher`'s
`#0D0A07` above and below it, and the two blacks visibly disagreed: the
painting's top and bottom rows are a dark **blue**, about `rgb(10,11,30)`, while
`#0D0A07` is a warm near-black. Matching them with a flat colour is not possible
either — the painting's edge is pure black at the far left and right and blue
through the middle.

So the transition is baked into the image: its own edge rows are extended
outward, then faded into `#0D0A07`, leaving the file's outermost rows exactly the
launcher's colour and no seam at any window shape. From the artist's original
(3840x1240, 16-bit, 16 MB — it lives outside the repo; the shipped 8-bit file is
1.1 MB):

```sh
magick "launcher background.png" -depth 8 -resize 1920x -strip art.png
# Extend the edge rows outward. Smooth them horizontally first: stretching a
# single row as-is leaves visible vertical streaks.
magick art.png -crop 1920x1+0+0   +repage -resize 240x1\! -resize 1920x230\! ext_top.png
magick art.png -crop 1920x1+0+619 +repage -resize 240x1\! -resize 1920x230\! ext_bot.png
magick ext_top.png art.png ext_bot.png -append full.png
# Fade the outer 230px of each end into the launcher's own colour.
magick -size 1920x230 gradient:white-black m_top.png
magick -size 1920x620 xc:black             m_mid.png
magick -size 1920x230 gradient:black-white m_bot.png
magick m_top.png m_mid.png m_bot.png -append mask.png
magick -size 1920x1080 xc:'#0D0A07' mask.png -alpha off -compose CopyOpacity -composite pad.png
magick full.png pad.png -compose over -composite -strip assets/launcher_background.png
```

If `.launcher`'s `background-color` ever changes, the `xc:'#0D0A07'` above has to
change with it or the seam comes back.

The two icons are separate mechanisms and both are needed. `SDL_SetWindowIcon`
(`set_window_icon` in `main.cpp`, decoding `assets/icon.png` with rt64's stb)
covers the running window, the taskbar and Alt-Tab; the icon Explorer shows for
the **file** can only come from a compiled-in Windows resource, `icons/app.rc`
with a multi-size `app.ico`. That needs `enable_language(RC)`, since `project()`
lists only CXX and C, and it compiles under the cross-build with the `llvm-rc`
that ships beside clang-cl.

### What it costs, measured

The UI is not free on this machine, and the figure is worth having before anyone
re-measures the audio and finds it moved:

| build | window | mean `consumed`, whole run | mean `consumed`, once the game makes sound |
|---|---|---|---|
| `-DHH_UI=OFF` | 640x480 | 0.895 | 0.864 |
| default | 640x480 | 0.857 | 0.811 |
| default | 1280x720 | — | 0.786 |

Two 120-second `HH_AUTOSTART` runs per row, same binary tree, `HH_TRACE_AUDIO`
averaged per 2-second window; the right-hand column filters to windows with
`peak > 1000`, i.e. after the game starts synthesising.

**The launcher costs about 4 points of audio delivery; the window size costs 2.**
The window was the first suspect and it was largely wrong — worth stating, because
this project has a habit of them. The cost is *not* menu compositing: RmlUi's
`Update()`/`Render()` are already gated behind `is_any_context_shown()`, so
nothing draws while you play. It is the per-frame input and event layer —
`recomp::poll_inputs`, `handle_events`, the focus/primary-input bookkeeping, and
`update_rumble` on every VI — which is the price of remappable bindings and a
gamepad, and it is llvmpipe-conditioned like everything else here. Do not read the
0.92 recorded above under "The audio path" as contradicted: that was measured on
what is now the `-DHH_UI=OFF` build.

### Proving it drew, without looking at it

"Every font loaded and RmlUi logged no errors" is a **weaker claim than it
sounds** — a document can load, lay out to nothing, and report nothing wrong. And
this machine cannot settle it by looking: screenshots do not work from the guest,
and the window has a history of being hard to find.

So the port measures it. `HH_TRACE_UI=1` reads the geometry counters the render
interface keeps and reports them once a second:

```
[gfx] window 1280x720 at 1280,720 on display 0 via x11 (usable 3840x2160 at 0,0)
[ui] a context is SHOWN, game not started -- 660 batches/s, 20155 vertices/s
```

~660 batches and ~20,150 vertices per second is ~11 batches a frame at 60, steady
— RmlUi resolved a layout, shaped text into vertices and handed them over. That is
the last link host code owns; RT64 and the compositor own what happens after.
`RenderGeometry` increments the counters, `recompui::debug_take_geometry_counts`
reads and zeroes them.

**And the window is not off-screen, which had been assumed.** It reports 1280x720
at 1280,720 on a 3840x2160 desktop — exactly centred — and crucially **`via x11`**,
so that is real geometry from XWayland rather than a guess. (Under `wayland` a
client is never told its own position, so SDL would answer with what it last
requested and the claim would be unfalsifiable from inside the process. Always
read the driver before trusting the coordinates.) The off-screen branch exists and
correctly stayed quiet; if a window ever does land with no overlap at all, it is
flagged `OFF-SCREEN` and moved to the centre.

The stronger proof is behavioural, though: a run with **no `HH_AUTOSTART`** reached
the game, and the only path to `recomp::start_game` in that build is the
launcher's Start Game button. So the launcher drew, was legible enough to pick the
right item from, and its button works.

### What did not come across, and why

Every Goemon setting whose only reader was a Goemon *patch* arrived **absent, not
disabled** — targeting mode, the analog camera and its four settings, autosave,
the three cheat toggles, swap/attack-while-moving, and the Debug (warp) tab. A
control that changes nothing is worse than a missing one. They come back one at a
time, each with the patch that reads it: **the analog camera and its four
settings are back** (see [Analog camera](#analog-camera-)), and so is
**Autosave** (see [Autosave](#autosave-️-implemented-not-yet-play-verified)). The
rest are still absent.

Also gone: separate BGM and SE volume, which Goemon splits inside the game's own
mixer by patch. **Main volume is here and does work**, applied by the port in
`queue_samples` — it rides the channel swap that happens anyway, and at 100 the
multiply is skipped entirely so the default path stays bit-identical to the audio
that was tuned by ear.

Kept but dormant: the **mod UI scripting API** (`ui_api_events.cpp`). A mod can
queue UI callbacks, but nothing dispatches them — Goemon ticks them by calling
`recomp_run_ui_callbacks()` from a per-frame patch, and adding a per-frame patch
here is Phase E work, not launcher work. Also kept: the **GPU Driver** tab, which
hides itself off Android (`driver_supported` is false). That is no longer a
placeholder — the loader behind it is here, see
[the custom Vulkan driver](#the-custom-vulkan-driver) under Android.

Texture packs are removed rather than dormant: the mod menu's special-case for the
HD-texture-pack config option is gone, because this port registers neither the
texture-pack content type nor the `.rtz` container, so no loaded mod could ever
present that option.

There is **no cover art and no app icon.** Goemon's belong to that project. The
launcher's layout keeps the slot — an `<svg>` in a `.launcher__background-wrapper`
in `assets/launcher.rml` — and it is empty.

### The one trap when trimming this

`ui_config.cpp`'s Debug menu made the single
`constructor.RegisterArray<std::vector<std::string>>()` call, and RmlUi's type
register is **per-Context, not per-model** — so the GPU Driver menu's own
`vector<string>` binding depended on the Debug menu having run. Deleting the
Debug tab without moving that call would have broken the driver menu on Android
only, long after the change. It now lives in `make_nav_help_bindings`, the first
model built.

## Controller input

**The whole input layer came over with the launcher and was complete; one line of
SDL initialisation kept all of it unreachable.** `src/game/input.cpp` (1163
lines), `src/game/controls.cpp` and `assets/config_menu/controls.rml` are
Goemon's, wired into `ultramodern::input::callbacks_t` in `main()` exactly as
Goemon wires them — `poll_input`, `get_input`, `set_rumble`,
`get_connected_device_info`, plus `vi_callback = recomp::update_rumble`. The
remapper works, the bindings persist to `controls.json`, and rumble ramps.

What was missing: `main()` called `SDL_Init(SDL_INIT_AUDIO)` where Goemon's
`create_gfx` calls `SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER |
SDL_INIT_JOYSTICK)`. SDL emits `SDL_CONTROLLERDEVICEADDED` — and refreshes pad
state inside `SDL_PumpEvents` — only while the joystick subsystem is up, so no
pad ever opened, every controller binding read as unpressed, and the port was
keyboard-only however many controllers were plugged in. Nothing logged anything,
because from the port's side nothing had happened.

Now set, all before `SDL_Init` because SDL reads them at init and open time:

| | |
|---|---|
| `SDL_INIT_GAMECONTROLLER \| SDL_INIT_JOYSTICK` | the fix. `-DHH_UI=OFF` still asks for `AUDIO` alone — its `get_input` reads the keyboard and nothing else, so pads there would only cost HIDAPI's scan thread in the build whose job is measuring the game |
| `GAMECONTROLLER_USE_BUTTON_LABELS=0` | buttons reported positionally, so `SDL_CONTROLLER_BUTTON_A` is always the south face button and a `controls.json` means the same thing on every pad |
| `JOYSTICK_HIDAPI_PS4_RUMBLE=1`, `..._PS5_RUMBLE=1` | DualShock 4 / DualSense rumble needs SDL to drive the extended HIDAPI report |

`JOYSTICK_ALLOW_BACKGROUND_EVENTS` is deliberately *not* among them: SDL re-reads
that one dynamically and `ui_state.cpp`'s `apply_background_input_mode` drives it
from the config.

`recompcontrollerdb.txt` is Goemon's SDL_GameControllerDB subset, loaded from
`get_program_path()` — the repository root on Linux, beside the `.exe` on Windows,
where the build now copies it along with `assets/`. A missing db does not stop a
pad working; it makes *some* pads work wrongly, which is the harder thing to
diagnose, so the connect handler prints the pad's name, GUID, button/axis counts
and the mapping SDL actually chose. Those prints moved from stdout to **stderr**,
because runs here end with `timeout -k` and stdout's block buffer goes with the
process.

Startup now says so in one line, which is also the check that the subsystem is up:

```
[input] 17 controller mappings from recompcontrollerdb.txt, 0 joystick(s) attached
```

### What the game actually reads

`func_800021B4_2DB4` calls `osContStartReadData`/`osContGetReadData` and builds a
0x20-byte record per port at `D_80089474`:

| offset | |
|---|---|
| `+0x2` | raw button word |
| `+0x4` | its newly-pressed edge |
| `+0x6` / `+0x8` | stick X / Y |
| `+0xA` | a direction word **synthesised from the stick**, at ±0x28/0x29 out of 84, reusing the N64 D-pad bit values |
| `+0xC` | its newly-pressed edge |

Scanning every `andi` against a D-pad bit in the disassembly and attributing it to
the last load into that register: **45 sites read `+0xC`, and not one reads a
D-pad bit out of `+0x2` or `+0x4`.** The hardware D-pad is dead input in this
game — so `.dpad_*` is left unbound in the defaults for a reason that is Hybrid
Heaven's own, and a physical D-pad bound there would do nothing. It drives the
C-buttons instead. The same scan over the raw word gives the buttons the game
does read: **A, B, Z, Start, L, R and all four C-buttons.**

The one thing this implies for play: **directions come only from the left stick.**
A pad's D-pad cannot move the player or a menu cursor in-game (the port's *own*
menus are unaffected — `ui_state.cpp` reads the physical D-pad straight from SDL).

### Verifying it

`0 joystick(s) attached` is what WSL reports: it passes no USB gamepad through, so
the Linux build here cannot be tested against real hardware, and the Windows
`.exe` — which sees the host's pads — is where a controller test belongs. It now
runs straight out of `build-win/` with no hand-copying, since `assets/` is copied
beside it.

## Analog camera ✅

**Right-stick free look.** Options → General → **Analog Camera**, default **Off**.
Per-axis invert and sensitivity under it. **RB + right stick** zooms (up in, down
out, 40–250% of the room's own distance). **R3** hands the camera back to the game.

Full reverse engineering: [`docs/hybridheaven-camera-re.md`](hybridheaven-camera-re.md).
The implementation is `patches/camera.c`; the host side is thin wrappers over an
input layer that already had everything (right stick, deadzone, R3, suppression).

### It follows the game's own camera modes (2026-07-30)

**The camera is yours only where the game would have let you move it anyway.**

Hybrid Heaven has an explicit camera state machine — `D_80163740_542E50`, a mode in
−1..8, dispatched once a frame and mirrored to `gState+0x29A` — and the C-button
camera is gated on it. `patches/camera.c` applies the game's own C-left rule
(`scene gate == 0 && mirror == 2`) and, when it fails, releases **both** the camera
and the right stick, so the feature is simply absent there.

| mode | what it is | analog camera |
|---|---|---|
| 0 | cutscenes, elevators, transitions | released |
| **2** | **exploration** | **yours** |
| 3 | battle | released |
| 5 | ladder / climbing | released |

**Holding R releases it too** — R fires the gun, and the game frames that shot
itself.

This replaced overriding the camera unconditionally in every situation, which is
what made the feature feel like it was fighting the game. Verified over 16.8
minutes of play: **zero engagement outside mode 2**, across 8.7 minutes of battle
and 3.5 of cutscene, and 308 R-held samples with none engaged.

**Framing distance** is now the game's too. The camera goal turns out *not* to be
where the game's camera settles — the game eases an eye *position* toward a goal
that keeps moving with the player and trails it, while this patch eased a *scalar
radius* and converged exactly onto it, framing at 0.83× the game's distance
("the camera comes too close behind the character"). It now scales by a gain
measured from the live/goal ratio, sampled only on frames the camera is *not* ours
— which is what keeps it clear of the feedback loop that once walked the camera
into the player. Framing measured at ~1.12× afterwards.

A/B: `HH_CAM_NO_MODE_GATE=1`, `HH_CAM_NO_AIM_RELEASE=1`,
`HH_CAM_NO_FRAMING_GAIN=1`, `HH_CAM_LEGACY_RADIUS_EASE=1`. Trace the game's own
camera machine with `HH_TRACE_CAMMODE=1`.

> Much of the machinery documented below — the staleness test, both trust
> thresholds, the occlusion dwell — exists to survive readings taken in modes the
> camera now refuses to run in. It is still carried, and should be measured inside
> mode 2 and deleted if it never fires.

### Why this game makes it cheap

**Hybrid Heaven stores no camera yaw anywhere.** The camera *is* one parameter
block — `(*(void**)0x801BBCD8)->0x2C`, holding eye, look-at, up and fovy as floats
— and every consumer recomputes its angles from it. The one that matters is
`func_8011A878_4F9F88`, which rebuilds the player's **movement basis** from
`at - eye` every frame. So moving the eye is the whole feature: walking follows the
view natively, with no counter-rotation of the left stick and no basis rework.
Quest 64 has that property; Goemon 64 does not, which is why its equivalent patch
is 836 lines.

The hook is `func_80119F9C_4F96AC`, the shared yaw/pitch getter. It has ~30 call
sites and runs ~30 times a frame, which forces two design choices: the rotation is
an **absolute override** rather than an offset (so it is idempotent at any call
rate), and the accumulator advances on **wall-clock dt** rather than per call.

### Three quantities, kept apart

This is the part that took six iterations to get right, and every one of the
failures below came from collapsing two of them together.

| | |
|---|---|
| **normal** | what the room frames at. Tracks the game's goal distance, rising at 2.0/s and falling at 0.15/s, so a doorway cannot redefine it |
| **want** | `normal × zoom` — what the player asked for |
| **limit** | a *cap*, not a distance. When the game pulls its own goal well below normal, something is in the way, and the camera may not be further out than that **regardless of zoom** |

Plus the **anchor**: while the camera is ours, the look-at is pulled onto
`player.y + 15` — the offset the game's own orbit camera uses. Without it the orbit
circles whatever point a room chose to frame, and rooms with fixed camera angles
(elevator shafts, corridor shots) read as the camera having stopped tracking you,
because it has.

### The game's own numbers are not all instructions

Three separate bugs were the same mistake: taking a value the game maintains and
treating it as authoritative.

- **The camera goal distance is stale in every mode that sets the look-at directly
  and never writes the goal fields** (`func_801154A4_4F4BB4` is one). Measured
  **561** on a ladder and **630** in attract mode, against a real 35–50. Following
  it pinned the base at its ceiling and, multiplied by a zoom the player had set,
  put the camera 561 units out with the character off screen. A live goal always
  aims within 14–19 of the player, so `|goal_at − player|` is the test; the
  threshold is deliberately loose at 150, because wrongly calling a *live* goal
  stale costs the door fix, which is worse.
- **The occlusion pull-in is temporary.** The game drops its goal to **17** to clear
  a doorway (35 clear) — that *is* its occlusion handling, and following it is what
  fixed doors. Adopting it as the framing distance is what broke them again: the
  camera came out of the doorway still tight, and a player who zoomed out to
  compensate pushed it straight back behind the door, because zoom was multiplying
  the number holding the camera clear.
- **The look-at is not the anchor** — see above.

### The other three

- **The horizon rolled over.** The integrator back-solves camera roll at frame
  start against the stored up vector, then rebuilds the up vector from it at frame
  end. Rotating between those two points leaves `sin(pitch) × Δyaw` of residue,
  which the game faithfully rebuilds as real roll, compounding every frame.
  Invisible with the camera level; 90° over a few seconds while turning *and*
  pitched. Fixed by reporting roll 0 while engaged. A/B: `HH_CAM_NO_ROLL_FIX=1`
  → 0° → −24° → −67° → −112° → −156°.
- **The camera walked into the player.** Reading the radius off the *live* eye feeds
  the rotation into itself: the game eases the eye toward a goal at another azimuth,
  we reset the azimuth, and the sideways part of that pull returns as a
  *shortening*. Found sitting exactly on a 0.5× clamp floor — 24.6 against a
  take-over radius of 49.49. Fixed by never reading the radius back from the eye.
- **The pitch limits were `0x0F00`, commented "67 degrees".** A *quarter* turn is
  `0x800` here, so that was 169° — the camera walked past vertical and hung upside
  down. Now ±`0x600`, which is 67.5° and also exactly what the game's own orbit
  camera clamps the same quantity to.

### Verifying a gamepad feature with no gamepad

WSL passes no pad through, so every path past the engage threshold is unreachable
on the Linux build. `HH_CAM_FORCE_X`, `HH_CAM_FORCE_Y` and `HH_CAM_FORCE_ZOOM` pin
the stick and the zoom modifier so those paths can be driven anyway, and
`HH_TRACE_CAM=1` narrates what happened. **Two bugs were found this way and by
nothing else** — the 169° pitch limit, and an inverted zoom axis on the first
build. The trace also carries a round-trip check on the patch's own arithmetic
(decompose the eye offset with the game's atan2, rebuild it with the game's
sin/cos, report the error): **0.000 at radius 70**, which is what proves the angle
conventions agree rather than merely compile.

### Buttons, and one that cost a weapon

Zoom went on the right trigger first, matching Goemon. **The right trigger is N64
R, and R fires the gun** — the mask that stopped zooming from also pressing R cost
a weapon. It is on **RB** now, whose N64 meaning is C-down, whose in-game job is
the game's own close-look camera. That is the button worth spending: the feature
replaces it. C-down is masked while the camera is engaged (a 250 ms lease, renewed
per step, so it returns by itself), which also means D-pad-down loses C-down while
engaged; the other three C-buttons still work off the D-pad.

The game's own camera control is untouched and still works: hold **C-left** to
orbit, **C-down** for the close look, both driving the camera from the *left* stick
— which is the limitation this removes. With the setting off, nothing in
`patches/camera.c` runs.

### A dead end worth not repeating

`func_801084C4_4E7BD4` is the game's own camera raycast and the obvious way to keep
the line of sight clear. Its convention is known (arg1 in `$f12`, arg2 in `$f14`,
args 3–6 in `a2`/`a3`/stack, read off the asm at `0x801104DC`) and **it returns 0
for every ray fired from this hook**. Note before repeating the test: the two
wrappers install *different* surface filters — `func_8010843C_4E7B4C` collides with
everything, `func_801084C4_4E7BD4` filters against `D_80163570_542C80` — so "it
cannot even hit a floor" proves nothing. It is moot regardless: the goal distance
already encodes the game's own occlusion decision, and is a better source than a
second opinion that could disagree with it.

### Open

**The floor makes the camera pan out** (reported 2026-07-29, undiagnosed). Suspect
either the `normal` tracker rising as the game's goal grows when the camera meets
the ground, or the anchor lowering the look-at. `HH_TRACE_CAM=1` and read
`distance N (room normal M, zoom P%)`: `room normal` climbing is the tracker,
`distance` above it is zoom or the cap.

Not yet judged: whether `normal`'s 0.15/s fall is slow enough for long tight
corridors, and whether anchoring hijacks scripted shots — it overrides the look-at
in every situation while engaged, cutscenes included. `HH_CAM_NO_ANCHOR=1` reverts.


## Autosave ✅

**Commits your progress through the game's own save routine**, to whichever slot
the save menu last selected. Options → General → **Autosave**, default **Off**.
Manual trigger **L + R + Z**; a 2-minute timer as well.

Full rundown: [`docs/autosave.md`](autosave.md). The implementation is
`patches/autosave.c`; the `.manual.bak` rollback point is
`src/game/save_rollback.cpp`.

### It calls the game's save code rather than reimplementing it

This is the one structural difference from Goemon64Recomp's autosave, and it is
the whole design. Goemon reimplements its game's save routine because that
routine sits in an overlay that is not resident during gameplay. Here it does not
have to: **`.file_7` IS the gameplay engine** — gState, the camera state machine,
the player code and the whole save/load layer, all in one resident overlay that
`patches/camera.c` already runs inside every frame.

So the patch calls `func_80141628_520D38(device, slot)` and then
`func_80141568_520C78(device)` — the body of the game's own
`func_80141268_520978`, minus its final 616-instruction save-menu message UI,
which is the one part that must not run over live gameplay. A resulting save is
not "indistinguishable from" a real one by argument; it **is** one.

### The gate is the camera-mode gate, already measured on hardware

Rather than write a new safe-state predicate from the disassembly, it reuses the
conjunction the analog camera hangs off (`scene gate == 0 && gState+0x29A == 2`).
That gate was measured over a 19-minute session: **mode 2 during exploration
accepted every time, mode 0 during cutscenes and elevators refused every time,
mode 3 for the whole of four battles.** Cutscenes, elevators and combat are
therefore excluded without this feature enumerating any of them.

### The marshal is not idempotent, and that decided the settled check

The obvious settled check is to run the game's marshal into a scratch buffer and
hash the 0xD00 payload it produces. That is unavailable: the bulk serializer
`func_80144C40_524350` opens by folding elapsed-since-last-save counters into
their totals and resetting them, so running it to *look* would bank counters with
no save behind them. The check hashes the marshal's **sources** instead — five
ranges, and `docs/autosave.md` states plainly which parts of the payload they do
not cover.

### What is proven

A 175-second `HH_AUTOSTART` run with the setting forced On produced exactly one
line, and it is load-bearing five ways at once:

```
[autosave] interval elapsed, suppressed: unsafe state (cam mode 0 | last change mask 0x01)
```

The poll is reached, the setting is read, the settle hashing runs and
discriminates (`0x01` — one range moved, four did not), and the 2-minute
wall-clock timer fires and re-arms. `HH_TRACE_CAMMODE=1` on the same run gave
7,368 `[cammode]` lines, all `MODE 0` — so the hook runs continuously and
attract-screen mode 0 is the honest reason for the refusal.

### Then proven on hardware, and two bugs it found

**Device-verified 2026-08-01 on a Retroid Pocket 5**: manual combo and the
2-minute timer both commit, and a resulting save **loads through the game's own
load menu**. The pak was pulled and diffed rather than trusted — one autosave
changed 16 bytes (two directory, fourteen slot) and the slot's **checksum
recomputed valid**, which a stale echo or torn write cannot do. `.manual.bak`
stayed on the last deliberate save throughout.

Playing it found two bugs no automated run could:

- **The settled check refused every save.** Range A was a contiguous
  gState `+0x00..+0x20` chosen as a "safe superset" of the marshal's fields. It
  contained `+0x0D`, which changes every frame and which the marshal never reads
  — one byte of collateral pinned the stability window at 0 forever. Now it
  hashes the marshal's exact field list; measured stability went from 0 ms to
  42,758 ms against a 167 ms requirement.
- **A wrong signature segfaulted the game.** `func_80141568_520C78` reads only
  `$a0` in its own body, so it looks like it takes one argument. It does not —
  `$a1` passes through untouched to the directory builder, which dereferenced
  garbage. *A function's signature is not what its own body reads, it is what its
  callees read.*


## Widescreen ✅

**The game is scissoring itself, and that was the whole of it.** Setting
`ar_option` to Expand is all widescreen takes in Goemon64Recomp, and this port
had that setting, applied it correctly, and still drew a 4:3 picture with black
bars inside a 16:9 window.

The measurements, in the order that made each next one obvious:

| what was checked | result |
|---|---|
| Is the setting on? | The **Windows** config had `"ar_option": "Expand"` all along. The Linux one said `Original` — a red herring, and `from_or_default` only fills in a default for an *absent* key |
| Is RT64 widening the target? | Yes. `resolutionScale 4.000 x 3.000` — **a ratio scale of 1.333**, exactly 16:9 from 4:3. `HH_TRACE_ASPECT=1` reports it |
| Is RT64 widening the *projection*? | **No.** `adjust=0` on every projection across a 90-second run, in both video modes |
| Why not? | `projScissor=[64..1216]` inside `fbScissor=[0..1280]` — the game clips its own 3D view to **the middle 90%** |

RT64's `G_EX_ASPECT_AUTO` heuristic (`ProjectionProcessor::processScene`) widens
a projection only if it reaches **both** edges of the framebuffer, because one
that does not is usually a sub-panel — a minimap, a split-screen viewport.
Hybrid Heaven's never does. Goemon needs no patch because its 3D view does reach
the edges; the mechanism transfers, the premise behind it does not.

The insets are static and were found in the ROM, matching the runtime figures
exactly:

| ROM | rect | |
|---|---|---|
| `0x6CCA60` | (16,8)-(304,232) | 288x224 of 320x240 |
| `0x5EF2D8`, `0x6CCAE0` | (32,16)-(608,464) | 576x448 of 640x480, the **high-resolution video mode** the game's own Resolution setting selects |
| `0x6CCB60`, `0x5EF338` | (32,90)-(608,390) | the game's **cinematic letterbox** — bars that are content, not overscan |
| `0x04E1B0` | (0,0)-(320,240) | full frame, boot/RDP init |

### The fix, and why it is where it is

`src/main/widescreen.cpp` rewrites those scissor commands in rdram to the full
framebuffer, from `send_dl` — where this port already inspects every display list
before RT64 sees it. **Quest 64 has the identical defect**, and
`Quest64-Recomp/patches/widescreen.c` calls it "the authentic overscan border";
that file is the reference to read before changing anything here. It does the
rewriting from a game patch hooked into a per-frame function, which this port has
no equivalent of — doing it on the host instead also means the aspect setting can
be read directly, so the `recomp_get_target_aspect_ratio` export that Goemon and
Quest64 both carry is the one piece of their widescreen scaffolding this port
does not need.

Three things worth keeping straight:

- **Both axes are widened, and the first version only widened x.** The reasoning
  for x-only — 16:9 adds horizontal field of view and none vertical — was wrong,
  and a screenshot settled it in one look: the game draws nothing in the strips
  its 3.3% vertical inset hides, so they showed as two black bars across an
  otherwise correct picture. The projection already covers the full framebuffer
  height, so what the inset was hiding is correctly rasterised scene.
- **The cinematic letterbox is matched separately and keeps its vertical crop.**
  Matching on x alone would have flattened a cutscene's framing. That is why the
  table matches all four bounds rather than just the two that get widened.
- **Quest 64's "dominant defect" is not a defect here.** It had to widen the
  per-frame colour clear as well, because only the cleared region composes as
  fresh content. Hybrid Heaven already clears the whole frame — `(0,0)-(639,479)`
  right after a full-frame scissor, then narrows the scissor for the 3D. The
  fill-rectangle path is kept because it costs one command byte in the same scan.

**RT64 needs no patch for any of this.** An earlier version of this fix added a
tolerance to that
`coversWholeWidth` test inside the fork; it was reverted once the scissor rewrite
made it unnecessary, and the measurement is the reason it could be: with the
rewrite in place and RT64 completely stock, the heuristic's own comparison goes
from `inter=[128..2432] fb=[0..2560] -> adjust=0` to `inter=[0..2560]
fb=[0..2560] -> adjust=1`, in both video modes. The fork does now carry one
rendering patch, but it is not this one and it is not about the scissor — see
"The logo screens" below.

`HH_NO_WIDESCREEN_SCISSOR=1` is the A/B; `HH_TRACE_WIDESCREEN=1` reports every
safe-rect-shaped command in rdram, matched or not, which is how a rect this table
is missing shows up as a line rather than as silence.

### The transition bug — an offset is not where the rect is (fixed 2026-08-01)

**Reported:** between screens, only the middle 4:3 transitions; the border around
it stays black and static until the new screen arrives. Reported for room
changes, menus and title screens alike — "every transition I've seen".

The first version of this file cached the *rdram offset* of every safe rect it
found, rewrote those offsets each frame, and once anything was cached rescanned
only every 256th frame. The safe rect is emitted after however many commands the
current screen puts in front of it, so **its offset moves whenever the screen's
content does** — measured at `dl+0xA0`, `dl+0xB8` and `dl+0x140` across a single
boot. A transition changes that content by definition, the cached offsets stop
matching, and the live list goes out with the game's own 4:3 scissor until the
next full scan, up to 256 frames later. The border is *black* rather than stale
for the reason listed above as a non-defect: the game's full-frame colour clear
still runs, so the strips outside its 4:3 scissor are cleared and then never
drawn into. The property that made this game easy in 2026-07 is what made the
symptom look like a redraw bug.

The fix drives the rewrite from the list the game is submitting **this** frame:
`send_dl` passes `data_ptr` in, a bounded window from it is scanned every frame
and widened in place, and the 8 MB scan stays as the fallback for anything
outside that window on its old cadence. Sites are also widened the moment they
are found rather than on the next call. `HH_WIDESCREEN_LEGACY_SCAN=1` restores
both halves of the old behaviour.

**The measure is the part worth keeping.** The obvious one — did the rewrite
widen anything this frame — reads as healthy while the picture is wrong, because
the game rotates two display-list buffers `0x10048` apart and a cached site in
one goes on being rewritten every frame while the rect in the other has moved.
Measured `fresh=1` on every frame of a stretch that was going out at 4:3. What
works is to ask the only question that decides the picture: after the rewrite
pass, does the list RT64 is about to walk still hold a safe rect with the game's
own bytes in it? Its window is deliberately *narrower* (32 KB) than the one the
fix widens (256 KB), because the two buffers are 64 KB apart and counting the
sibling's un-widened rects turns a miss rate into an upper bound on one.

### The logo screens — one picture, two scales (fixed 2026-08-01)

**Reported:** the Konami screen and the "KCEO presents" screen are glitched in
widescreen — a dashed border floating around the Konami logo, stray black blocks
scattered around the KCEO logo, and the logo itself torn into offset pieces.

**Two A/Bs placed it before a line of code was read**, and both of them ruled out
the obvious suspect:

| arm | intro screens |
|---|---|
| `HH_NO_WIDESCREEN_SCISSOR=1`, aspect Expand | **still glitched** — so the scissor rewrite above is not the cause |
| aspect `Original` | **clean** — so it is the Expand path, and only that |

The screens are **pure 2D**: no perspective or orthographic projection in the
whole frame, just texture rectangles — 303 of them per frame, in two layers. RT64
scales a rect by one of two rules (`FramebufferRenderer::addFramebuffer`), and it
applied a different one to each layer:

| layer | rects | RT64's reading | result |
|---|---|---|---|
| 64x64 tiles covering the whole 640x480 frame | 193 | its slice spans the scissor, so it is a **background** | `inv=1.000`, stretched to the widened frame |
| 32x32 tiles over the middle of the screen | 110 | narrower than the scissor, so it is a **sprite** | `inv=0.750`, kept at native size, centred |

One picture, two scales, 1.33 apart — which is the reported symptom exactly, down
to the arithmetic: the border marks sit at window x 528 and 1392 where the
stretched copy of the same feature sits at 625 and 1300, and `960 ± (x-320)·2.25`
against `x·3` accounts for both.

**That rule is right for a HUD over a 3D scene and wrong for a screen that is all
2D**, so the fix is one condition: a frame that draws **no transformed geometry
at all** is a 2D screen, and every rect in it gets the same scale. It lives in
the fork, in `rt64_framebuffer_renderer.cpp`, because there is nothing on the game
side to say it — the display list is honest, RT64's inference from it is what
splits.

Two things about it that were measured rather than assumed:

- **The test has to be workload-wide, not framebuffer-pair-wide.** In gameplay the
  game submits a rect-only pair and a geometry pair in the same frame, so a
  per-pair test calls the rect-only half of *every gameplay frame* a 2D screen and
  re-scales the HUD with it: **1232 of 1242 passes** fired. Workload-wide fires on
  **1314 of 15200** passes across a 90-second boot — the two logo screens and two
  brief fades, nothing else. `HH_TRACE_2D_ASPECT=1` is that count.
- **`stretch` is a decision, not a default that fell out.** `native` renders the
  same screens pillarboxed at their true proportions and is *cleaner*: stretching
  a tiled image by a non-integer factor leaves one-pixel colour slivers at the
  tile seams, visible under the Konami logo. Those slivers are in the legacy
  rendering too — they are not new — but `native` is the only arm without them.
  Filling the window won on the trade.

`HH_2D_ASPECT=legacy` restores RT64's own per-rect decision exactly. Verified on
both builds: the Linux build for the A/Bs and the counts, and the Windows build
(RTX 5080, Vulkan) for the picture the player actually gets.

| arm (same binary, one env switch) | frames with an un-widened safe rect in the live list |
|---|---|
| `HH_WIDESCREEN_LEGACY_SCAN=1` | 1267 / 3584 — **35.4%** |
| default | 0 / 4096 — **0.00%** |

The fixed arm registered 976 safe rects at offsets it had not already cached, so
the layout was moving constantly while the miss rate stayed at zero. That figure
is a floor on churn *within* a run and **not** a control across the two arms —
the live-list pass looks every frame and the cache-only path every 256th, so the
fix inflates it by existing. No cross-arm control was found, and the attempt is
recorded so it is not repeated: the game's own full-frame scissor is untouched by
either arm, but it is emitted before the variable part of the list, so its offset
never moves.

Both figures are from `HH_AUTOSTART` boot runs on the Linux build, which reach
the logos and the title screen. **Confirmed by eye on 2026-08-01**: the user
played the Windows build — the same report that opened this section — and the
transitions read correctly. The counter was never the last word on this one; the
picture was.

`hr_option` (the HUD ratio) is now wired to RT64's `extAspectRatio` as well —
this port had been dropping it silently. It changes nothing yet and the comment
says so: RT64 reads HUD origins only from the extended GBI, and neither this port
nor Goemon tags a rect.

## The renderer

`src/main/rt64_render_context.cpp` drives RT64 from the display lists the game
was already producing. It is Goemon64Recomp's context with two things left out
that this port does not have — the texture-pack action queue and the Android
window handoff. That is most of the omitted length and none of the behaviour.
`recompui::set_render_hooks()` came back with the launcher, and is called before
`app->setup()` so the hooks are in place by the time RT64 has a device to invoke
them with.

The game renders as far as the "Memory Pak Enhanced" screen, which is also where
it waits, because that screen wants a button. Input is remappable and a gamepad
works — see "The launcher"; `-DHH_UI=OFF` falls back to the fixed keyboard layout
documented under "Environment variables".

### The runaway display list ✅ — and it was the leak that killed the VM

**Press the button and the game continues; leave that screen alone and the
screen goes black, the game stops responding, and the process grows at
~490 MB/s until the OOM killer takes it — and, uncapped, the whole WSL VM with
it.** That was written off for two sessions as "uncapped runs kill WSL". It is
not about being uncapped; running uncapped only removes what stops the leak from
killing the VM.

`Interpreter::processDisplayLists` (`lib/rt64/src/hle/rt64_interpreter.cpp`)
walks `while (dl != nullptr)` and ends **only** when a command sets `dl` to null
— a `G_ENDDL`. Nothing else bounds it: no command limit, no address range, and
an unrecognised opcode *logs and then advances* rather than stopping. So a list
that never terminates executes memory as GBI commands forever, appending to the
state's buffers the whole way, and the gfx thread never returns from `send_dl`.

**The list's address is `0x00000000`.** The submitted task's display-list
pointer is null and RT64 interprets rdram from offset 0. Guarded on the renderer
side with a `1 << 20` command ceiling that abandons the frame and prints
`[rt64] display list at 0x... exceeded 1048576 commands without ending`. A real
frame is thousands of commands, so the ceiling cannot truncate valid content,
and abandoning one frame beats losing the process.

Verified by A/B: three runs after that ceiling stayed flat at 556/625/619 MB
where the leak previously fired in about 60% of runs, and one of those three hit
the runaway at t=32–33 s — the same instant the leak used to start — printed the
diagnostic, and carried on with RSS 553 → 584 MB, the gfx queue back to 0 and
audio still running. That is the same event being caught, not three quiet runs.

**Why the task arrived with a null `data_ptr` is now answered** — see below. The
ceiling stays as the backstop; the guard that actually stops it is one layer up.

### Why the `data_ptr` was null ✅ — the game re-initialises the task in flight

Hybrid Heaven drives the RSP through a scheduler of libultra's shape.
`func_80001BC0_27C0` builds an array of **three** `OSScTask`-shaped records at
`0x8005C3A8`, stride `0x58`, with the `OSTask` at `+0x10` — so `data_ptr` is
`+0x40` from a record's base and `output_buff_size` is `+0x3C`. That init writes
`data_ptr = 0`, `data_size = 0`, `output_buff_size = 0` and
`ucode_boot_size = 0`. The only thing that ever fills them is
`func_80001D5C_295C`, the end-of-frame call, which appends `G_FULLSYNC`/`G_ENDDL`
and then writes

```
data_ptr        = D_800692B0 + (s16)D_8008934C * 0x10048
data_size       = (D_8008D5BC - data_ptr) & ~7
output_buff_size = D_800642B0 + 0x4000
ucode_boot_size  = 0xD0
```

before `func_80000ED0_1AD0` advances `sc->0x898` (the rotating record index) and
posts `&record[old_index]` to the scheduler's task queue.

**`D_800692B0` is a fixed address in `.main`, so that expression cannot evaluate
to zero for any buffer index.** A submitted task with `data_ptr == 0` is
therefore not a torn write and not a wrong address — it is a record that has not
been through the fill. `output_buff_size` is the field that proves it: the init
writes 0 there and the fill writes `0x800682B0`, and every null-`data_ptr` task
observed had `output_buff_size = 0` and `ucode_boot_size = 0` as well. The whole
record was pristine.

**What re-pristines it is `func_80001BC0_27C0` running again.** It is called from
the boot path once and from the boot state machine `func_80001454_2054` at
`0x800015B8`, immediately after `osViSetMode` and `osViSetSpecialFeatures(0x4A)`
— the video-mode change that ends the "Memory Pak Enhanced" screen. That branch
is taken when the flag at `0x801BBD54` reads zero and a five-tick countdown
expires.

**`0x801BBD54` is not the controller.** It was guessed to be, on the strength of
the report that the failure happens only when the screen is left alone, and that
guess is wrong: it goes 0 → 1 → 0 by itself, identically, in runs with and
without a button press. The game sets and clears it. Do not read the five-tick
countdown as an input timeout.

**The report was right anyway, and pressing Start is what separates the two
cases — but not through that flag.** Measured with the `[input]` trace, which
prints every change in the button word so a run's log says whether a press
arrived and when:

| run | Start (`0x1000`) arrived | passes through `func_80001BC0_27C0` | null lists |
|---|---|---|---|
| three runs, no press | — | **two** (`0→1→2→3→1→2`) | 3, 1, 1 |
| pressed at frame ~1710 (~62 s) | after the transition | **two** | 1 |
| pressed at frames ~475/~510 (~17 s) | before the transition | **one** (`0→1→2→3`) | **0** |

Left alone, the state machine falls back from state 3 into state 1 and runs the
whole mode change a *second* time, and it is the second pass that lands on a
record already queued. Pressing Start in time takes it through once and it stops
there. The window is only about 28 seconds — the game leaves that screen on its
own — which is why the first two attempts at this test pressed too late and
looked like a negative result.

Two caveats on that table. The "pressed early" row is a single run, and every
row is on `llvmpipe`; the number of passes is a property of the game's state
machine but which frame the second one lands on is renderer timing. The guard
below makes the difference moot either way.

A traced run shows it exactly. `HH_TRACE_GFX=1` prints all three records per
frame; around the transition (`state` is `D_80037730`, `pend` is `sc->0x89C`,
the game's own count of tasks outstanding):

```
f=782 state=2/1  r0=800692B0/800682B0 r1=800792F8/800682B0 r2=800792F8/800682B0
f=783 state=3/-1 r0=00000000/00000000 r1=00000000/00000000 r2=800692B0/800682B0
f=785 state=2/5  pend=2
f=786 BAD state=0/-1 r0=00000000/00000000 r1=00000000/00000000 r2=00000000/00000000
      task data_ptr=00000000 size=00000000 obs=00000000 ubs=00000000
f=787 state=0/-1 r0=800692B0/800682B0 r1=00000000/00000000 r2=00000000/00000000
```

f=783 catches the init loop mid-pass — records 0 and 1 wiped, record 2 not yet.
f=786 is the one that lands: the record whose pointer was already sitting in the
scheduler's queue gets zeroed before `osSpTaskStartGo` reads it, so
`submit_rsp_task` copies a pristine `OSTask`. **One frame later the game is
drawing normally again**, and from there its lists grow from `0xE8` to `0x1068`
bytes — it is past the boot screens and into real content.

**Nothing rebases or corrupts anything; the game overwrites a task it has
already handed off.** The window exists because the game's scheduler blocks
waiting for the previous task to complete, and in this port "complete" means
`dp_complete()`, which the gfx thread sends only *after* RT64 has finished
rendering. On hardware the RSP is done inside a frame and the queue drains
before the next retrace; here the renderer is the slow part, the task queue backs
up (`pend` reached 2 on the frame before the failure), and the retrace-driven
state machine gets far enough ahead to re-init records that are still queued.
ultramodern says as much in `gfx_thread_func`, where it sends `sp_complete()`
early: *"Games usually preserve the RSP inputs until the RDP is finished as
well, so sending this early shouldn't be an issue in most cases."* Hybrid Heaven
is one of the other cases.

**The fix is a guard in `send_dl`, not a change to the game.** A display list
that does not start inside rdram is not a display list, so the frame is dropped
before RT64 ever sees it — a million commands earlier than the ceiling, and with
the same outcome, since the game submits the next frame from a refilled record
regardless. `HH_NO_DL_GUARD=1` restores the old behaviour as the A/B, and the
drop always prints the full task and scheduler state, so a run that hits it says
so.

**The A/B is 6 for 6, and it corrects the previous entry above.** Three runs
with the guard and three with `HH_NO_DL_GUARD=1`, 150 s each, screen left alone:

| | guard | `HH_NO_DL_GUARD=1` |
|---|---|---|
| null lists dropped / interpreted | 3, 1, 1 | 1, 1, 1 |
| RT64 command ceiling fired | 0/3 | 3/3 |
| ended by | `timeout` (124), all three | **SIGSEGV, all three** |

So this is not intermittent at all — leave the screen alone and it happens every
run, at the mode change. And **the command ceiling on its own was not enough.**
It abandons the frame, but RT64 has already appended a million commands' worth of
state, and every unguarded run then died with a `SEGV_ACCERR` inside
`libvulkan_lvp` at an address far outside rdram. The earlier claim here that
three post-ceiling runs "stayed flat" was measuring memory, which the ceiling
does fix; it was not measuring survival. Dropping the list before RT64 sees it
fixes both.

Three more things worth knowing:

- **It runs on `llvmpipe` here**, Mesa's software rasteriser, because this
  machine has no working GPU passthrough. Correct but slow. The `Device Name:`
  line the port prints at startup is the thing to check before blaming RT64 for
  the frame rate; the `gfxstream`/`virtio` ICDs are installed, so a machine with
  passthrough should pick a real device.
- **The null renderer stays.** `-DHH_RT64=OFF` builds
  `src/main/null_render_context.cpp`, whose virtuals do nothing. It runs
  everything below the display list without standing up RT64, which makes it the
  bisector — a failure that survives it is not a renderer bug. It earned that
  role: the whole CPU side was brought up behind it.
- **RT64 needs no dependencies installed**, and the two things it does need from
  this side are in "Building".

## The overlay loader hook

Hybrid Heaven loads 92 code overlays out of its archive at runtime. A static
recompilation cannot notice that happening — the DMA is a memcpy into rdram, and
the overlay's real code is native code already in the binary, unreachable until
something maps its vram addresses to it. `recomp_load_overlays` is that mapping,
so the loader is patched to call it.

`patches/required.c` reimplements `func_8000469C_529C` — `load(file_index, dest)`,
110 instructions at `lib/hybridheaven/asm/usa/main_4F10.s:296` — with the call
inserted before the load. This mirrors `Goemon64Recomp/patches/required.c`, which
patches the same function in the same archive format for the same reason. A patch
only has to *behave* the same, not match, so this is a reimplementation from the
disassembly rather than a decompilation.

**The hook is correct by construction, and that was checked rather than assumed.**
The patch passes librecomp values the game computes from its own two tables, and
librecomp matches them against the section table N64Recomp generated from the
decomp ELF. For all 92 overlays those agree on every axis that matters:

- the file table's ROM address equals the section's `rom_addr` — 0 mismatches;
- the load-address table's `start` equals the section's `ram_addr` — 0 mismatches;
- each file's ROM range fully covers its own section **and no other**, so one
  `recomp_load_overlays` call per load registers exactly one section.

Two of Goemon's three patches do not transfer, and neither does its
`overlay_apply_relocations`: all three exist to convert TLB-mapped overlay
addresses to KSEG0, and Hybrid Heaven TLB-maps nothing.

### Is the hook the only way bytes reach an overlay slot?

The eviction is only sound if it is, and until 2026-07-26 nothing had checked —
`verify_overlay_hook.py` checks table *agreement*, not load-path *exclusivity*.
The audit is written out in full in `patches/required.c`, above the patched
function. In short: the game's PI DMA wrapper `func_80001FE8_2BE8` has 9 call
sites in 8 functions, and the recompiled C and the decomp asm agree on that
count. One is the hook. One is a dispatcher that routes file ids below 0x8000
straight to the hook and serves 0x8000–0x8B89 out of a second archive entirely.
Four have fixed destinations, and **none of the four overlaps any of the 94
registered section ranges** — including the two that look like they should:
`0x801077E0` sits 0x50 below `.file_7`, and `0x80191520` is exactly where
`.file_7`'s section ends, which is not an overrun but `.file_7`'s own `.bss` per
splat, so the audio banks load *inside* the permanently resident overlay.

**This bounds the class; it does not close it.** Three paths take a
caller-supplied destination, and the static closure keeps widening — one
forwarder call site is itself inside an overlay. Closing it properly means a
runtime check in librecomp's `do_dma`, the one choke point that is exhaustive by
construction, which would be a seventh patch to a vendored submodule. Deliberately
not taken yet.

### Why no relocation step is needed

Two independent reasons, either of which suffices:

1. ~~**The overlays do not move.**~~ **This one is false, and running the port
   disproved it.** The load-address table does give every file a fixed slot whose
   `start` equals the section's link address, and `tools/verify_overlay_hook.py`
   still confirms that for all 92 — but the table is only what the *boot path*
   passes. `func_8000469C_529C` is `load(file_index, dest)`, and `dest` is a
   **parameter**: a caller is free to pass something else, and one does. A traced
   run shows the code section at ROM `0x006A1BC0` loaded once to its link address
   `0x801BF1A0` and once to `0x801FA948`, a delta of `0x3B7A8`.
2. ~~**Even if one moves, the recompiler already handles it**~~ — it does, by
   reading `section_addresses[]` from the `RELOC_HI16`/`RELOC_LO16` a relocatable
   section's `%hi`/`%lo` pairs become, and `load_overlay` set that entry from the
   address passed to `recomp_load_overlays`. **That is the wrong thing to want
   here**, and the hazard noted below turned out to be live: two copies of one
   overlay really are resident at the same time, and one entry per section cannot
   describe both. The port now keeps the link address instead — see "The same
   overlay, live at two addresses". Nothing is relocated because nothing on this
   game's hardware would be.

### Eviction on load ✅

`load_overlay` only ever *added* to `func_map`, and nothing ever removed a
previous occupant, so two tenants of overlapping slots stayed resolvable at once
and an indirect call through a vram both cover reached whichever registered last.
Hybrid Heaven's slots overlap by construction: `0x8038B7E0` + `0x2580` runs past
the start of the `0x8038CFC0` slot, `0x80358820` + `0x343A0` ends in the middle
of the `0x8038B7E0` one, and `0x803837E0` + `0xA5E0` straddles both.

The loader hook now announces the eviction the game never announces. Before the
load, `patches/required.c` calls `recomp_evict_overlays(buf_start, extent)` with
the **whole range the call is about to write** — the loaded bytes plus the
zero-filled `.bss` tail, whichever reaches further — and every load announces it,
not only the 92 code overlays, because an asset loaded over a dead overlay
destroys it just as thoroughly. A traced run fires it 5 to 9 times.

**The obvious implementation does not work, which is why there is a patch.**
librecomp's `unload_overlays(ram, size)` exits on a section the range covers only
*partially*, and that is precisely the case here.
`tools/n64modernruntime-evict-overlapping-overlays.patch` adds
`unload_overlapping_overlays`, which drops what the range touches and returns how
many *sections* went. It removes a `func_map` entry only when that entry still
points at the section being dropped — overlapping sections can define a function
at the same address, and the later loader owns it.

**It used to drop every touched section whole, and that was the
`Failed to find function at 0x803757B0` abort.** See "Partial eviction" below.
A section the range covers entirely still goes as a unit; a section it covers
only in part now loses just the functions whose bytes were overwritten.

Two things this deliberately does *not* do:

- **it does not expand the range to a closure.** Growing the range until every
  loaded section is wholly in or wholly out would let one `unload_overlays` call
  do the job with no patch at all, but it over-evicts: a `0x750` load at
  `0x8038CFC0` would chain through the `0x8038B7E0` tenant into the `0x80358820`
  one, which the load never touches. Evicting a section the game can still call
  turns a silent wrong answer into a hard `get_function` exit;
- **it does not drop an earlier copy of the same section.** That was tried, on
  the reasoning that `section_addresses` holds one address per section so a
  second live copy cannot be described. The game disproved it immediately:
  `Failed to find function at 0x801CBDC0`, inside the copy that had just been
  evicted. Both copies are genuinely live. See below.

`HH_NO_EVICT=1` turns the whole thing off, which is the A/B for anything that
looks like it might be eviction's fault.

### Partial eviction ✅ — and this was `Failed to find function at 0x803757B0`

> **Superseded in its headline, 2026-07-30, and kept because the mechanism it
> describes is real and shipping.** Partial eviction was necessary but it was not
> the cause: `.file_56` was never *registered* at all, because the game brings it
> in through a second, asynchronous archive loader the port had not patched. That
> is fixed in librecomp's `do_dma`. What this section gets right is the eviction
> rule itself — including the one-word amendment below, which is what the abort
> turned into once the overlay started being registered.

The abort was recorded as "the shared-slot ambiguity reaching the indirect call
path". **It is not ambiguity.** `0x803757B0` is defined by exactly one function
in the entire game — `func_803757B0_8193D0`, in `.file_56` — so there is nothing
to disambiguate. It is a residency failure, and the port caused it.

The geometry is in the generated section table and needs no run to see:

| section | ram | size | functions |
|---|---|---|---|
| `.file_56` | `0x80358820` | `0x343A0` | 454 |
| `.file_54` | `0x803837E0` | `0xA5E0` | — |

`.file_54` loads **inside** `.file_56`. Its `0xA5F0` extent overwrites 22 of
`.file_56`'s 454 functions and leaves **432 untouched** — still valid
instructions sitting in rdram, exactly as they would be on hardware, where a DMA
into the middle of an overlay destroys what it covers and nothing else.
`func_803757B0_8193D0` is `0xE030` below the overwritten region, so it is one of
the 432. Dropping the section whole made every one of them a hard
`get_function` exit, and the game calls that one: it is a constructor, clearing
three bytes of an object and registering `func_803758FC_81951C` as its handler
through `func_800058DC_64DC` — the shape reached from a per-type function-pointer
table.

`unload_overlapping_overlays` now distinguishes the two cases. Full coverage
drops the section as before; partial coverage calls `drop_overwritten_functions`,
which walks `FuncEntry.rom_size` and erases only the entries whose extent
intersects the load. `HH_EVICT_WHOLE_SECTION=1` restores the old behaviour.

**One word finer, added 2026-07-30:** a function whose **final word** is all the
load overwrites keeps its entry. That word is the delay slot of its returning
`jr $ra`, every instruction that does its work is intact, and the recompiled
function is host code translated from all of them at build time — so what the
clip leaves in rdram never executes either way. What lands there is a
neighbouring overlay's base address, not code the game means to run. `.file_55`
loads at `0x803757E0`, four bytes inside `func_803757B0_8193D0`
(`0x803757B0`–`0x803757E4`), and the game calls `0x803757B0` immediately
afterwards; dropping it there aborted the first run that ever cleared the battle
transition. A load reaching one word deeper covers `jr $ra` itself and still
drops the function. `HH_DROP_TAIL_CLIPPED=1` restores the strict rule, and
`HH_SELFTEST_TAIL_CLIP=1` checks all three properties without a battle.

**And the cost of leaving the section loaded, found the same day by playing past
the fix above.** Partial eviction's whole point is that the section survives — but
*listed as loaded* and *callable* are then two different states, and the claim path
conflated them. `register_unannounced_overlays` returned early on "already in
`loaded_sections`", so when the game re-loaded `.file_56` its 147 dropped functions
were never restored, and it called one: `func_803758FC_81951C` at `0x803758FC`, the
handler `func_803757B0_8193D0` installs, which is `0x11C` inside `.file_55`'s range
and so genuinely destroyed. On hardware the re-DMA brings it back. The claim path
now checks rather than assumes — any function missing or owned by something else
means the bytes have just arrived, so it drops and re-adds the section, in that
order, leaving no duplicate. `HH_NO_RELOAD_REPAIR=1` restores the early return.

**User-verified in gameplay 2026-07-30** — the user played eight battles, reported
"all battles worked just fine" and quit cleanly — with the
counts agreeing exactly — 8 tail clips and 8 `.file_55` claims (one per entry),
`.file_56` claimed once and then repaired 7 times at 147 of 454 functions, because
a section that is never removed can only ever be claimed once.

**The partial case is not hypothetical — it fires in ordinary play.** A 300-second
run evicts 8 times, and one of them is partial:

```
[overlay] evict ram 801E1BE0 size 0001B8E0 -> dropped 0 sections and 43 functions
                                              from sections left loaded
```

That range clips the *second* live copy of the `0x006A1BC0` overlay at
`0x801FA948`, destroying 43 of its functions and leaving the rest. The old code
dropped that whole copy — and this port already learned once, the hard way, that
dropping that copy is wrong: it produced `Failed to find function at 0x801CBDC0`.
So the two known `Failed to find function` addresses in this project's history,
`0x801CBDC0` and `0x803757B0`, are the same mistake at two scales.

**The `.file_56` case specifically still needs the self-test**, because that
overlay is never loaded in a run this port can currently play through — which is
why the abort was reported at ~150 s and reproduces in none of the ten runs here.
`HH_SELFTEST_EVICT=1` loads `.file_56` at the first overlay load, applies
`.file_54`'s range to it, prints the result, and drops whatever is left so the
state is restored exactly:

```
with the fix:              dropped 0 sections and 22 functions
HH_EVICT_WHOLE_SECTION=1:  dropped 1 section  and 0 functions
```

That 22 is the number the section table predicts, arrived at independently — the
runtime walking its own `rom_size` fields, not a restatement of the analysis.

### The same overlay, live at two addresses ✅

A traced run loads the section at ROM `0x006A1BC0` to its link address
`0x801BF1A0` **and** to `0x801FA948`, and then calls `0x801CBDC0` — inside the
first copy — after the second has loaded. Both are live, and neither is a mistake
by the loader: `func_8000469C_529C` is `load(file_index, dest)` and `dest` is a
caller's choice.

`section_addresses[]` holds one address per section, and the recompiled
`RELOC_HI16`/`RELOC_LO16` read it while the code runs, so tracking the most
recent load address leaves the *other* copy computing its data addresses from the
wrong base — 0x3B7A8 out, in this case.

The answer is that this game should not track a load address at all.
**Hybrid Heaven has no relocation step**, so a copy of an overlay at some other
address goes on carrying the absolute addresses it was linked with, and reaches
the original copy's data no matter where it runs from. That is what the hardware
does with the same bytes. Keeping `section_addresses` at the link address
reproduces it exactly, and it is the only policy that can describe two live
copies at once.

`recomp::overlays::set_overlay_relocation_enabled(false)`, called from
`src/main/register_overlays.cpp`, is that policy; the default is unchanged for
games that do relocate or that map overlays to a fixed virtual address, as
Goemon does. `HH_RELOCATE_OVERLAYS=1` restores the tracking as the A/B.

This supersedes the reasoning in "Why no relocation step is needed" below: the
recompiler *would* have handled a moved overlay, but handling it is the wrong
thing to do for a game that does not.

### The SIGSEGV that was not a crash ✅

For most of this port's life every run ended in a SIGSEGV, and it was recorded
here as the top open defect — a stack with two overlapping overlay sections in
it, read as the stale-`func_map` hazard finally biting. The co-residency was
real. **The crash was not.**

The runs were ended with `timeout 60`. `timeout` sends SIGTERM, SDL turns SIGTERM
into an `SDL_QUIT` event, `update_gfx` turns that into `ultramodern::quit()` —
and `recomp::start` then joins its own threads, joins ultramodern's, and
**munmaps rdram while the threads the game created with `osCreateThread` are
still running**. Whichever one is mid-instruction faults at a perfectly ordinary
game address. The backtrace shows overlay frames because that is where the game
happened to be.

What gave it away was the fault address, once the crash handler started printing
it: `SEGV_MAPERR` at `rdram + 0x37764`, inside a 512 MB mapping that is supposed
to be readable and writable throughout. `HH_DUMP_MAPS=1` then showed no such
mapping at all, and the SDL event's own timestamp was 45043 ms in a 45-second
run. Run the same binary under `timeout -s KILL` and there is no signal at all.

Both halves are fixed. `tools/n64modernruntime-keep-rdram-on-shutdown.patch`
stops freeing rdram — the process is exiting and the allocation is reserved
rather than committed, so leaving it to `exit()` costs nothing — and `main`
finishes with `std::_Exit` rather than returning, because the static destructors
were racing the same threads one layer up (one run tore down ultramodern's
message queues while a game thread was dequeuing from one). Restore both once
game threads are joined rather than abandoned.

**One exception, added 2026-10-04: an Android restart returns instead.** The
`_Exit` predates the Android port, and it silently broke **Settings → Restart**
there from the day that feature landed: the relaunch is done by
`MainActivity.onDestroy()` handing off to `RestartActivity`, and `onDestroy` only
runs after `SDL_main` returns — so `_Exit` killed the process first and the player
landed on the home screen (logcat: `request_restart(2)`, then Zygote `exited
cleanly (0)`, no `RestartActivity`). Returning is safe on Android where it is not
on desktop: SDL's `nativeRunMain` does not call `exit()` after `SDL_main`, and
`RestartActivity` ends the old process with `killProcess`, so no static destructor
runs either way. A plain Quit still `_Exit`s. Verified on the RP5: To Title
Screen and To App Menu both relaunch, and Quit still exits without one.

**The moral is worth keeping.** A crash reported at a plausible address, in
plausible code, with a plausible backtrace, was an artefact of how the run was
stopped. The fault address and `si_code` are what separated the two, and neither
was being printed. They are now.

### Ambiguous call targets ✅ — and this was the null dereference

With the shutdown out of the way the port reached a real fault, and it turned out
to be a symptom of something larger. The backtrace contained
`func_8038D28C_84D86C`, whose section (`rom=0x0084D5A0`, in the `0x8038CFC0`
slot) **was never loaded in that run**. It ran because the call to it was a
direct C call in the recompiled output rather than a `func_map` lookup.

Two sections define a function at vram `0x8038D28C`, and their RAM ranges
overlap, so only one can ever be resident. They are not remotely the same
function:

| | `func_8038D28C_84D86C` (`0x8038CFC0` slot) | `func_8038D28C_85204C` (`0x8038B7E0` slot) |
|---|---|---|
| | `jal 0x80229500` | `andi $a0, $a0, 0xFFFF` |
| | `lui $a1, 0x8039` / `addiu $a1, -0x2D40` | `jal 0x80020718` |
| | `jal 0x800058DC` | |

The caller does `jal 0x8038D28C` with `$a0 = 0x7D`. What it means is the one on
the right — mask to 16 bits, call the audio routine in `.main`; a sound effect.
What it got was the one on the left, which calls `0x80229500` and takes the
address of `0x8038D2C0` in an overlay that was never loaded. `func_80229500`
then dereferenced a null pointer at offset `0x111`. **The "next failure along"
was this failure all along.**

Across the whole output before the fix:

```
15696 placeholder-named functions in the section table
 2139 vram addresses defined by more than one section
  106 of those are the target of a direct call
 1004 direct call sites go to an ambiguous vram
   98 have every one of their sites bound to a single tenant
```

This is the ambiguity Phase A solved for *data* — `overlay_syms.ld` exists
because "an address in a slot" is not "a symbol in a file" — one level up, and it
had never been checked for calls.

**Why the recompiler could not see it.** `resolve_jal` treats a relocatable
section as a candidate only when it is the caller's own, so an ambiguous target
would normally come out as `Ambiguous` and fall back to lookup. But a `jal` with
an `R_MIPS_26` reloc never reaches that logic on equal terms: the caller passes
`target_section = reloc_section`, and `resolve_jal` then measures
`in_current_section` against *that* section. The target is trivially inside it,
the exact match succeeds, and the call binds by name. The recompiler resolved it
correctly given the reloc; the reloc was the linker's guess about which of 44
overlapping sections owns an address.

`tools/n64recomp-ambiguous-jal-lookup.patch` adds the missing test: when the
target vram is defined by more than one *relocatable* section and the resolution
came through another section's reloc, resolve it at runtime instead. A call whose
target section is the caller's own is left alone — that one is unambiguous,
because the caller running at all means its own section is resident.

```
LOOKUP_FUNC sites                     1393   (was 904)
direct calls to an ambiguous vram      515   (was 1004)
  of those, crossing sections            0   (was 489)
```

All 515 that remain are a section calling its own address. **The port then runs
for as long as it is left alone** — 39 loads across 30 distinct sections in two
minutes, no signal — where before it faulted after 26.

### The audio microcode hits an unhandled jump target ✅

The other failure mode, also timing-dependent:

```
RSP ucode 2 exited unexpectedly. exit_reason: 3
Failed to execute task type: 2
```

Task type 2 is `M_AUDTASK` and `exit_reason` 3 is `RspExitReason::UnhandledJumpTarget`
(`librecomp/include/librecomp/rsp.hpp`). The reading recorded here was that this
is an indirect branch target missing from `extra_indirect_branch_targets` in
`aspMain.toml` — Phase B took that list of 16 from Goemon, and the natural guess
was that Hybrid Heaven drives the same microcode down a path Goemon's list never
needed.

**The target has now been read, and it is not that.** `rsp/aspMain.cpp` prints
`Unhandled jump target 0x%04X` plus a register dump before returning, and what it
prints is:

```
Unhandled jump target 0x638F in microcode aspMain, coming from [rsp/aspMain.cpp:112]
Unhandled jump target 0x0000 in microcode aspMain, coming from [rsp/aspMain.cpp:112]
Unhandled jump target 0x0001 in microcode aspMain, coming from [rsp/aspMain.cpp:112]
```

**A different value each time, and none of them is an address in this
microcode.** The text is `0xE20` bytes at IMEM `0x1080` and IMEM is 4 KB, so
`0x638F` is out of range outright and `0x0000`/`0x0001` are not targets at all.

**The list of 16 targets is provably complete, and the microcode's own dispatch
table is what proves it.** aspMain dispatches each audio command with

```
r1 = (cmd >> 23) & 0xFE     # opcode, doubled, from the command word's top byte
r2 = lh 0x10(r1)            # jump table, at DMEM 0x10
jr r2
```

and that table is the head of the microcode's data segment, at ROM `0x4E520`.
Decoding it gives targets inside the text for opcodes `0x00`-`0x0F`, and nothing
but unrelated data past that -- `0xF000`, `0x0F00`, `0x00F0`, a run of bit masks.
Those sixteen entries are, value for value, `aspMain.toml`'s
`extra_indirect_branch_targets`. **That list is not Goemon's list that happens to
work here; it is the entire dispatch table, and it cannot be short.**

So the valid opcode range is `0x00`-`0x0F`, and the observed failures are opcodes
`0x79` and `0x34` -- indices `0xF2` and `0x68`, which read the bit masks past the
end of the table and jump to them. The command list held something that is not a
command, and adding any of the printed values to the toml would only move the
failure.

Phase B is untouched by this, and on firmer ground than before: the microcode
recompiles byte-for-byte identically to Goemon's *and* its target list is now
known to be complete by construction.

`aspMain_reporting` in `src/main/main.cpp` wraps the microcode and, on a failed
task, prints the OSTask and walks the command list for the first invalid opcode
with the surrounding commands. That dump is what solved it: the list was valid
for exactly 5120 commands -- `0xA000`, the whole buffer -- and then ran into the
OSTask that sits immediately after it, whose first word `800350D0` reads as
opcode `0x80`. See "The audio path" above; the cause was an unbounded
`get_frames_remaining`, and the same number produced the other failure too.

**Run it with `stdbuf -o0`** — that diagnostic goes to `stdout` via `printf`, and
the error path exits without flushing, so a redirected run loses it entirely.

### What the host side owes — nothing outstanding

This section used to list three things blocked on `src/` existing. All three are
done: `recomp_api.cpp` unwraps the host exports, `register_patches.cpp` wires the
generated tables into librecomp, and CMake shells out to `make -C patches` and
the recompiler as part of the build.

### `.main` does not need an explicit load — settled

The open question was whether a relocatable `.main` needs a `recomp_load_overlays`
covering its ROM range at startup, since `librecomp` only puts a section's
functions into `func_map` when that section is *loaded*. **It does not.**

`recomp::init` (`librecomp/src/recomp.cpp`) already calls
`load_overlays(0x1000, entrypoint, 1024 * 1024)` before the game runs an
instruction. Running that function's actual bounds logic against this game's
generated `section_table` selects exactly two sections:

| section | ROM | loaded to | link address |
|---|---|---|---|
| `.entry` | `0x1000`–`0x1060` | `0x80000400` | `0x80000400` |
| `.main` | `0x1060`–`0x5E7D0` | `0x80000460` | `0x80000460` |

Both land at their link addresses, and the next section by ROM order is
`.file_7` at `0x4E6F40`, far outside the window — so nothing is pulled in by
accident either. No startup call is needed and none should be added.

### Two facts worth knowing before touching the loader

**Every file in the shipped ROM is uncompressed.** `hybridheaven.z64` is the
*decompressed* ROM, and in it all 625 file-table entries have bit 31 clear. The
loader's compressed branch is therefore dead at runtime; `required.c` keeps it
because the original has it, but only the PI-DMA path executes.

**`file_id` and `.file_N` differ by one.** The loader's `file_id` is 1-based and
reads table entry `file_id - 1`; splat names its segments by the 0-based entry
index. `.file_7` is the loader's file 8. Both conventions appear in
`lib/hybridheaven/PLAN.md` — its Phase A boot-path list uses loader ids, while
`tools/overlay_map.py` uses splat's.

## The prerequisites — six local patches, across two submodules

The six patches are **committed**, one commit each, on a `hybridheaven-port`
branch in each of the two submodules:

| Submodule | Branch | Base |
|---|---|---|
| `lib/N64ModernRuntime` | `hybridheaven-port` | `089f12f` |
| `lib/N64ModernRuntime/N64Recomp` | `hybridheaven-port` | `07fcdac` |

Both working trees are clean, and `git submodule update` now restores the
patched commits instead of discarding them. They used to live as uncommitted
working-tree edits mirrored to `tools/*.patch`, which one `git submodule update`
would have wiped — and the nested `N64Recomp` was not even on a branch. The
`tools/*.patch` files are kept as the readable record of what each change is and
why, and each commit names its mirror.

To rebuild the branches from the mirrors — after a fresh clone, or to re-base
them on a newer upstream:

```bash
cd lib/N64ModernRuntime/N64Recomp
git checkout -b hybridheaven-port 07fcdac
git apply ../../../tools/n64recomp-cross-section-calls.patch
git apply ../../../tools/n64recomp-ambiguous-jal-lookup.patch
git apply ../../../tools/n64recomp-skip-unsupported-relocs.patch
git apply ../../../tools/n64recomp-gcc-weak-recomp-func.patch

cd ..    # lib/N64ModernRuntime
git checkout -b hybridheaven-port 089f12f
git apply ../../tools/n64modernruntime-evict-overlapping-overlays.patch
git apply ../../tools/n64modernruntime-keep-rdram-on-shutdown.patch
```

`n64recomp-ambiguous-jal-lookup.patch` is written against the tree with
`n64recomp-cross-section-calls.patch` already applied, so apply them in that
order. The dependency is semantic, not textual: both orders apply cleanly under
`git apply` and produce an identical tree, so git will not catch a swap. What a
swap costs you is an intermediate commit that does not build, which matters when
bisecting or when re-basing these onto a newer upstream one patch at a time.

The first three change the recompiler binary and need a rebuild
(`cmake --build ~/projects/N64Recomp-build-patched --target N64RecompCLI`).
`n64recomp-gcc-weak-recomp-func.patch` edits `include/recomp.h`, which is consumed
when the *generated C* is compiled; the two `n64modernruntime-` patches are
librecomp source. All three take effect on the next `cmake --build build`.

### `tools/n64recomp-ambiguous-jal-lookup.patch`

Resolves a `jal` at runtime when the target vram is defined by more than one
relocatable section and the match came through another section's reloc, instead
of trusting the linker's arbitrary attribution of an address several overlays
share. See "Ambiguous call targets" above — it removed the fault the port used to
die on, and it is a correctness bug for any game with overlapping overlay slots,
so it is worth upstreaming.

### `tools/n64modernruntime-evict-overlapping-overlays.patch`

Adds `unload_overlapping_overlays`, which drops every loaded section a range
touches — each one whole, whether or not the range covers all of it — and
`recomp::overlays::set_overlay_relocation_enabled`, which lets a game that never
relocates an overlay keep `section_addresses` at the link address. See "Eviction
on load" and "The same overlay, live at two addresses" above for why neither can
be expressed with the API as it stands. Both are general enough to upstream.

### `tools/n64modernruntime-keep-rdram-on-shutdown.patch`

Stops `recomp::start` munmapping rdram on the way out, because the threads the
game created are not joined first and the free lands under them. See "The SIGSEGV
that was not a crash" — this one cost the project a misdiagnosed top defect, so
it is worth upstreaming with the explanation attached.

### `tools/n64recomp-gcc-weak-recomp-func.patch`

Makes the GCC branch of `RECOMP_FUNC` weak, matching what the Clang branch
already does deliberately. Without it, patched functions are emitted as two
strong definitions in two static archives and the linker silently keeps the
wrong one, so every patch is discarded by a build that otherwise looks fine.
See "Why the loader hook never fired" above for the full account.

### `tools/n64recomp-skip-unsupported-relocs.patch`

`recomp_overlays.inl` came out with one reloc entry reading `.type = ` — nothing
at all — so the generated overlay table would not compile. The entry is a single
`R_MIPS_PC16` at vram `0x800272A8`, a branch to `handle_CpU`.

It appeared because of Phase C. `handle_CpU` is inside a run of `exceptasm.s`
that splat had merged; naming it turned what had been an internal branch into a
branch to a *global* symbol, and the assembler emits a relocation for that.
`elf.cpp` casts the ELF reloc type straight to `RelocType`, which only has eight
values, and `R_MIPS_PC16` is 10 — so the emitter indexed `reloc_names` out of
bounds and formatted whatever it found. Same class of bug as the segfault below:
an unvalidated index into a container.

A PC-relative branch within a section is position-independent and needs no
runtime fixup, so the patch skips reloc types the table cannot express and warns
instead of silently emitting garbage:

```
Warning: skipping unsupported reloc type 10 at 0x800272A8 in section .main
```

Worth upstreaming alongside the other one.

### `tools/n64recomp-cross-section-calls.patch`

Hybrid Heaven jumps directly between two *different* relocatable sections.
N64Recomp does not handle that — `recompilation.cpp` says so in a comment:
*"If a game ever needs to jump between multiple relocatable sections, relocation
will be necessary here."*

What happens without the patch is not a diagnostic but a **segfault**.
`resolve_jal` decides the target is in-section relative to the *reloc's* section,
then `CreateStatic` registers the synthesized static under the *caller's*
section. When those differ, the recorded address lies outside the section it was
filed under, and computing that static's extent reads out of bounds with a
negative length. ASan pins it at `main.cpp:818`.

The patch resolves such a call by runtime lookup instead — which is the correct
answer anyway, since which overlay is resident at a shared slot address is a
runtime question. It also reorders a bounds check in `main.cpp` that dereferences
before testing (defence in depth; it no longer fires once the first fix is in).

**Use `~/projects/N64Recomp-build-patched/N64Recomp`.** The older
`~/projects/N64Recomp-build` is built from the *unpatched* source and
will segfault on this game. Both are out-of-tree CMake builds; the patched one
comes from this repo's submodule, and needs rebuilding (`cmake --build . --target
N64RecompCLI`) whenever either patch changes.

This is worth upstreaming.

## A short function size is a truncated jump table ✅ fixed 2026-08-01

An Android session died after 45 minutes. The tombstone said `SIGSEGV`, null
deref, in `thread_cleaner_func` — and **named the teardown, not the cause.** One
millisecond earlier the game printed `Switch-case out of bounds in
func_8035A3D8_7FDFF8 at 0x8035A3F8 for jump table at 0x8038A000`. `librecomp`'s
`switch_error()` calls `exit(EXIT_FAILURE)` from a game thread, so static
destructors run under a live ultramodern — hence the destroyed-mutex `FORTIFY`
warnings and the fatal signal. `RelWithDebInfo` defines `NDEBUG`, so the
`assert(false)` above that `exit()` is a no-op and never gets a say.

The cause is in the decomp, not here. splat ends a function at a `jr $ra` and
opens a new symbol on the next instruction, which in `.file_56` is a jump-table
*case body*. N64Recomp's `analysis.cpp` sizes a table by walking entries while
they fall inside `[func.vram, func.vram + size)`, so a short size ends the table
early — **and the entry that does it lands exactly on the exclusive bound.** An
8-entry table became 2 cases, and every index the game's own `sltiu $at, $a0,
0x8` accepts above 1 aborted the process.

Bounded rather than guessed: 258 jr-jump-table switches in `RecompiledFuncs/`,
tested as emitted-`case` count against each function's own `sltiu` bound.
**Exactly two were truncated** — `func_8035A3D8_7FDFF8` (6 of 8 cases lost) and
`func_8035A938_7FE558` (2 of 5, and it had never fired). Both fixed in the decomp
by asserting the real sizes in `symbol_addrs.txt`; the sweep now reports zero.

Verified through to the binary rather than to the source, because a regenerated
`RecompiledFuncs/` proves nothing on its own. The compiled function went from
`cmp x8, #0x1` / `b.ne` falling into `switch_error` to `cmp w8, #0x8` / `b.hs`
over a real dispatch, and its eight targets match the ROM's table at `0x82DC20`
word for word.

**A spurious split is identifiable without any of this: it has a symbol and zero
`R_MIPS_26` callers.** All three symbols absorbed here had none; both genuine
neighbours, `0x8035A434` and `0x8035AA44`, have them.

The fixed build is installed on the Retroid Pocket 5 and reaches the launcher.
**It has not been exercised**: nothing has yet called `func_8035A3D8` with `a0` in
2..7, and the old build survived 45 minutes before it did. Confirming the fix
means playing, not launching.

## What the decomp owed — done

With the patch applied and `.main` listed in `hybridheaven.overlays.txt`, twelve
functions blocked recompilation, in two classes. Both are fixed, and both were
fixed by *naming* — no decompilation, and not one byte of the ROM changed.

**Absolute-symbol jal targets (8 targets, 3 callers in `.main`).** splat could not
attribute these to a segment and emitted them into
`.splat/usa/undefined_funcs_auto.ld` as absolute symbols
(`func_80126744 = 0x80126744;`). That links, but an absolute symbol has no
section, so neither does the `R_MIPS_26` the assembler makes against it — and
N64Recomp needs the section to resolve a call from one relocatable section into
another. Asserting the target's real name in the decomp's `symbol_addrs.txt`
makes splat emit a section-bearing symbol instead.

| target | resolved to | called from |
|---|---|---|
| `0x80126744` | `func_80126744_505E54` | `func_80001454_2054` |
| `0x80133AA0` | `func_80133AA0_5131B0` | `func_80001454_2054` |
| `0x80141108` | `func_80141108_520818` | `func_800183D0_18FD0` |
| `0x801414B0` | `func_801414B0_520BC0` | `func_800183D0_18FD0` |
| `0x80141934` | `func_80141934_521044` | `func_800183D0_18FD0` |
| `0x80151BFC` | `func_80151BFC_53130C` | `func_800021B4_2DB4` |
| `0x80151DA4` | `func_80151DA4_5314B4` | `func_800021B4_2DB4` |
| `0x801521C8` | `func_801521C8_5318D8` | `func_800021B4_2DB4` |

All eight are in `.file_7`, the sole tenant of its RAM slot, so none is
ambiguous. `0x80152238` — thought to be a ninth, an alternate entry point 8 bytes
into `func_80152230_531940` — turned out to be simpler than that: the bytes are
`jr $ra / nop / jr $ra / nop`, two independent do-nothing stubs that splat had
merged because nothing named the second. It now has its own symbol, and the
caller `func_801BF248_6A1C68` in `.file_23` resolves.

The recompiler reports only the *first* failure in a function, so the eight
surfaced three at a time. `lib/hybridheaven/tools/undefined_funcs_report.py`
enumerates the whole class instead of chasing it: it cross-references every entry
in `undefined_funcs_auto.ld` against the ELF's section layout and reports the
ones whose callers sit outside the target's section. It now reports only
`0x8400103C` and `0x84001064`, which are inside the two ignored rspboot
pseudo-functions and are RSP branch displacements, not addresses.

**Unnamed libultra (4 blocking, 7 named).** N64Recomp recognises libultra **by
name**, against the hardcoded sets in its `src/symbol_lists.cpp`. Hybrid Heaven's
symbol table still has ~90 libultra functions under `func_XXXXXXXX` placeholders,
so the recompiler treats them as game code and chokes on privileged instructions
it never has to translate.

All of the blockers were one run of `os/exceptasm.s`, which splat had cut into
three functions because the rest are reached by branch rather than `jal`. The
seven real ones are now named, and every one of them is in N64Recomp's
`ignored_funcs`:

| vram | name | what identifies it |
|---|---|---|
| `0x800270B0` | `__osExceptionPreamble` | `lui`/`addiu`/`jr $k0` into `0x800270C0`, nothing else |
| `0x800270C0` | `__osException` | saves `$at`–`$ra` to `__osThreadSave`, dispatches on cop0 Cause |
| `0x800275E4` | `send_mesg` | indexes `__osEventStateTab`, returns `jr $s2` |
| `0x80027698` | `handle_CpU` | masks Cause `0x30000000`, shifts by 28, sets `SR_CU1` |
| `0x80027824` | `__osDispatchThread` | the only function in the ROM that writes cop0 EPC, then `eret` |
| `0x800279A0` | `__osCleanupThread` | `jal osDestroyThread` with `$a0 = 0`, no return |
| `0x80033E00` | `__osGetCause` | `mfc0 $v0, $13` / `jr $ra` / `nop`, in full |

### Why this uses `elf_path`, not `symbols_file_path`

`mnsg.toml` drives its recompilation from `Goemon64RecompSyms/mnsg.syms.toml`.
That cannot work here. The `.syms.toml` format has **no field for a reloc's
target section** — an entry is `{ type, vram, target_vram }` — and both ends
assume the target is the containing section: `--dump-context` emits a reloc only
when `reloc.target_section == section_index` (`main.cpp`), and the parser sets
`target_section = section_index` unconditionally (`config.cpp`). Every
cross-section reloc is therefore dropped on the way out and unrecoverable on the
way in.

Since Hybrid Heaven's whole difficulty is cross-section calls, the recompilation
reads the decomp's ELF directly. Goemon gets away with symbols mode because its
`.main` never calls into an overlay. The Syms repo is still generated and still
useful for mods; it just cannot drive this build.

#### Is that the only approach? No — and the alternative was weighed (2026-08-02)

The gap is a **missing field, not a fundamental limit**. Adding `target_section`
to a `.syms.toml` reloc entry and fixing the two sites above would make symbols
mode work. It is more than two lines — section *indices* are not stable across
decomp rebuilds, so it would have to key on section *name*, and it wants a format
version so older files do not silently misparse — but it is tractable, and more
upstreamable than most of this port's patches, since the gap affects any game
with cross-section calls. `tools/n64recomp-cross-section-calls.patch` already
does the harder half: resolving such a call by runtime lookup.

The payoff would be real. Symbols mode means CI never builds the decomp, the
retail-versus-decompressed ROM distinction disappears, and the port stops
depending on a *built* decomp — which removes the two-clones hazard entirely.

**Decided against, for one reason: an ELF cannot go stale.** It is the source of
truth and `make` gates it on `61ed3d5d`. A `.syms.toml` is a *cache* of that ELF:
change the decomp, forget to regenerate, and the recompilation silently runs
against stale section data. That is precisely the failure class this project has
been bitten by seven separate times, and trading an unstaleable input for a
cacheable one — to save CI time that is now cached anyway — is a bad trade.

Two lesser options, recorded so they are not re-derived. **Publishing the built
ELF** (committed or as an artifact) skips the decomp build, but it is 42 MB and
contains the game's code, so it has the ROM's legal shape without the "player
supplies it" defence. **Splitting the decomp build into its own workflow** that
publishes the ELF keyed on the decomp SHA is the cheap improvement if the CI job
proves too slow — the cache already gets most of it, but only within one workflow.

Revisit the format change if either becomes true: outside contributors need to
build the port without a decomp toolchain, or the cross-section work gets
upstreamed properly, in which case the format fix is what makes the rest coherent.

### Why `.main` must be relocatable

`.main` calls directly into `.file_7`, a permanently resident module. N64Recomp
only loads relocations for sections marked relocatable, so `.main`'s 1773
`R_MIPS_26` relocs are otherwise dropped and those calls cannot be resolved —
`resolve_jal` refuses to bind a non-relocatable caller to a relocatable target
without a reloc. Listing `.main` in `hybridheaven.overlays.txt` loads them and
resolves 8 of the 16 failures directly. Making `.file_7` non-relocatable instead
does not work: it calls into genuinely ambiguous shared slots, and the failures
just move (16 → 100+).

## Regenerating

```bash
cd lib/hybridheaven && make setup && make      # must print 61ed3d5d...
cd ../..
N64Recomp hybridheaven.dump_context.toml --dump-context   # for mods; see below
mv dump.toml HybridHeaven-RecompSyms/hybridheaven.syms.toml
mv data_dump.toml HybridHeaven-RecompSyms/hybridheaven.datasyms.toml
N64Recomp hybridheaven.toml    # needs the patch above; reads the ELF directly
RSPRecomp aspMain.toml
make -C patches                # patches/build/patches.elf
N64Recomp patches.toml         # RecompiledPatches/, reads the .syms.toml files
```

`ld.lld: warning: cannot find entry symbol __start` from the patch link is
expected and harmless — `patches.elf` is a freestanding collection of functions
for N64Recomp to read, not a program, so it has no entry point.

The patch build needs `clang`, `ld.lld` and `llvm-nm`. Note `patches/Makefile`
sets `LD :=` rather than `?=`: GNU make predefines both `CC` and `LD`, so `?=`
never fires and the build silently uses the host toolchain. Goemon64Recomp's
Makefile has `LD ?= ld.lld` with a comment saying make does not predefine `LD` —
it does, and the result is `relocations in generic ELF (EM: 8)` from the host
linker.

Then check the hook did not silently drift:

```bash
uv run python tools/verify_overlay_hook.py
```

That is worth running after anything that changes the decomp's segmentation. The
failure it exists to catch is silent: if the game's file table and the generated
section table stop agreeing, `recomp_load_overlays` finds no section, loads
nothing, and the first indirect call into the overlay dies in `get_function`
with "Failed to find function at 0x..." — with no earlier warning.

`N64Recomp hybridheaven.toml` must exit 0 and leave 319 files in
`RecompiledFuncs/`. If it stops with "No function found for jal target", run
`uv run python tools/undefined_funcs_report.py` in `lib/hybridheaven` — a target
that crosses a section boundary needs its real name asserted in that repo's
`symbol_addrs.txt`, not an entry in `ignored` here. But note the limit of that
advice: a target in a *shared* slot cannot be fixed by naming at all, because
several tenants define a function there and asserting one is a guess. The report
distinguishes the two, and "Ambiguous call targets" above is what that class
needs instead.

**A change to the recompiler needs both rebuilds.** Editing one of the
`n64recomp-` patches means
`cmake --build ~/projects/N64Recomp-build-patched --target N64RecompCLI`
*and* re-running `N64Recomp hybridheaven.toml`; doing only the first leaves the
old generated C in place and the build looks clean.

A decomp build that no longer matches the ROM is an ELF that no longer describes
it, so check the hash before trusting a dump.
