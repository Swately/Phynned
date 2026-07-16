// framework/render/vulkan/include/phyriad/render/vulkan/VulkanBackend.hpp
// VulkanBackend — implementación de IRenderBackend usando Vulkan 1.2+.
//
// Implementa el ciclo new_frame / end_frame / present delegando en:
//   VulkanContext — contexto de Vulkan (instancia, device, colas)
//   Swapchain     — swapchain, render pass, framebuffers, sincronización
//
// Flujo de frame:
//   new_frame()  → acquire swapchain image, begin render pass, ImGui::NewFrame()
//   end_frame()  → ImGui::Render, grabar draw data en command buffer
//   present()    → submit command buffer, present swapchain image
//
// Requisitos:
//   - GLFW debe estar inicializado y glfwVulkanSupported() debe retornar GLFW_TRUE.
//   - ImGui::CreateContext() debe haberse llamado antes de init().
//   - El SDK de Vulkan (LunarG) debe estar disponible en el sistema.
//
// Ciclo de vida idéntico al de OpenGL3Backend (§IRenderBackend):
//   init() → new_frame() → end_frame() → present() → [repeat] → shutdown()
//
// No incluye <vulkan/vulkan.h> en este header — la dependencia del SDK Vulkan
// queda confinada a las unidades de compilación (.cpp) del pillar.
//
#pragma once
#include <phyriad/render/IRenderBackend.hpp>
#include <phyriad/render/FrameArena.hpp>
#include <phyriad/hal/Timestamp.hpp>
#include <cstdint>
#include <expected>

struct GLFWwindow;

namespace phyriad::render::vulkan {

// Forward declarations — evitan incluir <vulkan/vulkan.h> en este header.
class VulkanContext;
class Swapchain;

// ─────────────────────────────────────────────────────────────────────────────
// VulkanBackend
// ─────────────────────────────────────────────────────────────────────────────
class VulkanBackend final : public IRenderBackend {
public:
    VulkanBackend()  noexcept;
    ~VulkanBackend() noexcept override;

    VulkanBackend(VulkanBackend const&)            = delete;
    VulkanBackend& operator=(VulkanBackend const&) = delete;
    VulkanBackend(VulkanBackend&&)                 = delete;
    VulkanBackend& operator=(VulkanBackend&&)      = delete;

    // ── IRenderBackend ────────────────────────────────────────────────────────

    [[nodiscard]] auto init(GLFWwindow* window,
                             FrameArena* arena) noexcept
        -> std::expected<void, phyriad::Error> override;

    void        new_frame()                          noexcept override;
    RenderStats end_frame()                          noexcept override;
    void        present()                            noexcept override;
    void        resize(uint32_t w, uint32_t h,
                       float dpi_scale = 1.0f)       noexcept override;
    void        shutdown()                           noexcept override;

    // ── Composite interop accessor ───────────────────────────────────────────
    // Read-only access to the underlying VulkanContext. CompositeBackend uses
    // this to query external-memory extension availability and to construct
    // ExternalImage instances for VK↔GL shared-memory interop. Returns
    // nullptr when init() has not yet succeeded.
    [[nodiscard]] VulkanContext* context() noexcept { return ctx_; }
    [[nodiscard]] const VulkanContext* context() const noexcept { return ctx_; }

private:
    // ── Submódulos ────────────────────────────────────────────────────────────
    VulkanContext* ctx_   {nullptr};  // contexto Vulkan (instance, device, queues)
    Swapchain*     swap_  {nullptr};  // swapchain, framebuffers, sync

    // ── Estado de init ────────────────────────────────────────────────────────
    GLFWwindow* window_      {nullptr};
    FrameArena* arena_       {nullptr};
    bool        initialized_ {false};

    // Índice de imagen adquirido en new_frame(), consumido en present().
    uint32_t image_index_    {0};
    bool     frame_acquired_ {false};

    // ── Métricas de frame ─────────────────────────────────────────────────────
    uint64_t frame_id_         {0};
    hal::tsc_t frame_start_tsc_{0};
    hal::tsc_t last_present_tsc_{0};
    float    fps_ema_          {0.0f};
    uint32_t dropped_frames_   {0};
};

} // namespace phyriad::render::vulkan
// Made with my soul - Swately <3
