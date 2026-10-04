/**
 * Host exports callable from patch code.
 *
 * Each one is declared in patches/misc_funcs.h, given an address in
 * patches/syms.ld, and registered by src/main/register_patches.cpp. The MIPS
 * side calls it as an ordinary C function; on this side it arrives as a recomp
 * function that pulls its arguments out of the context.
 */

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "librecomp/overlays.hpp"
#include "librecomp/helpers.hpp"
#include "recomp.h"
#include "recomp_input.h"
#include "hh_config.h"
#include "hh_save_rollback.h"
#include "ultramodern/ultramodern.hpp"

// Forward-declared rather than reached through recomp_ui.h, which pulls in SDL.h
// -- and this translation unit is part of HybridHeavenRuntime, which is built
// without SDL's include directories on purpose (it is the glue below the UI).
// Both are defined in the UI sources: show_saved_indicator in
// src/ui/ui_saved_indicator.cpp, is_config_menu_open in src/ui/ui_config.cpp.
// Neither takes or returns a UI type, which is what makes declaring them here
// safe rather than merely convenient.
namespace recompui {
    void show_saved_indicator();
    bool is_config_menu_open();
}

#if !defined(HH_UI)
// --- No-UI build stubs ------------------------------------------------------
//
// The host exports below are registered unconditionally, because the generated
// `manual_patch_symbols` table names every one of them and the patch blob calls
// them by absolute address -- there is no build in which they may simply not
// exist. But their real implementations live in the UI sources (ui_config.cpp,
// input.cpp, ui_saved_indicator.cpp), which `-DHH_UI=OFF` does not compile.
//
// Before these stubs existed the no-UI configuration did not link at all, and
// had not for some time: twelve undefined references, ten of them predating the
// autosave work. That build is the one used for measuring the game below the
// display list, so leaving it broken quietly removes a measuring instrument.
//
// Every stub answers "the feature is off". That is not a placeholder, it is the
// correct answer: a build with no settings menu has no way to turn the analog
// camera or autosave ON, so a patch asking whether they are enabled must hear
// no. The two setters are no-ops for the same reason -- nothing to suppress
// when nothing is driving the camera.
namespace hybridheaven {
    AnalogCamMode get_analog_cam_mode() { return AnalogCamMode::Off; }
    CameraInvertMode get_analog_camera_invert_mode() { return CameraInvertMode::InvertNone; }
    int get_analog_cam_sensitivity_x() { return 50; }
    int get_analog_cam_sensitivity_y() { return 50; }
    AutosaveMode get_autosave_mode() { return AutosaveMode::Off; }
}

namespace recomp {
    void get_right_analog(float* x, float* y) { *x = 0.0f; *y = 0.0f; }
    bool get_camera_recenter_pressed() { return false; }
    bool get_camera_zoom_held() { return false; }
    void set_right_analog_suppressed(bool) {}
    void set_c_down_suppressed(bool) {}
}

namespace recompui {
    void show_saved_indicator() {}
    bool is_config_menu_open() { return false; }
}
#endif // !HH_UI

// How many times the loader hook has actually run.
//
// This exists to be zero. The port shipped one build in which the patches were
// linked but silently discarded -- GCC did not treat RECOMP_FUNC as weak, so the
// unpatched recompilation won every override and the loader hook never ran. The
// build succeeded, the game booted, and nothing said a word. Whether a patch
// took effect is a link-time property that no table lookup reveals, so the only
// honest test is whether the hook is observed doing its job.
//
// Atomic because the read comes off the RSP task path rather than the game
// thread that writes it.
static std::atomic<uint32_t> overlay_load_count{0};

uint32_t hh_overlay_load_count() {
    return overlay_load_count.load(std::memory_order_relaxed);
}

// Set HH_TRACE_OVERLAYS=1 to log every overlay load. During bring-up this is
// the cheapest proof that the game is actually progressing rather than sitting
// in a wait loop -- with no renderer there is otherwise nothing to look at.
static bool trace_overlays() {
    static const bool enabled = getenv("HH_TRACE_OVERLAYS") != nullptr;
    return enabled;
}

/**
 * Registers a freshly loaded overlay with librecomp, mapping the ROM range to
 * where it was loaded so its functions land in `func_map`.
 *
 * The loader hook calls this before the data lands -- see patches/required.c.
 * The section table it matches against comes from the decomp ELF, and
 * tools/verify_overlay_hook.py asserts the two still agree.
 */
/**
 * Exercises the partial-eviction path on the one case that provokes it, because
 * the game does not reach that case in any run this port can currently play
 * through.
 *
 * `.file_56` occupies 0x80358820 + 0x343A0 and holds 454 functions. `.file_54`
 * loads 0xA5F0 bytes at 0x803837E0, inside it -- which destroys 22 of those 454
 * and leaves 432 intact in rdram, `func_803757B0_8193D0` at 0x803757B0 among
 * them. Dropping the section whole made all 432 unreachable, and a later call to
 * 0x803757B0 aborted with `Failed to find function`.
 *
 * Those two numbers come from the generated section table, so having the runtime
 * report them back is a real check rather than a restatement: it is the eviction
 * code walking `FuncEntry.rom_size` itself. Run it against
 * `HH_EVICT_WHOLE_SECTION=1` to see the behaviour it replaced.
 *
 * It leaves no residue. `.file_56` is not loaded at this point in the game, so
 * dropping whatever survives restores the state exactly; `section_addresses` is
 * untouched either way, since overlay relocation is disabled for this game.
 */
static void selftest_partial_eviction() {
    constexpr uint32_t file_56_rom = 0x007FC440, file_56_ram = 0x80358820, file_56_size = 0x000343A0;
    constexpr uint32_t file_54_ram = 0x803837E0, file_54_size = 0x0000A5F0;

    load_overlays(file_56_rom, (int32_t)file_56_ram, file_56_size);

    uint32_t sections = unload_overlapping_overlays((int32_t)file_54_ram, file_54_size);
    uint32_t funcs = recomp::overlays::take_partial_eviction_func_count();
    fprintf(stderr, "[selftest] a .file_54 load at %08X+%X over a loaded .file_56: "
                    "dropped %u section%s and %u function%s "
                    "(expected 0 sections and 22 functions with the fix, "
                    "1 section and 0 functions without it)\n",
            file_54_ram, file_54_size, sections, sections == 1 ? "" : "s",
            funcs, funcs == 1 ? "" : "s");

    uint32_t left = unload_overlapping_overlays((int32_t)file_56_ram, file_56_size);
    recomp::overlays::take_partial_eviction_func_count();
    fprintf(stderr, "[selftest] cleanup dropped %u remaining section%s of .file_56\n",
            left, left == 1 ? "" : "s");
}

/**
 * Exercises the unannounced-overlay claim on the exact load that crashes,
 * because the load itself is only reachable by playing into the battle system.
 *
 * `Failed to find function at 0x803757B0` is a call into `.file_56` -- the
 * 0x343A0 battle-system overlay at ROM 0x7FC440, slot 0x80358820 -- which the
 * game brings in through `func_80004838_5438`, its asynchronous archive loader.
 * The port patches only the blocking one, so nothing ever registers it. The fix
 * is in librecomp's `do_dma`, which every PI DMA passes through whatever the
 * guest-side path.
 *
 * What this proves that a boot run cannot: that the claim path actually fires and
 * actually makes the address callable. An unattended run never reaches a battle,
 * and the fix is deliberately a no-op on everything else -- so "it did not
 * misfire" is all a boot run can report, and that is equally true of a fix that
 * does nothing at all.
 *
 * The numbers are the same ones selftest_partial_eviction uses, and they come
 * from the generated section table via `register_unannounced_overlays` itself
 * rather than being restated here, so this is the runtime's own bookkeeping
 * answering.
 *
 * It leaves no residue: `.file_56` is not loaded at this point in the game, so
 * dropping it again restores the state exactly.
 */
