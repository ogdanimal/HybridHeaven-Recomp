# On-screen (touch) controls

Android only. The app shipped controller-only. This adds a touchscreen control scheme
so a plain phone can play, without changing anything for a handheld that already has
sticks.

Ported from [Goemon 64: Recompiled](https://github.com/ogdanimal/Goemon64Recomp-Android),
where the overlay was contributed by [@epic-ship-it](https://github.com/epic-ship-it) in
[#25](https://github.com/ogdanimal/Goemon64Recomp-Android/pull/25).
The two ports share their Android layer and input path, so the code is the same apart
from names; what is specific to Hybrid Heaven is the action column of the table below.

- **What it looks like:** analog stick under the left thumb; A and B under the right
  with the C diamond above them; Z in the top-left corner and L beside R in the
  top right, under the index fingers; Start and a settings handle in the middle.
  The shoulders are the one place this layout differs from Goemon's: there Z sat
  just right of L, which in Hybrid Heaven is on top of the radar.
- **When it appears:** set under **Settings → Touch → On-Screen Controls**. Auto
  (the default) shows it until a gamepad is used, then hides it until the screen is
  touched again; On and Off are also available.
- **Settings:** the Touch tab also has **Stick Sensitivity** and **Edit Layout**.
  Long-press the ☰ handle for size, opacity and vibration. A short tap on ☰ opens
  the game's own menu (it is the on-screen stand-in for Select).
- **Reaching the tab:** Touch is the last tab, and with the Saves and GPU Driver tabs
  present the row is wider than the menu, so it scrolls sideways. Drag the row with a
  finger; a controller scrolls it automatically as focus moves. RmlUi 6.0 has no
  drag-scrolling of its own, so this is `TabStripDragScroller` in
  `src/ui/ui_config.cpp`; a drag swallows the click that ends it, so dragging never
  switches tabs.

---

## How it works

### The overlay is an Android View, not part of the renderer

The pad is a transparent `View` composited over SDL's `SurfaceView`, drawn with
`Canvas`. The recomp has its own RmlUi layer and drawing the pad there was the obvious
alternative; a View wins on the things that matter here. It needs no knowledge of RT64
or Vulkan, so changing it never touches the native build, and it gets Android's
multi-touch, haptics and safe-area insets for free.

Everything is drawn as vectors, no bitmaps: the APK does not grow, it stays sharp at
any density, and restyling is an edit to one file rather than an image pipeline.

### It presents itself as an extra gamepad

The one seam into native code is a virtual gamepad shaped exactly like an SDL game
controller — a button bitmask and an axis array. `input.cpp` merges it inside
`controller_button_state()` and `controller_axis_state()`, the same two functions
every physical pad already flows through.

That single decision is what keeps the feature small:

- Every binding in **Settings → Controls** applies to the on-screen buttons unchanged.
  Rebind B and the on-screen B moves with it.
- A rebind can never detach it, because it has no bindings of its own.
- It **merges** with a physical pad rather than fighting it, exactly as two physical
  pads already do.
- Analog-camera mode, C-button masking, the autosave combo and mods need no knowledge
  that it exists.

The alternative — emitting N64 buttons directly — would have needed a second, parallel
binding system that could silently drift out of step with the real one.

### What each control emits

| On-screen | SDL input | N64 | In game |
|---|---|---|---|
| Stick | `AXIS_LEFTX` / `LEFTY` | Analog stick | Walk, run, examine, open doors (field) · toggle items (mode select) |
| A | `BUTTON_A` | A | Jump, talk, climb, fire defuser (field) · select (mode select) |
| B | `BUTTON_B` | B | View map (field) · cancel (mode select) |
| C▲ ▶ ▼ ◀ | `BUTTON_DPAD_UP/RIGHT/DOWN/LEFT` | C-buttons | Rotate camera (field) |
| Z | `AXIS_TRIGGERLEFT` | Z | Crawl (field) · run (battle) |
| R | `AXIS_TRIGGERRIGHT` | R | Pick up defuser (field) · scroll items (menu) · grab enemy (battle) |
| L | `BUTTON_LEFTSHOULDER` | L | Unused by the game; for mods |
| Start | `BUTTON_START` | Start | Field ↔ menu screen (field) · help (battle) |
| ☰ | `BUTTON_BACK` | — | Opens this app's settings menu |

The in-game actions are the retail manual's, the same wording as the README's controls
table.

**Why the C-buttons emit D-pad.** Each C direction has up to three stock bindings: a
face/shoulder button, the right stick, and the D-pad. The D-pad is the only one that
covers all four directions (C-Right has no face button at all) *and* survives
analog-camera mode, which suppresses the right stick. Hybrid Heaven never reads the
N64 D-pad, which is why a physical D-pad is bound to the C-buttons in the first place. So D-pad is the binding that
makes the on-screen C cluster behave the same either way.

### Details that matter in the hand

- **Hit targets are bigger than the artwork.** A thumb's contact patch is wide and its
  reported centre lands low of where the player thinks they pressed.
- **Hit-testing runs smallest control first**, so a deliberate press on a small C
  button is never swallowed by the stick's generous margin.
- **State, not edges.** Every event recomputes the whole button mask from the live
  pointers, rather than keeping per-button hold counters. Two fingers on one button
  work either way, but only this version has no counter that can drift, so a gesture
  cancelled by the system cannot strand a button held forever.
- **The overlay owns the whole gesture** while it is shown. Android delivers every
  pointer of a gesture to whichever view claimed its `ACTION_DOWN`; letting an
  empty-space touch fall through to SDL would hand SDL the rest of the gesture, and a
  thumb resting on the picture would silently kill the buttons under the other hand.
- **The ☰ handle is click-on-release**, alone among the controls. A tap opens the
  game's menu and a long press opens the overlay's settings, and those are only
  distinguishable once the finger lifts. Every other control fires on contact.
- **The pad hides for menus**, polled from
  `recompui::is_context_capturing_input_snapshot()` — a per-frame atomic, never the
  `ui_state_mutex`-guarded call, because the poll runs on Android's main thread — and
  stops consuming touches entirely so SDL's touch-to-mouse emulation can drive the
  RmlUi menu underneath.
- **A tap is never shorter than one game input poll.** The game samples input once
  per poll and the overlay reports what is held right now, so a tap that started and
  ended between two polls used to be lost. `android_touch.cpp` now records every
  button that goes down and hands it to the next poll (`latch_for_poll`, called from
  `recomp::poll_inputs`), so it reads as held for that one poll even if the finger
  has lifted. Only rising edges are recorded: a press still down at the poll, and its
  release, are unaffected. Not a fixed minimum hold, which would lengthen presses
  that did not need it and merge rapid taps. Measured in Goemon 64: Recompiled on
  the RP5 with `adb shell input tap` (a press of a few ms) on its save-select
  screens: 1 of 12 taps registered before, 16 of 16 after.
- **Sizes scale off `TouchLayout.sizingUnit()`** — the height of the widest 16:9 box
  that fits. Sizing off height alone oversizes buttons on a 4:3 screen until they
  collide; off width alone they balloon on a 21:9.

---

## Where the code is

| File | What it does |
|---|---|
| `touch/TouchControl.java` | The control vocabulary and the SDL input each one emits |
| `touch/TouchLayout.java` | Geometry, sizing, safe-area clamping, persistence |
| `touch/TouchPad.java` | Multi-touch state machine → button mask + axes |
| `touch/TouchOverlayView.java` | Drawing and touch dispatch |
| `touch/TouchOverlayController.java` | Installs the view, polls menu state, settings + editor |
| `touch/TouchPrefs.java` | SharedPreferences persistence |
| `touch/NativeTouch.java` | The JNI seam |
| `src/main/android_touch.cpp` | Virtual pad state + JNI entry points |
| `include/hh_touch.h` | The API `input.cpp` reads |
| `src/game/input.cpp` | The merge into the physical-pad path |
| `src/ui/ui_state.cpp` | Publishes the per-frame menu-open snapshot the overlay polls (`is_context_capturing_input_snapshot`), so the poll never waits on `ui_state_mutex` |
| `src/ui/ui_config.cpp` | The Touch tab's bindings, and `TabStripDragScroller` for the scrolling tab row |

### The SDL constants are duplicated

JNI cannot read a C enum, so `TouchControl.Sdl` holds a copy of the SDL button and axis
values. `android_touch.cpp` static-asserts every one against the real enum, and the
failure message names `TouchControl.java`. If SDL is ever bumped and renumbers, the
native build breaks loudly instead of shipping a pad where every button quietly presses
the wrong thing.

---

## Known gaps

- **No layout profiles.** One layout, not a set you can switch between. The editor and
  the serialisation format would both take it, but nothing selects among them yet.
- **The stick has no floating mode.** The base is fixed. A "recentre where the thumb
  lands" option is a common preference and is not implemented.
- **The editor moves and resizes controls, but does not rotate or reshape them.**
- **The labels assume the default bindings.** The on-screen "A" sends the controller's
  A button, not N64 A, so it means whatever **Settings → Controls** has bound to that
  button. With the stock bindings every label is right; after a remap, the labels can
  be wrong.
- **There is no right stick.** Analog Camera mode is driven by the right stick, and
  its recentre by R3, so neither can be used from the touchscreen alone. The C
  diamond still works in that mode, because it emits D-pad rather than right-stick
  input.
- **No per-orientation layouts.** The app is landscape-locked
  (`android:screenOrientation="landscape"`), so there is only one to store. If that
  lock is ever lifted, layouts would need storing per orientation.
