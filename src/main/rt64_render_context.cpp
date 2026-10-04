/**
 * The RT64-backed renderer.
 *
 * This is what turns the display lists the game already produces into a
 * picture. Everything below it was working long before this file existed --
 * see null_render_context.cpp, which is still the right thing to build against
 * when bisecting, since a failure that survives it is not a renderer bug.
 *
 * Derived from Goemon64Recomp's src/main/rt64_render_context.cpp, with the
 * texture-pack action queue left out because this port does not have it. The
 * Android window-handoff path was left out too until the Android target existed;
 * it is back, and it is the reason for the __ANDROID__ blocks below.
 */

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <vector>

#if defined(__ANDROID__)
// To pull the ANativeWindow back out of the SDL_Window for RT64's RenderWindow.
#include <SDL.h>
#include <SDL_syswm.h>
// ANativeWindow_acquire/_release, for the resume-window ownership invariant.
#include <android/native_window.h>
#include "hh_support.h"
#endif

#define HLSL_CPU
#include "common/rt64_device_workarounds.h"
#include "hle/rt64_application.h"

#include "ultramodern/ultramodern.hpp"
#include "ultramodern/config.hpp"
#include "ultramodern/renderer_context.hpp"

#include "hh_render.h"
#include "hh_widescreen.h"

// Defined in main.cpp -- diagnostic guest-memory read for the FrameProbe, and
// the HH_FRAME_GOVERNOR_OFF experiment poke.
namespace hybridheaven {
uint32_t debug_read_guest_u32(uint32_t vaddr);
void apply_frame_governor_override();
}

#if defined(HH_UI)
#   include "recomp_ui.h"
#endif

#if defined(__ANDROID__)
// A fresh ANativeWindow, published by the SDL/main thread on resume and consumed
// by the gfx thread in update_screen to hand down to the swap chain. A global
// atomic rather than a method on the context, so the main thread never needs a
// reference to a RendererContext the gfx thread owns; each end touches only its
// own side.
//
// Ordering: publish stores with release, consume exchanges with acq_rel. SDL has
// already run surfaceCreated/surfaceChanged synchronously on the UI thread before
// DIDENTERFOREGROUND, so the window is fully constructed before the store, and
// release/acquire on the pointer alone is enough to hand it over safely on ARM's
// weak memory model.
//
// OWNERSHIP: this slot owns exactly one reference. SDL drops its own reference a
// bounded ~500ms after surfaceDestroyed no matter who is still using the window,
// so a bare pointer parked here could be freed before the gfx thread adopts it --
// which is reachable by backgrounding and foregrounding quickly. Acquiring at
// publish keeps it alive while it is in our custody, and the reference is handed
// down to the swap chain on consume. A publish that supersedes a window nobody
// consumed releases the old one rather than leaking it.
static std::atomic<void*> g_pending_resume_window{nullptr};

namespace hybridheaven::renderer {
    void android_publish_resume_window(void* native_window) {
        if (native_window != nullptr) {
            ANativeWindow_acquire(static_cast<ANativeWindow*>(native_window));
        }
        void* prev = g_pending_resume_window.exchange(native_window, std::memory_order_acq_rel);
        if (prev != nullptr) {
            ANativeWindow_release(static_cast<ANativeWindow*>(prev));
        }
    }
}
#endif

// RT64 reads and writes these through its `Core` struct. They are the RCP
// registers the renderer cares about; nothing here emulates them, and the DPC
// ones exist only so RT64 has somewhere to point.
static uint8_t DMEM[0x1000];
static uint8_t IMEM[0x1000];

unsigned int MI_INTR_REG = 0;

unsigned int DPC_START_REG = 0;
unsigned int DPC_END_REG = 0;
unsigned int DPC_CURRENT_REG = 0;
unsigned int DPC_STATUS_REG = 0;
unsigned int DPC_CLOCK_REG = 0;
unsigned int DPC_BUFBUSY_REG = 0;
unsigned int DPC_PIPEBUSY_REG = 0;
unsigned int DPC_TMEM_REG = 0;

// ultramodern raises the interrupts itself, so RT64's hook has nothing to do.
static void dummy_check_interrupts() {}

static RT64::UserConfiguration::Antialiasing device_max_msaa = RT64::UserConfiguration::Antialiasing::None;
static bool sample_positions_supported = false;
static bool high_precision_fb_enabled = false;

static RT64::UserConfiguration::Antialiasing compute_max_supported_aa(plume::RenderSampleCounts bits) {
    if (bits & plume::RenderSampleCount::Bits::COUNT_2) {
        if (bits & plume::RenderSampleCount::Bits::COUNT_4) {
            if (bits & plume::RenderSampleCount::Bits::COUNT_8) {
                return RT64::UserConfiguration::Antialiasing::MSAA8X;
            }
            return RT64::UserConfiguration::Antialiasing::MSAA4X;
        }
        return RT64::UserConfiguration::Antialiasing::MSAA2X;
    }
    return RT64::UserConfiguration::Antialiasing::None;
}

static RT64::UserConfiguration::AspectRatio to_rt64(ultramodern::renderer::AspectRatio option) {
    switch (option) {
        case ultramodern::renderer::AspectRatio::Original:    return RT64::UserConfiguration::AspectRatio::Original;
        case ultramodern::renderer::AspectRatio::Expand:      return RT64::UserConfiguration::AspectRatio::Expand;
        case ultramodern::renderer::AspectRatio::Manual:      return RT64::UserConfiguration::AspectRatio::Manual;
        case ultramodern::renderer::AspectRatio::OptionCount: return RT64::UserConfiguration::AspectRatio::OptionCount;
    }
    return RT64::UserConfiguration::AspectRatio::Original;
}

static RT64::UserConfiguration::Antialiasing to_rt64(ultramodern::renderer::Antialiasing option) {
    switch (option) {
        case ultramodern::renderer::Antialiasing::None:        return RT64::UserConfiguration::Antialiasing::None;
        case ultramodern::renderer::Antialiasing::MSAA2X:      return RT64::UserConfiguration::Antialiasing::MSAA2X;
        case ultramodern::renderer::Antialiasing::MSAA4X:      return RT64::UserConfiguration::Antialiasing::MSAA4X;
        case ultramodern::renderer::Antialiasing::MSAA8X:      return RT64::UserConfiguration::Antialiasing::MSAA8X;
        case ultramodern::renderer::Antialiasing::OptionCount: return RT64::UserConfiguration::Antialiasing::OptionCount;
    }
    return RT64::UserConfiguration::Antialiasing::None;
}

static RT64::UserConfiguration::RefreshRate to_rt64(ultramodern::renderer::RefreshRate option) {
    switch (option) {
        case ultramodern::renderer::RefreshRate::Original:    return RT64::UserConfiguration::RefreshRate::Original;
        case ultramodern::renderer::RefreshRate::Display:     return RT64::UserConfiguration::RefreshRate::Display;
        case ultramodern::renderer::RefreshRate::Manual:      return RT64::UserConfiguration::RefreshRate::Manual;
        case ultramodern::renderer::RefreshRate::OptionCount: return RT64::UserConfiguration::RefreshRate::OptionCount;
    }
    return RT64::UserConfiguration::RefreshRate::Original;
}

