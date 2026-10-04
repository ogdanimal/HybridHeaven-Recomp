#include <algorithm>
#include <cmath>

#include "recomp_ui.h"
#include "recomp_input.h"
#include "hh_sound.h"
#include "hh_config.h"
#include "hh_render.h"
#include "hh_support.h"
#include "promptfont.h"
#include "ultramodern/config.hpp"
#include "ultramodern/ultramodern.hpp"
#include "RmlUi/Core.h"

#include "core/ui_context.h"
#include "ui_gpu_driver.h"
#include "ui_saves.h"

ultramodern::renderer::GraphicsConfig new_options;
Rml::DataModelHandle nav_help_model_handle;
Rml::DataModelHandle general_model_handle;
Rml::DataModelHandle controls_model_handle;
Rml::DataModelHandle graphics_model_handle;
Rml::DataModelHandle sound_options_model_handle;

// True if controller config menu is open, false if keyboard config menu is open, undefined otherwise
bool configuring_controller = false;

int recompui::config_tab_to_index(recompui::ConfigTab tab) {
    switch (tab) {
    case recompui::ConfigTab::General:
        return 0;
    case recompui::ConfigTab::Controls:
        return 1;
    case recompui::ConfigTab::Graphics:
        return 2;
    case recompui::ConfigTab::Sound:
        return 3;
    case recompui::ConfigTab::Mods:
        return 4;
    case recompui::ConfigTab::Saves:
        return 5;
    case recompui::ConfigTab::Driver:
        return 6;
    default:
        assert(false && "Unknown config tab.");
        return 0;
    }
}

template <typename T>
void get_option(const T& input, Rml::Variant& output) {
    std::string value = "";
    to_json(value, input);

    if (value.empty()) {
        throw std::runtime_error("Invalid value :" + std::to_string(int(input)));
    }

    output = value;
}

template <typename T>
void set_option(T& output, const Rml::Variant& input) {
    T value = T::OptionCount;
    from_json(input.Get<std::string>(), value);

    if (value == T::OptionCount) {
        throw std::runtime_error("Invalid value :" + input.Get<std::string>());
    }

    output = value;
}

template <typename T>
void bind_option(Rml::DataModelConstructor& constructor, const std::string& name, T* option) {
    constructor.BindFunc(name,
        [option](Rml::Variant& out) { get_option(*option, out); },
        [option](const Rml::Variant& in) {
            set_option(*option, in);
            graphics_model_handle.DirtyVariable("options_changed");
            graphics_model_handle.DirtyVariable("ds_info");
        }
    );
};

template <typename T>
void bind_atomic(Rml::DataModelConstructor& constructor, Rml::DataModelHandle handle, const char* name, std::atomic<T>* atomic_val) {
    constructor.BindFunc(name,
        [atomic_val](Rml::Variant& out) {
            out = atomic_val->load();
        },
        [atomic_val, handle, name](const Rml::Variant& in) mutable {
            atomic_val->store(in.Get<T>());
            handle.DirtyVariable(name);
        }
    );
}

// Atomic counterpart of bind_option, for enum options whose backing store is
// read every frame from the game/render thread (control + cheat options). The
// load/store removes the data race; the DirtyVariable calls mirror bind_option
// exactly so behavior is unchanged.
template <typename T>
void bind_atomic_option(Rml::DataModelConstructor& constructor, const std::string& name, std::atomic<T>* option) {
    constructor.BindFunc(name,
        [option](Rml::Variant& out) {
            T value = option->load();
            get_option(value, out);
        },
        [option](const Rml::Variant& in) {
            T value = option->load();
            set_option(value, in);
            option->store(value);
            graphics_model_handle.DirtyVariable("options_changed");
            graphics_model_handle.DirtyVariable("ds_info");
        }
    );
}

static int scanned_binding_index = -1;
static int scanned_input_index = -1;
static int focused_input_index = -1;
static int focused_config_option_index = -1;

static bool msaa2x_supported = false;
static bool msaa4x_supported = false;
static bool msaa8x_supported = false;
static bool sample_positions_supported = false;

// Android is always fullscreen-composited by the OS, so the Window Mode option
// is meaningless there. Bound into the graphics data model to hide that row and
// reroute menu navigation around it (see graphics.rml).
#if defined(__ANDROID__)
static bool is_android = true;
#else
static bool is_android = false;
#endif

// Downsampling (supersampling) is only offered for the base resolutions. The
// higher fixed multiples (3x-8x) render large enough that stacking downsampling
// on top would blow past RT64's ResolutionMultiplierLimit, and Auto manages its
// own scale, so downsampling is disabled/forced-off for everything else.
static bool downsampling_available(ultramodern::renderer::Resolution res) {
    return res == ultramodern::renderer::Resolution::Original
        || res == ultramodern::renderer::Resolution::Original2x;
}

static bool cont_active = true;

// The only thing left of Goemon64Recomp's Debug menu: what F8 checks before it
// toggles the RmlUi inspector (see ui_state.cpp). Atomic because the SDL thread
// reads it while the UI thread writes it.
static std::atomic<bool> debug_mode_enabled = false;

static recomp::InputDevice cur_device = recomp::InputDevice::Controller;

int recomp::get_scanned_input_index() {
    return scanned_input_index;
}

void recomp::finish_scanning_input(recomp::InputField scanned_field) {
    recomp::set_input_binding(static_cast<recomp::GameInput>(scanned_input_index), scanned_binding_index, cur_device, scanned_field);
    scanned_input_index = -1;
    scanned_binding_index = -1;
    controls_model_handle.DirtyVariable("inputs");
    controls_model_handle.DirtyVariable("active_binding_input");
    controls_model_handle.DirtyVariable("active_binding_slot");
    nav_help_model_handle.DirtyVariable("nav_help__accept");
    nav_help_model_handle.DirtyVariable("nav_help__exit");
    graphics_model_handle.DirtyVariable("gfx_help__apply");
}

void recomp::cancel_scanning_input() {
    recomp::stop_scanning_input();
    scanned_input_index = -1;
    scanned_binding_index = -1;
    controls_model_handle.DirtyVariable("inputs");
    controls_model_handle.DirtyVariable("active_binding_input");
    controls_model_handle.DirtyVariable("active_binding_slot");
    nav_help_model_handle.DirtyVariable("nav_help__accept");
    nav_help_model_handle.DirtyVariable("nav_help__exit");
    graphics_model_handle.DirtyVariable("gfx_help__apply");
}

