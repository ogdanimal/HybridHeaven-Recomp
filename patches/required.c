/**
 * Patches the port cannot run without.
 *
 * Right now that is one function: the overlay loader. Hybrid Heaven loads 92
 * code overlays out of its Nisitenma-Ichigo archive at runtime, and a static
 * recompilation has no way to notice that happening -- the "DMA" is a memcpy
 * into rdram, and the recompiled functions for the overlay are native code
 * sitting in the binary, unreachable until something maps their vram addresses
 * to them. `recomp_load_overlays` is that mapping, so the loader has to be
 * patched to call it.
 *
 * This mirrors Goemon64Recomp/patches/required.c, which patches the same
 * function in the same archive format for the same reason. Two of its three
 * patches do not transfer: both exist to convert Goemon's TLB-mapped overlay
 * addresses into KSEG0, and Hybrid Heaven does not TLB-map anything. Its
 * `overlay_apply_relocations` call drops out for the same reason -- see the
 * note at the bottom.
 */

#include "patches.h"
#include "misc_funcs.h"
#include "game_funcs.h"

/**
 * load(file_index, dest) -- vram 0x8000469C, 110 instructions at
 * lib/hybridheaven/asm/usa/main_4F10.s:296.
 *
 * Decompresses or DMAs archive file `file_id` to `buf_start`, then zero-fills
 * from the end of the loaded data to the end of the load-table extent (that
 * tail is the overlay's .bss). Returns the end of the extent, which callers
 * use to decide where to load the next file.
 *
 * This is a reimplementation rather than a matching decompilation: a patch
 * only has to behave the same, so the original's do-nothing 16-iteration delay
 * loop before the zero-fill is dropped along with the cache maintenance.
 *
 *
 * Is this the only way bytes reach an overlay slot?
 *
 * The eviction below is only sound if it is. Nothing had ever checked, so here
 * is the audit. The game's PI DMA wrapper is `func_80001FE8_2BE8(rom, dest,
 * size)`; it has 9 direct call sites, in 8 functions, and the recompiled C and
 * the decomp asm agree on that count. What each one does with `dest`:
 *
 *   this function            the overlay path -- the one being hooked.
 *   func_80004EE8_5AE8       a dispatcher, not a loader. file_id < 0x8000 tail-
 *                            calls this function; 0x8000..0x8B89 is a second
 *                            archive with its own tables (D_800399B8,
 *                            D_80039A78) and DMAs to a caller-supplied dest;
 *                            >= 0x8B8A returns without loading.
 *   func_80003DB4_49B4       fixed 0x2000 buffer at 0x80089518.
 *   func_80003EB8_4AB8       fixed 0x2000 buffer at 0x8008B518. These two are
 *                            one double-buffered ROM streamer.
 *   func_8001BFE4_1CBE4      fixed dest 0x801077E0, size from a table byte.
 *   func_800201D0_20DD0      audio banks, six of them, from 0x80191520 up.
 *   func_80004838_5438 (x2)  streaming reader; dest comes from its caller.
 *   func_8000DDB0_E9B0       a pure forwarder around the wrapper -- it sets a
 *                            busy flag, forwards a0/a1/a2 unchanged, clears the
 *                            flag. 8 further call sites, one of them in an
 *                            overlay (.file_56).
 *
 * The four fixed destinations were checked against the ram range of all 94
 * registered sections: none of them overlaps one. Two are worth naming because
 * they look like they should:
 *
 *   0x801077E0 sits 0x50 below .file_7, which loads at 0x80107830. The write is
 *   small and the gap holds it, but the margin is 80 bytes.
 *
 *   0x80191520 is exactly where .file_7's section ends, which reads like an
 *   overrun and is not one: splat gives .file_7 a bss_size of 0x2DC80 at vram
 *   0x80191520, so the audio banks are being loaded into .file_7's own bss. A
 *   section's recompiled extent covers text and data only, so this region is
 *   outside every section range -- and .file_7 is the permanently resident
 *   overlay, never evicted by anything.
 *
 * What this does NOT establish: three of the paths above (the 0x8000+ archive,
 * func_80004838_5438, and everything upstream of func_8000DDB0_E9B0) take their
 * destination from a caller, and bounding those statically means a full dataflow
 * closure that keeps widening -- one forwarder call site is itself inside an
 * overlay. So the honest state is: no *fixed* destination in the game aims at an
 * overlay slot, and the dispatcher routes the entire overlay file-id range here.
 * The caller-supplied paths are unproven, and the way to close them for good is
 * a runtime check in the one place that is exhaustive by construction --
 * librecomp's `do_dma` -- rather than more reading.
 */