static void selftest_unannounced_overlay() {
    constexpr uint32_t file_56_rom = 0x007FC440, file_56_ram = 0x80358820, file_56_size = 0x000343A0;
    constexpr int32_t crash_addr = 0x803757B0;
    // The async loader's chunk size lives at D_800892B0+0x42CC and is set by its
    // caller, so the exact value is not known here. 0x2000 is only a stand-in for
    // "smaller than the file"; what matters is that no single chunk completes the
    // section, which is the property that broke the first version of this fix.
    constexpr uint32_t chunk = 0x2000;

    auto callable = [](int32_t addr) { return recomp::overlays::find_loaded_function(addr) != nullptr; };

    bool before = callable(crash_addr);

    // Replay a chunked load the way func_80004838_5438 issues it: chunk-sized
    // transfers advancing ROM and destination together, then a final short one.
    UnannouncedOverlay claim{};
    uint32_t claimed = 0;
    uint32_t chunks = 0;
    bool callable_early = false;
    for (uint32_t done = 0; done < file_56_size; done += chunk) {
        uint32_t this_chunk = std::min(chunk, file_56_size - done);
        claimed += register_unannounced_overlays(file_56_rom + done, (int32_t)(file_56_ram + done),
                                                this_chunk, &claim);
        chunks++;
        // Nothing may become callable before the run completes: registering a
        // half-fetched overlay would hand the game code it has not finished
        // reading.
        if (done + this_chunk < file_56_size && callable(crash_addr)) {
            callable_early = true;
        }
    }
    bool after = callable(crash_addr);

    fprintf(stderr, "[selftest] .file_56 replayed as %u unannounced chunks of %X: claimed %u section%s"
                    " (ram %08X); 0x%08X callable before: %s, mid-load: %s, after: %s -- %s\n",
            chunks, chunk, claimed, claimed == 1 ? "" : "s", claim.section_ram, (uint32_t)crash_addr,
            before ? "yes" : "no", callable_early ? "YES" : "no", after ? "yes" : "no",
            (!before && !callable_early && after && claimed == 1) ? "PASS" : "FAIL");

    // Offering the completed load again must be a no-op -- this is what stops the
    // patched loader's own transfer being registered twice, which would leave a
    // duplicate in loaded_sections for eviction to trip over.
    uint32_t again = register_unannounced_overlays(file_56_rom, (int32_t)file_56_ram, file_56_size, &claim);
    fprintf(stderr, "[selftest] the same load offered again claimed %u section%s -- %s\n",
            again, again == 1 ? "" : "s", again == 0 ? "PASS" : "FAIL");

    uint32_t left = unload_overlapping_overlays((int32_t)file_56_ram, file_56_size);
    recomp::overlays::take_partial_eviction_func_count();

    // A whole-file transfer in one DMA must still work: that is what the blocking
    // loader does, and an overlay arriving unannounced that way has to be claimed
    // just the same.
    uint32_t whole = register_unannounced_overlays(file_56_rom, (int32_t)file_56_ram, file_56_size, &claim);
    fprintf(stderr, "[selftest] the same load as one whole-file DMA claimed %u section%s,"
                    " 0x%08X callable: %s -- %s\n",
            whole, whole == 1 ? "" : "s", (uint32_t)crash_addr,
            callable(crash_addr) ? "yes" : "no",
            (whole == 1 && callable(crash_addr)) ? "PASS" : "FAIL");

    left += unload_overlapping_overlays((int32_t)file_56_ram, file_56_size);
    recomp::overlays::take_partial_eviction_func_count();

    // The ROM streamer is the case that has to be refused, because it genuinely
    // reads ROM that lies inside a section. This is its real read -- 0x210 bytes
    // from rom 0x5D280, which is inside .main -- aimed at its real buffer.
    claim = UnannouncedOverlay{};
    uint32_t streamer = register_unannounced_overlays(0x0005D280, (int32_t)0x80089518, 0x210, &claim);
    fprintf(stderr, "[selftest] the 0x80089518 stream buffer read of .main claimed %u,"
                    " implied base %08X against .main's 80000460 -- %s\n",
            streamer, claim.declined_ram, streamer == 0 ? "PASS" : "FAIL");

    fprintf(stderr, "[selftest] cleanup dropped %u section%s of .file_56\n",
            left, left == 1 ? "" : "s");
}

/**
 * Exercises the tail-clip rule on the exact pair of loads that abort, for the
 * same reason the test above exists: entering a battle is the only thing that
 * produces them, and an unattended run cannot reach one.
 *
 * Both overlays arrive together on battle entry. `.file_56` takes the 0x343A0
 * slot at 0x80358820; `.file_55` then loads 0x14700 bytes at 0x803757E0, which
 * is four bytes inside `func_803757B0_8193D0` (0x803757B0-0x803757E4) -- the
 * `nop` in the delay slot of its `jr $ra`, with its twelve real instructions
 * untouched. The game calls 0x803757B0 immediately afterwards.
 *
 * Four properties, and the last two are the ones that make the first two mean
 * anything: that the strict rule really does drop the function (so the fix is
 * what keeps it, not something else), and that a load reaching one word further
 * back still drops it (so the exemption is confined to the delay slot and is not
 * a blanket tolerance for overwritten code).
 */
static void selftest_tail_clip() {
    constexpr uint32_t file_56_rom = 0x007FC440, file_56_ram = 0x80358820, file_56_size = 0x000343A0;
    constexpr uint32_t file_55_ram = 0x803757E0, file_55_size = 0x00014700;
    constexpr int32_t crash_addr = 0x803757B0;

    auto callable = [](int32_t addr) { return recomp::overlays::find_loaded_function(addr) != nullptr; };
    auto claim_file_56 = [&]() {
        UnannouncedOverlay claim{};
        return register_unannounced_overlays(file_56_rom, (int32_t)file_56_ram, file_56_size, &claim) == 1
               && callable(crash_addr);
    };
    auto drop_file_56 = [&]() {
        unload_overlapping_overlays((int32_t)file_56_ram, file_56_size);
        recomp::overlays::take_partial_eviction_func_count();
        recomp::overlays::take_tail_clip_func_count();
    };

    // 1. The real sequence: .file_56 in, then .file_55 onto its tail.
    bool registered = claim_file_56();
    unload_overlapping_overlays((int32_t)file_55_ram, file_55_size);
    uint32_t dropped = recomp::overlays::take_partial_eviction_func_count();
    uint32_t kept = recomp::overlays::take_tail_clip_func_count();
    bool survives = callable(crash_addr);
    fprintf(stderr, "[selftest] .file_55 at %08X over .file_56: %08X registered: %s,"
                    " survives the clip: %s (%u kept, %u dropped) -- %s\n",
            file_55_ram, (uint32_t)crash_addr, registered ? "yes" : "no",
            survives ? "yes" : "no", kept, dropped,
            (registered && survives && kept >= 1) ? "PASS" : "FAIL");
    drop_file_56();

    // 2. The A/B. HH_DROP_TAIL_CLIPPED=1 is this, and it must lose the function --
    //    otherwise the run above proved nothing about which rule kept it.
    recomp::overlays::set_tail_clip_tolerated(false);
    claim_file_56();
    unload_overlapping_overlays((int32_t)file_55_ram, file_55_size);
    bool strict_survives = callable(crash_addr);
    uint32_t strict_dropped = recomp::overlays::take_partial_eviction_func_count();
    recomp::overlays::take_tail_clip_func_count();
    fprintf(stderr, "[selftest] under the strict rule %08X survives: %s (%u dropped) -- %s\n",
            (uint32_t)crash_addr, strict_survives ? "yes" : "no", strict_dropped,
            (!strict_survives && strict_dropped >= 1) ? "PASS" : "FAIL");
    drop_file_56();
    recomp::overlays::set_tail_clip_tolerated(true);

    // 3. One word further back covers `jr $ra` itself, which is a real
    //    instruction, so the function must still be dropped.
    claim_file_56();
    unload_overlapping_overlays((int32_t)(file_55_ram - 4), file_55_size);
    bool deep_survives = callable(crash_addr);
    uint32_t deep_dropped = recomp::overlays::take_partial_eviction_func_count();
    uint32_t deep_kept = recomp::overlays::take_tail_clip_func_count();
    fprintf(stderr, "[selftest] a load 4 bytes deeper (%08X) drops %08X: %s (%u kept, %u dropped) -- %s\n",
            file_55_ram - 4, (uint32_t)crash_addr, deep_survives ? "no" : "yes", deep_kept, deep_dropped,
            (!deep_survives && deep_dropped >= 1) ? "PASS" : "FAIL");
    drop_file_56();

    // 4. The next abort, and the reason this test grew a fourth case. The tail-clip
    //    rule got the run past 0x803757B0 and into `func_803757B0_8193D0`, which
    //    installs `func_803758FC_81951C` at 0x803758FC as a handler. That address
    //    is 0x11C inside `.file_55`'s range, so it is genuinely destroyed and
    //    genuinely dropped -- and the game calls it anyway, which on hardware only
    //    makes sense if `.file_56`'s bytes are back by then.
    //
    //    Partial eviction leaves `.file_56` LISTED as loaded while dropping those
    //    functions, so the re-load hit the claim path's "already loaded, nothing to
    //    do" and repaired nothing. This replays all three loads in order and checks
    //    that the handler is callable again at the end.
    constexpr int32_t handler_addr = 0x803758FC;
    claim_file_56();
    unload_overlapping_overlays((int32_t)file_55_ram, file_55_size);
    recomp::overlays::take_partial_eviction_func_count();
    recomp::overlays::take_tail_clip_func_count();
    bool handler_gone = !callable(handler_addr);

    UnannouncedOverlay reclaim{};
    uint32_t reclaimed = register_unannounced_overlays(file_56_rom, (int32_t)file_56_ram,
                                                      file_56_size, &reclaim);
    uint32_t repaired = recomp::overlays::take_reload_repair_func_count();
    bool handler_back = callable(handler_addr);
    fprintf(stderr, "[selftest] .file_56 re-loaded while still listed: %08X was dropped: %s,"
                    " restored: %s (%u repaired, claimed %u) -- %s\n",
            (uint32_t)handler_addr, handler_gone ? "yes" : "no", handler_back ? "yes" : "no",
            repaired, reclaimed,
            (handler_gone && handler_back && repaired >= 1 && reclaimed == 0) ? "PASS" : "FAIL");
    drop_file_56();

    // 5. The A/B. HH_NO_RELOAD_REPAIR=1 is this, and it must leave the handler
    //    missing -- otherwise case 4 proved nothing about which change restored it.
    recomp::overlays::set_reload_repair_enabled(false);
    claim_file_56();
    unload_overlapping_overlays((int32_t)file_55_ram, file_55_size);
    recomp::overlays::take_partial_eviction_func_count();
    recomp::overlays::take_tail_clip_func_count();
    register_unannounced_overlays(file_56_rom, (int32_t)file_56_ram, file_56_size, &reclaim);
    bool strict_handler_back = callable(handler_addr);
    fprintf(stderr, "[selftest] without the repair %08X comes back: %s -- %s\n",
            (uint32_t)handler_addr, strict_handler_back ? "yes" : "no",
            strict_handler_back ? "FAIL" : "PASS");
    drop_file_56();
    recomp::overlays::set_reload_repair_enabled(true);
    recomp::overlays::take_reload_repair_func_count();
}