void recomp::config_menu_set_cont_or_kb(bool cont_interacted) {
    if (cont_active != cont_interacted) {
        cont_active = cont_interacted;

        if (nav_help_model_handle) {
            nav_help_model_handle.DirtyVariable("nav_help__navigate");
            nav_help_model_handle.DirtyVariable("nav_help__accept");
            nav_help_model_handle.DirtyVariable("nav_help__exit");
        }

        if (graphics_model_handle) {
            graphics_model_handle.DirtyVariable("gfx_help__apply");
        }
    }
}

void close_config_menu_impl() {
    hybridheaven::save_config();

    recompui::ContextId config_context = recompui::get_config_context_id();
    recompui::ContextId sub_menu_context = recompui::get_config_sub_menu_context_id();

    if (recompui::is_context_shown(sub_menu_context)) {
    	recompui::hide_context(sub_menu_context);
    }
    else {
    	recompui::hide_context(config_context);
    }

    if (!ultramodern::is_game_started()) {
        recompui::show_context(recompui::get_launcher_context_id(), "");
    }
}

// TODO: Remove once RT64 gets native fullscreen support on Linux.
// Android also defines __linux__ but must be excluded: it is always
// fullscreen-composited by the OS, and calling SDL_SetWindowFullscreen(0)
// there tears down the immersive flags and brings the system bars back.
#if defined(__linux__) && !defined(__ANDROID__)
extern SDL_Window* window;
#endif

void apply_graphics_config(void) {
    ultramodern::renderer::set_graphics_config(new_options);
#if defined(__linux__) && !defined(__ANDROID__) // TODO: Remove once RT64 gets native fullscreen support on Linux
    if (new_options.wm_option == ultramodern::renderer::WindowMode::Fullscreen) {
        SDL_SetWindowFullscreen(window,SDL_WINDOW_FULLSCREEN_DESKTOP);
    } else {
        SDL_SetWindowFullscreen(window,0);
    }
#endif
}

void close_config_menu() {
    if (ultramodern::renderer::get_graphics_config() != new_options) {
        recompui::open_choice_prompt(
            "Graphics options have changed",
            "Would you like to apply or discard the changes?",
            "Apply",
            "Discard",
            []() {
                apply_graphics_config();
                graphics_model_handle.DirtyAllVariables();
                close_config_menu_impl();
            },
            []() {
                new_options = ultramodern::renderer::get_graphics_config();
                graphics_model_handle.DirtyAllVariables();
                close_config_menu_impl();
            },
            recompui::ButtonVariant::Success,
            recompui::ButtonVariant::Error,
            true,
            "config__close-menu-button"
        );
        return;
    }

    close_config_menu_impl();
}

void hybridheaven::open_quit_game_prompt() {
    recompui::open_choice_prompt(
        "Are you sure you want to quit?",
        "Any progress since your last save will be lost.",
        "Quit",
        "Cancel",
        []() {
            ultramodern::quit();
        },
        []() {},
        recompui::ButtonVariant::Error,
        recompui::ButtonVariant::Tertiary,
        true,
        "config__quit-game-button"
    );
}

// The restart button is only meaningful for a running game, and is implemented
// as an Android process relaunch (see hybridheaven::request_restart), so it is
// hidden on desktop and before the game starts rather than left visible only to
// explain that it does nothing. Both conditions are effectively monotonic --
// desktop is compile-time, and is_game_started() only ever flips false->true --
// so the change detector means this costs one DOM write per process.
void recompui::sync_restart_button_visibility() {
#if defined(__ANDROID__)
    const bool visible = ultramodern::is_game_started();
#else
    const bool visible = false;
#endif

    static bool applied = false;
    static bool last_visible = false;
    if (applied && last_visible == visible) {
        return;
    }

    ContextId old_context = recompui::try_close_current_context();

    for (ContextId ctx : { recompui::get_config_context_id(), recompui::get_config_sub_menu_context_id() }) {
        if (ctx == ContextId::null()) {
            continue;
        }
        Rml::ElementDocument* doc = ctx.get_document();
        if (doc == nullptr) {
            continue;
        }
        if (Rml::Element* button = doc->GetElementById("config__restart-game-button")) {
            if (visible) {
                // Remove rather than set a display: .icon-button is a centering
                // flex box (recomp.rcss), so hardcoding a value here would
                // un-center the svg and change the button's spacing.
                button->RemoveProperty(Rml::PropertyId::Display);
            }
            else {
                button->SetProperty(Rml::PropertyId::Display, Rml::Style::Display::None);
            }
            applied = true;
            last_visible = visible;
        }
    }

    if (old_context != ContextId::null()) {
        old_context.open();
    }
}

void hybridheaven::open_restart_game_prompt() {
#if defined(__ANDROID__)
    recompui::open_three_choice_prompt(
        "Restart the game?",
        "Any progress since your last save will be lost.",
        "To Title Screen",
        "To App Menu",
        "Cancel",
        []() {
            hybridheaven::request_restart(hybridheaven::RestartTarget::TitleScreen);
        },
        []() {
            hybridheaven::request_restart(hybridheaven::RestartTarget::AppMenu);
        },
        []() {},
        recompui::ButtonVariant::Warning,
        recompui::ButtonVariant::Warning,
        recompui::ButtonVariant::Tertiary,
        true,
        "config__restart-game-button"
    );
#endif
}

// These defaults values don't matter, as the config file handling overrides them.
// All fields are atomic: the input thread reads these every frame while the UI
// thread writes them. Mirrors SoundOptionsContext. Zero-initialized as a global;
// config load overwrites every field via the setters below.
//
// Goemon64Recomp, which this file came from, also kept its game-specific
// settings here (targeting mode, analog camera, the cheat toggles). Most are
// gone rather than disabled: every one of them was read by a Goemon patch this
// port does not have, so they would have been settings that change nothing.
// Phase E is where game settings come back, one patch at a time -- the analog
// camera below is the first, and patches/camera.c is its reader.
struct ControlOptionsContext {
    std::atomic<int> rumble_strength; // 0 to 100
    std::atomic<int> gyro_sensitivity; // 0 to 100
    std::atomic<int> mouse_sensitivity; // 0 to 100
    std::atomic<int> joystick_deadzone; // 0 to 100
    std::atomic<recomp::BackgroundInputMode> background_input_mode;
    std::atomic<hybridheaven::AnalogCamMode> analog_cam_mode;
    std::atomic<hybridheaven::TouchControlsMode> touch_controls_mode;
    std::atomic<int> touch_stick_sensitivity; // 0 to 100, lower = finer near centre
    std::atomic<hybridheaven::CameraInvertMode> analog_camera_invert_mode;
    std::atomic<int> analog_cam_sensitivity_x; // 0 to 100, 50 = default rate
    std::atomic<int> analog_cam_sensitivity_y; // 0 to 100, 50 = default rate
    std::atomic<hybridheaven::AutosaveMode> autosave_mode;
};

