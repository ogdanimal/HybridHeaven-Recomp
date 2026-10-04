/**
 * Analog camera -- right-stick free look.
 *
 * Full reverse engineering behind this file: docs/hybridheaven-camera-re.md.
 * The short version, because the design follows directly from it:
 *
 *   - The camera's whole state is one parameter block, reached as
 *     `(*(void**)0x801BBCD8)->0x2C`, holding eye, look-at, up and fovy as
 *     floats. There is no separate camera object the game keeps privately.
 *
 *   - Every consumer derives its angles from that block rather than from a
 *     stored yaw -- including `func_8011A878_4F9F88`, which recomputes the
 *     player's MOVEMENT BASIS from `at - eye` every frame. So moving the eye is
 *     enough: walking follows the view natively, with no counter-rotation of the
 *     left stick and no basis rework. Quest 64 has the same property; Goemon 64
 *     does not, which is why its patch is twenty times this size.
 *
 *   - `func_80119F9C_4F96AC(&yaw, &pitch)` is the getter that derives those
 *     angles. The per-frame camera integrator `func_8011A3D4_4F9AE4` calls it
 *     exactly once, AFTER it has finished moving eye and look-at toward their
 *     goals and BEFORE it rebuilds the up vector from the result. That is the
 *     one point in the frame where the camera is final and nothing has consumed
 *     it yet, so this patch replaces that getter: rotate the eye first, then
 *     return the angles of the rotated result. The game's own up-vector step
 *     then keeps the view level for free.
 *
 * WHY THE ROTATION IS AN ABSOLUTE OVERRIDE, NOT AN OFFSET
 *
 * The getter has roughly thirty call sites and can run more than once per
 * frame. An offset ("rotate by delta") would apply once per CALL and turn call
 * count into camera speed. Setting the azimuth and elevation ABSOLUTELY is
 * idempotent -- running it twice in a frame gives the same camera as running it
 * once -- so the patch is safe at any call rate. For the same reason the
 * accumulator advances against wall-clock time rather than per call.
 *
 * That is also what makes it hold its aim: the game keeps easing the camera
 * toward whatever goal the current room or script asked for, and re-imposing
 * our angles every frame means that pull can move the camera's DISTANCE but
 * never its direction. Zelda64Recomp and Goemon reached the same design from the
 * opposite direction, after an added offset visibly drifted back while walking.
 *
 * WHY THERE IS NO COLLISION CODE IN HERE
 *
 * Because the game's goal distance already is it. When something comes between
 * the camera and the player the game pulls its goal in -- measured at 17 units
 * while walking through a doorway, against 35 for the same room clear -- so
 * following that distance faithfully inherits the game's own occlusion handling
 * for free. The camera got caught behind doors only because this file was
 * flooring the distance at 30 and easing it too slowly to follow.
 *
 * A raycast was tried first and abandoned. `func_801084C4_4E7BD4` is the game's
 * own camera probe and its convention is known (arg1 in $f12, arg2 in $f14, args
 * 3-6 in a2/a3/stack, read off the asm at 0x801104DC); called from this hook it
 * returned 0 for every ray tried. Whether that is the hook's phase or the surface
 * filter was never settled -- `func_801084C4_4E7BD4` filters surface types
 * against D_80163570_542C80 while `func_8010843C_4E7B4C` collides with
 * everything, so the "it cannot even hit a floor" test was inconclusive. None of
 * it matters now: the goal distance is the better source anyway, because it is
 * what the game itself decided rather than a second opinion that could disagree.
 *
 * WHAT IS DELIBERATELY LEFT TO THE GAME
 *
 * Only the direction is ours. The framing distance is read live from the camera
 * every frame, so a room that pulls the camera back still pulls it back -- it
 * is only clamped against a runaway (see ACAM_RADIUS_MIN/MAX_SCALE). Hybrid
 * Heaven's own C-button camera states are untouched and still work; this is
 * additive, and with the setting off nothing in this file runs at all.
 */

#include "patches.h"
#include "misc_funcs.h"
#include "autosave.h"

/* ---------------------------------------------------------------------------
 * Game types and symbols.
 * ------------------------------------------------------------------------- */

/* The camera parameter block. Only the fields this patch touches are typed;
 * offsets and the rest of the layout are in the RE doc. */
typedef struct CameraParams {
    /* 0x00 */ char unk_00[0x30];
    /* 0x30 */ f32 eye_x, eye_y, eye_z;
    /* 0x3C */ f32 at_x, at_y, at_z;
    /* 0x48 */ f32 up_x, up_y, up_z;
} CameraParams;

/* The camera task. Its 0x2C field points at the parameter block. */
typedef struct CameraTask {
    /* 0x00 */ char unk_00[0x2C];
    /* 0x2C */ CameraParams *params;
} CameraTask;

/* Pointer to the camera task, in .file_7's bss. Absolute in patches/syms.ld;
 * see the note there for why that is sound for an overlay address. */
extern CameraTask *D_801BBCD8_59B3E8;

/* gState, the gameplay engine's master struct -- 0xF00+ bytes of which this
 * patch understands eight fields. Reached by offset rather than through a padded
 * struct, because a struct would imply the rest is known. Offsets from
 * docs/hybridheaven-camera-re.md. */
extern u8 D_801BBBF0_59B300[];

#define GS_PTR(off) (*(u8 **)(D_801BBBF0_59B300 + (off)))
#define GS_F32(off) (*(f32 *)(D_801BBBF0_59B300 + (off)))
#define GS_S16(off) (*(s16 *)(D_801BBBF0_59B300 + (off)))
#define GS_U8(off)  (*(u8 *)(D_801BBBF0_59B300 + (off)))

#define GS_PLAYER_TASK   0x0E0  /* player task; ->0x2C data has pos at +4/8/C */
#define GS_BASIS_YAW     0x232  /* movement basis, rebuilt from the camera */
#define GS_BASIS_FROZEN  0x234  /* basis-freeze latch */
#define GS_CAM_OWNER     0x2B0  /* task that has claimed the camera, 0 for none */

/* THE GAME'S OWN CAMERA MODE, and the gate this whole file now hangs off.
 *
 * `D_80163740_542E50` holds a camera mode in -1..8. `func_8010DD4C_4ED45C`
 * dispatches on it through jtbl_80185CD8_5653E8 once a frame -- ten states, each
 * installing its own handler -- and mirrors it here, at gState+0x29A. The mirror
 * is read rather than the selector because the mirror is what the GAME's own gate
 * reads: `func_8011B254_4FA964` is three instructions and returns exactly this
 * byte.
 *
 * The scene triple at +0x254..0x256 is the other half of that gate.
 * `func_8011B260_4FA970` returns 0 for scene 2-8-1 and otherwise packs the triple
 * as decimal a*10000 + b*100 + c; zero is the permissive answer. The .file_9 entry
 * code then requires, to enter the ORBIT camera on C-left, that the scene gate be
 * zero AND the mirror be exactly 2.
 *
 * That conjunction is replicated verbatim in acam_mode_allows_player(), because it
 * is the game's answer to the question this feature kept getting wrong: may the
 * player move the camera right now. Measured over a 19-minute play session
 * (34,653 samples): mode 2 during exploration with C-left accepted every time,
 * mode 0 during cutscenes and elevators with C-left refused every time, mode 3
 * for the whole of four battles. The scene triple was 0-0-0 for every sample, so
 * in practice the gate reduces to "mirror == 2" -- but it is written out in full
 * because 2-8-1 exists in the ROM for a reason this session never visited. */
#define GS_CAM_MODE      0x29A  /* mirror of D_80163740_542E50, the camera mode */
#define GS_SCENE_A       0x254
#define GS_SCENE_B       0x255
#define GS_SCENE_C       0x256

/* Held buttons, gState's copy of the primary controller record (+0x9C, so held is
 * at +0x9E). Standard N64 bit values.
 *
 * R (0x0010) is the gun. `func_801E5010_596010` puts the player into the aim
 * state `func_801EFF78_5A0F78` while it is held, and the game frames that shot
 * itself. Read the GAME's button word rather than the host's pad so this follows
 * whatever R is bound to, and so it agrees with the state machine that acts on
 * it. Note this is a different physical control from the analog camera's zoom
 * modifier, which is the right SHOULDER (bound to N64 C-down). */
#define GS_BUTTONS       0x09E
#define BTN_R            0x0010

/* The mode in which the game lets the player drive the camera. */
#define ACAM_PLAYER_MODE 2

/* The camera GOAL the game is easing toward: where it wants the eye and the
 * look-at to be. Note the 12-byte stride -- these are not three contiguous
 * vectors, each component has its own slot. */
#define GS_GOAL_EYE_X    0x2C0
#define GS_GOAL_EYE_Y    0x2CC
#define GS_GOAL_EYE_Z    0x2D8
#define GS_GOAL_AT_X     0x2E8
#define GS_GOAL_AT_Y     0x2F4
#define GS_GOAL_AT_Z     0x300

/* Where the collision raycast leaves its hit point. The game's own camera states
 * read these three immediately after their own probe. Unused here for now -- the
 * raycast was abandoned (see the header) -- and kept because reinstating it is
 * the known fix for the one case the goal distance cannot cover: occlusion along
 * an azimuth the player chose and the game therefore never measured. */
#define GS_HIT_X         0x380
#define GS_HIT_Y         0x384
#define GS_HIT_Z         0x388

/* PROVENANCE of the last goal write, and the reason this patch stopped inferring
 * it. `func_8011AAF4_4FA204` is the single setter behind all 386 goal-write
 * sites, and before it stores a goal it stores three bytes about the write:
 *
 *     8011AB08  andi  $a3, $a3, 0xFF
 *     8011AB38  sb    $a3, 0x2AE($v1)     <- arg4 & 0xFF, on EVERY write
 *     8011AB44  sb    $zero, 0x2B4($v1)   <- cleared when arg5 == 1
 *     8011AB58  sb    $t9, 0x2B5($v1)     <- arg5, and the setter branches on == 2
 *
 * So the game has been recording which kind of write last touched the goal for
 * as long as there has been a goal, and every staleness question this file has
 * answered by measuring distances has had a direct answer sitting in gState.
 *
 * The meanings are NOT known yet, and nothing here acts on them -- they are
 * reported through recomp_analog_cam_provenance and no more. Three of this
 * feature's six shipped bugs came from reading one of the game's own numbers as
 * an instruction when it was stale, temporary, or not what it looked like; a
 * fourth is not being added by guessing at a byte. */