extern "C" void recomp_load_overlays(uint8_t* rdram, recomp_context* ctx) {
    uint32_t rom = _arg<0, uint32_t>(rdram, ctx);
    PTR(void) ram = _arg<1, PTR(void)>(rdram, ctx);
    uint32_t size = _arg<2, uint32_t>(rdram, ctx);

    // Run on the first real load, which is the earliest point at which the
    // overlay tables are initialised.
    if (getenv("HH_SELFTEST_EVICT") != nullptr) {
        static bool done = false;
        if (!done) {
            done = true;
            selftest_partial_eviction();
        }
    }

    if (getenv("HH_SELFTEST_DMA_OVERLAY") != nullptr) {
        static bool done = false;
        if (!done) {
            done = true;
            selftest_unannounced_overlay();
        }
    }

    if (getenv("HH_SELFTEST_TAIL_CLIP") != nullptr) {
        static bool done = false;
        if (!done) {
            done = true;
            selftest_tail_clip();
        }
    }

    if (trace_overlays()) {
        fprintf(stderr, "[overlay] rom %08X -> ram %08X size %08X\n",
                rom, (uint32_t)ram, size);
    }

    overlay_load_count.fetch_add(1, std::memory_order_relaxed);
    load_overlays(rom, ram, size);
}

/**
 * Drops every overlay whose bytes the range [ram, ram+size) is about to
 * overwrite, so nothing stays callable at an address the game has reused.
 *
 * The game never announces an eviction -- its overlay slots are fixed and it
 * simply loads over a slot's previous tenant -- so the loader hook announces one
 * on its behalf, from the extent it is about to write. Without this
 * `load_overlay` only ever *adds* to `func_map`, and an indirect call through a
 * vram that two tenants both cover reaches whichever registered last. That was
 * observed rather than feared: a SIGSEGV with frames from
 * `rom=0x008505A0 ram=0x8038B7E0 size=0x2580` and from
 * `rom=0x0084D5A0 ram=0x8038CFC0 size=0x750`, whose RAM range sits entirely
 * inside the first's.
 *
 * librecomp's own `unload_overlays` cannot express this: it exits on a section
 * the range covers only partially, and these slots overlap partially by
 * construction -- 0x8038B7E0 + 0x2580 runs past the start of the 0x8038CFC0
 * slot, and 0x80358820 + 0x343A0 ends in the middle of the 0x8038B7E0 one.
 * Hence `unload_overlapping_overlays`; see
 * tools/n64modernruntime-evict-overlapping-overlays.patch.
 */
extern "C" void recomp_evict_overlays(uint8_t* rdram, recomp_context* ctx) {
    PTR(void) ram = _arg<0, PTR(void)>(rdram, ctx);
    uint32_t size = _arg<1, uint32_t>(rdram, ctx);

    // HH_NO_EVICT=1 turns the eviction off, which is the A/B for any failure
    // that might be caused by it rather than fixed by it. Without it the port is
    // back to resolving an address two tenants both cover to whichever loaded
    // last; with it, a call into an evicted overlay fails loudly in
    // `get_function` instead.
    static const bool disabled = getenv("HH_NO_EVICT") != nullptr;
    if (disabled) {
        return;
    }

    uint32_t num_evicted = unload_overlapping_overlays(ram, size);
    uint32_t num_funcs = recomp::overlays::take_partial_eviction_func_count();
    uint32_t num_kept = recomp::overlays::take_tail_clip_func_count();

    if (trace_overlays() && (num_evicted != 0 || num_funcs != 0 || num_kept != 0)) {
        fprintf(stderr, "[overlay] evict ram %08X size %08X -> dropped %u section%s"
                        " and %u function%s from sections left loaded, kept %u tail-clipped\n",
                (uint32_t)ram, size, num_evicted, num_evicted == 1 ? "" : "s",
                num_funcs, num_funcs == 1 ? "" : "s", num_kept);
    }
}

/* ---------------------------------------------------------------------------
 * Analog camera.
 *
 * The reader for all of these is patches/camera.c, which replaces the game's
 * own camera-angle getter. The host side is nearly all inherited: the right
 * stick, its deadzone, R3 and the right-stick suppression flag all came over
 * with Goemon64Recomp's input layer and were already wired up in
 * src/game/input.cpp -- nothing but these thin wrappers was missing.
 * ------------------------------------------------------------------------- */

// Microsecond clock. The patch accumulates rotation against wall-clock dt rather
// than per call, because the function it replaces has ~30 call sites and can run
// several times in one frame; a per-call step would then turn framerate and call
// count into camera speed.
extern "C" void recomp_time_us(uint8_t* rdram, recomp_context* ctx) {
    _return(ctx, static_cast<u32>(std::chrono::duration_cast<std::chrono::microseconds>(
        ultramodern::time_since_start()).count()));
}

extern "C" void recomp_get_analog_cam_enabled(uint8_t* rdram, recomp_context* ctx) {
    _return<s32>(ctx, hybridheaven::get_analog_cam_mode() == hybridheaven::AnalogCamMode::On);
}

extern "C" void recomp_get_analog_inverted_axes(uint8_t* rdram, recomp_context* ctx) {
    s32* x_out = _arg<0, s32*>(rdram, ctx);
    s32* y_out = _arg<1, s32*>(rdram, ctx);

    hybridheaven::CameraInvertMode mode = hybridheaven::get_analog_camera_invert_mode();

    *x_out = (mode == hybridheaven::CameraInvertMode::InvertX || mode == hybridheaven::CameraInvertMode::InvertBoth);
    *y_out = (mode == hybridheaven::CameraInvertMode::InvertY || mode == hybridheaven::CameraInvertMode::InvertBoth);
}

// Per-axis sensitivity, 0-100 (50 = the tuned default rate).
extern "C" void recomp_get_analog_cam_sensitivity(uint8_t* rdram, recomp_context* ctx) {
    s32* x_out = _arg<0, s32*>(rdram, ctx);
    s32* y_out = _arg<1, s32*>(rdram, ctx);

    *x_out = hybridheaven::get_analog_cam_sensitivity_x();
    *y_out = hybridheaven::get_analog_cam_sensitivity_y();
}

// Right stick, past the configured per-axis deadzone (get_right_analog) and then
// a small radial one so a stick that rests slightly off-centre does not creep the
// camera. Rescaled so the usable range still reaches 1.0.
extern "C" void recomp_get_camera_inputs(uint8_t* rdram, recomp_context* ctx) {
    float* x_out = _arg<0, float*>(rdram, ctx);
    float* y_out = _arg<1, float*>(rdram, ctx);

    constexpr float radial_deadzone = 0.05f;

    // HH_CAM_FORCE_X / HH_CAM_FORCE_Y pin the stick to a constant, in [-1, 1].
    // This is not a cheat, it is the only way this feature can be exercised on
    // this machine at all: WSL passes no USB pad through, so the right stick
    // does not exist here and every code path past the engage threshold is
    // otherwise unreachable. Absent, they change nothing.
    static const float* forced = [] () -> const float* {
        static float v[2];
        const char* fx = getenv("HH_CAM_FORCE_X");
        const char* fy = getenv("HH_CAM_FORCE_Y");
        if (fx == nullptr && fy == nullptr) {
            return nullptr;
        }
        v[0] = std::clamp(fx != nullptr ? std::strtof(fx, nullptr) : 0.0f, -1.0f, 1.0f);
        v[1] = std::clamp(fy != nullptr ? std::strtof(fy, nullptr) : 0.0f, -1.0f, 1.0f);
        fprintf(stderr, "[cam] right stick forced to (%.3f, %.3f)\n", v[0], v[1]);
        return v;
    }();

    if (forced != nullptr) {
        *x_out = forced[0];
        *y_out = forced[1];
        return;
    }

    float x, y;
    recomp::get_right_analog(&x, &y);

    float magnitude = std::sqrt(x * x + y * y);

    if (magnitude < radial_deadzone) {
        *x_out = 0.0f;
        *y_out = 0.0f;
    }
    else {
        float scale = ((magnitude - radial_deadzone) / (1.0f - radial_deadzone)) / magnitude;
        *x_out = x * scale;
        *y_out = y * scale;
    }
}

// R3 held state. The patch edge-detects it and hands the camera back to the game.
extern "C" void recomp_get_camera_recenter_pressed(uint8_t* rdram, recomp_context* ctx) {
    _return<s32>(ctx, recomp::get_camera_recenter_pressed() ? 1 : 0);
}

// Stops the right stick from also firing its C-button bindings while the analog
// camera owns it. The patch re-asserts this every frame it runs and clears it
// when it stops, so the C-buttons come back on their own outside gameplay.
extern "C" void recomp_set_right_analog_suppressed(uint8_t* rdram, recomp_context* ctx) {
    s32 suppressed = _arg<0, s32>(rdram, ctx);

    recomp::set_right_analog_suppressed(suppressed != 0);
}