ControlOptionsContext control_options_context;

// Whether on-screen controls exist on this platform. A plain bool rather than a
// compile-time constant in the RML, because RmlUi data bindings need something to
// point at.
#if defined(__ANDROID__)
bool touch_supported = true;
#else
bool touch_supported = false;
#endif

int recomp::get_rumble_strength() {
    return control_options_context.rumble_strength;
}

void recomp::set_rumble_strength(int strength) {
    // Clamp here: this setter is the single chokepoint for both the UI slider and
    // config load, so a hand-edited/corrupt controls value (negative, or a huge
    // number the rumble scaler would overflow into signed-16-bit UB) is bounded
    // to the 0-100% range before any consumer sees it. Same for the setters below.
    control_options_context.rumble_strength = std::clamp(strength, 0, 100);
    if (general_model_handle) {
        general_model_handle.DirtyVariable("rumble_strength");
    }
}

int recomp::get_gyro_sensitivity() {
    return control_options_context.gyro_sensitivity;
}

int recomp::get_mouse_sensitivity() {
    return control_options_context.mouse_sensitivity;
}

int recomp::get_joystick_deadzone() {
    return control_options_context.joystick_deadzone;
}

void recomp::set_gyro_sensitivity(int sensitivity) {
    control_options_context.gyro_sensitivity = std::clamp(sensitivity, 0, 100);
    if (general_model_handle) {
        general_model_handle.DirtyVariable("gyro_sensitivity");
    }
}

void recomp::set_mouse_sensitivity(int sensitivity) {
    control_options_context.mouse_sensitivity = std::clamp(sensitivity, 0, 100);
    if (general_model_handle) {
        general_model_handle.DirtyVariable("mouse_sensitivity");
    }
}

void recomp::set_joystick_deadzone(int deadzone) {
    // >100 makes apply_joystick_deadzone's (1 - deadzone/100) divisor negative
    // (axis inversion); exactly 100 divides by zero (NaN stick). Clamp to the
    // 0-100% range; the exact-100 divisor is additionally guarded in the consumer.
    control_options_context.joystick_deadzone = std::clamp(deadzone, 0, 100);
    if (general_model_handle) {
        general_model_handle.DirtyVariable("joystick_deadzone");
    }
}

recomp::BackgroundInputMode recomp::get_background_input_mode() {
    return control_options_context.background_input_mode;
}

void recomp::set_background_input_mode(recomp::BackgroundInputMode mode) {
    control_options_context.background_input_mode = mode;
    if (general_model_handle) {
        general_model_handle.DirtyVariable("background_input_mode");
    }
    SDL_SetHint(
        SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS,
        mode == recomp::BackgroundInputMode::On
            ? "1"
            : "0"
    );
}

hybridheaven::AnalogCamMode hybridheaven::get_analog_cam_mode() {
    return control_options_context.analog_cam_mode;
}

void hybridheaven::set_analog_cam_mode(hybridheaven::AnalogCamMode mode) {
    control_options_context.analog_cam_mode = mode;
    if (general_model_handle) {
        general_model_handle.DirtyVariable("analog_cam_mode");
    }
}

hybridheaven::TouchControlsMode hybridheaven::get_touch_controls_mode() {
    return control_options_context.touch_controls_mode;
}

void hybridheaven::set_touch_controls_mode(hybridheaven::TouchControlsMode mode) {
    control_options_context.touch_controls_mode = mode;
    if (general_model_handle) {
        general_model_handle.DirtyVariable("touch_controls_mode");
    }
}

int hybridheaven::get_touch_stick_sensitivity() {
    return control_options_context.touch_stick_sensitivity;
}

void hybridheaven::set_touch_stick_sensitivity(int value) {
    control_options_context.touch_stick_sensitivity = std::clamp(value, 0, 100);
    if (general_model_handle) {
        general_model_handle.DirtyVariable("touch_stick_sensitivity");
    }
}

bool recompui::is_config_menu_open() {
    return recompui::is_context_shown(recompui::get_config_context_id()) ||
           recompui::is_context_shown(recompui::get_config_sub_menu_context_id());
}

hybridheaven::AutosaveMode hybridheaven::get_autosave_mode() {
    return control_options_context.autosave_mode;
}

void hybridheaven::set_autosave_mode(hybridheaven::AutosaveMode mode) {
    control_options_context.autosave_mode = mode;
    if (general_model_handle) {
        general_model_handle.DirtyVariable("autosave_mode");
    }
}

hybridheaven::CameraInvertMode hybridheaven::get_analog_camera_invert_mode() {
    return control_options_context.analog_camera_invert_mode;
}

void hybridheaven::set_analog_camera_invert_mode(hybridheaven::CameraInvertMode mode) {
    control_options_context.analog_camera_invert_mode = mode;
    if (general_model_handle) {
        general_model_handle.DirtyVariable("analog_camera_invert_mode");
    }
}

int hybridheaven::get_analog_cam_sensitivity_x() {
    return control_options_context.analog_cam_sensitivity_x;
}

void hybridheaven::set_analog_cam_sensitivity_x(int sensitivity) {
    control_options_context.analog_cam_sensitivity_x = std::clamp(sensitivity, 0, 100);
    if (general_model_handle) {
        general_model_handle.DirtyVariable("analog_cam_sensitivity_x");
    }
}

int hybridheaven::get_analog_cam_sensitivity_y() {
    return control_options_context.analog_cam_sensitivity_y;
}

void hybridheaven::set_analog_cam_sensitivity_y(int sensitivity) {
    control_options_context.analog_cam_sensitivity_y = std::clamp(sensitivity, 0, 100);
    if (general_model_handle) {
        general_model_handle.DirtyVariable("analog_cam_sensitivity_y");
    }
}

// Only a main volume here, where Goemon64Recomp also had separate BGM and SE
// sliders. Those two were split inside the *game's* audio code by a patch, so
// without that patch there is nothing to apply them to; main volume is applied
// by the port itself, in main.cpp's queue_samples, and works on any game.
struct SoundOptionsContext {
    std::atomic<int> main_volume; // Option to control the volume of all sound
    void reset() {
        main_volume = 100;
    }
    SoundOptionsContext() {
        reset();
    }
};

