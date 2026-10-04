/**
 * Saving: stop telling the save layer that a Rumble Pak is in the slot.
 *
 * Symptom: "Physically damaged Controller Pak is inserted in Controller 1."
 * Cause: the port answers PFS_OK to three device probes that are mutually
 * exclusive on hardware, and the game keeps the LAST answer.
 *
 *
 * The function being patched
 *
 * `func_80002BE0_37E0(channel)` is the game's "what is in this slot" probe. In
 * lib/hybridheaven/asm/usa/main_30D0.s:566 it is:
 *
 *     code = 0;
 *     if (osPfsInitPak(&queue, pfs, ch) == PFS_ERR_NEW_PACK) code = 2;
 *     if (osPfsInit  (&queue, pfs, ch) == 0)                 code = 7;
 *     if (osMotorInit(&queue, pfs, ch) == 0)                 code = 0xF;
 *     if (code != 0) return code;
 *     ret = osPfsInitPak(&queue, pfs, ch);
 *     return ret < 12 ? jtbl_8004B9D0_4C5D0[ret] : 0;
 *
 * Three probes, checked in order, each overwriting the last. On hardware at
 * most one can succeed -- a slot holds one pak -- so the ordering never
 * matters. In this port all three return PFS_OK, so `code` ends at 0xF and the
 * osPfsInitPak mapping at the bottom is never reached.
 *
 * What 0xF costs is visible in the save UI. `.file_7` at 0x8013F238 stores the
 * returned code and dispatches on it through jtbl_8018F00C_56E71C (16 entries,
 * asm/usa/data/file_7_data.data.s:100196):
 *
 *     code 0     no message; the save layer proceeds        <-- the good state
 *     code 1     message 3
 *     code 2     caller re-probes once (PFS_ERR_NEW_PACK)
 *     code 3     message 5
 *     code 4, 5  message 9
 *     code 7     message 8
 *     code 0xF   message 7                                  <-- what we hit
 *
 * That is why a trace of a failed save showed osPfsInitPak and nothing else:
 * the file-level calls (osPfsFindFile, osPfsFileState, osPfsReadWriteFile) sit
 * behind code 0 and had never once been reached.
 *
 *
 * Why patch the game rather than the runtime
 *
 * The hardware-faithful runtime fix is for both device probes to fail, since
 * the port presents a Controller Pak. That works and it costs the rumble:
 * 0x80027D04 (named `osPfsInit`, though its body selects bank 0xFE and then
 * 0x80 and returns PFS_ERR_DEVICE on either -- it is a device-identification
 * probe, not libultra's osPfsInit; see the note in the decomp's
 * symbol_addrs.txt) is also what gates the game's own rumble-available flag,
 * D_80037780_38380, in `func_80002A94_3694`. Making it fail turns the rumble
 * off with it.
 *
 * Patching here separates the two questions, which is the one thing hardware
 * cannot do: the save layer is told there is a Controller Pak, the rumble
 * subsystem keeps its own probe and keeps working. `osMotorInit` (0x80031FF0)
 * has exactly one caller in the whole game -- this function -- so dropping it
 * here costs nothing at all.
 *
 * HH_PAK_LEGACY_PROBE=1 restores the original three-probe function exactly,
 * which is the A/B.
 */

#include "patches.h"
#include "misc_funcs.h"
#include "game_funcs.h"

