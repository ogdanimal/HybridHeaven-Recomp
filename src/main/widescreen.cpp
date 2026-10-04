/**
 * Widescreen: widening the game's own 4:3 safe rect.
 *
 * Setting the aspect ratio to Expand is the whole of widescreen in
 * Goemon64Recomp, because RT64 works out for itself which projection is the 3D
 * view and widens it. For Hybrid Heaven it produced a 16:9 render target with a
 * 4:3 picture inside it -- black bars, with the setting on and RT64 behaving
 * exactly as designed. One thing causes that, and this file is the whole fix:
 *
 *   The game scissors its own 3D view to the middle 90% of the framebuffer --
 *   (16,8)-(304,232) of 320x240, and (32,16)-(608,464) of 640x480 in the
 *   high-resolution mode its own Resolution setting selects. RT64's
 *   `G_EX_ASPECT_AUTO` heuristic (ProjectionProcessor::processScene) only widens
 *   a projection that reaches BOTH edges of the framebuffer, because one that
 *   does not is usually a sub-panel -- a minimap, a split-screen viewport. This
 *   one never reaches them, so the projection stayed 4:3; and even had it been
 *   widened, the RDP would still have clipped the extra geometry away at the
 *   same inset.
 *
 * Widening the game's own scissor fixes both at once, and measurably so: with
 * this file's edit in place and RT64 completely stock, the heuristic's own
 * comparison goes from `inter=[128..2432] fb=[0..2560] -> adjust=0` to
 * `inter=[0..2560] fb=[0..2560] -> adjust=1`. An earlier version of this fix
 * added a tolerance to that heuristic inside RT64; it was reverted once this
 * file made it unnecessary, which is the better outcome -- the fork carries no
 * widescreen patch at all.
 *
 * Quest 64 has the identical defect and Quest64-Recomp fixes it the same way, in
 * `patches/widescreen.c`, which calls the inset "the authentic overscan border"
 * and is worth reading before changing anything here. Two lessons taken from it:
 *
 *   - The per-frame COLOUR CLEAR can matter as much as the scissor. Quest64-Recomp
 *     records it as "the dominant defect": only the region the game CLEARS
 *     composes as fresh content, so widening the scissor alone leaves a stale
 *     ring rather than a picture. **It is not a defect in this game**, which is
 *     worth stating because it was expected to be: `HH_TRACE_WIDESCREEN=1` shows
 *     Hybrid Heaven clearing (0,0)-(639,479) and (0,0)-(319,239) -- the whole
 *     frame -- immediately after a full-frame scissor, and only then narrowing
 *     the scissor to the safe rect for the 3D. The fill-rectangle path below is
 *     kept anyway, since it costs one command byte in the same scan.
 *   - The commands cannot be poked at a fixed address. The ROM-derived copies
 *     read as zeros at runtime and the dynamic display lists move between
 *     scenes, so both ports scan rdram for the vanilla encoding and cache the
 *     hits. Confirmed here: the live sites are in the 0x80069xxx/0x80079xxx
 *     dynamic lists, not at the ROM offsets the encodings were found at.
 *
 * WHAT THIS PORT DOES DIFFERENTLY, and why: Quest64-Recomp does the rewriting
 * from a game patch, hooked into a function that runs every frame. This port has
 * no per-frame patch hook, but it does have `send_dl`, where it already inspects
 * every display list before RT64 sees it -- so the same edit happens here, on the
 * host, one call earlier in the same path. That also means the aspect setting can
 * be read directly instead of through a `recomp_get_target_aspect_ratio` export,
 * which is the one piece of Goemon's and Quest64's widescreen scaffolding this
 * port does not need.
 *
 * BOTH axes are widened, as Quest64-Recomp does. The first version of this file
 * widened x only, reasoning that expanding to 16:9 adds field of view
 * horizontally and none vertically, so moving the top and bottom would change
 * the framing the game intends. That was wrong, and a screenshot settled it in
 * one look: the game does not draw anything in those strips, it just leaves them
 * black, so the 3.3% vertical inset showed as two bars across a picture that was
 * otherwise correct. The projection already covers the full framebuffer height,
 * so what the vertical bounds were hiding is correctly rasterised scene.
 *
 * The exception is the cinematic letterbox -- see `safe_rects` below.
 *
 * AN OFFSET IS NOT WHERE THE RECT IS. The first version of this file cached the
 * rdram offset of every safe rect it found and rewrote those offsets each frame,
 * rescanning only every 256th frame once anything was cached. That is wrong in a
 * way that only shows during screen transitions, and it was reported as one: the
 * middle 4:3 of the picture transitions while the strips around it sit there
 * black until the new screen arrives.
 *
 * The safe rect is emitted after however many commands the current screen puts in
 * front of it, so its offset within the display-list buffer moves whenever the
 * screen's content does -- measured at dl+0xA0, dl+0xB8 and dl+0x140 across a
 * single boot. A transition changes that content by definition. The cached
 * offsets stop matching, the live list goes out with the game's own 4:3 scissor,
 * and the game's full-frame colour clear -- the one Quest64-Recomp warns about
 * and this game does correctly -- is exactly what makes the strips black rather
 * than stale. Nothing recovers it until the next full scan, up to 256 frames
 * later, which is most of a transition.
 *
 * So the rewriting is driven by the display list the game is submitting THIS
 * frame: `send_dl` passes `data_ptr` in, and a bounded window from it is scanned
 * every frame and widened in place. The full 8 MB scan stays as the fallback for
 * anything outside that window, on its old slow cadence, and the cache stays for
 * the revert path. `HH_WIDESCREEN_LEGACY_SCAN=1` restores the cache-only
 * behaviour, both halves of it.
 *
 * That bug also retired the obvious way to measure this. "Did the rewrite widen
 * anything this frame" reads as healthy while the picture is wrong, because the
 * game rotates two display-list buffers and a cached site in one goes on being
 * rewritten every frame while the rect in the other has moved. The measure that
 * works asks the only question that decides the picture: after the rewrite pass,
 * does the list RT64 is about to walk still hold a safe rect with the game's own
 * bytes in it? Both are in the trace; only the second one was ever right.
 *
 * `HH_NO_WIDESCREEN_SCISSOR=1` disables the rewriting, and `HH_TRACE_WIDESCREEN=1`
 * reports every safe-rect command found in rdram, every frame that reaches RT64
 * un-widened, and a periodic rate for them.
 */

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "ultramodern/renderer_context.hpp"

