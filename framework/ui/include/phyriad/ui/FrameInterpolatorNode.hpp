// framework/ui/include/phyriad/ui/FrameInterpolatorNode.hpp
// FrameInterpolatorNode — nodo del grafo que interpola frames intermedios.
//
// Posición en el pipeline:
//   AppLogicNode → RenderNode → FrameInterpolatorNode → PresentNode
//
// Responsabilidad:
//   Recibe dos RenderedFrames consecutivos (A, B) desde RenderNode.
//   Genera un frame interpolado C = blend(A, B, 0.5) entre A y B.
//   Publica A, C, B en ese orden a PresentNode para triplicar la tasa efectiva.
//
// Implementaciones disponibles (seleccionables en el constructor):
//   v1 — LinearBlend: 50% A + 50% B en cada pixel (GPU compute shader).
//        Rápido. Produce ghosting en movimiento rápido.
//        Adecuado para UI/tool windows de baja dinámica.
//   v2 — OpticalFlow: usa motion vectors para warp A hacia la posición temporal
//        de C. Requiere que el RenderNode produzca motion vectors.
//        No-op si motion vectors no están disponibles.
//
// Cuando no hay interpolación (disabled_ = true o PHYRIAD_BUILD_VULKAN=OFF),
//   el nodo es un pass-through: publica RenderedFrame sin modificar.
//
// Thread safety: FrameInterpolatorNode corre en su propio GraphRuntime thread.
//   NO es thread-safe para llamadas concurrentes.
//
#pragma once
#include <phyriad/ui/types/RenderedFrame.hpp>
#include <phyriad/node/Port.hpp>
#include <phyriad/schema/Error.hpp>
#include <cstdint>
#include <expected>

#ifdef PHYRIAD_BUILD_VULKAN
#   include <phyriad/render/vulkan/FramePool.hpp>
#   include <phyriad/render/vulkan/BlendPipeline.hpp>
#   include <phyriad/render/vulkan/OpticalFlowPipeline.hpp>
#   include <vulkan/vulkan.h>
#endif

namespace phyriad::ui {

// ─────────────────────────────────────────────────────────────────────────────
// FrameInterpolatorNode
// ─────────────────────────────────────────────────────────────────────────────
class FrameInterpolatorNode {
public:
    // ── Algorithm selection ───────────────────────────────────────────────────
    enum class Algorithm : uint8_t {
        PassThrough  = 0,  // disabled — forward frames unchanged
        LinearBlend  = 1,  // v1: 50% A + 50% B (fast, ghosting on motion)
        OpticalFlow  = 2,  // v2: warp using motion vectors (quality)
    };

    using input_type  = RenderedFrame;
    using output_type = RenderedFrame;

    // ── Construction ─────────────────────────────────────────────────────────
    explicit FrameInterpolatorNode(Algorithm algo = Algorithm::LinearBlend) noexcept;
    ~FrameInterpolatorNode() noexcept;

    FrameInterpolatorNode(FrameInterpolatorNode const&)            = delete;
    FrameInterpolatorNode& operator=(FrameInterpolatorNode const&) = delete;
    FrameInterpolatorNode(FrameInterpolatorNode&&)                 = delete;
    FrameInterpolatorNode& operator=(FrameInterpolatorNode&&)      = delete;

    // ── Inlet / Outlet ────────────────────────────────────────────────────────
    [[nodiscard]] node::Inlet<RenderedFrame>&  inlet()  noexcept { return inlet_;  }
    [[nodiscard]] node::Outlet<RenderedFrame>& outlet() noexcept { return outlet_; }
    [[nodiscard]] std::size_t inlet_count()  const noexcept { return 1u; }

    // ── Lifecycle (GraphRuntime callbacks) ────────────────────────────────────
    [[nodiscard]] auto on_start() noexcept -> std::expected<void, phyriad::Error>;
    [[nodiscard]] auto tick()     noexcept -> std::expected<void, phyriad::Error>;
    void               on_stop()  noexcept;

    // ── Config ────────────────────────────────────────────────────────────────
    void set_algorithm(Algorithm algo) noexcept { algo_ = algo; }
    [[nodiscard]] Algorithm algorithm() const noexcept { return algo_; }

#ifdef PHYRIAD_BUILD_VULKAN
    // Bind to the shared FramePool that RenderNode writes to and PresentNode reads from.
    // Must be called before on_start().
    //
    // `phys_device` is required to find memory types when the BlendPipeline
    // allocates descriptor-set ring storage. It MUST be the physical device
    // backing `device` — typically obtained from VulkanContext::physical_device().
    void bind_frame_pool(render::vulkan::FramePool* pool,
                         VkDevice                   device,
                         VkPhysicalDevice           phys_device,
                         VkQueue                    compute_queue,
                         uint32_t                   compute_family) noexcept;
#endif

private:
    node::Inlet<RenderedFrame>  inlet_;
    node::Outlet<RenderedFrame> outlet_;
    Algorithm                   algo_     {Algorithm::LinearBlend};

    // Previous frame buffer (needed to generate C = blend(A, B)).
    RenderedFrame  prev_frame_{};
    bool           has_prev_  {false};

#ifdef PHYRIAD_BUILD_VULKAN
    render::vulkan::FramePool*           pool_           {nullptr};
    VkDevice                             device_         {VK_NULL_HANDLE};
    VkPhysicalDevice                     phys_device_    {VK_NULL_HANDLE};
    VkQueue                              compute_queue_  {VK_NULL_HANDLE};
    uint32_t                             compute_family_ {UINT32_MAX};
    VkCommandPool                        cmd_pool_       {VK_NULL_HANDLE};
    VkCommandBuffer                      cmd_buf_        {VK_NULL_HANDLE};
    VkFence                              fence_          {VK_NULL_HANDLE};
    render::vulkan::BlendPipeline        blend_pipe_     {};
    // OpticalFlow pipeline shares cmd_pool_/cmd_buf_/fence_ with the
    // BlendPipeline above. Only the algorithm chosen at construction
    // time decides which gets initialised in on_start().
    render::vulkan::OpticalFlowPipeline  of_pipe_        {};
#endif

    // ── Internal helpers ──────────────────────────────────────────────────────
    RenderedFrame blend_linear(const RenderedFrame& a,
                               const RenderedFrame& b) noexcept;
    RenderedFrame blend_optical_flow(const RenderedFrame& a,
                                     const RenderedFrame& b) noexcept;
};

} // namespace phyriad::ui
// Made with my soul - Swately <3
