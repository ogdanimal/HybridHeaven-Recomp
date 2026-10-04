/**
 * Entry point.
 *
 * Registers the game, wires up the callbacks librecomp requires, and hands over
 * to `recomp::start`. Goemon64Recomp's src/main/main.cpp is the model, and the
 * launcher, config menus and input remapper now come from it too (src/ui/).
 *
 * Two builds come out of this file:
 *
 *   HH_UI on (default) -- the launcher owns startup. `recomp::start` shows the
 *       RmlUi launcher, and the game boots when the player picks Start Game.
 *       Input, config and the SDL event pump come from src/game/, so the
 *       keyboard is remappable and a gamepad works.
 *
 *   HH_UI off (implied by -DHH_RT64=OFF) -- no UI at all: the game starts
 *       immediately against whichever ROM was validated, exactly as this port
 *       did before the launcher existed. That is the bisect build, and it is the
 *       reason the two paths are kept apart rather than merged; see
 *       null_render_context.cpp.
 *
 * Everything this port measures itself with is common to both: the crash
 * handler, the HH_ switches, the RSP tracing, the audio queue accounting.
 */

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <string>
#include <typeinfo>
#include <vector>

// Android defines __linux__ too, and all of these exist in Bionic EXCEPT
// <execinfo.h>: there is no backtrace()/backtrace_symbols_fd() outside glibc.
// That single absence is why the crash handler below is desktop-only and Android
// gets its own classifier in android_diag.cpp.
#if defined(__linux__)
#   include <dirent.h>
#   if !defined(__ANDROID__)
#       include <execinfo.h>
#   endif
#   include <fcntl.h>
#   include <malloc.h>
#   include <sys/syscall.h>
#   include <unistd.h>
#endif

#if defined(__ANDROID__)
#   include <android/log.h>
#   include "hh_support.h"
#endif

#include <thread>

// SDL's headers sit at <SDL2/...> on Linux and at the top level in the prebuilt
// Windows development package, which is why the UI files carry this same split.
// Android is the Windows case rather than the Linux one despite being Linux:
// there is no system SDL2 to install headers under SDL2/, so lib/rt64 fetches
// the SDL sources and the include root is that tree's own include/ directory.
#if defined(_WIN32)
#include <SDL.h>
// For the HWND behind the SDL window -- see to_window_handle.
#include <SDL_syswm.h>
#elif defined(__ANDROID__)
#include <SDL.h>
#else
#include <SDL2/SDL.h>
#endif

#include "librecomp/game.hpp"
#include "librecomp/overlays.hpp"
#include "librecomp/rsp.hpp"
#include "ultramodern/ultramodern.hpp"
#include "ultramodern/error_handling.hpp"
#include "ultramodern/events.hpp"
#include "ultramodern/input.hpp"
#include "ultramodern/renderer_context.hpp"
#include "ultramodern/threads.hpp"

#include "hh_game.h"
#include "ovl_patches.hpp"
#include "rsp.h"

#if defined(HH_UI)
#   include "recomp_ui.h"
#   include "recomp_input.h"
#   include "hh_config.h"
#   include "hh_sound.h"
#   include "hh_support.h"
#endif

// Not under HH_UI: the `.manual.bak` rollback point is maintained whether or not
// the settings menu exists to switch autosave on.
#include "hh_save_rollback.h"

// The window icon needs both: `get_asset_path` is in support.cpp, which the build
// compiles only with the UI, and the PNG decoder is rt64's vendored stb (declared
// here, defined in rt64_texture_cache.cpp). A headless -DHH_UI=OFF measurement
// build has neither and does not want a window icon anyway.
#if defined(HH_UI) && defined(HH_RT64)
#   define HH_WINDOW_ICON 1
#   include <fstream>
#   include "stb/stb_image.h"
#endif

namespace hybridheaven {
    namespace renderer {
        std::unique_ptr<ultramodern::renderer::RendererContext> create_render_context(
            uint8_t* rdram, ultramodern::renderer::WindowHandle window_handle, bool developer_mode);

        // Declared here rather than by including hh_render.h, which drags in the
        // rt64 headers -- this file is compiled for the -DHH_RT64=OFF build too.
        // Both render contexts define it; see the comment on the declaration in
        // include/hh_render.h.
        extern std::atomic<uint64_t> gfx_tasks_submitted;
    }
}

extern "C" void recomp_entrypoint(uint8_t* rdram, recomp_context* ctx);

// Generated into RecompiledFuncs/lookup.cpp as `(gpr)(int32_t)0x80000400u`.
// Use it rather than writing the address out: `gpr` is 64-bit and every MEM_*
// macro subtracts 0xFFFFFFFF80000000, so the value has to be the SIGN-EXTENDED
// 0xFFFFFFFF80000400. A plain 0x80000400 lands ~4GB into rdram and faults on
// the very first DMA in recomp::init.
gpr get_entrypoint_address();

// ---------------------------------------------------------------------------
// Crash reporting
//
// A recompiled game that faults gives you a bare SIGSEGV and nothing else, and
// this project has no debugger available. Printing a backtrace from the signal
// handler is what turns that into a starting point. Build with `-g -rdynamic`
// for names; run the addresses through addr2line for lines.
//
// Only async-signal-safe calls are used here: backtrace_symbols_fd writes
// directly to the fd rather than allocating, unlike backtrace_symbols.
// ---------------------------------------------------------------------------

// The rdram base, recorded on the way past. The fault address a SIGSEGV carries
// is a host pointer, and it only becomes a clue once it is turned back into the
// guest address the recompiled code was reaching for -- the difference between
// "a bad pointer somewhere" and "0x3B7A8 past where that overlay actually is".
static std::atomic<uint8_t*> rdram_base{nullptr};

// Read a guest word for diagnostics, from any thread. 32-bit words are stored
// host-order in rdram (see the other direct reads below), so this is a plain
// load. Returns 0 before the game is mapped.
namespace hybridheaven {
uint32_t debug_read_guest_u32(uint32_t vaddr) {
    const uint8_t* base = rdram_base.load(std::memory_order_relaxed);
    if (base == nullptr || vaddr < 0x80000000u) {
        return 0;
    }
    return *(const uint32_t*)(base + (vaddr - 0x80000000u));
}

// HH_FRAME_GOVERNOR_OFF=1 -- experiment switch for the periodic double-frame.
//
// The game's main loop paces itself two ways at once: it consumes retrace
// messages from its 64-deep client queue AND busy-waits on osGetTime until
// `frame-step` (D_8017AA90, normally 2) frames have elapsed since the frame
// started (func_80001454_2054, spin at .L80001A88_2688). On hardware COUNT and
// VI come off the same oscillator, so the two paces agree. In the port both are
// wall-clock but the governor adds its per-frame overhead ON TOP of the
// 33.33 ms wait, so the loop free-runs slightly slower than the 60 Hz retrace
// stream and beats against it: every ~44 frames the drift crosses a frame
// boundary and the loop eats a 67 ms double-frame, then resyncs -- measured
// metronomic at 1.47-1.50 s, 1.3M osGetTime calls inside each spike, with the
// main thread never reaching a scheduling point for the duration (which is
// also what starves audio-event delivery).
//
// Setting frame-step to 0 makes the governor skip the spin outright (`blez`
// straight past the wait loop), so the loop paces off the retrace queue alone
// -- the pacing hardware effectively provided. The poke only rewrites the value
// 2 (the steady-state), so any special value the game sets for other states is
// preserved. Poked once per gfx task from the gfx thread; a torn read is
// impossible for a u32 flag and the worst case of losing a race is one paced
// frame. Off by default; this is the A/B for the frametime spike.
static bool frame_governor_off() {
    static const bool on = getenv("HH_FRAME_GOVERNOR_OFF") != nullptr;
    return on;
}

// HH_FRAME_GOVERNOR_US=<microseconds> -- the candidate FIX, as opposed to the
// diagnostic above. MEASURED: with the governor off entirely the game submits
// a frame every retrace (60 fps, p50 16.7 ms) -- the spin was the only thing
// holding it to 30 -- so removing it outright almost certainly also doubles
// game speed, which is why it stays a diagnostic.
//
// This override instead RETUNES the governor: it rewrites the frame-period
// double the spin divides by (D_8004B908, 16666.666 us in the ROM) to a value
// slightly SHORT of the VI period. The spin then always finishes a little
// before the next retrace pair has arrived, the client queue drains, and the
// main thread blocks on osRecvMesg once per frame -- so the RETRACE STREAM
// paces the loop at exactly 30 fps and the queue's blocking recv absorbs the
// difference. The beat (governor period 33.33 ms + loop overhead against the
// port's exact-60 Hz stream) disappears, and with it the 67 ms double-frame
// every 1.47 s. The game still runs 30 fps; the governor still bounds the
// frame time from below; only the pacer changes from the drifting clock spin
// to the message queue -- which is the pacer the game's own queue-full
// tolerance (it survives 64-deep backlogs and drops) shows it can live with.
//
// 16000 (4% short) is the suggested value: well past the measured ~370 us of
// loop overhead, well short of anything that could matter to game logic, since
// the loop still cannot run faster than the retraces arrive.
//
// D_8004B900 -- the neighbouring constant the MUSIC tempo path divides by --
// is deliberately not touched.
//
// A double in rdram is two host-order 32-bit words, high word first (matching
// the direct u32 reads elsewhere in this file).
void apply_frame_governor_override() {
    uint8_t* base = rdram_base.load(std::memory_order_relaxed);
    if (base == nullptr) {
        return;
    }

    if (frame_governor_off()) {
        uint32_t* step = (uint32_t*)(base + (0x8017AA90u - 0x80000000u));
        if (*step == 2) {
            *step = 0;
        }
        return;
    }

    static const double period_us = [] {
        const char* env = getenv("HH_FRAME_GOVERNOR_US");
        return env != nullptr ? atof(env) : 0.0;
    }();
    if (period_us <= 0.0) {
        return;
    }

    uint32_t* hi = (uint32_t*)(base + (0x8004B908u - 0x80000000u));
    uint32_t* lo = (uint32_t*)(base + (0x8004B90Cu - 0x80000000u));
    uint64_t bits;
    memcpy(&bits, &period_us, sizeof(bits));
    const uint32_t want_hi = (uint32_t)(bits >> 32);
    const uint32_t want_lo = (uint32_t)bits;
    if (*hi != want_hi || *lo != want_lo) {
        *hi = want_hi;
        *lo = want_lo;
    }
}
}

// Desktop Linux only, NOT Android: this handler's whole output is
// backtrace_symbols_fd(), which Bionic does not have. Android installs the
// classifier in android_diag.cpp instead -- same signals, but it reports the
// fault address against the rdram base and writes to logcat, since a device has
// no stderr anyone reads.
#if defined(__linux__) && !defined(__ANDROID__)
// Writes `digits` hex digits of `value` and returns one past the last.
static char* write_hex(char* out, uint64_t value, int digits) {
    for (int i = digits - 1; i >= 0; i--) {
        *out++ = "0123456789ABCDEF"[(value >> (i * 4)) & 0xF];
    }
    return out;
}

static void crash_handler(int sig, siginfo_t* info, void* context) {
    (void)context;

    // Formatted by hand rather than with fprintf, which is not async-signal-safe.
    // Knowing WHICH signal matters more than it looks: a SIGSEGV and a SIGFPE in
    // recompiled code look identical in a backtrace but mean completely
    // different things (a bad address versus a division the game never expected
    // to trap).
    char header[] = "\n*** caught signal NN at 0xXXXXXXXXXXXXXXXX, backtrace follows ***\n";
    char* slot = header + sizeof("\n*** caught signal ") - 1;
    slot[0] = char('0' + (sig / 10) % 10);
    slot[1] = char('0' + sig % 10);
    write_hex(slot + sizeof(" at 0x") - 1 + 2, (uint64_t)info->si_addr, 16);
    ssize_t ignored = write(STDERR_FILENO, header, sizeof(header) - 1);

    // The same address as the game would name it. rdram is one flat allocation
    // and every MEM_* access is `rdram + (vaddr - 0xFFFFFFFF80000000)`, so the
    // offset from the base inverts straight back to a KSEG0 address. Anything
    // outside the 512 MB window is not a guest access at all, and saying so is
    // just as useful -- it means the fault is in the host layer.
    // si_code separates "nothing is mapped there" (SEGV_MAPERR, 1) from "mapped
    // but not with those permissions" (SEGV_ACCERR, 2). rdram is one 512 MB
    // read-write mapping, so a MAPERR inside it would mean the base is wrong
    // rather than the access.
    char detail[] = "    si_code N, rdram base 0xXXXXXXXXXXXXXXXX\n";
    detail[sizeof("    si_code ") - 1] = char('0' + (info->si_code & 0xF));
    write_hex(detail + sizeof("    si_code N, rdram base 0x") - 1, (uint64_t)rdram_base.load(), 16);
    ignored = write(STDERR_FILENO, detail, sizeof(detail) - 1);

    // The address as the game would name it. Every MEM_* access is
    // `rdram + (vaddr - 0xFFFFFFFF80000000)`, so adding 0x80000000 back to the
    // offset from the base inverts it -- in 32 bits, deliberately. A guest
    // pointer that is small or garbage rather than KSEG0 wraps, and the wrapped
    // value is the useful one: an offset of 0x80000111 means the game
    // dereferenced 0x111, i.e. a null pointer plus a struct offset. Only an
    // offset inside the 512 MB window is a real access; the rest is printed
    // anyway, marked, because that is exactly when it is worth seeing.
    uint8_t* base = rdram_base.load();
    if (base != nullptr) {
        uint64_t offset = (uint64_t)((uint8_t*)info->si_addr - base);
        char guest[] = "    guest address 0xXXXXXXXX (rdram offset 0xXXXXXXXXXXXXXXXX)\n";
        write_hex(guest + sizeof("    guest address 0x") - 1, 0x80000000ull + offset, 8);
        write_hex(guest + sizeof("    guest address 0xXXXXXXXX (rdram offset 0x") - 1, offset, 16);
        ignored = write(STDERR_FILENO, guest, sizeof(guest) - 1);

        if (offset >= 0x20000000ull) {
            char bad[] = "    ^ outside rdram: not an address the game could have meant\n";
            ignored = write(STDERR_FILENO, bad, sizeof(bad) - 1);
        }
    }
    (void)ignored;

    void* frames[64];
    int count = backtrace(frames, 64);
    backtrace_symbols_fd(frames, count, STDERR_FILENO);

    // HH_DUMP_MAPS=1 adds the process's memory map, which is what settles
    // whether a fault address is inside rdram at all -- the arithmetic above
    // assumes a base recorded much earlier, and a mismatch there would make
    // every "guest address" it prints a fiction. open/read/write are all
    // async-signal-safe; the C library's file streams are not.
    if (getenv("HH_DUMP_MAPS") != nullptr) {
        int maps = open("/proc/self/maps", O_RDONLY);
        if (maps >= 0) {
            char buffer[4096];
            ssize_t got;
            while ((got = read(maps, buffer, sizeof(buffer))) > 0) {
                ignored = write(STDERR_FILENO, buffer, got);
            }
            close(maps);
        }
    }

    signal(sig, SIG_DFL);
    raise(sig);
}