/* @recomp Patched to register the overlay with the recomp runtime. */
RECOMP_PATCH u8 *func_8000469C_529C(u32 file_id, u8 *buf_start) {
    u32 cur;
    u32 file_rom_addr;
    u32 file_size;
    u32 file_size_aligned;
    u32 clobber_size;
    u8 *buf_cur;
    u8 *buf_end;

    if (file_id == 0 || file_id >= NI_FILE_ID_MAX) {
        return NULL;
    }

    cur = file_id - 1;

    file_rom_addr = NI_FILE_ENTRY(cur) & NI_FILE_ADDR_MASK;
    file_size = (NI_FILE_ENTRY(cur + 1) & NI_FILE_ADDR_MASK) - file_rom_addr;

    /* The load-table extent, not the file size: the difference is the .bss
     * zero-filled below. */
    buf_end = buf_start + (D_80037C5C_3885C[cur].end - D_80037C5C_3885C[cur].start);
    buf_cur = buf_start;

    /* @recomp Evict whatever this load is about to overwrite, before the load
     * registers anything new.
     *
     * Hybrid Heaven's overlay slots are fixed and shared -- 0x8038CFC0 has 41
     * tenants -- and the game just loads over the previous one without saying
     * so. The runtime has no other way to learn that the outgoing tenant's code
     * is gone, and `load_overlay` only ever adds to its function map, so an
     * indirect call through an address two tenants both cover would otherwise
     * reach whichever registered last. That was observed as a SIGSEGV with
     * frames from two overlapping sections at once.
     *
     * The range is the whole extent this call writes -- the loaded bytes plus
     * the zero-filled .bss tail -- and it is announced for every load, not only
     * the 92 code overlays, because an asset loaded over a dead overlay
     * destroys it just as thoroughly. The extent is normally the larger of the
     * two, but nothing guarantees it, so take whichever reaches further. */
    clobber_size = (u32)(buf_end - buf_start);
    if (clobber_size < file_size) {
        clobber_size = file_size;
    }
    recomp_evict_overlays((void *)buf_start, clobber_size);

    D_8008DC18_8E818 = TRUE;

    if (file_size != 0) {
        /* @recomp Register the overlay with the runtime. This has to happen
         * before anything can call into the overlay, and it is keyed on the
         * ROM range, so it is correct here regardless of which branch below
         * actually moves the bytes.
         *
         * The game's own file table supplies `file_rom_addr` and `file_size`,
         * and librecomp matches them against the section table N64Recomp
         * generated from the decomp ELF. Those agree exactly for all 92
         * overlays -- same ROM address, and each file's ROM range covers its
         * own section and no other -- so this call loads exactly one section. */
        recomp_load_overlays(file_rom_addr, (void *)buf_start, file_size);

        if (NI_FILE_ENTRY(cur) & NI_FILE_COMP_MASK) {
            /* Dead in the shipped ROM: hybridheaven.z64 is the decompressed
             * ROM, in which every one of the 625 entries has bit 31 clear.
             * Kept because it is what the original does, and it costs nothing. */
            buf_cur = func_80003824_4424(file_rom_addr, buf_start, file_size);
        } else {
            file_size_aligned = (file_size + 1) & ~1u;
            func_80001FE8_2BE8(file_rom_addr, buf_start, file_size_aligned);
            buf_cur = buf_start + file_size_aligned;
        }

        /* @recomp The original follows the decompression with
         * osWritebackDCache / osInvalICache / osInvalDCache over the loaded
         * range. There is no instruction cache to invalidate in a static
         * recompilation -- the overlay's code is the native code registered by
         * the call above, not the bytes just written -- and rdram needs no
         * coherency management. Goemon64Recomp drops the same call. */
    }

    D_8008DC18_8E818 = FALSE;

    while (buf_cur < buf_end) {
        *buf_cur = 0;
        buf_cur++;
    }

    return buf_end;
}

/*
 * Why there is no overlay_apply_relocations here.
 *
 * Goemon64Recomp calls one after each load, to fix up absolute addresses in an
 * overlay that landed somewhere other than where it was linked. Hybrid Heaven
 * needs nothing equivalent, for two independent reasons:
 *
 *  1. Its overlays do not move. Every one of the 92 has a fixed slot in the
 *     load-address table at ROM 0x3885C, and for all 92 that slot's `start` is
 *     exactly the section's link address in the decomp ELF. A shared slot --
 *     0x8038CFC0 has 41 tenants -- is still a fixed address; the tenants take
 *     turns, they do not relocate.
 *
 *  2. Even if one did move, the recompiler already handles it. Sections listed
 *     in hybridheaven.overlays.txt are relocatable, so their %hi/%lo pairs come
 *     out as RELOC_HI16/RELOC_LO16, which read `section_addresses[]` at
 *     execution time. `load_overlay` sets that entry from the `ram` argument
 *     passed above, so relocation is a consequence of the call already being
 *     made. Goemon's helper exists for its TLB-mapped overlays, which is a
 *     problem this game does not have.
 */