#define GS_GOAL_TYPE     0x2AE
#define GS_GOAL_LATCH    0x2B4
#define GS_GOAL_MODE     0x2B5

/* Trig on the game's 13-bit binary angles: 0x2000 is a full turn. Using the
 * game's own helpers rather than a private implementation keeps the angles this
 * patch produces bit-consistent with the ones every other consumer derives. */
f32 func_8001EAD0_1F6D0(s16 angle);              /* sin */
f32 func_8001EB64_1F764(s16 angle);              /* cos */
s16 func_8001EF38_1FB38(f32 y, f32 x);           /* atan2 -> 13-bit angle */
f32 _nsqrtf(f32 x);                              /* sqrt.s */

/* Back-solves the camera's ROLL: given the view angles and the stored up
 * vector, how far is that up vector rotated about the view axis. */
void func_8011A06C_4F977C(s16 yaw, s16 pitch, f32 up_x, f32 up_y, f32 up_z, s16 *roll_out);

#define ANGLE_MASK  0x1FFF
#define ANGLE_HALF  0x1000

/* ---------------------------------------------------------------------------
 * Tuning.
 * ------------------------------------------------------------------------- */

/* Full-deflection rotation rates at sensitivity 50, in angle units per second.
 * 0x2000 is a full turn, so 3000/s is a little under 2.2 s per revolution --
 * deliberately slower than the game's own C-button camera (~4030/s), which is
 * quick because it is a momentary mode you hold rather than something you live
 * with while walking. */
#define ACAM_YAW_RATE    3000.0f
#define ACAM_PITCH_RATE  1200.0f

/* Elevation limits, absolute -- and note the scale: a QUARTER turn is 0x800, not
 * 0x1000, so 0x600 is 67.5 degrees and anything at or past 0x800 would tip the
 * camera over its own pole and hang it upside down. These are the exact bounds
 * Hybrid Heaven's own orbit camera uses on the same quantity
 * (`func_8011913C_4F884C` clamps its pitch to +-0x600), which is both a check on
 * the arithmetic and the right answer on feel: it is the range the game's
 * artists framed rooms for.
 *
 * The first draft of this file had 0x0F00 here with a comment claiming 67
 * degrees. It is 169. A forced-stick run walked the camera straight past
 * vertical and printed it. */
#define ACAM_PITCH_MAX   0x0600
#define ACAM_PITCH_MIN  (-0x0600)

/* Zoom: a multiplier on the framing distance, driven by the right stick's Y
 * while the zoom modifier (the physical right trigger) is held. 1.0 is the
 * game's own distance for the room, below that dollies in, above it out.
 *
 * This exists because the automatic answer was not good enough on its own. The
 * distance follows the game's goal, and some rooms simply ask for a very tight
 * shot -- the camera arriving there is correct and still not what a player wants
 * from a free camera. Goemon 64: Recompiled reached the same conclusion and put
 * zoom on the same button. */
#define ACAM_ZOOM_MIN   0.40f
#define ACAM_ZOOM_MAX   2.50f
#define ACAM_ZOOM_RATE  1.00f   /* multiplier change per second, full deflection */

/* Absolute bounds on the base framing distance, before zoom.
 *
 * The floor was 30 and that was wrong. THE GAME'S GOAL DISTANCE IS ITS OCCLUSION
 * SOLUTION: when something comes between the camera and the player -- a door
 * being the obvious case -- the game pulls its goal in, and measured in play it
 * pulls in to 17. A floor of 30 made that unreachable, so the camera stayed 46
 * units back and sat behind the door while the game was busy telling us how to
 * avoid exactly that.
 *
 * 16 is just clear of the movement-basis latch, which the game arms below a
 * HORIZONTAL 10 (see ACAM_MIN_HORIZ) -- and the pitch cap keeps the horizontal
 * reach above that even when the camera is jammed this close. */
#define ACAM_DIST_MIN  16.0f
#define ACAM_DIST_MAX  400.0f

/* How far the game's GOAL look-at may sit from the player before the goal is
 * treated as stale and ignored.
 *
 * Not every camera mode uses the goal. Some -- `func_801154A4_4F4BB4` is one --
 * snap the look-at straight onto the player every frame and never write the goal
 * fields at all, which leaves whatever the last mode that did use them put there.
 * Reading a distance out of that is reading a leftover.
 *
 * Measured on a ladder: goal distance 561 while the camera belonged about 50 back,
 * which pinned the base at the 400 ceiling and, multiplied by a 140% zoom the
 * player had set, put the camera 561 units out with the character off screen.
 *
 * A live goal always aims near the player -- the camera's own look-at measured 14
 * to 19 units from it in every follow-mode sample taken. So the distance from
 * goal-look-at to player is the staleness test.
 *
 * 150 is deliberately loose. Reading a stale goal costs a wrong framing distance;
 * wrongly calling a LIVE goal stale costs the door pull-in that depends on it, and
 * that one is worse. The leftovers actually observed were 561 and 630, so a
 * generous threshold still separates them cleanly. The measured distance is in the
 * HH_TRACE_CAM output, so this can be tightened from data rather than from
 * taste. */
#define ACAM_GOAL_TRUST_DIST  150.0f

/* The tighter threshold a goal must ALSO pass before it may widen the room's
 * framing. This is the fix for "the floor makes the camera pan out".
 *
 * The comment above says 150 can be tightened from data rather than taste, and
 * the data arrived. Two runs, independently:
 *
 *   basisfix.log  goal aims 42,  radius 80  steady (mean 80.0 over 60)   -> normal 16 -> 80
 *   gapfix.log    goal aims 100, radius 248 steady (mean 248.0 over 60)  -> normal 226 -> 246
 *
 * Both sail through 150, both are steady rather than transient -- so neither the
 * tracker nor any averaging of it was ever going to help; it was faithfully
 * adopting exactly what the game asked for. And in the second the patch's own
 * anchored look-at sat at 15 from the player while the GOAL's sat at 100: the
 * goal was not framing the player at all, so its radius was never a framing
 * distance. Same mistake as items 1-3 in the header -- a number the game
 * maintains is only an instruction in the situation it is maintained for.
 *
 * Why this is a SECOND threshold instead of a smaller first one: the two
 * directions have opposite failure costs, exactly as the ease rates do. A goal
 * pulling IN is occlusion -- a door, now -- and wrongly distrusting it puts the
 * camera behind the door, the failure the 150 comment is guarding against and
 * the one the user has confirmed fixed. A goal pushing OUT is never urgent, so
 * it can be held to the standard a genuine follow-goal meets anyway: 14 to 19
 * from the player, against our own anchor at 15. 30 clears that with room to
 * spare and rejects 42 and 100 alike.
 *
 * Deliberately NOT given a dwell like ACAM_OCCLUSION_DWELL_US. Both observed
 * cases were steady for 60 samples, so any dwell long enough to be meaningful
 * would simply reinstate the pan-out a couple of seconds later.
 *
 * HH_CAM_LEGACY_TRUST=1 restores the single-threshold behaviour. */
#define ACAM_GOAL_FRAME_DIST  30.0f

/* The room's NORMAL framing distance is tracked separately from the distance in
 * use, and that separation is what makes zoom and occlusion coexist.
 *
 * ONE rate, and it is symmetric. It used to be two -- rise 2.0, fall 0.15 -- on
 * the reasoning that a transient pull-in must not redefine the room's framing.
 * The intent was right and the mechanism was wrong, because a 13:1 asymmetry does
 * not merely resist transients, it RATCHETS: it is a peak detector.
 *
 * With the goal alternating between the 17 and 65 measured in one doorway, the
 * tracker's equilibrium is where the two pulls cancel,
 *
 *     2.00 * (65 - n) + 0.15 * (17 - n) = 0   ->   n = 61.6
 *
 * i.e. within 5% of the HIGH value, against an arithmetic mean of 41. "What this
 * room frames at" was really "the widest thing any goal writer asked for
 * recently", and recovery from a single 65 spike to within 10% of a true 35 took
 * fourteen seconds. Any transient wide goal during a mode handoff panned the
 * camera out and held it there -- which is the shape of the open floor bug.
 *
 * Symmetric, the equilibrium under any flicker is its mean, which is at least a
 * statistic. Whether the mean is the RIGHT statistic is a question about the
 * goal's actual distribution, and that distribution is now in the trace
 * (recomp_analog_cam_provenance) instead of being guessed at. If the excursions
 * turn out to be two-sided -- the 65 looks like a goal writer's overshoot, not a
 * framing -- a median over a time-sampled window is the tool, and it replaces
 * this rate and the freeze below with one mechanism. Do not add a third rate.
 *
 * HH_CAM_LEGACY_NORMAL=1 restores the old asymmetric pair exactly. */
#define ACAM_NORMAL_EASE  2.00f

/* Legacy asymmetric rates, reachable only under HH_CAM_LEGACY_NORMAL. */
#define ACAM_NORMAL_RISE  2.00f
#define ACAM_NORMAL_FALL  0.15f

/* How long a goal below ACAM_OCCLUSION_RATIO is read as occlusion before it is
 * accepted as the room's framing after all.
 *
 * The freeze it gates is not the old slow fall in disguise -- it is the property
 * the slow fall was accidentally providing, stated directly: while something is
 * in the way, the goal distance carries no information about how the room is
 * framed, so it must not move `normal` AT ALL. That matters beyond tidiness,
 * because `want` is `normal * zoom`: if occlusion were allowed to pull `normal`
 * down to 17, a player zoomed to 2.5x would get want = 42 and be pushed straight
 * back behind the door -- the exact failure the header records.
 *
 * The dwell is what stops the freeze deadlocking. A room that genuinely wants a
 * much tighter shot forever would otherwise hold `normal` wide indefinitely,
 * with the cap doing all the work and the player's zoom inoperative. Two seconds
 * is longer than any doorway transit at walking pace and shorter than a room. */