static RT64::UserConfiguration::InternalColorFormat to_rt64(ultramodern::renderer::HighPrecisionFramebuffer option) {
    switch (option) {
        case ultramodern::renderer::HighPrecisionFramebuffer::Off:         return RT64::UserConfiguration::InternalColorFormat::Standard;
        case ultramodern::renderer::HighPrecisionFramebuffer::On:          return RT64::UserConfiguration::InternalColorFormat::High;
        case ultramodern::renderer::HighPrecisionFramebuffer::Auto:        return RT64::UserConfiguration::InternalColorFormat::Automatic;
        case ultramodern::renderer::HighPrecisionFramebuffer::OptionCount: return RT64::UserConfiguration::InternalColorFormat::OptionCount;
    }
    return RT64::UserConfiguration::InternalColorFormat::Automatic;
}

static void set_application_user_config(RT64::Application* application,
                                        const ultramodern::renderer::GraphicsConfig& config) {
    switch (config.res_option) {
        default:
        case ultramodern::renderer::Resolution::Auto:
            application->userConfig.resolution = RT64::UserConfiguration::Resolution::WindowIntegerScale;
            application->userConfig.downsampleMultiplier = 1;
            break;
        case ultramodern::renderer::Resolution::Original:
            application->userConfig.resolution = RT64::UserConfiguration::Resolution::Manual;
            application->userConfig.resolutionMultiplier = std::max(config.ds_option, 1);
            application->userConfig.downsampleMultiplier = std::max(config.ds_option, 1);
            break;
        case ultramodern::renderer::Resolution::Original2x:
            application->userConfig.resolution = RT64::UserConfiguration::Resolution::Manual;
            application->userConfig.resolutionMultiplier = 2.0 * std::max(config.ds_option, 1);
            application->userConfig.downsampleMultiplier = std::max(config.ds_option, 1);
            break;
    }

    // The HUD ratio, which the config menu has always offered and this port has
    // never sent anywhere. Goemon's mapping exactly: RT64 keeps a second,
    // "extended GBI" aspect ratio for elements the display list has tagged with
    // an origin, and drives them from `extAspectPercentage` -- 1.0 pins a tagged
    // element to the widened edge, 0.0 leaves it at the framebuffer centre.
    //
    // **It changes nothing until something tags a display list.** RT64 reads
    // those origins only from the extended GBI (`rt64_gbi_extended.cpp`), and
    // `computeOrigin` falls back to the framebuffer centre for anything left at
    // `G_EX_ORIGIN_NONE` -- which is everything this game emits, and everything
    // Goemon emits too; neither port tags a rect. Wired anyway, because the
    // option is *in the menu*: a setting that reaches nothing is at least
    // consistent between the two ports, and this is the half a HUD patch would
    // otherwise have to discover missing.
    switch (config.hr_option) {
        default:
        case ultramodern::renderer::HUDRatioMode::Original:
            application->userConfig.extAspectRatio = RT64::UserConfiguration::AspectRatio::Original;
            break;
        case ultramodern::renderer::HUDRatioMode::Clamp16x9:
            application->userConfig.extAspectRatio = RT64::UserConfiguration::AspectRatio::Manual;
            application->userConfig.extAspectTarget = 16.0 / 9.0;
            break;
        case ultramodern::renderer::HUDRatioMode::Full:
            application->userConfig.extAspectRatio = RT64::UserConfiguration::AspectRatio::Expand;
            break;
    }

    application->userConfig.aspectRatio = to_rt64(config.ar_option);
    application->userConfig.antialiasing = to_rt64(config.msaa_option);
    application->userConfig.refreshRate = to_rt64(config.rr_option);
    application->userConfig.refreshRateTarget = config.rr_manual_value;
    application->userConfig.internalColorFormat = to_rt64(config.hpfb_option);
    application->userConfig.displayBuffering = RT64::UserConfiguration::DisplayBuffering::Triple;

    // Host swap-chain vsync. RT64 upstream has no field for this and plume's
    // Vulkan swap chain hardcodes it on at creation, so `userConfig.vsync` is a
    // LOCAL ADDITION to the rt64 submodule (rt64_user_configuration.h/.cpp and
    // the setVsyncEnabled calls in rt64_application.cpp). If a submodule update
    // ever drops those, this line stops compiling -- which is the intended
    // failure mode, since silently losing the patch is the worse one.
    application->userConfig.vsync = (config.vsync_option != ultramodern::renderer::VSync::Off);

    // Say which way vsync is set, once and on every change, for the same reason
    // the aspect-ratio line below exists: a graphics setting that silently fails
    // to apply is this port's recurring failure mode, and vsync has no visible
    // symptom on a display that never tears. Note Vulkan only honours Off where
    // the device reports IMMEDIATE support -- plume falls back to FIFO
    // otherwise, so this line states what was REQUESTED, not what was granted.
    {
        static ultramodern::renderer::VSync last_vsync = ultramodern::renderer::VSync::OptionCount;
        if (config.vsync_option != last_vsync) {
            fprintf(stderr, "[gfx] vsync requested %s\n",
                    (config.vsync_option == ultramodern::renderer::VSync::Off) ? "Off" : "On");
            last_vsync = config.vsync_option;
        }
    }

    // Not part of userConfig: the sites that read this are deep inside command
    // recording with no path to the configuration. See rt64_device_workarounds.h.
    RT64::SetFramebufferSyncEnabled(config.fbe_option != ultramodern::renderer::FramebufferEffects::Off);

    // Say which aspect ratio is in force, once and again on every change. This
    // is not a trace switch because of how the question came up: the shipped
    // default is Expand, the saved `graphics.json` on the machine the port is
    // developed on said Original, and `from_or_default` only fills in a default
    // for a key that is *absent* -- so widescreen was off and there was no way
    // to tell from a log. `Original` here means the game is being drawn 4:3
    // whatever the window's shape.
    {
        static ultramodern::renderer::AspectRatio last_ar = ultramodern::renderer::AspectRatio::OptionCount;
        static ultramodern::renderer::HUDRatioMode last_hr = ultramodern::renderer::HUDRatioMode::OptionCount;
        if (config.ar_option != last_ar || config.hr_option != last_hr) {
            const char* ar = "?";
            switch (config.ar_option) {
                case ultramodern::renderer::AspectRatio::Original: ar = "Original (4:3)"; break;
                case ultramodern::renderer::AspectRatio::Expand:   ar = "Expand (window)"; break;
                case ultramodern::renderer::AspectRatio::Manual:   ar = "Manual"; break;
                default: break;
            }
            const char* hr = "?";
            switch (config.hr_option) {
                case ultramodern::renderer::HUDRatioMode::Original:  hr = "Original"; break;
                case ultramodern::renderer::HUDRatioMode::Clamp16x9: hr = "Clamp16x9"; break;
                case ultramodern::renderer::HUDRatioMode::Full:      hr = "Full"; break;
                default: break;
            }
            fprintf(stderr, "[gfx] aspect %s, HUD ratio %s\n", ar, hr);
            last_ar = config.ar_option;
            last_hr = config.hr_option;
        }
    }
}

