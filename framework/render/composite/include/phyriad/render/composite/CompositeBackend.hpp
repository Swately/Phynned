// framework/render/composite/include/phyriad/render/composite/CompositeBackend.hpp
// CompositeBackend — dual-API backend: Vulkan for the 3-D scene layer,
//                    OpenGL 3 for the ImGui UI overlay.
//
// Architecture:
//   ┌──────────────────────────────────┐
//   │          GLFWwindow              │
//   │  ┌────────────┐  ┌────────────┐ │
//   │  │VulkanBackend│  │OpenGL3Back.│ │
//   │  │ (scene)    │  │ (UI/ImGui) │ │
//   │  └────────────┘  └────────────┘ │
//   │         ↕ external-memory       │
//   └──────────────────────────────────┘
//
// Frame pipeline (per-frame order):
//   1. scene_.new_frame()   — acquire VK swapchain image, begin render pass
//   2. scene_.end_frame()   — record ImGui-less draw, blit to external image
//   3. Export the VK image to an OpenGL texture (ext_memory interop)
//   4. ui_.new_frame()      — ImGui::NewFrame() in GL context
//   5. Composite VK texture as background, ImGui overlay on top
//   6. ui_.end_frame()      — flush GL
//   7. ui_.present()        — glfwSwapBuffers (vsync)
//
// Platform-specific interop:
//   Windows — VK_KHR_external_memory_win32 + GL_EXT_external_objects_win32
//   Linux   — VK_KHR_external_memory_fd    + GL_EXT_memory_object_fd
//   The feature-test macro PHYRIAD_COMPOSITE_INTEROP_AVAILABLE is set to 1
//   if the required Vulkan extension headers are detectable at compile time.
//   When unavailable the composite path falls back to Vulkan-only rendering.
//
// Acceptance criterion (master plan §5.3):
//   Demo showing 3-D Vulkan scene + ImGui overlay in the same GLFW window.
//
#pragma once
#include <phyriad/render/IRenderBackend.hpp>
#include <phyriad/schema/Error.hpp>
#include <cstdint>
#include <expected>

// Pull in concrete backends only when they are available.
#ifdef PHYRIAD_BUILD_VULKAN
#   include <phyriad/render/vulkan/VulkanBackend.hpp>
#   include <phyriad/render/vulkan/ExternalImage.hpp>
#endif
#include <phyriad/render/opengl3/OpenGL3Backend.hpp>
#include <phyriad/render/opengl3/ExternalTexture.hpp>

struct GLFWwindow;

namespace phyriad::render::composite {

// ─────────────────────────────────────────────────────────────────────────────
// SharedFrame — OS handle for the exported VK image (platform-specific).
// ─────────────────────────────────────────────────────────────────────────────
struct SharedFrame {
#if defined(_WIN32) && defined(PHYRIAD_BUILD_VULKAN)
    void* win32_handle{nullptr};   // HANDLE from vkGetMemoryWin32HandleKHR
#elif !defined(_WIN32) && defined(PHYRIAD_BUILD_VULKAN)
    int  fd{-1};                   // fd from vkGetMemoryFdKHR
#endif
    uint32_t gl_texture{0};        // GL texture object wrapping the VK image
    uint32_t width{0};
    uint32_t height{0};
    bool     valid{false};
};

// ─────────────────────────────────────────────────────────────────────────────
// CompositeBackend
// ─────────────────────────────────────────────────────────────────────────────
class CompositeBackend final : public IRenderBackend {
public:
    CompositeBackend()  noexcept;
    ~CompositeBackend() noexcept override;

    CompositeBackend(CompositeBackend const&)            = delete;
    CompositeBackend& operator=(CompositeBackend const&) = delete;
    CompositeBackend(CompositeBackend&&)                 = delete;
    CompositeBackend& operator=(CompositeBackend&&)      = delete;

    // ── IRenderBackend ────────────────────────────────────────────────────────