// Reports each take-over/hand-back of the camera under HH_TRACE_CAM.
//
// It exists because this machine cannot test the feature: WSL passes no USB pad
// through, so the right stick only exists on the Windows build, played by hand.
// A line per transition is what turns "the patch is linked" -- which the symbol
// table already proves -- into "the patch engaged, at this angle", which it
// does not. One call per take-over, so it is free when nothing happens.
extern "C" void recomp_analog_cam_engaged(uint8_t* rdram, recomp_context* ctx) {
    static const bool enabled = getenv("HH_TRACE_CAM") != nullptr;
    if (!enabled) {
        return;
    }

    s32 engaged = _arg<0, s32>(rdram, ctx);
    s32 yaw = _arg<1, s32>(rdram, ctx);
    s32 pitch = _arg<2, s32>(rdram, ctx);
    s32 roll = _arg<3, s32>(rdram, ctx);

    // engaged == 2 is "still holding it", sent on every step so the log can show
    // the camera actually turning -- the difference between "the patch ran" and
    // "the stick moved the camera". Rate-limited to a line a second here rather
    // than in the patch, since this is the side that knows the trace is on.
    if (engaged == 2) {
        static auto last = std::chrono::steady_clock::now() - std::chrono::seconds(10);
        auto now = std::chrono::steady_clock::now();
        if (now - last < std::chrono::seconds(1)) {
            return;
        }
        last = now;
        // Roll is the one to watch. It is the game's own back-solve of how far
        // the up vector is tilted about the view axis, and in normal play it is
        // zero. A non-zero value that GROWS is the signature of the camera
        // rolling the horizon over -- see the patch of func_8011A0F0_4F9800.
        fprintf(stderr, "[cam] analog camera holding, yaw %d (%.1f deg) pitch %d (%.1f deg) roll %d (%.1f deg)%s\n",
                yaw, yaw * 360.0 / 8192.0, pitch, pitch * 360.0 / 8192.0,
                roll, roll * 360.0 / 8192.0,
                (roll > 40 || roll < -40) ? "  <-- horizon is tilting" : "");
        return;
    }

    // Angles are the game's 13-bit binary form; degrees are for the reader.
    fprintf(stderr, "[cam] analog camera %s at yaw %d (%.1f deg) pitch %d (%.1f deg)\n",
            engaged ? "engaged" : "released",
            yaw, yaw * 360.0 / 8192.0, pitch, pitch * 360.0 / 8192.0);
}

// Round-trip check for the analog camera's own arithmetic, reported once per
// take-over under HH_TRACE_CAM.
//
// The patch decomposes the eye offset into (azimuth, elevation, radius) with the
// game's atan2 and rebuilds it with the game's sin/cos. A convention error
// anywhere in that -- a swapped atan2 argument, sin where cos belongs, the wrong
// axis carrying the elevation -- would not fail to compile and would not crash;
// it would simply throw the camera somewhere else the instant it engaged. So the
// patch rebuilds the eye from the values it just captured, with zero rotation
// applied, and reports how far the result landed from where the eye actually
// was. Anything but ~0 means the decompose/recompose pair disagree.
extern "C" void recomp_analog_cam_selftest(uint8_t* rdram, recomp_context* ctx) {
    static const bool enabled = getenv("HH_TRACE_CAM") != nullptr;
    if (!enabled) {
        return;
    }

    s32 dev_milli = _arg<0, s32>(rdram, ctx);
    s32 radius_milli = _arg<1, s32>(rdram, ctx);

    double dev = dev_milli / 1000.0;
    double radius = radius_milli / 1000.0;
    fprintf(stderr, "[cam] round trip: rebuilt eye is %.3f from where it was, at radius %.2f (%.3f%%)%s\n",
            dev, radius, radius > 0.0 ? 100.0 * dev / radius : 0.0,
            (radius > 0.0 && dev > 0.02 * radius) ? "  <-- WRONG, the angle convention is off" : "");
}

// A/B for the roll fix in patches/camera.c's func_8011A0F0_4F9800.
//
// HH_CAM_NO_ROLL_FIX=1 lets the analog rotation run inside the game's roll
// back-solve, which is what the first version of this feature did. The camera
// then tilts the horizon a little on every frame in which it turns WHILE
// PITCHED, compounding until the world is on its side. Kept because a fix with
// no way back is a fix nobody can confirm caused the change.
extern "C" void recomp_get_analog_cam_roll_fix(uint8_t* rdram, recomp_context* ctx) {
    static const bool disabled = getenv("HH_CAM_NO_ROLL_FIX") != nullptr;
    _return<s32>(ctx, disabled ? 0 : 1);
}

// A/B for the framing-distance tracker in patches/camera.c.
//
// HH_CAM_LEGACY_NORMAL=1 restores the asymmetric rise/fall pair the tracker used
// to run: 2.00/s opening out against 0.15/s closing in. That 13:1 ratio was meant
// to stop a transient pull-in redefining the room's framing, and instead made the
// tracker a peak detector -- under the 17/65 goal flicker measured in a doorway
// its equilibrium is 61.6, within 5% of the high value and half again the mean,
// with a fourteen-second recovery from a single spike. Kept because a fix nobody
// can switch off is a fix nobody can attribute.
extern "C" void recomp_get_analog_cam_legacy_normal(uint8_t* rdram, recomp_context* ctx) {
    static const bool legacy = getenv("HH_CAM_LEGACY_NORMAL") != nullptr;
    _return<s32>(ctx, legacy ? 1 : 0);
}

// A/B for the movement-basis fix in patches/camera.c.
//
// HH_CAM_NO_BASIS_FIX=1 stops the patch rebuilding gState+0x232, restoring the
// worst bug this feature has had: movement inverted in both axes, in some rooms
// and not others, persisting until R3 handed the camera back -- while the picture
// stayed perfectly correct throughout.
//
// The game derives the movement basis from the camera in func_8011A878_4F9F88,
// but only four player-state functions ever CALL it. In any other state the basis
// keeps its last value. That is safe in the original game and only there: its
// camera can only be turned from inside a C-button camera state, so you can never
// move and look at once, and a basis going stale while the camera turns is
// unreachable. The analog camera turns the camera in every state.
//
// Measured before the fix: basis frozen at 1971 (86.6 deg) while the camera swept
// a full circle, drift running -4038 to +3992. At 177 degrees that reads as a
// clean inversion in both axes, which is exactly how it was reported.
extern "C" void recomp_get_analog_cam_no_basis_fix(uint8_t* rdram, recomp_context* ctx) {
    static const bool disabled = getenv("HH_CAM_NO_BASIS_FIX") != nullptr;
    _return<s32>(ctx, disabled ? 1 : 0);
}

// HH_CAM_LEGACY_TRUST=1 restores the floor pan-out: any goal passing the loose
// 150-unit staleness threshold may widen the room's framing.
//
// Measured in two independent runs, both with the goal steady rather than
// transient -- so the tracker was never the cause, it was faithfully adopting
// what the game asked for:
//
//   basisfix.log  goal aims 42  from the player, radius 80  -> normal 16 -> 80
//   gapfix.log    goal aims 100 from the player, radius 248 -> normal 226 -> 246
//
// In the second the patch's own anchored look-at sat at 15 from the player while
// the goal's sat at 100, so the goal was not framing the player and its radius
// was never a framing distance. See ACAM_GOAL_FRAME_DIST in patches/camera.c for
// why the fix is a second, tighter threshold in the outward direction only rather
// than a smaller value for the existing one.
extern "C" void recomp_get_analog_cam_legacy_trust(uint8_t* rdram, recomp_context* ctx) {
    static const bool legacy = getenv("HH_CAM_LEGACY_TRUST") != nullptr;
    _return<s32>(ctx, legacy ? 1 : 0);
}

// HH_CAM_FORCE_DIST=<units> pins the room's framing distance, for the same reason
// HH_CAM_FORCE_X/Y/ZOOM exist: it makes an otherwise unreachable state reachable
// on a machine with no pad.
//
// The specific state is the DYNAMIC pitch cap binding. That cap is the steepest
// elevation whose horizontal reach still clears the movement-basis freeze latch,
// so it only bites when the camera is framed close -- below about 36 units, where
// atan2(sqrt(r*r-196), 14) finally drops under the fixed 0x600 bound. Every room
// reachable in an unattended run frames at 246, and zoom floors at 0.40x, so the
// closest this machine can get unaided is 98 and the cap is dead code in practice.
// Pinning the distance is the only way to test it here.
//
// 0 or unset leaves the tracker alone. Bring-up only; nothing ships reading this.
extern "C" void recomp_get_analog_cam_force_dist(uint8_t* rdram, recomp_context* ctx) {
    static const s32 forced = []() {
        const char* v = getenv("HH_CAM_FORCE_DIST");
        return v != nullptr ? atoi(v) : 0;
    }();
    _return<s32>(ctx, forced);
}