SoundOptionsContext sound_options_context;

void hybridheaven::reset_sound_settings() {
    sound_options_context.reset();
    if (sound_options_model_handle) {
        sound_options_model_handle.DirtyAllVariables();
    }
}

void hybridheaven::set_main_volume(int volume) {
    // Volumes are a 0-100% factor (main.cpp normalizes /100); an unclamped
    // config value (e.g. 10000 -> 100x amplification / clipping, or negative)
    // must be bounded here. Same for bgm/se below.
    sound_options_context.main_volume.store(std::clamp(volume, 0, 100));
    if (sound_options_model_handle) {
        sound_options_model_handle.DirtyVariable("main_volume");
    }
}

int hybridheaven::get_main_volume() {
    return sound_options_context.main_volume.load();
}

recompui::ContextId config_context;

recompui::ContextId recompui::get_config_context_id() {
	return config_context;
}

// Helper copied from RmlUi to get a named child.
Rml::Element* recompui::get_child_by_tag(Rml::Element* parent, const std::string& tag)
{
	// Look for the existing child
	for (int i = 0; i < parent->GetNumChildren(); i++)
	{
        Rml::Element* child = parent->GetChild(i);
		if (child->GetTagName() == tag)
			return child;
	}

    return nullptr;
}

// Lets the tab strip be dragged sideways.
//
// The strip scrolls horizontally because, with the Touch tab on Android, the tabs
// no longer fit beside the icon buttons, and RmlUi 6.0 only scrolls on the wheel or
// the scrollbar -- a touch swipe arrives (via SDL's touch-to-mouse emulation) as a
// press and a move, which RmlUi treats as hover. So the drag is done here: press
// anywhere on the strip, move past a small threshold, and the strip follows the
// finger.
//
// The click that ends a drag is swallowed, so dragging never switches tabs. A tap
// that stays under the threshold is an ordinary click and selects the tab as before.
class TabStripDragScroller : public Rml::EventListener {
public:
    void attach(Rml::Element* tabs) {
        strip = tabs;
        // Capture phase on the strip: the click has to be stopped before it reaches
        // the tab, and before the tabset's default action, which is what switches tabs.
        strip->AddEventListener(Rml::EventId::Mousedown, this, true);
        strip->AddEventListener(Rml::EventId::Click, this, true);
        // Move and release are taken on the whole document, so a drag that leaves the
        // strip's bounds keeps scrolling and still ends cleanly.
        Rml::ElementDocument* doc = strip->GetOwnerDocument();
        doc->AddEventListener(Rml::EventId::Mousemove, this, true);
        doc->AddEventListener(Rml::EventId::Mouseup, this, true);
    }

    void ProcessEvent(Rml::Event& event) override {
        switch (event.GetId()) {
            case Rml::EventId::Mousedown:
                if (event.GetParameter<int>("button", -1) == 0) {
                    pressed = true;
                    dragged = false;
                    press_x = event.GetParameter<float>("mouse_x", 0.0f);
                    press_scroll = strip->GetScrollLeft();
                }
                break;
            case Rml::EventId::Mousemove:
                if (pressed) {
                    float dx = event.GetParameter<float>("mouse_x", press_x) - press_x;
                    if (!dragged && std::fabs(dx) > drag_threshold_px()) {
                        dragged = true;
                    }
                    if (dragged) {
                        strip->SetScrollLeft(press_scroll - dx);
                    }
                }
                break;
            case Rml::EventId::Mouseup:
                // dragged is left set: the click for this release is dispatched after
                // the mouseup, and it is what clears the flag.
                pressed = false;
                break;
            case Rml::EventId::Click:
                if (dragged) {
                    dragged = false;
                    event.StopPropagation();
                }
                break;
            default:
                break;
        }
    }

private:
    float drag_threshold_px() const {
        Rml::Context* context = strip->GetContext();
        float ratio = context != nullptr ? context->GetDensityIndependentPixelRatio() : 1.0f;
        return 12.0f * ratio;
    }

    Rml::Element* strip = nullptr;
    bool pressed = false;
    bool dragged = false;
    float press_x = 0.0f;
    float press_scroll = 0.0f;
};

class ConfigTabsetListener : public Rml::EventListener {
    void ProcessEvent(Rml::Event& event) override {
        if (event.GetId() == Rml::EventId::Tabchange) {
            int tab_index = event.GetParameter<int>("tab_index", 0);
            // The strip scrolls, so a tab chosen by controller, by the shoulder
            // buttons or from code may be off the edge. Bring it into view.
            // Controller focus already does this for the tab it lands on; this
            // covers every other way the active tab changes.
            {
                Rml::Element* tabs = recompui::get_child_by_tag(recompui::get_config_tabset(), "tabs");
                if (tabs != nullptr && tab_index >= 0 && tab_index < tabs->GetNumChildren()) {
                    tabs->GetChild(tab_index)->ScrollIntoView(
                        Rml::ScrollIntoViewOptions{Rml::ScrollAlignment::Nearest, Rml::ScrollAlignment::Nearest});
                }
            }
            bool in_mod_tab = (tab_index == recompui::config_tab_to_index(recompui::ConfigTab::Mods));
            if (in_mod_tab) {
                recompui::set_config_tabset_mod_nav();
            }
            else {
                Rml::ElementTabSet* tabset = recompui::get_config_tabset();
                Rml::Element* tabs = recompui::get_child_by_tag(tabset, "tabs");
                if (tabs != nullptr) {
                    size_t num_children = tabs->GetNumChildren();
                    for (size_t i = 0; i < num_children; i++) {
                        tabs->GetChild(i)->SetProperty(Rml::PropertyId::NavDown, Rml::Style::Nav::Auto);
                    }
                }
            }
        }
    }
};