static ultramodern::renderer::SetupResult map_setup_result(RT64::Application::SetupResult rt64_result) {
    switch (rt64_result) {
        case RT64::Application::SetupResult::Success:                 return ultramodern::renderer::SetupResult::Success;
        case RT64::Application::SetupResult::DynamicLibrariesNotFound: return ultramodern::renderer::SetupResult::DynamicLibrariesNotFound;
        case RT64::Application::SetupResult::InvalidGraphicsAPI:      return ultramodern::renderer::SetupResult::InvalidGraphicsAPI;
        case RT64::Application::SetupResult::GraphicsAPINotFound:     return ultramodern::renderer::SetupResult::GraphicsAPINotFound;
        case RT64::Application::SetupResult::GraphicsDeviceNotFound:  return ultramodern::renderer::SetupResult::GraphicsDeviceNotFound;
    }
    return ultramodern::renderer::SetupResult::InvalidGraphicsAPI;
}

static ultramodern::renderer::GraphicsApi map_graphics_api(RT64::UserConfiguration::GraphicsAPI api) {
    switch (api) {
        case RT64::UserConfiguration::GraphicsAPI::D3D12:     return ultramodern::renderer::GraphicsApi::D3D12;
        case RT64::UserConfiguration::GraphicsAPI::Vulkan:    return ultramodern::renderer::GraphicsApi::Vulkan;
        case RT64::UserConfiguration::GraphicsAPI::Metal:     return ultramodern::renderer::GraphicsApi::Metal;
        case RT64::UserConfiguration::GraphicsAPI::Automatic: return ultramodern::renderer::GraphicsApi::Auto;
    }
    return ultramodern::renderer::GraphicsApi::Auto;
}

hybridheaven::renderer::RT64Context::RT64Context(uint8_t* rdram,
                                                 ultramodern::renderer::WindowHandle window_handle,
                                                 bool debug) {
    // RT64 wants a ROM header to fingerprint the game for its per-title
    // workarounds. This port has no use for those, so it gets zeroes.
    static unsigned char dummy_rom_header[0x40];

#if defined(__ANDROID__)
    // Publish the RDRAM base so the crash classifier can tell an out-of-bounds
    // recompiled guest pointer from an ordinary host fault. This is the first
    // callback the port gets that is handed the base, same as on desktop.
    hybridheaven::diag::set_rdram_base(rdram);
#endif

#if defined(HH_UI)
    // Hands RT64 the callbacks it draws the interface through, so the launcher
    // and menus composite over the frame. Before app->setup(): the hooks have to
    // be in place by the time RT64 has a device to invoke them with.
    recompui::set_render_hooks();
#endif

    RT64::Application::Core appCore{};
#if defined(_WIN32)
    appCore.window = window_handle.window;
#elif defined(__APPLE__)
    appCore.window.window = window_handle.window;
    appCore.window.view = window_handle.view;
#elif defined(__ANDROID__)
    // Android is NOT the plain-Linux case, even though it reaches the same #else
    // by default -- and being wrong here is silent, because both branches compile.
    // On Linux RT64's RenderWindow IS the window handle. On Android it is an
    // ANativeWindow*, because Plume builds the Vulkan surface straight from it
    // with vkCreateAndroidSurfaceKHR rather than going through SDL. The
    // ultramodern WindowHandle is still the SDL_Window*, so the native window has
    // to be extracted from it.
    {
        SDL_SysWMinfo wmInfo;
        SDL_VERSION(&wmInfo.version);
        SDL_GetWindowWMInfo(window_handle, &wmInfo);
        appCore.window = wmInfo.info.android.window;
    }
#else
    appCore.window = window_handle;
#endif

    appCore.checkInterrupts = dummy_check_interrupts;

    appCore.HEADER = dummy_rom_header;
    appCore.RDRAM = rdram;
    appCore.DMEM = DMEM;
    appCore.IMEM = IMEM;

    appCore.MI_INTR_REG = &MI_INTR_REG;

    appCore.DPC_START_REG = &DPC_START_REG;
    appCore.DPC_END_REG = &DPC_END_REG;
    appCore.DPC_CURRENT_REG = &DPC_CURRENT_REG;
    appCore.DPC_STATUS_REG = &DPC_STATUS_REG;
    appCore.DPC_CLOCK_REG = &DPC_CLOCK_REG;
    appCore.DPC_BUFBUSY_REG = &DPC_BUFBUSY_REG;
    appCore.DPC_PIPEBUSY_REG = &DPC_PIPEBUSY_REG;
    appCore.DPC_TMEM_REG = &DPC_TMEM_REG;

    ultramodern::renderer::ViRegs* vi_regs = ultramodern::renderer::get_vi_regs();

    appCore.VI_STATUS_REG = &vi_regs->VI_STATUS_REG;
    appCore.VI_ORIGIN_REG = &vi_regs->VI_ORIGIN_REG;
    appCore.VI_WIDTH_REG = &vi_regs->VI_WIDTH_REG;
    appCore.VI_INTR_REG = &vi_regs->VI_INTR_REG;
    appCore.VI_V_CURRENT_LINE_REG = &vi_regs->VI_V_CURRENT_LINE_REG;
    appCore.VI_TIMING_REG = &vi_regs->VI_TIMING_REG;
    appCore.VI_V_SYNC_REG = &vi_regs->VI_V_SYNC_REG;
    appCore.VI_H_SYNC_REG = &vi_regs->VI_H_SYNC_REG;
    appCore.VI_LEAP_REG = &vi_regs->VI_LEAP_REG;
    appCore.VI_H_START_REG = &vi_regs->VI_H_START_REG;
    appCore.VI_V_START_REG = &vi_regs->VI_V_START_REG;
    appCore.VI_V_BURST_REG = &vi_regs->VI_V_BURST_REG;
    appCore.VI_X_SCALE_REG = &vi_regs->VI_X_SCALE_REG;
    appCore.VI_Y_SCALE_REG = &vi_regs->VI_Y_SCALE_REG;

    RT64::ApplicationConfiguration appConfig;
    appConfig.useConfigurationFile = false;
#if defined(__ANDROID__)
    // RT64's detectDataPath() takes its __linux__ branch here, and that branch is
    // wrong for an Android app: HOME is unset, so it falls back to
    // getpwuid()->pw_dir, which is "/". It then throws out of
    // std::filesystem::create_directories("/.rt64") with EACCES, and nothing on
    // the desktop path catches it -- the result is a SIGABRT on the gfx thread
    // during setup, which reads as a renderer failure rather than a path one.
    // Point it at the app-private directory the assets and config already resolve
    // under, and turn the detection off so it never looks at the filesystem root.
    appConfig.detectDataPath = false;
    appConfig.dataPath = hybridheaven::android_program_path();
#endif

    app = std::make_unique<RT64::Application>(appCore, appConfig);

    auto& cur_config = ultramodern::renderer::get_graphics_config();
    set_application_user_config(app.get(), cur_config);
    app->userConfig.developerMode = debug;
    // Prevent the upscaling of texture rectangles.
    app->userConfig.upscale2D = RT64::UserConfiguration::Upscale2D::Original;
    // Force gbi depth branches to prevent LODs from kicking in.
    app->enhancementConfig.f3dex.forceBranch = true;
    // Scale LODs based on the output resolution.
    app->enhancementConfig.textureLOD.scale = true;
    // Goemon disables this because GPU copying breaks its menu effects. Kept the
    // same here until there is a picture to judge it against.
    app->emulatorConfig.framebuffer.copyWithGPU = false;

    // Widescreen needs nothing here. Setting `ar_option` to Expand is the whole of
    // the renderer's part, as it is in Goemon: RT64 widens the render target and
    // works out for itself which projection is the 3D view. The reason that
    // silently failed for this game -- its 3D view is scissored to the middle 90%
    // of the framebuffer, so RT64 read it as a sub-panel and refused to widen it --
    // is fixed on the game's side of the line instead, in src/main/widescreen.cpp,
    // which is also where the finding is written up. `HH_TRACE_ASPECT=1` reports
    // the ratio scale RT64 actually applied, which is what separates "the setting
    // is on" from "the picture is wider".

    switch (cur_config.api_option) {
        case ultramodern::renderer::GraphicsApi::D3D12:
            app->userConfig.graphicsAPI = RT64::UserConfiguration::GraphicsAPI::D3D12;
            break;
        case ultramodern::renderer::GraphicsApi::Vulkan:
            app->userConfig.graphicsAPI = RT64::UserConfiguration::GraphicsAPI::Vulkan;
            break;
        case ultramodern::renderer::GraphicsApi::Metal:
            app->userConfig.graphicsAPI = RT64::UserConfiguration::GraphicsAPI::Metal;
            break;
        case ultramodern::renderer::GraphicsApi::Auto:
            app->userConfig.graphicsAPI = RT64::UserConfiguration::GraphicsAPI::Automatic;
            break;
    }

    uint32_t thread_id = 0;
#ifdef _WIN32
    thread_id = window_handle.thread_id;
#endif
    setup_result = map_setup_result(app->setup(thread_id));
    chosen_api = map_graphics_api(app->chosenGraphicsAPI);
    if (setup_result != ultramodern::renderer::SetupResult::Success) {
        app = nullptr;
        return;
    }

#if defined(__ANDROID__)
    // Report the device we actually got. This is where an optional user-supplied
    // Vulkan driver is confirmed or refuted: loading one succeeds even when the
    // system driver ends up being used, so the selected device name is the only
    // evidence of which driver rendered. It deliberately does NOT clear the boot
    // latch -- reaching here only means Vulkan initialised, which is earlier than
    // the fault this feature exists for; see the definition in android_glue.cpp.
    // A no-op in a build without custom-driver support.
    hybridheaven::report_render_device(app->device->getDescription().name.c_str());
#endif

    app->setFullScreen(cur_config.wm_option == ultramodern::renderer::WindowMode::Fullscreen);

    // Downgrade the configuration to what the device actually supports rather
    // than asking for MSAA it cannot do.
    if (app->device->getCapabilities().sampleLocations) {
        plume::RenderSampleCounts color_sample_counts = app->device->getSampleCountsSupported(plume::RenderFormat::R8G8B8A8_UNORM);
        plume::RenderSampleCounts depth_sample_counts = app->device->getSampleCountsSupported(plume::RenderFormat::D32_FLOAT);
        plume::RenderSampleCounts common_sample_counts = color_sample_counts & depth_sample_counts;
        device_max_msaa = compute_max_supported_aa(common_sample_counts);
        sample_positions_supported = true;
    }
    else {
        device_max_msaa = RT64::UserConfiguration::Antialiasing::None;
        sample_positions_supported = false;
    }

    high_precision_fb_enabled = app->shaderLibrary->usesHDR;
}