#include "hh_widescreen.h"

namespace {

// The rdram the game can address: 8 MB, which this game requires (it refuses to
// boot without the expansion pak). Everything above it is the recompiler's own
// heap and holds no display lists, so scanning it would only cost time.
constexpr uint32_t rdram_bytes = 0x00800000u;

// rdram holds 32-bit words in host order, so a word is a plain read -- the same
// rule as `guest_w` in rt64_render_context.cpp. Only sub-word accesses carry the
// address xor, and nothing here does one.
inline uint32_t read_word(const uint8_t* rdram, uint32_t off) {
    return *reinterpret_cast<const uint32_t*>(rdram + off);
}

inline void write_word(uint8_t* rdram, uint32_t off, uint32_t value) {
    *reinterpret_cast<uint32_t*>(rdram + off) = value;
}

// RDP command bytes.
constexpr uint32_t cmd_setscissor = 0xEDu;
constexpr uint32_t cmd_fillrect   = 0xF6u;

// Both commands pack their rectangle as four 12-bit 10.2 fixed-point fields
// across the two words, but in opposite orders:
//
//   gsDPSetScissor(mode, ulx, uly, lrx, lry)
//       w0 = ED << 24 | ulx << 12 | uly          w1 = mode << 28 | lrx << 12 | lry
//   gsDPFillRectangle(ulx, uly, lrx, lry)
//       w0 = F6 << 24 | lrx << 12 | lry          w1 =             ulx << 12 | uly
//
// so which word holds the left edge depends on the command. `q` throughout means
// a quarter-pixel: the raw field value, four times the pixel coordinate.
inline uint32_t field_hi(uint32_t word) { return (word >> 12) & 0xFFFu; }
inline uint32_t field_lo(uint32_t word) { return word & 0xFFFu; }

inline uint32_t with_field_hi(uint32_t word, uint32_t value_q) {
    return (word & ~(0xFFFu << 12)) | ((value_q & 0xFFFu) << 12);
}

inline uint32_t with_field_lo(uint32_t word, uint32_t value_q) {
    return (word & ~0xFFFu) | (value_q & 0xFFFu);
}

// One of the game's safe rects, and what it should become in widescreen.
//
// Found by scanning the ROM for scissor commands with plausible screen
// rectangles: four survive, and the two full-height ones are exactly the two
// insets RT64 reports at runtime -- 16..304 of 320 in the normal video mode and
// 32..608 of 640 in the high-resolution one the game's own Resolution setting
// selects. The third is the same 576-wide inset with a cinematic vertical crop.
//
// Matching is on all four bounds, so one entry covers a rect wherever it appears
// but a rect with different y bounds is a DIFFERENT entry -- which is what keeps
// the cinematic letterbox distinguishable from the overscan inset. The
// `min_`/`max_` pairs give the bottom-right corner a tolerance because
// Quest64-Recomp found the low sub-pixel bits of the colour clear varying frame
// to frame.
struct SafeRect {
    const char* name;
    uint32_t    ulx_q;        // vanilla left edge, quarter-pixels
    uint32_t    min_lrx_q;    // vanilla right edge, inclusive range
    uint32_t    max_lrx_q;
    uint32_t    full_lrx_q;   // the whole framebuffer width, quarter-pixels
    uint32_t    uly_q;        // vanilla top edge
    uint32_t    min_lry_q;    // vanilla bottom edge, inclusive range
    uint32_t    max_lry_q;
    uint32_t    full_lry_q;   // 0 means LEAVE THE VERTICAL BOUNDS ALONE
};

// The game's overscan inset is 3.3% on every side, and both axes of it are dead
// space -- the projection already covers the full framebuffer height, so the
// strips above and below the scissor hold correctly rasterised scene that is
// then clipped away and left black. Opening the vertical bounds shows it.
//
// The third entry is why the y bounds are matched at all rather than just
// blanked: (32,90)-(608,390) is the game's own CINEMATIC letterbox, a 2.13:1 crop
// it applies deliberately. Its bars are content, not overscan, so it is widened
// horizontally and its vertical crop is kept -- which is what `full_lry_q = 0`
// means. Matching on x alone would have flattened a cutscene's framing.
constexpr SafeRect safe_rects[] = {
    // (16,8)-(304,232) of 320x240.
    { "320",           16u * 4u, 303u * 4u, 304u * 4u, 320u * 4u,
                        8u * 4u, 231u * 4u, 232u * 4u, 240u * 4u },
    // (32,16)-(608,464) of 640x480, the high-resolution video mode.
    { "640",           32u * 4u, 607u * 4u, 608u * 4u, 640u * 4u,
                       16u * 4u, 463u * 4u, 464u * 4u, 480u * 4u },
    // (32,90)-(608,390): the cinematic letterbox. Horizontal only.
    { "640 letterbox", 32u * 4u, 607u * 4u, 608u * 4u, 640u * 4u,
                       90u * 4u, 389u * 4u, 390u * 4u, 0u },
};

struct Site {
    uint32_t off;          // byte offset of w0 within rdram
    uint32_t vanilla_w0;   // exactly what the game wrote, so a revert is exact
    uint32_t vanilla_w1;
    uint32_t wide_w0;
    uint32_t wide_w1;
    const char* name;
    uint32_t cmd;
};

std::vector<Site> sites;

// Every safe rect found at an offset not already cached, for the whole run. The
// exposure control for the trace summary: it rises with how often the display
// list's layout moves, which is what the live-list pass exists to keep up with.
uint64_t sites_registered = 0;

// A cap, so a false positive in the scan cannot grow without bound. Two command
// types across two video modes and a double-buffered display list needs a
// handful; 64 is far above that and still small enough to walk every frame.
constexpr size_t max_sites = 64;

bool disabled() {
    static const bool off = getenv("HH_NO_WIDESCREEN_SCISSOR") != nullptr;
    return off;
}

bool tracing() {
    static const bool on = getenv("HH_TRACE_WIDESCREEN") != nullptr;
    return on;
}

// Restores the behaviour this file had before the live-list pass: rects are
// widened only at offsets the periodic full scan has already cached, and only on
// the call after it cached them. Both halves of the A/B, so the two changes
// cannot be credited to each other.
bool legacy_live_scan() {
    static const bool on = getenv("HH_WIDESCREEN_LEGACY_SCAN") != nullptr;
    return on;
}

// Decode a candidate command pair. Returns the matching safe rect, or null.
// `left_word`/`right_word` are resolved by command, per the packing note above.
const SafeRect* match(uint32_t cmd, uint32_t w0, uint32_t w1, uint32_t* top_left_word) {
    uint32_t ulx_q, uly_q, lrx_q, lry_q;
    if (cmd == cmd_setscissor) {
        // Reject a non-zero scissor mode: the game's screen scissors all use 0,
        // and matching interlace modes would rewrite something else.
        if ((w1 >> 28) != 0) {
            return nullptr;
        }
        ulx_q = field_hi(w0); uly_q = field_lo(w0);
        lrx_q = field_hi(w1); lry_q = field_lo(w1);
        *top_left_word = 0;
    }
    else {
        ulx_q = field_hi(w1); uly_q = field_lo(w1);
        lrx_q = field_hi(w0); lry_q = field_lo(w0);
        *top_left_word = 1;
    }

    for (const SafeRect& rect : safe_rects) {
        if (ulx_q == rect.ulx_q && lrx_q >= rect.min_lrx_q && lrx_q <= rect.max_lrx_q &&
            uly_q == rect.uly_q && lry_q >= rect.min_lry_q && lry_q <= rect.max_lry_q) {
            return &rect;
        }
    }
    return nullptr;
}

// Whether the aspect setting currently wants the wide rect. File scope because
// `add_site` applies its edit the moment it finds one, and is reached from the
// scans rather than from the entry point that reads the setting.
bool want_wide_now = false;

void add_site(uint8_t* rdram, uint32_t off, uint32_t cmd, uint32_t w0, uint32_t w1,
              const SafeRect& rect, uint32_t top_left_word) {
    for (const Site& existing : sites) {
        if (existing.off == off) {
            return;
        }
    }

    sites_registered++;

    Site site{};
    site.off = off;
    site.vanilla_w0 = w0;
    site.vanilla_w1 = w1;
    site.name = rect.name;
    site.cmd = cmd;

    // Widen: top-left corner to the framebuffer's origin, bottom-right to its
    // full extent. `full_lry_q == 0` keeps the vanilla vertical crop, which is
    // what the cinematic letterbox wants.
    uint32_t top_left = (top_left_word == 0) ? w0 : w1;
    uint32_t bottom_right = (top_left_word == 0) ? w1 : w0;

    top_left = with_field_hi(top_left, 0);
    bottom_right = with_field_hi(bottom_right, rect.full_lrx_q);
    if (rect.full_lry_q != 0) {
        top_left = with_field_lo(top_left, 0);
        bottom_right = with_field_lo(bottom_right, rect.full_lry_q);
    }

    if (top_left_word == 0) {
        site.wide_w0 = top_left;
        site.wide_w1 = bottom_right;
    }
    else {
        site.wide_w1 = top_left;
        site.wide_w0 = bottom_right;
    }

    // APPLY IT HERE, not on the next call. The rect is in the list RT64 is about
    // to walk, so a site registered now and widened later is a frame the player
    // sees at 4:3 -- and the offsets move often enough (see the live-list pass in
    // `widen_display_lists`) that "later" was most of a transition.
    if (want_wide_now && !legacy_live_scan()) {
        write_word(rdram, off, site.wide_w0);
        write_word(rdram, off + 4, site.wide_w1);
    }

    // Registering is what the cache is for: reverting to the game's own bytes when
    // the setting goes back to Original, and carrying a site the live-list pass
    // cannot see. The edit above does not depend on it, so hitting the cap costs
    // the revert and not the picture.
    //
    // Oldest out when full, rather than refusing the new one. The list's layout
    // moves as screens change, so entries left by buffers the game has stopped
    // rebuilding accumulate and never fall out on their own -- they still hold the
    // wide bytes this file wrote, which is one of the two encodings the walk
    // accepts. Refusing new sites would hand a full cache of exactly those to the
    // revert path, which needs the LIVE ones.
    if (sites.size() >= max_sites) {
        sites.erase(sites.begin());
    }
    sites.push_back(site);

    // First sighting of each site. Once unconditionally -- that line is what says
    // the widescreen edit found something to edit at all, and a silent run is
    // otherwise indistinguishable between "the game stopped emitting these" and
    // "the scan is looking for the wrong encoding". Every one after it is behind
    // the trace switch, because the live-list pass re-finds these whenever the
    // list's layout shifts and that is normal rather than notable.
    static bool announced = false;
    if (!announced || tracing()) {
        announced = true;
        fprintf(stderr, "[widescreen] %s safe rect at 0x%08X (%s %08X %08X -> %08X %08X;"
                        " full %u x %u q)\n",
            site.name, 0x80000000u + off,
            cmd == cmd_setscissor ? "scissor" : "fillrect",
            site.vanilla_w0, site.vanilla_w1, site.wide_w0, site.wide_w1,
            rect.full_lrx_q, rect.full_lry_q);
    }
}

// Scan a byte range for safe-rect commands, widening each one as it is found.
// Display-list commands are 64-bit and 8-byte aligned, so the step is 8.
void scan_range(uint8_t* rdram, uint32_t begin, uint32_t end) {
    for (uint32_t off = begin; off + 8 <= end; off += 8) {
        const uint32_t w0 = read_word(rdram, off);
        const uint32_t cmd = w0 >> 24;
        if (cmd != cmd_setscissor && cmd != cmd_fillrect) {
            continue;
        }
        const uint32_t w1 = read_word(rdram, off + 4);
        uint32_t top_left_word = 0;
        const SafeRect* rect = match(cmd, w0, w1, &top_left_word);
        if (rect != nullptr) {
            add_site(rdram, off, cmd, w0, w1, *rect, top_left_word);
        }
    }
}

void scan(uint8_t* rdram) {
    for (uint32_t off = 0; off + 8 <= rdram_bytes; off += 4) {
        const uint32_t w0 = read_word(rdram, off);
        const uint32_t cmd = w0 >> 24;
        if (cmd != cmd_setscissor && cmd != cmd_fillrect) {
            continue;
        }

        const uint32_t w1 = read_word(rdram, off + 4);
        uint32_t top_left_word = 0;
        const SafeRect* rect = match(cmd, w0, w1, &top_left_word);
        if (rect != nullptr) {
            add_site(rdram, off, cmd, w0, w1, *rect, top_left_word);
        }
        else if (tracing()) {
            // Every safe-rect-shaped command that did NOT match, so a rect this
            // table is missing shows up as a line rather than as silence. Capped,
            // because an 8 MB scan finds a lot of data that merely looks like a
            // command byte.
            static uint32_t reported = 0;
            if (reported < 60) {
                const bool scissor = (cmd == cmd_setscissor);
                const uint32_t ulx_q = scissor ? field_hi(w0) : field_hi(w1);
                const uint32_t lrx_q = scissor ? field_hi(w1) : field_hi(w0);
                const uint32_t uly_q = scissor ? (w0 & 0xFFFu) : (w1 & 0xFFFu);
                const uint32_t lry_q = scissor ? (w1 & 0xFFFu) : (w0 & 0xFFFu);
                if (ulx_q < lrx_q && uly_q < lry_q && lrx_q <= 640u * 4u && lry_q <= 480u * 4u &&
                    (lrx_q - ulx_q) >= 160u * 4u) {
                    reported++;
                    fprintf(stderr, "[widescreen] unmatched %s at 0x%08X: (%.1f,%.1f)-(%.1f,%.1f)\n",
                        scissor ? "scissor" : "fillrect", 0x80000000u + off,
                        ulx_q / 4.0, uly_q / 4.0, lrx_q / 4.0, lry_q / 4.0);
                }
            }
        }
    }
}

} // namespace