static void install_crash_handler() {
    struct sigaction action{};
    action.sa_sigaction = crash_handler;
    action.sa_flags = SA_SIGINFO;
    sigemptyset(&action.sa_mask);

    sigaction(SIGSEGV, &action, nullptr);
    sigaction(SIGBUS, &action, nullptr);
    sigaction(SIGILL, &action, nullptr);
    sigaction(SIGFPE, &action, nullptr);
    sigaction(SIGABRT, &action, nullptr);
}
#elif defined(_WIN32)
// Windows delivers none of the signals the Linux handler is built around, and
// the failure this was written for prints *nothing*: an uncaught C++ exception
// and a failed assertion both end the process through `__fastfail`, so the exit
// code -- 0xC0000409, STATUS_STACK_BUFFER_OVERRUN, which here means "the CRT
// gave up" rather than anything about a stack buffer -- is the whole report.
// The Windows build died exactly that way with five lines of output and no
// indication of which of them was last.
//
// Three hooks, because there are three separate ways out and no one hook sees
// the others:
//
//   - `std::set_terminate` runs while the exception is still current, so it is
//     the only place the *type* and `what()` of an uncaught C++ exception can
//     be recovered. After it returns the CRT calls abort() and the type is gone;
//   - `SIGABRT` covers abort() reached any other way -- a failed assert, a
//     `std::terminate` from a noexcept violation. The UCRT raises SIGABRT before
//     it fastfails *only* if a handler is installed, which is why this is worth
//     installing even though nothing here raises it;
//   - `SetUnhandledExceptionFilter` covers a real hardware fault (access
//     violation, illegal instruction) that no `__except` caught.
//
// Addresses are printed twice: absolute, and as an RVA from the module base,
// because the RVA is what symbolizes offline against the PDB and the absolute
// one is meaningless after ASLR:
//
//   llvm-symbolizer --obj=build-win/HybridHeavenRecompiled.exe 0x<rva>
static void write_backtrace_windows(const char* what, uint64_t code, const void* address) {
    HMODULE module = GetModuleHandleW(nullptr);
    const uint64_t base = (uint64_t)module;

    fprintf(stderr, "\n*** %s: code 0x%08llX at 0x%016llX ***\n",
            what, (unsigned long long)code, (unsigned long long)(uintptr_t)address);
    fprintf(stderr, "    module base 0x%016llX, rdram base 0x%016llX\n",
            (unsigned long long)base, (unsigned long long)(uintptr_t)rdram_base.load());

    // The guest address, on the same reasoning as the Linux handler: rdram is one
    // flat allocation, so an offset from its base inverts back to the KSEG0
    // address the recompiled code was reaching for. Only meaningful for a fault
    // that carries an address, which is why it is skipped when there is none.
    uint8_t* rdram = rdram_base.load();
    if (rdram != nullptr && address != nullptr) {
        uint64_t offset = (uint64_t)((const uint8_t*)address - rdram);
        fprintf(stderr, "    guest address 0x%08llX (rdram offset 0x%016llX)%s\n",
                (unsigned long long)(uint32_t)(0x80000000ull + offset),
                (unsigned long long)offset,
                offset >= 0x20000000ull ? "  <- outside rdram, so a host address" : "");
    }

    void* frames[62];
    USHORT count = CaptureStackBackTrace(0, 62, frames, nullptr);
    for (USHORT i = 0; i < count; i++) {
        const uint64_t address_i = (uint64_t)(uintptr_t)frames[i];
        // Frames from other modules (the Vulkan driver, SDL2.dll) have no
        // meaningful RVA against this PDB, and printing one would invite
        // symbolizing it against the wrong module.
        if (address_i >= base) {
            fprintf(stderr, "    #%02u 0x%016llX  rva 0x%llX\n",
                    unsigned(i), (unsigned long long)address_i,
                    (unsigned long long)(address_i - base));
        }
        else {
            fprintf(stderr, "    #%02u 0x%016llX  (below module base)\n",
                    unsigned(i), (unsigned long long)address_i);
        }
    }
    fflush(stderr);
}

static void windows_terminate_handler() {
    // Rethrowing is the documented way to get at the exception that is killing
    // the process: `std::current_exception()` is still set inside a terminate
    // handler, and there is no other route to its type.
    const char* detail = "std::terminate with no active exception";
    std::string owned;
    if (std::exception_ptr active = std::current_exception()) {
        try {
            std::rethrow_exception(active);
        }
        catch (const std::exception& e) {
            owned = std::string(typeid(e).name()) + ": " + e.what();
            detail = owned.c_str();
        }
        catch (...) {
            detail = "uncaught exception, not derived from std::exception";
        }
    }

    write_backtrace_windows(detail, 0, nullptr);

    // Not abort(): that re-enters terminate on some paths and replaces this
    // report with the very fastfail it exists to explain.
    _Exit(3);
}

static void windows_abort_handler(int sig) {
    (void)sig;
    write_backtrace_windows("SIGABRT", 0, nullptr);
    _Exit(3);
}

static LONG WINAPI windows_exception_filter(EXCEPTION_POINTERS* info) {
    write_backtrace_windows("unhandled exception",
                            info->ExceptionRecord->ExceptionCode,
                            info->ExceptionRecord->ExceptionAddress);
    return EXCEPTION_EXECUTE_HANDLER;
}

static void install_crash_handler() {
    std::set_terminate(windows_terminate_handler);
    signal(SIGABRT, windows_abort_handler);
    SetUnhandledExceptionFilter(windows_exception_filter);
}
#elif defined(__ANDROID__)
// android_diag.cpp owns the signal handlers here; see hh_support.h.
static void install_crash_handler() {
    hybridheaven::diag::install_crash_handler();
}
#else
static void install_crash_handler() {}
#endif

// The renderer factory is the first callback the port gets that is handed the
// rdram base, so it is where the crash handler's copy comes from. It runs long
// before the game is doing anything a fault could come out of.
static std::unique_ptr<ultramodern::renderer::RendererContext> create_render_context_recording_rdram(
        uint8_t* rdram, ultramodern::renderer::WindowHandle window_handle, bool developer_mode) {
    rdram_base.store(rdram);
    return hybridheaven::renderer::create_render_context(rdram, window_handle, developer_mode);
}

// ---------------------------------------------------------------------------
// The game
//
// `rom_hash` is checked against the file the user supplies. With
// `decompression_routine` set, librecomp validates that file and THEN expands it
// in memory, so the two roles come apart: the file a player provides and keeps on
// disk is their own RETAIL cart dump (sha1 16dbc21620b52deab5c5abf8a309ac60adfbee85),
// while what every ROM read sees at runtime is the decompressed image the
// recompiler was built from. The archive offsets baked into the recompiled
// section table are decompressed-ROM offsets, and so are the entries in the
// game's own Nisitenma-Ichigo table -- that has not changed, it is just produced
// on the way in now instead of being demanded up front.
//
// This is how Goemon64Recomp does it, and it is what makes "pick your own ROM"
// possible: a phone cannot run the decomp's `make setup`, so requiring the
// pre-decompressed file would have made the Android launcher sideload-only.
// src/game/rom_decompression.cpp is the routine; its output is verified
// byte-identical to `make setup`'s by tools/verify_rom_decompression.cpp.
// ---------------------------------------------------------------------------

// The retail Hybrid Heaven (USA) cart dump, 16 MiB. NOT the decompressed image
// (which is 0x42DF81DF02A923C9) -- see above for why those swapped roles.
constexpr uint64_t hybridheaven_rom_hash = 0x0F6A72F2C36A216DULL;

std::vector<recomp::GameEntry> supported_games = {
    {
        .rom_hash = hybridheaven_rom_hash,
        .internal_name = "HYBRIDHEAVEN",
        .game_id = u8"hybridheaven.us",
        .mod_game_id = "hybridheaven",
        .save_type = recomp::SaveType::AllowAll,
        .is_enabled = true,
        .decompression_routine = hybridheaven::decompress_hh,
        .has_compressed_code = true,
        .entrypoint_address = get_entrypoint_address(),
        .entrypoint = recomp_entrypoint,
    }
};

// ---------------------------------------------------------------------------
// Callbacks
// ---------------------------------------------------------------------------

// The audio microcode. Task type 2 is M_AUDTASK; see lib/hybridheaven/PLAN.md
// Phase B for how aspMain was identified and why it is the only one that needs
// recompiling (RT64 implements F3DEX itself, and with the null renderer there is
// no graphics task to serve at all).
// The task currently being run, captured so the wrapper below can describe the
// one that failed. `run_task` hands the OSTask to this callback and to nothing
// else, and by the time librecomp prints "exited unexpectedly" the microcode's
// register state is gone.
//
// Reading the fields directly is correct: rdram stores each 32-bit word in host
// order (MEM_W has no address xor -- only the sub-word accessors do), so a
// pointer into rdram can be used as an ordinary struct pointer.
static const OSTask* current_rsp_task = nullptr;

// aspMain with a post-mortem.
//
// The audio microcode dispatches each command with `lh` from a jump table
// indexed by the command word's opcode byte, so an `UnhandledJumpTarget` exit
// means the command list held something that is not a command -- the target
// printed is whatever half-word sat past the end of the table. Printing the list
// is what separates "the pointer is wrong" from "the list went bad partway
// through", which point at completely different bugs.
// Audio-task accounting, for the HH_TRACE_AUDIO report.
//
// `produced frames/s` alone cannot separate "the game asked for small buffers"
// from "the game was not asked to run" -- both read as underproduction. Counting
// the tasks splits them, and the mean size (produced / tasks) is the
// controller's OUTPUT, which is the other half of a loop whose input is
// get_frames_remaining().
//
// `over_992` is the one that matters. The game computes `736 - reported + 0x100`
// and clamps it up to 720 with an UNSIGNED compare (sltu, main_20420.s). Once
// reported exceeds 992 the subtraction goes negative, the s16 it is stored in
// reads as a huge unsigned number, and the clamp does not fire -- so a value
// past 992 does not produce the minimum, it produces a NEGATIVE length. Any
// nonzero count here means the port is handing the game arithmetic it cannot do.
static bool trace_audio_enabled() {
    static const bool on = getenv("HH_TRACE_AUDIO") != nullptr;
    return on;
}

// Ground truth for the OUTPUT of the game's rate controller.
//
// The previous accounting inferred what the game did from what it was told, and
// got it wrong (see get_frames_remaining). This measures the game's own answer
// instead: `queue_samples` receives the buffer the game actually synthesised, so
// `sample_count / 2` IS the size func_8001FD14_20914 computed. No model, no
// assumed constants, nothing sampled at second hand.
//
// The buckets are the regimes of the game's own arithmetic (§3 of
// docs/frametime-investigation.md), so a run says directly which one it lives in:
//
//   at_floor   size == floor (720)          -- controller railed at minimum
//   in_band    floor < size < ceiling       -- controller actually regulating
//   at_ceiling size >= ceiling (992)        -- railed at maximum
//
// And the one that cannot be seen by looking at buffers at all: a size <= 0 makes
// func_8002C4D0_2D0D0 skip its synthesis loop (`blez $s3`, 2D1C8) and produce
// NOTHING, so there is no buffer and no queue_samples call. It shows up only as
// audio tasks that produced no buffer -- `silent_tasks` below, which is why the
// task counter and the buffer counter are kept separately and differenced.
static std::atomic<uint64_t> audio_buffers_window{0};
static std::atomic<uint64_t> audio_size_at_floor{0};
static std::atomic<uint64_t> audio_size_in_band{0};
static std::atomic<uint64_t> audio_size_at_ceiling{0};
static std::atomic<uint32_t> audio_size_min{UINT32_MAX};
static std::atomic<uint32_t> audio_size_max{0};

// The last value get_frames_remaining handed back, so a buffer can be paired
// with the report that produced it.
static std::atomic<uint32_t> audio_last_reported{0};

static void note_audio_size(size_t frames) {
    if (!trace_audio_enabled()) {
        return;
    }
    audio_buffers_window.fetch_add(1, std::memory_order_relaxed);

    // Pair the report with the size it produced, against the game's arithmetic
    // modelled from main_20420.s.
    //
    // READ THE AGGREGATE, NOT THE PAIRS. The pairing lags by about one task:
    // func_8001FD14_20914 queues the PREVIOUS buffer (osAiSetNextBuffer) before
    // it reads osAiGetLength and sizes the next one, so the buffer seen here was
    // sized from an earlier read than `audio_last_reported` holds. Individual
    // MISMATCH lines are therefore expected and prove nothing on their own --
    // pairing across a lag is the same class of error that invalidated the first
    // over-992 count, and it is written down here so it is not made a third time.
    //
    // What IS robust is the aggregate, because the drain phase holds the report
    // above 992 for many consecutive tasks, so the lagged read was above 992 too:
    // over 209 gameplay windows, 10708 reports above 992 and the size NEVER left
    // [720, 992] -- not once below the floor, not once above the ceiling.
    {
        const uint32_t reported = audio_last_reported.load(std::memory_order_relaxed);
        const uint8_t* base = rdram_base.load();
        uint32_t target = 736, floor_v = 720;
        if (base != nullptr) {
            target = *(const uint32_t*)(base + (0x80096340u - 0x80000000u));
            floor_v = *(const uint32_t*)(base + (0x8009633Cu - 0x80000000u));
        }
        const int32_t t9 = (int32_t)target - (int32_t)reported + 0x100;
        const uint16_t stored = (uint16_t)(t9 & 0x0000FFF0);
        const int32_t a3 = (int16_t)stored;                  // lh sign-extends
        const uint32_t predicted = ((uint32_t)a3 < floor_v)  // sltu
                                       ? floor_v
                                       : (uint32_t)a3;
        if (predicted != (uint32_t)frames) {
            static uint32_t mismatches = 0;
            if ((++mismatches & (mismatches - 1)) == 0) {
                fprintf(stderr,
                        "[audio] MISMATCH #%u: last report %u -> predicted %d, "
                        "game produced %zu (target %u floor %u)\n",
                        mismatches, reported, (int32_t)predicted, frames, target, floor_v);
            }
        }
    }

    // Read the game's OWN target and floor out of .main_bss rather than assuming
    // 736/720/992: they are written at runtime by func_8001F8A0_204A0 from
    // whatever osAiSetFrequency returned, and the two builds here opened at
    // different rates (44095 and 48000).
    const uint8_t* base = rdram_base.load();
    uint32_t floor_v = 720, ceiling_v = 992;
    if (base != nullptr) {
        floor_v = *(const uint32_t*)(base + (0x8009633Cu - 0x80000000u));
        ceiling_v = *(const uint32_t*)(base + (0x80096340u - 0x80000000u)) + 0x100;
    }

    const uint32_t f = static_cast<uint32_t>(frames);
    if (f <= floor_v) {
        audio_size_at_floor.fetch_add(1, std::memory_order_relaxed);
    } else if (f >= ceiling_v) {
        audio_size_at_ceiling.fetch_add(1, std::memory_order_relaxed);
    } else {
        audio_size_in_band.fetch_add(1, std::memory_order_relaxed);
    }

    uint32_t prev = audio_size_min.load(std::memory_order_relaxed);
    while (f < prev && !audio_size_min.compare_exchange_weak(prev, f, std::memory_order_relaxed)) {
    }
    prev = audio_size_max.load(std::memory_order_relaxed);
    while (f > prev && !audio_size_max.compare_exchange_weak(prev, f, std::memory_order_relaxed)) {
    }
}

static std::atomic<uint64_t> audio_tasks_window{0};
static std::atomic<uint64_t> audio_reported_sum{0};
static std::atomic<uint32_t> audio_reported_max{0};
static std::atomic<uint32_t> audio_reported_over_992{0};
// How many times osAiGetLength is READ per audio task. If the game read it once
// per task, this would track the task count. It does not, and the difference is
// the point: a game that polls the length to decide WHETHER to build another
// buffer is controlling its task RATE with it, which is the variable that
// actually moves here (29.8/s vs 90-120/s) -- and those polls are compares, not
// the subtraction that sizes the buffer, so a value above 992 in a poll is not
// the arithmetic failure a value above 992 in the sizing read would be.
static std::atomic<uint64_t> audio_reported_reads{0};

// Wall time inside aspMain, in microseconds, since the last report.
//
// This is the measurement that separates the two explanations for an audio task
// that only runs ~30 times a second when the VI thread offers it 60 chances:
// either each task takes longer than a VI period (16.7 ms) and the game's own
// message queue is full when the next AI interrupt arrives -- in which case
// ultramodern DROPS it, since the AI enqueue passes requeue_if_blocked=false
// (ultramodern/src/events.cpp:332) -- or the task is quick and the missing
// invocations are being lost somewhere that is not compute.
static std::atomic<uint64_t> audio_task_us{0};

