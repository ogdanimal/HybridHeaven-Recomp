#include "patches.h"
#include "autosave.h"
#include "misc_funcs.h"

/**
 * Autosave. Ported from Goemon64Recomp's patches/autosave.c, which is itself a
 * port of Zelda64Recomp's patches/autosaving.c.
 *
 * THE ONE STRUCTURAL DIFFERENCE FROM GOEMON, AND IT IS THE WHOLE DESIGN
 * ====================================================================
 *
 * Goemon *reimplements* its game's save routine, because that routine lives in
 * an overlay that is not resident during gameplay. Hybrid Heaven does not need
 * that, and reimplementing here would be actively wrong.
 *
 * `.file_7` IS the gameplay engine. It spans 0x80107830..0x80191520, it holds
 * gState, the camera state machine, the player code AND the whole save/load
 * layer, and patches/camera.c already runs from inside it every frame. So the
 * game's own save code is callable from here directly, and this file calls it:
 *
 *     func_80141628_520D38(device, slot)   marshal live state + write the slot
 *     func_80141568_520C78(device, dir+4)  write the 0x100-byte directory
 *
 * That is the body of the game's own save routine `func_80141268_520978`,
 * minus its final call to `func_8014307C_52278C` -- 616 instructions of
 * save-menu message UI, which is the one part that must NOT run over live
 * gameplay. Everything a save actually consists of is the two calls above plus
 * the directory-entry fields between them, all reproduced below in the order
 * the original writes them.
 *
 * The consequence is that a resulting save is not "indistinguishable from" a
 * real one by argument -- it IS one, produced by the same code, so there is no
 * custom format, checksum, slot or respawn handling anywhere in this feature.
 *
 * WHY THE MARSHAL IS NEVER RUN SPECULATIVELY
 * ==========================================
 *
 * The obvious way to build the settled/changed check would be to run the
 * marshal `func_80141F28_521638` into a scratch buffer and hash the 0xD00 it
 * produces -- that is exactly the payload, so it would be the perfect measure.
 *
 * It is not available. The marshal is NOT idempotent. Its bulk serializer
 * `func_80144C40_524350` opens by accumulating counters back into game state:
 *
 *     8018_DC40[0x88] += 8017_DDA4[0x2]      (and the same for 0x89/0x8A/0x9C)
 *     8017_DDA4[0x0..0x7] = <reset>
 *
 * i.e. "fold the elapsed-since-last-save counters into the totals, then reset
 * them". Running it to *look* at the result would bank those counters without
 * a save behind it, and the next real save would then under-count. So this
 * file never calls the marshal except as part of a save it is actually
 * committing, and the settled check below hashes the marshal's SOURCES
 * instead. See "The settled check".
 *
 * (The same fact makes the timed save harmless, which is worth stating because
 * it looks like the opposite: those counters are a running total plus a delta,
 * so folding the delta in more often does not change the total.)
 */

/* ---------------------------------------------------------------------------
 * Game functions. Plain C declarations resolved by name by N64Recomp against
 * the reference symbols -- the established idiom, cf. patches/camera.c's
 * declaration of func_8011A06C_4F977C. Argument shapes are recovered from the
 * register usage at each call site.
 * ------------------------------------------------------------------------- */

/* Marshal live state into a 0xD00 image and write it to `slot`. `device` is the
 * save layer's controller selector, NOT a channel index: 1 means controller 1
 * (channel 0) and 2 means controller 2 (channel 1). Anything else is a no-op
 * returning zero, which is why hh_save_now_inner validates it rather than
 * trusting the return.
 *
 * Returns 0 on success, otherwise the save layer's own status code (the values
 * `.file_7` dispatches on: 1 no pak, 3 comms failure, 8/9 write errors, ...).
 *
 * Internally: alloc 0xD00, func_80141F28_521638 (the marshal), then
 * func_800032E0_3EE0(ch, 0, slot * 0xD00 + 0x100, 0xD00, buf), then free. */
s32 func_80141628_520D38(u8 device, u8 slot);

