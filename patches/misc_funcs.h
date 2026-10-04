#ifndef MISC_FUNCS_H
#define MISC_FUNCS_H

#include "patch_helpers.h"

/* Host exports. Every name here must also be given an address in syms.ld, in
 * the manual-patch-symbol range 0x8F000000-0x90000000 that librecomp's
 * `is_manual_patch_symbol` recognises, and must be registered on the host side
 * in src/main/register_patches.cpp. The Makefile fails the link if a
 * `recomp_*` symbol is referenced but missing from syms.ld. */

/* Tells librecomp that the ROM range [rom, rom+size) has been loaded to `ram`,
 * which puts that section's functions into `func_map` and records the section's
 * runtime address for RELOC_HI16/RELOC_LO16. Without it, an indirect call into
 * a freshly loaded overlay hits `get_function`'s "Failed to find function"
 * path and exits. */
DECLARE_FUNC(void, recomp_load_overlays, u32 rom, void *ram, u32 size);

/* Tells librecomp that [ram, ram+size) is about to be overwritten, so every
 * section whose loaded bytes that range touches -- wholly or partly -- stops
 * being callable. The game reuses fixed overlay slots and never announces an
 * eviction, so the loader announces one on its behalf. See
 * `recomp_evict_overlays` in src/game/recomp_api.cpp for why librecomp's own
 * `unload_overlays` cannot be used here. */
DECLARE_FUNC(void, recomp_evict_overlays, void *ram, u32 size);

/* --- Analog camera ------------------------------------------------------- */

/* Microseconds since the runtime started. Free-running; the patch only ever
 * takes differences, so a wrap at 2^32 us (~71 min) costs one clamped frame. */
DECLARE_FUNC(u32, recomp_time_us);

/* Is the Analog Camera setting on? */
DECLARE_FUNC(s32, recomp_get_analog_cam_enabled);

/* Per-axis invert, as two booleans. */
DECLARE_FUNC(void, recomp_get_analog_inverted_axes, s32 *invert_x, s32 *invert_y);

/* Per-axis sensitivity, 0-100 each. 50 is the tuned default rate. */
DECLARE_FUNC(void, recomp_get_analog_cam_sensitivity, s32 *sens_x, s32 *sens_y);

/* Right stick, deadzoned, each axis in [-1, 1]. */
DECLARE_FUNC(void, recomp_get_camera_inputs, f32 *x, f32 *y);

/* R3 held (level, not edge -- the patch edge-detects it, because it can run
 * more than once per frame). */
DECLARE_FUNC(s32, recomp_get_camera_recenter_pressed);

/* Suppress the right stick's C-button bindings while the camera owns it. */
DECLARE_FUNC(void, recomp_set_right_analog_suppressed, s32 suppressed);

/* Where the room wants the camera pointed, against where the patch is holding it.
 * Under HH_TRACE_CAM. */
DECLARE_FUNC(void, recomp_analog_cam_goal, s32 goal_yaw, s32 held_yaw);

/* Framing distance in use and the zoom multiplier in percent, under
 * HH_TRACE_CAM. */
DECLARE_FUNC(void, recomp_analog_cam_zoom, s32 radius, s32 zoom_pct);

/* Physical RB held state -- the zoom modifier. Read directly rather than through
 * the N64 C-down binding it shares, so it still works while C-down is masked out
 * of the game's button word. */
DECLARE_FUNC(s32, recomp_get_camera_zoom_held);

/* Suppress N64 C-DOWN while the camera is engaged. RB is bound to C-down and is
 * also the zoom modifier, so without this every zoom would also fire C-down and
 * throw the player into the game's own close-look camera. Armed only while
 * engaged, so C-down is untouched until the camera is actually being driven.
 *
 * Not the right trigger, where this started: that is N64 R, and R fires the
 * gun. */
DECLARE_FUNC(void, recomp_set_c_down_suppressed, s32 suppressed);

/* Reports a change in whether the analog camera owns the camera, under
 * HH_TRACE_CAM. Called only on the transition, so it costs one call per
 * take-over -- but it is the only thing that can say the patch is running at
 * all on a machine where nobody can watch the screen. */
DECLARE_FUNC(void, recomp_analog_cam_engaged, s32 engaged, s32 yaw, s32 pitch, s32 roll);

/* A/B for anchoring the look-at to the player: 1 under HH_CAM_NO_ANCHOR. */
DECLARE_FUNC(s32, recomp_get_analog_cam_no_anchor);