static RspExitReason aspMain_reporting(uint8_t* rdram, uint32_t ucode_addr) {
    const auto asp_t0 = std::chrono::steady_clock::now();
    RspExitReason reason = aspMain(rdram, ucode_addr);
    audio_task_us.fetch_add(
        (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - asp_t0).count(),
        std::memory_order_relaxed);

    if (reason != RspExitReason::Broke && current_rsp_task != nullptr) {
        const OSTask* task = current_rsp_task;
        uint32_t data_ptr = (uint32_t)task->t.data_ptr;
        uint32_t data_size = (uint32_t)task->t.data_size;

        fprintf(stderr, "[rsp] audio task failed: ucode=%08X ucode_data=%08X "
                        "data_ptr=%08X data_size=%08X flags=%08X\n",
                (uint32_t)task->t.ucode, (uint32_t)task->t.ucode_data,
                data_ptr, data_size, (uint32_t)task->t.flags);

        // Command words are 8 bytes: opcode in the top byte of the first word.
        // A valid list is a run of small opcodes; garbage is obvious by eye.
        // The bound is rdram's size, not a guess at a plausible list length --
        // an implausible `data_size` is itself the thing worth seeing.
        if (data_ptr >= 0x80000000u && data_size != 0 && data_size < 0x00800000u) {
            const uint32_t* words = (const uint32_t*)(rdram + (data_ptr - 0x80000000u));
            uint32_t count = data_size / sizeof(uint32_t);
            uint32_t first_bad = UINT32_MAX;

            // aspMain's jump table at DMEM 0x10 has entries for opcodes 0x00-0x0F
            // and unrelated data past that, so anything above 0x0F is not a
            // command -- it indexes past the table and jumps to a bit mask.
            for (uint32_t i = 0; i < count; i += 2) {
                if ((words[i] >> 24) > 0x0F) {
                    first_bad = i;
                    break;
                }
            }

            if (first_bad == UINT32_MAX) {
                fprintf(stderr, "[rsp] every opcode in the list is valid (%u commands)\n",
                        count / 2);
            }
            else {
                fprintf(stderr, "[rsp] first invalid opcode 0x%02X at command %u of %u"
                                " (byte offset 0x%X)\n",
                        words[first_bad] >> 24, first_bad / 2, count / 2, first_bad * 4);
            }

            uint32_t from = (first_bad == UINT32_MAX || first_bad < 8) ? 0 : first_bad - 8;
            for (uint32_t i = from; i < count && i < from + 24; i += 2) {
                fprintf(stderr, "[rsp]   %08X: %08X %08X%s\n",
                        data_ptr + i * 4, words[i], words[i + 1],
                        i == first_bad ? "   <-- here" : "");
            }
        }
    }

    return reason;
}

static size_t get_frames_remaining();

// HH_TRACE_RSP=1 logs every audio task. What makes this worth having is the
// comparison: a `data_size` only looks wrong once a healthy one is on the screen
// above it.
static bool trace_rsp() {
    static const bool enabled = getenv("HH_TRACE_RSP") != nullptr;
    return enabled;
}

// Defined in src/game/recomp_api.cpp -- counts loader-hook invocations.
uint32_t hh_overlay_load_count();

// Did the patches actually take effect?
//
// Linking the patch blob is not the same as overriding with it, and the
// difference has bitten this port once already: a build where RECOMP_FUNC was
// not weak discarded every patched function, ran fine, and never loaded an
// overlay through the hook. Nothing reported it.
//
// The game cannot reach its first frames without loading overlays, so by the
// time a comfortable number of RSP tasks have gone by, a zero count means the
// hook is not installed. Checked from here because it is the one path that is
// certain to run and certain to run late enough.
static void check_patches_took_effect() {
    static uint32_t tasks_seen = 0;
    static bool checked = false;

    if (checked) {
        return;
    }
    // ~1 second of frames. Far past the boot loads, far short of anything a
    // slow host would trip.
    if (++tasks_seen < 60) {
        return;
    }
    checked = true;

    if (hh_overlay_load_count() == 0) {
        fprintf(stderr,
                "[patches] ERROR: %u RSP tasks in and the overlay loader hook has "
                "never run.\n"
                "[patches] The patches are not overriding the base recompilation. "
                "Check that tools/n64recomp-gcc-weak-recomp-func.patch is applied "
                "to lib/N64ModernRuntime/N64Recomp.\n",
                tasks_seen);
    }
}

static RspUcodeFunc* get_rsp_microcode(const OSTask* task) {
    current_rsp_task = task;
    check_patches_took_effect();

    if (trace_rsp()) {
        // frames_remaining is what osAiGetLength reports back to the game, and
        // the game sizes each audio frame from it: `target - queued`. If it ever
        // grows without bound the subtraction stops meaning anything.
        // The game sizes each audio frame as `D_80096340 - osAiGetLength()/4`,
        // clamped up (never down) against D_8009633C, in
        // func_8001FD14_20914. Both live in .main_bss, so they can be read
        // straight out of rdram -- 32-bit words are stored in host order.
        const uint32_t* target = (const uint32_t*)(rdram_base.load() + (0x80096340u - 0x80000000u));
        const uint32_t* floor_ = (const uint32_t*)(rdram_base.load() + (0x8009633Cu - 0x80000000u));
        fprintf(stderr, "[rsp] task type=%u data_ptr=%08X data_size=%08X frames_remaining=%zu"
                        " target=%u floor=%u\n",
                (unsigned)task->t.type, (uint32_t)task->t.data_ptr,
                (uint32_t)task->t.data_size, get_frames_remaining(), *target, *floor_);
    }

    switch (task->t.type) {
        case M_AUDTASK:
            // Count the task and nothing else. The `reported` accounting used to
            // live here and it was WRONG, in a way that produced a real false
            // finding -- see get_frames_remaining. This point is RSP dispatch,
            // which happens after the game has already read osAiGetLength, sized
            // its buffer and synthesised; the queue has grown by the synthesised
            // audio in between, so the value read here is not the value the game
            // acted on. It also cost an unconditional SDL_GetQueuedAudioSize on
            // the audio path in every build, traced or not.
            audio_tasks_window.fetch_add(1, std::memory_order_relaxed);
            return aspMain_reporting;
        default:
            fprintf(stderr, "Unhandled RSP task type: %u\n", task->t.type);
            return nullptr;
    }
}

// Audio. SDL is opened lazily at the first frequency the game asks for, since
// ultramodern calls set_frequency before any samples are queued.
static SDL_AudioDeviceID audio_device = 0;
static uint32_t audio_frequency = 0;

// ---------------------------------------------------------------------------
// Which audio path: the upstream N64Recomp one, or this port's original.
//
// HH_AUDIO_LEGACY=1 restores everything this port's audio figures were measured
// against -- device reopened at the game's own rate, the true backlog reported,
// whole chunks refused on overflow. Keep it: every number in PLAN.md and the
// README came off that path, and without a switch the four changes below would
// only ever be creditable to each other.
//
// The default is now the path Zelda64Recomp, Goemon64Recomp and Quest64-Recomp
// all share, essentially verbatim -- the same file, the same comments, differing
// only in a volume namespace. That is three shipped ports against this port's
// one bespoke implementation, and measurement says the bespoke one is broken in
// a specific way: it pins the game's own audio rate control at its floor. The
// four differences, all of which matter and all of which are here:
//
//   1. ONE DEVICE, FIXED RATE. Upstream opens 48 kHz once and resamples the
//      game's rate into it with SDL_ConvertAudio. This port reopened SDL at
//      whatever the game asked for -- 44,095 Hz, which no device runs natively,
//      so the OS resamples anyway and a mid-run reopen is a glitch on its own.
//   2. A 256-FRAME DEVICE BUFFER, not 1024. The game regulates its backlog to
//      about 257 frames. With a 1024-frame buffer its entire control range is
//      narrower than one buffer, so the loop cannot settle at its own setpoint --
//      an actuator coarser than the thing it regulates.
//   3. REPORT ONE VI LESS than is really queued. Upstream's comment says exactly
//      why: it "prevents audio popping on games that use the buffered audio byte
//      count to determine how many samples to generate". Hybrid Heaven is such a
//      game -- `992 - remaining`, floored at 720 -- and measured across 958 tasks
//      it generated 709-722 EVERY time, i.e. the floor, always. Production
//      reduced to `720 x tasks-per-second` with no feedback at all.
//   4. DECIMATE ON OVERFLOW, don't refuse chunks. Above 100 ms queued, keep one
//      sample in 2^n. Shedding a whole 720-frame chunk is a 16 ms hole; thinning
//      is a pitch artefact nobody can hear. Over-production is only safe if
//      there is a graceful way to shed it, and that is what makes (3) affordable.
// ---------------------------------------------------------------------------
static bool audio_legacy() {
    static const bool legacy = getenv("HH_AUDIO_LEGACY") != nullptr;
    return legacy;
}

// The rate the game asks for, and the rate the device actually runs at. Equal on
// the legacy path; independent on the upstream one, which is the point of it.
static uint32_t output_sample_rate = 48000;
constexpr uint32_t input_channels = 2;
static uint32_t output_channels = 2;

// Frames duplicated across a chunk boundary so the resampler has real samples to
// interpolate from at both ends instead of running off the edge of the buffer,
// and discarded again after conversion. Without it every chunk join is a click --
// which only arises because we resample at all, so the legacy path has no use
// for it.
constexpr uint32_t duplicated_input_frames = 4;
static uint32_t discarded_output_frames = 0;
static SDL_AudioCVT audio_convert{};

// Bytes per frame differs by path and getting it wrong is this port's most
// repeated bug: the legacy queue is interleaved s16 (4 bytes), the upstream one
// is interleaved float (8).
constexpr size_t upstream_bytes_per_frame = input_channels * sizeof(float);

// What SDL is holding, expressed in GAME frames on either path.
//
// The queue is the game's own s16 frames on the legacy path and resampled float
// frames on the upstream one, so a single byte divisor is wrong for one of them.
// Every audio bug this port has had came from a units slip on exactly this
// quantity, so it is computed once, here, and nowhere else. 64-bit throughout:
// a second of backlog times 48000 overflows 32 bits.
static size_t queued_game_frames();

static void update_audio_converter() {
    int ret = SDL_BuildAudioCVT(&audio_convert, AUDIO_F32, input_channels, static_cast<int>(audio_frequency),
                                AUDIO_F32, static_cast<Uint8>(output_channels), static_cast<int>(output_sample_rate));
    if (ret < 0) {
        fprintf(stderr, "[audio] SDL_BuildAudioCVT failed (%u -> %u Hz): %s\n",
                audio_frequency, output_sample_rate, SDL_GetError());
        return;
    }
    discarded_output_frames = duplicated_input_frames * output_sample_rate / audio_frequency;
}

static void set_frequency(uint32_t freq) {
    if (audio_legacy()) {
        if (audio_device != 0) {
            SDL_CloseAudioDevice(audio_device);
            audio_device = 0;
        }

        SDL_AudioSpec want{};
        want.freq = static_cast<int>(freq);
        want.format = AUDIO_S16SYS;
        want.channels = 2;
        want.samples = 1024;

        SDL_AudioSpec have{};
        audio_device = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
        if (audio_device == 0) {
            fprintf(stderr, "Failed to open audio device: %s\n", SDL_GetError());
            return;
        }
        // Always reported, not gated: a reopen mid-run is itself a glitch, and the
        // obtained spec is the first thing to check when the queue misbehaves.
        static unsigned opens = 0;
        fprintf(stderr, "[audio] legacy open #%u: want %u Hz %u ch, got %d Hz %d ch fmt=%04X buf=%u\n",
                ++opens, freq, want.channels, have.freq, have.channels,
                have.format, have.samples);
        audio_frequency = freq;
        SDL_PauseAudioDevice(audio_device, 0);
        return;
    }

    // Upstream path: the game's rate only ever changes the CONVERTER. The device
    // is opened once, at a rate a real device runs natively, and never reopened.
    audio_frequency = freq;

    if (audio_device == 0) {
        SDL_AudioSpec want{};
        want.freq = static_cast<int>(output_sample_rate);
        want.format = AUDIO_F32;
        want.channels = static_cast<Uint8>(output_channels);
        // 0x100, as upstream. See (2) above -- this is the granularity the game's
        // ~257-frame setpoint has to be regulated at.
        want.samples = 0x100;

        SDL_AudioSpec have{};
        audio_device = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
        if (audio_device == 0) {
            fprintf(stderr, "Failed to open audio device: %s\n", SDL_GetError());
            return;
        }
        output_sample_rate = static_cast<uint32_t>(have.freq);
        output_channels = have.channels;
        fprintf(stderr, "[audio] open: device %d Hz %d ch fmt=%04X buf=%u; game asks %u Hz (resampled)\n",
                have.freq, have.channels, have.format, have.samples, freq);
        SDL_PauseAudioDevice(audio_device, 0);
    }
    else {
        fprintf(stderr, "[audio] game rate now %u Hz; device stays at %u Hz\n", freq, output_sample_rate);
    }

    update_audio_converter();
}

// Reused across calls so a per-buffer allocation does not land on the audio path.
static std::vector<int16_t> audio_swap_buffer;

// Two things here are easy to get wrong, and both were.
//
// `sample_count` is the TOTAL number of int16 samples across both channels --
// ultramodern computes it as byte_count / sizeof(int16_t) in queue_audio_buffer
// -- NOT the per-channel frame count. Queueing sample_count * 2 * sizeof(int16_t)
// bytes hands SDL twice the length that exists, and its memcpy runs off the end
// of rdram. That is what crashed the first time the game pushed an audio buffer,
// and it crashed inside libSDL2, nowhere that looked like this code.
//
// Note this callback and get_frames_remaining below use DIFFERENT units:
// ultramodern multiplies that one's return by 2 * sizeof(int16_t) to get bytes,
// so it really is frames, while this one really is samples.
//
// The channel swap is not cosmetic. N64Recomp stores rdram with an address xor
// for endianness, so the two 16-bit halves of each word come out transposed --
// which, for interleaved stereo, is exactly left and right. Goemon64Recomp
// corrects the same thing in its own queue_samples, for the same reason.

