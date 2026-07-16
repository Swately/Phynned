// framework/render/include/phyriad/render/MockRenderBackend.hpp
// Headless IRenderBackend implementation for unit and integration tests.
//
// MockRenderBackend satisfies the IRenderBackend interface without making any
// GL, GLFW, or ImGui calls.  It records call counts and returns predictable
// RenderStats values — suitable for all headless test scenarios.
//
// Usage:
//   MockRenderBackend mock;
//   auto r = mock.init(nullptr, nullptr);
//   assert(r.has_value());
//   mock.new_frame();
//   auto stats = mock.end_frame();
//   assert(stats.frame_id == 1u);
//   mock.present();
//   mock.shutdown();
//
#pragma once
#include "IRenderBackend.hpp"
#include "RenderStats.hpp"
#include "FrameArena.hpp"
#include <phyriad/schema/Error.hpp>
#include <cstdint>
#include <expected>

struct GLFWwindow;

namespace phyriad::render {

class MockRenderBackend final : public IRenderBackend {
public:
    MockRenderBackend()  = default;
    ~MockRenderBackend() noexcept override = default;

    // ── Call-count telemetry ──────────────────────────────────────────────────
    int      init_count      {0};
    int      new_frame_count {0};
    int      end_frame_count {0};
    int      present_count   {0};
    int      resize_count    {0};
    int      shutdown_count  {0};
    bool     initialized     {false};
    uint64_t last_frame_id   {0};
    uint32_t last_resize_w   {0};
    uint32_t last_resize_h   {0};
    float    last_resize_dpi {1.0f};

    // ── Fault injection ───────────────────────────────────────────────────────
    bool fail_init{false};

    void reset() noexcept {
        init_count = new_frame_count = end_frame_count = 0;
        present_count = resize_count = shutdown_count  = 0;
        initialized   = false;
        last_frame_id = 0;
        last_resize_w = last_resize_h = 0;
        last_resize_dpi = 1.0f;
        fail_init     = false;
    }

    // ── IRenderBackend ────────────────────────────────────────────────────────
    [[nodiscard]] auto init(GLFWwindow* /*window*/,
                            FrameArena* /*arena*/) noexcept
        -> std::expected<void, phyriad::Error> override
    {
        ++init_count;
        if (fail_init) {
            return std::unexpected(phyriad::Error{
                .code           = phyriad::ErrorCode::ResourceInitFailed,
                .source_node_id = 0u,
                .timestamp_ns   = 0u});
        }
        initialized = true;
        return {};
    }

    void new_frame() noexcept override { ++new_frame_count; }

    [[nodiscard]] RenderStats end_frame() noexcept override {
        ++end_frame_count;
        RenderStats stats{};
        stats.frame_id      = ++last_frame_id;
        stats.draw_calls    = 1u;
        stats.vertices      = 100u;
        stats.indices       = 150u;
        stats.fps_ema       = 60.0f;
        stats.frame_time_ms = 16.67f;
        return stats;
    }

    void present() noexcept override { ++present_count; }

    void resize(uint32_t w, uint32_t h, float dpi_scale = 1.0f) noexcept override {
        ++resize_count;
        last_resize_w   = w;
        last_resize_h   = h;
        last_resize_dpi = dpi_scale;
    }

    void shutdown() noexcept override {
        ++shutdown_count;
        initialized = false;
    }
};

} // namespace phyriad::render
// Made with my soul - Swately <3
