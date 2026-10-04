# Hybrid Heaven: Recompiled

A native port of **Hybrid Heaven** (Konami, 1999) to **Windows, Linux and Android** — it plays the game natively on your PC, phone or handheld, with no emulator. The N64 code is statically recompiled with [N64: Recompiled](https://github.com/N64Recomp/N64Recomp) and runs on [N64ModernRuntime](https://github.com/N64Recomp/N64ModernRuntime), using [RT64](https://github.com/rt64/rt64) for rendering — the same stack as [Goemon 64: Recompiled](https://github.com/klorfmorf/Goemon64Recomp).

The decompilation it is built on lives in [`lib/hybridheaven`](lib/hybridheaven). For how the port itself works, see [BUILDING.md](BUILDING.md) and [`docs/development-notes.md`](docs/development-notes.md).

## Download

Get a build from the releases page:

**[Releases](../../releases)**

| Platform | File |
|---|---|
| Windows | `HybridHeaven-<version>-windows-x64.zip` |
| Linux | `HybridHeaven-<version>-linux-x86_64.tar.gz` |
| Android | `HybridHeaven-<version>-android-arm64-v8a.apk` |

Versions ending in `-rc1`, `-rc2`… are release candidates, published as prereleases for testing.

No build includes the game. You'll need your own legally obtained ROM.

## Getting Started

1. Install it:
   - **Windows** — unzip, and run `HybridHeavenRecompiled.exe` from inside the folder.
   - **Linux** — extract, and run `./HybridHeavenRecompiled` from inside the folder (it finds its `assets/` relative to where you run it).
   - **Android** — install the APK.
2. On first launch you'll be asked to pick your ROM. Use the file picker; the ROM is verified and copied into the app's own storage, so you only do this once.
3. Press **Start Game**.

**The ROM is an ordinary retail Hybrid Heaven (USA) cartridge dump** — sha1 `16dbc21620b52deab5c5abf8a309ac60adfbee85`. Nothing has to be decompressed or converted first; the port unpacks the game's internal archive itself as it loads.

**Requirements:**

| | |
|---|---|
| **Windows** | 64-bit, and a GPU with Direct3D 12 or Vulkan |
| **Linux** | A Vulkan-capable GPU, plus SDL2 and GTK 3 (`libsdl2-2.0-0`, `libgtk-3-0` on Debian/Ubuntu) |
| **Android** | 9.0+, a 64-bit (`arm64-v8a`) device, and a Vulkan-capable GPU |

Keyboard and gamepad both work on desktop. On Android a handheld's built-in controls work as-is, a physical or Bluetooth pad works, and a phone with neither gets [on-screen touch controls](#on-screen-controls).

Tested on a **Retroid Pocket 5** (Android 13, Snapdragon 865) and on Windows with an RTX 5080 (Direct3D 12 and Vulkan).

## Something's Wrong — Quick Fixes

| Problem | Try this |
|---|---|
| **Android:** the game closes itself a few seconds after starting | Likely a bug in your device's own GPU driver. Turn off **Settings → Graphics → Framebuffer Effects**, *or* load a different driver (see [GPU Driver](#gpu-driver)). |
| Want to bring a save over from another device or a backup | **Settings → Saves → Choose a save file** — no adb or file manager needed. See [Importing a Save](#importing-a-save). |
| **Android:** replacing a test or self-built APK fails to install, or asks you to uninstall first | It was signed with a different key. Uninstalling **deletes the app's saves** — copy `hybridheaven.us.bin` off the device first, then bring it back with [Importing a Save](#importing-a-save). |
| "Failed to write to the save file" | The message names the exact file and what the system said about it, which usually points at the cause. If it doesn't, [open an issue](../../issues) with that text. |
| **Windows:** it asks for the ROM again, but you already picked one | The ROM lives under `%LOCALAPPDATA%\HybridHeavenRecompiled` — **Local**AppData, not `%APPDATA%`. |
| Your D-pad doesn't move your character | Working as intended — the game itself only moves with the stick. See [Troubleshooting Details](#troubleshooting-details). |
| Some controller buttons do nothing, and don't respond when rebinding | Your system doesn't recognize your pad's exact model. [Open an issue](../../issues) with the pad model and your OS / Android version. |
| Still stuck | [Open an issue](../../issues) with your platform, device model and OS / Android version. |

More detail on each of these is in [Troubleshooting Details](#troubleshooting-details) below.

## Default Controls

Everything is remappable in **Settings → Controls**.

### Gamepad

Face buttons are positional (Xbox layout): **A** is the bottom button, **B** the right, **X** the left and **Y** the top, whatever your pad prints on them. The in-game actions are the ones the original manual lists, by the screen they apply to: **field**, **battle**, **menu** and **mode select**.

| Button | N64 | In-game action |
|---|---|---|
| A | A | Jump, talk to characters, climb up levels, fire defuser (field) · select item / go forward (mode select) |
| B | B | View map (field) · cancel / go backward (mode select) |
| X | C-Up | Rotate camera (field) |
| Y | C-Left | Rotate camera (field) |
| Right Bumper | C-Down | Rotate camera (field) |
| Left Trigger | Z | Crawl (field) · run (battle) |
| Right Trigger | R | Pick up defuser (field) · scroll items (menu) · grab enemy (battle) |
| Left Bumper | L | Unused by the game (free for mods) |
| Left stick | Analog stick | Walk, run, dash, examine objects, open doors, climb ladders (field) · toggle through items (mode select) |
| Right stick | C-buttons | Rotate camera — drives the analog camera instead when Analog Camera is on |
| D-Pad ↑ ↓ ← → | C-Up / C-Down / C-Left / C-Right | Rotate camera (field) |
| Right stick click (R3) | — | Hand the camera back to the game when Analog Camera is on |
| Start | Start | Toggle field screen ↔ menu screen (field) · help (battle) |
| Select | — | Open this app's settings menu |

The right stick and D-Pad both cover the C-buttons, so with Analog Camera on, the right stick orbits while the D-Pad keeps the C-button actions.

### Keyboard

| Key | N64 |
|---|---|
| Space | A |
| Left Shift | B |
| Q | Z |
| E | L |
| R | R |
| W A S D | Analog stick |
| Arrow keys | C-buttons |
| I J K L | D-pad |
| Enter | Start |
| Escape | Open this app's settings menu |
| F | Apply (in the settings menu) |

In-game actions are the same as the gamepad table — only the physical control differs.

## Optional Features

Everything below is opt-in and lives in the in-game settings menu.

- **Analog Camera** — free-look with the right stick while you walk, which the game's own C-button camera can't do. Hold **Right Bumper** + push the stick to zoom. Per-axis invert and sensitivity settings included. Click **R3** to hand control back to the game. It's yours during exploration and stays out of the way in cutscenes, on ladders and in battle — and holding **R** releases it, since the game uses R itself.
- **Autosave** — saves through the game's own save routine, to the slot the save menu last selected, so it's an ordinary save rather than a separate slot. **Back up your save before enabling this** — it overwrites that slot. Manual save: **L + R + Z** during normal play; otherwise every 2 minutes. It won't fire at unsafe moments — cutscenes, elevators, battles, or while a file is loading. Details: [`docs/autosave.md`](docs/autosave.md).
- **Widescreen** — **Settings → Graphics → Aspect Ratio**: `Expand` fills the window, `Original` keeps 4:3.
- **Higher internal resolutions**, **MSAA**, **V-Sync** and **framerate** options — all under **Settings → Graphics**.
- **Restart Game** *(Android)* — the button at the top of the settings menu restarts to the title screen or back to this app's launcher, without closing the app.
- **Mods** — the mod menu is present (**Settings → Mods**).
- **Import a save** — see below.
- **GPU Driver** *(Android)* — see below.
- **On-Screen Controls** *(Android)* — see below. On by default, but only until a gamepad is used.

### Importing a Save

<a id="importing-a-save"></a>

**Settings → Saves** brings in a save file you already have — from this port on another device, or a backup. It opens the system file picker and the app copies the file into place itself, so on Android you don't need adb or a file manager that can reach `Android/data`. Saves are the same file on every platform, so a Windows save imports on Android and the other way round.

The file is checked before anything is replaced, the same way the game's own loader checks it: it has to be the right size, carry Hybrid Heaven's save header, and hold at least one saved game. A save for a different game, an empty save, or a file from another emulator that happens to be the same size is refused rather than imported. Whatever save was already there is kept alongside the new one as `hybridheaven.us.bin.pre-import.bak`, so an import can be undone by renaming that file back.

Importing is only possible before you start the game. Once it's running the game holds your save in memory and writes it back out as it goes, so a file swapped underneath it would be overwritten.

Controller Pak images from other emulators (`.mpk`) are a different format and can't be imported yet.

### GPU Driver

<a id="gpu-driver"></a>

On Adreno devices, **Settings → GPU Driver** lets you load a different Vulkan driver instead of your device's own — useful when the system driver is what's broken (see the crash fix above). It's per-app: nothing on your device is modified, no root needed.

Import a driver `.adpkg` or `.so` through the file picker and restart when asked. [Mr. Purple's purple-turnip](https://github.com/MrPurple666/purple-turnip/releases) builds are the usual source for Adreno handhelds; which build works best depends on your chip and Android version, and no particular build has been tested with Hybrid Heaven yet. A driver that doesn't work costs you nothing but a restart: the app confirms a new driver actually works before keeping it, and falls back to the system driver otherwise.

The available replacement drivers are Adreno/Qualcomm builds, so there's nothing useful to import on Mali or other hardware.

### On-Screen Controls

<a id="on-screen-controls"></a>

*Android only. Contributed to [Goemon64Recomp-Android](https://github.com/ogdanimal/Goemon64Recomp-Android) by [@epic-ship-it](https://github.com/epic-ship-it) in [#25](https://github.com/ogdanimal/Goemon64Recomp-Android/pull/25), and ported here.*

On a device with no gamepad, a full N64 pad is drawn over the game: analog stick under the left thumb, A and B under the right with the C-buttons above them, Z in the top-left corner and L/R top right, and Start in the middle.

By default it **hides as soon as a gamepad is used** and comes back the next time you touch the screen, so a handheld with real controls never has it in the way and a phone never has to go looking for a setting.

Whether it appears at all is under **Settings → Touch → On-Screen Controls** (Auto / On / Off). The same tab has **Stick Sensitivity**, which softens the stick near its centre so slow walking is easy to hold on a small screen without losing top speed, and **Edit Layout**, which lets you drag the controls wherever your hands want them, over the running game. Touch is the last tab; if it is off the edge of the screen, drag the row of tabs sideways to reach it.

**Long-press the ☰ handle** for size, opacity and vibration. A short tap on ☰ opens this app's settings menu (☰ is the on-screen stand-in for Select).

The on-screen buttons go through the same bindings as a physical controller, so anything you remap in **Settings → Controls** moves them too, and they work alongside a real pad rather than instead of it. There is no on-screen right stick, so **Analog Camera** needs a gamepad; the on-screen C-buttons still turn the game's own camera.

Full detail: [`docs/touch-controls.md`](docs/touch-controls.md).

## Troubleshooting Details

<a id="troubleshooting-details"></a>

The quick fixes above cover the common cases. This section explains *why*, for anyone who wants it.

**Android crash a few seconds into the game.** On Goemon 64: Recompiled, which shares this renderer, some Qualcomm Vulkan drivers were found to crash on a call the renderer makes for framebuffer effects — a bug in the driver, not the game. The same settings exist here. Either workaround is enough on its own: **Graphics → Framebuffer Effects → Off** avoids the call (works everywhere, at the cost of some visual effects), or **Settings → GPU Driver** runs the game on a different driver entirely (Adreno only). Both are reachable on an affected device, because this app's own menu comes up before the game starts rendering.

**The D-pad doesn't move you.** Hybrid Heaven never reads the N64 D-pad: it takes all movement from the analog stick, and the original manual lists no action for the Control Pad at all. A D-pad bound to the N64 D-pad would do nothing, so by default yours drives the C-buttons (camera) instead, which the game does read.

**Controller buttons that do nothing.** Controllers are recognized through SDL's controller database, bundled with the app. If your pad is newer than your system knows about, it can be handed over as a generic device with some buttons in slots that have no defined meaning — those buttons do nothing in *any* app, and remapping can't reach them. Reports with the pad model let a database entry be added.

**Building Windows yourself.** Release builds work on Direct3D 12 and Vulkan. A Windows build cross-compiled from Linux/WSL can't sign its Direct3D 12 shaders and closes instantly on the default renderer; set `"api_option": "Vulkan"` in `graphics.json` for those. See [BUILDING.md](BUILDING.md).

## ROM and Storage

This is not an emulator and doesn't include copyrighted game assets. On first launch, the ROM you select is hash-verified and copied into the app's own storage — on Android that's app-scoped, with no manual folder setup or legacy storage permissions. Saves and settings live in the same place:

| Platform | Location |
|---|---|
| Windows | `%LOCALAPPDATA%\HybridHeavenRecompiled` |
| Linux | `~/.config/HybridHeavenRecompiled` |
| Android | `Android/data/com.hybridheaven.recomp/files/data` |

The Android location is awkward to reach by hand on modern Android, which is why moving a save in is done through [Importing a Save](#importing-a-save) rather than by copying files around. On desktop you can also copy `hybridheaven.us.bin` into `saves/` directly — with the game closed, since it writes its in-memory copy back on exit.

The port never modifies your ROM file, and the shipped builds contain no game data: the recompiler reads the game at *build* time, and the app loads the cartridge dump you supply.

## Building

A CMake build for desktop, and a Gradle module under `android/` that drives the same CMake build for the APK:

- Initialize submodules first: `git submodule update --init --recursive`
- **A supported ROM is required at build time.** `RecompiledFuncs/` and `RecompiledPatches/` are generated (not committed) by building the decompilation and running the recompiler against it; the ROM's data never ships in a build. A clean clone can't build without this step.
- Android: **NDK 27.1.12297006**, **CMake 3.22.1**, native code for `arm64-v8a`
- A release keystore can be supplied via `keystore.properties` (repo root or `android/`)

See **[BUILDING.md](BUILDING.md)** for the full step-by-step for all three platforms, CI, and cutting a release. [`docs/development-notes.md`](docs/development-notes.md) is the engineering journal behind it.

## Credits

- [N64: Recompiled](https://github.com/N64Recomp/N64Recomp) and [N64ModernRuntime](https://github.com/N64Recomp/N64ModernRuntime) contributors
- [RT64](https://github.com/rt64/rt64) contributors
- [Goemon 64: Recompiled](https://github.com/klorfmorf/Goemon64Recomp) contributors — this port's launcher, settings menus and input layer are theirs
- [Goemon64Recomp-Android](https://github.com/ogdanimal/Goemon64Recomp-Android), whose Android layer this port's comes from, and [@linkzenic](https://github.com/linkzenic)'s [Zelda64Recomp-Android](https://github.com/linkzenic/Zelda64Recomp-Android), which paved the way for it
- [Zelda64Recomp](https://github.com/Zelda64Recomp/Zelda64Recomp), the base those projects build on
- [@epic-ship-it](https://github.com/epic-ship-it) — the [on-screen touch controls](#on-screen-controls), written for Goemon64Recomp-Android ([#25](https://github.com/ogdanimal/Goemon64Recomp-Android/pull/25))
- The [mnsg](https://github.com/klorfmorf/mnsg) decompilation of Mystical Ninja Starring Goemon, whose archive tooling made Hybrid Heaven's ROM format tractable
- SDL contributors

### Artwork

| What | File | By |
|---|---|---|
| Launcher background | `assets/launcher_background.png` | **I_LIKE_SONIC** |
| Application icon | `assets/icon.png`, `icons/app.ico` | **SolarisTM** |

Both were uploaded to SteamGridDB by their authors and are credited here by the handles they published under. **That credit travels with any build** — keep it in place, and keep it accurate, if these files are moved or re-derived. The shipped copies are re-encoded from the originals (8-bit, resized, and the background extended to 16:9); the derivation is recorded under "The artwork" in [`docs/development-notes.md`](docs/development-notes.md) so the originals can always be recovered.

## License

GPL-3 — see [`COPYING`](COPYING). The launcher, settings menus and input layer come from Goemon64Recomp, which is GPL-3.