// The backlog the game is allowed to SEE, in frames -- and nothing else.
// Reached only after a real stall: the game sizes each audio frame as
// `736 - osAiGetLength()/4 + 0x100` and its clamp floors the result at 720, so a
// healthy queue sits two orders of magnitude below this.
//
// This bounds exactly one thing: the number get_frames_remaining reports, which
// the game's s16 arithmetic genuinely constrains. How much SDL may *hold* is an
// unrelated question with a different answer -- see audio_queue_cap_frames
// below. Sharing this constant between the two cost a quarter of the audio.
// The largest value the game's own arithmetic survives -- NOT a round number,
// and 0x4000 was wrong.
//
// func_8001FD14_20914 computes `736 - reported + 0x100` and clamps the result up
// to 720 with `sltu` -- an UNSIGNED compare against a value it reloaded with
// `lh`, sign-extended. So the clamp only rescues a SMALL POSITIVE result. Past
// reported = 992 the subtraction goes negative, the sign-extended halfword reads
// as a huge unsigned number, `sltu` is false, and the clamp is skipped entirely:
//
//     reported =   992  ->  size =    720   (clamped, fine)
//     reported =  1008  ->  size =    -16
//     reported = 16384  ->  size = -15392
//
// The old cap of 0x4000 sat 16 times past that edge, so it did not prevent the
// wrap it was added for -- it relocated it. The comment it carried ("every value
// above the cap was already indistinguishable", because the game would just
// generate its minimum) is false for exactly this reason: above 992 the game does
// not generate its minimum, it generates a negative length. And the queue crossed
// 17184 frames on every cycle of the oscillation, so this fired constantly rather
// than only after a stall.
//
// 992 is `target + 0x100` with the game's own target of 736. Capping here costs
// nothing that was ever real: 992 already maps to the 720 floor, so every value
// this replaces was one the game could not use.
// MEASURED 2026-07-28, AND THE WHOLE PREMISE IS RETIRED: the game's size never
// leaves [720, 992], whatever this cap lets through.
//
// Ground truth, taken from the buffers the game actually synthesised
// (queue_samples receives them, so sample_count/2 IS the computed size) rather
// than from any model: over 209 gameplay windows with **10708 reports above
// 992**, the size was never once below the floor and never once above the
// ceiling, and `silent tasks` -- tasks that produced no buffer at all, which is
// what a size <= 0 would give via the `blez $s3` skip -- was **0 in every
// window**.
//
// So the failure this cap exists to prevent DOES NOT HAPPEN. Reporting 16384
// yields a 720-frame buffer, not a negative one. Whatever rescues it, the static
// reading of `sltu` against a sign-extended `lh` (see below) does not describe
// what runs -- most likely this is not the only audio-task builder, and the one
// on the hot path clamps differently. That is an open question, but it is no
// longer a blocking one, because the answer either way is that capping at 992
// fixes nothing that occurs.
//
// Consequences: `over-992` is NOT a defect counter and must not be read as one;
// the cap-992 experiment was treating a non-problem, which is the simplest
// explanation for it showing no benefit; and the crash regime past ~33760 is
// equally unobserved on this path. Keep the cap as cheap insurance, stop
// treating it as a lead.
//
// 0x4000 IS STILL THE DEFAULT, but NOT for the reason first written here.
//
// The original note claimed 992 "measured worse" (`consumed` 0.936 -> 0.876,
// empty-queue windows 20% -> 36%) and explained it by the negative length
// ACCIDENTALLY COMPENSATING -- pushing the game to generate more buffers.
// BOTH HALVES OF THAT ARE RETRACTED.
//
//  - A negative size makes the game generate NOTHING, so it cannot compensate
//    for anything. func_8002C4D0_2D0D0 takes the size in $s3, skips its first
//    loop on a signed `slt`, then hits `blez $s3` at 2D1C8 and skips the
//    synthesis loop outright.
//  - The 4031 over-992 events that motivated the change were sampled at RSP
//    dispatch, not at the game's read -- see get_frames_remaining. The mean size
//    in those same windows was exactly 720, which a game seeing >992 could not
//    have produced.
//
// With the defect rate unknown and one run per configuration on non-identical
// gameplay, the "regression" is most likely variance. 0x4000 stays default only
// because nothing has yet measured a reason to move it -- not because the wrap
// is doing useful work.
//
// There are two failure regimes past the edge, and they are NOT the same:
// between 992 and ~33760 the size is negative and the game falls silent; past
// ~33760 the s16 wraps back POSITIVE and the game overruns its 0xA000 command
// list, which is what actually crashed this port. 0x4000 sits between them.
//
// HH_AUDIO_REPORT_CAP=<frames> selects it. Re-measure with fixed content and
// n>=5 before concluding anything; and see docs/frametime-investigation.md,
// which argues the right fix is to change what "backlog" MEANS (report something
// FIFO-sized) rather than to cap what the whole SDL queue reports.
constexpr size_t max_backlog_frames_default = 0x4000;

static size_t max_backlog_frames_value() {
    static const size_t cap = [] {
        const char* env = getenv("HH_AUDIO_REPORT_CAP");
        if (env != nullptr) {
            const unsigned long long v = strtoull(env, nullptr, 0);
            if (v != 0) {
                return size_t{ v };
            }
        }
        return max_backlog_frames_default;
    }();
    return cap;
}
constexpr size_t bytes_per_frame = 2 * sizeof(int16_t);

static size_t queued_game_frames() {
    if (audio_device == 0) {
        return 0;
    }
    const uint64_t queued_bytes = SDL_GetQueuedAudioSize(audio_device);
    if (audio_legacy()) {
        return static_cast<size_t>(queued_bytes / bytes_per_frame);
    }
    if (output_sample_rate == 0 || audio_frequency == 0) {
        return 0;
    }
    return static_cast<size_t>(queued_bytes * audio_frequency
                               / (uint64_t{ output_sample_rate } * upstream_bytes_per_frame));
}

// How much real backlog SDL is allowed to hold before the queue is cleared.
//
// This used to be `max_backlog_frames` as well, on the reasoning that the two
// jobs meet at the same boundary. They do not, and sharing the constant cost a
// quarter of the game's audio.
//
// `max_backlog_frames` is bounded by the *game's arithmetic*: it sizes each
// frame as `target - osAiGetLength()/4` and keeps the result in an s16, so what
// the game is shown has to stay well under 33760. Nothing about SDL's queue is
// constrained by that. The only job of this second cap is to bound latency if
// the device genuinely stalls.
//
// Measured over 150-second runs, averaged across the windows past t=32s where
// the game is actually synthesising: mean production is 0.88-0.90 of the device
// rate on six cores and 0.96 on twelve -- the game is *behind*, not ahead --
// while the queue sat at 0 in most reporting windows.
//
// Against that, discarding the whole backlog at 0x4000 threw away roughly a
// quarter of everything synthesised -- over a million frames per 150-second run.
// The extra cores bought nothing, because the cap threw the surplus away as fast
// as they made it. That last part is the diagnosis, not a footnote: when more CPU
// buys zero improvement, CPU is not the binding constraint.
//
// The per-run figures live in one place, the A/B table in the README. Quoting
// them here too is how the first write-up ended up pairing one run's dropped
// count with another run's ratio.
//
// A producer that averages below the device rate cannot grow a queue without
// bound, so the cap does not need to be tight to be safe -- bursts drain
// themselves. Set it where only a real stall reaches it.
//
// HH_AUDIO_QUEUE_CAP=<frames> overrides it; 0x4000 reproduces the old behaviour.
static size_t audio_queue_cap_frames() {
    static const size_t cap = [] {
        const char* env = getenv("HH_AUDIO_QUEUE_CAP");
        if (env != nullptr) {
            size_t v = strtoull(env, nullptr, 0);
            if (v != 0) {
                return v;
            }
        }
        return size_t{ 0x10000 };
    }();
    return cap;
}

// HH_AUDIO_CLEAR_ON_OVERFLOW=1 restores the original overflow behaviour --
// discard the WHOLE backlog with SDL_ClearQueuedAudio and enqueue the new chunk
// anyway -- so the two discard policies can be A/B'd on one binary at one cap.
// Without this the comparison needs two builds, and the cap change and the
// discard change get credited to each other.
static bool audio_clear_on_overflow() {
    static const bool clear = getenv("HH_AUDIO_CLEAR_ON_OVERFLOW") != nullptr;
    return clear;
}

// Frames discarded by the cap since the last HH_TRACE_AUDIO report, so the
// report can account for every frame it produced. Written by the drop below and
// zeroed by the report; meaningless without HH_TRACE_AUDIO and costs nothing.
static size_t audio_dropped_frames_window = 0;