hybridheaven::renderer::RT64Context::~RT64Context() = default;

// ---------------------------------------------------------------------------
// Where a graphics task comes from, and how to tell a null one apart
// ---------------------------------------------------------------------------
//
// Hybrid Heaven drives the RSP through a scheduler of libultra's shape.
// `func_80001BC0_27C0` builds an array of **three** OSScTask-shaped records at
// `0x8005C3A8`, stride 0x58, with the OSTask at +0x10 -- so `data_ptr` is +0x40
// from the record's base. That init writes `data_ptr = 0`, `data_size = 0`,
// `output_buff_size = 0` and `ucode_boot_size = 0`; the only thing that ever
// fills them is `func_80001D5C_295C`, the end-of-frame call, which sets
//
//     data_ptr         = D_800692B0 + (s16)D_8008934C * 0x10048
//     data_size        = (D_8008D5BC - data_ptr) & ~7
//     output_buff_size = D_800642B0 + 0x4000
//     ucode_boot_size  = 0xD0
//
// `D_800692B0` is a fixed address in `.main`, so **that expression can never
// evaluate to zero** for any buffer index. A submitted task with `data_ptr == 0`
// is therefore a record that has not been through the fill -- and the two
// _size fields say so independently, which is what separates "never filled"
// from a torn write.
//
// That is what happens. `func_80001454_2054` reaches `func_80001BC0_27C0` at the
// video-mode change that ends the "Memory Pak Enhanced" screen and re-initialises
// all three records, including one whose pointer is already sitting in the
// scheduler's task queue. The game overwrites a task it has already handed off.
// The window is this port's renderer latency: the game's scheduler waits on
// `dp_complete()`, which the gfx thread sends only after RT64 has rendered, so
// the queue backs up while the retrace-driven state machine runs on. One frame
// later the game is drawing normally again.
//
// Handing that list to RT64 costs a whole frame of runaway before the command
// ceiling in `Interpreter::processDisplayLists` catches it, so `send_dl` drops
// it here instead. `HH_NO_DL_GUARD=1` is the A/B.

static bool trace_gfx() {
    static const bool enabled = getenv("HH_TRACE_GFX") != nullptr;
    return enabled;
}

// The A/B for the guard below: with it set, a null display list is handed to
// RT64 the way it used to be, and the command ceiling in
// `Interpreter::processDisplayLists` catches it a million commands later.
static bool no_dl_guard() {
    static const bool disabled = getenv("HH_NO_DL_GUARD") != nullptr;
    return disabled;
}

// rdram holds 32-bit words in host order (only the sub-word accessors carry the
// address xor), so a word is a plain read; a halfword needs the `^ 2` that
// `MEM_H` applies.
static uint32_t guest_w(const uint8_t* rdram, uint32_t vaddr) {
    return *(const uint32_t*)(rdram + (vaddr - 0x80000000u));
}

static int16_t guest_h(const uint8_t* rdram, uint32_t vaddr) {
    return *(const int16_t*)(rdram + ((vaddr ^ 2) - 0x80000000u));
}

static uint8_t guest_b(const uint8_t* rdram, uint32_t vaddr) {
    return *(const uint8_t*)(rdram + ((vaddr ^ 3) - 0x80000000u));
}

// Graphics tasks the game has submitted, which is the frame rate AS THE GAME
// SEES IT rather than as the display does -- the game submits one task per frame
// it believes it drew. Exported because the audio report needs it counted over
// the SAME window it counts audio over: audio production per second and frames
// per second sampled separately cannot be divided by each other, and the whole
// question is whether their ratio is constant.
std::atomic<uint64_t> hybridheaven::renderer::gfx_tasks_submitted{0};

