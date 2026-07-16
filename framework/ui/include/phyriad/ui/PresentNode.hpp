// framework/ui/include/phyriad/ui/PresentNode.hpp
// PresentNode — nodo terminal del pipeline de frames.
//
// Posición en el pipeline:
//   AppLogicNode → RenderNode → FrameInterpolatorNode → PresentNode
//
// Responsabilidad:
//   Recibe RenderedFrame desde FrameInterpolatorNode (o directamente
//   desde RenderNode en el pipeline simple).
//   Presenta la imagen al swapchain (vkQueuePresentKHR / glfwSwapBuffers).
//   Libera el slot de FramePool cuando el GPU termina (vkWaitForFences).
//
// Publicaciones:
//   outlet() — render::RenderStats con frame timing y estado del present.
//
// Cuando PHYRIAD_BUILD_VULKAN=OFF, usa glfwSwapBuffers (OpenGL fallback).
//
#pragma once
#include <phyriad/ui/types/RenderedFrame.hpp>
#include <phyriad/render/RenderStats.hpp>
#include <phyriad/node/Port.hpp>
#include <phyriad/schema/Error.hpp>
#include <cstdint>
#include <expected>

#ifdef PHYRIAD_BUILD_VULKAN
#   include <phyriad/render/vulkan/FramePool.hpp>
#   include <phyriad/render/vulkan/Swapchain.hpp>
#   include <vulkan/vulkan.h>
struct GLFWwindow;
#endif

namespace phyriad::ui {

// ─────────────────────────────────────────────────────────────────────────────
// PresentNode
// ─────────────────────────────────────────────────────────────────────────────
class PresentNode {
public:
    using input_type  = RenderedFrame;
    using output_type = render::RenderStats;

    // ── Construction ─────────────────────────────────────────────────────────
    PresentNode() noexcept = default;
    ~PresentNode() noexcept;

    PresentNode(PresentNode const&)            = delete;
    PresentNode& operator=(PresentNode const&) = delete;
    PresentNode(PresentNode&&)                 = delete;
    PresentNode& operator=(PresentNode&&)      = delete;

    // ── Inlet / Outlet ────────────────────────────────────────────────────────
    [[nodiscard]] node::Inlet<RenderedFrame>&     inlet()  noexcept { return inlet_;  }
    [[nodiscard]] node::Outlet<render::RenderStats>& outlet() noexcept { return outlet_; }
    [[nodiscard]] std::size_t inlet_count() const noexcept { return 1u; }

    // ── Lifecycle ─────────────────────────────────────────────────────────────
    [[nodiscard]] auto on_start() noexcept -> std::expected<void, phyriad::Error>;
    [[nodiscard]] auto tick()     noexcept -> std::expected<void, phyriad::Error>;
    void               on_stop()  noexcept;

#ifdef PHYRIAD_BUILD_VULKAN
    // Bind Vulkan resources. Must be called before on_start().
    //   pool         — shared FramePool (owner of VkImage handles)
    //   swapchain    — the Swapchain that owns present queues + semaphores
    //   device       — VkDevice
    //   present_queue— queue for vkQueuePresentKHR
    void bind_vulkan(render::vulkan::FramePool* pool,
                     render::vulkan::Swapchain* swapchain,
                     VkDevice                   device,
                     VkQueue                    present_queue) noexcept;
#endif

private:
    node::Inlet<RenderedFrame>       inlet_;
    node::Outlet<render::RenderStats> outlet_;

    uint64_t frame_counter_{0};

    // Frame-pacing state. tsc_freq_ is initialised lazily on the first tick
    // via hal::calibrate_tsc_freq() (which sleeps ~10 ms once, then caches);
    // the calibration cost is paid out-of-band from any real frame and
    // amortised over the lifetime of the present node.
    uint64_t prev_present_tsc_{0};
    uint64_t tsc_freq_{0};

#ifdef PHYRIAD_BUILD_VULKAN
    render::vulkan::FramePool* pool_          {nullptr};
    render::vulkan::Swapchain* swapchain_     {nullptr};
    VkDevice                   device_        {VK_NULL_HANDLE};
    VkQueue                    present_queue_ {VK_NULL_HANDLE};

    // Per-in-flight fence for vkWaitForFences before release().
    static constexpr uint32_t kFenceCount = render::vulkan::FramePool::kPoolSize;
    VkFence fences_[kFenceCount]{};
    bool    fences_initialized_{false};
#endif
};

} // namespace phyriad::ui
// Made with my soul - Swately <3
