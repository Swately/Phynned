// framework/render/opengl3/include/phyriad/render/opengl3/FSR1Pass.hpp
// FSR1Pass — AMD FidelityFX Super Resolution 1.0, OpenGL 3.3 fragment-shader port.
//
// Two-pass post-process pipeline:
//   Pass 1 EASU — Edge-Adaptive Spatial Upsampling.
//     Reads an input texture at (input_width x input_height).
//     Writes to an internal FBO at (output_width x output_height).
//   Pass 2 RCAS — Robust Contrast-Adaptive Sharpening.
//     Reads the EASU output and writes to the caller-bound draw framebuffer.
//
// Both passes use a full-screen quad + fragment shader; no compute required.
// Minimum requirement: OpenGL 3.3 Core Profile, GLFW context current.
//
// Lifecycle:
//   FSR1Pass fsr;
//   fsr.init({640, 360, 1920, 1080, 0.2f});    // once
//   // per frame:
//   fsr.apply(scene_texture_id);               // writes to current bound draw FBO
//   fsr.resize(640, 360, 1920, 1080);          // on resolution change
//   fsr.shutdown();                            // explicit or via destructor
//
#pragma once
#include <phyriad/schema/Error.hpp>
#include <cstdint>
#include <expected>

namespace phyriad::render::opengl3 {

class FSR1Pass {
public:
    // ── Configuration ─────────────────────────────────────────────────────────
    struct Config {
        uint32_t input_width   {0};
        uint32_t input_height  {0};
        uint32_t output_width  {0};
        uint32_t output_height {0};
        // RCAS sharpness: 0.0 = maximum, 2.0 = minimum (matches AMD convention).
        float    sharpness     {0.2f};
    };

    // ── Lifecycle ─────────────────────────────────────────────────────────────
    FSR1Pass()  noexcept;
    ~FSR1Pass() noexcept;

    FSR1Pass(FSR1Pass const&)            = delete;
    FSR1Pass& operator=(FSR1Pass const&) = delete;
    FSR1Pass(FSR1Pass&&)                 = delete;
    FSR1Pass& operator=(FSR1Pass&&)      = delete;

    // Compile shaders, create FBO and quad geometry.
    // Requires an active OpenGL 3.3 context.
    [[nodiscard]] std::expected<void, phyriad::Error>
        init(const Config& cfg) noexcept;

    // Release all GL resources.  Safe to call on an uninitialized object.
    void shutdown() noexcept;

    // ── Per-frame ─────────────────────────────────────────────────────────────

    // Run EASU then RCAS.
    //   input_tex_id  — GL texture handle of the scene at input resolution.
    //                   Must be GL_TEXTURE_2D, RGBA8, clamped-to-edge.
    // Writes the upscaled+sharpened image to the draw framebuffer that is
    // currently bound when apply() is called.  Caller is responsible for
    // binding the destination FBO and setting glViewport to output size.
    void apply(uint32_t input_tex_id) noexcept;

    // ── Resize ────────────────────────────────────────────────────────────────

    // Update resolution without recompiling shaders.
    void resize(uint32_t input_w, uint32_t input_h,
                uint32_t output_w, uint32_t output_h) noexcept;

    // ── Accessors ─────────────────────────────────────────────────────────────

    [[nodiscard]] bool     initialized()   const noexcept { return initialized_;  }
    // Handle of the intermediate EASU texture (output resolution, RGBA8).
    [[nodiscard]] uint32_t easu_tex_id()   const noexcept { return easu_tex_;     }

private:
    // ── Init helpers ──────────────────────────────────────────────────────────
    [[nodiscard]] bool load_gl_functions()   noexcept;
    [[nodiscard]] bool compile_shaders()     noexcept;
    [[nodiscard]] bool create_fbo()          noexcept;
    [[nodiscard]] bool create_quad()         noexcept;

    // ── Uniform upload ────────────────────────────────────────────────────────
    void upload_easu_constants() noexcept;
    void upload_rcas_constants() noexcept;

    // ── Cleanup helpers ───────────────────────────────────────────────────────
    void destroy_shaders() noexcept;
    void destroy_fbo()     noexcept;
    void destroy_quad()    noexcept;

    // ── State ─────────────────────────────────────────────────────────────────
    Config   cfg_       {};
    bool     initialized_  {false};

    // EASU program.
    uint32_t easu_prog_     {0};
    int      u_easu_tex_    {-1};
    int      u_easu_con0_   {-1};
    int      u_easu_con1_   {-1};

    // RCAS program.
    uint32_t rcas_prog_     {0};
    int      u_rcas_tex_    {-1};
    int      u_rcas_con_    {-1};

    // Intermediate EASU framebuffer (output_width x output_height, RGBA8).
    uint32_t easu_fbo_  {0};
    uint32_t easu_tex_  {0};

    // Full-screen quad.
    uint32_t quad_vao_  {0};
    uint32_t quad_vbo_  {0};
};

} // namespace phyriad::render::opengl3
// Made with my soul - Swately <3
