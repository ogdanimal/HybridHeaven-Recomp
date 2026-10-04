/**
 * Registers the recompiled patches, so their versions of patched functions win.
 *
 * `manual_patch_symbols` is what makes `recomp_load_overlays` reachable from
 * patch code: the patch calls an absolute address in the 0x8F000000 range
 * (patches/syms.ld), and this table is where librecomp looks it up.
 */

#include "ovl_patches.hpp"
#include "../../RecompiledPatches/patches_bin.h"
#include "../../RecompiledPatches/recomp_overlays.inl"

#include "librecomp/overlays.hpp"
#include "librecomp/game.hpp"

void hybridheaven::register_patches() {
    // The blob is generated as unsigned char (a braced initializer of 0xNN
    // literals narrows if the array is plain char); the API wants const char*.
    recomp::overlays::register_patches(reinterpret_cast<const char*>(hh_patches_bin),
                                       sizeof(hh_patches_bin),
                                       section_table, ARRLEN(section_table));
    recomp::overlays::register_base_exports(export_table);
    recomp::overlays::register_base_events(event_names);
    recomp::overlays::register_manual_patch_symbols(manual_patch_symbols);
}