class ConfigMenu : public recompui::MenuController {
private:
    ConfigTabsetListener config_tabset_listener;
    TabStripDragScroller tab_strip_scroller;
public:
    ConfigMenu() {

    }
    ~ConfigMenu() override {

    }
    void load_document() override {
		config_context = recompui::create_context(hybridheaven::get_asset_path("config_menu.rml"));
        recompui::update_mod_list(false);
        Rml::ElementTabSet* tabset = recompui::get_config_tabset();
        tabset->AddEventListener(Rml::EventId::Tabchange, &config_tabset_listener);
        if (Rml::Element* tabs = recompui::get_child_by_tag(tabset, "tabs")) {
            tab_strip_scroller.attach(tabs);
        }
    }
    void register_events(recompui::UiEventListenerInstancer& listener) override {
        recompui::register_event(listener, "apply_options",
            [](const std::string& param, Rml::Event& event) {
                graphics_model_handle.DirtyVariable("options_changed");
                apply_graphics_config();
            });
        recompui::register_event(listener, "config_keydown",
            [](const std::string& param, Rml::Event& event) {
                if (!recompui::is_prompt_open() && event.GetId() == Rml::EventId::Keydown) {
                    auto key = event.GetParameter<Rml::Input::KeyIdentifier>("key_identifier", Rml::Input::KeyIdentifier::KI_UNKNOWN);
                    switch (key) {
                        case Rml::Input::KeyIdentifier::KI_ESCAPE:
                            close_config_menu();
                            break;
                        case Rml::Input::KeyIdentifier::KI_F:
                            graphics_model_handle.DirtyVariable("options_changed");
                            apply_graphics_config();
                            break;
                    }
                }
            });
        // This needs to be separate from `close_config_menu` so it ensures that the event is only on the target
        recompui::register_event(listener, "close_config_menu_backdrop",
            [](const std::string& param, Rml::Event& event) {
                if (event.GetPhase() == Rml::EventPhase::Target) {
                    close_config_menu();
                }
            });
        recompui::register_event(listener, "close_config_menu",
            [](const std::string& param, Rml::Event& event) {
                close_config_menu();
            });

        recompui::register_event(listener, "open_quit_game_prompt",
            [](const std::string& param, Rml::Event& event) {
                hybridheaven::open_quit_game_prompt();
            });

        recompui::register_event(listener, "open_restart_game_prompt",
            [](const std::string& param, Rml::Event& event) {
                hybridheaven::open_restart_game_prompt();
            });

        recompui::register_event(listener, "toggle_input_device",
            [](const std::string& param, Rml::Event& event) {
                cur_device = cur_device == recomp::InputDevice::Controller
                    ? recomp::InputDevice::Keyboard
                    : recomp::InputDevice::Controller;
                controls_model_handle.DirtyVariable("input_device_is_keyboard");
                controls_model_handle.DirtyVariable("inputs");
            });

        // Goemon64Recomp registered its warp/set-time debug events here. Those
        // drove a scene table this game has no equivalent of yet, so the Debug
        // tab is not carried over.

        recompui::register_gpu_driver_events(listener);

#if defined(__ANDROID__)
        // Hands off to the Android side, which puts the live overlay into edit mode
        // over the running game. The editor is deliberately the real overlay rather
        // than a mock: the only question it answers is "can my thumb reach that",
        // and a mock at a different size, without the game behind it, cannot answer
        // that. Closes the menu first for the same reason -- you cannot judge a
        // layout you cannot see.
        recompui::register_event(listener, "touch_edit_layout",
            [](const std::string& /*param*/, Rml::Event& /*event*/) {
                recompui::hide_all_contexts();
                hybridheaven::request_touch_layout_editor();
            });
#endif
        recompui::register_saves_events(listener);
    }

    void bind_config_list_events(Rml::DataModelConstructor &constructor) {
        constructor.BindEventCallback("set_cur_config_index",
            [](Rml::DataModelHandle model_handle, Rml::Event& event, const Rml::VariantList& inputs) {
                int option_index = inputs.at(0).Get<size_t>();
                // watch for mouseout being overzealous during event bubbling, only clear if the event's attached element matches the current
                if (option_index == -1 && event.GetType() == "mouseout" && event.GetCurrentElement() != event.GetTargetElement()) {
                    return;
                }
                focused_config_option_index = option_index;
                model_handle.DirtyVariable("cur_config_index");
            });

        constructor.Bind("cur_config_index", &focused_config_option_index);
    }

    void make_graphics_bindings(Rml::Context* context) {
        Rml::DataModelConstructor constructor = context->CreateDataModel("graphics_model");
        if (!constructor) {
            throw std::runtime_error("Failed to make RmlUi data model for the graphics config menu");
        }

        // No sleep here (N13): get_graphics_config() reads the config struct that
        // load_config() populated at startup, so it is already valid by the time
        // init_hook builds the menus -- the old 50ms render-thread sleep guarded
        // nothing. The renderer's async capability data (MSAA support, etc.) and a
        // fresh new_options are applied later by update_supported_options(), the
        // gfx_init_callback, which DirtyAllVariables() to refresh the whole menu.
        new_options = ultramodern::renderer::get_graphics_config();
        bind_config_list_events(constructor);

        constructor.BindFunc("res_option",
            [](Rml::Variant& out) { get_option(new_options.res_option, out); },
            [](const Rml::Variant& in) {
                set_option(new_options.res_option, in);
                graphics_model_handle.DirtyVariable("options_changed");
                graphics_model_handle.DirtyVariable("ds_info");
                graphics_model_handle.DirtyVariable("ds_option");
            }
        );
        bind_option(constructor, "wm_option", &new_options.wm_option);
        bind_option(constructor, "ar_option", &new_options.ar_option);
        bind_option(constructor, "hr_option", &new_options.hr_option);
        bind_option(constructor, "msaa_option", &new_options.msaa_option);
        bind_option(constructor, "fbe_option", &new_options.fbe_option);
        bind_option(constructor, "vsync_option", &new_options.vsync_option);
        bind_option(constructor, "rr_option", &new_options.rr_option);
        constructor.BindFunc("rr_manual_value",
            [](Rml::Variant& out) {
                out = new_options.rr_manual_value;
            },
            [](const Rml::Variant& in) {
                new_options.rr_manual_value = in.Get<int>();
                graphics_model_handle.DirtyVariable("options_changed");
            });
        constructor.BindFunc("ds_option",
            [](Rml::Variant& out) {
                if (!downsampling_available(new_options.res_option)) {
                    out = 1;
                } else {
                    out = new_options.ds_option;
                }
            },
            [](const Rml::Variant& in) {
                new_options.ds_option = in.Get<int>();
                graphics_model_handle.DirtyVariable("options_changed");
                graphics_model_handle.DirtyVariable("ds_info");
            });

        constructor.BindFunc("display_refresh_rate",
            [](Rml::Variant& out) {
                out = ultramodern::get_display_refresh_rate();
            });

        constructor.BindFunc("options_changed",
            [](Rml::Variant& out) {
                out = (ultramodern::renderer::get_graphics_config() != new_options);
            });
        constructor.BindFunc("ds_info",
            [](Rml::Variant& out) {
                if (!downsampling_available(new_options.res_option)) {
                    out = (new_options.res_option == ultramodern::renderer::Resolution::Auto)
                        ? "Downsampling is not available at auto resolution"
                        : "Downsampling is not available at this resolution";
                    return;
                }
                switch (new_options.res_option) {
                    default:
                    case ultramodern::renderer::Resolution::Original:
                        if (new_options.ds_option == 2) {
                            out = "Rendered in 480p and scaled to 240p";
                        } else if (new_options.ds_option == 4) {
                            out = "Rendered in 960p and scaled to 240p";
                        }
                        return;
                    case ultramodern::renderer::Resolution::Original2x:
                        if (new_options.ds_option == 2) {
                            out = "Rendered in 960p and scaled to 480p";
                        } else if (new_options.ds_option == 4) {
                            out = "Rendered in 4K and scaled to 480p";
                        }
                        return;
                }
                out = "";
            });
        
        constructor.BindFunc("gfx_help__apply", [](Rml::Variant& out) {
            if (cont_active) {
                out = \
                    (recomp::get_input_binding(recomp::GameInput::APPLY_MENU, 0, recomp::InputDevice::Controller).to_string() != "" ?
                        " " + recomp::get_input_binding(recomp::GameInput::APPLY_MENU, 0, recomp::InputDevice::Controller).to_string() :
                        ""
                    ) + \
                    (recomp::get_input_binding(recomp::GameInput::APPLY_MENU, 1, recomp::InputDevice::Controller).to_string() != "" ?
                        " " + recomp::get_input_binding(recomp::GameInput::APPLY_MENU, 1, recomp::InputDevice::Controller).to_string() :
                        ""
                    );
            } else {
                out = " " PF_KEYBOARD_F;
            }
        });

        constructor.Bind("msaa2x_supported", &msaa2x_supported);
        constructor.Bind("msaa4x_supported", &msaa4x_supported);
        constructor.Bind("msaa8x_supported", &msaa8x_supported);
        constructor.Bind("sample_positions_supported", &sample_positions_supported);
        constructor.Bind("is_android", &is_android);

        graphics_model_handle = constructor.GetModelHandle();
    }