static void queue_samples(int16_t* audio_data, size_t sample_count) {
    if (audio_device == 0) {
        return;
    }

    // HH_TRACE_AUDIO=1 reports the production rate against wall time. If the
    // game generates more frames per second than the device consumes, the queue
    // grows without bound and no amount of draining sounds good -- that is a
    // rate bug, and this is what distinguishes it from a stall.
    static const bool trace_rate = getenv("HH_TRACE_AUDIO") != nullptr;
    if (trace_rate) {
        using clock = std::chrono::steady_clock;
        static clock::time_point t0 = clock::now();
        static size_t frames_since = 0;
        frames_since += sample_count / 2;

        // The game's own answer, before this function does anything to it.
        note_audio_size(sample_count / 2);

        // Peak amplitude over the window. A queue that drains on schedule proves
        // only that the device consumes; it says nothing about what it consumed.
        // Silence and music are indistinguishable by rate alone, so report the
        // loudest sample too -- a peak pinned at 0 means the game is synthesising
        // nothing and the fault is upstream of SDL entirely.
        static int peak_window = 0;
        for (size_t i = 0; i < sample_count; ++i) {
            int a = audio_data[i] < 0 ? -static_cast<int>(audio_data[i]) : audio_data[i];
            if (a > peak_window) {
                peak_window = a;
            }
        }

        // The high-water mark of the queue over the window, sampled before the
        // cap below can act on it. The instantaneous `queue` figure is taken once
        // every two seconds and is therefore blind to exactly the excursion that
        // matters: a producer this bursty can fill and overflow several times
        // between two reports, and every one of those sheds audio.
        // `qmax` against the cap is what says whether the cap is being hit by a
        // genuine stall or by ordinary jitter.
        //
        // qmax legitimately reads slightly ABOVE the cap -- by up to one chunk,
        // ~700 frames. The check runs before the enqueue, so the queue is allowed
        // to sit one chunk over until the next callback notices. 17088 against a
        // 16384 cap is that, not a leak.
        static unsigned queue_max_window = 0;
        unsigned queue_now = static_cast<unsigned>(queued_game_frames());
        if (queue_now > queue_max_window) {
            queue_max_window = queue_now;
        }

        auto now = clock::now();
        // What the device actually took, by accounting rather than assumption.
        // Everything produced either left through the device, was thrown away by
        // the cap, or is still sitting in the queue:
        //
        //     consumed = produced - dropped - (queue_end - queue_start)
        //
        // This is the number that separates the two explanations for a queue that
        // keeps hitting the cap. If `consumed` tracks the device rate, the game is
        // outproducing it and the fault is upstream. If `consumed` falls short,
        // the device stopped taking audio and the queue is a symptom -- the same
        // reading that turned 590 queued gfx items from a leak into a stalled
        // consumer.
        static unsigned queue_at_window_start = 0;
        // The game's own frame count at the last report. Audio production and
        // video frames have to be counted over the SAME window: two rates
        // sampled independently cannot be divided by each other, and the ratio --
        // samples of audio per video frame -- is the thing worth knowing. If it
        // is constant while the frame rate moves, audio production is a function
        // of frame rate and the audio shortfall IS the frame pacing.
        static uint64_t gfx_at_window_start = 0;
        // 2 s by default. HH_AUDIO_REPORT_MS shortens it, which matters more than
        // it looks: `qmax` sat at ~25000 in every window while the sampled `queue`
        // read 0 in some of them, so the 2 s series was ALIASING a faster
        // oscillation and the period read off it is not the real one. Measure the
        // period at a window shorter than the cycle before believing any figure
        // for it.
        static const double report_secs = [] {
            const char* env = getenv("HH_AUDIO_REPORT_MS");
            const double v = (env != nullptr) ? atof(env) : 0.0;
            return v > 0.0 ? v / 1000.0 : 2.0;
        }();
        double secs = std::chrono::duration<double>(now - t0).count();
        if (secs >= report_secs) {
            const uint64_t gfx_now =
                hybridheaven::renderer::gfx_tasks_submitted.load(std::memory_order_relaxed);
            const uint64_t gfx_in_window = gfx_now - gfx_at_window_start;
            double produced = frames_since / secs;
            double consumed = (double(frames_since) - double(audio_dropped_frames_window)
                               - (double(queue_now) - double(queue_at_window_start))) / secs;
            const uint64_t tasks = audio_tasks_window.exchange(0, std::memory_order_relaxed);
            const uint64_t rsum = audio_reported_sum.exchange(0, std::memory_order_relaxed);
            const uint32_t rmax = audio_reported_max.exchange(0, std::memory_order_relaxed);
            const uint32_t rover = audio_reported_over_992.exchange(0, std::memory_order_relaxed);
            const uint64_t rreads = audio_reported_reads.exchange(0, std::memory_order_relaxed);
            const uint64_t bufs = audio_buffers_window.exchange(0, std::memory_order_relaxed);
            const uint64_t s_floor = audio_size_at_floor.exchange(0, std::memory_order_relaxed);
            const uint64_t s_band = audio_size_in_band.exchange(0, std::memory_order_relaxed);
            const uint64_t s_ceil = audio_size_at_ceiling.exchange(0, std::memory_order_relaxed);
            const uint32_t s_min = audio_size_min.exchange(UINT32_MAX, std::memory_order_relaxed);
            const uint32_t s_max = audio_size_max.exchange(0, std::memory_order_relaxed);
            const uint64_t asp_us = audio_task_us.exchange(0, std::memory_order_relaxed);
            fprintf(stderr, "[audio] tasks %.1f/s, mean size %.0f, reported mean %.0f max %u, "
                            "over-992 %u; reads %llu (%.2f/task); aspMain %.2fms/task, %.0f%% of wall\n",
                    tasks / secs,
                    tasks ? double(frames_since) / double(tasks) : 0.0,
                    tasks ? double(rsum) / double(tasks) : 0.0, rmax, rover,
                    (unsigned long long)rreads, tasks ? double(rreads) / double(tasks) : 0.0,
                    tasks ? (double(asp_us) / 1000.0) / double(tasks) : 0.0,
                    100.0 * (double(asp_us) / 1e6) / secs);
            // The controller's OUTPUT, measured rather than modelled. `silent`
            // is tasks that produced no buffer at all -- the `blez $s3` skip --
            // and is the number that says whether the port is handing the game
            // arithmetic it cannot use. It is a signed difference on purpose: a
            // small negative just means a buffer landed either side of a window
            // boundary, whereas a persistent positive is the real thing.
            fprintf(stderr, "[audio] size: %llu buffers (floor %llu, in-band %llu, ceiling %llu), "
                            "min %u max %u; silent tasks %lld\n",
                    (unsigned long long)bufs,
                    (unsigned long long)s_floor, (unsigned long long)s_band,
                    (unsigned long long)s_ceil,
                    s_min == UINT32_MAX ? 0u : s_min, s_max,
                    (long long)tasks - (long long)bufs);
            // The VI thread enqueues one AI message per VI (60/s, ungated), but
            // they are only DELIVERED when a guest thread reaches a scheduling
            // point and calls dequeue_external_messages. A backlog here means the
            // interrupts were offered and the guest never collected them, which
            // is a different fault from the guest collecting them and declining
            // to act -- and neither is "the audio task is slow", which aspMain's
            // 0.2 ms already rules out.
            // Backlog AND cumulative drops. The backlog alone cannot see a drop
            // -- a message is popped before delivery is attempted -- and citing
            // it as though it could was one of this investigation's wrong turns.
            // Drops are what would starve the guest of retraces, and each lost
            // retrace is one audio frame the game never runs.
            static uint64_t last_drops = 0;
            const uint64_t drops_now = ultramodern::debug_external_message_drops();
            static uint64_t last_retraces = 0;
            const uint64_t retraces_now = ultramodern::debug_retrace_messages_sent();
            fprintf(stderr, "[audio] retrace messages sent %.1f/s\n",
                    double(retraces_now - last_retraces) / secs);
            last_retraces = retraces_now;
            static uint64_t last_gsf = 0;
            const uint64_t gsf_now = ultramodern::debug_guest_send_failures();
            fprintf(stderr, "[audio] guest osSendMesg failures %llu total (%.1f/s)\n",
                    (unsigned long long)gsf_now, double(gsf_now - last_gsf) / secs);
            last_gsf = gsf_now;
            // The two counters that discriminate the trough mechanisms.
            //
            // `yields` is the guest audio dispatcher entering its yield/handoff
            // path -- it found a gfx task in flight on the RSP when the audio
            // task arrived. If yields/s tracks tasks/s in trough windows, the
            // audio cadence is serialized against the gfx frame through the
            // shared SP-done queue at sc+0xE8.
            //
            // `ext delivery` is how long external messages (retrace, SP done,
            // DP done) sat in the port's queue before ANY guest thread pumped
            // them. The guest cannot see an event before this latency elapses,
            // and the audio chain pays it at least twice per frame (retrace in,
            // SP done back). A mean that is a large fraction of the 16.7 ms VI
            // period in trough windows -- while boot windows show near zero --
            // is the cooperative-delivery quantization, and it is the number
            // HH_AUDIO_INLINE_RSP exists to halve.
            static uint64_t last_yields = 0;
            const uint64_t yields_now = ultramodern::debug_sp_task_yields();
            uint64_t deliv_count = 0, deliv_total_us = 0, deliv_max_us = 0;
            ultramodern::debug_external_delivery_window(&deliv_count, &deliv_total_us, &deliv_max_us);
            // osgettime yields: how often HH_OSGETTIME_YIELD_US actually turned
            // the governor's spin into a scheduling point. 0.0/s with the switch
            // set means it never engaged -- the run measured nothing, rather than
            // measuring a fix that did not work.
            static uint64_t last_ogt_yields = 0;
            const uint64_t ogt_yields_now = ultramodern::debug_osgettime_yields();
            fprintf(stderr, "[audio] sp-task yields %.1f/s; osgettime yields %.1f/s; "
                            "ext delivery %llu msgs, mean %.2fms, max %.2fms\n",
                    double(yields_now - last_yields) / secs,
                    double(ogt_yields_now - last_ogt_yields) / secs,
                    (unsigned long long)deliv_count,
                    deliv_count ? (double(deliv_total_us) / 1000.0) / double(deliv_count) : 0.0,
                    double(deliv_max_us) / 1000.0);
            last_yields = yields_now;
            last_ogt_yields = ogt_yields_now;
            fprintf(stderr, "[audio] external message backlog %zu; drops %llu total (%.1f/s)\n",
                    ultramodern::debug_external_message_count(),
                    (unsigned long long)drops_now,
                    double(drops_now - last_drops) / secs);
            last_drops = drops_now;
            fprintf(stderr, "[audio] produced %.0f frames/s over %.1fs; device wants %u; "
                            "ratio %.3f; consumed %.0f/s (%.3f); dropped %zu; "
                            "queue %u frames; qmax %u (cap %zu); peak %d; "
                            "gfx %llu (%.1f/s, %.0f samples/frame)\n",
                    produced, secs, audio_frequency,
                    audio_frequency ? produced / audio_frequency : 0.0,
                    consumed, audio_frequency ? consumed / audio_frequency : 0.0,
                    audio_dropped_frames_window,
                    queue_now, queue_max_window, audio_queue_cap_frames(), peak_window,
                    (unsigned long long)gfx_in_window, gfx_in_window / secs,
                    gfx_in_window ? double(frames_since) / double(gfx_in_window) : 0.0);
            t0 = now;
            frames_since = 0;
            peak_window = 0;
            queue_max_window = 0;
            queue_at_window_start = queue_now;
            gfx_at_window_start = gfx_now;
            audio_dropped_frames_window = 0;
        }
    }

    // Bound the real queue, not just the number the game is shown -- by refusing
    // new audio, not by deleting audio already accepted.
    //
    // The cap in get_frames_remaining stops the game's arithmetic from wrapping,
    // but it does nothing to SDL's queue, which after a long stall -- an llvmpipe
    // hitch, a window drag -- holds seconds of audio and never gives them back.
    // The game cannot drain it: it has no ceiling on its own frame size and a
    // floor of 720, so once the backlog is past the report cap every response it
    // makes is the same one, and the latency is permanent. Something on this side
    // has to bound it.
    //
    // This used to call SDL_ClearQueuedAudio, which discards the ENTIRE backlog.
    // At the current cap that is ~1.5 s of audio gone in one hole. Dropping only
    // the chunk that would overflow costs ~700 frames -- about 16 ms -- and turns
    // the cap into what it should be: a latency ceiling the stream leans against,
    // shedding the excess one chunk at a time until production falls back below
    // the device rate. The cap and the clear were written together, so how much
    // damage the clear did was never separable from the cap being too low.
    //
    // At the default cap this path never fires under llvmpipe -- production
    // averages 0.92-0.97 of the device rate, the queue drains itself, and a
    // 150-second run at 0x10000 hits the cap zero times. So the policy was
    // measured at 0x4000, where it fires constantly, against the old behaviour on
    // the same binary via HH_AUDIO_CLEAR_ON_OVERFLOW. Refusing the chunk wins:
    // see the table in the README. It should matter more on hardware that can
    // sustain production >= 1.0, which is where clearing deletes 1.5 s a time.
    //
    // One caveat is real. For a genuine *device* stall, clear-all resynchronised
    // to fresh audio immediately where this replays up to a capful of stale audio
    // first. That trade is deliberate: overproduction is the case that recurs,
    // a stalled device is already broken.
    //
    // Note the trip COUNT rises sharply when this policy is in force at a cap
    // that is too low -- the queue parks at the cap and refuses chunk after chunk,
    // rather than emptying and refilling. Compare frames shed, never trip counts:
    // ~1500 refusals of 720 frames beat ~70 clears of 16,700.
    //
    // Applies on BOTH paths, in game frames so the comparison means the same
    // thing either side. It was legacy-only for one build, while the upstream path
    // bounded latency by thinning instead -- but thinning turned out to invent
    // audio (see the skip_factor comment below), so it is off by default and this
    // is once again the only valve. It should almost never fire: the game throttles
    // itself once it can see the buffer, and the cap is set for a real stall.
    // In FRAMES, not bytes -- the name was `queued_bytes` while this was
    // legacy-only and s16, and carrying that name over while changing the unit is
    // precisely how this port lost a quarter of its audio once already.
    const size_t queued_frames = queued_game_frames();
    if (queued_frames > audio_queue_cap_frames()) {
        // Logged on powers of two rather than once or every time: one line does
        // not distinguish a single stall from a standing condition, and every
        // line would bury the run. The gap between reports IS the frequency.
        static unsigned drops = 0;
        static const auto boot = std::chrono::steady_clock::now();
        ++drops;
        const bool clear_all = audio_clear_on_overflow();
        const size_t shed = clear_all ? queued_frames : sample_count / 2;
        audio_dropped_frames_window += shed;
        if ((drops & (drops - 1)) == 0) {
            double at = std::chrono::duration<double>(
                            std::chrono::steady_clock::now() - boot).count();
            fprintf(stderr, "[audio] t=%6.1fs queue at %zu frames exceeds cap %zu -- "
                            "%s (%zu frames; %u time%s so far)\n",
                    at, queued_frames,
                    audio_queue_cap_frames(),
                    clear_all ? "CLEARED THE BACKLOG" : "dropped this chunk",
                    shed, drops, drops == 1 ? "" : "s");
        }
        if (clear_all) {
            SDL_ClearQueuedAudio(audio_device);
        } else {
            return;
        }
    }

    if (!audio_legacy()) {
        // Upstream's queue_samples, with this port's volume control folded into
        // the same pass. Reused buffers so nothing allocates on the audio path.
        static std::vector<float> swap_buffer;
        static std::array<float, duplicated_input_frames * input_channels> duplicated_sample_buffer{};

        const size_t resampled_sample_count = sample_count + duplicated_input_frames * input_channels;
        const size_t max_sample_count = std::max<size_t>(resampled_sample_count,
                                                         resampled_sample_count * std::max(audio_convert.len_mult, 1));
        if (max_sample_count > swap_buffer.size()) {
            swap_buffer.resize(max_sample_count);
        }

        // A chunk shorter than the duplicated run would read past its own end
        // below. Upstream asserts this away; refusing is better than a torn read.
        if (sample_count <= duplicated_input_frames * input_channels) {
            return;
        }

        // Carry the tail of the previous chunk in as this chunk's head, so the
        // resampler interpolates across the join from real samples.
        for (size_t i = 0; i < duplicated_input_frames * input_channels; i++) {
            swap_buffer[i] = duplicated_sample_buffer[i];
        }

#if defined(HH_UI)
        const float cur_main_volume = hybridheaven::get_main_volume() / 100.0f;
#else
        const float cur_main_volume = 1.0f;
#endif
        // 1.0f/32768.0f, as Quest64-Recomp. Goemon and Zelda64Recomp use 0.5f,
        // i.e. 6 dB quieter -- deliberately NOT copied, because this port's audio
        // was judged good by ear at full scale and halving it here would be an
        // unannounced level change riding along with a structural one.
        //
        // The channel swap is not cosmetic: N64Recomp stores rdram with an
        // address xor for endianness, so the halves of each word come out
        // transposed, which for interleaved stereo is exactly left and right.
        for (size_t i = 0; i + 1 < sample_count; i += input_channels) {
            swap_buffer[i + 0 + duplicated_input_frames * input_channels] = audio_data[i + 1] * (1.0f / 32768.0f) * cur_main_volume;
            swap_buffer[i + 1 + duplicated_input_frames * input_channels] = audio_data[i + 0] * (1.0f / 32768.0f) * cur_main_volume;
        }

        for (size_t i = 0; i < duplicated_input_frames * input_channels; i++) {
            duplicated_sample_buffer[i] = swap_buffer[i + sample_count];
        }

        audio_convert.buf = reinterpret_cast<Uint8*>(swap_buffer.data());
        audio_convert.len = static_cast<int>(resampled_sample_count * sizeof(swap_buffer[0]));

        if (SDL_ConvertAudio(&audio_convert) < 0) {
            fprintf(stderr, "[audio] SDL_ConvertAudio failed: %s\n", SDL_GetError());
            return;
        }

        const uint64_t cur_queued_microseconds =
            uint64_t(SDL_GetQueuedAudioSize(audio_device)) / upstream_bytes_per_frame * 1000000 / output_sample_rate;
        uint32_t num_bytes_to_queue = audio_convert.len_cvt
                                    - static_cast<uint32_t>(output_channels * discarded_output_frames * sizeof(swap_buffer[0]));
        float* samples_to_queue = swap_buffer.data() + output_channels * discarded_output_frames / 2;

        // Decimate rather than refuse: past the threshold, keep one sample in 2^n.
        // Capped at 8 (1 in 256) because `1u << skip_factor` is undefined once it
        // reaches the shift width -- upstream has no cap and is wrong about it
        // past a ~3.2 s backlog, which a device stall reaches.
        //
        // THE THRESHOLD IS NOT UPSTREAM'S 100 ms, deliberately. At 100 ms this shed
        // 20k-63k frames per 2-second window -- a third to a half of everything the
        // game made -- and delivered 0.712 against the old path's 0.905, while
        // production itself was healthy at up to 1.46x. The cause is the shape of
        // THIS game's controller: it modulates over only a 272-frame band
        // (`992 - remaining`, floored at 720), which the queue crosses in about
        // 6 ms, so its output is effectively bang-bang and the queue oscillates
        // with a ~300 ms amplitude. A threshold inside that amplitude does not trim
        // a surplus, it destroys the compensation that the offset just restored.
        // Set it clear of the swing, so it is what it should be: a safety valve for
        // a genuine device stall, not a term in the control loop.
        //
        // Same error this port already made once, when the report cap and the queue
        // cap were one constant -- a shedding policy at the wrong operating point
        // costs a large fraction of the audio and reads as underproduction.
        //
        // AND IT IS OFF BY DEFAULT, because thinning is not free the way the
        // upstream comment implies. Keeping one sample in 2^n with no filtering is
        // decimation without an anti-alias stage: it does not merely remove audio,
        // it FOLDS everything above the new Nyquist back down as new content. The
        // user reported "additional sounds that are not supposed to be there" on
        // the 100 ms build, which is exactly that signature and exactly when this
        // was firing hardest. A latency valve must not invent audio.
        //
        // With the VI offset restoring the game's own throttle, the queue is
        // bounded by the game anyway, so this is not load-bearing. The cap above
        // still bounds a genuine device stall by refusing chunks -- a hole, but an
        // honest one. HH_AUDIO_SKIP_MS=<ms> re-enables thinning (100 = upstream).
        static const uint64_t skip_threshold_us = [] {
            const char* env = getenv("HH_AUDIO_SKIP_MS");
            if (env != nullptr) {
                const unsigned long long v = strtoull(env, nullptr, 0);
                if (v != 0) {
                    return uint64_t{ v } * 1000;
                }
            }
            return uint64_t{ 0 };
        }();
        uint32_t skip_factor = skip_threshold_us == 0
                                 ? 0u
                                 : static_cast<uint32_t>(cur_queued_microseconds / skip_threshold_us);
        if (skip_factor > 8) {
            skip_factor = 8;
        }
        if (skip_factor != 0) {
            const uint32_t skip_ratio = 1u << skip_factor;
            const size_t before = num_bytes_to_queue / upstream_bytes_per_frame;
            num_bytes_to_queue /= skip_ratio;
            for (size_t i = 0; i < num_bytes_to_queue / (output_channels * sizeof(swap_buffer[0])); i++) {
                samples_to_queue[2 * i + 0] = samples_to_queue[2 * skip_ratio * i + 0];
                samples_to_queue[2 * i + 1] = samples_to_queue[2 * skip_ratio * i + 1];
            }
            // Thinned frames are still frames that never reach the device, so the
            // accounting has to see them or `consumed` stops closing.
            audio_dropped_frames_window += before - num_bytes_to_queue / upstream_bytes_per_frame;
        }

        SDL_QueueAudio(audio_device, samples_to_queue, num_bytes_to_queue);
        return;
    }

    if (audio_swap_buffer.size() < sample_count) {
        audio_swap_buffer.resize(sample_count);
    }

    // Main volume rides along on the channel swap that has to happen anyway, so
    // it costs one multiply per sample and no extra pass. Scaling here rather
    // than with SDL_MixAudio keeps it out of the queue accounting above: the
    // frame counts stay the counts the game produced.
    //
    // At 100 the multiply is skipped outright, so the default path is
    // bit-identical to what it was before there was a volume control -- which
    // matters, because the audio in this port was got right by ear and every
    // figure in the README was measured on that path.
#if defined(HH_UI)
    const int volume = hybridheaven::get_main_volume();
#else
    const int volume = 100;
#endif

    size_t i = 0;
    if (volume == 100) {
        for (; i + 1 < sample_count; i += 2) {
            audio_swap_buffer[i + 0] = audio_data[i + 1];
            audio_swap_buffer[i + 1] = audio_data[i + 0];
        }
        if (i < sample_count) {
            audio_swap_buffer[i] = audio_data[i];
        }
    } else {
        // int32 for the product: an s16 at full scale times 100 overflows s16
        // long before the divide brings it back.
        auto scale = [volume](int16_t s) -> int16_t {
            return static_cast<int16_t>(static_cast<int32_t>(s) * volume / 100);
        };
        for (; i + 1 < sample_count; i += 2) {
            audio_swap_buffer[i + 0] = scale(audio_data[i + 1]);
            audio_swap_buffer[i + 1] = scale(audio_data[i + 0]);
        }
        if (i < sample_count) {
            audio_swap_buffer[i] = scale(audio_data[i]);
        }
    }

    SDL_QueueAudio(audio_device, audio_swap_buffer.data(),
                   static_cast<Uint32>(sample_count * sizeof(int16_t)));
}