/* jtbl_8004B9D0_4C5D0, read out as a table.
 *
 * The jump table holds code addresses; six distinct targets, each of which
 * loads one constant into $v1 and branches to the epilogue. Entries 3 and 5-9
 * point straight AT the epilogue, where $v1 still holds the running `code` --
 * which is 0 on every path that reaches the switch -- so those map to 0, and so
 * does an out-of-range return via the `sltiu $at, $v0, 0xC` guard.
 *
 *   ret 0  -> 0x80002CC0 -> 0     PFS_OK
 *   ret 1  -> 0x80002CC8 -> 1     PFS_ERR_NOPACK
 *   ret 2  -> 0x80002CD0 -> 2     PFS_ERR_NEW_PACK
 *   ret 3  -> 0x80002CEC -> 0     PFS_ERR_INCONSISTENT (the game forgives it)
 *   ret 4  -> 0x80002CD8 -> 3     PFS_ERR_CONTRFAIL
 *   ret 5  -> 0x80002CEC -> 0     PFS_ERR_INVALID
 *   ret 6  -> 0x80002CEC -> 0     PFS_ERR_BAD_DATA
 *   ret 7  -> 0x80002CEC -> 0     PFS_DATA_FULL
 *   ret 8  -> 0x80002CEC -> 0     PFS_DIR_FULL
 *   ret 9  -> 0x80002CEC -> 0     PFS_ERR_EXIST
 *   ret 10 -> 0x80002CE0 -> 5     PFS_ERR_ID_FATAL
 *   ret 11 -> 0x80002CE8 -> 4     PFS_ERR_DEVICE
 */
static const u8 pak_code_from_pfs_err[12] = { 0, 1, 2, 0, 3, 0, 0, 0, 0, 0, 5, 4 };

#define PAK_CODE_UNKNOWN 0

/* @recomp Consult only osPfsInitPak. See the file header: the two device probes
 * this replaces both answer PFS_OK in the port, which cannot happen on hardware,
 * and the last of them wins. */
RECOMP_PATCH s32 func_80002BE0_37E0(s32 arg0) {
    s32 channel = arg0 & 0xFF;
    OSPfs *pfs = &D_8005CE70_5DA70[channel];
    s32 ret;
    s32 code;

    if (recomp_get_pak_legacy_probe()) {
        code = 0;
        if (osPfsInitPak(&D_8005CE20_5DA20, pfs, channel) == PFS_ERR_NEW_PACK) {
            code = 2;
        }
        if (osPfsInit(&D_8005CE20_5DA20, pfs, channel) == 0) {
            code = 7;
        }
        /* The third probe, as a constant.
         *
         * The original calls osMotorInit here. This does not, and the reason is
         * a link-time one rather than a shortcut: ultramodern already defines
         * `osMotorInit` unsuffixed with its own signature, so the forwarder that
         * makes osPfsInitPak/osPfsInit callable from patch code cannot be
         * written for it (see src/game/pak_shims.cpp).
         *
         * Reproducing its answer costs nothing because its answer never varies:
         * ultramodern's osMotorInit is `pfs->channel = channel; return 0;` --
         * unconditional success, input.cpp:154. If that ever stops being true,
         * this branch stops reproducing the old behaviour and the comparison it
         * exists to support becomes wrong. */
        code = 0xF;
        if (code != 0) {
            recomp_pak_probe_result(channel, -1, code);
            return code;
        }
        ret = osPfsInitPak(&D_8005CE20_5DA20, pfs, channel);
        code = (ret >= 0 && ret < 12) ? pak_code_from_pfs_err[ret] : PAK_CODE_UNKNOWN;
        recomp_pak_probe_result(channel, ret, code);
        return code;
    }

    /* One call, where the original makes two.
     *
     * The original's first call exists only to catch PFS_ERR_NEW_PACK before
     * the device probes clobber it, and its second call is what feeds the
     * table. Collapsing them changes the answer only in the case the two calls
     * disagree, which on hardware is the first-ever probe of a pak: NEW_PACK
     * then PFS_OK. That case is still handled, one level up -- the save UI at
     * 0x8013F254 re-probes when it gets code 2 -- and it cannot arise here
     * anyway, since librecomp's osPfsInitPak never reports NEW_PACK. */
    ret = osPfsInitPak(&D_8005CE20_5DA20, pfs, channel);
    code = (ret >= 0 && ret < 12) ? pak_code_from_pfs_err[ret] : PAK_CODE_UNKNOWN;

    recomp_pak_probe_result(channel, ret, code);
    return code;
}