/* A/B for the roll fix: 0 under HH_CAM_NO_ROLL_FIX, restoring the behaviour
 * that tilted the horizon. */
DECLARE_FUNC(s32, recomp_get_analog_cam_roll_fix);

/* A/B for the framing-distance tracker: 1 under HH_CAM_LEGACY_NORMAL, restoring
 * the asymmetric rise/fall pair that ratcheted the room's framing distance up to
 * the widest goal any writer had recently asked for. */
DECLARE_FUNC(s32, recomp_get_analog_cam_legacy_normal);

/* HH_CAM_FORCE_DIST pins the room's framing distance, 0 for off. Bring-up only:
 * the dynamic pitch cap only binds below ~36 units and nothing reachable in an
 * unattended run frames closer than 98, so this is the only way to test it here. */
DECLARE_FUNC(s32, recomp_get_analog_cam_force_dist);

/* A/B for the movement-basis fix: 1 under HH_CAM_NO_BASIS_FIX, restoring the
 * behaviour where the basis goes stale in any player state that does not rebuild
 * it, inverting movement while the picture stays correct. */
DECLARE_FUNC(s32, recomp_get_analog_cam_no_basis_fix);

/* A/B for the floor pan-out fix: 1 under HH_CAM_LEGACY_TRUST, letting any goal
 * that passes the loose staleness threshold widen the room's framing. */
DECLARE_FUNC(s32, recomp_get_analog_cam_legacy_trust);

/* Reports the game's movement basis and the state of its basis-freeze latch,
 * under HH_TRACE_CAM. `horiz` is the camera's HORIZONTAL distance from its
 * look-at, which is what arms that latch below 10. */
/* librecomp's argument helpers only reach args 0 through 3, so the last four
 * values travel two-to-a-word. All four are small and non-negative: a 13-bit
 * angle, a u8 latch, and two distances in world units. */
DECLARE_FUNC(void, recomp_analog_cam_basis, s32 horiz, s32 basis_yaw,
             s32 latch_and_expected, s32 follow);

/* Reports how far a zero-rotation rebuild of the eye landed from where the eye
 * actually was, under HH_TRACE_CAM. See the host side for why.
 *
 * Both are thousandths of a world unit rather than floats: librecomp's argument
 * helpers only carry a float in the first slot (o32 puts the rest in integer
 * registers), and fixed point is plenty for a deviation that should be zero. */
DECLARE_FUNC(void, recomp_analog_cam_selftest, s32 dev_milli, s32 radius_milli);

/* Reports the PROVENANCE of the camera goal the patch is reading, under
 * HH_TRACE_CAM. The point of it is that the goal's freshness has until now been
 * inferred -- from how far the goal's look-at sits from the player -- while the
 * game has been recording the fact directly all along.
 *
 * `func_8011AAF4_4FA204`, the one setter behind all 386 goal-write sites, stores
 * three bytes about every write before it stores the goal itself (see
 * GS_GOAL_TYPE / _LATCH / _MODE in camera.c). None of them were documented and
 * none were being read. Their meanings are still unknown, so they are reported
 * rather than acted on -- reading an unknown number as an instruction is the
 * mistake this feature has already made three times.
 *
 * The two ages are the other half, and they are what makes the goal-eye/goal-at
 * split visible: the staleness test only ever validated the look-at, so a mode
 * that refreshes the look-at over a leftover eye passes it. Tracking when each
 * half last CHANGED separates them.
 *
 * `packed_bytes` carries the three bytes plus the occlusion flag; ages are in
 * milliseconds, capped; `goal_radius` is instantaneous, and the host accumulates
 * its spread between reports -- which is the reading that says whether the
 * distance tracker should be following a mean, a median or neither. */
DECLARE_FUNC(void, recomp_analog_cam_provenance, s32 packed_bytes, s32 eye_age_ms,
             s32 at_age_ms, s32 goal_radius);

/* Reports THE GAME'S OWN CAMERA MODE STATE MACHINE, under HH_TRACE_CAMMODE.
 *
 * Everything else in this header traces the analog camera. This one traces the
 * game, and it exists because the analog camera has so far been built AGAINST
 * the game's camera rather than around it -- overriding the direction absolutely
 * and re-imposing it every frame, in every situation, whether or not the game
 * would have let a player touch the camera at all.
 *
 * The game does have that notion, and it is not the goal-provenance bytes above.
 * `D_80163740_542E50` is a camera MODE selector taking -1..8, dispatched through
 * jtbl_80185CD8_5653E8 in `func_8010DD4C_4ED45C` -- ten states, each installing
 * its own per-frame handler -- and mirrored to gState+0x29A every frame. The
 * C-button camera is gated on it: `func_8011B254_4FA964` returns that mirror and
 * the entry code in .file_9 requires it to be exactly 2. That is why the camera
 * cannot be moved during a cutscene or on an elevator.
 *
 * Only two arguments, because the host reads the rest out of RDRAM directly at
 * absolute addresses -- gState, the mode selector and the camera task are all at
 * fixed vram in .file_7, and the call site is inside .file_7, so residency is a
 * precondition of the call. Keeping the formatting host-side also means the log
 * can be reshaped without regenerating the patch blob, which is the step that
 * silently does not happen on the Windows build. */
