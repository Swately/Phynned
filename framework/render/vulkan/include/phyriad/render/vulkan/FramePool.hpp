// framework/render/vulkan/include/phyriad/render/vulkan/FramePool.hpp
// FramePool — pool pre-asignado de imágenes Vulkan para el pipeline
//             RenderNode → FrameInterpolatorNode → PresentNode.
//
// Propósito:
//   Evitar asignaciones dinámicas en el hot path del render.
//   RenderNode adquiere un slot (acquire()), renderiza a su imagen,
//   publica el handle_id en RenderedFrame.
//   PresentNode recibe handle_id, presenta la imagen, llama release().
//
// Modelo de concurrencia: lock-free usando CAS atómico por slot.
//   - Múltiples productores/consumidores pueden adquirir/liberar slots
//     concurrentemente sin bloqueo.
//   - acquire() retorna UINT32_MAX si todos los slots están en uso
//     (situación rara — el producer debe manejar el skip de frame).
//
// REGLA (PHYRIAD_IMPLEMENTATION_STRATEGIES §8.4):
//   NUNCA pasar VkImage directamente en mensajes POD del grafo.
//   Siempre pasar el índice del slot (handle_id).  El FramePool es el owner.
//
// Layout de Slot:
//   image     — VkImage creado para el swapchain
//   view      — VkImageView del image
//   memory    — VkDeviceMemory asociada
//   in_use    — atomic bool (0=libre, 1=en uso)
//
#pragma once
#include <vulkan/vulkan.h>
#include <atomic>
#include <array>
#include <cstdint>
#include <phyriad/hal/MemoryOrder.hpp>

namespace phyriad::render::vulkan {

// ─────────────────────────────────────────────────────────────────────────────
// FramePool
// ─────────────────────────────────────────────────────────────────────────────
class FramePool {
public:
    static constexpr uint32_t kPoolSize = 4u;

    // ── Slot ─────────────────────────────────────────────────────────────────
    struct Slot {
        VkImage        image  {VK_NULL_HANDLE};
        VkImageView    view   {VK_NULL_HANDLE};
        VkDeviceMemory memory {VK_NULL_HANDLE};

        // Padding to avoid false sharing on CAS.
        alignas(64) std::atomic<uint8_t> in_use{0u};
    };

    FramePool()  noexcept = default;
    ~FramePool() noexcept = default;

    FramePool(FramePool const&)            = delete;
    FramePool& operator=(FramePool const&) = delete;
    FramePool(FramePool&&)                 = delete;
    FramePool& operator=(FramePool&&)      = delete;

    // ── Acquire — O(kPoolSize) CAS scan ──────────────────────────────────────
    // Returns slot index [0..kPoolSize), or UINT32_MAX if all slots are in use.
    // The caller must release() after the GPU has finished with the image.
    [[nodiscard]] uint32_t acquire() noexcept
    {
        for (uint32_t i = 0u; i < kPoolSize; ++i) {
            uint8_t expected = 0u;
            if (slots_[i].in_use.compare_exchange_strong(
                    expected, 1u,
                    std::memory_order_acq_rel,  // HAL: explicit ordering — see surrounding context
                    std::memory_order_relaxed))  // HAL: relaxed — secondary atomic in compound op
            {
                return i;
            }
        }
        return UINT32_MAX;  // pool exhausted
    }

    // ── Release — O(1) atomic store ──────────────────────────────────────────
    // Must only be called after vkWaitForFences confirms the GPU is done.
    void release(uint32_t idx) noexcept
    {
        if (idx >= kPoolSize) return;
        hal::seq_store_release(slots_[idx].in_use, 0u);
    }

    // ── Slot accessor ─────────────────────────────────────────────────────────
    [[nodiscard]] Slot&       slot(uint32_t idx) noexcept       { return slots_[idx]; }
    [[nodiscard]] const Slot& slot(uint32_t idx) const noexcept { return slots_[idx]; }

    // ── Lifecycle (called by VulkanBackend or PresentNode) ────────────────────
    // init(): allocate kPoolSize images on the device for the given format/extent.
    // Returns true on success.  VkImages stay alive until shutdown().
    [[nodiscard]] bool init(VkDevice         device,
                            VkPhysicalDevice phys_dev,
                            uint32_t         width,
                            uint32_t         height,
                            VkFormat         format) noexcept;

    // shutdown(): destroy all Vulkan objects in reverse order. Idempotent.
    void shutdown(VkDevice device) noexcept;

    [[nodiscard]] bool initialized() const noexcept { return initialized_; }
    [[nodiscard]] uint32_t width()   const noexcept { return width_;  }
    [[nodiscard]] uint32_t height()  const noexcept { return height_; }

private:
    std::array<Slot, kPoolSize> slots_{};
    bool     initialized_{false};
    uint32_t width_  {0};
    uint32_t height_ {0};
    VkFormat format_ {VK_FORMAT_UNDEFINED};
};

} // namespace phyriad::render::vulkan
// Made with my soul - Swately <3
