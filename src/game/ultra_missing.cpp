/**
 * libultra functions N64Recomp expects the runtime to supply, and it does not.
 *
 * N64Recomp will not recompile a function whose name is in its `ignored_funcs`
 * list; where one is called it emits `<name>_recomp` and leaves the definition
 * to the host. N64ModernRuntime implements most of that list but not all of it,
 * and this file covers the remainder for Hybrid Heaven.
 *
 * `tools/recomp_symbol_gap.py` is what decides whether anything belongs here.
 * Its rule is worth restating: a missing symbol usually means the decomp still
 * has a libultra function under a placeholder, and the fix is to *name* that
 * function, not to add a stub here. Only symbols genuinely called from game
 * code belong in this file.
 */

#include <cmath>
#include <cstdint>

#include "ultramodern/ultramodern.hpp"
#include "recomp.h"

/**
 * double -> unsigned long long, as IDO's runtime does it.
 *
 * This one really is called from game code -- three sites, none of them
 * libultra -- so naming cannot make it go away.
 *
 * Hybrid Heaven's own copy at vram 0x80034AB8 sets the FPU to round toward
 * zero, `cvt.l.d`s, and on overflow retries after subtracting 2^63 and ORs the
 * sign bit back in; anything still out of range, and anything negative, returns
 * all-ones. C's conversion already truncates toward zero, so the only parts
 * worth reproducing are the saturation and the negative case.
 *
 * The return is an o32 64-bit value: $v0 holds the high word and $v1 the low,
 * each sign-extended into its 64-bit slot -- which is exactly what the original
 * ends with, `dsra32 $v1, $v1, 0` / `dsra32 $v0, $v0, 0`.
 */
extern "C" void __d_to_ull_recomp(uint8_t* rdram, recomp_context* ctx) {
    (void)rdram;
    double d = ctx->f12.d;
    uint64_t v;

    // 18446744073709551616.0 is 2^64. The !(d >= 0.0) form also catches NaN.
    if (!(d >= 0.0) || d >= 18446744073709551616.0) {
        v = UINT64_MAX;
    } else {
        v = static_cast<uint64_t>(d);
    }

    ctx->r2 = static_cast<int32_t>(static_cast<uint32_t>(v >> 32));
    ctx->r3 = static_cast<int32_t>(static_cast<uint32_t>(v));
}

/**
 * The SI access mutex.
 *
 * These are NOT implemented, deliberately, and reaching them means something is
 * wrong rather than merely unfinished. They are only reachable through
 * `__osContRamRead` / `__osContRamWrite`, which are still under placeholders
 * (vram 0x80034060 and 0x80033E10) along with the rest of the osPfs subsystem
 * -- about 23 functions that have to be named as a unit, because naming any
 * subset just moves the gap. See "The libultra naming gap" in
 * docs/development-notes.md.
 *
 * Until that is done, the Controller Pak path runs the game's own driver, which
 * would poke SI registers librecomp does not emulate and then block forever on
 * an SI message that never arrives. Failing loudly here turns that hang into a
 * diagnosable stop, which is the same thing librecomp's own ultra_stubs.cpp
 * does for the subsystems it has not implemented.
 *
 * The right fix is to name the osPfs family, at which point librecomp's
 * `pak.cpp` -- which reimplements all twelve public `osPfs*` entry points --
 * handles Controller Pak natively and neither of these is ever emitted.
 */
static void si_access_unimplemented(const char* who) {
    ultramodern::error_handling::message_box(
        "Hybrid Heaven: the Controller Pak path is not supported yet.\n\n"
        "The osPfs subsystem is still under placeholder symbols, so the game's "
        "own SI driver was reached instead of the runtime's. See "
        "\"The libultra naming gap\" in the project README.");
    ultramodern::error_handling::quick_exit(__FILE__, __LINE__, who);
}

extern "C" void __osSiGetAccess_recomp(uint8_t* rdram, recomp_context* ctx) {
    (void)rdram; (void)ctx;
    si_access_unimplemented("__osSiGetAccess");
}

extern "C" void __osSiRelAccess_recomp(uint8_t* rdram, recomp_context* ctx) {
    (void)rdram; (void)ctx;
    si_access_unimplemented("__osSiRelAccess");
}