static size_t get_frames_remaining() {
    if (audio_device == 0) {
        return 0;
    }

    size_t queued = queued_game_frames();

    // Cap what the backlog is allowed to look like.
    //
    // On hardware osAiGetLength reports the length of the DMA the AI is playing
    // -- one buffer, a couple of thousand samples at most. It physically cannot
    // report a second of audio. SDL's queue can, and does after a stall, and a
    // game that sizes its next frame from the difference then computes something
    // its own arithmetic cannot hold.
    //
    // Hybrid Heaven's audio frame is `736 - osAiGetLength()/4 + 0x100`, masked to
    // 0xFFF0 and stored as an s16 (func_8001FD14_20914). Its clamp raises the
    // result to a floor of 720 and has no ceiling, so a backlog above 33760
    // frames wraps that s16 to a large POSITIVE count, and the game synthesises
    // ~32700 samples in one frame -- overrunning its 0xA000 command list into the
    // OSTask that follows it, and exhausting the audio DMA free list on the way.
    // Both of the port's remaining failures were that.
    //
    // Capping costs nothing in behaviour: the game's own target is 736 frames
    // (44100/60) before the 0x100 bias, so 992 once biased, and any backlog past
    // that already produces the same "generate the minimum" response -- every
    // value above the cap was already indistinguishable. The bound is chosen well
    // clear of the s16 the game keeps the result in.
    //
    // This cap does NOT bound latency, and must not be confused with the one that
    // does. queue_samples enforces audio_queue_cap_frames() against the real
    // queue; this number only shapes what the game is told.
    //
    // HH_AUDIO_VI_OFFSET=1 reports one video frame's worth LESS than is really
    // queued, which is what Goemon64Recomp does unconditionally -- its comment
    // says it "prevents audio popping on games that use the buffered audio byte
    // count to determine how many samples to generate". Hybrid Heaven is exactly
    // such a game, and measurement says the honest number has disabled its rate
    // control completely:
    //
    //   the game sizes each task as `992 - remaining` and clamps UP to a floor of
    //   720 (target 736 and floor 720, read out of .main_bss). Anything at or
    //   above 272 remaining therefore produces 720, and every value this function
    //   has ever been observed to return -- 720, or the 0x4000 cap -- is above it.
    //   Across 958 traced tasks the game generated 709-720 frames EVERY time, in
    //   both boot and gameplay, so production is purely `720 x tasks-per-second`
    //   and the controller has no authority at all. A slow window can never be
    //   made up, because the mechanism for making it up is pinned shut.
    //
    // Subtracting a VI puts the controller back in range: a real queue of 720
    // reports 0, the game generates its full 992, and it has headroom in both
    // directions again. Opt-in rather than default because it is unmeasured here
    // -- Goemon can afford to over-produce because it sheds the surplus by
    // decimating samples, where this port refuses whole chunks, so the latency
    // consequence has to be measured before this becomes the shipped behaviour.
    // Report one VI's worth LESS than is really queued -- upstream's
    // `buffer_offset_frames = 1.0f`, and the whole reason the game's rate control
    // works there and not here. Its comment: this "prevents audio popping on
    // games that use the buffered audio byte count to determine how many samples
    // to generate". Hybrid Heaven computes `992 - remaining` and clamps up to a
    // floor of 720, so it needs to SEE something under 272 to ever generate more
    // than the minimum, and the honest number never is.
    //
    // On the legacy path this is off, because every figure that path produced was
    // measured without it. HH_AUDIO_VI_OFFSET=1 turns it on there too, which is
    // how the offset alone was A/B'd against the offset plus everything else.
    const bool offset = !audio_legacy() || getenv("HH_AUDIO_VI_OFFSET") != nullptr;
    if (offset && queued != 0 && audio_frequency != 0) {
        const size_t frames_per_vi = audio_frequency / 60;
        queued = (queued > frames_per_vi) ? (queued - frames_per_vi) : 0;
    }

    // Damp the game's rate controller, because at unity gain it oscillates.
    //
    // This is the fix for the periodic hitch: measured on BOTH builds -- llvmpipe
    // and an RTX 5080 at `consumed` 0.89-1.00, so it is not a performance
    // shortfall -- the SDL queue limit-cycles between ~25000 frames and ZERO,
    // and the game's audio work per rendered frame swings 1141 <-> 1745 samples
    // in lockstep with it. The empty end of every cycle is an underrun; the full
    // end is a 50% burst of RSP audio DSP on the guest thread. Those are the
    // audio dropout and the frametime hitch, and they are one bug.
    //
    // The loop, from func_8001FD14_20914 (asm/usa/main_20420.s) and confirmed
    // against the RSP trace:
    //
    //     size = max_u(720, (736 - osAiGetLength()/4 + 0x100) & 0xFFF0)
    //
    // so `size = 992 - reported`, with a floor of 720. Write the queue as an
    // integrator over tasks, c = frames consumed per task interval:
    //
    //     Q[n+1] = Q[n] + size[n] - c
    //
    // The game computes size[n] for a buffer that is queued BEHIND the one
    // playing, so the reported length it acts on is a task stale -- the
    // correction lands one step late, and the port adds SDL's own 256-frame
    // device buffer on top of that. With reported = Q[n-1] - offset the
    // characteristic polynomial is
    //
    //     z^2 - z + 1/K = 0        (K = 1 with no damping)
    //
    // whose roots at K = 1 have magnitude exactly 1. The loop sits precisely ON
    // the unit circle: a sustained oscillation that neither grows nor decays,
    // which is exactly the sawtooth in the traces. It is not marginal by a
    // little -- it is marginal by construction, and no amount of host speed
    // moves it, which is why the RTX 5080 oscillates identically to llvmpipe.
    //
    // Dividing the reported deviation by K divides the loop gain by K and pulls
    // the roots inside the circle: |z| = 1/sqrt(K), so K = 4 gives |z| = 0.5 and
    // an oscillation that dies in a few tasks instead of never.
    //
    // The equilibrium moves but stays hardware-shaped. The fixed point is
    // Q* = 110*K + offset, so K = 4 parks the queue near 1240 frames -- about
    // 28 ms, still inside the couple of buffers the AI's own FIFO would hold on
    // hardware, and well short of the 570 ms the undamped loop was reaching.
    // What this does NOT do is invent or discard a single sample: it only
    // changes the number the game is shown, which is the same lever upstream's
    // one-VI offset already pulls.
    //
    // MEASURED, AND IT DID NOT WORK -- default 1 (off). K = 4 gave `consumed`
    // 0.915 against 0.936 undamped and put MORE windows at an empty queue (26%
    // against 20%), so the theory above, however tidy, does not describe the
    // dominant term. What it misses is that the game varies its TASK RATE far
    // more than its buffer size: 29.8/s while it is idling and 90-120/s in a
    // burst, against a VI thread offering 60 interrupts/s. Damping the size
    // report cannot touch that, which is why it moved nothing.
    //
    // Kept, with its derivation, because the derivation is still correct about
    // the loop it models -- it is just not the loop that dominates. Anything that
    // later fixes the task-rate burst will want this switch to re-test the size
    // loop underneath it.
    //
    // HH_AUDIO_DAMP=<K> enables it.
    static const size_t damp = [] {
        const char* env = getenv("HH_AUDIO_DAMP");
        if (env != nullptr) {
            const unsigned long long v = strtoull(env, nullptr, 0);
            if (v != 0) {
                return size_t{ v };
            }
        }
        return size_t{ 1 };
    }();
    if (damp > 1) {
        queued /= damp;
    }

    const size_t cap = max_backlog_frames_value();
    const size_t reported = queued < cap ? queued : cap;

    // Account for the value HERE, because this function IS osAiGetLength -- this
    // is the only point that sees what the game actually reads.
    //
    // The previous version sampled at RSP dispatch instead, and that produced a
    // false finding that reached a handover document: "over-992 fired 4031 times
    // in 81 s", read as the game being handed arithmetic it cannot do. The
    // game's own output refutes it. Past 992 the size goes negative, and a
    // negative size synthesises NOTHING -- func_8002C4D0_2D0D0 reaches
    // `blez $s3` at 2D1C8 with the size in $s3 and skips the synthesis loop,
    // having already skipped the loop above it on a signed `slt`. Yet the drain
    // windows that supposedly saw >992 on nearly every task reported a mean size
    // of exactly 720. A game generating 720 did not see a value above 992.
    //
    // The two reads disagree because dispatch happens after the game has sized
    // its buffer AND synthesised into it, so the queue has grown in between.
    // Treat the 4031 figure as retracted; this counter replaces it.
    //
    // Gated, so an untraced build does not pay for it and -- more importantly --
    // so the instrumentation does not perturb the path it is measuring.
    audio_last_reported.store(static_cast<uint32_t>(reported), std::memory_order_relaxed);
    if (trace_audio_enabled()) {
        audio_reported_reads.fetch_add(1, std::memory_order_relaxed);
        audio_reported_sum.fetch_add(reported, std::memory_order_relaxed);
        uint32_t r = static_cast<uint32_t>(reported);
        uint32_t prev_max = audio_reported_max.load(std::memory_order_relaxed);
        while (r > prev_max
               && !audio_reported_max.compare_exchange_weak(prev_max, r,
                                                            std::memory_order_relaxed)) {
        }
        // The count that matters is not "above 992" but "the game will now
        // synthesise nothing", which is the same event stated as the game sees
        // it. Both regimes do it: above 992 the size is negative, and above
        // ~33760 it wraps back to a large POSITIVE size, which is the overrun
        // that caused this port's earlier crashes rather than mere silence.
        if (reported > 992) {
            audio_reported_over_992.fetch_add(1, std::memory_order_relaxed);
        }
    }

    return reported;
}

#if !defined(HH_UI)
// Input, for the no-UI build only. A fixed keyboard layout as controller 1, with
// no remapper, no gamepad and a stick that reads full deflection or nothing.
// src/game/input.cpp + controls.cpp replace all of it when HH_UI is on; this
// stays because the bisect build has to be able to press Start without dragging
// in RmlUi to do it.
//
//   Enter  Start        X  A        C  B        Space  Z
//   W A S D  analog stick        arrows  D-pad
//   I J K L  C-up/left/down/right     Q  L        E  R
//
// The N64 button bits are libultra's CONT_* values.
namespace {
    constexpr uint16_t BTN_A       = 0x8000;
    constexpr uint16_t BTN_B       = 0x4000;
    constexpr uint16_t BTN_Z       = 0x2000;
    constexpr uint16_t BTN_START   = 0x1000;
    constexpr uint16_t BTN_DUP     = 0x0800;
    constexpr uint16_t BTN_DDOWN   = 0x0400;
    constexpr uint16_t BTN_DLEFT   = 0x0200;
    constexpr uint16_t BTN_DRIGHT  = 0x0100;
    constexpr uint16_t BTN_L       = 0x0020;
    constexpr uint16_t BTN_R       = 0x0010;
    constexpr uint16_t BTN_CUP     = 0x0008;
    constexpr uint16_t BTN_CDOWN   = 0x0004;
    constexpr uint16_t BTN_CLEFT   = 0x0002;
    constexpr uint16_t BTN_CRIGHT  = 0x0001;

    struct KeyBinding {
        SDL_Scancode key;
        uint16_t button;
    };

    constexpr KeyBinding key_bindings[] = {
        { SDL_SCANCODE_RETURN, BTN_START },
        { SDL_SCANCODE_X,      BTN_A     },
        { SDL_SCANCODE_C,      BTN_B     },
        { SDL_SCANCODE_SPACE,  BTN_Z     },
        { SDL_SCANCODE_Q,      BTN_L     },
        { SDL_SCANCODE_E,      BTN_R     },
        { SDL_SCANCODE_UP,     BTN_DUP    },
        { SDL_SCANCODE_DOWN,   BTN_DDOWN  },
        { SDL_SCANCODE_LEFT,   BTN_DLEFT  },
        { SDL_SCANCODE_RIGHT,  BTN_DRIGHT },
        { SDL_SCANCODE_I,      BTN_CUP    },
        { SDL_SCANCODE_K,      BTN_CDOWN  },
        { SDL_SCANCODE_J,      BTN_CLEFT  },
        { SDL_SCANCODE_L,      BTN_CRIGHT },
    };
}

// The main thread's SDL_PollEvent in update_gfx is what keeps SDL's keyboard
// state current, so there is nothing to pump here.
static void poll_input() {}

static bool get_input(int controller_num, uint16_t* buttons, float* x, float* y) {
    if (controller_num != 0) {
        return false;
    }

    // Read from any thread. SDL_GetKeyboardState hands back a pointer to the
    // array the event pump writes; reading it while the main thread updates it
    // can only ever miss or double-report a single frame of a keypress.
    const Uint8* keys = SDL_GetKeyboardState(nullptr);
    if (keys == nullptr) {
        *buttons = 0;
        *x = 0.0f;
        *y = 0.0f;
        return true;
    }

    uint16_t pressed = 0;
    for (const KeyBinding& binding : key_bindings) {
        if (keys[binding.key]) {
            pressed |= binding.button;
        }
    }
    // Report every change in the button word. This is an edge, not a poll, so a
    // held key costs one line and an idle run costs none -- cheap enough to
    // leave on unconditionally. It exists because "did the keypress reach the
    // game" is otherwise unanswerable from a log, and this machine cannot
    // screenshot the window: a run that behaves identically with and without a
    // press is ambiguous between "the press changed nothing" and "the press
    // never arrived".
    {
        static uint16_t last_buttons = 0;
        if (pressed != last_buttons) {
            fprintf(stderr, "[input] buttons %04X -> %04X\n", last_buttons, pressed);
            last_buttons = pressed;
        }
    }

    *buttons = pressed;

    // The stick is reported in the -1..1 range ultramodern scales to the N64's.
    *x = float(keys[SDL_SCANCODE_D] ? 1 : 0) - float(keys[SDL_SCANCODE_A] ? 1 : 0);
    *y = float(keys[SDL_SCANCODE_W] ? 1 : 0) - float(keys[SDL_SCANCODE_S] ? 1 : 0);
    return true;
}

static void set_rumble(int /*controller_num*/, bool /*rumble*/) {}

static ultramodern::input::connected_device_info_t get_connected_device_info(int controller_num) {
    if (controller_num != 0) {
        return { ultramodern::input::Device::None, ultramodern::input::Pak::None };
    }
    // No Controller Pak: the osPfs subsystem is still under placeholder symbols
    // and reaching it stops the program on purpose. See
    // src/game/ultra_missing.cpp.
    return { ultramodern::input::Device::Controller, ultramodern::input::Pak::None };
}
#endif // !HH_UI

// The window. Deliberately not static: the UI reaches it by `extern SDL_Window*
// window` from ui_state.cpp (to build its RmlUi system interface) and
// ui_config.cpp (to toggle fullscreen, which RT64 cannot do itself on Linux).
// That is how Goemon64Recomp's main.cpp declares it, and the UI was taken from
// there unchanged.
SDL_Window* window = nullptr;

static ultramodern::gfx_callbacks_t::gfx_data_t create_gfx() {
    if (SDL_InitSubSystem(SDL_INIT_VIDEO) < 0) {
        fprintf(stderr, "Failed to initialize SDL video: %s\n", SDL_GetError());
    }
    return nullptr;
}

// ultramodern's WindowHandle is a plain SDL_Window* on Linux, but on Windows it
// is { HWND, thread id } -- so neither return path below can simply hand back
// `window`. Both go through here, including the failure path: a default-
// constructed handle is the closest thing to the null SDL_Window* that path
// returns on Linux.
static ultramodern::renderer::WindowHandle to_window_handle(SDL_Window* w) {
#if defined(_WIN32)
    if (w == nullptr) {
        return ultramodern::renderer::WindowHandle{};
    }

    SDL_SysWMinfo wm_info;
    SDL_VERSION(&wm_info.version);
    if (SDL_GetWindowWMInfo(w, &wm_info) != SDL_TRUE) {
        fprintf(stderr, "Failed to get window info: %s\n", SDL_GetError());
        return ultramodern::renderer::WindowHandle{};
    }

    // The thread id is this thread's on purpose: it is the one that created the
    // window and therefore owns its message queue, which is where the renderer
    // posts. Goemon64Recomp builds the handle the same way.
    return ultramodern::renderer::WindowHandle{ wm_info.info.win.window, GetCurrentThreadId() };
#else
    return w;
#endif
}

