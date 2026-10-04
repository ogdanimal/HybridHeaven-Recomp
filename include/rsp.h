#ifndef __HH_RSP_H__
#define __HH_RSP_H__

#include "librecomp/rsp.hpp"

/**
 * The audio microcode, recompiled from this game's ROM by
 * `RSPRecomp aspMain.toml` into rsp/aspMain.cpp. It is byte-identical to
 * Goemon64Recomp's, which is what confirmed the config -- see
 * lib/hybridheaven/PLAN.md Phase B.
 *
 * aspMain is the only microcode that needs recompiling. There are exactly two
 * SP tasks in the ROM, and the graphics one is F3DEX, which RT64 implements
 * itself.
 */
// C++ linkage, not extern "C": RSPRecomp emits a plain C++ definition in
// rsp/aspMain.cpp, and declaring it extern "C" here leaves it unresolved.
RspExitReason aspMain(uint8_t* rdram, uint32_t ucode_addr);

#endif
