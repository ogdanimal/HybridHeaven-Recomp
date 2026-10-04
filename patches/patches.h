#ifndef PATCHES_H
#define PATCHES_H

/* libultra functions that N64ModernRuntime reimplements natively are reached
 * under a `_recomp` name (N64Recomp's `reimplemented_funcs` in
 * src/symbol_lists.cpp). A patch that calls one must use that name, or it
 * links against the game's own copy, which the recompiler never emitted --
 * every one of these is in `ignored_funcs` too.
 *
 * Only add a rename when a patch actually calls the function; an unused
 * #define here is harmless but an incorrect one is not. Goemon64Recomp's
 * patches.h carries a much longer list because its patches do far more. */
#define osWritebackDCacheAll osWritebackDCacheAll_recomp

#include <ultra64.h>

#define RECOMP_EXPORT      __attribute__((section(".recomp_export")))
#define RECOMP_PATCH       __attribute__((section(".recomp_patch")))
#define RECOMP_FORCE_PATCH __attribute__((section(".recomp_force_patch")))
#define RECOMP_DECLARE_EVENT(func) \
    _Pragma("GCC diagnostic push") \
    _Pragma("GCC diagnostic ignored \"-Wunused-parameter\"") \
    __attribute__((noinline, weak, used, section(".recomp_event"))) void func {} \
    _Pragma("GCC diagnostic pop")

#include "patch_helpers.h"

#ifndef NULL
#define NULL ((void *)0)
#endif

#define TRUE  1
#define FALSE 0

#endif /* PATCHES_H */