/* Write the 0x100-byte directory block that sits at pak file offset 0, ahead of
 * the four slots. Same `device` convention, same return convention. The
 * directory is what the load menu lists, so a slot written without this is
 * present on the pak and invisible in the UI.
 *
 * THE SECOND ARGUMENT IS NOT OPTIONAL, AND ITS ABSENCE SEGFAULTED THE GAME ON
 * DEVICE (2026-08-01). This function's own body reads only `a0`: it dispatches
 * on the device and calls func_80142350_521A60 without ever setting `a1`. That
 * makes it look like a one-argument function in isolation, and it is not --
 * `a1` PASSES THROUGH untouched to func_80142350_521A60, which hands it to
 * func_80141BD0_5212E0 as the source it builds the 0x100 image from. Declared
 * with one parameter, `a1` held whatever the caller happened to leave there and
 * the builder dereferenced it.
 *
 * The lesson is the one this project keeps relearning in new clothes: a
 * function's signature is not what its own body reads, it is what its callees
 * read. Recover argument shapes from CALL SITES, and for a dispatch wrapper
 * check the callee too.
 *
 * The game passes the directory array base PLUS 4 -- D_801BEB84 for device 1 and
 * D_801BEBAC for device 2. That +4 is also why the per-slot fields below sit at
 * +4..+0xB of an 8-byte entry: relative to the pointer the game actually hands
 * over, they are simply +0..+7. */
s32 func_80141568_520C78(u8 device, void *dir_base);

/* Chapter/progress code for the directory entry, called by the game's save
 * routine only when gState[0] is zero. NOT a pure getter -- it walks the task
 * list, writes D_801BBAD0_59B1E0 and consumes RNG through
 * func_8012FF58_50F668. It is called here exactly where and as often as the
 * game's own save routine calls it, and nowhere else. */
s32 func_80108280_4E7990(void);

/* ---------------------------------------------------------------------------
 * Game data. Every symbol here is `.file_7` and is given an absolute address in
 * patches/syms.ld; syms.ld explains at length why data must be absolute while
 * functions must stay undefined. The "do not generalise to any overlay address"
 * caveat there applies and is satisfied for the same reason: this file only
 * ever runs from inside a `.file_7` function, so `.file_7` being resident is a
 * precondition of every read below.
 * ------------------------------------------------------------------------- */

/* gState, the gameplay engine's master struct. Reached by offset rather than
 * through a padded struct, for the reason camera.c gives: a struct would imply
 * the rest is known. */
extern u8 D_801BBBF0_59B300[];

#define GS_U8(off)  (*(volatile u8  *)(D_801BBBF0_59B300 + (off)))
#define GS_U16(off) (*(volatile u16 *)(D_801BBBF0_59B300 + (off)))

/* The camera mode mirror and the scene triple -- the safe-state gate. See
 * "The safe-state gate" below; the offsets and their meanings are camera.c's,
 * measured over a 19-minute session (34,653 samples). */
#define GS_CAM_MODE      0x29A
#define GS_SCENE_A       0x254
#define GS_SCENE_B       0x255
#define GS_SCENE_C       0x256

/* gState's copy of the primary controller record is at +0x9C, so the held
 * button word is at +0x9E. Standard N64 bit values. */
#define GS_BUTTONS       0x09E

#define BTN_L            0x0020
#define BTN_R            0x0010
#define BTN_Z            0x2000

/* The mode in which the game lets the player drive -- exploration on foot. */
#define CAM_MODE_PLAYER  2

/* The save cursor, a 6-byte struct in `.file_7` bss. The load path reads [5]
 * for controller 1 and [1] for controller 2; the game's own save routine reads
 * [5] for BOTH devices, which is reproduced here rather than corrected --
 * saving to a slot the game would not have saved to is precisely the failure
 * this feature must not have.
 *
 * [2] is the device selector the save routine dispatches on (1 or 2). */
extern u8 D_801BEC00_59E310[];

#define SAVE_DEVICE  (*(volatile u8 *)(D_801BEC00_59E310 + 2))
#define SAVE_SLOT    (*(volatile u8 *)(D_801BEC00_59E310 + 5))

/* The in-RAM directory, one 8-byte entry per slot, one array per device. */
extern u8 D_801BEB80_59E290[];   /* device 1 (controller 1) */
extern u8 D_801BEBA8_59E2B8[];   /* device 2 (controller 2) */