// Returns true when the task is worth handing to RT64.
static bool inspect_gfx_task(const uint8_t* rdram, const OSTask* task) {
    static uint32_t bad_reports = 0;

    const uint64_t frames =
        hybridheaven::renderer::gfx_tasks_submitted.fetch_add(1, std::memory_order_relaxed) + 1;

    const uint32_t data_ptr = (uint32_t)task->t.data_ptr;
    // The game's display-list buffers live in KSEG0 rdram and nowhere else.
    const bool bad = (data_ptr - 0x80000000u) >= 0x00800000u;

    // Every task under the switch; a bad one always, but bounded -- once the
    // frame is abandoned the next one usually looks the same, and the first few
    // are what carry the information.
    if (!trace_gfx() && (!bad || bad_reports >= 8)) {
        return !bad;
    }
    if (bad) {
        ++bad_reports;
    }

    // The scheduler object is `D_8005C4B0`: `+0x898` is the rotating record
    // index `func_80000704_1304` returns, `+0x89C` the pending-task count
    // `func_80000ED0_1AD0` bumps under an interrupt mask, and `+0x8C8` the queue
    // it posts the record pointer to. `+0x88C` is the task the scheduler thread
    // currently has on the RSP.
    //
    // A record is "filled" exactly when `output_buff_size` (+0x3C) is nonzero:
    // the init writes 0 there and the end-of-frame fill writes
    // `D_800642B0 + 0x4000`. So printing +0x3C alongside +0x40 says whether a
    // record has ever been through `func_80001D5C_295C`, which is the thing a
    // null `data_ptr` on its own cannot distinguish from a torn write.
    const uint32_t sc = 0x8005C4B0u;
    const uint32_t rec = 0x8005C3A8u;

    fprintf(stderr,
            "[gfx] f=%llu%s state=%u/%u btn=%u idx=%u pend=%u buf=%d gfxptr=%08X | "
            "r0=%08X/%08X r1=%08X/%08X r2=%08X/%08X | cur=%08X mq=%08X | "
            "task data_ptr=%08X size=%08X obs=%08X ubs=%08X\n",
            (unsigned long long)frames, bad ? " BAD" : "",
            guest_w(rdram, 0x80037730), guest_w(rdram, 0x80037734),
            (unsigned)guest_b(rdram, 0x801BBD54),
            guest_w(rdram, sc + 0x898), guest_w(rdram, sc + 0x89C),
            (int)guest_h(rdram, 0x8008934C), guest_w(rdram, 0x8008D5BC),
            guest_w(rdram, rec + 0 * 0x58 + 0x40), guest_w(rdram, rec + 0 * 0x58 + 0x3C),
            guest_w(rdram, rec + 1 * 0x58 + 0x40), guest_w(rdram, rec + 1 * 0x58 + 0x3C),
            guest_w(rdram, rec + 2 * 0x58 + 0x40), guest_w(rdram, rec + 2 * 0x58 + 0x3C),
            guest_w(rdram, sc + 0x88C), guest_w(rdram, sc + 0x8C8),
            data_ptr, (uint32_t)task->t.data_size,
            (uint32_t)task->t.output_buff_size, (uint32_t)task->t.ucode_boot_size);

    return !bad;
}

// ---------------------------------------------------------------------------
// HH_TRACE_FRAME=1 -- frame pacing, and WHERE a hitch was spent.
//
// The gfx thread is one serial loop (ultramodern/src/events.cpp:440): it pulls
// an action and either walks a display list (send_dl) or presents
// (update_screen). So the interval between two consecutive display lists
// decomposes exactly three ways, and which one grows names the culprit:
//
//   dl      -- inside send_dl. RT64 walking the list. A renderer cost.
//   present -- inside update_screen. RT64 presenting / waiting on the GPU.
//   idle    -- the gfx thread had nothing to do. The GAME did not submit.
//
// That last one is the fork this whole probe exists for. A hitch that is idle
// time is not a graphics bug at all -- it is the guest thread stalled, and
// looking at RT64 for it wastes a session. A hitch that is present time is the
// GPU or the swapchain. A hitch that is dl time is the display list itself.
//
// Reporting is per-spike plus a periodic summary. The summary carries the
// interval BETWEEN spikes, because that is the actual claim being tested: a
// fixed period with a small spread means a timer or a fixed-size buffer
// draining, while a wide spread means it is content-driven. A mean alone cannot
// tell those apart, so the standard deviation is printed with it.
namespace {

struct FrameProbe {
    using clock = std::chrono::steady_clock;

    static double ms(clock::duration d) {
        return std::chrono::duration<double, std::milli>(d).count();
    }

    bool enabled() {
        static const bool on = getenv("HH_TRACE_FRAME") != nullptr;
        return on;
    }

    // A frame is "slow" past this. Default 30 ms -- comfortably past the 16.7 ms
    // the game targets, so ordinary jitter does not fill the log.
    double threshold_ms() {
        static const double t = [] {
            const char* env = getenv("HH_TRACE_FRAME_MS");
            double v = (env != nullptr) ? atof(env) : 0.0;
            return v > 0.0 ? v : 30.0;
        }();
        return t;
    }

    double report_secs() {
        static const double t = [] {
            const char* env = getenv("HH_TRACE_FRAME_REPORT");
            double v = (env != nullptr) ? atof(env) : 0.0;
            return v > 0.0 ? v : 5.0;
        }();
        return t;
    }

    clock::time_point boot = clock::now();
    clock::time_point window_start = clock::now();

    // Set at the top of send_dl; the interval between two of these is the frame.
    clock::time_point last_dl_start{};
    bool have_last = false;

    // Phase totals accumulated since the previous display list, so a spike can
    // be decomposed the moment it is noticed rather than reconstructed later.
    double dl_ms_since = 0.0;       // the PREVIOUS send_dl's own duration
    double present_ms_since = 0.0;  // every update_screen in between

    // Window totals.
    double window_dl_ms = 0.0;
    double window_present_ms = 0.0;
    std::vector<double> intervals;
    uint64_t window_spikes = 0;

    // Spacing between spikes -- the periodicity claim.
    clock::time_point last_spike{};
    bool have_last_spike = false;
    std::vector<double> spike_gaps;

    uint32_t overlays_at_last_spike = 0;

    // update_screen runs once per ScreenUpdateAction, and the VI thread enqueues
    // exactly one of those per VI iteration, unconditionally
    // (ultramodern/src/events.cpp:300) -- before any retrace_count gating. So
    // this count IS the rate at which the port is driving VI, and it is the fork
    // the audio investigation now turns on.
    //
    // The guest's audio task rate is the RETRACE rate: osViSetEvent posts 0x29A
    // per retrace, the scheduler thread (func_80000774_1374) matches it and
    // notifies every client (func_80000A0C_160C), and the audio thread runs
    // exactly one audio frame per notification -- stock audiomgr.c, no polling.
    // The game's target of 736 = 44100/60 says it expects 60 of those a second.
    // Gameplay delivers ~30 audio tasks/s.
    //
    // If this counter reads ~60/s while audio tasks read ~30/s, the VI is being
    // driven correctly and the retraces are being lost inside the guest or at
    // the message queue. If it reads ~30/s, the port is not driving VI at 60 Hz
    // in the first place and nothing downstream can recover it.
    uint64_t window_vi_updates = 0;

    void note_present(double ms_taken) {
        if (!enabled()) {
            return;
        }
        window_vi_updates++;
        present_ms_since += ms_taken;
        window_present_ms += ms_taken;
    }

    void note_dl(double ms_taken) {
        if (!enabled()) {
            return;
        }
        dl_ms_since = ms_taken;
        window_dl_ms += ms_taken;
    }

    // osGetTime calls at the previous frame start. The delta across a spiked
    // interval is the discriminator the whole stall question turns on: the
    // guest's frame governor (func_80001454_2054) busy-waits on osGetTime, so a
    // spike whose interval contains tens of thousands of osGetTime calls
    // stalled in the SPIN (the game chose to wait), while one containing a
    // handful stalled doing WORK (the game was busy). The two point at
    // completely different fixes.
    uint64_t osgt_at_last_dl = 0;

