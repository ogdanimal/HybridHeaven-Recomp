/**
 * A renderer that draws nothing.
 *
 * The point of this is to run the game's CPU side without RT64. Everything
 * below the display list is real -- the recompiled code executes, the overlay
 * loader hook fires, libultra goes through ultramodern, audio is real -- and
 * only the graphics are dropped. That makes it possible to reach and debug a
 * first boot without first standing up RT64's shader pipeline, and it is a
 * useful bisector afterwards: a crash that persists here is not a renderer bug.
 *
 * `send_dl` discarding tasks is safe because ultramodern signals task
 * completion itself; the game's RSP task queue does not stall on us.
 *
 * Replace this with an RT64-backed context (Goemon64Recomp's
 * src/main/rt64_render_context.cpp, ~669 lines) to get a picture.
 */

#include <atomic>
#include <cstdint>
#include <cstdio>

#include "ultramodern/renderer_context.hpp"
#include "ultramodern/config.hpp"

namespace hybridheaven {
    namespace renderer {

        // Never incremented here: this build draws nothing, so it submits no
        // graphics tasks and the audio report shows 0 frames, which is the truth
        // for it. It exists so the -DHH_RT64=OFF bisect build links.
        std::atomic<uint64_t> gfx_tasks_submitted{0};

        class NullRenderContext final : public ultramodern::renderer::RendererContext {
        public:
            NullRenderContext() {
                setup_result = ultramodern::renderer::SetupResult::Success;
                chosen_api = ultramodern::renderer::GraphicsApi::Auto;
            }

            bool valid() override { return true; }

            bool update_config(const ultramodern::renderer::GraphicsConfig& /*old_config*/,
                               const ultramodern::renderer::GraphicsConfig& /*new_config*/) override {
                return true;
            }

            void enable_instant_present() override {}

            void send_dl(const OSTask* /*task*/) override {}

            void update_screen() override {}

            void shutdown() override {}

            // The game paces itself against this, so report a normal refresh
            // rate rather than 0 -- a zero here makes the frame pacing divide
            // by it.
            uint32_t get_display_framerate() const override { return 60; }

            float get_resolution_scale() const override { return 1.0f; }
        };

        std::unique_ptr<ultramodern::renderer::RendererContext> create_render_context(
                uint8_t* /*rdram*/,
                ultramodern::renderer::WindowHandle /*window_handle*/,
                bool /*developer_mode*/) {
            return std::make_unique<NullRenderContext>();
        }

    } // namespace renderer
} // namespace hybridheaven