#if !defined(HH_WINDOW_ICON)
static void set_window_icon(SDL_Window*) {}
#else
// The icon SDL puts on the window itself -- the taskbar entry, Alt-Tab, and the
// title bar where the platform draws one.
//
// This is NOT the same thing as the icon Explorer shows for the .exe: that one
// comes from a Windows resource compiled into the binary (icons/app.rc), which
// nothing at runtime can supply. Both exist, from the same artwork.
//
// Read from assets/ rather than embedded in the binary, which is what
// Goemon64Recomp does with a file_to_c step. Every other asset this port loads
// already comes from that directory, and the build's codegen has been a recurring
// source of silently-stale output -- so a missing icon is a logged line here
// rather than a second embed step to keep in sync.
static void set_window_icon(SDL_Window* window) {
    const std::filesystem::path path = hybridheaven::get_asset_path("icon.png");

    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) {
        fprintf(stderr, "[gfx] window icon not found at %s\n", path.string().c_str());
        return;
    }
    const std::streamsize size = file.tellg();
    file.seekg(0, std::ios::beg);
    std::vector<uint8_t> bytes(static_cast<size_t>(size > 0 ? size : 0));
    if (size <= 0 || !file.read(reinterpret_cast<char*>(bytes.data()), size)) {
        fprintf(stderr, "[gfx] window icon at %s could not be read\n", path.string().c_str());
        return;
    }

    // Four channels forced, so the surface below can assume a 32-bit RGBA layout
    // whatever the file actually stores. The artwork is opaque RGB.
    int width = 0, height = 0, channels_in_file = 0;
    stbi_uc* pixels = stbi_load_from_memory(bytes.data(), static_cast<int>(bytes.size()),
                                            &width, &height, &channels_in_file, 4);
    if (pixels == nullptr) {
        fprintf(stderr, "[gfx] window icon at %s is not a decodable image: %s\n",
            path.string().c_str(), stbi_failure_reason());
        return;
    }

    SDL_Surface* surface = SDL_CreateRGBSurfaceFrom(
        pixels, width, height, 32, width * 4,
        0x000000FFu, 0x0000FF00u, 0x00FF0000u, 0xFF000000u);
    if (surface != nullptr) {
        SDL_SetWindowIcon(window, surface);
        SDL_FreeSurface(surface);
    }
    else {
        fprintf(stderr, "[gfx] window icon surface failed: %s\n", SDL_GetError());
    }

    // Safe immediately: SDL_SetWindowIcon copies the pixels into its own storage.
    stbi_image_free(pixels);
}
#endif // HH_WINDOW_ICON

static ultramodern::renderer::WindowHandle create_window(ultramodern::gfx_callbacks_t::gfx_data_t) {
    // SDL_WINDOW_VULKAN is what lets plume create a VkSurface from this window
    // (RT64 is built with RT64_SDL_WINDOW_VULKAN on non-Windows). Without it
    // SDL_Vulkan_CreateSurface fails and RT64's setup reports no graphics API.
    uint32_t flags = SDL_WINDOW_RESIZABLE;
#if defined(HH_RT64)
    flags |= SDL_WINDOW_VULKAN;
#endif

    // 640x480 was fine when the window only ever held the game. The launcher and
    // the config menus are laid out in dp against the window, and at 480 tall the
    // menus clip. 1280x720 is the compromise: Goemon64Recomp opens at 1600x960,
    // which is a lot of pixels to ask of llvmpipe.
    //
    // The size is not free, and on this machine it is not even cheap: llvmpipe
    // rasterises on the CPU, the default Auto resolution tracks the window, and
    // the audio thread is competing for the same cores -- so the window size
    // shows up directly in the audio production rate. HH_WINDOW=WxH is the A/B
    // switch for exactly that measurement; see the table in
    // docs/development-notes.md.
#if defined(HH_UI)
    int window_w = 1280, window_h = 720;
#else
    int window_w = 640, window_h = 480;
#endif
    if (const char* size = getenv("HH_WINDOW")) {
        int w = 0, h = 0;
        if (sscanf(size, "%dx%d", &w, &h) == 2 && w >= 320 && h >= 240) {
            window_w = w;
            window_h = h;
            fprintf(stderr, "[gfx] HH_WINDOW=%dx%d\n", window_w, window_h);
        } else {
            fprintf(stderr, "[gfx] HH_WINDOW=\"%s\" ignored -- want WxH, at least 320x240.\n", size);
        }
    }

    window = SDL_CreateWindow("Hybrid Heaven: Recompiled",
                              SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                              window_w, window_h,
                              flags);
    if (window == nullptr) {
        fprintf(stderr, "Failed to create window: %s\n", SDL_GetError());
        return to_window_handle(window);
    }

    set_window_icon(window);

    // Report where the window actually landed, and drag it back on-screen if it
    // did not land anywhere visible.
    //
    // This exists because under WSLg the window has repeatedly come up parked
    // off-screen, which is indistinguishable from "the port draws nothing" unless
    // you already know to go looking for it -- and this machine cannot take a
    // screenshot to settle the difference. SDL_WINDOWPOS_CENTERED is a *request*;
    // what the compositor did with it is the thing worth logging.
    //
    // Do not read a failure to reposition as a fault. Wayland deliberately does
    // not let a client place its own toplevel, so SDL_SetWindowPosition is a no-op
    // there and the log line is then simply the record of where it went.
    {
        int x = 0, y = 0, w = 0, h = 0;
        SDL_GetWindowPosition(window, &x, &y);
        SDL_GetWindowSize(window, &w, &h);

        SDL_Rect usable{};
        const int display = SDL_GetWindowDisplayIndex(window);
        const bool have_bounds =
            display >= 0 && SDL_GetDisplayUsableBounds(display, &usable) == 0;

        // Off-screen means no overlap at all with the usable desktop; a window
        // merely hanging over an edge is ordinary and left alone.
        const bool offscreen =
            have_bounds && (x + w <= usable.x || y + h <= usable.y ||
                            x >= usable.x + usable.w || y >= usable.y + usable.h);

        // Name the video driver, because it decides whether the position above is
        // a fact or a guess. Under x11 the compositor reports real geometry; under
        // wayland a client is not told where it is, so SDL answers with what it
        // last requested and "on-screen" stops being falsifiable from in here.
        const char* driver = SDL_GetCurrentVideoDriver();
        fprintf(stderr, "[gfx] window %dx%d at %d,%d on display %d via %s", w, h, x, y, display,
                driver != nullptr ? driver : "?");
        if (have_bounds) {
            fprintf(stderr, " (usable %dx%d at %d,%d)%s",
                    usable.w, usable.h, usable.x, usable.y, offscreen ? " -- OFF-SCREEN" : "");
        } else {
            fprintf(stderr, " (display bounds unavailable: %s)", SDL_GetError());
        }
        fprintf(stderr, "\n");

        if (offscreen) {
            const int to_x = usable.x + (usable.w - w) / 2;
            const int to_y = usable.y + (usable.h - h) / 2;
            SDL_SetWindowPosition(window, to_x, to_y);
            SDL_GetWindowPosition(window, &x, &y);
            fprintf(stderr, "[gfx] moved it to %d,%d (asked for %d,%d)%s\n",
                    x, y, to_x, to_y,
                    (x == to_x && y == to_y) ? "" : " -- the compositor declined");
        }
    }

    // Ask for focus regardless. Cheap, and on a session with several windows it
    // is the difference between the launcher being in front and being behind
    // whatever the run was started from.
    SDL_RaiseWindow(window);

    return to_window_handle(window);
}

// Set by the SIGTERM/SIGINT handler, read on the render thread. See
// install_termination_handler for why this port does not let SDL own these.
static std::atomic<bool> termination_requested = false;

static void update_gfx(ultramodern::gfx_callbacks_t::gfx_data_t) {
    // A signal is not a question. `timeout -s TERM` and Ctrl-C both have to end
    // the process, and with the UI in place SDL_QUIT no longer does that on its
    // own: input.cpp answers it mid-game by opening the confirm prompt, which in
    // a headless run nobody is there to answer. So termination arrives out of
    // band, and only the window's close button reaches that prompt.
    //
    // This is still the orderly shutdown, not a kill: ultramodern::quit() is what
    // tears the renderer down and joins the saving thread. Skipping it is what
    // used to produce a SIGSEGV -- rdram freed under the game's own threads --
    // that read like a crash in the game.
    if (termination_requested.load()) {
        ultramodern::quit();
        return;
    }

#if defined(HH_UI)
    // Owns the pump: it feeds RmlUi, drives the input remapper's binding scan,
    // and tracks controller connect/disconnect.
    recomp::handle_events();

    // HH_TRACE_UI=1 answers "did the launcher draw?" without anyone looking at
    // the screen -- which this machine cannot do: WSLg parks the window somewhere
    // invisible often enough that its absence proves nothing, and screenshots do
    // not work from the guest.
    //
    // The point is that "every font loaded and RmlUi logged no errors" is a
    // WEAKER claim than it sounds. A document can load, lay out to nothing, and
    // report nothing wrong. Geometry reaching the render interface cannot: it
    // means RmlUi resolved a layout, shaped text into vertices and handed them
    // over. That is the last link host code owns; RT64 and the compositor own the
    // rest.
    if (getenv("HH_TRACE_UI") != nullptr) {
        using clock = std::chrono::steady_clock;
        static clock::time_point last = clock::now();
        auto now = clock::now();
        if (now - last >= std::chrono::seconds(1)) {
            uint64_t batches = 0, vertices = 0;
            recompui::debug_take_geometry_counts(&batches, &vertices);
            const double secs = std::chrono::duration<double>(now - last).count();
            fprintf(stderr, "[ui] %s, game %s -- %.0f batches/s, %.0f vertices/s\n",
                    recompui::is_any_context_shown() ? "a context is SHOWN" : "no context shown",
                    ultramodern::is_game_started() ? "started" : "not started",
                    batches / secs, vertices / secs);
            last = now;
        }
    }
#else
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        if (event.type == SDL_QUIT) {
            ultramodern::quit();
        }
    }
#endif
}

#if defined(__linux__)
static void termination_handler(int) {
    // Async-signal-safe: one relaxed store, nothing else. update_gfx does the work.
    termination_requested.store(true);
}

static void install_termination_handler() {
    struct sigaction sa{};
    sa.sa_handler = termination_handler;
    sa.sa_flags = SA_RESTART;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGTERM, &sa, nullptr);
    sigaction(SIGINT, &sa, nullptr);
}
#else
static void install_termination_handler() {}
#endif

static void vi_callback() {}
static void gfx_init_callback() {}

static void message_box(const char* msg) {
    fprintf(stderr, "[message] %s\n", msg);
    if (window != nullptr) {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Hybrid Heaven: Recompiled", msg, window);
    }
}

#if defined(HH_UI)
// The UI's message box, with the port's stderr line kept in front of it. A run
// under `timeout` has no one to click OK, so losing the log line to a modal that
// nobody sees is how a diagnosis gets lost.
static void ui_message_box(const char* msg) {
    fprintf(stderr, "[message] %s\n", msg);
    recompui::message_box(msg);
}
#endif

static std::string get_game_thread_name(const OSThread* t) {
    return "[Game] " + std::to_string(t->id);
}

// HH_TRACE_MEM=1 reports, once a second, where the port's memory is going.
//
// This exists because the port has an intermittent leak that runs at ~490 MB/s
// once it starts, and `LD_PRELOAD` cannot find it: a tracer hooking malloc,
// calloc, realloc, memalign, aligned_alloc, posix_memalign, all four operator
// new forms, mmap and mmap64 -- every one control-tested and confirmed loaded
// into this binary -- logged nothing at all while 2.5 GB of fresh anonymous
// mappings appeared. So ask the process itself instead of the allocator.
//
// `mallinfo2` is the load-bearing number. If `hblkhd` tracks RSS, the memory
// really is coming through glibc malloc and the interposition result is the
// thing that needs explaining; if `hblkhd` stays flat while RSS climbs, the
// memory never touches malloc and the search moves to whoever is calling mmap
// without going through libc's wrapper.
//
// The queue depths are the standing suspects: all five are
// BlockingConcurrentQueues with no ceiling, so any consumer that stops draining
// turns its queue into exactly this leak. `external_messages` is first among
// them, because sp_complete/dp_complete enqueue with requeue_if_blocked = true,
// and a game wedged with a full SP/DP queue makes dequeue_external_messages
// rebuild a vector of every undeliverable message on every pass.
//
// Reported trigger: sitting at the expansion pak screen without pressing Enter.
// SIGUSR2 makes a thread print its own backtrace. The reporter below signals
// every thread once the leak is unmistakably running, which is the only way to
// see the stack of a thread that is wedged somewhere else -- there is no
// debugger on this machine.
static void dump_stack_handler(int) {
// Desktop Linux only. Bionic has no backtrace()/backtrace_symbols_fd(), so on
// Android the tid line is all that can be produced here -- which is still worth
// printing, because the reporter below uses it to tell which threads answered
// the signal at all. Symbolizing an Android stack means addr2line against the
// unstripped .so, off the device.
#if defined(__linux__)
    char hdr[96];
    int n = snprintf(hdr, sizeof hdr, "\n[stack] tid=%d\n", (int)gettid());
    ssize_t w = write(STDERR_FILENO, hdr, n); (void)w;
#   if !defined(__ANDROID__)
    void* bt[32];
    int c = backtrace(bt, 32);
    backtrace_symbols_fd(bt, c, STDERR_FILENO);
#   endif
#endif
}

// Signal every thread in the process to dump its stack.
static void dump_all_thread_stacks() {
#if defined(__linux__)
    DIR* d = opendir("/proc/self/task");
    if (d == nullptr) {
        return;
    }
    pid_t pid = getpid();
    while (struct dirent* e = readdir(d)) {
        if (e->d_name[0] == '.') {
            continue;
        }
        long tid = strtol(e->d_name, nullptr, 10);
        if (tid <= 0) {
            continue;
        }
        syscall(SYS_tgkill, pid, (pid_t)tid, SIGUSR2);
        std::this_thread::sleep_for(std::chrono::milliseconds(15));
    }
    closedir(d);
#endif
}