    // Called at the TOP of send_dl, before any work, so the interval measured is
    // frame-start to frame-start.
    void frame_start(uint32_t overlay_loads) {
        if (!enabled()) {
            return;
        }
        auto now = clock::now();
        const uint64_t osgt_now = ultramodern::debug_osgettime_calls();

        if (have_last) {
            const double interval = ms(now - last_dl_start);
            intervals.push_back(interval);

            if (interval >= threshold_ms()) {
                window_spikes++;
                // Whatever the interval was not spent computing, the gfx thread
                // spent waiting on the game.
                const double idle = interval - dl_ms_since - present_ms_since;
                double gap = 0.0;
                if (have_last_spike) {
                    gap = ms(now - last_spike);
                    spike_gaps.push_back(gap);
                }
                fprintf(stderr,
                        "[frame] t=%.2fs SPIKE %.1fms = dl %.1f + present %.1f + idle %.1f"
                        " | %.2fs since last spike | %u overlay loads since"
                        " | %llu osGetTime calls | frame-step %u\n",
                        ms(now - boot) / 1000.0, interval, dl_ms_since, present_ms_since, idle,
                        gap / 1000.0, overlay_loads - overlays_at_last_spike,
                        (unsigned long long)(osgt_now - osgt_at_last_dl),
                        // D_8017AA90 -- the target N the governor spins to.
                        hybridheaven::debug_read_guest_u32(0x8017AA90u));
                last_spike = now;
                have_last_spike = true;
                overlays_at_last_spike = overlay_loads;
            }
        }
        osgt_at_last_dl = osgt_now;

        last_dl_start = now;
        have_last = true;
        dl_ms_since = 0.0;
        present_ms_since = 0.0;

        maybe_report(now);
    }

    static double percentile(std::vector<double>& v, double p) {
        if (v.empty()) {
            return 0.0;
        }
        size_t idx = (size_t)(p * (double)(v.size() - 1));
        std::nth_element(v.begin(), v.begin() + idx, v.end());
        return v[idx];
    }

    void maybe_report(clock::time_point now) {
        const double secs = std::chrono::duration<double>(now - window_start).count();
        if (secs < report_secs() || intervals.empty()) {
            return;
        }

        double sum = 0.0, worst = 0.0;
        for (double v : intervals) {
            sum += v;
            if (v > worst) {
                worst = v;
            }
        }
        const double mean = sum / (double)intervals.size();

        // The periodicity test. A tight spread around the mean is a timer; a
        // spread the size of the mean is content.
        double gap_mean = 0.0, gap_sd = 0.0;
        if (!spike_gaps.empty()) {
            for (double g : spike_gaps) {
                gap_mean += g;
            }
            gap_mean /= (double)spike_gaps.size();
            for (double g : spike_gaps) {
                gap_sd += (g - gap_mean) * (g - gap_mean);
            }
            gap_sd = std::sqrt(gap_sd / (double)spike_gaps.size());
        }

        fprintf(stderr,
                "[frame] t=%.1fs %zu frames (%.1f/s) mean %.1fms p50 %.1f p95 %.1f p99 %.1f max %.1f"
                " | dl %.0f%% present %.0f%% idle %.0f%%"
                " | VI %.1f/s"
                " | %llu spikes >%.0fms, gap %.2fs +/- %.2fs (n=%zu)\n",
                std::chrono::duration<double>(now - boot).count(),
                intervals.size(), (double)intervals.size() / secs,
                mean, percentile(intervals, 0.50), percentile(intervals, 0.95),
                percentile(intervals, 0.99), worst,
                100.0 * window_dl_ms / (secs * 1000.0),
                100.0 * window_present_ms / (secs * 1000.0),
                100.0 * (secs * 1000.0 - window_dl_ms - window_present_ms) / (secs * 1000.0),
                (double)window_vi_updates / secs,
                (unsigned long long)window_spikes, threshold_ms(),
                gap_mean / 1000.0, gap_sd / 1000.0, spike_gaps.size());

        window_start = now;
        intervals.clear();
        spike_gaps.clear();
        window_dl_ms = 0.0;
        window_present_ms = 0.0;
        window_spikes = 0;
        window_vi_updates = 0;
    }
};

FrameProbe frame_probe;

// ---------------------------------------------------------------------------
// HH_TRACE_GFX=<seconds> -- RT64's own profilers, logged instead of drawn.
//
// FrameProbe above measures the GAME's submission loop: send_dl to send_dl, at
// the ~30 Hz the game runs its logic. It cannot see the number an external FPS
// overlay reports, because that counts PRESENTS, and RT64 presents interpolated
// frames from its own thread at the swap chain rate -- `framesToPresent =
// frameCounters.count` in rt64_present_queue.cpp. A drop from 75 to 37 happens
// entirely inside a layer FrameProbe never enters.
//
// RT64 already times every stage; it just renders the numbers into an ImGui
// window (rt64_state.cpp, developer mode) rather than printing them. Everything
// here is read off those same ProfilingTimers, so this is the debug overlay in
// log form -- no rebuild of RT64, no one squinting at a graph mid-battle.
//
// `presentProfiler` is logged with logAndRestart(), so its average is the mean
// INTERVAL between presents, not the cost of one. 1000/avg is therefore the
// overlay's FPS figure, measured in-process and timestamped against everything
// else in the log.
//
// Which stage grows names the cause, and they are mutually exclusive:
//
//   rendGPU  grows -> the GPU cannot draw the room in the frame budget.
//   dlCPU    grows -> RT64 is CPU-bound walking the display list.
//   match    grows -> frame interpolation is the cost, not the drawing.
//   shaders  climb -> pipelines being compiled on first sight of a material.
//                     This one RECOVERS once the room's set is cached, which is
//                     the signature to check first for a per-room stutter.
//   present  grows with everything else flat -> the stall is the swap chain:
//                     vsync, the compositor, or the driver.
struct GfxProbe {
    using clock = std::chrono::steady_clock;

    bool enabled() {
        static const bool on = getenv("HH_TRACE_GFX") != nullptr;
        return on;
    }

    double report_secs() {
        static const double t = [] {
            const char* env = getenv("HH_TRACE_GFX");
            double v = (env != nullptr) ? atof(env) : 0.0;
            // HH_TRACE_GFX=1 is the natural way to switch a thing on, so treat
            // it as "1 second" rather than as an interval someone chose.
            return v >= 0.1 ? v : 1.0;
        }();
        return t;
    }

    clock::time_point boot = clock::now();
    clock::time_point window_start = clock::now();
    uint32_t last_shader_count = 0;
    uint32_t last_overlay_loads = 0;
    uint64_t updates = 0;

    // How many frames RT64 decided to present for each workload, sampled.
    //
    // This is the number the whole interpolation question turns on, and it is
    // NOT a cost -- it is a decision. rt64_workload_queue.cpp:1001:
    //
    //   logicalTicks += targetRate;
    //   displayFrames = (logicalTicks - displayTicks) / viOriginalRate;
    //
    // ...but ONLY when `generateInterpolatedFrames`; otherwise `displayFrames`
    // stays at its initialiser of 1. So at 75 Hz over a 30 Hz game this reads
    // 2 and 3 alternating (mean 2.5) while interpolation is on, and a flat 1
    // the moment it switches off -- at which point the presented rate collapses
    // to the game's own rate with every stage still costing exactly what it did.
    //
    // A mean between 1 and 2.5 means it is toggling frame to frame, which is a
    // different fault from it being cleanly off, so the window keeps `ones` --
    // how many samples read exactly 1 -- rather than the mean alone.
    uint64_t count_samples = 0;
    uint64_t count_total = 0;
    uint64_t count_ones = 0;
    uint32_t count_min = UINT32_MAX;
    uint32_t count_max = 0;

