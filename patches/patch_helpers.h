#ifndef PATCH_HELPERS_H
#define PATCH_HELPERS_H

#ifdef MIPS
#include <ultra64.h>
#else
#include "recomp.h"
#endif

#ifdef __cplusplus
#define EXTERNC extern "C"
#else
#define EXTERNC
#endif

/* A host export has two faces. Compiled for MIPS it is an ordinary C
 * prototype, so patch code can call it naturally; on the host side it is a
 * recomp function taking (rdram, ctx) and pulling its arguments out of the
 * context. DECLARE_FUNC writes whichever one the current translation unit
 * needs, so a single header serves both. */
#ifdef MIPS
#define DECLARE_FUNC(type, name, ...) \
        EXTERNC type name(__VA_ARGS__)
#else /* MIPS */
#define DECLARE_FUNC(type, name, ...) \
        EXTERNC void name(uint8_t *rdram, recomp_context *ctx)
#endif

#endif /* PATCH_HELPERS_H */