DECLARE_FUNC(void, recomp_cam_trace, void *params, s32 acam_engaged);

/* A/B for the mode gate: 1 under HH_CAM_NO_MODE_GATE, which drives the camera in
 * every one of the game's camera modes -- cutscenes, elevators and battles
 * included -- restoring the behaviour this feature shipped with. */
DECLARE_FUNC(s32, recomp_get_analog_cam_no_mode_gate);

/* A/B for the gun-aim release: 1 under HH_CAM_NO_AIM_RELEASE, which keeps the
 * camera while R is held instead of letting the game frame its own gun shot. */
DECLARE_FUNC(s32, recomp_get_analog_cam_no_aim_release);

/* A/B for the framing-distance fix: 1 under HH_CAM_LEGACY_RADIUS_EASE, which
 * restores the asymmetric radius ease that tracked the goal's valleys and sat at
 * roughly half the distance the game frames at. */
DECLARE_FUNC(s32, recomp_get_analog_cam_legacy_radius_ease);

/* A/B for the framing gain: 1 under HH_CAM_NO_FRAMING_GAIN, which uses the goal
 * radius raw. The goal is not where the game's camera settles -- it trails a
 * moving goal and sits further out -- so following it faithfully framed at about
 * 0.83x the game's own distance. */
DECLARE_FUNC(s32, recomp_get_analog_cam_no_framing_gain);

/* --- Controller Pak ------------------------------------------------------ */

/* HH_PAK_LEGACY_PROBE=1: restore the game's original three-probe slot check,
 * which the port's own PFS_OK-to-everything answers turn into "a Rumble Pak is
 * inserted". This is the A/B for patches/pak.c -- with it set, the save UI puts
 * its message back up. */
DECLARE_FUNC(s32, recomp_get_pak_legacy_probe);

/* What the slot probe decided, under HH_TRACE_PFS. `pfs_err` is osPfsInitPak's
 * return, or -1 when the legacy path short-circuited on a device probe and never
 * called it; `code` is the pak state the game will dispatch on -- 0 is the only
 * value that lets the save layer proceed. */
DECLARE_FUNC(void, recomp_pak_probe_result, s32 channel, s32 pfs_err, s32 code);

/* --- Autosave ------------------------------------------------------------ */

/* Is the Autosave setting On? Gates the whole feature, timer and manual combo
 * alike -- see the early return in update_autosave. */
DECLARE_FUNC(s32, recomp_get_autosave_enabled);

/* Bracket around the autosave's own pak traffic. The host's save-rollback
 * observer maintains a `.manual.bak` from writes it does NOT see bracketed, so
 * this must cover the whole save body and not just the slot write --
 * patches/autosave.c's hh_save_now explains what an unbracketed directory write
 * would do to it. */
DECLARE_FUNC(void, recomp_set_autosave_in_progress, s32 in_progress);

/* Raise the transient "Saved" toast (src/ui/ui_saved_indicator.cpp). Called
 * only on a status-0 save, so it can never claim a save that did not happen. */
DECLARE_FUNC(void, recomp_notify_saved);

/* Is the port's own settings menu open? A 2-minute boundary must not commit
 * underneath it: the game ticks there with input zeroed, so the save would be
 * coherent but against the gate's intent. */
DECLARE_FUNC(s32, recomp_is_config_menu_open);

/* Autosave diagnostics, formatted host-side. `event` selects the line (see the
 * AUTOSAVE_EV_* defines in patches/autosave.c) and a/b/c are its fields; the
 * host names them, because a refusal that prints only numbers is a refusal
 * nobody reads. Always on rather than behind an HH_TRACE_* switch: these fire
 * at most once per save or per combo press, and the one failure this feature
 * can have is refusing silently. */
DECLARE_FUNC(void, recomp_autosave_log, s32 event, s32 a, s32 b, s32 c);

#endif /* MISC_FUNCS_H */