// The game's movement basis and its freeze latch, under HH_TRACE_CAM.
//
// `func_8011A878_4F9F88` rebuilds the player's movement basis from the camera
// every frame -- except that it arms a latch (gState->0x234 = 30) whenever the
// camera's HORIZONTAL distance from its look-at drops under 10, and while that
// latch is set the basis is frozen for as long as the stick is held. The game
// can only reach that state by shoving its own camera in close; an analog camera
// reaches it by looking down, because pitch shrinks the horizontal distance by
// cos(pitch) while leaving the real distance alone. A frozen basis is felt as
// the character pulling in a direction that is not the one being asked for.
extern "C" void recomp_analog_cam_basis(uint8_t* rdram, recomp_context* ctx) {
    static const bool enabled = getenv("HH_TRACE_CAM") != nullptr;
    if (!enabled) {
        return;
    }

    s32 horiz = _arg<0, s32>(rdram, ctx);
    s32 basis = _arg<1, s32>(rdram, ctx);
    s32 packed_latch = _arg<2, s32>(rdram, ctx);
    s32 packed_follow = _arg<3, s32>(rdram, ctx);

    s32 latch = (packed_latch >> 16) & 0xFFFF;
    s32 expected = packed_latch & 0xFFFF;
    s32 at_to_player = (packed_follow >> 16) & 0xFFFF;
    s32 goal_radius = packed_follow & 0xFFFF;

    // How far the movement basis has drifted from the direction the camera is
    // actually pointing. Zero is correct. A steady non-zero value while walking
    // is the ordering hazard: some camera modes read the basis after the
    // look-at snaps to the player but before the analog rotation is re-applied,
    // so the basis comes from a camera pose that is about to be overwritten,
    // skewed by however fast the player is moving. That is felt as the
    // character pulling off the direction being asked for.
    s32 drift = ((basis - expected + 4096) & 0x1FFF) - 4096;
    bool bad = drift > 100 || drift < -100;

    static auto last = std::chrono::steady_clock::now() - std::chrono::seconds(10);
    auto now = std::chrono::steady_clock::now();
    // Report once a second normally, but never drop a drift report -- the point
    // is to catch the moment it happens, which is while the player is moving.
    if (!bad && now - last < std::chrono::seconds(1)) {
        return;
    }
    if (bad && now - last < std::chrono::milliseconds(250)) {
        return;
    }
    last = now;

    // at_to_player is the one that answers "why did the camera stop following
    // me". The look-at is what the camera orbits, and the patch places the eye
    // around it; if the look-at has been left far behind the player then no
    // amount of getting the orbit right will make the camera follow.
    fprintf(stderr, "[cam] basis %d (%.1f deg), drift %d (%.1f deg), horiz %d, "
                    "latch %d, look-at is %d from the player, goal radius %d%s%s%s\n",
            basis, basis * 360.0 / 8192.0,
            drift, drift * 360.0 / 8192.0, horiz, latch,
            at_to_player, goal_radius,
            latch != 0 ? "  <-- basis FROZEN" : "",
            bad ? "  <-- BASIS != CAMERA" : "",
            at_to_player > 60 ? "  <-- LOOK-AT HAS LOST THE PLAYER" : "");
}

// Hides N64 C-down from the game while the analog camera is engaged.
//
// RB is bound to C-down and is also the camera's zoom modifier, so without this
// every zoom press would also fire C-down -- putting the player into the game's
// own close-look camera state, which fights the camera being driven. Armed only
// while engaged, so a player who leaves the setting on and never touches the
// right stick loses nothing.
extern "C" void recomp_set_c_down_suppressed(uint8_t* rdram, recomp_context* ctx) {
    s32 suppressed = _arg<0, s32>(rdram, ctx);

    recomp::set_c_down_suppressed(suppressed != 0);
}

// Physical RB held state -- the analog camera's zoom modifier. Read straight from
// the pad rather than through the N64 C-down binding it shares, because C-down is
// masked out of the game's button word while the camera is engaged; going through
// the binding would read the mask, not the button.
extern "C" void recomp_get_camera_zoom_held(uint8_t* rdram, recomp_context* ctx) {
    // HH_CAM_FORCE_ZOOM=1 holds the modifier down, for the same reason
    // HH_CAM_FORCE_X/Y exist: no pad reaches this machine, so the zoom path is
    // otherwise unreachable and untestable here.
    static const bool forced = getenv("HH_CAM_FORCE_ZOOM") != nullptr;
    if (forced) {
        _return<s32>(ctx, 1);
        return;
    }
    _return<s32>(ctx, recomp::get_camera_zoom_held() ? 1 : 0);
}

// Framing distance in use and the player's zoom multiplier, under HH_TRACE_CAM.
extern "C" void recomp_analog_cam_zoom(uint8_t* rdram, recomp_context* ctx) {
    static const bool enabled = getenv("HH_TRACE_CAM") != nullptr;
    if (!enabled) {
        return;
    }
    static auto last = std::chrono::steady_clock::now() - std::chrono::seconds(10);
    auto now = std::chrono::steady_clock::now();
    if (now - last < std::chrono::seconds(1)) {
        return;
    }
    last = now;

    s32 packed_dist = _arg<0, s32>(rdram, ctx);
    s32 packed = _arg<1, s32>(rdram, ctx);

    s32 radius = packed_dist & 0xFFFF;
    s32 normal = (packed_dist >> 16) & 0xFFFF;
    s32 zoom_pct = packed & 0xFFF;
    // 14 bits. It was 15, which ran into `owned` at bit 26 and reported every
    // owned sample's gap 16384 too high -- see the packing side in camera.c.
    s32 gap = (packed >> 12) & 0x3FFF;
    bool stale = (packed & (1 << 27)) != 0;
    bool occluded = (packed & (1 << 28)) != 0;
    bool anchored = (packed & (1 << 29)) != 0;
    bool drifting = (packed & (1 << 30)) != 0;
    bool owned = (packed & (1 << 26)) != 0;

    // `normal` is the room's own framing distance and `radius` is what is in use.
    // They differ for two legitimate reasons and it matters which: the player's
    // zoom, or the game capping the distance because something is in the way.
    // `gap` is how far the game's goal is aimed from the player -- the number the
    // staleness threshold is set from.
    fprintf(stderr, "[cam] distance %d (room normal %d, zoom %d%%)%s%s, anchor %s, goal aims %d from the player\n",
            radius, normal, zoom_pct,
            occluded ? ", CAPPED (something in the way)" : "",
            stale ? ", goal STALE" : "",
            !anchored ? "OFF"
                      : (drifting ? (owned ? "pulling onto the player (a task owns the camera)"
                                           : "pulling onto the player")
                                  : (owned ? "on the player (a task owns the camera)"
                                           : "on the player")),
            gap);
}


// Where the room wants the camera pointed, against where the patch is holding it.
//
// Printed on a change rather than on a clock, because the event this is for is a
// doorway: a second of samples either side of it is what distinguishes "the door
// is solid and the camera needs collision" from "the room reframed and our
// absolute azimuth stranded the camera".
extern "C" void recomp_analog_cam_goal(uint8_t* rdram, recomp_context* ctx) {
    static const bool enabled = getenv("HH_TRACE_CAM") != nullptr;
    if (!enabled) {
        return;
    }

    s32 goal_yaw = _arg<0, s32>(rdram, ctx);
    s32 held_yaw = _arg<1, s32>(rdram, ctx);

    s32 diff = ((goal_yaw - held_yaw + 4096) & 0x1FFF) - 4096;
    s32 mag = diff < 0 ? -diff : diff;

    // Report when the divergence moves by more than ~5 degrees, plus a slow
    // heartbeat so a steady state still shows up.
    static s32 last_reported = 1 << 20;
    static auto last = std::chrono::steady_clock::now() - std::chrono::seconds(10);
    auto now = std::chrono::steady_clock::now();
    s32 moved = diff - last_reported;
    if (moved < 0) {
        moved = -moved;
    }
    if (moved < 120 && now - last < std::chrono::seconds(2)) {
        return;
    }
    last_reported = diff;
    last = now;

    fprintf(stderr, "[cam] room wants %d (%.1f deg), holding %d (%.1f deg), "
                    "apart by %d (%.1f deg)%s\n",
            goal_yaw, goal_yaw * 360.0 / 8192.0,
            held_yaw, held_yaw * 360.0 / 8192.0,
            diff, diff * 360.0 / 8192.0,
            mag > 1024 ? "  <-- ROOM AND CAMERA DISAGREE STRONGLY" : "");
}