    /// Initialise both sub-backends.
    /// Call after creating the GLFW window (before making any GL context
    /// current — CompositeBackend manages context current-ness internally).
    [[nodiscard]] auto init(GLFWwindow* window,
                            FrameArena* arena) noexcept
        -> std::expected<void, phyriad::Error> override;

    /// Begin a new composite frame:
    ///   scene layer: new_frame() on the Vulkan backend.
    ///   ui   layer:  prepared but not started until end_frame().
    void new_frame() noexcept override;

    /// End the composite frame:
    ///   1. Finish Vulkan scene recording.
    ///   2. Export Vulkan image → OpenGL texture via external memory.
    ///   3. Render composite (VK texture + ImGui overlay) in OpenGL.
    /// Returns RenderStats merged from both backends.
    [[nodiscard]] RenderStats end_frame() noexcept override;

    /// Present to the display (glfwSwapBuffers via OpenGL backend).
    void present() noexcept override;

    /// Resize both backends and the shared frame.
    void resize(uint32_t width, uint32_t height,
                float dpi_scale = 1.0f) noexcept override;

    /// Idempotent shutdown of both backends and shared frame handles.
    void shutdown() noexcept override;

    // ── Composite-specific accessors ─────────────────────────────────────────

    /// Returns true when the external-memory interop was successfully set up.
    /// If false, CompositeBackend fell back to Vulkan-only or OpenGL-only.
    [[nodiscard]] bool interop_active() const noexcept { return interop_active_; }

    /// Returns true when the Vulkan scene backend is initialized.
    [[nodiscard]] bool scene_initialized() const noexcept { return scene_initialized_; }

#ifdef PHYRIAD_BUILD_VULKAN
    /// Direct access to the Vulkan scene backend.
    [[nodiscard]] vulkan::VulkanBackend& scene_backend() noexcept { return scene_; }
    [[nodiscard]] const vulkan::VulkanBackend& scene_backend() const noexcept { return scene_; }
#endif

    /// Direct access to the OpenGL UI backend.
    [[nodiscard]] opengl3::OpenGL3Backend& ui_backend() noexcept { return ui_; }
    [[nodiscard]] const opengl3::OpenGL3Backend& ui_backend() const noexcept { return ui_; }

private:
    // ── Internal helpers ──────────────────────────────────────────────────────

    /// Attempt to set up the VK→GL external-memory interop for the current
    /// window size. Returns true on success.
    bool setup_interop(uint32_t width, uint32_t height) noexcept;

    /// Destroy all interop-related resources (GL texture, OS handle, etc.).
    void teardown_interop() noexcept;

    /// Draw the shared VK frame as a fullscreen quad in the current GL context.
    void draw_scene_quad() noexcept;

    // ── Sub-backends ──────────────────────────────────────────────────────────
#ifdef PHYRIAD_BUILD_VULKAN
    vulkan::VulkanBackend   scene_;
#endif
    opengl3::OpenGL3Backend ui_;

    GLFWwindow* window_           {nullptr};
    FrameArena* arena_            {nullptr};

    bool        scene_initialized_{false};
    bool        ui_initialized_   {false};
    bool        interop_active_   {false};
    bool        shutdown_done_    {false};

    SharedFrame shared_frame_;

    // ── VK↔GL interop resources (v1.0) ─────────────────────────────────
    // ext_image_ owns the Vulkan-side image + memory; ext_texture_ wraps
    // the same memory on the GL side. Lifetime: created in setup_interop,
    // destroyed in teardown_interop. Both are no-ops on platforms / driver
    // configurations where the external-memory extensions are absent.
#ifdef PHYRIAD_BUILD_VULKAN
    vulkan::ExternalImage      ext_image_;
#endif
    opengl3::ExternalTexture   ext_texture_;

    // Fullscreen-quad GL resources (created during setup_interop).
    uint32_t quad_vao_  {0};
    uint32_t quad_vbo_  {0};
    uint32_t quad_prog_ {0};  // minimal GLSL shader: texture rect → screen
};

} // namespace phyriad::render::composite
// Made with my soul - Swately <3