void hybridheaven::widescreen::widen_display_lists(uint8_t* rdram, uint32_t data_ptr) {
    if (disabled() || rdram == nullptr) {
        return;
    }

    // The live display list, as an rdram offset. `UINT32_MAX` if the task points
    // outside rdram, which send_dl's own guard already treats as no list at all.
    const uint32_t live = ((data_ptr - 0x80000000u) < rdram_bytes) ? ((data_ptr - 0x80000000u) & ~7u)
                                                                  : UINT32_MAX;

    const bool want_wide =
        ultramodern::renderer::get_graphics_config().ar_option != ultramodern::renderer::AspectRatio::Original;
    want_wide_now = want_wide;

    // Walk the cached sites first; this is the whole cost on a steady frame.
    //
    // A site holding neither encoding has been overwritten -- the display list
    // moved, or the buffer is being rebuilt as something else -- so it is dropped
    // and the scan below re-finds the live copy. The game rebuilds its dynamic
    // lists every frame, so a site flipping back to vanilla and being rewritten
    // once per frame is the NORMAL case, not a failure.
    uint32_t fresh = 0;   // sites found holding the game's own bytes and widened NOW
    uint32_t dropped = 0; // sites whose bytes are neither encoding -- the list moved

    for (size_t i = 0; i < sites.size();) {
        Site& site = sites[i];
        const uint32_t w0 = read_word(rdram, site.off);
        const uint32_t w1 = read_word(rdram, site.off + 4);

        if (w0 == site.vanilla_w0 && w1 == site.vanilla_w1) {
            if (want_wide) {
                write_word(rdram, site.off, site.wide_w0);
                write_word(rdram, site.off + 4, site.wide_w1);
                fresh++;
            }
            i++;
        }
        else if (w0 == site.wide_w0 && w1 == site.wide_w1) {
            if (!want_wide) {
                // Restore the game's own bytes exactly, so switching the setting
                // back to Original gives the authentic inset rather than an
                // approximation of it.
                write_word(rdram, site.off, site.vanilla_w0);
                write_word(rdram, site.off + 4, site.vanilla_w1);
            }
            i++;
        }
        else {
            sites.erase(sites.begin() + i);
            dropped++;
        }
    }

    if (!want_wide) {
        return;
    }

    static uint64_t calls = 0;
    const uint64_t call = calls++;

    // THE LIVE LIST, EVERY FRAME. The cache above keys on an rdram offset, and an
    // offset is not where the rect is -- it is where the rect was when the list
    // last had this layout. Measured: the same 640 safe rect appears at dl+0xA0,
    // dl+0xB8 and dl+0x140 in one boot, because it sits after however many
    // commands the current screen puts in front of it. Every screen change moves
    // it, the cached offsets stop matching, and nothing widened the live list
    // until the next full scan -- up to 256 frames away, which is the whole of a
    // transition. That is the pillarboxed picture with a static border: the
    // game's full-frame clear still runs, so the strips outside its 4:3 scissor
    // are cleared and then never drawn into.
    //
    // So scan the buffer the game is submitting, on the frame it submits it. The
    // window is bounded because this is a hot path, and generous against the
    // largest offset measured -- roughly a thousand times it. Anything past it,
    // or in a list this one branches to, is still the full scan's job below.
    constexpr uint32_t live_window = 0x40000u; // 256 KB
    if (live != UINT32_MAX && !legacy_live_scan()) {
        scan_range(rdram, live, std::min(live + live_window, rdram_bytes));
    }

    // The full scan is the fallback, and it is what costs: 2M word reads over the
    // 8 MB the game can address. It stays on the slow cadence -- the live-list
    // pass above is what has to be prompt, and it is.
    const uint64_t mask = sites.empty() ? 0x7u : 0xFFu;
    const size_t before = sites.size();
    const bool scanned = ((call & mask) == 0);
    if (scanned) {
        scan(rdram);
    }

    // THE EXPOSURE MEASURE, and it has to be this one. The obvious measure -- did
    // the rewrite pass widen anything this frame -- reads as healthy while the
    // picture is wrong: with two display-list buffers in rotation, a cached site
    // in one can be rewritten every frame while the rect in the OTHER has moved
    // and is going out at 4:3. That was measured, not imagined: `fresh=1` on
    // every frame of a stretch that this check catches as a miss.
    //
    // So ask the only question that decides the picture: after the rewrite pass,
    // does the list RT64 is about to walk still contain a safe rect holding the
    // game's own bytes? A yes is a frame the player sees pillarboxed. Nothing
    // about it is inferred.
    if (tracing() && live != UINT32_MAX) {
        static uint64_t miss_frames = 0, seen_frames = 0, reported = 0;
        seen_frames++;

        // Deliberately NARROWER than the window the pass above widens, and the
        // difference matters. The game rotates two display-list buffers 0x10048
        // apart, so a 256 KB window from either one contains the other -- and a
        // rect still holding the game's bytes over there is in a list that is not
        // being submitted, which is not a frame anyone sees at 4:3. Counting those
        // turns this from a miss rate into an upper bound on one. 32 KB stays
        // inside the live buffer and still clears the largest offset measured
        // (0x1748) by twenty times.
        //
        // The pass above has no such constraint: widening the sibling buffer early
        // is free and correct, because it is what gets submitted next.
        const uint32_t win_end = std::min(live + 0x8000u, rdram_bytes - 8u);
        uint32_t hits = 0, first_delta = 0;
        const char* first_name = "";
        for (uint32_t off = live; off <= win_end; off += 8) {
            const uint32_t w0 = read_word(rdram, off);
            const uint32_t cmd = w0 >> 24;
            if (cmd != cmd_setscissor && cmd != cmd_fillrect) {
                continue;
            }

            uint32_t tlw = 0;
            const SafeRect* r = match(cmd, w0, read_word(rdram, off + 4), &tlw);
            if (r != nullptr) {
                if (hits++ == 0) {
                    first_delta = off - live;
                    first_name = r->name;
                }
            }
        }

        if (hits != 0) {
            miss_frames++;
            if (reported < 40) {
                reported++;
                fprintf(stderr, "[widescreen] MISS f=%llu dl=%08X: %u un-widened %s rect(s)"
                                " in the live list, first at +0x%X (fresh=%u, %zu cached)"
                                " [%llu/%llu frames]\n",
                    (unsigned long long)call, data_ptr, hits, first_name, first_delta,
                    fresh, sites.size(),
                    (unsigned long long)miss_frames, (unsigned long long)seen_frames);
            }
        }

        // Periodic, because the per-frame lines are capped and a capped count is
        // not a rate.
        //
        // `registered` is the closest thing to an exposure figure, and it is worth
        // knowing exactly what it is and is not. It counts safe rects found at an
        // offset not already cached, so WITHIN a run it is a floor on how much the
        // list's layout moved -- 976 of them over a boot is what says the layout
        // churned constantly while the miss rate stayed at zero, which is the
        // reading that matters. ACROSS the two arms it is not a control at all:
        // the live-list pass looks every frame and the cache-only path looks every
        // 256th, so the fix inflates this count by existing.
        //
        // The obvious cross-arm control does not exist here, and the attempt is
        // worth recording so it is not made again: the game's own full-frame
        // scissor is untouched by either arm, but it is emitted before the variable
        // part of the list, so its offset never moves. Only commands after the
        // content move, and the safe rect is the one this file rewrites.
        if ((call & 0x1FFu) == 0x1FFu) {
            fprintf(stderr, "[widescreen] summary f=%llu: %llu/%llu frames missed (%.2f%%),"
                            " %llu registered, %zu cached\n",
                (unsigned long long)call, (unsigned long long)miss_frames,
                (unsigned long long)seen_frames,
                seen_frames ? 100.0 * double(miss_frames) / double(seen_frames) : 0.0,
                (unsigned long long)sites_registered, sites.size());
        }

        if (scanned && sites.size() != before) {
            fprintf(stderr, "[widescreen] scan f=%llu dl=%08X: %zu -> %zu sites (%u dropped this frame)\n",
                (unsigned long long)call, data_ptr, before, sites.size(), dropped);
        }
    }
}