    void make_controls_bindings(Rml::Context* context) {
        Rml::DataModelConstructor constructor = context->CreateDataModel("controls_model");
        if (!constructor) {
            throw std::runtime_error("Failed to make RmlUi data model for the controls config menu");
        }

        constructor.BindFunc("input_count", [](Rml::Variant& out) { out = static_cast<uint64_t>(recomp::get_num_inputs()); } );
        constructor.BindFunc("input_device_is_keyboard", [](Rml::Variant& out) { out = cur_device == recomp::InputDevice::Keyboard; } );

        constructor.RegisterTransformFunc("get_input_name", [](const Rml::VariantList& inputs) {
            return Rml::Variant{recomp::get_input_name(static_cast<recomp::GameInput>(inputs.at(0).Get<size_t>()))};
        });

        constructor.RegisterTransformFunc("get_input_enum_name", [](const Rml::VariantList& inputs) {
            return Rml::Variant{recomp::get_input_enum_name(static_cast<recomp::GameInput>(inputs.at(0).Get<size_t>()))};
        });

        constructor.RegisterTransformFunc("get_input_description", [](const Rml::VariantList& inputs) {
            if (inputs.empty()) {
                return Rml::Variant{""};
            }

            // Get as int first to properly handle -1
            int index = inputs.at(0).Get<int>();

            // Bounds check before casting to enum
            if (index < 0 || index >= static_cast<int>(recomp::GameInput::COUNT)) {
                return Rml::Variant{""};
            }

            return Rml::Variant{recomp::get_input_description(static_cast<recomp::GameInput>(index))};
        });

        constructor.BindEventCallback("set_input_binding",
            [](Rml::DataModelHandle model_handle, Rml::Event& event, const Rml::VariantList& inputs) {
                scanned_input_index = inputs.at(0).Get<size_t>();
                scanned_binding_index = inputs.at(1).Get<size_t>();
                recomp::start_scanning_input(cur_device);
                model_handle.DirtyVariable("active_binding_input");
                model_handle.DirtyVariable("active_binding_slot");
            });

        constructor.BindEventCallback("reset_input_bindings_to_defaults",
            [](Rml::DataModelHandle model_handle, Rml::Event& event, const Rml::VariantList& inputs) {
                if (cur_device == recomp::InputDevice::Controller) {
                    hybridheaven::reset_cont_input_bindings();
                } else {
                    hybridheaven::reset_kb_input_bindings();
                }
                model_handle.DirtyAllVariables();
                nav_help_model_handle.DirtyVariable("nav_help__accept");
                nav_help_model_handle.DirtyVariable("nav_help__exit");
                graphics_model_handle.DirtyVariable("gfx_help__apply");
            });

        constructor.BindEventCallback("clear_input_bindings",
            [](Rml::DataModelHandle model_handle, Rml::Event& event, const Rml::VariantList& inputs) {
                recomp::GameInput input = static_cast<recomp::GameInput>(inputs.at(0).Get<size_t>());
                for (size_t binding_index = 0; binding_index < recomp::bindings_per_input; binding_index++) {
                    recomp::set_input_binding(input, binding_index, cur_device, recomp::InputField{});
                }
                model_handle.DirtyVariable("inputs");
                graphics_model_handle.DirtyVariable("gfx_help__apply");
            });

        constructor.BindEventCallback("reset_single_input_binding_to_default",
            [](Rml::DataModelHandle model_handle, Rml::Event& event, const Rml::VariantList& inputs) {
                recomp::GameInput input = static_cast<recomp::GameInput>(inputs.at(0).Get<size_t>());
                hybridheaven::reset_single_input_binding(cur_device, input);
                model_handle.DirtyVariable("inputs");
                nav_help_model_handle.DirtyVariable("nav_help__accept");
                nav_help_model_handle.DirtyVariable("nav_help__exit");
            });

        constructor.BindEventCallback("set_input_row_focus",
            [](Rml::DataModelHandle model_handle, Rml::Event& event, const Rml::VariantList& inputs) {
                int input_index = inputs.at(0).Get<size_t>();
                // watch for mouseout being overzealous during event bubbling, only clear if the event's attached element matches the current
                if (input_index == -1 && event.GetType() == "mouseout" && event.GetCurrentElement() != event.GetTargetElement()) {
                    return;
                }
                focused_input_index = input_index;
                model_handle.DirtyVariable("cur_input_row");
                model_handle.DirtyVariable("cur_input_index");
            });

        // Rml variable definition for an individual InputField.
        struct InputFieldVariableDefinition : public Rml::VariableDefinition {
            InputFieldVariableDefinition() : Rml::VariableDefinition(Rml::DataVariableType::Scalar) {}

            virtual bool Get(void* ptr, Rml::Variant& variant) override { variant = reinterpret_cast<recomp::InputField*>(ptr)->to_string(); return true; }
            virtual bool Set(void* ptr, const Rml::Variant& variant) override { return false; }
        };
        // Static instance of the InputField variable definition to have a pointer to return to RmlUi.
        static InputFieldVariableDefinition input_field_definition_instance{};

        // Rml variable definition for an array of InputField values (e.g. all the bindings for a single input).
        struct BindingContainerVariableDefinition : public Rml::VariableDefinition {
            BindingContainerVariableDefinition() : Rml::VariableDefinition(Rml::DataVariableType::Array) {}

            virtual bool Get(void* ptr, Rml::Variant& variant) override { return false; }
            virtual bool Set(void* ptr, const Rml::Variant& variant) override { return false; }

            virtual int Size(void* ptr) override { return recomp::bindings_per_input; }
            virtual Rml::DataVariable Child(void* ptr, const Rml::DataAddressEntry& address) override {
                recomp::GameInput input = static_cast<recomp::GameInput>((uintptr_t)ptr);
                return Rml::DataVariable{&input_field_definition_instance, &recomp::get_input_binding(input, address.index, cur_device)};
            }
        };
        // Static instance of the InputField array variable definition to have a fixed pointer to return to RmlUi.
        static BindingContainerVariableDefinition binding_container_var_instance{};

        // Rml variable definition for an array of an array of InputField values (e.g. all the bindings for all inputs).
        struct BindingArrayContainerVariableDefinition : public Rml::VariableDefinition {
            BindingArrayContainerVariableDefinition() : Rml::VariableDefinition(Rml::DataVariableType::Array) {}

            virtual bool Get(void* ptr, Rml::Variant& variant) override { return false; }
            virtual bool Set(void* ptr, const Rml::Variant& variant) override { return false; }

            virtual int Size(void* ptr) override { return recomp::get_num_inputs(); }
            virtual Rml::DataVariable Child(void* ptr, const Rml::DataAddressEntry& address) override {
                // Encode the input index as the pointer to avoid needing to do any allocations.
                return Rml::DataVariable(&binding_container_var_instance, (void*)(uintptr_t)address.index);
            }
        };

        // Static instance of the BindingArrayContainerVariableDefinition variable definition to have a fixed pointer to return to RmlUi.
        static BindingArrayContainerVariableDefinition binding_array_var_instance{};

        struct InputContainerVariableDefinition : public Rml::VariableDefinition {
            InputContainerVariableDefinition() : Rml::VariableDefinition(Rml::DataVariableType::Struct) {}

            virtual bool Get(void* ptr, Rml::Variant& variant) override { return true; }
            virtual bool Set(void* ptr, const Rml::Variant& variant) override { return false; }

            virtual int Size(void* ptr) override { return recomp::get_num_inputs(); }
            virtual Rml::DataVariable Child(void* ptr, const Rml::DataAddressEntry& address) override {
                if (address.name == "array") {
                    return Rml::DataVariable(&binding_array_var_instance, nullptr);
                }
                else {
                    recomp::GameInput input = recomp::get_input_from_enum_name(address.name);
                    if (input != recomp::GameInput::COUNT) {
                        return Rml::DataVariable(&binding_container_var_instance, (void*)(uintptr_t)input);
                    }
                }
                return Rml::DataVariable{};
            }
        };

        // Dummy type to associate with the variable definition.
        struct InputContainer {};
        constructor.RegisterCustomDataVariableDefinition<InputContainer>(Rml::MakeUnique<InputContainerVariableDefinition>());

        // Dummy instance of the dummy type to bind to the variable.
        static InputContainer dummy_container;
        constructor.Bind("inputs", &dummy_container);

        constructor.BindFunc("cur_input_row", [](Rml::Variant& out) {
            if (focused_input_index == -1) {
                out = "NONE";
            }
            else {
                out = recomp::get_input_enum_name(static_cast<recomp::GameInput>(focused_input_index));
            }
        });

        constructor.BindFunc("active_binding_input", [](Rml::Variant& out) {
            if (scanned_input_index == -1) {
                out = "NONE";
            }
            else {
                out = recomp::get_input_enum_name(static_cast<recomp::GameInput>(scanned_input_index));
            }
        });

        constructor.Bind<int>("active_binding_slot", &scanned_binding_index);

        constructor.Bind<int>("cur_input_index", &focused_input_index);

        controls_model_handle = constructor.GetModelHandle();
    }

