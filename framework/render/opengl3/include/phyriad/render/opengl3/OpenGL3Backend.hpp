// framework/render/opengl3/include/phyriad/render/opengl3/OpenGL3Backend.hpp
// OpenGL 3.3 Core Profile render backend for phyriad::render.
//
// Implements IRenderBackend via Dear ImGui's OpenGL3 backend wrappers.
// Manages per-frame timing, draw-call counting, and EMA FPS computation.
//
// Requirements:
//   - GLFW must be initialised and the window's GL context must be current
//     before init() is called.
//   - ImGui::CreateContext() must have been called before init().
//   - imgui_lib (CMake target) must be linked.
//
#pragma once
#include <phyriad/render/IRenderBackend.hpp>
#include <phyriad/render/FrameArena.hpp>
#include <phyriad/hal/Timestamp.hpp>
#include <cstdint>
#include <expected>

struct GLFWwindow;

namespace phyriad::render::opengl3 {

class OpenGL3Backend final : public IRenderBackend {
public:
    OpenGL3Backend()  = default;
    ~OpenGL3Backend() noexcept override { shutdown(); }

    OpenGL3Backend(OpenGL3Backend const&)            = delete;
    OpenGL3Backend& operator=(OpenGL3Backend const&) = delete;
    OpenGL3Backend(OpenGL3Backend&&)                 = delete;
    OpenGL3Backend& operator=(OpenGL3Backend&&)      = delete;

    // ── IRenderBackend ────────────────────────────────────────────────────────
    [[nodiscard]] auto init(GLFWwindow* window,
                            FrameArena* arena) noexcept
        -> std::expected<void, phyriad::Error> override;

    void        new_frame()                              noexcept override;
    RenderStats end_frame()                              noexcept override;
    void        present()                                noexcept override;
    void        resize(uint32_t w, uint32_t h,
                       float dpi_scale = 1.0f)           noexcept override;
    void        shutdown()                               noexcept override;

private:
    GLFWwindow*    window_           {nullptr};
    FrameArena*    arena_            {nullptr};
    bool           initialized_      {false};
    uint64_t       frame_id_         {0};
    hal::tsc_t     frame_start_tsc_  {0};
    hal::tsc_t     last_present_tsc_ {0};
    float          fps_ema_          {0.0f};
    uint32_t       dropped_frames_   {0};
    float          last_dpi_scale_   {0.0f};
};

} // namespace phyriad::render::opengl3
// Made with my soul - Swately <3