/* Written into directory entry +7 by the game's save routine. */
extern u16 D_8017DC88_55D398;

/* Two of the marshal's source blocks, watched by the settled check. */
extern u8 D_8017DC40_55D350[];   /* 0x9E bytes, the bulk serializer's first run */
extern u8 D_80183CE0_5633F0[];   /* 0x56 six-byte records */
extern u8 D_801BED38_59E448[];   /* 0x200 bytes, copied verbatim into the image */

/* Set while a file load is in progress; +8 begins a 0x64-byte block the marshal
 * copies into the image at +0x300. Already declared in game_funcs.h for the
 * loader hook, redeclared here so this file stands alone. */
extern u8 D_8008DC18_8E818;

/* Number of save slots. The pak file the game creates is 0x3500 bytes
 * (func_80142240_521950 passes that to func_80002EF0_3AF0) and the layout is a
 * 0x100 directory followed by 0xD00 slots: 0x100 + 4 * 0xD00 == 0x3500 exactly.
 * Derived from the game's own allocation rather than from a UI bound, so it is
 * the number the pak actually has room for. */
#define SAVE_SLOT_COUNT  4

/* Distinct from any status the save layer produces, so they are identifiable in
 * the log. The save layer's own codes are small positive integers. */
#define AUTOSAVE_ERR_NO_DEVICE (-2)
#define AUTOSAVE_ERR_BAD_SLOT  (-3)

/* ---------------------------------------------------------------------------
 * The safe-state gate.
 *
 * Zelda64Recomp uses gCanPause and Goemon re-evaluates its game's pause-gate
 * conjunction. Hybrid Heaven has something better already measured: the gate
 * patches/camera.c hangs off, which is the game's own answer to "may the player
 * drive right now".
 *
 * `func_8011B254_4FA964` returns gState+0x29A, the mirror of the camera mode
 * selector D_80163740_542E50 that func_8010DD4C_4ED45C dispatches on once a
 * frame. `func_8011B260_4FA970` returns 0 for scene 2-8-1 and otherwise packs
 * the scene triple as decimal; zero is the permissive answer. The game requires
 * both -- scene gate zero AND mirror exactly 2 -- before it will hand the
 * player the orbit camera.
 *
 * What that buys, from camera.c's measurements over a 19-minute session:
 * mode 2 during exploration accepted every time, **mode 0 during cutscenes and
 * elevators refused every time, mode 3 for the whole of four battles**. So the
 * conjunction excludes cutscenes, elevators and combat without this file having
 * to enumerate any of them -- the same trick as gCanPause, using a gate this
 * port has already validated on hardware rather than one written from the
 * disassembly for this feature.
 *
 * The load flag is checked on top of it, because a file load is the one unsafe
 * state that has nothing to do with the camera.
 *
 * WHAT THIS GATE DOES NOT DO, stated so it is not mistaken for cover it does
 * not give: it says nothing about the pause menu, and it is not a claim that
 * every mode-2 frame is mid-transaction-free. The settled check below is what
 * covers the second; the first is untested and is the first thing to look at if
 * a save ever lands somewhere it should not.
 * ------------------------------------------------------------------------- */

static s32 autosave_is_safe(void) {
    /* A file load is in flight -- the engine is being overwritten underneath
     * us. Nothing else about the frame matters. */
    if (D_8008DC18_8E818 != 0) {
        return FALSE;
    }

    /* The game's own scene gate. Zero is permissive; 2-8-1 is the one scene
     * that is written out in full here because it exists in the ROM for a
     * reason camera.c's session never visited. */
    if (GS_U8(GS_SCENE_A) == 2 && GS_U8(GS_SCENE_B) == 8 && GS_U8(GS_SCENE_C) == 1) {
        return FALSE;
    }

    /* Exploration on foot, and nothing else. */
    if (GS_U8(GS_CAM_MODE) != CAM_MODE_PLAYER) {
        return FALSE;
    }

    return TRUE;
}