#define ACAM_OCCLUSION_DWELL_US  2000000u

/* How far below normal the game's goal has to drop before it is read as "something
 * is in the way" rather than "this room is framed a little tighter". */
#define ACAM_OCCLUSION_RATIO  0.85f

/* THE CHARACTER IS THE ANCHOR.
 *
 * Everything else in this file orbits the camera's look-at, and for most of the
 * game that is the player -- measured at 14 to 19 units away, all of it the height
 * offset below. But some rooms install a camera that aims somewhere else entirely:
 * a fixed angle for a set piece, an elevator shaft, a corridor shot. The orbit
 * then faithfully circles a point that is not the player, and the camera reads as
 * having stopped tracking you, because it has.
 *
 * So while the camera is ours, the look-at is pulled onto the player too. The
 * offset is the game's own: `func_8011913C_4F884C` aims its orbit camera at
 * player.y + 15.0, which is also why the measured look-at-to-player distance sits
 * at 14 to 19 rather than 0.
 *
 * Eased rather than snapped, because on taking over -- or on a room that had the
 * look-at somewhere far off -- the correction can be large. */
#define ACAM_ANCHOR_HEIGHT  15.0f
#define ACAM_ANCHOR_EASE    8.0f

/* How fast the framing distance eases toward the game's goal, per second --
 * ASYMMETRIC, and the asymmetry is the whole point.
 *
 * Pulling IN has to be quick, because the reason the game pulls in is that
 * something is in the way NOW: a door you are walking through. Easing that
 * gently means spending the whole doorway behind the door.
 *
 * Pushing OUT can afford to be gentle. Nothing is occluded on the way out, so
 * the only requirement is that regaining the room's normal framing reads as a
 * camera move rather than a cut -- which is what the earlier "instantly zooms in
 * on the character" complaint was really about, a symmetric ease fast enough for
 * doors being far too fast for reframing.
 *
 * IN is not instant either: the game's goal flickers (17 and 65 alternating in
 * the same doorway, in the measured log), and following that rigidly would
 * jitter. 6.0 rides the low value without chasing every flick.
 *
 * THERE IS DELIBERATELY NO RATIO CLAMP HERE ANY MORE, and the reason is the
 * history. The distance used to be read back off the LIVE eye, which fed our own
 * rotation into itself: the game eases the eye toward a goal at some other
 * azimuth, we reset the azimuth, and the part of that pull which was sideways
 * came back as a SHORTENING. A 0.5x-2.5x clamp against the take-over distance was
 * meant to contain that, and in play the radius simply walked down and SAT on the
 * floor -- 24.6 against a take-over radius of 49.49, exactly 0.5x, camera stuck
 * close and no longer following the player.
 *
 * The distance now comes from the game's own goal, which our rotation cannot
 * influence, so the loop is gone -- and with it the reason for the clamp. Keeping
 * the clamp would then be actively wrong: it is anchored to whatever the framing
 * happened to be at take-over, so a room that legitimately wants a much wider or
 * tighter shot gets held back. Measured that too: goal radius 248 against a
 * take-over radius of 70, which the clamp would have pinned at 175.
 *
 * AND IT WAS STILL WRONG, IN EXACTLY THE WAY ACAM_NORMAL_EASE DESCRIBES -- only
 * mirrored. That comment establishes that an asymmetric ease does not resist
 * transients, it RATCHETS, and it fixed `normal` by making the rate symmetric.
 * This pair was left at 6.0 in / 1.5 out, a 4:1 asymmetry the other way, which
 * makes the radius a VALLEY detector: with the 17/65 goal flicker measured in one
 * doorway the equilibrium is
 *
 *     6.0 * (17 - r) + 1.5 * (65 - r) = 0   ->   r = 26.6
 *
 * i.e. within 15% of the LOW value, against an arithmetic mean of 41.
 *
 * Measured in play, over two 17-minute sessions with the camera confined to mode
 * 2 (analog cam OFF vs ENGAGED, same modes, so the rooms are comparable):
 *
 *     the GAME frames at   p25 42.4   median 70.4   p75 79.1
 *     this patch framed at p25 17.8   median 38.6   p75 54.6
 *
 * Roughly half the game's distance, with a quarter of all samples pinned within
 * 2 units of the ACAM_DIST_MIN floor. The user's report was "the camera often
 * comes too close behind the character", which is what a valley detector feels
 * like.
 *
 * Symmetric now, and the reason the fast pull-in is not needed is that it was
 * doing the CAP's job a second time: `limit` above already collapses `target` the
 * moment the game's goal says something is in the way, so the ease does not also
 * have to race. Occlusion response is unchanged; what changes is that an ordinary
 * flickering goal no longer drags the camera onto the player.
 *
 * HH_CAM_LEGACY_RADIUS_EASE=1 restores the old 6.0/1.5 pair exactly. */
/* Smoothing on the live/goal ratio, per sample. This runs from the camera getter,
 * which is called of the order of thirty times a frame, so a small alpha is
 * seconds rather than frames -- deliberately slow, because it is a calibration
 * and not a tracker. Bounded to [1.0, 3.0]: below 1.0 would mean the game sits
 * INSIDE its own goal, which no sample showed and which would pull the camera in
 * rather than out; 3.0 is far above the 1.20 measured and exists only so a
 * degenerate goal cannot run it away. */
#define ACAM_GAIN_ALPHA   0.02f
#define ACAM_GAIN_MIN     1.0f
#define ACAM_GAIN_MAX     3.0f

#define ACAM_RADIUS_EASE      3.0f
#define ACAM_RADIUS_EASE_IN   6.0f
#define ACAM_RADIUS_EASE_OUT  1.5f

/* The camera's HORIZONTAL distance from its look-at is never allowed below this.
 *
 * It is not a framing preference, it is a hard edge in the game.
 * `func_8011A878_4F9F88`, which rebuilds the player's movement basis from the
 * camera every frame, arms a latch the moment that horizontal distance drops
 * under 10.0 -- and while the latch is set the basis is FROZEN for as long as
 * the stick is held, so the character keeps travelling along a direction the
 * camera has since left. It is felt as the character pulling somewhere other
 * than where it was pushed.
 *
 * The game can only reach that state by driving its own camera in close. An
 * analog camera reaches it by looking DOWN, because pitch shrinks the horizontal
 * distance by cos(pitch) while leaving the real distance untouched: at the
 * +-0x600 pitch limit the horizontal reach is only 0.38x the radius. Measured at
 * 13 units in an ordinary room, against a threshold of 10 -- so a
 * tighter-framed room crosses it.
 *
 * 14 rather than 10: the game keeps easing the eye toward its own goal between
 * our steps, so the value it tests is not exactly the one we set. */
#define ACAM_MIN_HORIZ  14.0f

/* Stick deflection needed to take the camera over in the first place. Above the
 * host's radial deadzone, so a resting stick can never engage it. */
#define ACAM_ENGAGE_THRESHOLD  0.15f

/* Longest dt one step may integrate, in microseconds. A load, a pause or the
 * 71-minute wrap of recomp_time_us would otherwise arrive as one enormous step
 * and snap the camera round. */
#define ACAM_MAX_DT_US  50000u

/* Below this the eye and the look-at are effectively the same point, the
 * direction is undefined, and there is nothing to rotate. */
#define ACAM_MIN_RADIUS  0.01f

/* ---------------------------------------------------------------------------
 * State.
 * ------------------------------------------------------------------------- */

static s32 g_acam_engaged = 0;    /* the camera's direction is ours */
static f32 g_acam_yaw = 0.0f;     /* absolute azimuth of the EYE offset */
static f32 g_acam_pitch = 0.0f;   /* absolute elevation of the EYE offset */

/* The dynamic elevation cap the framing distance allowed on the previous step.
 * Carried between steps because the cap is computed from the distance, which is
 * only known after the accumulator has already been integrated. Starts at the
 * fixed bound so the first step cannot cap something it has not measured. */
static f32 g_acam_pitch_cap = (f32)ACAM_PITCH_MAX;
static f32 g_acam_radius = 0.0f;  /* base framing distance in use, eased toward
                                   * the game's goal distance */
static f32 g_acam_zoom = 1.0f;    /* player's multiplier on that distance */
/* HH_CAM_NO_ANCHOR=1 leaves the look-at alone, i.e. the behaviour where the
 * camera orbits whatever the room chose to aim at. The A/B for anchoring. */
static s32 g_acam_no_anchor = -1;

/* HH_CAM_LEGACY_NORMAL=1 restores the asymmetric rise/fall tracker that ratchets
 * (see ACAM_NORMAL_EASE). The A/B for the distance fix. */
static s32 g_acam_legacy_normal = -1;

/* HH_CAM_FORCE_DIST pins the room's framing distance, 0 for off. Bring-up only --
 * the dynamic pitch cap is unreachable on this machine without it. */
static s32 g_acam_force_dist = -1;

/* HH_CAM_LEGACY_TRUST=1 lets any goal that passes ACAM_GOAL_TRUST_DIST widen the
 * room's framing, restoring the floor pan-out. The A/B for ACAM_GOAL_FRAME_DIST. */
static s32 g_acam_legacy_trust = -1;

/* HH_CAM_NO_BASIS_FIX=1 stops the patch rebuilding the player's movement basis,
 * restoring the behaviour where movement inverts in any room whose player state
 * does not refresh it. The A/B for the basis fix at the end of acam_apply. */
static s32 g_acam_no_basis_fix = -1;

/* HH_CAM_NO_MODE_GATE=1 drives the camera in EVERY camera mode, restoring the
 * behaviour this feature shipped with -- overriding the direction during
 * cutscenes, elevators and battles, which the game itself never permits. The A/B
 * for the mode gate. */