// The camera goal's PROVENANCE, under HH_TRACE_CAM.
//
// Everything the analog camera decides about DISTANCE rests on two inferences,
// and this is what replaces them with measurements.
//
// The first is staleness. Not every camera mode writes the goal fields, so the
// patch has to judge whether the goal it is reading is live, and it does that by
// measuring how far the goal's LOOK-AT sits from the player. That validates one
// half of a pair: the distance comes from the goal's EYE, and nothing checked
// that. A mode refreshing the look-at over a leftover eye from an earlier wide
// shot passes the test with a huge radius. So both halves are timed separately
// here, and `eye` far older than `at` is exactly that failure, visible at last.
//
// The second is what the room "normally" frames at, which the patch tracks by
// easing toward the goal distance. Which statistic that ease should converge on
// depends on how the goal is actually distributed -- a mean is right for
// symmetric noise, a median for two-sided outliers, neither for a step. So the
// spread is accumulated between reports rather than sampled: min, max and mean of
// the goal radius, plus how much of the window was classified as occluded.
//
// The three bytes are the game's own record, undocumented until now and never
// read: `func_8011AAF4_4FA204` stores arg4 at gState+0x2AE on every goal write,
// clears gState+0x2B4 when arg5 is 1, and stores arg5 at gState+0x2B5 -- then
// branches on that last one being 2. Their meanings are unknown. They are printed
// so they can be correlated against what the camera is visibly doing, and nothing
// reads them for a decision until that correlation exists.
extern "C" void recomp_analog_cam_provenance(uint8_t* rdram, recomp_context* ctx) {
    static const bool enabled = getenv("HH_TRACE_CAM") != nullptr;
    if (!enabled) {
        return;
    }

    s32 packed = _arg<0, s32>(rdram, ctx);
    s32 eye_age_ms = _arg<1, s32>(rdram, ctx);
    s32 at_age_ms = _arg<2, s32>(rdram, ctx);
    s32 goal_radius = _arg<3, s32>(rdram, ctx);

    s32 type = packed & 0xFF;
    s32 mode = (packed >> 8) & 0xFF;
    s32 latch = (packed >> 16) & 0xFF;
    bool occluded = (packed & (1 << 24)) != 0;
    bool stale = (packed & (1 << 25)) != 0;

    // Accumulated across the reporting window, because the distribution is the
    // point -- a single sample cannot distinguish a steady 41 from a 17/65 dither
    // with the same mean, and those two want different trackers.
    static s32 lo = 0x7FFFFFFF;
    static s32 hi = -1;
    static double sum = 0.0;
    static s32 count = 0;
    static s32 occluded_samples = 0;

    if (goal_radius < lo) {
        lo = goal_radius;
    }
    if (goal_radius > hi) {
        hi = goal_radius;
    }
    sum += goal_radius;
    count++;
    if (occluded) {
        occluded_samples++;
    }

    // Report on a heartbeat, but never miss a change in the three bytes -- a
    // change of write type is the event this exists to catch, and it is what a
    // mode handoff or a scripted shot taking the camera should look like.
    static s32 last_type = -1;
    static s32 last_mode = -1;
    static s32 last_latch = -1;
    bool changed = (type != last_type) || (mode != last_mode) || (latch != last_latch);

    static auto last = std::chrono::steady_clock::now() - std::chrono::seconds(10);
    auto now = std::chrono::steady_clock::now();
    if (!changed && now - last < std::chrono::seconds(1)) {
        return;
    }
    last = now;
    last_type = type;
    last_mode = mode;
    last_latch = latch;

    double mean = count > 0 ? sum / count : 0.0;
    s32 occl_pct = count > 0 ? (100 * occluded_samples / count) : 0;

    // A goal eye much older than the goal look-at is the failure the staleness
    // test cannot see: the half that decides the distance is a leftover while the
    // half that is checked looks fresh.
    bool split = (eye_age_ms != 0xFFFF && at_age_ms != 0xFFFF) &&
                 (eye_age_ms > 500) && (eye_age_ms > at_age_ms * 4);

    fprintf(stderr, "[cam] goal write: type %d, mode %d, latch %d; "
                    "eye last changed %dms ago, look-at %dms ago; "
                    "radius now %d (window min %d, max %d, mean %.1f over %d, occluded %d%%)%s%s%s\n",
            type, mode, latch,
            eye_age_ms, at_age_ms,
            goal_radius, hi < 0 ? 0 : lo, hi < 0 ? 0 : hi, mean, count, occl_pct,
            stale ? ", goal judged STALE" : "",
            changed ? "  <-- write type changed" : "",
            split ? "  <-- GOAL EYE IS A LEFTOVER, look-at is not" : "");

    lo = 0x7FFFFFFF;
    hi = -1;
    sum = 0.0;
    count = 0;
    occluded_samples = 0;
}

// A/B for anchoring the camera's look-at to the player.
//
// HH_CAM_NO_ANCHOR=1 leaves the look-at as the game set it, which is the
// behaviour where a room's fixed camera angle makes the orbit circle a point that
// is not the player -- felt as the camera stopping tracking you.
extern "C" void recomp_get_analog_cam_no_anchor(uint8_t* rdram, recomp_context* ctx) {
    static const bool disabled = getenv("HH_CAM_NO_ANCHOR") != nullptr;
    _return<s32>(ctx, disabled ? 1 : 0);
}

// A/B for the mode gate.
//
// HH_CAM_NO_MODE_GATE=1 drives the camera in every one of the game's camera
// modes, restoring the behaviour the analog camera shipped with: overriding the
// direction during cutscenes, on elevators and through battles, none of which the
// game itself permits a player to do.
extern "C" void recomp_get_analog_cam_no_mode_gate(uint8_t* rdram, recomp_context* ctx) {
    static const bool disabled = getenv("HH_CAM_NO_MODE_GATE") != nullptr;
    _return<s32>(ctx, disabled ? 1 : 0);
}

// A/B for the gun-aim release.
//
// HH_CAM_NO_AIM_RELEASE=1 keeps the analog camera engaged while R is held,
// restoring the behaviour where the right stick overrides the framing the game
// chose for its own gun shot.
extern "C" void recomp_get_analog_cam_no_aim_release(uint8_t* rdram, recomp_context* ctx) {
    static const bool disabled = getenv("HH_CAM_NO_AIM_RELEASE") != nullptr;
    _return<s32>(ctx, disabled ? 1 : 0);
}

// A/B for the framing-distance fix.
//
// HH_CAM_LEGACY_RADIUS_EASE=1 restores the asymmetric 6.0-in/1.5-out ease, which
// makes the radius a valley detector on a flickering goal and measured a median
// framing distance of 38.6 against the game's own 70.4 in the same camera mode.
extern "C" void recomp_get_analog_cam_legacy_radius_ease(uint8_t* rdram, recomp_context* ctx) {
    static const bool legacy = getenv("HH_CAM_LEGACY_RADIUS_EASE") != nullptr;
    _return<s32>(ctx, legacy ? 1 : 0);
}

// A/B for the framing gain.
//
// HH_CAM_NO_FRAMING_GAIN=1 follows the camera goal's radius raw. Measured inside
// camera mode 2: the goal sits at median 37.9 and the game's own camera at 45.5,
// because the game eases its eye toward a goal that keeps moving and so trails
// behind it. Tracking the goal exactly therefore frames closer than the game does.
extern "C" void recomp_get_analog_cam_no_framing_gain(uint8_t* rdram, recomp_context* ctx) {
    static const bool disabled = getenv("HH_CAM_NO_FRAMING_GAIN") != nullptr;
    _return<s32>(ctx, disabled ? 1 : 0);
}

// --- The game's own camera mode machine -----------------------------------
//
// HH_TRACE_CAMMODE=1. Everything above traces the analog camera; this traces the
// GAME, and it exists because the two have never been compared.
//
// The analog camera currently overrides the camera's direction absolutely and
// re-imposes it every frame, unconditionally, in every situation. The game has a
// much narrower idea of when a player may touch the camera, and it is explicit
// rather than inferred:
//
//   `D_80163740_542E50` is a camera MODE selector holding -1..8. Once per frame
//   `func_8010DD4C_4ED45C` copies it to gState+0x29A and dispatches on it through
//   jtbl_80185CD8_5653E8, installing that mode's handler into the camera task's
//   exec slot. Ten states. The game's own debug strings for them are still in the
//   ROM next to that jump table -- "process(cam_fix_main)" and
//   "process(cam_free_main)", plus a whole `< CAMERA DEBUG INFO >` readout.
//
//   The C-button camera is gated on that mirror. `func_8011B254_4FA964` is three
//   instructions and returns it; `func_8011B260_4FA970` returns a packed scene id
//   (a*10000 + b*100 + c from gState+0x254..0x256), zero only for scene 2-8-1.
//   The entry code in .file_9 then requires, for C-LEFT (the orbit camera), that
//   BOTH the scene gate is zero AND the mirror is exactly 2; for C-DOWN, either.
//
// So "you cannot move the camera during a cutscene or on an elevator" is not a
// quirk to be worked around -- it is a mode the game is in, readable every frame.
// This trace records it against what the camera is actually doing, so the analog
// camera can be rebuilt to honour the same states instead of fighting them.
//
// Two caveats it deliberately leaves visible rather than papering over. The gate
// above was read out of ONE of the two .file_9 sites that enter a camera state,
// so a different player state may gate differently -- which is why the player
// task's own exec pointer is logged beside the camera's. And the trace runs from
// inside the camera getter, so a mode that never calls it would show up as a gap
// rather than as a mode; the call counter is printed so a gap is distinguishable
// from a quiet mode.
namespace {

// Fixed vram in .file_7. Sound to read at an absolute address for the same
// reason patches/syms.ld gives for gState: the overlay's slot is fixed, its
// relocation is disabled, and the caller lives inside .file_7, so residency is a
// precondition of the call.
constexpr gpr kGState  = (gpr)(s32)0x801BBBF0;  // gState
constexpr gpr kCamMode = (gpr)(s32)0x80163740;  // the mode selector

// gState offsets, all established in docs/hybridheaven-camera-re.md except the
// three the mode machine adds (0x254..0x256 scene, 0x29A mode mirror).
constexpr s32 kGsButtons   = 0x9E;   // held buttons, copy of the controller record
constexpr s32 kGsPlayer    = 0xE0;   // player task
constexpr s32 kGsBasisYaw  = 0x232;
constexpr s32 kGsBasisFroz = 0x234;
constexpr s32 kGsCamTask   = 0x24C;  // the camera STATE MACHINE's task
constexpr s32 kGsScene     = 0x254;  // +0,+1,+2 -- the scene triple
constexpr s32 kGsMirror    = 0x29A;  // the mode mirror the C-gate reads
constexpr s32 kGsGoalType  = 0x2AE;
constexpr s32 kGsCamOwner  = 0x2B0;
constexpr s32 kGsGoalLatch = 0x2B4;
constexpr s32 kGsGoalMode  = 0x2B5;
constexpr s32 kTaskExec    = 0x1C;   // a task's per-frame function pointer

// N64 C-button bits, from the controller record.
constexpr u32 kCRight = 0x0001;
constexpr u32 kCLeft  = 0x0002;
constexpr u32 kCDown  = 0x0004;
constexpr u32 kCUp    = 0x0008;

// The ten mode handlers, read off jtbl_80185CD8_5653E8's dispatch stubs. Mode -1
// installs its handler directly; 0..8 go through a descriptor whose +0xC holds
// it. Printed beside the ACTUAL installed pointer so the two cross-check -- that
// the descriptor's +0xC is what reaches the task is an inference, and a mismatch
// in the log refutes it rather than hiding it.
struct CamModeInfo {
    s32 mode;
    u32 handler;
    const char* note;
};

constexpr CamModeInfo kCamModes[] = {
    { -1, 0x8010E668, "no camera state installed" },
    {  0, 0x8010E680, "" },
    {  1, 0x8010E890, "" },
    {  2, 0x8010F6E0, "C-BUTTONS ACCEPTED HERE" },
    {  3, 0x80113148, "" },
    {  4, 0x801147C0, "" },
    {  5, 0x80115390, "" },
    {  6, 0x80115620, "" },
    {  7, 0x80115B50, "" },
    {  8, 0x80115F10, "" },
};

const CamModeInfo* cam_mode_info(s32 mode) {
    for (const CamModeInfo& m : kCamModes) {
        if (m.mode == mode) {
            return &m;
        }
    }
    return nullptr;
}

} // namespace