/* ---------------------------------------------------------------------------
 * The settled check.
 *
 * Require the save-relevant state to hold still for a short span before a save
 * may commit, so a write never lands mid-transaction. This protects the
 * autosave itself; the host's `.bak` rollback point protects everything around
 * it (src/game/save_rollback.cpp).
 *
 * MEASURED IN TIME, NOT FRAMES -- and that is not a stylistic choice. Goemon
 * counts frames because its poll is called from a patched main loop exactly
 * once per frame. This one is called from camera.c's func_80119F9C_4F96AC
 * patch, which has 30+ call sites across the camera state handlers and can run
 * more than once in a frame (camera.c edge-detects its recenter button for the
 * same reason). A frame counter here would be a call counter wearing a frame
 * counter's name, and would settle in a third of the time it claimed to.
 *
 * WHAT IS WATCHED
 * ---------------
 * The marshal's sources, since the marshal's output is off limits (see the file
 * header). Five ranges, every one of them read by func_80141F28_521638 or by
 * the bulk serializer it calls:
 *
 *   A  gState +0x02, 0x0A      the halfwords at +0x2/+0x4/+0x6/+0x8/+0xA.
 *   B  gState +0x0E, 0x10      +0xE..+0x1D. Includes the four halfwords
 *                              func_80126968_506078 refreshes at save time,
 *                              which are therefore stale between saves --
 *                              harmless, they simply never move.
 *   C  gState +0x39D, 0x01     the lone far byte the marshal reads.
 *   D  8008_DC18 +0x08, 0x64   copied to image +0x300.
 *   E  801B_ED38, 0x200        copied verbatim (func_8014C294 returns the base,
 *                              func_8014C2A0 returns the length, both are
 *                              two-instruction constants).
 *   F  8017_DC40, 0x9E         the bulk serializer's first block.
 *   G  8018_3CE0, 0x204        its 0x56 six-byte records.
 *
 * RANGES A-C ARE THE MARSHAL'S EXACT FIELD LIST, NOT A CONVENIENT BLOCK AROUND
 * IT, AND THAT DISTINCTION IS THE WHOLE OF A SHIPPED BUG.
 *
 * The first version of this file hashed gState +0x00..+0x20 in one go, on the
 * reasoning that a contiguous superset of the fields could only ever be
 * over-cautious. It cannot. **gState +0x0D changes on essentially every poll**
 * -- and the marshal never reads it, so it is not in the save at all. One
 * volatile byte of collateral held `settle_stable_since_us` at `now_us`
 * forever, the window never elapsed, and the feature refused *every* save while
 * reporting a completely healthy-looking `0/167 ms`.
 *
 * The comment that used to sit here claimed this check "cannot produce a FALSE
 * unsettled". It could, it did, and the claim is what stopped the 0/167 in the
 * first device log from being read as the smoking gun it was. Bytes +0x00,
 * +0x01, +0x0C, +0x0D, +0x1E and +0x1F are excluded because the marshal does not
 * read them; do not widen these ranges back to a tidy block without checking
 * each added byte for volatility first.
 *
 * NOT COMPLETE, and saying so is the point: func_80144C40_524350 continues past
 * range E into several more blocks that were not traced. So this check can miss
 * a transaction confined entirely to an unwatched block. It cannot produce a
 * FALSE unsettled, which is the direction that would refuse every save and look
 * exactly like a gate working correctly -- the failure Goemon's own notes
 * record hitting three times. Widening it is a matter of tracing the rest of
 * that function and adding ranges; nothing else here changes.
 *
 * Player world position is deliberately absent -- it is not in any watched
 * range -- so walking around does not prevent settling. A check that only
 * settled while standing still would refuse almost every save.
 * ------------------------------------------------------------------------- */

/* Microseconds the watched state must hold still. 167ms is ten frames at 60Hz,
 * matching Zelda64Recomp's ~10-frame window, expressed in the unit this poll
 * can actually measure. */
#define SETTLE_US 167000u

#define SETTLE_RANGE_COUNT 7

static u32 settle_hash[SETTLE_RANGE_COUNT];
static u32 settle_stable_since_us;
static s32 settle_primed;
static u32 settle_changed_mask;