static s32 g_acam_no_mode_gate = -1;

/* HH_CAM_NO_AIM_RELEASE=1 keeps the camera while the gun is up. The A/B for the
 * aim release. */
static s32 g_acam_no_aim_release = -1;

/* HH_CAM_LEGACY_RADIUS_EASE=1 restores the asymmetric 6.0-in/1.5-out radius ease
 * that tracks the goal's valleys (see ACAM_RADIUS_EASE). The A/B for the framing
 * distance fix. */
static s32 g_acam_legacy_radius_ease = -1;

/* HH_CAM_NO_FRAMING_GAIN=1 uses the goal radius raw, restoring the framing that
 * measured ~0.83x the game's own distance. The A/B for the gain below. */
static s32 g_acam_no_framing_gain = -1;

/* HOW MUCH FURTHER THE GAME SITS THAN ITS OWN GOAL ASKS FOR.
 *
 * The framing distance was read straight off the goal radius, and that was
 * measured to be faithful and still wrong -- the goal is simply not where the
 * game's camera ends up. Over one session, inside camera mode 2 and separated by
 * whether this patch was driving:
 *
 *     goal radius                 median 37.9
 *     our radius, engaged         median 37.9   <- tracking the goal exactly
 *     the GAME's radius, not ours median 45.5   <- where it actually sits
 *
 * The cause is that the game eases its EYE POSITION in three dimensions toward a
 * goal point that keeps moving with the player, so it trails and settles further
 * out than the goal radius. This patch eases a SCALAR radius, which converges
 * precisely onto that radius instead. Following the goal more accurately was
 * therefore the wrong objective; the earlier attempt at this -- making the ease
 * symmetric -- corrected a real bias in how the goal was followed and moved the
 * median by 0.7 units, because the target itself was wrong.
 *
 * So measure the discrepancy rather than assume a constant: the ratio of the
 * game's live radius to its goal radius, sampled ONLY on frames where the camera
 * is not ours. That restriction is what makes it sound. Reading the live radius
 * while we are imposing an azimuth is the feedback loop this file's header
 * forbids -- the game eases the eye toward a goal at another azimuth, we reset
 * the azimuth, and the sideways part of that pull returns as a shortening. While
 * we are NOT driving, there is no loop: the eye is entirely the game's, and its
 * distance is exactly the quantity being asked for.
 *
 * Starts at 1.0, so before any sample has been taken the behaviour is the old
 * one rather than a guess. */
static f32 g_acam_game_gain = 1.0f;

static s32 g_acam_goal_stale = 1; /* the game's camera goal is a leftover */
static f32 g_acam_goal_at_to_player = 0.0f;
static f32 g_acam_goal_radius = 0.0f;  /* distance the game is asking for */
static f32 g_acam_normal = 0.0f;       /* what this room frames at */
static s32 g_acam_occluded = 0;        /* the cap is binding */

/* When the goal was last seen BELOW the occlusion ratio, so the freeze can be
 * released once it has persisted too long to be something in the way. 0 for
 * "not currently below". */
static u32 g_acam_occl_since_us = 0;

/* Independent freshness of the goal's two halves.
 *
 * The staleness test measures |goal_look_at - player|, which validates the
 * LOOK-AT and says nothing about the EYE -- yet the distance comes from the eye.
 * A mode that refreshes the look-at near the player over a leftover eye from an
 * earlier wide shot therefore passes the test with a huge goal radius, and the
 * old ratcheting tracker adopted it within half a second. It was undetectable by
 * construction, so detect it directly: snapshot both halves and record when each
 * last CHANGED. Reported, not yet acted on. */
static f32 g_acam_seen_goal_eye[3];
static f32 g_acam_seen_goal_at[3];
static u32 g_acam_goal_eye_us = 0;
static u32 g_acam_goal_at_us = 0;
static s32 g_acam_goal_seen = 0;  /* the snapshots hold a real sample */
static s32 g_acam_anchored = 0;        /* the look-at was pulled onto the player */
static f32 g_acam_anchor_gap = 0.0f;   /* how far it had drifted */

static u32 g_acam_last_us = 0;

/* The roll the game last back-solved for itself, carried into the trace. It is
 * the quantity that ran away when this was wrong, so it is the one worth
 * watching. */
static s32 g_acam_last_roll = 0;

/* Both accumulators are floats even though the game's angles are 13-bit
 * integers, and that is not cosmetic. A gentle push is a fraction of an angle
 * unit per frame -- a stick at 10% with sensitivity 50 is under half a unit --
 * so integrating in s16 would truncate it to nothing and leave a dead band far
 * wider than any deadzone setting. They are converted to s16 only at the point
 * the game's trig is called. */

static f32 acam_absf(f32 v) {
    return v < 0.0f ? -v : v;
}

/**
 * May the player drive the camera right now -- the GAME's answer, not ours.
 *
 * This is `func_8011B260_4FA970() == 0 && func_8011B254_4FA964() == 2`, the exact
 * conjunction the .file_9 entry code applies before it will put the player into
 * the orbit camera state on a C-left press. Reimplemented here rather than called
 * because both live in .file_7 and one of them is three instructions; what
 * matters is that the predicate is the same one, not that the code is shared.
 *
 * Deliberately the C-LEFT rule and not the C-down rule. C-down's is a disjunction
 * (`gate == 0 || mirror == 2`), which is satisfied by the scene gate alone and so
 * was true even during the cutscenes in the traced session -- one C-down press at
 * 19:08 was accepted in mode 0. C-down is the momentary close-look, though; the
 * orbit camera is the one that is actually a free camera, and its rule is the one
 * that matches what a player means by "can I move the camera here".
 */
static s32 acam_mode_allows_player(void) {
    u32 a = GS_U8(GS_SCENE_A);
    u32 b = GS_U8(GS_SCENE_B);
    u32 c = GS_U8(GS_SCENE_C);
    u32 scene_id;

    /* One scene is special-cased to the permissive answer by the game itself. */
    if (a == 2 && b == 8 && c == 1) {
        scene_id = 0;
    } else {
        scene_id = a * 10000u + b * 100u + c;
    }

    return (scene_id == 0) && ((s8)GS_U8(GS_CAM_MODE) == ACAM_PLAYER_MODE);
}

/* Records when each half of the game's camera goal last CHANGED.
 *
 * Called before the engagement gate rather than alongside the rest of the goal
 * handling, and that placement is the point: the ages have to already be true
 * when the camera is taken over. Tracked from inside the engaged path they would
 * read zero -- "just written", the most trusting answer possible -- at exactly
 * the moment a leftover goal does its damage.
 *
 * Exact float compares, deliberately. The question is not "did this move much",
 * it is "did the setter run", and the setter stores whatever it stores; a
 * tolerance would silently classify a slow deliberate pan as no write at all. */
static void acam_track_goal_writes(u32 now) {
    f32 ex = GS_F32(GS_GOAL_EYE_X);
    f32 ey = GS_F32(GS_GOAL_EYE_Y);
    f32 ez = GS_F32(GS_GOAL_EYE_Z);
    f32 ax = GS_F32(GS_GOAL_AT_X);
    f32 ay = GS_F32(GS_GOAL_AT_Y);
    f32 az = GS_F32(GS_GOAL_AT_Z);

    if (!g_acam_goal_seen) {
        g_acam_goal_seen = 1;
        g_acam_goal_eye_us = now;
        g_acam_goal_at_us = now;
    } else {
        if (ex != g_acam_seen_goal_eye[0] ||
            ey != g_acam_seen_goal_eye[1] ||
            ez != g_acam_seen_goal_eye[2]) {
            g_acam_goal_eye_us = now;
        }
        if (ax != g_acam_seen_goal_at[0] ||
            ay != g_acam_seen_goal_at[1] ||
            az != g_acam_seen_goal_at[2]) {
            g_acam_goal_at_us = now;
        }
    }

    g_acam_seen_goal_eye[0] = ex;
    g_acam_seen_goal_eye[1] = ey;
    g_acam_seen_goal_eye[2] = ez;
    g_acam_seen_goal_at[0] = ax;
    g_acam_seen_goal_at[1] = ay;
    g_acam_seen_goal_at[2] = az;
}

/* The disengage transition: the camera's direction goes back to the game.
 *
 * Shared by the three paths that give the camera back, which used to carry three
 * copies of it. Whether the right stick is ALSO given back is deliberately not
 * decided here, and that asymmetry is the design rather than the drift it looks
 * like: the stick belongs to the analog camera for as long as the FEATURE is on.
 * R3 and a degenerate pose hand back the camera while leaving the feature on, so
 * the stick stays claimed and ready to re-take -- handing it to the C-buttons
 * there would make the next push drive the game's camera instead of ours. Only
 * acam_release, which runs when the setting itself is off, gives it back. */
static void acam_disengage(void) {
    if (!g_acam_engaged) {
        return;
    }
    recomp_analog_cam_engaged(0, (s32)g_acam_yaw, (s32)g_acam_pitch, g_acam_last_roll);
    g_acam_engaged = 0;
    g_acam_zoom = 1.0f;
    recomp_set_c_down_suppressed(0);
}

/* Hands back the camera AND the right stick. Called only when the setting is off,
 * which is why it is the one path that drops both leases. */
static void acam_release(void) {
    acam_disengage();
    /* Unconditional, unlike the disengage above: the feature is off, so both
     * leases go whether or not the camera was ever ours. */
    g_acam_zoom = 1.0f;
    recomp_set_c_down_suppressed(0);
    recomp_set_right_analog_suppressed(0);
}

/**
 * Applies the analog camera to the parameter block, in place.
 *
 * Idempotent within a frame: it sets the eye's azimuth and elevation to the
 * accumulator's absolute values rather than adding a delta, and preserves the
 * radius it read. Only the time-based accumulation is rate-dependent, and that
 * is measured against the clock rather than counted per call.
 */
