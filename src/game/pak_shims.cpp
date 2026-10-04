/**
 * Lets patch code call the libultra functions the runtime provides.
 *
 * The two recompilations name a runtime-provided libultra function differently.
 * The base recompilation knows `osPfsInitPak` is reimplemented on the host and
 * emits a call to `osPfsInitPak_recomp`; the patch recompilation resolves the
 * same name against HybridHeaven-RecompSyms/hybridheaven.syms.toml, where it is
 * an ordinary game function at 0x80032FB0, and emits a call to plain
 * `osPfsInitPak`. Nothing defines that, so a patch that calls libultra links
 * green through `--unresolved-symbols=ignore-all` on the MIPS side and then
 * fails at the final host link:
 *
 *   libPatchesLib.a(patches.c.o): in function `func_80002BE0_37E0':
 *   undefined reference to `osPfsInitPak'
 *
 * A loud failure, which is the good case -- patches silently not being applied
 * has cost this project five separate debugging sessions. These forwarders are
 * the fix, and they are only safe for names the runtime does NOT already define
 * unsuffixed. That is checked, not assumed:
 *
 *   nm liblibrecomp.a libultramodern.a | grep -w osPfsInitPak   -> nothing
 *   nm liblibrecomp.a libultramodern.a | grep -w osPfsInit      -> nothing
 *   nm liblibrecomp.a libultramodern.a | grep -w osMotorInit    -> T (ultramodern)
 *
 * So `osMotorInit` must NOT be forwarded here: ultramodern defines it with its
 * own signature and a second definition would be a duplicate symbol. patches/
 * pak.c is written not to call it for that reason.
 *
 * Add a forwarder here whenever a patch starts calling a new libultra function,
 * after checking the same way.
 */

#include "librecomp/helpers.hpp"
#include "recomp.h"

extern "C" {

void osPfsInitPak_recomp(uint8_t* rdram, recomp_context* ctx);
void osPfsInit_recomp(uint8_t* rdram, recomp_context* ctx);

void osPfsInitPak(uint8_t* rdram, recomp_context* ctx) {
    osPfsInitPak_recomp(rdram, ctx);
}

void osPfsInit(uint8_t* rdram, recomp_context* ctx) {
    osPfsInit_recomp(rdram, ctx);
}

}