/* FNV-1a. Change detection only -- collision resistance is not needed and is
 * not claimed. ~0x506 bytes per poll. */
static u32 settle_hash_range(const volatile u8 *p, u32 size) {
    u32 h = 0x811C9DC5u;
    u32 i;

    for (i = 0; i < size; i++) {
        h ^= (u32)p[i];
        h *= 0x01000193u;
    }
    return h;
}

static void settle_hash_all(u32 out[SETTLE_RANGE_COUNT]) {
    out[0] = settle_hash_range((const volatile u8 *)(D_801BBBF0_59B300 + 0x02), 0x0A);
    out[1] = settle_hash_range((const volatile u8 *)(D_801BBBF0_59B300 + 0x0E), 0x10);
    out[2] = settle_hash_range((const volatile u8 *)(D_801BBBF0_59B300 + 0x39D), 0x01);
    out[3] = settle_hash_range((const volatile u8 *)(&D_8008DC18_8E818 + 8), 0x64);
    out[4] = settle_hash_range((const volatile u8 *)D_801BED38_59E448, 0x200);
    out[5] = settle_hash_range((const volatile u8 *)D_8017DC40_55D350, 0x9E);
    out[6] = settle_hash_range((const volatile u8 *)D_80183CE0_5633F0, 0x204);
}

/* Must be called on every poll, not lazily at trigger time -- the stability
 * window is meaningless otherwise. */
static void update_settle_state(u32 now_us) {
    u32 fresh[SETTLE_RANGE_COUNT];
    u32 changed = 0;
    s32 i;

    settle_hash_all(fresh);

    for (i = 0; i < SETTLE_RANGE_COUNT; i++) {
        if (fresh[i] != settle_hash[i]) {
            changed |= (1u << i);
        }
        settle_hash[i] = fresh[i];
    }

    /* The first pass after enabling only seeds the hashes; everything looks
     * "changed" against zeroed state, which is not a real transaction. */
    if (!settle_primed) {
        settle_primed = 1;
        settle_stable_since_us = now_us;
        settle_changed_mask = 0;
        return;
    }

    if (changed != 0) {
        settle_stable_since_us = now_us;
        settle_changed_mask = changed;
    }
}

static s32 autosave_is_settled(u32 now_us) {
    return (u32)(now_us - settle_stable_since_us) >= SETTLE_US;
}

/* ---------------------------------------------------------------------------
 * The save itself.
 * ------------------------------------------------------------------------- */

/* The body. Do NOT call this directly -- hh_save_now() wraps it with the
 * rollback bracket, and every pak write it makes must happen inside that
 * bracket. This mirrors func_80141268_520978 exactly, in its order, minus the
 * message UI it ends with. */
static s32 hh_save_now_inner(void) {
    u8 device = SAVE_DEVICE;
    u8 slot = SAVE_SLOT;
    u8 *dir_base;
    volatile u8 *entry;
    s32 status;

    /* Both cursors are validated BEFORE anything is written, and the validation
     * is load-bearing rather than defensive tidiness.
     *
     * The device selector is the harmless one: the game's save routine simply
     * falls through when it is neither 1 nor 2, returning an uninitialised
     * stack byte. Refusing here turns that into a reportable status.
     *
     * The slot cursor is the dangerous one. It is owned by the save/load menu,
     * so on a session that has never opened that menu it may be stale, and
     * nothing downstream re-checks it: the pak byte offset is computed as
     * slot * 0xD00 + 0x100 with no bound anywhere between here and the write.
     * An in-range wrong slot silently overwrites a different save; an
     * out-of-range one writes past the pak file. */
    if (device != 1 && device != 2) {
        return AUTOSAVE_ERR_NO_DEVICE;
    }
    if (slot >= SAVE_SLOT_COUNT) {
        return AUTOSAVE_ERR_BAD_SLOT;
    }

    /* Marshal and write the slot. The game bails on any non-zero status here
     * without touching the directory, so a failed write cannot leave the
     * directory claiming a save that is not there. */
    status = func_80141628_520D38(device, slot);
    if (status != 0) {
        return status;
    }

    /* The directory entry for the slot just written. The game's routine builds
     * this from gState between the two writes; the fields and their order are
     * its own.
     *
     * Note both device branches of func_80141268_520978 write the SAME gState
     * fields -- the compiler materialised the second branch's addresses as
     * D_801BBBFA/D_801BBBF8/D_801BBC0D, which are gState +0xA/+0x8/+0x1D. Only
     * the directory base differs, so there is one path here rather than two. */
    dir_base = (device == 1) ? D_801BEB80_59E290 : D_801BEBA8_59E2B8;
    entry = (volatile u8 *)(dir_base + (u32)slot * 8);

    entry[4] = 1;
    entry[5] = 1;
    entry[6] = (GS_U8(0) == 0) ? (u8)(func_80108280_4E7990() >> 8) : 0xFF;
    entry[7] = (u8)D_8017DC88_55D398;
    *(volatile u16 *)(entry + 8) = GS_U16(0xA);
    entry[10] = (u8)GS_U16(0x8);
    entry[11] = GS_U8(0x1D);

    /* Publish it. Everything above is in RAM until this lands.
     *
     * The +4 is the game's own convention at both of its call sites, not an
     * adjustment of ours -- see the declaration above. */
    return func_80141568_520C78(device, (void *)(dir_base + 4));
}