static void acam_apply(CameraParams *cam) {
    f32 stick_x, stick_y;
    f32 dx, dy, dz;
    f32 horiz, radius;
    f32 scaled_x, scaled_y, dt_s, raw_y, pitch_before;
    f32 cos_pitch, dir_x, dir_y, dir_z;
    s16 yaw_a, pitch_a;
    s32 invert_x, invert_y;
    s32 sens_x, sens_y;
    u32 now, dt;

    if (g_acam_no_anchor < 0) {
        g_acam_no_anchor = recomp_get_analog_cam_no_anchor();
    }
    if (g_acam_legacy_normal < 0) {
        g_acam_legacy_normal = recomp_get_analog_cam_legacy_normal();
    }
    if (g_acam_force_dist < 0) {
        g_acam_force_dist = recomp_get_analog_cam_force_dist();
    }
    if (g_acam_no_basis_fix < 0) {
        g_acam_no_basis_fix = recomp_get_analog_cam_no_basis_fix();
    }
    if (g_acam_legacy_trust < 0) {
        g_acam_legacy_trust = recomp_get_analog_cam_legacy_trust();
    }
    if (g_acam_no_mode_gate < 0) {
        g_acam_no_mode_gate = recomp_get_analog_cam_no_mode_gate();
    }
    if (g_acam_no_aim_release < 0) {
        g_acam_no_aim_release = recomp_get_analog_cam_no_aim_release();
    }
    if (g_acam_legacy_radius_ease < 0) {
        g_acam_legacy_radius_ease = recomp_get_analog_cam_legacy_radius_ease();
    }
    if (g_acam_no_framing_gain < 0) {
        g_acam_no_framing_gain = recomp_get_analog_cam_no_framing_gain();
    }

    if (!recomp_get_analog_cam_enabled()) {
        acam_release();
        return;
    }

    /* One clock read for the whole step, taken before anything branches on it.
     * It used to be read twice -- once to seed the accumulator on take-over and
     * again to measure dt -- which made the first dt a measurement of the code
     * between the two calls rather than of a frame. */
    now = recomp_time_us();

    /* Goal freshness is tracked whether or not the camera is ours, so the ages
     * are already meaningful at take-over. */
    acam_track_goal_writes(now);

    /* THE MODE GATE: if the game would not let a player move the camera, neither
     * do we, and we hand back everything -- camera and stick both.
     *
     * This is the inversion the rest of this file was written without. Every
     * other mechanism in here re-imposes our direction unconditionally and then
     * tries to cope with the consequences: the goal-staleness test, the two trust
     * thresholds, the occlusion dwell, the anchor. Most of those exist because the
     * camera was being driven in situations the game had already reserved for
     * itself -- a fixed-angle set piece, a ladder, an elevator, a cutscene -- and
     * the leftovers they were built to reject (goal radius 561 on a ladder, 630 in
     * attract mode) are readings taken from exactly those situations.
     *
     * Full release rather than a bare disengage, so a mode the analog camera has
     * no business in behaves precisely as if the feature were switched off,
     * including giving the right stick back to its C-button bindings. Re-entering
     * mode 2 then takes the camera over again from wherever the game left it,
     * through the ordinary engage path, which captures the live pose and so cannot
     * jump.
     *
     * Verified against a 19-minute session before it was written: C-left was
     * accepted on all 5 presses inside mode 2 and refused on all 5 inside mode 0,
     * and the four battles were mode 3 throughout. */
    if (!g_acam_no_mode_gate && !acam_mode_allows_player()) {
        acam_release();
        return;
    }

    /* THE GUN. R puts the player into the aim state and the game frames that shot
     * for itself; holding our own direction across it is the same mistake as
     * holding it across a cutscene, one state further down. So R closes the gate
     * exactly as a non-player camera mode does -- full release, camera and stick
     * both, and the ordinary engage path takes it back from the live pose when R
     * comes up.
     *
     * Gated on the game's own held-button word rather than on the player state
     * pointer, because the state is entered from two different .file_9 sites and
     * the button is the thing both of them test.
     *
     * HH_CAM_NO_AIM_RELEASE=1 keeps the camera while aiming, restoring the
     * behaviour where the right stick overrides the gun camera. */
    if (!g_acam_no_aim_release && (GS_S16(GS_BUTTONS) & BTN_R) != 0) {
        acam_release();
        return;
    }

    /* Take the right stick away from its C-button bindings. The host side treats
     * this as a short lease rather than a latch, so it has to be renewed on every
     * step -- which is the point: if this function ever stops being called, the
     * C-buttons come back by themselves instead of staying dead. */
    recomp_set_right_analog_suppressed(1);

    /* R3 hands the camera back. Held, not edge-triggered: an edge would release
     * the camera and then let the very next step re-take it from a stick that is
     * still pushed, so R3 would do nothing at all in the one situation anyone
     * presses it -- mid-look. Holding it keeps handing back; letting go lets you
     * take it again. */
    if (recomp_get_camera_recenter_pressed()) {
        acam_disengage();
        return;
    }

    dx = cam->eye_x - cam->at_x;
    dy = cam->eye_y - cam->at_y;
    dz = cam->eye_z - cam->at_z;

    horiz = _nsqrtf(dx * dx + dz * dz);
    radius = _nsqrtf(horiz * horiz + dy * dy);
    if (radius < ACAM_MIN_RADIUS || horiz < ACAM_MIN_RADIUS) {
        /* Degenerate pose -- a cut mid-flight, or a camera sitting on top of its
         * own target. Leave it alone and drop the engagement so re-engaging
         * captures a sane direction rather than a meaningless one. */
        acam_disengage();
        return;
    }

    /* Calibrate against the game while the camera is still the game's.
     *
     * Only while disengaged, which is the whole basis of it -- see
     * g_acam_game_gain. `radius` here is the live camera the game placed, and
     * nothing of ours has touched it on this path. */
    if (!g_acam_engaged) {
        f32 ggx = GS_F32(GS_GOAL_EYE_X) - GS_F32(GS_GOAL_AT_X);
        f32 ggy = GS_F32(GS_GOAL_EYE_Y) - GS_F32(GS_GOAL_AT_Y);
        f32 ggz = GS_F32(GS_GOAL_EYE_Z) - GS_F32(GS_GOAL_AT_Z);
        f32 goal_r = _nsqrtf(ggx * ggx + ggy * ggy + ggz * ggz);

        if (goal_r > 1.0f && radius > 1.0f) {
            f32 ratio = radius / goal_r;
            if (ratio < ACAM_GAIN_MIN) {
                ratio = ACAM_GAIN_MIN;
            }
            if (ratio > ACAM_GAIN_MAX) {
                ratio = ACAM_GAIN_MAX;
            }
            g_acam_game_gain += (ratio - g_acam_game_gain) * ACAM_GAIN_ALPHA;
        }
    }

    recomp_get_camera_inputs(&stick_x, &stick_y);
    recomp_get_analog_inverted_axes(&invert_x, &invert_y);
    recomp_get_analog_cam_sensitivity(&sens_x, &sens_y);

    /* Zoom uses the stick BEFORE the invert settings. Invert Y is about which way
     * the view tilts; reversing which way the camera dollies along with it would
     * be a surprise, not a preference. */
    raw_y = stick_y;

    if (invert_x) {
        stick_x = -stick_x;
    }
    if (invert_y) {
        stick_y = -stick_y;
    }

    if (!g_acam_engaged) {
        /* Not ours yet: only a deliberate push takes the camera over, and until
         * it does the game's camera is untouched. Capture where the game has it
         * so taking over never jumps. */
        if (acam_absf(stick_x) < ACAM_ENGAGE_THRESHOLD &&
            acam_absf(stick_y) < ACAM_ENGAGE_THRESHOLD) {
            return;
        }
        {
            s16 captured_pitch = func_8001EF38_1FB38(dy, horiz) & ANGLE_MASK;
            if (captured_pitch & ANGLE_HALF) {
                captured_pitch -= 0x2000;
            }
            g_acam_engaged = 1;
            g_acam_yaw = (f32)(func_8001EF38_1FB38(dz, dx) & ANGLE_MASK);
            g_acam_pitch = (f32)captured_pitch;
            g_acam_radius = radius;
            g_acam_normal = radius;
            g_acam_zoom = 1.0f;
            g_acam_last_us = now;
            g_acam_occl_since_us = 0;
            recomp_analog_cam_engaged(1, (s32)g_acam_yaw, (s32)g_acam_pitch, g_acam_last_roll);

            /* Rebuild the eye from exactly what was just captured, with no
             * rotation applied, and report the difference. It should be the
             * game's own trig rounding and nothing more. Anything larger means
             * the decompose and the recompose disagree about a convention, which
             * would otherwise show up only as the camera jumping the instant
             * anyone touched the stick -- on a machine that has no stick. */
            {
                s16 cy = (s16)(s32)g_acam_yaw;
                f32 cp = func_8001EB64_1F764(captured_pitch);
                f32 ex = (cam->at_x + radius * cp * func_8001EB64_1F764(cy)) - cam->eye_x;
                f32 ey = (cam->at_y + radius * func_8001EAD0_1F6D0(captured_pitch)) - cam->eye_y;
                f32 ez = (cam->at_z + radius * cp * func_8001EAD0_1F6D0(cy)) - cam->eye_z;
                recomp_analog_cam_selftest(
                    (s32)(_nsqrtf(ex * ex + ey * ey + ez * ez) * 1000.0f),
                    (s32)(radius * 1000.0f));
            }
        }
    }

    dt = now - g_acam_last_us;
    g_acam_last_us = now;
    if (dt > ACAM_MAX_DT_US) {
        dt = ACAM_MAX_DT_US;
    }

    /* Sign-preserving quadratic response: fine aim near centre, full rate at the
     * rim. Sensitivity is a plain multiplier around the tuned default of 50. */
    scaled_x = stick_x * acam_absf(stick_x) * (ACAM_YAW_RATE * (f32)sens_x / 50.0f);
    scaled_y = stick_y * acam_absf(stick_y) * (ACAM_PITCH_RATE * (f32)sens_y / 50.0f);

    dt_s = (f32)dt * (1.0f / 1000000.0f);

    /* Yaw sign matches the game's own camera control: `func_80118038_4F7748`
     * adds a positive stick X to its forward yaw, and the eye's azimuth differs
     * from the forward yaw by exactly half a turn, so the same sign here gives
     * the same direction of travel a C-button camera does. */
    g_acam_yaw += scaled_x * dt_s;
    while (g_acam_yaw >= 8192.0f) {
        g_acam_yaw -= 8192.0f;
    }
    while (g_acam_yaw < 0.0f) {
        g_acam_yaw += 8192.0f;
    }

    /* The zoom modifier reassigns the stick's Y axis for as long as it is held:
     * push up to come in, down to pull out. Pitch is left exactly where it was,
     * so letting go of the modifier resumes tilting from the same angle.
     *
     * The modifier is the PHYSICAL right shoulder button, read directly rather
     * than through the N64 C-down binding it shares -- because C-down is masked
     * out of the game's button word while the camera is engaged (see
     * recomp_set_c_down_suppressed), and going through the binding would read the
     * mask rather than the button. Without that masking, zooming would also fire
     * C-down, which throws the player into the game's own close-look camera. */
    if (recomp_get_camera_zoom_held()) {
        recomp_set_c_down_suppressed(1);
        /* SDL's stick Y is positive DOWNWARD, so adding it means pushing UP
         * reduces the multiplier and brings the camera IN -- push toward the
         * character to get closer to it. */
        g_acam_zoom += raw_y * acam_absf(raw_y) * ACAM_ZOOM_RATE * dt_s;
        if (g_acam_zoom < ACAM_ZOOM_MIN) {
            g_acam_zoom = ACAM_ZOOM_MIN;
        }
        if (g_acam_zoom > ACAM_ZOOM_MAX) {
            g_acam_zoom = ACAM_ZOOM_MAX;
        }
        scaled_y = 0.0f;
    } else {
        /* Claim C-down for as long as the camera is ours, so the game never sees
         * a press that was meant for the camera. The host treats it as a short
         * lease, renewed here, so C-down comes back on its own the moment this
         * stops running. */
        recomp_set_c_down_suppressed(1);
    }

    /* SDL reports the right stick's Y as positive DOWNWARD, so subtracting
     * raises the camera when the stick is pushed up -- it looks down at Diaz,
     * which is the un-inverted convention. Invert Y flips it. */
    pitch_before = g_acam_pitch;
    g_acam_pitch -= scaled_y * dt_s;
    if (g_acam_pitch > (f32)ACAM_PITCH_MAX) {
        g_acam_pitch = (f32)ACAM_PITCH_MAX;
    }
    if (g_acam_pitch < (f32)ACAM_PITCH_MIN) {
        g_acam_pitch = (f32)ACAM_PITCH_MIN;
    }

    /* Hold the accumulator against LAST step's dynamic cap, in one direction
     * only: it may keep whatever it already had, but it must not grow past the
     * cap.
     *
     * Both halves matter and they used to be one destructive clamp further down,
     * which got this wrong in both directions at once. Clamping the accumulator
     * DESTROYED pitch the player had set -- zoom in tight enough and the cap
     * falls to about 29 degrees, taking any steeper angle with it permanently, so
     * zooming back out left the camera inexplicably level. Not clamping it at all
     * would bank angle the player cannot see: a held stick reaches the fixed
     * +-0x600 bound in a second and a third, and the whole 38 degrees of it would
     * then arrive as a swing the instant the camera zoomed out.
     *
     * Last step's cap rather than this step's, because this step's depends on the
     * framing distance that has not been computed yet. It moves with the zoom, so
     * it is a step stale at most. */
    if (g_acam_pitch > g_acam_pitch_cap && g_acam_pitch > pitch_before) {
        g_acam_pitch = (pitch_before > g_acam_pitch_cap) ? pitch_before : g_acam_pitch_cap;
    }
    if (g_acam_pitch < -g_acam_pitch_cap && g_acam_pitch < pitch_before) {
        g_acam_pitch = (pitch_before < -g_acam_pitch_cap) ? pitch_before : -g_acam_pitch_cap;
    }

    /* Is the game's camera goal live, or a leftover from a mode that stopped
     * writing it? Answered from how far the goal's look-at is from the player. */
    g_acam_goal_stale = 1;
    {
        f32 gx = GS_F32(GS_GOAL_EYE_X) - GS_F32(GS_GOAL_AT_X);
        f32 gy = GS_F32(GS_GOAL_EYE_Y) - GS_F32(GS_GOAL_AT_Y);
        f32 gz = GS_F32(GS_GOAL_EYE_Z) - GS_F32(GS_GOAL_AT_Z);
        u8 *ptask = GS_PTR(GS_PLAYER_TASK);

        g_acam_goal_radius = _nsqrtf(gx * gx + gy * gy + gz * gz);

        if (ptask != NULL) {
            u8 *pdata = *(u8 **)(ptask + 0x2C);
            if (pdata != NULL) {
                f32 ax = GS_F32(GS_GOAL_AT_X) - *(f32 *)(pdata + 0x4);
                f32 ay = GS_F32(GS_GOAL_AT_Y) - *(f32 *)(pdata + 0x8);
                f32 az = GS_F32(GS_GOAL_AT_Z) - *(f32 *)(pdata + 0xC);
                g_acam_goal_at_to_player = _nsqrtf(ax * ax + ay * ay + az * az);
                if (g_acam_goal_at_to_player < ACAM_GOAL_TRUST_DIST &&
                    g_acam_goal_radius > 1.0f) {
                    g_acam_goal_stale = 0;
                }
            }
        }
    }

    /* Distance.
     *
     * Three quantities, and keeping them apart is the whole design:
     *
     *   normal  -- what this room frames at, tracked slowly so a doorway cannot
     *              redefine it (ACAM_NORMAL_RISE / _FALL).
     *   want    -- normal scaled by the player's zoom. What was asked for.
     *   limit   -- a CAP, not a distance: when the game pulls its own goal well
     *              below normal it is telling us something is in the way, and the
     *              camera must not be further out than that no matter what zoom
     *              says.
     *
     * The first version conflated limit and want -- it made the occlusion pull-in
     * the framing distance itself. Two failures followed from that one mistake.
     * The camera came out of a doorway still tight, because the pull-in had become
     * the distance; and a player who zoomed out to compensate pushed the camera
     * straight back behind the door, because zoom was multiplying the very number
     * that was holding it clear. Measured: base pinned at the 16 floor with zoom
     * at 208%, in front of a door the camera was stuck behind.
     *
     * As a cap it cannot be zoomed past, and it lifts by itself when the doorway
     * is behind you -- which is what "pull back out to where it was" needs. */
    {
        f32 want;
        f32 limit = 1.0e9f;
        f32 target;

        if (!g_acam_goal_stale) {
            /* Scaled to where the GAME would have ended up, not to what its goal
             * asks for -- the two differ by the lag measured in g_acam_game_gain,
             * and following the goal raw is what framed the camera at ~0.83x the
             * game's distance and read as "too close behind the character". */
            f32 goal_radius = g_acam_no_framing_gain
                                  ? g_acam_goal_radius
                                  : g_acam_goal_radius * g_acam_game_gain;
            s32 below = (goal_radius < g_acam_normal * ACAM_OCCLUSION_RATIO);

            if (g_acam_legacy_normal) {
                /* The old asymmetric tracker, kept reachable so the change can be
                 * A/B'd. It ratchets -- see ACAM_NORMAL_EASE for the arithmetic. */
                if (goal_radius > g_acam_normal) {
                    g_acam_normal += (goal_radius - g_acam_normal) * ACAM_NORMAL_RISE * dt_s;
                } else {
                    g_acam_normal += (goal_radius - g_acam_normal) * ACAM_NORMAL_FALL * dt_s;
                }
                if (below) {
                    limit = goal_radius;
                }
            } else {
                /* Classify FIRST, then track. The old code let the rate asymmetry
                 * do the classifying, which is what made it a peak detector.
                 *
                 * Below the room's baseline, the goal is occlusion and carries no
                 * information about how the room is framed, so it does not move
                 * `normal` at all -- it only caps. Once it has stayed there longer
                 * than any doorway, it stops being occlusion and is adopted, which
                 * is what keeps the freeze from deadlocking a genuinely tight room
                 * with the player's zoom inoperative. */
                if (below) {
                    if (g_acam_occl_since_us == 0) {
                        g_acam_occl_since_us = now;
                    }
                } else {
                    g_acam_occl_since_us = 0;
                }

                if (below && (u32)(now - g_acam_occl_since_us) < ACAM_OCCLUSION_DWELL_US) {
                    limit = goal_radius;
                } else if (goal_radius > g_acam_normal && !g_acam_legacy_trust &&
                           g_acam_goal_at_to_player > ACAM_GOAL_FRAME_DIST) {
                    /* Asking to widen the framing, from a look-at that is not on
                     * the player. That radius is not a framing distance -- see
                     * ACAM_GOAL_FRAME_DIST. Leave `normal` where it is: no cap
                     * either, because nothing is in the way. */
                } else {
                    g_acam_normal += (goal_radius - g_acam_normal) * ACAM_NORMAL_EASE * dt_s;
                }
            }
        }

        if (g_acam_normal < ACAM_DIST_MIN) {
            /* Clamp to the floor and nothing else. This used to seed the tracker
             * from `radius` -- the LIVE measured eye distance, which is mostly this
             * patch's own previous output -- and that is the one pattern the whole
             * distance design exists to avoid: it is how the radius came to walk
             * down and sit on its floor with the camera stuck close. The header
             * states the rule ("never read the framing distance back off the live
             * eye"); this line was the last place still breaking it. */
            g_acam_normal = ACAM_DIST_MIN;
        }
        if (g_acam_normal > ACAM_DIST_MAX) {
            g_acam_normal = ACAM_DIST_MAX;
        }

        /* Bring-up override, after the tracker so the trace still shows what the
         * tracker would have said. */
        if (g_acam_force_dist > 0) {
            g_acam_normal = (f32)g_acam_force_dist;
        }

        want = g_acam_normal * g_acam_zoom;
        target = (limit < want) ? limit : want;

        if (target < ACAM_DIST_MIN) {
            target = ACAM_DIST_MIN;
        }
        if (target > ACAM_DIST_MAX) {
            target = ACAM_DIST_MAX;
        }

        {
            f32 ease = ACAM_RADIUS_EASE;
            if (g_acam_legacy_radius_ease) {
                ease = (target < g_acam_radius) ? ACAM_RADIUS_EASE_IN
                                                : ACAM_RADIUS_EASE_OUT;
            }
            g_acam_radius += (target - g_acam_radius) * ease * dt_s;
        }
        if (g_acam_radius < ACAM_DIST_MIN) {
            g_acam_radius = ACAM_DIST_MIN;
        }
        if (g_acam_radius > ACAM_DIST_MAX) {
            g_acam_radius = ACAM_DIST_MAX;
        }

        g_acam_occluded = (limit < want) ? 1 : 0;
    }

    /* Zoom is already folded in, via `want`. Applying it again here is what let a
     * ceiling-pinned base and a 140% zoom put the ladder camera 561 units out.
     *
     * No ACAM_MIN_HORIZ floor here any more: g_acam_radius is already clamped to
     * ACAM_DIST_MIN, which is 16 against this threshold's 14, so the floor could
     * never bind -- and neither could the `radius <= ACAM_MIN_HORIZ` branch of the
     * pitch cap below, which had therefore never once executed. Both were read as
     * live safety nets while being unreachable. The real floor is ACAM_DIST_MIN. */
    radius = g_acam_radius;

    /* Second pitch limit, this one derived from the framing distance IN USE
     * -- after the room's distance and the player's zoom, which is why it sits
     * here and not up with the fixed limit. It is the steepest angle whose
     * horizontal reach still clears ACAM_MIN_HORIZ. Closer framing therefore
     * allows less pitch, which is what keeps the movement-basis latch out of reach
     * in every room rather than only in roomy ones. Solved with atan2 rather than
     * an arccos the game does not have: the angle whose cosine is h/r is
     * atan2(sqrt(r*r - h*h), h).
     *
     * Applied to `pitch_a`, the angle used THIS FRAME, and no longer written back
     * into the accumulator -- see where g_acam_pitch_cap is enforced during
     * integration for why a destructive clamp here was losing the player's aim. */
    {
        f32 applied_pitch = g_acam_pitch;

        g_acam_pitch_cap = (f32)(func_8001EF38_1FB38(
            _nsqrtf(radius * radius - ACAM_MIN_HORIZ * ACAM_MIN_HORIZ),
            ACAM_MIN_HORIZ) & ANGLE_MASK);
        if (g_acam_pitch_cap > (f32)ACAM_PITCH_MAX) {
            g_acam_pitch_cap = (f32)ACAM_PITCH_MAX;
        }

        if (applied_pitch > g_acam_pitch_cap) {
            applied_pitch = g_acam_pitch_cap;
        }
        if (applied_pitch < -g_acam_pitch_cap) {
            applied_pitch = -g_acam_pitch_cap;
        }

        pitch_a = (s16)(s32)applied_pitch;
    }

    yaw_a = (s16)(s32)g_acam_yaw;

    /* "Still holding it" -- the host drops all but one a second, and all of them
     * when HH_TRACE_CAM is off. */
    recomp_analog_cam_engaged(2, yaw_a, pitch_a, g_acam_last_roll);

    /* The camera's HORIZONTAL reach, and what the game has done with it. Pitch
     * shrinks this by cos(pitch) while leaving the real distance alone, and the
     * game freezes the player's movement basis whenever it falls under 10 --
     * see recomp_analog_cam_basis on the host side. */
    {
        /* How far the look-at has drifted from the player, and what distance the
         * game is currently asking for. Together these separate "the orbit is
         * wrong" from "the thing being orbited is not the player any more". */
        u8 *player = GS_PTR(GS_PLAYER_TASK);
        s32 at_to_player = -1;
        f32 goal_radius;
        f32 gx, gy, gz;

        if (player != NULL) {
            u8 *pdata = *(u8 **)(player + 0x2C);
            if (pdata != NULL) {
                f32 px = cam->at_x - *(f32 *)(pdata + 0x4);
                f32 py = cam->at_y - *(f32 *)(pdata + 0x8);
                f32 pz = cam->at_z - *(f32 *)(pdata + 0xC);
                at_to_player = (s32)_nsqrtf(px * px + py * py + pz * pz);
            }
        }

        gx = GS_F32(GS_GOAL_EYE_X) - GS_F32(GS_GOAL_AT_X);
        gy = GS_F32(GS_GOAL_EYE_Y) - GS_F32(GS_GOAL_AT_Y);
        gz = GS_F32(GS_GOAL_EYE_Z) - GS_F32(GS_GOAL_AT_Z);
        goal_radius = _nsqrtf(gx * gx + gy * gy + gz * gz);

        /* Two values per word -- the host helpers only reach four arguments.
         * `expected` is what the basis SHOULD be: the camera's forward azimuth,
         * half a turn from the eye-offset azimuth this patch imposes. */
        if (at_to_player < 0 || at_to_player > 0xFFFF) {
            at_to_player = 0xFFFF;
        }
        if (goal_radius < 0.0f || goal_radius > 65535.0f) {
            goal_radius = 65535.0f;
        }

        recomp_analog_cam_basis(
            (s32)(radius * func_8001EB64_1F764(pitch_a)),
            GS_S16(GS_BASIS_YAW),
            (((s32)GS_U8(GS_BASIS_FROZEN)) << 16) | (((yaw_a + 0x1000) & ANGLE_MASK)),
            (at_to_player << 16) | ((s32)goal_radius & 0xFFFF));

        /* How far the ROOM wants to point the camera, against where we are
         * holding it. This is the reading that a walk through a doorway settles.
         *
         * The camera getting caught behind a door has two candidate causes and
         * they need opposite fixes. Either the door is simply solid and the fix is
         * collision -- which needs a raycast that does not run from here -- or the
         * room reframes as you pass through, its goal azimuth swings, and holding
         * ours absolutely is what strands the camera on the wrong side. The second
         * is fixable without any collision at all: yield the azimuth when the game
         * insists on a different one for long enough.
         *
         * A big, sustained divergence appearing exactly as a door is crossed says
         * it is the second. A divergence that stays near zero while the camera is
         * visibly stuck says it is the first. */
        {
            f32 fx = GS_F32(GS_GOAL_AT_X) - GS_F32(GS_GOAL_EYE_X);
            f32 fz = GS_F32(GS_GOAL_AT_Z) - GS_F32(GS_GOAL_EYE_Z);
            s32 goal_yaw = func_8001EF38_1FB38(fz, fx) & ANGLE_MASK;

            recomp_analog_cam_goal(goal_yaw, (yaw_a + 0x1000) & ANGLE_MASK);
        }

        /* Distance in use and the player's zoom, so "the camera zoomed in" is a
         * number rather than an impression. */
        {
            /* 14 bits, NOT 15. At 15 the field ran bits 12-26 and collided with
             * the `owned` flag at bit 26: every owned sample read back 16384 too
             * high, and a whole task-owned stretch was recorded as "the goal aims
             * 16403-16412 from the player" when the true gap was 19-28. 0x3FFF is
             * still far above the 150 trust threshold, and anything approaching it
             * is stale by definition. */
            s32 gap = (s32)g_acam_goal_at_to_player;
            if (gap < 0 || gap > 0x3FFF) {
                gap = 0x3FFF;
            }
            recomp_analog_cam_zoom(
                ((s32)radius & 0xFFFF) | (((s32)g_acam_normal & 0xFFFF) << 16),
                ((s32)(g_acam_zoom * 100.0f) & 0xFFF) |
                ((gap & 0x3FFF) << 12) |
                (g_acam_goal_stale ? (1 << 27) : 0) |
                (g_acam_occluded ? (1 << 28) : 0) |
                ((g_acam_anchored != 0) ? (1 << 29) : 0) |
                ((g_acam_anchored == 2) ? (1 << 26) : 0) |
                ((((s32)g_acam_anchor_gap > 3) ? 1 : 0) << 30));
        }

        /* The goal's PROVENANCE: the three bytes the game records about every goal
         * write, and how long ago each half of the goal actually changed.
         *
         * This is the reading that replaces inference with fact. The staleness
         * test, the occlusion classifier and the unanswered question of whether
         * anchoring hijacks scripted shots are all currently answered by measuring
         * distances and hoping; every one of them is really a question about WHO
         * wrote the goal and WHEN, which is what these five numbers say. */
        {
            u32 eye_age = now - g_acam_goal_eye_us;
            u32 at_age = now - g_acam_goal_at_us;
            s32 eye_age_ms = (s32)(eye_age / 1000u);
            s32 at_age_ms = (s32)(at_age / 1000u);
            s32 traced_goal;

            if (!g_acam_goal_seen || eye_age_ms > 0xFFFF || eye_age_ms < 0) {
                eye_age_ms = 0xFFFF;
            }
            if (!g_acam_goal_seen || at_age_ms > 0xFFFF || at_age_ms < 0) {
                at_age_ms = 0xFFFF;
            }

            traced_goal = (s32)g_acam_goal_radius;
            if (traced_goal < 0 || traced_goal > 0xFFFF) {
                traced_goal = 0xFFFF;
            }

            recomp_analog_cam_provenance(
                ((s32)GS_U8(GS_GOAL_TYPE)) |
                (((s32)GS_U8(GS_GOAL_MODE)) << 8) |
                (((s32)GS_U8(GS_GOAL_LATCH)) << 16) |
                (g_acam_occluded ? (1 << 24) : 0) |
                (g_acam_goal_stale ? (1 << 25) : 0),
                eye_age_ms, at_age_ms, traced_goal);
        }
    }

    /* Pull the look-at onto the player, so the character is what the camera orbits
     * rather than whatever point the room wanted to frame.
     *
     * Unconditional while the camera is ours. The first attempt gated this on
     * gState->0x2B0, the game's "a task owns the camera" pointer, on the theory
     * that a scripted shot would set it -- but it is non-null during ordinary play
     * too, so the gate switched anchoring off almost everywhere, including exactly
     * the fixed-angle rooms it was meant for. The pointer is still read, for the
     * trace, because it may yet turn out to mean something useful. */
    if (!g_acam_no_anchor) {
        u8 *ptask = GS_PTR(GS_PLAYER_TASK);
        if (ptask != NULL) {
            u8 *pdata = *(u8 **)(ptask + 0x2C);
            if (pdata != NULL) {
                f32 tx = *(f32 *)(pdata + 0x4);
                f32 ty = *(f32 *)(pdata + 0x8) + ACAM_ANCHOR_HEIGHT;
                f32 tz = *(f32 *)(pdata + 0xC);
                f32 ex = tx - cam->at_x;
                f32 ey = ty - cam->at_y;
                f32 ez = tz - cam->at_z;
                f32 k = ACAM_ANCHOR_EASE * dt_s;

                g_acam_anchor_gap = _nsqrtf(ex * ex + ey * ey + ez * ez);
                g_acam_anchored = (GS_PTR(GS_CAM_OWNER) != NULL) ? 2 : 1;

                if (k > 1.0f) {
                    k = 1.0f;
                }
                cam->at_x += ex * k;
                cam->at_y += ey * k;
                cam->at_z += ez * k;
            }
        }
    } else {
        g_acam_anchored = 0;
    }

    /* The unit direction from the look-at to the eye, so the collision probe and
     * the final placement agree by construction. */
    cos_pitch = func_8001EB64_1F764(pitch_a);
    dir_x = cos_pitch * func_8001EB64_1F764(yaw_a);
    dir_y = func_8001EAD0_1F6D0(pitch_a);
    dir_z = cos_pitch * func_8001EAD0_1F6D0(yaw_a);

    cam->eye_x = cam->at_x + radius * dir_x;
    cam->eye_y = cam->at_y + radius * dir_y;
    cam->eye_z = cam->at_z + radius * dir_z;

    /* REBUILD THE PLAYER'S MOVEMENT BASIS, because the game will not.
     *
     * This is the fix for the worst bug this feature has had: movement inverted,
     * both axes, in some rooms and not others, persisting until R3 handed the
     * camera back -- while the picture stayed perfectly correct.
     *
     * `func_8011A878_4F9F88` derives the basis at gState+0x232 from the camera,
     * and it does so unconditionally once past its latch. The catch is WHO CALLS
     * IT: only four player-state functions do (`func_8010E794_4EDEA4`,
     * `func_801154A4_4F4BB4`, `func_80115734_4F4E44`, `func_80115C64_4F5374`). In
     * any other state the basis simply keeps its last value.
     *
     * That is safe in the original game, and only there. The game's camera can
     * only be turned from inside a C-button camera state -- a C-button puts the
     * player INTO that state and the stick then drives the camera -- so you can
     * never move and look at the same time, and a basis that goes stale while the
     * camera turns is a state that cannot be reached. The analog camera's whole
     * purpose is to break that invariant: it turns the camera in EVERY state,
     * including the ones that never refresh the basis.
     *
     * Measured: the basis sat frozen at 1971 (86.6 deg) while the camera swept
     * through a full circle, so drift ran from -4038 to +3992 -- 177 degrees at
     * worst, which is why it read as a clean inversion in both axes at once.
     *
     * Computed the game's own way rather than as `yaw_a + 0x1000`: the same atan2,
     * on the same two differences, off the eye THIS FUNCTION JUST WROTE. That is
     * bit-identical to what the game's rebuild would have produced, by
     * construction, instead of merely equal in exact arithmetic.
     *
     * The latch is honoured, and that is not defensive coding -- gState+0x234 is
     * how the game deliberately freezes the basis when the camera is inside 10
     * units of its look-at, so the character keeps running straight instead of
     * curving as a too-close camera swings. Writing through it would replace one
     * bug with a subtler one. */
    if (!g_acam_no_basis_fix && GS_U8(GS_BASIS_FROZEN) == 0) {
        GS_S16(GS_BASIS_YAW) = (s16)(func_8001EF38_1FB38(
            cam->at_z - cam->eye_z, cam->at_x - cam->eye_x) & ANGLE_MASK);
    }
}

