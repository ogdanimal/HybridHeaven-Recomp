# Building Guide

This covers building **Hybrid Heaven: Recompiled** from a clean clone, for
Linux, Windows and Android.

Building requires you to supply a **ROM of the US version of the game**. It is a
build-time input only, and no game data is ever shipped in the resulting binary
or APK.

The shape of it: clone with submodules, install the toolchain, build the
recompiler, produce the decompressed ROM from your cart dump, generate the C
sources, then build for whichever platform you want. Steps 1–5 are shared;
step 6 is per-platform.

## 1. Clone the repository (with submodules)

```bash
git clone --recurse-submodules https://github.com/ogdanimal/HybridHeaven-Recomp
cd HybridHeaven-Recomp
# if you forgot --recurse-submodules:
# git submodule update --init --recursive
```

**`--recursive` is not optional.** `lib/libadrenotools` (used only by the Android
custom-driver build) has a submodule of its own, and a plain `--init` leaves it
empty. The failure that produces is a CMake error from inside
`add_subdirectory(lib/linkernsbypass)`, which does not mention submodules at all.

The recompiler and runtime carry six local patches this port depends on. They are
**committed**, one commit each, on a `hybridheaven-port` branch in each of the
submodules that needs them, so a recursive clone gets them — you do not have to
apply anything by hand. Two of those branches live on forks under the same owner
(`ogdanimal/N64ModernRuntime` and `ogdanimal/N64Recomp`) rather than upstream,
which is why the recursive clone resolves at all: upstream does not have these
commits, and until 2026-08-02 the nested `N64Recomp` submodule still pointed
there and no clone of this repo could complete.

`tools/*.patch` keeps a readable record of what each one changes and why; see
"The prerequisites" in [`docs/development-notes.md`](docs/development-notes.md)
if you need to rebuild those branches on a newer upstream.

## 2. Install dependencies

### Host toolchain (Linux / WSL)

```bash
sudo apt-get install -y ninja-build cmake clang lld llvm make binutils-mips-linux-gnu
```

`clang`, `ld.lld` and `llvm-nm` are what link the MIPS patch ELF; the MIPS
binutils are for the decompilation in step 4.

