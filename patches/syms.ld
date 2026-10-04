/* Host exports callable from patch code.
 *
 * These addresses are not real. librecomp treats anything in
 * 0x8F000000-0x90000000 as a "manual patch symbol"
 * (`is_manual_patch_symbol` in librecomp/src/overlays.cpp) and looks it up in
 * the table registered by src/main/register_patches.cpp rather than in any
 * section. The value only has to be unique and in range.
 *
 * Every name here needs a matching entry in that host table, and every
 * `recomp_*` name a patch calls needs an entry here -- patches/Makefile fails
 * the link if one is missing. */

recomp_load_overlays   = 0x8F000000;
recomp_evict_overlays  = 0x8F000004;

recomp_time_us                     = 0x8F000008;
recomp_get_analog_cam_enabled      = 0x8F00000C;
recomp_get_analog_inverted_axes    = 0x8F000010;
recomp_get_analog_cam_sensitivity  = 0x8F000014;
recomp_get_camera_inputs           = 0x8F000018;
recomp_get_camera_recenter_pressed = 0x8F00001C;
recomp_set_right_analog_suppressed = 0x8F000020;
recomp_analog_cam_engaged          = 0x8F000024;
recomp_analog_cam_selftest         = 0x8F000028;
recomp_get_analog_cam_roll_fix     = 0x8F00002C;
recomp_analog_cam_basis            = 0x8F000030;
recomp_set_c_down_suppressed       = 0x8F000034;
recomp_get_camera_zoom_held        = 0x8F000038;
recomp_analog_cam_zoom             = 0x8F00003C;
recomp_analog_cam_goal             = 0x8F000044;
recomp_get_analog_cam_no_anchor    = 0x8F000048;
recomp_analog_cam_provenance       = 0x8F00004C;
recomp_get_analog_cam_legacy_normal = 0x8F000050;
recomp_get_analog_cam_force_dist   = 0x8F000054;
recomp_get_analog_cam_no_basis_fix = 0x8F000058;
recomp_get_analog_cam_legacy_trust = 0x8F00005C;

recomp_get_pak_legacy_probe        = 0x8F000060;
recomp_pak_probe_result            = 0x8F000064;

recomp_cam_trace                   = 0x8F000068;
recomp_get_analog_cam_no_mode_gate = 0x8F00006C;
recomp_get_analog_cam_no_aim_release = 0x8F000070;
recomp_get_analog_cam_legacy_radius_ease = 0x8F000074;
recomp_get_analog_cam_no_framing_gain = 0x8F000078;

recomp_get_autosave_enabled        = 0x8F00007C;
recomp_set_autosave_in_progress    = 0x8F000080;
recomp_notify_saved                = 0x8F000084;
recomp_is_config_menu_open         = 0x8F000088;
recomp_autosave_log                = 0x8F00008C;

/* Game DATA the patches reference, as absolute addresses.
 *
 * These have to be absolute, and the reason is not obvious. Left undefined
 * here, N64Recomp resolves them by name against data_reference_syms_files and
 * emits RELOC_HI16/RELOC_LO16 against the *reference* section index -- but the
 * generated code indexes the one global `section_addresses` array, which the
 * base game fills in using the index space of whatever the base recompilation
 * read. Those two index spaces are only the same when both recompilations read
 * the same file, which is how Goemon64Recomp gets away with it.
 *
 * They are not the same here. hybridheaven.toml has to use `elf_path` (see the
 * README on cross-section relocs), and ELF mode numbers sections by ELF section
 * index -- .main is 6 -- while the .syms.toml reference context numbers them
 * densely by position, making .main 1. A patch compiled the second way reads
 * section_addresses[1], which no section ever writes, gets 0, and dereferences
 * ~0x37C5C. That is exactly what made the loader hook segfault the moment it
 * first ran.
 *
 * An absolute symbol sidesteps the whole question: N64Recomp folds it to a
 * constant with no section lookup (SectionAbsolute, see config.cpp), which
 * `use_absolute_symbols = true` in patches.toml already enables. That is
 * correct here because every one of these lives in .main or .main_bss, and
 * .main is loaded at its link address and never moves -- it is only marked
 * relocatable so the recompiler loads its R_MIPS_26 relocs.
 *
 * FUNCTION references need none of this: those resolve by name and are emitted
 * as direct calls, so they are unaffected by the index mismatch. */

D_80038FE0_39BE0 = 0x80038FE0;   /* Nisitenma-Ichigo archive table, ROM 0x39BE0 */
D_80037C5C_3885C = 0x80037C5C;   /* load-address table, ROM 0x3885C */
D_8008DC18_8E818 = 0x8008DC18;   /* "load in progress" flag, .main_bss */
D_8005CE20_5DA20 = 0x8005CE20;   /* the osPfs/osMotor message queue, ROM 0x5DA20 */
D_8005CE70_5DA70 = 0x8005CE70;   /* OSPfs[4], one per controller, ROM 0x5DA70 */

/* .file_7 bss -- the gameplay engine. Absolute for the same reason as the .main
 * symbols above, and sound for the same reason too, but by a different argument:
 * .file_7 is an OVERLAY, so it is not "loaded at its link address and never
 * moves" in general. What holds instead is that this game's overlay slots are
 * fixed (the load-address table gives one vram per file id) and overlay
 * relocation is disabled for it, so .file_7 is always at its link address
 * whenever it is resident at all -- and patches/camera.c only ever runs from
 * inside a .file_7 function, so "resident" is a precondition of the read.
 *
 * Do not generalise that to any overlay address: slots are SHARED here, so a
 * vram names a slot rather than a thing in it. This one is safe because the
 * reader lives in the same overlay as the datum. */
D_801BBCD8_59B3E8 = 0x801BBCD8;  /* pointer to the camera task, .file_7 bss */
D_801BBBF0_59B300 = 0x801BBBF0;  /* gState, the gameplay engine's master struct */

/* Autosave. Same argument as the two above -- all `.file_7`, and patches/
 * autosave.c only ever runs from inside a `.file_7` function (camera.c's
 * per-frame patch calls it), so residency is a precondition of every read. */
D_801BEC00_59E310 = 0x801BEC00;  /* save cursor: [2] device, [5] slot */
D_801BEB80_59E290 = 0x801BEB80;  /* slot directory, device 1, 8 bytes/slot */
D_801BEBA8_59E2B8 = 0x801BEBA8;  /* slot directory, device 2, 8 bytes/slot */
D_8017DC88_55D398 = 0x8017DC88;  /* written into directory entry +7 */
D_8017DC40_55D350 = 0x8017DC40;  /* marshal source block, 0x9E bytes */
D_80183CE0_5633F0 = 0x80183CE0;  /* marshal source block, 0x56 * 6 bytes */
D_801BED38_59E448 = 0x801BED38;  /* marshal source block, 0x200 bytes */