/* ---------------------------------------------------------------------------
 * The hook.
 * ------------------------------------------------------------------------- */

/**
 * func_80119F9C_4F96AC(&yaw, &pitch) -- the camera's derived angles.
 *
 * Everything after the acam_apply call is the original, faithfully: the yaw of
 * `at - eye` masked to 13 bits, and its pitch measured against the horizontal
 * distance and folded into the signed half-range. Original at
 * lib/hybridheaven/asm/usa/file_7.s, 0x80119F9C, 0x70 bytes.
 */
RECOMP_PATCH void func_80119F9C_4F96AC(s16 *yaw_out, s16 *pitch_out) {
    CameraParams *cam = D_801BBCD8_59B3E8->params;
    f32 dx, dy, dz, horiz;
    s16 pitch;

    /* Trace the GAME's camera mode machine, before anything of ours touches the
     * camera and outside the enabled check -- the point of this log is to record
     * what the game does on its own, so it has to run with the feature off. The
     * host rate-limits and does all the formatting; see recomp_cam_trace. */
    /* Gain travels in the high bits: the host cannot read patch statics, and it
     * is the number that says whether the calibration converged. */
    recomp_cam_trace(cam, (g_acam_engaged & 0xFF) |
                          (((s32)(g_acam_game_gain * 100.0f) & 0xFFFF) << 8));

    /* The autosave's poll, and this is the port's only foothold that runs on
     * gameplay frames from inside `.file_7`. It lives here rather than in a
     * patch of its own because there is no once-per-frame game function to
     * patch that is not already this one -- and because a second RECOMP_PATCH
     * of this function is not possible.
     *
     * Two things follow, both of which patches/autosave.c is written around:
     * this runs more than once in some frames (30+ call sites across the camera
     * state handlers), and it does not run at all in states where the camera
     * task is idle. So the autosave measures its windows in microseconds rather
     * than frames, and its timer is wall-clock. Deliberately outside the analog
     * camera's enabled check: autosaving must not depend on a camera setting. */
    update_autosave();

    acam_apply(cam);

    dy = cam->at_y - cam->eye_y;
    dx = cam->at_x - cam->eye_x;
    dz = cam->at_z - cam->eye_z;

    horiz = _nsqrtf(dx * dx + dz * dz);

    *yaw_out = func_8001EF38_1FB38(dz, dx) & ANGLE_MASK;

    pitch = func_8001EF38_1FB38(dy, horiz);
    if (pitch & ANGLE_HALF) {
        *pitch_out = (pitch & ANGLE_MASK) - 0x2000;
    } else {
        *pitch_out = pitch & ANGLE_MASK;
    }
}

