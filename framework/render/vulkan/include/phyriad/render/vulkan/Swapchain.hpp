// framework/render/vulkan/include/phyriad/render/vulkan/Swapchain.hpp
// Swapchain — VkSwapchainKHR + render pass + framebuffers + sincronización.
//
// Gestiona el ciclo completo del swapchain de Vulkan:
//   - Selección de formato (VK_FORMAT_B8G8R8A8_UNORM) y modo de presentación
//     (VK_PRESENT_MODE_FIFO_KHR = vsync activado).
//   - Creación de image views y framebuffers para cada imagen del swapchain.
//   - Un render pass single-attachment compatible con ImGui.
//   - Command pool + command buffers (kMaxFramesInFlight = 2).
//   - Semáforos y fences para sincronización CPU-GPU.
//
// Modelo de frames en vuelo (double-buffering):
//   kMaxFramesInFlight = 2 — dos frames pueden procesarse simultáneamente
//   mientras la GPU consume uno, la CPU prepara el siguiente.
//
// Flujo por frame:
//   acquire_next_image()         → espera fence, adquiere imagen, devuelve índice
//   [grabar command buffer]
//   submit_and_present(index)    → submit + present, avanza frame_index
//
// Resize: recreate(new_w, new_h) destruye y recrea todos los recursos.
//
#pragma once
#include <phyriad/schema/Error.hpp>
#include <vulkan/vulkan.h>
#include <cstdint>
#include <expected>

namespace phyriad::render::vulkan {

class VulkanContext;

// ─────────────────────────────────────────────────────────────────────────────
// Swapchain
// ─────────────────────────────────────────────────────────────────────────────
class Swapchain {
public:
    // Máximo de frames procesados simultáneamente (double buffering).
    static constexpr uint32_t kMaxFramesInFlight = 2u;
    // Máximo de imágenes del swapchain que se gestionan internamente.
    static constexpr uint32_t kMaxImages         = 8u;

    Swapchain()  noexcept;
    ~Swapchain() noexcept;

    Swapchain(Swapchain const&)            = delete;
    Swapchain& operator=(Swapchain const&) = delete;
    Swapchain(Swapchain&&)                 = delete;
    Swapchain& operator=(Swapchain&&)      = delete;

    // ── Init / recreate ───────────────────────────────────────────────────────

    [[nodiscard]] auto init(VulkanContext* ctx,
                             uint32_t       width,
                             uint32_t       height) noexcept
        -> std::expected<void, phyriad::Error>;

    // Recrear el swapchain tras un resize o VK_ERROR_OUT_OF_DATE_KHR.
    [[nodiscard]] auto recreate(uint32_t new_width,
                                 uint32_t new_height) noexcept
        -> std::expected<void, phyriad::Error>;

    void shutdown() noexcept;

    // ── Frame API ─────────────────────────────────────────────────────────────

    // Adquirir la siguiente imagen disponible del swapchain.
    // Espera al fence del frame actual. Retorna el índice de imagen adquirido.
    // Si retorna ErrorCode::ResourceInitFailed, el swapchain está desactualizado
    // y se debe llamar a recreate().
    [[nodiscard]] auto acquire_next_image() noexcept
        -> std::expected<uint32_t, phyriad::Error>;

    // Enviar el command buffer actual a la cola gráfica y presentar la imagen.
    // Debe llamarse después de grabar los comandos en current_command_buffer().
    [[nodiscard]] auto submit_and_present(uint32_t image_index) noexcept
        -> std::expected<void, phyriad::Error>;

    // ── Accessores ────────────────────────────────────────────────────────────

    // Command buffer del frame actual — grabar ImGui render calls aquí.
    [[nodiscard]] VkCommandBuffer current_command_buffer() const noexcept {
        return cmd_bufs_[current_frame_];
    }

    [[nodiscard]] VkRenderPass render_pass()   const noexcept { return render_pass_;  }
    [[nodiscard]] VkFramebuffer framebuffer_at(uint32_t idx) const noexcept {
        return (idx < image_count_) ? framebuffers_[idx] : VK_NULL_HANDLE;
    }
    [[nodiscard]] VkCommandPool cmd_pool()    const noexcept { return cmd_pool_;    }
    [[nodiscard]] uint32_t width()        const noexcept { return width_;        }
    [[nodiscard]] uint32_t height()       const noexcept { return height_;       }
    [[nodiscard]] uint32_t image_count()  const noexcept { return image_count_;  }
    [[nodiscard]] uint32_t current_frame()const noexcept { return current_frame_;}
    [[nodiscard]] bool     initialized()  const noexcept { return initialized_;  }

private:
    [[nodiscard]] bool create_swapchain(uint32_t w, uint32_t h)  noexcept;
    [[nodiscard]] bool create_image_views()                       noexcept;
    [[nodiscard]] bool create_render_pass()                       noexcept;
    [[nodiscard]] bool create_framebuffers()                      noexcept;
    [[nodiscard]] bool create_command_pool_and_buffers()          noexcept;
    [[nodiscard]] bool create_sync_objects()                      noexcept;

    void destroy_swapchain_resources() noexcept;

    // ── Contexto compartido ───────────────────────────────────────────────────
    VulkanContext* ctx_         {nullptr};

    // ── Swapchain y render pass ───────────────────────────────────────────────
    VkSwapchainKHR swapchain_   {VK_NULL_HANDLE};
    VkRenderPass   render_pass_ {VK_NULL_HANDLE};
    VkFormat       image_format_{VK_FORMAT_UNDEFINED};
    uint32_t       width_       {0};
    uint32_t       height_      {0};
    uint32_t       image_count_ {0};

    // ── Recursos por imagen del swapchain ─────────────────────────────────────
    // Dimensionados para kMaxImages; image_count_ indica cuántos son válidos.
    VkImage       images_      [kMaxImages]{};   // propiedad del runtime — NO destruir
    VkImageView   image_views_ [kMaxImages]{};   // creados por nosotros — destruir
    VkFramebuffer framebuffers_[kMaxImages]{};   // creados por nosotros — destruir

    // ── Recursos por frame en vuelo ───────────────────────────────────────────
    VkCommandPool  cmd_pool_   {VK_NULL_HANDLE};
    VkCommandBuffer cmd_bufs_  [kMaxFramesInFlight]{};
    VkSemaphore image_available_[kMaxFramesInFlight]{};
    VkSemaphore render_finished_[kMaxFramesInFlight]{};
    VkFence     in_flight_      [kMaxFramesInFlight]{};

    uint32_t current_frame_ {0};
    bool     initialized_   {false};
};

} // namespace phyriad::render::vulkan
// Made with my soul - Swately <3