    static constexpr uint32_t kFactorBuckets = 8;
    uint64_t ring_samples = 0;
    uint64_t ring_uniform = 0;   // all three agree -- upstream's only good case
    uint64_t ring_pair = 0;      // two agree -- the case the majority fix repairs
    uint64_t ring_scattered = 0; // all three differ -- neither rule can help
    uint64_t ring_zero = 0;      // a zero slot; pushFactor(counter) can push one
    uint64_t factor_hist[kFactorBuckets] = {};

    void tick(RT64::Application* app, uint32_t overlay_loads) {
        if (!enabled() || app == nullptr) {
            return;
        }
        updates++;

        {
            const auto& shared = *app->sharedQueueResources;
            const uint32_t c = shared.interpolatedFrames[shared.interpolatedFramesIndex].count;
            count_samples++;
            count_total += c;
            count_ones += (c <= 1) ? 1 : 0;
            count_min = std::min(count_min, c);
            count_max = std::max(count_max, c);
        }

        // The factors ring itself, classified. The majority fix assumed the ring
        // is poisoned by ONE odd frame, leaving two slots in agreement -- so if
        // `scattered` is anything but rare, that assumption is wrong and the fix
        // cannot work no matter how it is tuned. Measured rather than reasoned
        // about, because the first version of this fix was built on the guess.
        {
            // Over the WHOLE ring. This looked at the first three slots while the
            // ring was three long; once it was widened to eight that silently
            // became "three arbitrary slots of eight, and not the recent ones",
            // which reported scattered 0% for a visibly jittering cadence.
            const auto& f = app->state->viHistory.factors;
            bool anyZero = false;
            bool uniform = true;
            uint32_t distinct = 0;
            for (size_t i = 0; i < f.size(); i++) {
                if (f[i] == 0) {
                    anyZero = true;
                }
                if (f[i] != f[0]) {
                    uniform = false;
                }
                bool seen = false;
                for (size_t j = 0; j < i; j++) {
                    seen = seen || (f[j] == f[i]);
                }
                if (!seen) {
                    distinct++;
                }
            }
            uniform = uniform && !anyZero;
            // "pair" now means: more than one value present, but only two
            // distinct ones -- the shape a majority rule could still repair.
            const bool pair = !uniform && !anyZero && (distinct == 2);
            ring_samples++;
            if (uniform) {
                ring_uniform++;
            }
            else if (pair) {
                ring_pair++;
            }
            else if (anyZero) {
                ring_zero++;
            }
            else {
                ring_scattered++;
            }
            for (uint32_t v : f) {
                if (v < kFactorBuckets) {
                    factor_hist[v]++;
                }
                else {
                    factor_hist[kFactorBuckets - 1]++;
                }
            }
        }

        const auto now = clock::now();
        const double secs = std::chrono::duration<double>(now - window_start).count();
        if (secs < report_secs()) {
            return;
        }

        const double present = app->presentQueue->presentProfiler.average();
        const uint32_t shaders = app->rasterShaderCache->shaderCount();

        fprintf(stderr,
                "[gfxprof] viRule=%s t=%.1fs present %.2fms (%.1f fps) | rendCPU %.2f rendGPU %.2f"
                " match %.2f workload %.2f | dlAPI %.2f dlCPU %.2f | scrAPI %.2f scrCPU %.2f"
                " viChg %.2f | shaders %u (+%u) | updates %.1f/s | ovl %u (+%u)"
                " | frames/workload %.2f (min %u max %u, %.0f%% ones)"
                " | swap %u target %u viOrig %u | interpTargets %zu colorImgs %zu"
                " | ring uniform %.0f%% pair %.0f%% scattered %.0f%% zero %.0f%%"
                " | factors {%u,%u,%u,%u,%u,%u,%u,%u}"
                " hist 1:%llu 2:%llu 3:%llu 4:%llu 5+:%llu | non-2 %.1f%%\n",
                RT64::usingLegacyViRate() ? "legacy" : "mean",
                std::chrono::duration<double>(now - boot).count(),
                present, present > 0.0 ? 1000.0 / present : 0.0,
                app->workloadQueue->rendererCPUProfiler.average(),
                app->workloadQueue->rendererGPUProfiler.average(),
                app->workloadQueue->matchingProfiler.average(),
                app->workloadQueue->workloadProfiler.average(),
                app->dlApiProfiler.average(),
                app->state->dlCpuProfiler.average(),
                app->screenApiProfiler.average(),
                app->state->screenCpuProfiler.average(),
                app->state->viChangedProfiler.average(),
                shaders, shaders - last_shader_count,
                (double)updates / secs,
                overlay_loads, overlay_loads - last_overlay_loads,
                count_samples ? (double)count_total / (double)count_samples : 0.0,
                count_min == UINT32_MAX ? 0 : count_min, count_max,
                count_samples ? 100.0 * (double)count_ones / (double)count_samples : 0.0,
                app->sharedQueueResources->swapChainRate,
                app->sharedQueueResources->targetRate,
                app->sharedQueueResources->viOriginalRate,
                app->sharedQueueResources->interpolatedColorTargets.size(),
                app->sharedQueueResources->colorImageAddressVector.size(),
                ring_samples ? 100.0 * (double)ring_uniform / (double)ring_samples : 0.0,
                ring_samples ? 100.0 * (double)ring_pair / (double)ring_samples : 0.0,
                ring_samples ? 100.0 * (double)ring_scattered / (double)ring_samples : 0.0,
                ring_samples ? 100.0 * (double)ring_zero / (double)ring_samples : 0.0,
                app->state->viHistory.factors[0], app->state->viHistory.factors[1],
                app->state->viHistory.factors[2], app->state->viHistory.factors[3],
                app->state->viHistory.factors[4], app->state->viHistory.factors[5],
                app->state->viHistory.factors[6], app->state->viHistory.factors[7],
                (unsigned long long)factor_hist[1], (unsigned long long)factor_hist[2],
                (unsigned long long)factor_hist[3], (unsigned long long)factor_hist[4],
                (unsigned long long)(factor_hist[5] + factor_hist[6] + factor_hist[7]),
                // Arm-independent and ring-width-independent: the share of
                // sampled factors that are not the steady value. This is the
                // exposure measure -- if it is ~0 the fix was never tested, and
                // a clean run proves nothing. It is comparable across every run.
                (factor_hist[1] + factor_hist[2] + factor_hist[3] + factor_hist[4] +
                 factor_hist[5] + factor_hist[6] + factor_hist[7]) > 0
                    ? 100.0 * (double)(factor_hist[1] + factor_hist[3] + factor_hist[4] +
                                       factor_hist[5] + factor_hist[6] + factor_hist[7]) /
                          (double)(factor_hist[1] + factor_hist[2] + factor_hist[3] + factor_hist[4] +
                                   factor_hist[5] + factor_hist[6] + factor_hist[7])
                    : 0.0);

        last_shader_count = shaders;
        last_overlay_loads = overlay_loads;
        updates = 0;
        count_samples = 0;
        count_total = 0;
        count_ones = 0;
        count_min = UINT32_MAX;
        count_max = 0;
        ring_samples = 0;
        ring_uniform = 0;
        ring_pair = 0;
        ring_scattered = 0;
        ring_zero = 0;
        memset(factor_hist, 0, sizeof(factor_hist));
        window_start = now;
    }
};

GfxProbe gfx_probe;

} // namespace

