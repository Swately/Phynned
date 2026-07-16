// framework/render/opengl3/src/OpenGL3Backend.cpp
// OpenGL 3.3 Core Profile render backend — implementation.
//
#include <phyriad/render/opengl3/OpenGL3Backend.hpp>
#include <phyriad/hal/Timestamp.hpp>

// ImGui backends — included after local headers to avoid macro collisions.
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>
#include <GLFW/glfw3.h>

#include <cstdio>

namespace phyriad::render::opengl3 {

// ── init ─────────────────────────────────────────────────────────────────────
std::expected<void, phyriad::Error>
OpenGL3Backend::init(GLFWwindow* window, FrameArena* arena) noexcept
{
    if (initialized_) shutdown();

    window_ = window;
    arena_  = arena;

    if (glfwGetCurrentContext() != window_)
        glfwMakeContextCurrent(window_);

    glfwSwapInterval(1);

    if (!ImGui_ImplGlfw_InitForOpenGL(window_, /*install_callbacks=*/true)) {
        std::fprintf(stderr, "[OpenGL3Backend] ImGui_ImplGlfw_InitForOpenGL failed\n");
        return std::unexpected(phyriad::Error{
            .code           = phyriad::ErrorCode::ResourceInitFailed,
            .source_node_id = 0,
            .timestamp_ns   = 0});
    }

    if (!ImGui_ImplOpenGL3_Init("#version 330 core")) {
        std::fprintf(stderr, "[OpenGL3Backend] ImGui_ImplOpenGL3_Init failed\n");
        ImGui_ImplGlfw_Shutdown();
        return std::unexpected(phyriad::Error{
            .code           = phyriad::ErrorCode::ResourceInitFailed,
            .source_node_id = 0,
            .timestamp_ns   = 0});
    }

    frame_id_         = 0;
    frame_start_tsc_  = hal::rdtsc();
    last_present_tsc_ = hal::rdtsc();
    fps_ema_          = 0.0f;
    dropped_frames_   = 0;
    initialized_      = true;
    return {};
}

// ── new_frame ─────────────────────────────────────────────────────────────────
void OpenGL3Backend::new_frame() noexcept
{
    if (!initialized_) [[unlikely]] return;
    frame_start_tsc_ = hal::rdtsc();
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();
}

// ── end_frame ─────────────────────────────────────────────────────────────────
RenderStats OpenGL3Backend::end_frame() noexcept
{
    RenderStats stats{};
    if (!initialized_) [[unlikely]] return stats;

    ImGui::Render();

    int fb_w = 0, fb_h = 0;
    glfwGetFramebufferSize(window_, &fb_w, &fb_h);
    if (fb_w > 0 && fb_h > 0) {
        glViewport(0, 0, fb_w, fb_h);
        glClearColor(0.1f, 0.1f, 0.1f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    }

    ImDrawData* draw_data = ImGui::GetDrawData();
    if (draw_data) {
        stats.draw_calls = static_cast<uint32_t>(draw_data->CmdListsCount);
        stats.vertices   = static_cast<uint32_t>(draw_data->TotalVtxCount);
        stats.indices    = static_cast<uint32_t>(draw_data->TotalIdxCount);
    }

    const hal::tsc_t end_tsc    = hal::rdtsc();
    const uint64_t   freq       = hal::tsc_freq_hz();
    const uint64_t   cpu_ns     = (freq > 0)
        ? static_cast<uint64_t>(end_tsc - frame_start_tsc_) * 1'000'000'000ULL / freq
        : 0u;

    stats.frame_id    = ++frame_id_;
    stats.cpu_time_ns = cpu_ns;
    stats.gpu_time_ns = 0u;

    const float wall_ms = (freq > 0)
        ? static_cast<float>(end_tsc - last_present_tsc_) * 1000.0f / static_cast<float>(freq)
        : 0.0f;
    stats.frame_time_ms = wall_ms;

    constexpr float kAlpha = 0.05f;
    const float inst_fps = (wall_ms > 0.0f) ? (1000.0f / wall_ms) : 0.0f;
    fps_ema_ = (fps_ema_ < 0.5f) ? inst_fps : fps_ema_ + kAlpha * (inst_fps - fps_ema_);
    stats.fps_ema = fps_ema_;

    if (wall_ms > 33.0f) ++dropped_frames_;
    stats.dropped_frames = dropped_frames_;
    stats.present_tsc    = last_present_tsc_;

    return stats;
}

// ── present ───────────────────────────────────────────────────────────────────
void OpenGL3Backend::present() noexcept
{
    if (!initialized_) [[unlikely]] return;
    glfwSwapBuffers(window_);
    last_present_tsc_ = hal::rdtsc();
}

// ── resize ────────────────────────────────────────────────────────────────────
void OpenGL3Backend::resize(uint32_t w, uint32_t h, float dpi_scale) noexcept
{
    if (!initialized_) [[unlikely]] return;
    glViewport(0, 0, static_cast<int>(w), static_cast<int>(h));

    const bool dpi_changed = (dpi_scale != last_dpi_scale_);
    if (dpi_changed) {
        last_dpi_scale_ = dpi_scale;
        // FontGlobalScale is a runtime multiplier — no atlas rebuild needed,
        // so the render pillar stays unaware of glyph-range configuration
        // (which is a UI concern owned by phyriad::ui::text::install_default_font).
        // Cost: linear-filtered upscaling looks slightly soft on high-DPI
        // displays; the trade buys clean layering and preserves the extended
        // Latin-1 + General-Punctuation glyphs installed at startup. If we
        // ever want pixel-perfect rescaling, the right place to drive it is
        // a DPI-change callback in the UI pillar, not here.
        ImGui::GetIO().FontGlobalScale = dpi_scale;
    }
}

// ── shutdown ──────────────────────────────────────────────────────────────────
void OpenGL3Backend::shutdown() noexcept
{
    if (!initialized_) return;
    initialized_ = false;
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    window_ = nullptr;
    arena_  = nullptr;
}

} // namespace phyriad::render::opengl3
// Made with my soul - Swately <3