/* Wraps the save in the rollback bracket, telling the host that every pak write
 * between these two calls is the autosave's own rather than a deliberate,
 * player-initiated save. The host maintains a `.manual.bak` rollback point by
 * observing pak writes it did NOT see bracketed -- see src/game/save_rollback.cpp
 * and docs/autosave.md.
 *
 * TWO THINGS THIS STRUCTURE IS DELIBERATELY BUYING, both easy to undo by
 * "simplifying" it back into the caller:
 *
 *  1. The bracket covers the WHOLE body, not just the slot write. A save also
 *     pushes the 0x100-byte directory through the pak, and an unbracketed
 *     directory write would arm the host's one-shot and copy autosave content
 *     into `.manual.bak` -- exactly the state it exists to roll back from. C
 *     has no RAII, so a single wrapper is what keeps every early return (bad
 *     device, bad slot, failed slot write) balanced.
 *  2. The bracket lives here, not at the call site, so the timer and the combo
 *     both inherit it instead of having to remember it.
 */
static s32 hh_save_now(void) {
    s32 status;

    recomp_set_autosave_in_progress(1);
    status = hh_save_now_inner();
    recomp_set_autosave_in_progress(0);

    return status;
}

/* ---------------------------------------------------------------------------
 * The timer.
 *
 * THREE BEHAVIOURS WORTH KNOWING, none of them incidental:
 *
 *  1. An elapsed interval does NOT consume the save. If the gate or the settled
 *     check refuses, the timer keeps retrying on every poll rather than
 *     skipping to the next period. Otherwise an interval that happened to
 *     elapse during a cutscene would silently lose that save entirely.
 *  2. Nothing is written if nothing changed. The settled hashes are snapshotted
 *     at each save; if they still match, the save is skipped. Standing idle
 *     therefore produces no writes at all -- which matters because every flush
 *     rotates the runtime's `.bak`, so a timer that wrote unconditionally would
 *     churn it for no benefit.
 *  3. A failed save resets the timer anyway. Otherwise a persistently failing
 *     save (no pak, say) would retry on every poll.
 * ------------------------------------------------------------------------- */

/* 2 minutes, matching Goemon. Wrap-safe: recomp_time_us is u32 microseconds and
 * wraps about every 71 minutes, but the unsigned subtraction below is correct
 * across a wrap for any interval short of the full period. */
#define AUTOSAVE_INTERVAL_US (120u * 1000u * 1000u)

static u32 autosave_last_us;
static s32 autosave_timer_primed;
static u32 autosave_saved_hash[SETTLE_RANGE_COUNT];
static s32 autosave_saved_hash_valid;

static s32 autosave_state_changed_since_save(void) {
    s32 i;

    if (!autosave_saved_hash_valid) {
        return TRUE;
    }
    for (i = 0; i < SETTLE_RANGE_COUNT; i++) {
        if (settle_hash[i] != autosave_saved_hash[i]) {
            return TRUE;
        }
    }
    return FALSE;
}