// Defined in src/game/recomp_api.cpp.
uint32_t hh_overlay_load_count();

void hybridheaven::renderer::RT64Context::send_dl(const OSTask* task) {
    // See the switch in main.cpp -- no-op unless HH_FRAME_GOVERNOR_OFF is set.
    hybridheaven::apply_frame_governor_override();
    frame_probe.frame_start(hh_overlay_load_count());
    const auto dl_t0 = FrameProbe::clock::now();
    struct DlTimer {
        FrameProbe::clock::time_point t0;
        ~DlTimer() { frame_probe.note_dl(FrameProbe::ms(FrameProbe::clock::now() - t0)); }
    } dl_timer{ dl_t0 };

    // A display list that does not start inside rdram is not a display list.
    // Interpreting one costs a whole frame of runaway before RT64's command
    // ceiling catches it, so drop it here instead: the game submits the next
    // frame from a refilled record and carries on either way.
    if (!inspect_gfx_task(app->core.RDRAM, task) && !no_dl_guard()) {
        return;
    }
    // Widen the game's own 4:3 safe rect before RT64 walks the list, so the
    // widened projection has somewhere to draw. See src/main/widescreen.cpp.
    hybridheaven::widescreen::widen_display_lists(app->core.RDRAM, (uint32_t)task->t.data_ptr);
    app->state->rsp->reset();
    app->interpreter->loadUCodeGBI(task->t.ucode & 0x3FFFFFF, task->t.ucode_data & 0x3FFFFFF, true);
    app->processDisplayLists(app->core.RDRAM, task->t.data_ptr & 0x3FFFFFF, 0, true);
}

void hybridheaven::renderer::RT64Context::update_screen() {
#if defined(__ANDROID__)
    // On the gfx thread: if the SDL/main thread published a fresh ANativeWindow on
    // resume, hand it down to the swap chain, which rebuilds the Vulkan surface
    // from it inside resize(). Consumed HERE rather than published straight from
    // the main thread so that only the gfx and present threads ever touch
    // RT64/Plume Vulkan state.
    if (void* nw = g_pending_resume_window.exchange(nullptr, std::memory_order_acq_rel)) {
        if (app && app->swapChain != nullptr) {
            // Hands our owned reference on to the swap chain.
            app->swapChain->setRenderWindow(static_cast<ANativeWindow*>(nw));
        }
        else {
            // Nothing to adopt it -- early startup or teardown. Release rather
            // than leak the reference taken at publish.
            ANativeWindow_release(static_cast<ANativeWindow*>(nw));
        }
    }
#endif
    const auto present_t0 = FrameProbe::clock::now();
    app->updateScreen();
    frame_probe.note_present(FrameProbe::ms(FrameProbe::clock::now() - present_t0));
    gfx_probe.tick(app.get(), hh_overlay_load_count());

    // What RT64 actually did with the aspect setting, as opposed to what it was
    // told. `resolutionScale` is `{ multiplier * aspectRatioScale, multiplier }`
    // (rt64_workload_queue.cpp), so the ratio of its two components IS
    // `aspectRatioTarget / aspectRatioSource` -- 1.00 means the render target is
    // still the game's own 4:3 and the window is being pillarboxed, whatever the
    // menu says. Reported on change only, so a steady run costs one line.
    //
    // This exists because "the config says Expand" and "the picture is wider"
    // turned out to be different questions, and nothing in between was visible.
    if (getenv("HH_TRACE_ASPECT") != nullptr) {
        auto* shared = app->presentQueue->ext.sharedResources;
        if (shared != nullptr) {
            const float sx = shared->resolutionScale.x;
            const float sy = shared->resolutionScale.y;
            const float ratio_scale = (sy > 0.0f) ? (sx / sy) : 0.0f;
            static float last_ratio_scale = -1.0f;
            static uint32_t last_w = 0, last_h = 0;
            if (ratio_scale != last_ratio_scale || shared->swapChainWidth != last_w ||
                shared->swapChainHeight != last_h) {
                fprintf(stderr, "[gfx] aspect: swapchain %ux%u (%.3f), resolutionScale %.3f x %.3f"
                                " => ratio scale %.3f (1.000 = no widening)\n",
                    shared->swapChainWidth, shared->swapChainHeight,
                    shared->swapChainHeight ? float(shared->swapChainWidth) / float(shared->swapChainHeight) : 0.0f,
                    sx, sy, ratio_scale);
                last_ratio_scale = ratio_scale;
                last_w = shared->swapChainWidth;
                last_h = shared->swapChainHeight;
            }
        }
    }
}

void hybridheaven::renderer::RT64Context::shutdown() {
    if (app != nullptr) {
        app->end();
    }
}

bool hybridheaven::renderer::RT64Context::update_config(const ultramodern::renderer::GraphicsConfig& old_config,
                                                        const ultramodern::renderer::GraphicsConfig& new_config) {
    if (old_config == new_config) {
        return false;
    }

    if (new_config.wm_option != old_config.wm_option) {
        app->setFullScreen(new_config.wm_option == ultramodern::renderer::WindowMode::Fullscreen);
    }

    set_application_user_config(app.get(), new_config);
    app->updateUserConfig(true);

    if (new_config.msaa_option != old_config.msaa_option) {
        app->updateMultisampling();
    }
    return true;
}

void hybridheaven::renderer::RT64Context::enable_instant_present() {
    app->enhancementConfig.presentation.mode = RT64::EnhancementConfiguration::Presentation::Mode::PresentEarly;
    app->updateEnhancementConfig();
}

uint32_t hybridheaven::renderer::RT64Context::get_display_framerate() const {
    return app->presentQueue->ext.sharedResources->swapChainRate;
}

float hybridheaven::renderer::RT64Context::get_resolution_scale() const {
    constexpr int ReferenceHeight = 240;
    switch (app->userConfig.resolution) {
        case RT64::UserConfiguration::Resolution::WindowIntegerScale:
            if (app->sharedQueueResources->swapChainHeight > 0) {
                return std::max(float((app->sharedQueueResources->swapChainHeight + ReferenceHeight - 1) / ReferenceHeight), 1.0f);
            }
            return 1.0f;
        case RT64::UserConfiguration::Resolution::Manual:
            return float(app->userConfig.resolutionMultiplier);
        case RT64::UserConfiguration::Resolution::Original:
        default:
            return 1.0f;
    }
}

std::unique_ptr<ultramodern::renderer::RendererContext> hybridheaven::renderer::create_render_context(
        uint8_t* rdram, ultramodern::renderer::WindowHandle window_handle, bool developer_mode) {
    return std::make_unique<hybridheaven::renderer::RT64Context>(rdram, window_handle, developer_mode);
}

RT64::UserConfiguration::Antialiasing hybridheaven::renderer::RT64MaxMSAA() {
    return device_max_msaa;
}

bool hybridheaven::renderer::RT64SamplePositionsSupported() {
    return sample_positions_supported;
}

bool hybridheaven::renderer::RT64HighPrecisionFBEnabled() {
    return high_precision_fb_enabled;
}