    void make_nav_help_bindings(Rml::Context* context) {
        Rml::DataModelConstructor constructor = context->CreateDataModel("nav_help_model");
        if (!constructor) {
            throw std::runtime_error("Failed to make RmlUi data model for nav help");
        }

        constructor.BindFunc("nav_help__navigate", [](Rml::Variant& out) {
            if (cont_active) {
                out = PF_DPAD;
            } else {
                out = PF_KEYBOARD_ARROWS PF_KEYBOARD_TAB;
            }
        });

        constructor.BindFunc("nav_help__accept", [](Rml::Variant& out) {
            if (cont_active) {
                out = \
                    recomp::get_input_binding(recomp::GameInput::ACCEPT_MENU, 0, recomp::InputDevice::Controller).to_string() + \
                    recomp::get_input_binding(recomp::GameInput::ACCEPT_MENU, 1, recomp::InputDevice::Controller).to_string();
            } else {
                out = PF_KEYBOARD_ENTER;
            }
        });

        constructor.BindFunc("nav_help__exit", [](Rml::Variant& out) {
            if (cont_active) {
                out = \
                    recomp::get_input_binding(recomp::GameInput::TOGGLE_MENU, 0, recomp::InputDevice::Controller).to_string() + \
                    recomp::get_input_binding(recomp::GameInput::TOGGLE_MENU, 1, recomp::InputDevice::Controller).to_string();
            } else {
                out = PF_KEYBOARD_ESCAPE;
            }
        });

        // Register the array type for string vectors. RmlUi's type register is
        // per-Context rather than per-model, so this one call serves every model
        // in the config menu and must not be repeated -- the GPU driver model
        // binds a std::vector<std::string> of its own on the strength of it.
        //
        // It lives here, in the first model built, because Goemon64Recomp made
        // the call from its Debug menu's bindings and this port has no Debug
        // menu: dropping that function without moving this would leave the
        // driver menu's list binding unregistered.
        constructor.RegisterArray<std::vector<std::string>>();

        nav_help_model_handle = constructor.GetModelHandle();
    }