// Reads a guest float. RDRAM is stored word-swapped, so a word load needs no
// index fix-up while bytes and halfwords do -- hence MEM_W here and MEM_BU/MEM_H
// elsewhere in this function.
static float cam_f32(uint8_t* rdram, gpr base, s32 off) {
    int32_t word = MEM_W(off, base);
    float out;
    std::memcpy(&out, &word, sizeof out);
    return out;
}

extern "C" void recomp_cam_trace(uint8_t* rdram, recomp_context* ctx) {
    static const bool enabled = getenv("HH_TRACE_CAMMODE") != nullptr;
    if (!enabled) {
        return;
    }

    PTR(void) params_ptr = _arg<0, PTR(void)>(rdram, ctx);
    const s32 acam_packed = _arg<1, s32>(rdram, ctx);
    const s32 acam_engaged = acam_packed & 0xFF;
    const float acam_gain = (float)((acam_packed >> 8) & 0xFFFF) / 100.0f;

    // Called from the camera getter, which runs ~30 times a frame. Counted so a
    // silent stretch in the log can be told apart from a mode that simply had
    // nothing to report -- if a cutscene stops calling the getter, the counter
    // stops with it and that is a finding, not a gap in the instrument.
    static u64 calls = 0;
    calls++;

    const s32 mode      = MEM_B(0, kCamMode);          // the selector, signed
    const s32 switch_a  = MEM_B(4, kCamMode);          // D_80163744, switch type
    const s32 switch_b  = MEM_B(8, kCamMode);          // D_80163748
    const s32 mirror    = MEM_B(kGsMirror, kGState);   // what the C-gate reads

    const u32 scene_a = MEM_BU(kGsScene + 0, kGState);
    const u32 scene_b = MEM_BU(kGsScene + 1, kGState);
    const u32 scene_c = MEM_BU(kGsScene + 2, kGState);

    // func_8011B260_4FA970, replicated exactly: zero for scene 2-8-1, otherwise
    // the triple packed as decimal. Zero is the permissive answer.
    const u32 scene_id = (scene_a == 2 && scene_b == 8 && scene_c == 1)
                             ? 0u
                             : (scene_a * 10000u + scene_b * 100u + scene_c);

    // The two gates, as .file_9 combines them. C-left is the orbit camera and
    // needs both; C-down is the close look and needs either.
    const bool c_left_ok = (scene_id == 0) && (mirror == 2);
    const bool c_down_ok = (scene_id == 0) || (mirror == 2);

    const u32 cam_task    = (u32)MEM_W(kGsCamTask, kGState);
    const u32 cam_handler = cam_task ? (u32)MEM_W(kTaskExec, (gpr)(s32)cam_task) : 0;
    const u32 cam_owner   = (u32)MEM_W(kGsCamOwner, kGState);

    // The player's state function. The camera mode is only half the story: the
    // C-button entry code lives in a player state, so a state that never runs it
    // ignores C regardless of the mode. These two together separate those cases.
    const u32 player_task = (u32)MEM_W(kGsPlayer, kGState);
    const u32 player_exec = player_task ? (u32)MEM_W(kTaskExec, (gpr)(s32)player_task) : 0;
    const u32 player_data = player_task ? (u32)MEM_W(0x2C, (gpr)(s32)player_task) : 0;

    float px = 0.0f, py = 0.0f, pz = 0.0f;
    if (player_data) {
        const gpr pd = (gpr)(s32)player_data;
        px = cam_f32(rdram, pd, 0x4);
        py = cam_f32(rdram, pd, 0x8);
        pz = cam_f32(rdram, pd, 0xC);
    }

    const u32 buttons = MEM_HU(kGsButtons, kGState);

    const s32 goal_type  = MEM_BU(kGsGoalType, kGState);
    const s32 goal_latch = MEM_BU(kGsGoalLatch, kGState);
    const s32 goal_mode  = MEM_BU(kGsGoalMode, kGState);

    const s32 basis_yaw  = MEM_H(kGsBasisYaw, kGState);
    const s32 basis_froz = MEM_BU(kGsBasisFroz, kGState);

    float ex = 0.0f, ey = 0.0f, ez = 0.0f;
    float ax = 0.0f, ay = 0.0f, az = 0.0f;
    float fovy = 0.0f;
    if (params_ptr) {
        const gpr p = (gpr)(s32)params_ptr;
        fovy = cam_f32(rdram, p, 0x1C);
        ex = cam_f32(rdram, p, 0x30);
        ey = cam_f32(rdram, p, 0x34);
        ez = cam_f32(rdram, p, 0x38);
        ax = cam_f32(rdram, p, 0x3C);
        ay = cam_f32(rdram, p, 0x40);
        az = cam_f32(rdram, p, 0x44);
    }

    const float rx = ex - ax, ry = ey - ay, rz = ez - az;
    const float radius = std::sqrt(rx * rx + ry * ry + rz * rz);

    // The GOAL's radius -- where the game wants the eye, against where it is.
    // This is the quantity the patch's distance tracker actually follows, and
    // until now only the live radius was logged, so "we frame at half the game's
    // distance" could not be attributed to the tracker or to the goal it reads.
    // Note the 12-byte stride: these are not contiguous vectors.
    const float gex = cam_f32(rdram, kGState, 0x2C0);
    const float gey = cam_f32(rdram, kGState, 0x2CC);
    const float gez = cam_f32(rdram, kGState, 0x2D8);
    const float gax = cam_f32(rdram, kGState, 0x2E8);
    const float gay = cam_f32(rdram, kGState, 0x2F4);
    const float gaz = cam_f32(rdram, kGState, 0x300);
    const float goal_radius =
        std::sqrt((gex - gax) * (gex - gax) + (gey - gay) * (gey - gay) +
                  (gez - gaz) * (gez - gaz));
    // How far the goal's look-at sits from the player: the patch's staleness test.
    const float goal_gap =
        std::sqrt((gax - px) * (gax - px) + (gay - py) * (gay - py) +
                  (gaz - pz) * (gaz - pz));

    using clock = std::chrono::steady_clock;
    static const auto start = clock::now();
    const auto now = clock::now();
    const double t = std::chrono::duration<double>(now - start).count();

    // A C-press that the game refuses is the single most direct evidence for
    // "I could not move the camera here", so it is reported on its own edge
    // rather than waiting for the heartbeat. Edge-detected on the button word
    // because this runs many times per frame.
    static u32 last_buttons = 0;
    const u32 pressed = buttons & ~last_buttons;
    last_buttons = buttons;

    if (pressed & (kCLeft | kCDown | kCRight | kCUp)) {
        const char* name = (pressed & kCLeft) ? "C-LEFT"
                         : (pressed & kCDown) ? "C-DOWN"
                         : (pressed & kCUp)   ? "C-UP"
                                              : "C-RIGHT";
        const bool ok = (pressed & kCLeft) ? c_left_ok
                      : (pressed & kCDown) ? c_down_ok
                                           : false;
        fprintf(stderr,
                "[cammode] %8.2fs  %-7s pressed in mode %d -> %s"
                "  (mirror %d, scene id %u)\n",
                t, name, mode, ok ? "ACCEPTED" : "REJECTED by the game",
                mirror, scene_id);
    }

    // Report every transition, and otherwise once a second. The transition is
    // the event this exists for; the heartbeat is what makes a long steady mode
    // legible against the user's narration of what they were doing.
    static s32 last_mode = -99;
    static s32 last_mirror = -99;
    static u32 last_handler = 0xFFFFFFFFu;
    static u32 last_player_exec = 0xFFFFFFFFu;
    static u32 last_scene = 0xFFFFFFFFu;
    static s32 last_goal_mode = -99;
    static double last_change_t = 0.0;

    const bool changed = mode != last_mode || mirror != last_mirror ||
                         cam_handler != last_handler ||
                         player_exec != last_player_exec ||
                         scene_id != last_scene || goal_mode != last_goal_mode;

    static auto last_beat = clock::now() - std::chrono::seconds(10);
    if (!changed && now - last_beat < std::chrono::seconds(1)) {
        return;
    }
    last_beat = now;

    if (changed) {
        const CamModeInfo* from = cam_mode_info(last_mode);
        const CamModeInfo* to = cam_mode_info(mode);
        (void)from;

        fprintf(stderr,
                "[cammode] %8.2fs  ---- MODE %d -> %d %s%s%s  (held %.2fs) ----\n",
                t, last_mode == -99 ? mode : last_mode, mode,
                to && to->note[0] ? "[" : "",
                to ? to->note : "",
                to && to->note[0] ? "]" : "",
                t - last_change_t);
        last_change_t = t;
    }

    const CamModeInfo* info = cam_mode_info(mode);
    const bool handler_matches = info && info->handler == cam_handler;

    fprintf(stderr,
            "[cammode] %8.2fs  %s mode %d  handler %08X%s  player_state %08X  "
            "owner %08X\n"
            "                    scene %u-%u-%u (gate id %u)  mirror %d  "
            "switch %d/%d  goal type/latch/mode %d/%d/%d\n"
            "                    C-left %s, C-down %s   analog cam %s\n"
            "                    eye (%.1f, %.1f, %.1f)  at (%.1f, %.1f, %.1f)  "
            "radius %.1f  fovy %.1f\n"
            "                    goal radius %.1f  goal gap %.1f  R %s  gain %.2f\n"
            "                    player (%.1f, %.1f, %.1f)  basis yaw %d%s  "
            "calls %llu\n",
            t, changed ? "NOW" : "   ", mode, cam_handler,
            handler_matches ? "" : " <-- NOT the mode's expected handler",
            player_exec, cam_owner,
            scene_a, scene_b, scene_c, scene_id, mirror,
            switch_a, switch_b,
            goal_type, goal_latch, goal_mode,
            c_left_ok ? "OK" : "blocked",
            c_down_ok ? "OK" : "blocked",
            acam_engaged ? "ENGAGED" : "off",
            ex, ey, ez, ax, ay, az, radius, fovy,
            goal_radius, goal_gap, (buttons & 0x0010) ? "HELD (gun)" : "-", acam_gain,
            px, py, pz, basis_yaw,
            basis_froz ? " (FROZEN)" : "",
            (unsigned long long)calls);

    last_mode = mode;
    last_mirror = mirror;
    last_handler = cam_handler;
    last_player_exec = player_exec;
    last_scene = scene_id;
    last_goal_mode = goal_mode;
}