The decomp uses [uv](https://astral.sh) to manage Python and its dependencies:

```bash
curl -LsSf https://astral.sh/uv/install.sh | sh
```

### Android SDK (only for step 6c)

Install through Android Studio or the command-line SDK tools, then add the
pinned versions the native build expects:

- **NDK 27.1.12297006**
- **CMake 3.22.1**
- **JDK 17**, compile SDK **34**

```bash
sdkmanager "ndk;27.1.12297006" "cmake;3.22.1"
```

`android/local.properties` points at your SDK and is per-clone.

### Windows SDK (only for step 6b)

The Windows build is cross-compiled from WSL with `clang-cl`, reading the MSVC
toolset and Windows SDK over `/mnt/c`. No Visual Studio invocation is involved
beyond its headers and import libraries. The toolchain file expects two symlinks,
and tells you so if they are missing:

```bash
ln -sfn '/mnt/c/Program Files (x86)/Microsoft Visual Studio/<VS>/<edition>/VC/Tools/MSVC/<version>' ~/.local/win/vctools
ln -sfn '/mnt/c/Program Files (x86)/Windows Kits/10' ~/.local/win/sdk
```

## 3. Build the recompiler

`N64Recomp` and `RSPRecomp` come from the pinned, patched submodule — build
*that* copy, not an upstream checkout, or the generated code will be missing the
fixes this port needs:

```bash
cmake -S lib/N64ModernRuntime/N64Recomp -B build-n64recomp -G Ninja
cmake --build build-n64recomp
```

You also need rt64's `file_to_c` host tool, which runs during the build and never
ships:

```bash
mkdir -p build-host-tools
c++ -std=c++17 -O2 -o build-host-tools/file_to_c_host \
    lib/rt64/src/tools/file_to_c/file_to_c.cpp
```

## 4. Produce the decompressed ROM

**Two different NTSC-U Hybrid Heaven ROMs are involved, with two different
hashes.** Mixing them up is the easiest way to fail this build.

| | Size | sha1 | Used for |
|---|---|---|---|
| Retail cart dump | 16 MiB | `16dbc21620b52deab5c5abf8a309ac60adfbee85` | What the finished **app** asks a player for and verifies at runtime. Not a build input. |
| Decompressed ROM | 32 MiB | `61ed3d5d5390c3c17b4026797b93be50a61dd6f2` | The **build** input — what the recompiler reads. |

The decompilation in `lib/hybridheaven` produces the second from the first:

```bash
cd lib/hybridheaven
cp "/path/to/Hybrid Heaven (USA).z64" config/usa/baserom.z64
make setup      # decompress the archive, then split with splat
make            # must verify against 61ed3d5d5390c3c17b4026797b93be50a61dd6f2
cd ../..
cp lib/hybridheaven/config/usa/baserom.decompressed.z64 hybridheaven.z64
```

`hybridheaven.z64` at the repository root is the path `hybridheaven.toml` and
`aspMain.toml` expect. Putting the 16 MiB cart dump there instead will not work.

A decomp build that no longer matches the ROM is an ELF that no longer describes
it — **check the hash before trusting anything downstream.**

## 5. Generate the C sources (host)

From the repository root:

```bash
# Symbol tables, for mods
./build-n64recomp/N64Recomp hybridheaven.dump_context.toml --dump-context
mv dump.toml       HybridHeaven-RecompSyms/hybridheaven.syms.toml
mv data_dump.toml  HybridHeaven-RecompSyms/hybridheaven.datasyms.toml

# Game  ->  RecompiledFuncs/*.c        (must exit 0 and leave 319 files)
./build-n64recomp/N64Recomp hybridheaven.toml

# RSP microcode  ->  rsp/aspMain.cpp
./build-n64recomp/RSPRecomp aspMain.toml
```

The patch codegen (`make -C patches` then `N64Recomp patches.toml`) is run for
you by the Linux CMake build in step 6a. Windows and Android **cannot** run it —
it needs `make`, `clang` and `ld.lld` to link a MIPS ELF — so they consume
`RecompiledPatches/` as static sources and fail the configure with an explanation
if it is absent. **Configure once on Linux before building either.**

> [!NOTE]
> `RecompiledFuncs/` and `RecompiledPatches/` are **generated, not committed**.
> A clean clone cannot build without this step.

`ld.lld: warning: cannot find entry symbol __start` from the patch link is
expected and harmless: `patches.elf` is a freestanding collection of functions for
the recompiler to read, not a program, so it has no entry point.

If `N64Recomp hybridheaven.toml` stops with `No function found for jal target`,
run `uv run python tools/undefined_funcs_report.py` inside `lib/hybridheaven` —
that report distinguishes a target needing its real name asserted from one in a
shared overlay slot, which naming cannot fix at all.

## 6a. Build for Linux

```bash
cmake -S . -B build -G Ninja -DHH_N64RECOMP=$PWD/build-n64recomp/N64Recomp
cmake --build build
./build/HybridHeavenRecompiled "/path/to/Hybrid Heaven (USA).z64"   # the RETAIL dump
```

The ROM argument is only needed once: it is validated and copied under the config
directory, and the launcher picks it up from there afterwards.

Two build options, and they are coupled:

| option | default | effect |
|---|---|---|
| `HH_RT64` | `ON` | The renderer. `OFF` builds `null_render_context.cpp`, which runs everything below the display list without standing up RT64 — the bisect build. |
| `HH_UI` | `ON` | The launcher and config menus. **Forced `OFF` by `-DHH_RT64=OFF`**, since the UI draws through RT64's render hooks and its shaders are compiled by rt64's DXC rules. |

With `-DHH_UI=OFF` the game starts directly against the stored ROM and polls a
fixed keyboard layout of its own, exactly as this port worked before the launcher
existed.

## 6b. Build for Windows (cross-compiled from WSL)

```bash
cmake -S . -B build-win -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-windows-clang-cl.cmake \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DRT64_FILE_TO_C=$PWD/build-host-tools/file_to_c_host
ninja -C build-win HybridHeavenRecompiled
```

That produces `build-win/HybridHeavenRecompiled.exe` with `SDL2.dll`, `dxil.dll`
and `dxcompiler.dll` beside it, plus `assets/` and `recompcontrollerdb.txt` — so
it runs where it is built, and from WSL directly through binfmt interop.

Three things to know:

- **`RT64_FILE_TO_C` must point somewhere durable, and `/tmp` is not.** It is
  recorded in `build-win/CMakeCache.txt` and consulted on every later build, so a
  scratch path leaves the tree looking configured while the next build dies with
  `missing and no known rule to make it` on a shader.
- **clang-cl, never `cl.exe`.** The configure step refuses MSVC outright: its
  `RECOMP_FUNC` has no weak linkage, and the patch override works *because* those
  definitions are weak. A loud configure error beats a silently patch-less binary.
- **Set `"api_option": "Vulkan"`** in `graphics.json` before running it. `Auto`
  picks D3D12, which aborts before it can even name a device — `dxc-linux` emits
  DXIL and cannot sign it, and a D3D12 device is the only thing that checks.

## 6c. Build the Android APK

The app is a Gradle module under `android/` that drives the same CMake build for
`arm64-v8a`. The main target becomes `libHybridHeaven.so` rather than an
executable, because SDLActivity loads a library and calls `SDL_main` inside it.

```bash
cd android
./gradlew :app:assembleDebug
```

The debug APK lands at `android/app/build/outputs/apk/debug/app-debug.apk`.

To include the **user-supplied Vulkan driver** feature (Adreno devices; see
Optional Features in the README), add one flag — which deliberately drives four
separate things at once, because any of them alone is broken:

```bash
git submodule update --init --recursive lib/libadrenotools
cd android && ./gradlew :app:assembleDebug -PcustomDriver=true
```

For a **release** APK, supply a signing keystore via `keystore.properties` (repo
root or `android/`) and run `./gradlew assembleRelease`. A release signed with a
different key will not install over an existing install.

## 7. Check it

Neither check needs the build, so both are cheap to run at any point:

```bash
cmake --build build --target check   # verify_overlay_hook.py + recomp_symbol_gap.py
```

`verify_overlay_hook.py` is worth running after anything that changes the decomp's
segmentation. The failure it exists to catch is silent: if the game's file table
and the generated section table stop agreeing, nothing loads and the first
indirect call into an overlay dies with `Failed to find function at 0x...` and no
earlier warning.

Two further checks run on their own — `patches/Makefile` fails the link on an
undefined `recomp_*` symbol, and the port complains at runtime if 60 RSP tasks go
by without the overlay loader hook running once. That last one exists because
whether a patch actually overrode is a link-time property no table lookup
reveals, and this port shipped one build where it had not.

**A change to the recompiler needs both rebuilds** — rebuilding
`build-n64recomp` *and* re-running `N64Recomp hybridheaven.toml`. Doing only the
first leaves the old generated C in place and the build looks clean.

## 8. Success

Run the desktop binary, or sideload the APK onto an arm64 Android device. It is
not an emulator and ships no game assets — on first launch it asks you to select
your own ROM.

> [!IMPORTANT]
> In the app you provide the **retail** cart dump, not the decompressed one. The
> decompressed ROM is a build-time input only. The port expands the game's
> internal archive in memory as it loads.

## 9. CI and releases

Two workflows, both in `.github/workflows/`:

| | trigger | does |
|---|---|---|
| `validate.yml` | push to `dev`/`main` | builds Linux, Windows and a debug APK; publishes nothing |
| `release.yml` | a `v*` tag on `main` | the same builds, versioned, with a **signed** APK — then one GitHub Release |

`release.yml` does not repeat the build: it calls `validate.yml` with
`release: true`, so a release is built exactly the way every push is checked. It
refuses a tag that is not on `main` or is not `vMAJOR.MINOR.PATCH[-suffix]`
(a `-suffix` makes it a prerelease), and refuses to publish if any package holds a
ROM, an ELF or generated game source.

**Secrets** (repository settings → Secrets and variables → Actions):

| secret | used by | holds |
|---|---|---|
| `HH_ARTIFACT_KEY` | both | any long random string (`openssl rand -base64 48`). Encrypts the `codegen` artifact, which holds the game's code as C, because a public repo's artifacts are downloadable by anyone signed in. Nobody needs to know it |
| `HHRS_REPO_WITH_PAT` | both | `https://x-access-token:<PAT>@github.com/ogdanimal/HybridHeaven-RecompSecrets.git` — the ROM repo |
| `RELEASE_KEYSTORE_BASE64` | release | `base64 -w0 release.jks` |
| `RELEASE_STORE_PASSWORD` | release | keystore password |
| `RELEASE_KEY_ALIAS` | release | key alias |
| `RELEASE_KEY_PASSWORD` | release | key password (the same as the store password for a PKCS12 keystore) |

Fine-grained PATs **expire**. When one does, every run fails within two minutes
at "Fetch private build inputs (ROM)", with an authentication
error that says nothing about the build.

**Creating the signing key — once, and keep it.** Android only installs an update
signed with the same key as the installed app, so losing it means every player
has to uninstall (deleting their saves) to take the next release:

```bash
keytool -genkeypair -v -storetype PKCS12 -keystore release.jks -alias hybridheaven-upload -keyalg RSA -keysize 4096 -validity 10000
```

Keep `release.jks` and its password somewhere backed up and **outside** any
repository (`*.jks` and `keystore.properties` are gitignored, but do not rely on
that). `keytool` ships with any JDK.

**Branches** follow Goemon64Recomp: work goes to `dev`; `main` only moves when a
release is cut, by fast-forwarding it to `dev`. Versions follow Goemon's too —
`v1.0.0` first, a patch bump per release, `-rc1` for a candidate (published as a
prerelease, never "Latest").

**Cutting a release**, once the secrets are in place:

```bash
git push origin dev:main && git tag v1.0.1 dev && git push origin v1.0.1
```