static void mem_report_thread() {
#if defined(__linux__)
    const auto t0 = std::chrono::steady_clock::now();
    bool dumped = false;
    while (true) {
        size_t rss_kb = 0;
        if (FILE* f = fopen("/proc/self/statm", "r")) {
            unsigned long total_pages = 0, res_pages = 0;
            if (fscanf(f, "%lu %lu", &total_pages, &res_pages) == 2) {
                rss_kb = (size_t)res_pages * ((size_t)getpagesize() / 1024);
            }
            fclose(f);
        }

        struct mallinfo2 mi = mallinfo2();
        double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();

        fprintf(stderr,
                "[mem] t=%6.1fs rss=%zuMB | malloc: arena=%zuMB hblkhd=%zuMB "
                "uordblks=%zuMB fordblks=%zuMB hblks=%zu | queues: extmsg=%zu "
                "gfx=%zu sptask=%zu timer=%zu delthread=%zu\n",
                secs, rss_kb / 1024,
                (size_t)mi.arena / (1024 * 1024), (size_t)mi.hblkhd / (1024 * 1024),
                (size_t)mi.uordblks / (1024 * 1024), (size_t)mi.fordblks / (1024 * 1024),
                (size_t)mi.hblks,
                ultramodern::debug_external_message_count(),
                ultramodern::debug_gfx_action_count(),
                ultramodern::debug_sp_task_count(),
                ultramodern::debug_timer_action_count(),
                ultramodern::debug_deleted_thread_count());

        // Once the gfx thread has clearly stopped draining -- the queue only
        // grows at the VI thread's 60 Hz, so 180 means ~3 s of no progress --
        // dump every thread's stack. Whichever one is inside the display list
        // path is the leak.
        if (!dumped && ultramodern::debug_gfx_action_count() > 180) {
            dumped = true;
            fprintf(stderr, "[mem] gfx queue stalled -- dumping all thread stacks\n");
            dump_all_thread_stacks();
        }

        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
#endif
}

// ---------------------------------------------------------------------------

#if defined(__ANDROID__)
// Android's process entry point is not this function: SDLActivity loads the .so
// and calls SDL_main, which android_glue.cpp bridges to game_main here. The body
// below is the same portable startup either way -- only the name differs.
//
// Nothing on Android reads a terminal, and libc's stdio goes to /dev/null there,
// so every diagnostic this port has -- RmlUi's logger, RT64's fprintf(), and all
// of the port's own printf()s -- would vanish silently. Pipe both streams into
// logcat instead, which is the only channel a device actually has. Without this
// an asset or UI load failure looks exactly like a hang.
static void android_redirect_stdio_to_logcat() {
    static bool started = false;
    if (started) return;
    started = true;
    setvbuf(stdout, nullptr, _IOLBF, 0);
    setvbuf(stderr, nullptr, _IONBF, 0);
    static int fds[2];
    if (pipe(fds) != 0) return;
    dup2(fds[1], STDOUT_FILENO);
    dup2(fds[1], STDERR_FILENO);
    std::thread([]{
        char buf[512];
        ssize_t n;
        std::string line;
        while ((n = read(fds[0], buf, sizeof(buf) - 1)) > 0) {
            buf[n] = '\0';
            line += buf;
            size_t pos;
            while ((pos = line.find('\n')) != std::string::npos) {
                __android_log_write(ANDROID_LOG_INFO, "HybridHeaven-stdio", line.substr(0, pos).c_str());
                line.erase(0, pos + 1);
            }
        }
    }).detach();
}

int game_main(int argc, char** argv) {
    android_redirect_stdio_to_logcat();
#else
int main(int argc, char** argv) {
#endif
    install_crash_handler();

    if (getenv("HH_TRACE_MEM") != nullptr) {
#if defined(__linux__)
        struct sigaction sa{};
        sa.sa_handler = dump_stack_handler;
        sa.sa_flags = SA_RESTART;
        sigemptyset(&sa.sa_mask);
        sigaction(SIGUSR2, &sa, nullptr);
#endif
        std::thread(mem_report_thread).detach();
    }

    // Take SIGTERM and SIGINT away from SDL. SDL's own handlers push SDL_QUIT,
    // which the UI answers with a confirm prompt rather than a shutdown -- see
    // update_gfx. Order matters: the hint has to be set before SDL_Init, and the
    // handlers installed after it, or SDL_Init would overwrite them.
    SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");

#if defined(HH_UI)
    // Gamepad hints. SDL reads these when the joystick subsystem comes up and
    // when a pad opens, so they have to be set before SDL_Init below. They are
    // Goemon64Recomp's, from the create_gfx that goes with the input layer this
    // port took src/game/input.cpp and controls.cpp from:
    //
    //   USE_BUTTON_LABELS=0     report buttons positionally, so
    //                           SDL_CONTROLLER_BUTTON_A is always the south face
    //                           button. With labels on, a Nintendo-layout pad
    //                           reports its physically-east button as A, and a
    //                           binding stored in controls.json means something
    //                           different depending on which pad wrote it.
    //   HIDAPI_PS4/PS5_RUMBLE=1 DualShock 4 and DualSense rumble only works when
    //                           SDL may drive the extended HIDAPI report; without
    //                           it recomp::set_rumble is silent on exactly the
    //                           pads people own.
    //
    // SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS is deliberately NOT set here.
    // SDL re-reads that one dynamically and the UI owns it: ui_state.cpp's
    // apply_background_input_mode drives it from the config's
    // background_input_mode, so a value set here would just be overwritten.
    SDL_SetHint(SDL_HINT_GAMECONTROLLER_USE_BUTTON_LABELS, "0");
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_PS4_RUMBLE, "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_PS5_RUMBLE, "1");

    // AUDIO plus the two gamepad subsystems. SDL emits SDL_CONTROLLERDEVICEADDED
    // -- and refreshes pad state inside SDL_PumpEvents -- only while the joystick
    // subsystem is up, so with AUDIO alone the whole gamepad half of
    // src/game/input.cpp is unreachable code: no pad ever opens, every controller
    // binding reads as unpressed, and the port is keyboard-only however many
    // controllers are plugged in. That was this port's state until now; the input
    // layer itself was already complete.
    //
    // The no-UI build asks for AUDIO only, and should: its get_input reads the
    // keyboard and nothing else (the fixed layout above), so opening pads there
    // would buy nothing and cost HIDAPI's scan thread in the build whose whole
    // job is measuring the game.
    constexpr Uint32 sdl_init_flags = SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER | SDL_INIT_JOYSTICK;
#else
    constexpr Uint32 sdl_init_flags = SDL_INIT_AUDIO;
#endif

    if (SDL_Init(sdl_init_flags) < 0) {
        fprintf(stderr, "Failed to initialize SDL: %s\n", SDL_GetError());
        return EXIT_FAILURE;
    }
    install_termination_handler();

#if defined(HH_UI)
    // Controller mappings, loaded before any pad can connect. SDL ships a
    // built-in table and auto-generates an entry for an unknown pad, so a missing
    // db does not stop a controller working -- it makes some controllers work
    // *wrongly*, which is the harder thing to diagnose. The file is the
    // SDL_GameControllerDB subset Goemon64Recomp carries (N64 and GameCube
    // adapters plus the pads its players reported), and the
    // CONTROLLERDEVICEADDED handler in src/game/input.cpp prints the pad's GUID
    // and the mapping SDL actually chose, which is what a report of "half the
    // buttons do nothing" needs to be actionable.
    //
    // get_program_path() is the .exe's directory on Windows and the empty path on
    // Linux, i.e. the working directory -- runs here start from the repository
    // root, where this file lives. The Windows build copies it beside the .exe.
    {
        std::string controller_db_path =
            (hybridheaven::get_program_path() / "recompcontrollerdb.txt").string();
        int added = SDL_GameControllerAddMappingsFromFile(controller_db_path.c_str());
        if (added < 0) {
            // Not fatal: SDL's built-in table still applies.
            fprintf(stderr, "[input] failed to load controller mappings from %s: %s\n",
                controller_db_path.c_str(), SDL_GetError());
        }
        else {
            // stderr, not stdout, and it carries the joystick count: runs here
            // end by `timeout -k`, which throws away whatever is still sitting in
            // stdout's block buffer, and this line is the answer to "is the
            // gamepad subsystem actually up" on a run nobody was watching.
            fprintf(stderr, "[input] %d controller mappings from %s, %d joystick(s) attached\n",
                added, controller_db_path.c_str(), SDL_NumJoysticks());
        }
    }
#endif

    // Where settings, the stored ROM and the SAVES live. Both builds have to agree
    // on this: play with the launcher, then bisect with -DHH_UI=OFF, and a
    // different directory would present as a lost save file rather than as two
    // directories -- a false lead this project does not need another of.
    //
    // get_app_folder_path() honours portable.txt next to the binary and
    // $APP_FOLDER_PATH, otherwise ~/.config/HybridHeavenRecompiled. It lives in
    // src/game/config.cpp, which the no-UI build does not compile (it pulls in the
    // whole config/input layer), so that build repeats the default here. Keep the
    // two in step; config.cpp is the original.
#if defined(HH_UI)
    std::filesystem::path config_path = hybridheaven::get_app_folder_path();
#else
    std::filesystem::path config_path;
    if (std::filesystem::exists("portable.txt")) {
        config_path = std::filesystem::current_path();
    } else if (const char* env = getenv("APP_FOLDER_PATH")) {
        config_path = std::filesystem::path{ env };
    } else if (const char* home = getenv("HOME")) {
        config_path = std::filesystem::path{ home } / ".config" / "HybridHeavenRecompiled";
    } else {
        config_path = std::filesystem::current_path() / "hybridheaven-recomp";
    }
#endif
    std::filesystem::create_directories(config_path);
    recomp::register_config_path(config_path);

    for (const auto& game : supported_games) {
        recomp::register_game(game);
    }

#if defined(__ANDROID__)
    // Register the ROM the Java launcher copied out of the Storage Access
    // Framework. This HAS to happen after register_game() above and cannot move
    // up into nativeInit(): select_rom() looks the title up in the game_roms map,
    // which is still empty that early, and answers OtherError. The failure is
    // quiet -- the launcher simply never offers "Start Game" -- so the ordering
    // is load-bearing rather than incidental.
    //
    // select_rom() re-validates the file and stores it under the config path,
    // which is what lets start_game() find it and the launcher show "Start Game"
    // straight away.
    {
        std::filesystem::path android_rom = hybridheaven::android_rom_path();
        if (std::filesystem::exists(android_rom)) {
            recomp::RomValidationError err = recomp::select_rom(android_rom, supported_games[0].game_id);
            __android_log_print(err == recomp::RomValidationError::Good ? ANDROID_LOG_INFO : ANDROID_LOG_ERROR,
                "HybridHeaven", "select_rom(%s) -> %d", android_rom.c_str(), static_cast<int>(err));
        } else {
            __android_log_print(ANDROID_LOG_ERROR, "HybridHeaven",
                "ROM not found at %s (the launcher should have copied it)", android_rom.c_str());
        }
    }
#endif

#if defined(HH_UI)
    // Before load_config: the graphics config it reads is what create_window and
    // the render context are built from, and the input bindings it reads are what
    // get_n64_input reports. Registering the UI's exports first also means a mod
    // can be loaded by the launcher without a second registration pass.
    recompui::register_ui_exports();
#endif

    hybridheaven::register_overlays();
    hybridheaven::register_patches();

#if defined(HH_UI)
    hybridheaven::load_config();
#endif

    // A ROM may be supplied on the command line; otherwise a previously validated
    // copy under the config path is used. With the UI, the launcher's Select ROM
    // is the other way in, and either way librecomp stores the validated copy so
    // the next launch needs neither.
    //
    // A bad ROM on the command line is still fatal rather than a launcher
    // message: someone who named a file meant that file.
    if (argc > 1) {
        recomp::RomValidationError err = recomp::select_rom(argv[1], supported_games[0].game_id);
        if (err != recomp::RomValidationError::Good) {
            fprintf(stderr, "ROM validation failed for %s (error %d).\n"
                            "This port wants an unmodified NTSC-U (USA) cartridge "
                            "dump, sha1 16dbc21620b52deab5c5abf8a309ac60adfbee85.\n",
                    argv[1], static_cast<int>(err));
            return EXIT_FAILURE;
        }
    }
    recomp::check_all_stored_roms();


    recomp::rsp::callbacks_t rsp_callbacks{
        .get_rsp_microcode = get_rsp_microcode,
    };
    ultramodern::renderer::callbacks_t renderer_callbacks{
        .create_render_context = create_render_context_recording_rdram,
    };
    ultramodern::audio_callbacks_t audio_callbacks{
        .queue_samples = queue_samples,
        .get_frames_remaining = get_frames_remaining,
        .set_frequency = set_frequency,
    };
    ultramodern::input::callbacks_t input_callbacks{
#if defined(HH_UI)
        // src/game/input.cpp and controls.cpp: remappable bindings for keyboard
        // AND controller, a real analog stick through the N64's octagonal gate,
        // and rumble. What this replaces was a hardcoded WASD keyboard with a
        // stick that only ever read full deflection or nothing.
        .poll_input = recomp::poll_inputs,
        .get_input = recomp::get_n64_input,
        .set_rumble = recomp::set_rumble,
        .get_connected_device_info = recomp::get_connected_device_info,
#else
        .poll_input = poll_input,
        .get_input = get_input,
        .set_rumble = set_rumble,
        .get_connected_device_info = get_connected_device_info,
#endif
    };
    ultramodern::gfx_callbacks_t gfx_callbacks{
        .create_gfx = create_gfx,
        .create_window = create_window,
        .update_gfx = update_gfx,
    };
    ultramodern::events::callbacks_t events_callbacks{
#if defined(HH_UI)
        // Rumble decays on the VI, and the graphics menu can only offer the
        // options the device turned out to support -- which is not known until
        // the renderer has come up, which is what gfx_init_callback reports.
        .vi_callback = recomp::update_rumble,
        .gfx_init_callback = recompui::update_supported_options,
#else
        .vi_callback = vi_callback,
        .gfx_init_callback = gfx_init_callback,
#endif
    };
    ultramodern::error_handling::callbacks_t error_handling_callbacks{
#if defined(HH_UI)
        .message_box = ui_message_box,
#else
        .message_box = message_box,
#endif
    };
    ultramodern::threads::callbacks_t threads_callbacks{
        .get_game_thread_name = get_game_thread_name,
    };

#if defined(HH_UI)
    // No start_game here: recomp::start brings up the launcher and blocks on the
    // game-start thread until something calls start_game, which is what the
    // launcher's Start Game button does. A missing or invalid ROM is not an error
    // any more either -- the launcher offers Select ROM instead of Start Game.
    //
    // HH_AUTOSTART=1 skips the launcher and boots straight into the game, for the
    // timed runs this port is measured with: 300 seconds of clean play is not a
    // measurement if 300 of those seconds were spent on a menu waiting for a
    // click. It needs an already-validated ROM, since there is nobody to pick one.
    if (getenv("HH_AUTOSTART") != nullptr) {
        if (!recomp::is_rom_valid(supported_games[0].game_id)) {
            fprintf(stderr,
                    "HH_AUTOSTART is set but no validated ROM is stored. Pass the "
                    "retail ROM once:\n  %s \"/path/to/Hybrid Heaven (USA).z64\"\n", argv[0]);
            return EXIT_FAILURE;
        }
        fprintf(stderr, "[launcher] HH_AUTOSTART set -- skipping the launcher.\n");
        recomp::start_game(supported_games[0].game_id);
    }

#if defined(__ANDROID__)
    // The same skip, reached the other way. A device has no environment to set
    // HH_AUTOSTART in; instead MainActivity was launched with the auto-start
    // extra, which is how the "restart to title screen" half of a restart gets
    // back into the game without stopping at the launcher again.
    else if (hybridheaven::android_autostart()) {
        if (recomp::is_rom_valid(supported_games[0].game_id)) {
            __android_log_print(ANDROID_LOG_INFO, "HybridHeaven",
                "autostart: skipping the launcher");
            recomp::start_game(supported_games[0].game_id);
        } else {
            // Not fatal here, unlike the HH_AUTOSTART path above: there IS a
            // launcher on screen and the user can pick a ROM with it.
            __android_log_print(ANDROID_LOG_ERROR, "HybridHeaven",
                "autostart requested but no validated ROM; falling back to the launcher");
        }
    }
#endif
#else
    // No launcher to pick a ROM with, so a valid one has to exist already.
    if (recomp::is_rom_valid(supported_games[0].game_id)) {
        recomp::start_game(supported_games[0].game_id);
    } else {
        fprintf(stderr,
                "No valid ROM. Pass the retail ROM as the first argument:\n"
                "  %s \"/path/to/Hybrid Heaven (USA).z64\"\n", argv[0]);
        return EXIT_FAILURE;
    }
#endif

    // The version the launcher shows. HH_VERSION_STRING is defined by the build
    // when there is a release to name -- gradle derives it from the git tag -- so
    // a packaged build cannot claim a version different from the one it shipped
    // as. Local builds have no tag and fall back to the development version.
    recomp::Version project_version{ 0, 1, 0 };
#if defined(HH_VERSION_STRING)
    if (!recomp::Version::from_string(HH_VERSION_STRING, project_version)) {
        fprintf(stderr, "[version] HH_VERSION_STRING \"%s\" is not major.minor.patch; "
                        "using the default.\n", HH_VERSION_STRING);
    }
#endif

    // Install the save observation hooks before recomp::start(), which reaches
    // ultramodern::init_saving() and spawns the saving thread that consumes
    // them. Unconditional, not gated on the autosave setting -- see
    // hybridheaven::init_save_rollback.
    hybridheaven::init_save_rollback();

    recomp::start(
        project_version,
        {},
        rsp_callbacks,
        renderer_callbacks,
        audio_callbacks,
        input_callbacks,
        gfx_callbacks,
        events_callbacks,
        error_handling_callbacks,
        threads_callbacks
    );

    if (audio_device != 0) {
        SDL_CloseAudioDevice(audio_device);
    }
    SDL_Quit();

    // Leave without unwinding. The threads the game created with osCreateThread
    // are never joined -- each exits on its own the next time it reaches a
    // scheduling point, and `recomp::start` returns without waiting -- so any
    // teardown after this point races them. Returning normally runs the static
    // destructors, and one run tore down ultramodern's message queues while a
    // game thread was still dequeuing from one. `_Exit` skips all of it and lets
    // the kernel reclaim everything, which is what a process on its way out
    // wants regardless. Replace this with a real join when there is one to make.
    std::_Exit(EXIT_SUCCESS);
}
