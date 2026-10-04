#ifndef __HH_CONFIG_H__
#define __HH_CONFIG_H__

#include <filesystem>
#include <string_view>
#include "ultramodern/config.hpp"
#include "recomp_input.h"

// The port's settings, as the UI sees them. Copied from Goemon64Recomp's
// goemon_config.h, minus every setting that only existed to feed one of that
// port's game patches (targeting mode, autosave, the cheat toggles). Those are
// not disabled here, they are absent: a setting whose reader does not exist is a
// control that silently does nothing. Phase E is where game settings come back,
// each with the patch that reads it.
//
// The analog camera is the first one back: its settings are below, and
// patches/camera.c is the reader.
//
// What is left is what any port of any game can honour: the graphics config
// (ultramodern's own), input bindings and feel, main volume, and the flag that
// gates the RmlUi inspector.
namespace hybridheaven {
    constexpr std::u8string_view program_id = u8"HybridHeavenRecompiled";
    constexpr std::string_view program_name = "Hybrid Heaven: Recompiled";

    // TODO: Move loading configs to the runtime once we have a way to allow per-project customization.
    void load_config();
    void save_config();

    void reset_input_bindings();
    void reset_cont_input_bindings();
    void reset_kb_input_bindings();
    void reset_single_input_binding(recomp::InputDevice device, recomp::GameInput input);

    std::filesystem::path get_app_folder_path();

    // Gates the F8 RmlUi inspector (see ui_state.cpp). Not a game debug mode.
    bool get_debug_mode_enabled();
    void set_debug_mode_enabled(bool enabled);

    void open_quit_game_prompt();
    void open_restart_game_prompt();

    // --- Analog camera ------------------------------------------------------
    //
    // Right-stick free-look. Hybrid Heaven's own camera control is a C-button
    // that swaps the LEFT stick from walking to looking (see
    // docs/hybridheaven-camera-re.md), so this is additive rather than a
    // replacement: with it off, nothing about the game's controls changes.

    enum class AnalogCamMode {
        On,
        Off,
        OptionCount
    };

    // Off listed FIRST: NLOHMANN_JSON_SERIALIZE_ENUM maps an unknown or
    // wrong-typed JSON value to the first pair's enum, so the fallback has to be
    // the state that changes nothing. Order does not affect known values.
    NLOHMANN_JSON_SERIALIZE_ENUM(hybridheaven::AnalogCamMode, {
        {hybridheaven::AnalogCamMode::Off, "Off"},
        {hybridheaven::AnalogCamMode::On, "On"}
    });

    AnalogCamMode get_analog_cam_mode();
    void set_analog_cam_mode(AnalogCamMode mode);

    // On-screen (touch) controls, Android only. Lives in the shared config rather
    // than on the Android side so the game's own menu can bind it like any other
    // option -- that is where a player looks for it, and it keeps one source of
    // truth for whether the overlay is on. The Java layer reads it back over JNI.
    //
    // Auto is listed first deliberately: NLOHMANN_JSON_SERIALIZE_ENUM maps an
    // unknown or wrong-typed value to the first entry, and Auto is the safe landing
    // spot -- a corrupt config leaves a phone with usable controls and a handheld
    // with its screen clear.
    enum class TouchControlsMode {
        Auto, // shown until a gamepad is used, then hidden until the screen is touched
        On,   // always drawn, even with a controller attached
        Off,  // never drawn -- the controller-only behaviour the app shipped with
        OptionCount
    };

    NLOHMANN_JSON_SERIALIZE_ENUM(hybridheaven::TouchControlsMode, {
        {hybridheaven::TouchControlsMode::Auto, "Auto"},
        {hybridheaven::TouchControlsMode::On, "On"},
        {hybridheaven::TouchControlsMode::Off, "Off"}
    });

    TouchControlsMode get_touch_controls_mode();
    void set_touch_controls_mode(TouchControlsMode mode);

    // How directly the on-screen stick follows the thumb, 0..100.
    //
    // 100 is linear -- the deflection sent is simply how far the thumb is from the
    // centre. That is the twitchy end, and it is twitchy for a concrete reason: the
    // stick is about 9 mm across on a 450 dpi phone, so a linear response puts the
    // entire walking range inside roughly 4.5 mm of travel and slow movement is
    // almost impossible to hold.
    //
    // Lower values bend the response, so thumb movement near the centre produces
    // proportionally less deflection while the rim still reaches full tilt. That buys
    // back the walking range without making the stick bigger or costing any top speed.
    int get_touch_stick_sensitivity();
    void set_touch_stick_sensitivity(int value);

    enum class CameraInvertMode {
        InvertNone,
        InvertX,
        InvertY,
        InvertBoth,
        OptionCount
    };

    // Unknown/wrong-typed values fall back to the first entry, InvertNone, which
    // is also the default -- unlike Goemon64Recomp, where this enum served two
    // settings with different defaults and one of them could not match.
    NLOHMANN_JSON_SERIALIZE_ENUM(hybridheaven::CameraInvertMode, {
        {hybridheaven::CameraInvertMode::InvertNone, "InvertNone"},
        {hybridheaven::CameraInvertMode::InvertX, "InvertX"},
        {hybridheaven::CameraInvertMode::InvertY, "InvertY"},
        {hybridheaven::CameraInvertMode::InvertBoth, "InvertBoth"}
    });

    CameraInvertMode get_analog_camera_invert_mode();
    void set_analog_camera_invert_mode(CameraInvertMode mode);

    // Rotation sensitivity, 0-100 per axis. 50 is the tuned default rate; the
    // patch scales its base yaw/pitch rates by (value / 50), so 100 is ~2x and 0
    // stops that axis dead.
    int get_analog_cam_sensitivity_x();
    void set_analog_cam_sensitivity_x(int sensitivity);
    int get_analog_cam_sensitivity_y();
    void set_analog_cam_sensitivity_y(int sensitivity);

    // --- Autosave -----------------------------------------------------------
    //
    // Commits progress through the game's OWN save routine and save-slot
    // format, to whichever slot the save menu last selected. See
    // docs/autosave.md; patches/autosave.c is the reader.

    enum class AutosaveMode {
        Off,
        On,
        OptionCount
    };

    // Off FIRST, and this one matters more than the analog camera's does. An
    // unknown or wrong-typed JSON value maps to the first pair, so a corrupt or
    // downgraded config must not silently start writing over the player's save
    // slot. The default when the key is absent is Off for the same reason --
    // this feature overwrites a real save, so anyone exposed to it opted in.
    NLOHMANN_JSON_SERIALIZE_ENUM(hybridheaven::AutosaveMode, {
        {hybridheaven::AutosaveMode::Off, "Off"},
        {hybridheaven::AutosaveMode::On, "On"}
    });

    AutosaveMode get_autosave_mode();
    void set_autosave_mode(AutosaveMode mode);
};

#endif
