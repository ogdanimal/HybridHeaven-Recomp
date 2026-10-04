#ifndef GAME_FUNCS_H
#define GAME_FUNCS_H

#include <ultra64.h>

/* Hybrid Heaven symbols the patches reference.
 *
 * FUNCTIONS stay *undefined* in patches.elf on purpose: N64Recomp resolves them
 * by name against func_reference_syms_file when it recompiles the patch and
 * emits a direct call, so the link must not try to satisfy them.
 *
 * DATA is different -- each of the three objects below is given an absolute
 * address in patches/syms.ld rather than being resolved by name. Resolving data
 * by name emits a relocation against the *reference* section index, which does
 * not match the index space the base recompilation uses here. syms.ld explains
 * it in full; it is the bug that made the loader hook fault the first time it
 * ever ran. */

/* The Nisitenma-Ichigo archive table, at ROM 0x39BE0. The symbol points at the
 * 16-byte ASCII signature "Nisitenma-Ichigo"; the big-endian u32 entries begin
 * immediately after it, which is why the loader indexes at +0x10.
 *
 * Entry i is file i's ROM offset, with bit 31 set when the file is
 * LZKN64-compressed. A file ends where the next entry begins.
 *
 * Note the two index conventions in play. The loader's `file_id` is 1-based,
 * so it reads entry `file_id - 1`; splat's `.file_N` segments are named by the
 * 0-based entry index. `.file_7` is therefore the loader's file_id 8. */
extern u32 D_80038FE0_39BE0[];
#define NI_SIGNATURE_WORDS 4
#define NI_FILE_ENTRY(i)   (D_80038FE0_39BE0[NI_SIGNATURE_WORDS + (i)])

#define NI_FILE_ADDR_MASK  0x7FFFFFFF
#define NI_FILE_COMP_MASK  0x80000000
/* The loader's own bound: it rejects file_id >= 0x271 (625). */
#define NI_FILE_ID_MAX     0x271

/* The load-address table at ROM 0x3885C, 624 entries, ending exactly where the
 * Nisitenma-Ichigo signature begins. Indexed by the same 0-based index as
 * NI_FILE_ENTRY. `end - start` exceeds the file's size by the amount of .bss
 * the loader zero-fills past the loaded data. */
typedef struct {
    u32 start;
    u32 end;
} FileLoadAddr;

extern FileLoadAddr D_80037C5C_3885C[];

/* Set while a file load is in progress. */
extern u8 D_8008DC18_8E818;

/* LZKN64 decompressor: reads `size` bytes from ROM `rom_addr`, writes the
 * decompressed result at `dest`, returns the end of what it wrote. */
u8 *func_80003824_4424(u32 rom_addr, u8 *dest, u32 size);

/* Plain ROM read (PI DMA) of `size` bytes from ROM `rom_addr` to `dest`. */
void func_80001FE8_2BE8(u32 rom_addr, u8 *dest, u32 size);

/* --- Controller Pak ------------------------------------------------------ */

/* The message queue every osPfs/osMotor call in this game is handed, and the
 * four OSPfs structs it inits per channel. Both live in `.main`'s data section
 * (asm/usa/data/main_data.data.s, ROM 0x5DA20 and 0x5DA70), so they are given
 * absolute addresses in syms.ld for the reason that file explains at length.
 *
 * The stride is worth stating because it was checked: the array spans 0x1A0
 * bytes for four entries, i.e. 0x68 each, which is sizeof(OSPfs) exactly. The
 * save layer and the rumble subsystem share these structs -- the same element is
 * passed to osPfsInitPak and to __osMotorAccess -- which is faithful, since a
 * slot holds one pak. */
extern OSMesgQueue D_8005CE20_5DA20;
extern OSPfs D_8005CE70_5DA70[4];

#endif /* GAME_FUNCS_H */
