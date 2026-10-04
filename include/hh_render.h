#ifndef __HH_RENDER_H__
#define __HH_RENDER_H__

#include <atomic>
#include <cstdint>
#include <memory>

#include "common/rt64_user_configuration.h"
#include "ultramodern/renderer_context.hpp"

namespace RT64 {
    struct Application;
}

namespace hybridheaven {
    namespace renderer {

        // Graphics tasks the game has submitted since startup -- the game's own
        // frame count, not the display's. Read by the audio report so frames and
        // audio are counted over the SAME window; two separately sampled rates
        // cannot be divided by each other, and their ratio is the whole question.
        // Defined in rt64_render_context.cpp and in null_render_context.cpp, so
        // the -DHH_RT64=OFF bisect build still links.
        extern std::atomic<uint64_t> gfx_tasks_submitted;

#if defined(__ANDROID__)
        // Publish a fresh ANativeWindow, from the SDL/main thread, for the gfx
        // thread to pick up on resume. Android destroys the surface behind the
        // window when the app goes to the background and hands back a NEW one on
        // the way in, so the pointer RT64 was built with is dangling by then --
        // it has to be replaced, not reused. Defined in rt64_render_context.cpp;
        // called from the SDL_APP_* gate in input.cpp.
        void android_publish_resume_window(void* native_window);
#endif

        // The RT64-backed renderer. See src/main/rt64_render_context.cpp, and
        // src/main/null_render_context.cpp for the one that draws nothing --
        // both define `create_render_context`, and CMake compiles exactly one.
        class RT64Context final : public ultramodern::renderer::RendererContext {
        public:
            ~RT64Context() override;
            RT64Context(uint8_t* rdram, ultramodern::renderer::WindowHandle window_handle, bool developer_mode);

            bool valid() override { return static_cast<bool>(app); }

            bool update_config(const ultramodern::renderer::GraphicsConfig& old_config,
                               const ultramodern::renderer::GraphicsConfig& new_config) override;

            void enable_instant_present() override;
            void send_dl(const OSTask* task) override;
            void update_screen() override;
            void shutdown() override;
            uint32_t get_display_framerate() const override;
            float get_resolution_scale() const override;

        private:
            std::unique_ptr<RT64::Application> app;
        };

        std::unique_ptr<ultramodern::renderer::RendererContext> create_render_context(
            uint8_t* rdram, ultramodern::renderer::WindowHandle window_handle, bool developer_mode);

        // What the device turned out to support, filled in during setup. A UI
        // would use these to bound its options; nothing does yet.
        RT64::UserConfiguration::Antialiasing RT64MaxMSAA();
        bool RT64SamplePositionsSupported();
        bool RT64HighPrecisionFBEnabled();

    } // namespace renderer
} // namespace hybridheaven

#endif
