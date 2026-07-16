// framework/render/vulkan/include/phyriad/render/vulkan/BlendPipeline.hpp
// BlendPipeline — reusable Vulkan compute pipeline for pixel-wise
// pairwise image blending (LinearBlend of A,B → C).
//
// Encapsulates the descriptor set layout, pipeline layout, compute
// pipeline, sampler, and rotating descriptor-set ring needed to dispatch
// the `frame_blend_linear.comp` shader. The SPV bytecode is embedded at
// build time via the spv_to_header CMake rule.
//
// Lifecycle:
//   pipe.init(device, phys_dev, max_in_flight)   // once at backend init
//     for each frame:
//       pipe.record_blend(cmd, a_view, b_view, c_view, w, h)
//   pipe.shutdown(device)
//
// Layout invariants (caller responsibility — record_blend does NOT
// transition images):
//   a_view, b_view  →  layout SHADER_READ_ONLY_OPTIMAL or GENERAL,
//                      created from images with USAGE_SAMPLED_BIT
//   c_view          →  layout GENERAL, created from an image with
//                      USAGE_STORAGE_BIT
//
// Threading: a single BlendPipeline instance is NOT thread-safe — the
// internal descriptor-set ring advances on each record_blend(). Use one
// instance per recording thread.
//
#pragma once
#include <vulkan/vulkan.h>
#include <cstdint>

namespace phyriad::render::vulkan {

class BlendPipeline {
public:
    BlendPipeline()  noexcept = default;
    ~BlendPipeline() noexcept = default;

    BlendPipeline(BlendPipeline const&)            = delete;
    BlendPipeline& operator=(BlendPipeline const&) = delete;
    BlendPipeline(BlendPipeline&&)                 = delete;
    BlendPipeline& operator=(BlendPipeline&&)      = delete;

    // ── init ───────────────────────────────────────────────────────────
    // Creates the sampler, descriptor set layout, pipeline layout, the
    // compute pipeline (loading the embedded SPV), the descriptor pool
    // sized for `max_in_flight` concurrent dispatches, and pre-allocates
    // `max_in_flight` descriptor sets.
    //
    // Returns true on success; on failure all partial objects are
    // destroyed and the instance is left in its initial state.
    //
    // Reasonable max_in_flight: the FramePool size of the consumer (4
    // for ui::FrameInterpolatorNode). The descriptor-set ring rotates
    // through these so back-to-back dispatches don't collide.
    [[nodiscard]] bool init(VkDevice         device,
                            VkPhysicalDevice phys_dev,
                            uint32_t         max_in_flight) noexcept;

    // ── shutdown ─────────────────────────────────────────────────────
    // Destroys all Vulkan objects in reverse creation order. Idempotent
    // — safe to call on an uninitialised instance or twice.
    void shutdown(VkDevice device) noexcept;

    [[nodiscard]] bool initialized() const noexcept { return pipeline_ != VK_NULL_HANDLE; }

    // ── record_blend ─────────────────────────────────────────────────
    // Records the compute dispatch into `cmd`. Does NOT call vkBegin /
    // vkEnd / vkQueueSubmit — the caller manages command-buffer
    // recording and submission. The caller is also responsible for
    // image-layout transitions before and after this call.
    //
    // Workgroup count = ceil(width/8) × ceil(height/8) × 1.
    //
    // Returns false if the descriptor-set ring is exhausted (caller is
    // dispatching more concurrent blends than `max_in_flight` allows).
    [[nodiscard]] bool record_blend(VkCommandBuffer cmd,
                                    VkImageView     a_view,
                                    VkImageView     b_view,
                                    VkImageView     c_view,
                                    uint32_t        width,
                                    uint32_t        height) noexcept;

    // ── Accessors (for tests / diagnostics) ──────────────────────────
    [[nodiscard]] VkPipeline           pipeline()        const noexcept { return pipeline_; }
    [[nodiscard]] VkPipelineLayout     pipeline_layout() const noexcept { return pipeline_layout_; }
    [[nodiscard]] VkDescriptorSetLayout dsl()             const noexcept { return dsl_; }

private:
    VkDevice              device_         {VK_NULL_HANDLE};
    VkSampler             sampler_        {VK_NULL_HANDLE};
    VkDescriptorSetLayout dsl_            {VK_NULL_HANDLE};
    VkPipelineLayout      pipeline_layout_{VK_NULL_HANDLE};
    VkPipeline            pipeline_       {VK_NULL_HANDLE};
    VkDescriptorPool      desc_pool_      {VK_NULL_HANDLE};

    // Descriptor-set ring. We pre-allocate max_in_flight sets and
    // rotate; the consumer guarantees no in-flight dispatch reuses a
    // set before its previous use has completed (via the frame fence).
    static constexpr uint32_t kMaxInFlight = 16u;  // hard cap (covers FramePool 4 + headroom)
    VkDescriptorSet       sets_[kMaxInFlight]{};
    uint32_t              n_sets_         {0u};
    uint32_t              next_set_       {0u};
};

} // namespace phyriad::render::vulkan
// Made with my soul - Swately <3
