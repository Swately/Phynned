// framework/render/vulkan/include/phyriad/render/vulkan/VulkanContext.hpp
// VulkanContext — VkInstance + VkPhysicalDevice + VkDevice + queues.
//
// Encapsula la vida útil de todos los objetos de contexto de Vulkan que son
// independientes de la ventana o el swapchain. Una sola instancia de
// VulkanContext es compartida por VulkanBackend y Swapchain.
//
// Responsabilidades:
//   - Crear VkInstance con las extensiones requeridas por GLFW + debug_utils
//   - Seleccionar el mejor VkPhysicalDevice (GPU discrete preferido)
//   - Crear VkDevice con VK_KHR_swapchain
//   - Encontrar familias de colas: graphics + present
//   - Crear VkDescriptorPool para ImGui (grande, estático)
//   - Gestionar el debug messenger en builds de debug
//
// Ciclo de vida:
//   VulkanContext ctx;
//   ctx.init(window) → ok
//   // usar ctx.device(), ctx.graphics_queue(), etc.
//   ctx.shutdown()   → destruye todo en orden inverso
//
#pragma once
#include <phyriad/schema/Error.hpp>
#include <vulkan/vulkan.h>
#include <cstdint>
#include <expected>

struct GLFWwindow;

namespace phyriad::render::vulkan {

// ─────────────────────────────────────────────────────────────────────────────
// VulkanContext
// ─────────────────────────────────────────────────────────────────────────────
class VulkanContext {
public:
    VulkanContext()  noexcept;
    ~VulkanContext() noexcept;

    VulkanContext(VulkanContext const&)            = delete;
    VulkanContext& operator=(VulkanContext const&) = delete;
    VulkanContext(VulkanContext&&)                 = delete;
    VulkanContext& operator=(VulkanContext&&)      = delete;

    // ── Init ──────────────────────────────────────────────────────────────────

    // Inicializar contexto completo para la ventana dada.
    // Crea surface, selecciona GPU, crea device, obtiene colas y pool de descriptores.
    // El window debe tener GLFW inicializado antes de llamar a este método.
    [[nodiscard]] auto init(GLFWwindow* window) noexcept
        -> std::expected<void, phyriad::Error>;

    // Destruir todo el contexto en orden inverso. Idempotente.
    void shutdown() noexcept;

    // ── Accessores (válidos tras init exitoso) ────────────────────────────────

    [[nodiscard]] VkInstance       instance()        const noexcept { return instance_;       }
    [[nodiscard]] VkPhysicalDevice physical_device() const noexcept { return phys_dev_;       }
    [[nodiscard]] VkDevice         device()          const noexcept { return device_;         }
    [[nodiscard]] VkSurfaceKHR     surface()         const noexcept { return surface_;        }
    [[nodiscard]] VkQueue          graphics_queue()  const noexcept { return gfx_queue_;      }
    [[nodiscard]] VkQueue          present_queue()   const noexcept { return present_queue_;  }
    [[nodiscard]] VkDescriptorPool descriptor_pool() const noexcept { return desc_pool_;      }
    [[nodiscard]] uint32_t         graphics_family() const noexcept { return gfx_family_;     }
    [[nodiscard]] uint32_t         present_family()  const noexcept { return present_family_; }
    [[nodiscard]] bool             initialized()     const noexcept { return initialized_;    }

    // ── External-memory interop (CompositeBackend VK↔GL) ─────────────────────
    // True when the device was created with the platform's external-memory
    // extensions enabled (VK_KHR_external_memory_win32 on Windows,
    // VK_KHR_external_memory_fd on POSIX). When false, CompositeBackend
    // gracefully degrades to Vulkan-only mode (no shared GL texture).
    [[nodiscard]] bool external_memory_supported() const noexcept {
        return external_memory_enabled_;
    }
    // Same for semaphore export (VK→GL sync without device_wait_idle).
    [[nodiscard]] bool external_semaphore_supported() const noexcept {
        return external_semaphore_enabled_;
    }

    // Esperar a que el device termine todo el trabajo en curso.
    // Llamar antes de destruir recursos del swapchain.
    void device_wait_idle() const noexcept;

private:
    // ── Pasos internos de init ────────────────────────────────────────────────
    [[nodiscard]] bool create_instance(GLFWwindow* window) noexcept;
    [[nodiscard]] bool create_surface(GLFWwindow* window)  noexcept;
    [[nodiscard]] bool select_physical_device()            noexcept;
    [[nodiscard]] bool find_queue_families()               noexcept;
    [[nodiscard]] bool create_device()                     noexcept;
    [[nodiscard]] bool create_descriptor_pool()            noexcept;

    // ── Helpers ───────────────────────────────────────────────────────────────
    [[nodiscard]] bool supports_device_extensions(VkPhysicalDevice dev) const noexcept;
    [[nodiscard]] bool setup_debug_messenger() noexcept;

    // ── Handles ───────────────────────────────────────────────────────────────
    VkInstance       instance_      {VK_NULL_HANDLE};
    VkPhysicalDevice phys_dev_      {VK_NULL_HANDLE};
    VkDevice         device_        {VK_NULL_HANDLE};
    VkSurfaceKHR     surface_       {VK_NULL_HANDLE};
    VkQueue          gfx_queue_     {VK_NULL_HANDLE};
    VkQueue          present_queue_ {VK_NULL_HANDLE};
    VkDescriptorPool desc_pool_     {VK_NULL_HANDLE};

    uint32_t gfx_family_     {UINT32_MAX};
    uint32_t present_family_ {UINT32_MAX};
    bool     initialized_    {false};

    // External-memory + semaphore extension presence flags. Populated
    // during create_device() — these extensions are NOT required for
    // basic Vulkan rendering; their absence only disables the
    // CompositeBackend VK↔GL interop path.
    bool external_memory_enabled_    {false};
    bool external_semaphore_enabled_ {false};

#ifndef NDEBUG
    VkDebugUtilsMessengerEXT debug_messenger_{VK_NULL_HANDLE};
#endif
};

} // namespace phyriad::render::vulkan
// Made with my soul - Swately <3