// --- Controller Pak -------------------------------------------------------

// HH_PAK_LEGACY_PROBE=1 restores the game's original three-probe slot check.
//
// See patches/pak.c. The game asks three mutually exclusive questions about a
// controller slot -- is it a Controller Pak, is it the device that answers at
// bank 0xFE/0x80, is it a Rumble Pak -- and keeps the last "yes". The port
// answers PFS_OK to all three, so the last one wins and the save layer is told a
// Rumble Pak is inserted. With this set, that behaviour comes back and the save
// UI puts its message up again, which is how the fix was attributed.
extern "C" void recomp_get_pak_legacy_probe(uint8_t* rdram, recomp_context* ctx) {
    static const bool legacy = getenv("HH_PAK_LEGACY_PROBE") != nullptr;
    _return<s32>(ctx, legacy ? 1 : 0);
}

// What the slot probe decided, under HH_TRACE_PFS.
//
// This is the one number that says whether saving can proceed at all: `.file_7`
// dispatches on it through a 16-entry table and only code 0 reaches the
// file-level osPfs calls. Everything else puts up a message.
//
// Logged at most once per (channel, code) pair. The title screen re-probes every
// 16 frames, so an unconditional line would bury the rest of the trace.
extern "C" void recomp_pak_probe_result(uint8_t* rdram, recomp_context* ctx) {
    static const bool trace = getenv("HH_TRACE_PFS") != nullptr;
    if (!trace) {
        return;
    }

    s32 channel = _arg<0, s32>(rdram, ctx);
    s32 pfs_err = _arg<1, s32>(rdram, ctx);
    s32 code = _arg<2, s32>(rdram, ctx);

    static s32 last_code[4] = { -1, -1, -1, -1 };
    if (channel < 0 || channel > 3) {
        return;
    }
    if (last_code[channel] == code) {
        return;
    }
    last_code[channel] = code;

    const char* meaning;
    switch (code) {
        case 0x0: meaning = "pak present and healthy -- saving can proceed"; break;
        case 0x1: meaning = "no pak (message 3)"; break;
        case 0x2: meaning = "pak changed; the caller re-probes"; break;
        case 0x3: meaning = "controller comms failure (message 5)"; break;
        case 0x4: meaning = "wrong device type (message 9)"; break;
        case 0x5: meaning = "dead pak id (message 9)"; break;
        case 0x7: meaning = "the bank-0xFE device probe said yes (message 8)"; break;
        case 0xF: meaning = "a Rumble Pak is inserted (message 7) <-- the old bug"; break;
        default:  meaning = "unmapped"; break;
    }

    if (pfs_err < 0) {
        fprintf(stderr, "[pfs] slot probe ch %d -> code 0x%X: %s"
                        "  (legacy path, osPfsInitPak not consulted)\n",
                channel, code, meaning);
    } else {
        fprintf(stderr, "[pfs] slot probe ch %d: osPfsInitPak -> %d, code 0x%X: %s\n",
                channel, pfs_err, code, meaning);
    }
}

// --- Autosave ---------------------------------------------------------------

extern "C" void recomp_get_autosave_enabled(uint8_t* rdram, recomp_context* ctx) {
    _return(ctx, static_cast<s32>(hybridheaven::get_autosave_mode() ==
                                  hybridheaven::AutosaveMode::On));
}

// True while the port's own settings menu (or its sub-menu) is open. The timed
// autosave consults this so a 2-minute boundary cannot commit while the player
// is in settings -- the game keeps ticking with input zeroed underneath the
// overlay, so such a save is coherent but against the gate's intent.
extern "C" void recomp_is_config_menu_open(uint8_t* rdram, recomp_context* ctx) {
    _return(ctx, static_cast<s32>(recompui::is_config_menu_open()));
}

// Bracket around the autosave's own pak traffic, so the save-rollback observer
// can tell it apart from a deliberate, player-initiated save. See
// src/game/save_rollback.cpp.
extern "C" void recomp_set_autosave_in_progress(uint8_t* rdram, recomp_context* ctx) {
    hybridheaven::set_autosave_in_progress(_arg<0, s32>(rdram, ctx) != 0);
}

// Raises the on-screen "Saved" toast. Called from the guest thread, so it only
// stores an atomic -- the UI work happens on the render thread. See
// src/ui/ui_saved_indicator.cpp.
extern "C" void recomp_notify_saved(uint8_t* rdram, recomp_context* ctx) {
    (void)rdram;
    (void)ctx;
    recompui::show_saved_indicator();
}

// Autosave diagnostics. The patch sends an event id and three integers; the
// naming lives here because patch code has no printf and because a refusal
// printed as bare numbers is a refusal nobody reads.
//
// Not behind an HH_TRACE_* switch, unlike every other trace in this file. These
// fire at most once per save or per combo press, and the one way this feature
// fails is by refusing silently -- which is indistinguishable from a gate
// working correctly unless the refusal says which guard rejected it.
extern "C" void recomp_autosave_log(uint8_t* rdram, recomp_context* ctx) {
    s32 event = _arg<0, s32>(rdram, ctx);
    s32 a = _arg<1, s32>(rdram, ctx);
    s32 b = _arg<2, s32>(rdram, ctx);
    s32 c = _arg<3, s32>(rdram, ctx);

    switch (event) {
        case 0:   // AUTOSAVE_EV_SAVED_MANUAL
        case 1: { // AUTOSAVE_EV_SAVED_TIMED
            const char* kind = (event == 0) ? "manual" : "timed";
            const char* meaning;
            switch (a) {
                case 0:  meaning = "committed"; break;
                case -2: meaning = "refused: no save device selected "
                                   "(the save menu has not chosen one yet)"; break;
                case -3: meaning = "refused: slot cursor out of range"; break;
                default: meaning = "save layer error"; break;
            }
            fprintf(stderr, "[autosave] %s save -> status %d: %s (device %d, slot %d)\n",
                    kind, a, meaning, b, c);
            break;
        }
        case 2:   // AUTOSAVE_EV_UNSAFE
            fprintf(stderr, "[autosave] refused: unsafe state "
                            "(cam mode %d want 2 | scene %d-%d-%d | loading %d)\n",
                    a, (b >> 16) & 0xFF, (b >> 8) & 0xFF, b & 0xFF, c);
            break;
        case 3:   // AUTOSAVE_EV_UNSETTLED
            fprintf(stderr, "[autosave] refused: save data not settled "
                            "(%d/%d ms | last change:%s%s%s%s%s%s%s)\n",
                    b, c,
                    (a & 0x01) ? " gs-head"  : "",
                    (a & 0x02) ? " gs-mid"   : "",
                    (a & 0x04) ? " gs-far"   : "",
                    (a & 0x08) ? " loadblk"  : "",
                    (a & 0x10) ? " block200" : "",
                    (a & 0x20) ? " bulk"     : "",
                    (a & 0x40) ? " records"  : "");
            break;
        case 4: { // AUTOSAVE_EV_SUPPRESSED
            // An interval elapsed and produced no save. Reported because the
            // alternative -- silence -- reads identically to a timer that fired
            // once and never re-armed. Rate-limited patch-side by reason, so a
            // repeated line means the reason actually changed, not that the
            // timer is spinning.
            const char* why;
            switch (a) {
                case 1:  why = "unsafe state"; break;
                case 2:  why = "settings menu open"; break;
                case 3:  why = "save data not settled"; break;
                case 4:  why = "nothing changed since the last save"; break;
                default: why = "unmapped"; break;
            }
            fprintf(stderr, "[autosave] interval elapsed, suppressed: %s "
                            "(cam mode %d | last change mask 0x%02X | settled for %u ms)\n",
                    why, b, c & 0xFF, (unsigned)((uint32_t)c >> 8));
            break;
        }
        default:
            fprintf(stderr, "[autosave] unknown event %d (%d, %d, %d)\n", event, a, b, c);
            break;
    }
}
