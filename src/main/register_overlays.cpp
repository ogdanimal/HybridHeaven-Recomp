/**
 * Hands librecomp the section table N64Recomp generated for the whole game.
 *
 * `recomp_overlays.inl` is generated output -- 94 code sections, 92 of them
 * overlays -- and including it here is what makes `init_overlays` able to see
 * them. Nothing is *loaded* by this; it only registers what exists. The static
 * segment gets loaded by `recomp::init` (see the README on `.main`), and the 92
 * overlays by the loader hook in patches/required.c.
 */

#include <cstdlib>

#include "ovl_patches.hpp"
#include "../../RecompiledFuncs/recomp_overlays.inl"

#include "librecomp/overlays.hpp"

void hybridheaven::register_overlays() {
    recomp::overlays::overlay_section_table_data_t sections {
        .code_sections = section_table,
        .num_code_sections = ARRLEN(section_table),
        .total_num_sections = num_sections,
    };

    recomp::overlays::overlays_by_index_t overlays {
        .table = overlay_sections_by_index,
        .len = ARRLEN(overlay_sections_by_index),
    };

    recomp::overlays::register_overlays(sections, overlays);

    // Hybrid Heaven has no relocation step -- see the note at the bottom of
    // patches/required.c -- so an overlay copied somewhere other than its link
    // address goes on addressing its data at the link address, which is exactly
    // what the copied instructions do on hardware. Tracking the load address
    // instead would rebase those references onto the copy, and this game
    // genuinely runs two copies of one overlay at once: a traced run loads the
    // section at ROM 0x006A1BC0 both to its link address 0x801BF1A0 and to
    // 0x801FA948, and then calls 0x801CBDC0 -- inside the first copy -- after
    // the second has loaded.
    //
    // HH_RELOCATE_OVERLAYS=1 restores the tracking, as the A/B against this.
    recomp::overlays::set_overlay_relocation_enabled(getenv("HH_RELOCATE_OVERLAYS") != nullptr);

    // A load into the middle of a larger overlay's region destroys only the bytes
    // it covers, so only those functions stop being callable. This game does that
    // by construction: `.file_54` loads 0xA5F0 bytes at 0x803837E0, inside the
    // 0x343A0 `.file_56` occupies from 0x80358820, which overwrites 22 of
    // `.file_56`'s 454 functions and leaves 432 intact -- including
    // `func_803757B0_8193D0` at 0x803757B0, the address the port aborted on.
    //
    // HH_EVICT_WHOLE_SECTION=1 restores dropping any overlapped section entire,
    // as the A/B against this.
    recomp::overlays::set_partial_eviction_enabled(getenv("HH_EVICT_WHOLE_SECTION") == nullptr);

    // The same load, one word finer. `.file_55` loads at 0x803757E0, which is the
    // last four bytes of `func_803757B0_8193D0` (0x803757B0-0x803757E4) -- the
    // `nop` in the delay slot of its `jr $ra`. Twelve real instructions survive,
    // the recompiled function was translated from all thirteen at build time, and
    // the game calls 0x803757B0 straight afterwards. Dropping it there is what
    // aborted the run that had just cleared the battle transition.
    //
    // HH_DROP_TAIL_CLIPPED=1 restores dropping a function whose tail is clipped,
    // as the A/B against this. This project has now been bitten four times by its
    // own guards, so every one of them keeps a way back to the strict rule.
    recomp::overlays::set_tail_clip_tolerated(getenv("HH_DROP_TAIL_CLIPPED") == nullptr);

    // And the third of the same family: a section can be listed as loaded and not
    // be callable, because partial eviction leaves it loaded while dropping what an
    // overlapping load destroyed. When its bytes arrive again the missing functions
    // have to come back, exactly as the re-DMA restores them in rdram. Without this
    // the claim path's "already loaded, nothing to do" is wrong -- it was reached
    // with 147 of `.file_56`'s functions missing, and the game called one of them.
    //
    // HH_NO_RELOAD_REPAIR=1 restores the early return, as the A/B against this.
    recomp::overlays::set_reload_repair_enabled(getenv("HH_NO_RELOAD_REPAIR") == nullptr);
}