/* Call after any save this feature makes, manual or timed, so a timed save does
 * not land moments after the player saved by hand. */
static void autosave_note_committed(u32 now_us, s32 status) {
    s32 i;

    autosave_last_us = now_us;

    if (status == 0) {
        for (i = 0; i < SETTLE_RANGE_COUNT; i++) {
            autosave_saved_hash[i] = settle_hash[i];
        }
        autosave_saved_hash_valid = 1;

        /* Both save paths funnel through here and only a status of 0 committed
         * anything, so this is the one place the "Saved" toast can be raised
         * without it ever lying. */
        recomp_notify_saved();
    }
}

/* ---------------------------------------------------------------------------
 * Per-frame poll.
 * ------------------------------------------------------------------------- */

/* Manual save trigger: L + R + Z, edge-triggered so holding it saves once.
 *
 * Same three buttons as Goemon's, chosen here for a Hybrid Heaven reason rather
 * than by inheritance. The README's input scan established what this game
 * reads: A, B, Z, Start, L, R and the four C-buttons, and **no D-pad bit is
 * read anywhere** -- so the D-pad combos other ports use are unavailable, not
 * merely unfashionable. Of what is left, every C-button drives an action, Start
 * opens the menu, and A/B are attack and guard. L, R and Z are the only three
 * that can be held together without committing the player to anything: R raises
 * the gun (func_801E5010_596010 enters the aim state) but firing needs a
 * separate press, and the save is edge-triggered so the combo resolves in one
 * poll.
 *
 * All three are read from the GAME's button word rather than the host pad, so
 * the combo follows whatever the player has bound and agrees with the state
 * machine that acts on it. Unlike Goemon this needs no physical-trigger
 * special case: HH's analog camera masks C-DOWN, not R (see
 * recomp_set_c_down_suppressed in misc_funcs.h), so none of L/R/Z is ever
 * masked out from under this combo. */
#define AUTOSAVE_COMBO (BTN_L | BTN_R | BTN_Z)

/* Diagnostic events, formatted host-side by recomp_autosave_log. The host does
 * the formatting for the reason recomp_cam_trace does: patch code has no
 * printf, and every one of these wants names rather than numbers. */
#define AUTOSAVE_EV_SAVED_MANUAL  0
#define AUTOSAVE_EV_SAVED_TIMED   1
#define AUTOSAVE_EV_UNSAFE        2
#define AUTOSAVE_EV_UNSETTLED     3
#define AUTOSAVE_EV_SUPPRESSED    4

/* Why an elapsed interval did not produce a save. Reported because the timed
 * path's silence is otherwise ambiguous in exactly the way that matters: a timer
 * that fires and is correctly suppressed, and a timer that fired once and never
 * re-armed, produce the identical empty log. Goemon's own notes record that
 * ambiguity costing three separate debugging cycles on this feature, in three
 * different places -- so the diagnostic that distinguishes them is built in from
 * the start here rather than added after the first confusing run.
 *
 * Rate-limited by REASON rather than by time: the timer retries on every poll
 * once its interval has elapsed, so an unconditional line would be thousands per
 * second. A change of reason is the only thing worth another line. */
#define SUPPRESS_NONE      0
#define SUPPRESS_UNSAFE    1
#define SUPPRESS_MENU      2
#define SUPPRESS_UNSETTLED 3
#define SUPPRESS_NO_CHANGE 4

static s32 autosave_last_suppress;