    void make_general_bindings(Rml::Context* context) {
        Rml::DataModelConstructor constructor = context->CreateDataModel("general_model");
        if (!constructor) {
            throw std::runtime_error("Failed to make RmlUi data model for the control options menu");
        }

        bind_config_list_events(constructor);

        // Get the handle before binding: bind_atomic captures it by value for its
        // DirtyVariable-on-write (mirrors make_sound_options_bindings).
        general_model_handle = constructor.GetModelHandle();

        bind_atomic(constructor, general_model_handle, "rumble_strength", &control_options_context.rumble_strength);
        bind_atomic(constructor, general_model_handle, "gyro_sensitivity", &control_options_context.gyro_sensitivity);
        bind_atomic(constructor, general_model_handle, "mouse_sensitivity", &control_options_context.mouse_sensitivity);
        bind_atomic(constructor, general_model_handle, "joystick_deadzone", &control_options_context.joystick_deadzone);
        bind_atomic_option(constructor, "background_input_mode", &control_options_context.background_input_mode);
        bind_atomic_option(constructor, "analog_cam_mode", &control_options_context.analog_cam_mode);
        bind_atomic_option(constructor, "touch_controls_mode", &control_options_context.touch_controls_mode);
        // Gates the Touch tab, the same way driver_supported gates the GPU Driver
        // tab. The tab's only action -- touch_edit_layout -- is registered inside
        // #if defined(__ANDROID__), so without this a desktop build would show a tab
        // whose button resolves to no listener and silently does nothing, next to a
        // setting that drives nothing.
        constructor.Bind("touch_supported", &touch_supported);
        bind_atomic(constructor, general_model_handle, "touch_stick_sensitivity",
                    &control_options_context.touch_stick_sensitivity);
        bind_atomic_option(constructor, "analog_camera_invert_mode", &control_options_context.analog_camera_invert_mode);
        bind_atomic(constructor, general_model_handle, "analog_cam_sensitivity_x", &control_options_context.analog_cam_sensitivity_x);
        bind_atomic(constructor, general_model_handle, "analog_cam_sensitivity_y", &control_options_context.analog_cam_sensitivity_y);
        bind_atomic_option(constructor, "autosave_mode", &control_options_context.autosave_mode);
        constructor.BindFunc("debug_mode",
            [](Rml::Variant& out) { out = debug_mode_enabled.load(); },
            [](const Rml::Variant& in) {
                debug_mode_enabled.store(in.Get<bool>());
                general_model_handle.DirtyVariable("debug_mode");
            });
    }

    void make_sound_options_bindings(Rml::Context* context) {
        Rml::DataModelConstructor constructor = context->CreateDataModel("sound_options_model");
        if (!constructor) {
            throw std::runtime_error("Failed to make RmlUi data model for the sound options menu");
        }

        bind_config_list_events(constructor);
        
        sound_options_model_handle = constructor.GetModelHandle();

        bind_atomic(constructor, sound_options_model_handle, "main_volume", &sound_options_context.main_volume);
    }

    void make_bindings(Rml::Context* context) override {
        // initially set cont state for ui help
        //recomp::config_menu_set_cont_or_kb(recompui::get_cont_active());
        make_nav_help_bindings(context);
        make_general_bindings(context);
        make_controls_bindings(context);
        make_graphics_bindings(context);
        make_sound_options_bindings(context);
        recompui::make_gpu_driver_bindings(context);
        recompui::make_saves_bindings(context);
    }
};

std::unique_ptr<recompui::MenuController> recompui::create_config_menu() {
    return std::make_unique<ConfigMenu>();
}

bool hybridheaven::get_debug_mode_enabled() {
    return debug_mode_enabled.load();
}

void hybridheaven::set_debug_mode_enabled(bool enabled) {
    debug_mode_enabled.store(enabled);
    if (general_model_handle) {
        general_model_handle.DirtyVariable("debug_mode");
    }
}

void recompui::update_supported_options() {
    msaa2x_supported = hybridheaven::renderer::RT64MaxMSAA() >= RT64::UserConfiguration::Antialiasing::MSAA2X;
    msaa4x_supported = hybridheaven::renderer::RT64MaxMSAA() >= RT64::UserConfiguration::Antialiasing::MSAA4X;
    msaa8x_supported = hybridheaven::renderer::RT64MaxMSAA() >= RT64::UserConfiguration::Antialiasing::MSAA8X;
    sample_positions_supported = hybridheaven::renderer::RT64SamplePositionsSupported();
    
    new_options = ultramodern::renderer::get_graphics_config();

    graphics_model_handle.DirtyAllVariables();
}

void recompui::toggle_fullscreen() {
    new_options.wm_option = (new_options.wm_option == ultramodern::renderer::WindowMode::Windowed) ? ultramodern::renderer::WindowMode::Fullscreen : ultramodern::renderer::WindowMode::Windowed;
    apply_graphics_config();
    graphics_model_handle.DirtyVariable("wm_option");
}

void recompui::set_config_tab(ConfigTab tab) {
    get_config_tabset()->SetActiveTab(config_tab_to_index(tab));
}

Rml::ElementTabSet* recompui::get_config_tabset() {
    ContextId config_context = recompui::get_config_context_id();

    ContextId old_context = recompui::try_close_current_context();

    Rml::ElementDocument *doc = config_context.get_document();
    assert(doc != nullptr);

    Rml::Element *tabset_el = doc->GetElementById("config_tabset");
    assert(tabset_el != nullptr);

    Rml::ElementTabSet *tabset = rmlui_dynamic_cast<Rml::ElementTabSet *>(tabset_el);
    assert(tabset != nullptr);

    if (old_context != ContextId::null()) {
        old_context.open();
    }

    return tabset;
}

Rml::Element* recompui::get_mod_tab() {
    ContextId config_context = recompui::get_config_context_id();

    ContextId old_context = recompui::try_close_current_context();

    Rml::ElementDocument* doc = config_context.get_document();
    assert(doc != nullptr);

    Rml::Element* tab_el = doc->GetElementById("tab_mods");
    assert(tab_el != nullptr);

    if (old_context != ContextId::null()) {
        old_context.open();
    }

    return tab_el;
}
