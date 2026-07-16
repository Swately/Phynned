// framework/render/include/phyriad/render/IRenderBackend.hpp
// Abstract render backend interface for phyriad::render.
//
// Separates the RenderNode (graph layer) from the concrete rendering API
// (OpenGL 3.3 today, Vulkan in Phase 3). The concrete backend is injected
// into RenderNode at construction; no runtime dispatch outside of the
// per-frame new_frame / end_frame / present calls.
//
// Lifecycle contract (MUST be followed by all implementations):
//   1. init(window, arena)  — called once after GLFW/GL context is current.
//   2. new_frame()           — called at the START of each frame.
//   3. end_frame()           — called AFTER all ImGui draw calls; returns RenderStats.
//   4. present()             — called AFTER end_frame(); blocks on vsync.
//   5. shutdown()            — idempotent; safe to call more than once.
//
// Vsync: the vsync block lives in present(), NOT in new_frame() or end_frame().
//
#pragma once
#include "RenderStats.hpp"
#include <phyriad/schema/Error.hpp>
#include <cstdint>
#include <expected>

struct GLFWwindow;

namespace phyriad::render {

class FrameArena;

class IRenderBackend {
public:
    virtual ~IRenderBackend() = default;

    [[nodiscard]] virtual auto init(GLFWwindow* window,
                                    FrameArena* arena) noexcept
        -> std::expected<void, phyriad::Error> = 0;

    virtual void new_frame() noexcept = 0;

    [[nodiscard]] virtual RenderStats end_frame() noexcept = 0;

    virtual void present() noexcept = 0;

    virtual void resize(uint32_t width, uint32_t height,
                        float dpi_scale = 1.0f) noexcept = 0;

    virtual void shutdown() noexcept = 0;
};

} // namespace phyriad::render
// Made with my soul - Swately <3