/**
 * func_8011A0F0_4F9800(&roll) -- the camera's current roll about its view axis.
 *
 * While the analog camera owns the camera this reports LEVEL. Otherwise it is
 * the original: ask for the view angles, then back-solve the roll that the
 * stored up vector represents at those angles.
 *
 * WHY, because the reasoning took a wrong turn first and the wrong turn is worth
 * keeping.
 *
 * The per-frame integrator calls this before it moves the camera and feeds the
 * answer into `func_8011A148_4F9858` at the end to rebuild the up vector. The
 * pairing works because the up vector being read here was built from the same
 * view angles this call is about to measure -- so a level camera back-solves to
 * zero and stays level.
 *
 * Turning the camera breaks that pairing, and the leftover is not noise: it is
 * exactly sin(pitch) x (yaw change), the roll a frame picks up when its up
 * vector is carried along an azimuth sweep instead of being rebuilt. The game
 * then faithfully rebuilds the up vector tilted by it, and the next frame
 * measures a larger value. Level camera, no effect; turning WHILE PITCHED, the
 * horizon rolls a little further every frame until the world is on its side.
 * Diaz was photographed lying sideways up a wall.
 *
 * THE WRONG TURN: holding the rotation still for the length of THIS call, so the
 * angles measured would match the up vector being measured against. Reasonable,
 * and measurably useless -- the A/B showed the same runaway to a tenth. The
 * reason is the thing to remember about hooking a shared getter: the getter has
 * ~30 call sites and runs ~30 times a frame, so the eye has already been rotated
 * many times BETWEEN the up vector's rebuild at the end of one frame and this
 * measurement at the start of the next. The mismatch was never created here, so
 * it could never be fixed here.
 *
 * Reporting level while engaged fixes it at the definition instead: the up
 * vector is rebuilt from the current angles with no roll, every frame, and no
 * accumulation is possible. It costs nothing real -- the game's own roll is zero
 * in ordinary play, scripted roll arrives through a different path
 * (`func_80119E24_4F9534`, which runs AFTER the rebuild and drives the up vector
 * toward a goal), and handing the camera back leaves it level, which is what the
 * original then measures.
 */
RECOMP_PATCH void func_8011A0F0_4F9800(s16 *roll_out) {
    CameraParams *cam;
    s16 yaw, pitch;

    func_80119F9C_4F96AC(&yaw, &pitch);
    cam = D_801BBCD8_59B3E8->params;

    if (g_acam_engaged && recomp_get_analog_cam_roll_fix()) {
        *roll_out = 0;
        g_acam_last_roll = 0;
        return;
    }

    func_8011A06C_4F977C(yaw, pitch, cam->up_x, cam->up_y, cam->up_z, roll_out);

    g_acam_last_roll = *roll_out;
    if (g_acam_last_roll & ANGLE_HALF) {
        g_acam_last_roll -= 0x2000;
    }
}