void update_autosave(void) {
    static s32 combo_was_held = 0;
    u16 held;
    s32 combo_held;
    u32 now_us;
    s32 is_safe;

    if (!recomp_get_autosave_enabled()) {
        /* Off disables the whole feature, not just the timer: this sits above
         * the combo handling, so the manual trigger does nothing either, the
         * settle tracking does not accumulate, and the Saved toast is never
         * seen. A fresh install is entirely inert until the player opts in. */
        combo_was_held = 0;
        settle_primed = 0;
        autosave_timer_primed = 0;
        autosave_last_suppress = SUPPRESS_NONE;
        return;
    }

    now_us = recomp_time_us();

    /* Unconditionally, every poll -- see update_settle_state(). */
    update_settle_state(now_us);

    if (!autosave_timer_primed) {
        autosave_last_us = now_us;
        autosave_timer_primed = 1;
    }

    is_safe = autosave_is_safe();

    held = GS_U16(GS_BUTTONS);
    combo_held = ((held & AUTOSAVE_COMBO) == AUTOSAVE_COMBO);

    if (combo_held && !combo_was_held) {
        if (!is_safe) {
            /* Every guard is reported, not just the ones failing at the time of
             * writing. The failure mode of a wrong constant here is that every
             * save is silently refused -- which looks exactly like a gate
             * working correctly. Goemon's notes record that shape costing three
             * separate debugging cycles. */
            recomp_autosave_log(AUTOSAVE_EV_UNSAFE,
                                (s32)GS_U8(GS_CAM_MODE),
                                (s32)((GS_U8(GS_SCENE_A) << 16) |
                                      (GS_U8(GS_SCENE_B) << 8) |
                                      GS_U8(GS_SCENE_C)),
                                (s32)D_8008DC18_8E818);
        } else if (!autosave_is_settled(now_us)) {
            /* The changed mask, not just "unsettled". A watched range that is
             * continuously volatile would refuse EVERY save while looking
             * exactly like a settled check working correctly; naming which
             * range last moved makes that visible in one test cycle. */
            recomp_autosave_log(AUTOSAVE_EV_UNSETTLED,
                                (s32)settle_changed_mask,
                                (s32)((now_us - settle_stable_since_us) / 1000u),
                                (s32)(SETTLE_US / 1000u));
        } else {
            s32 status = hh_save_now();

            /* Feeds the timer too, so a timed save cannot land moments after
             * the player just saved by hand. */
            autosave_note_committed(now_us, status);
            recomp_autosave_log(AUTOSAVE_EV_SAVED_MANUAL, status,
                                (s32)SAVE_DEVICE, (s32)SAVE_SLOT);
        }
    }

    combo_was_held = combo_held;

    /* The timer. Deliberately does NOT consume the interval when it cannot
     * save -- see the header above; it retries on the next poll instead. */
    if ((u32)(now_us - autosave_last_us) >= AUTOSAVE_INTERVAL_US) {
        s32 suppress;

        /* Don't let a 2-minute boundary commit while the port's settings menu
         * is open: the game ticks with input zeroed underneath the overlay, so
         * a timed save there is coherent but against the gate's intent. The
         * manual combo needs no such check -- its reads already come from the
         * zeroed game button word while the menu is up. */
        if (!is_safe) {
            suppress = SUPPRESS_UNSAFE;
        } else if (recomp_is_config_menu_open()) {
            suppress = SUPPRESS_MENU;
        } else if (!autosave_is_settled(now_us)) {
            suppress = SUPPRESS_UNSETTLED;
        } else if (!autosave_state_changed_since_save()) {
            suppress = SUPPRESS_NO_CHANGE;
        } else {
            suppress = SUPPRESS_NONE;
        }

        if (suppress == SUPPRESS_NONE) {
            s32 status = hh_save_now();

            autosave_note_committed(now_us, status);
            autosave_last_suppress = SUPPRESS_NONE;
            recomp_autosave_log(AUTOSAVE_EV_SAVED_TIMED, status,
                                (s32)SAVE_DEVICE, (s32)SAVE_SLOT);
        } else if (suppress != autosave_last_suppress) {
            /* The mask alone says WHICH range last moved but not whether the
             * window ever elapses, and those are different questions -- a range
             * that moves once a second and one that moves every poll report the
             * same mask while behaving completely differently. Ship the settle
             * age alongside it, capped so it cannot overflow the field. */
            u32 age_ms = (now_us - settle_stable_since_us) / 1000u;

            if (age_ms > 0xFFFFFu) {
                age_ms = 0xFFFFFu;
            }
            autosave_last_suppress = suppress;
            recomp_autosave_log(AUTOSAVE_EV_SUPPRESSED, suppress,
                                (s32)GS_U8(GS_CAM_MODE),
                                (s32)((settle_changed_mask & 0xFF) | (age_ms << 8)));
        }
    }
}
