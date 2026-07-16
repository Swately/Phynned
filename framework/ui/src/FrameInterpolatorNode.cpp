// framework/ui/src/FrameInterpolatorNode.cpp
// FrameInterpolatorNode implementation.
//
// v1 (LinearBlend) — sin contexto Vulkan: blends el handle_id de A y B
//   en el RenderedFrame de salida. El FramePool slot de C es adquirido
//   y la mezcla se realiza en GPU mediante un compute pipeline.
//
// Sin Vulkan (PassThrough): publica el frame recibido sin modificar.
//
#include <phyriad/ui/FrameInterpolatorNode.hpp>
#include <phyriad/hal/Timestamp.hpp>
#include <cstdio>
#include <cstring>

namespace phyriad::ui {

// ─────────────────────────────────────────────────────────────────────────────
// Construction
// ─────────────────────────────────────────────────────────────────────────────
FrameInterpolatorNode::FrameInterpolatorNode(Algorithm algo) noexcept
    : algo_(algo)
{}

FrameInterpolatorNode::~FrameInterpolatorNode() noexcept
{
    on_stop();
}

// ─────────────────────────────────────────────────────────────────────────────
// bind_frame_pool (Vulkan only)
// ─────────────────────────────────────────────────────────────────────────────
#ifdef PHYRIAD_BUILD_VULKAN
void FrameInterpolatorNode::bind_frame_pool(
    render::vulkan::FramePool* pool,
    VkDevice                   device,
    VkPhysicalDevice           phys_device,
    VkQueue                    compute_queue,
    uint32_t                   compute_family) noexcept
{
    pool_           = pool;
    device_         = device;
    phys_device_    = phys_device;
    compute_queue_  = compute_queue;
    compute_family_ = compute_family;
}
#endif

// ─────────────────────────────────────────────────────────────────────────────
// on_start
// ─────────────────────────────────────────────────────────────────────────────
std::expected<void, phyriad::Error> FrameInterpolatorNode::on_start() noexcept
{
#ifdef PHYRIAD_BUILD_VULKAN
    const bool needs_compute = (algo_ == Algorithm::LinearBlend ||
                                algo_ == Algorithm::OpticalFlow);
    if (needs_compute && pool_ && device_ != VK_NULL_HANDLE) {
        // Validate that bind_frame_pool() supplied everything we need.
        if (phys_device_ == VK_NULL_HANDLE || compute_queue_ == VK_NULL_HANDLE ||
            compute_family_ == UINT32_MAX)
        {
            std::fprintf(stderr,
                "[FrameInterpolatorNode] %s: bind_frame_pool() did not supply "
                "phys_device + queue + family — falling back to PassThrough.\n",
                (algo_ == Algorithm::LinearBlend) ? "LinearBlend" : "OpticalFlow");
            algo_ = Algorithm::PassThrough;
            return {};
        }

        // Command pool + buffer for compute submissions.
        VkCommandPoolCreateInfo pool_info{};
        pool_info.sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        pool_info.queueFamilyIndex = compute_family_;
        pool_info.flags            = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        if (vkCreateCommandPool(device_, &pool_info, nullptr, &cmd_pool_) != VK_SUCCESS) {
            return std::unexpected(phyriad::Error{phyriad::ErrorCode::ResourceInitFailed});
        }

        VkCommandBufferAllocateInfo alloc_info{};
        alloc_info.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        alloc_info.commandPool        = cmd_pool_;
        alloc_info.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        alloc_info.commandBufferCount = 1u;
        if (vkAllocateCommandBuffers(device_, &alloc_info, &cmd_buf_) != VK_SUCCESS) {
            return std::unexpected(phyriad::Error{phyriad::ErrorCode::ResourceInitFailed});
        }

        // Fence for tick-bounded compute waits. Start signaled so the
        // first tick can vkResetFences before submitting.
        VkFenceCreateInfo fence_info{};
        fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        fence_info.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        if (vkCreateFence(device_, &fence_info, nullptr, &fence_) != VK_SUCCESS) {
            return std::unexpected(phyriad::Error{phyriad::ErrorCode::ResourceInitFailed});
        }

        // Initialise the pipeline matching the chosen algorithm.
        // Both share cmd_pool_/cmd_buf_/fence_ but own their own
        // descriptor pools + pipelines.
        if (algo_ == Algorithm::LinearBlend) {
            if (!blend_pipe_.init(device_, phys_device_,
                                   render::vulkan::FramePool::kPoolSize))
            {
                std::fprintf(stderr,
                    "[FrameInterpolatorNode] BlendPipeline::init failed — "
                    "falling back to PassThrough.\n");
                algo_ = Algorithm::PassThrough;
                return {};
            }
        } else /* Algorithm::OpticalFlow */ {
            if (!of_pipe_.init(phys_device_, device_,
                                pool_->width(), pool_->height(),
                                render::vulkan::FramePool::kPoolSize,
                                /*search_radius*/ 8))
            {
                std::fprintf(stderr,
                    "[FrameInterpolatorNode] OpticalFlowPipeline::init failed "
                    "— falling back to PassThrough.\n");
                algo_ = Algorithm::PassThrough;
                return {};
            }
        }
    }
#endif
    return {};
}

// ─────────────────────────────────────────────────────────────────────────────
// blend_linear — produces interpolated RenderedFrame metadata.
// ─────────────────────────────────────────────────────────────────────────────
// Produces an interpolated RenderedFrame C between A and B. On Vulkan
// builds with an initialised BlendPipeline, this dispatches a compute
// shader that writes the pixel-wise midpoint of A's and B's images into
// a freshly-acquired FramePool slot. The returned RenderedFrame carries
// C's slot id so the PresentNode releases the correct slot after present.
//
// When the BlendPipeline is not available (no FramePool bound, no
// Vulkan, on_start() failed) blend_linear silently degenerates to
// metadata-only interpolation reusing A's slot — the downstream
// presenter sees a "blended" timing label without an actual pixel blend.
// The PassThrough fallback in on_start ensures this path only runs
// during graceful degradation.
RenderedFrame FrameInterpolatorNode::blend_linear(
    const RenderedFrame& a,
    const RenderedFrame& b) noexcept
{
    RenderedFrame c{};
    // Interpolated frame timing: midpoint between A and B.
    c.cpu_time_ns    = (a.cpu_time_ns + b.cpu_time_ns) / 2u;
    c.gpu_time_ns    = (a.gpu_time_ns + b.gpu_time_ns) / 2u;
    c.present_tsc    = (a.present_tsc + b.present_tsc)  / 2u;
    c.frame_id       = a.frame_id;        // C is conceptually "between" A and B
    c.image_index    = a.image_index;     // overwritten if real blend succeeds
    c.handle_id      = a.handle_id;       // overwritten if real blend succeeds
    c.is_interpolated = 1u;

#ifdef PHYRIAD_BUILD_VULKAN
    // Real GPU blend path.
    if (pool_ && blend_pipe_.initialized() && cmd_buf_ != VK_NULL_HANDLE &&
        a.handle_id < render::vulkan::FramePool::kPoolSize &&
        b.handle_id < render::vulkan::FramePool::kPoolSize)
    {
        // Acquire a free slot from the pool for the output frame C.
        const uint32_t c_idx = pool_->acquire();
        if (c_idx == UINT32_MAX) {
            // Pool exhausted — caller will be publishing more frames than
            // the pool has slots. Skip the real blend; metadata-only C
            // reuses A's slot (downstream will see a duplicate present).
            return c;
        }

        const auto& slot_a = pool_->slot(a.handle_id);
        const auto& slot_b = pool_->slot(b.handle_id);
        auto&       slot_c = pool_->slot(c_idx);

        // Wait for the previous tick's submission to complete before
        // reusing cmd_buf_ + descriptor set.
        (void)vkWaitForFences(device_, 1u, &fence_, VK_TRUE, UINT64_MAX);
        (void)vkResetFences(device_, 1u, &fence_);
        (void)vkResetCommandBuffer(cmd_buf_, 0u);

        VkCommandBufferBeginInfo bi{};
        bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        (void)vkBeginCommandBuffer(cmd_buf_, &bi);

        // Transition the output slot to GENERAL for storage write. Inputs
        // are assumed to be in SHADER_READ_ONLY_OPTIMAL — the RenderNode
        // upstream produced them via colour-attachment render passes, the
        // present-side barrier from the previous frame already left them
        // shader-readable. If they weren't (e.g. on first use), this is a
        // validation-layer error the caller will see in their log.
        VkImageMemoryBarrier to_general{};
        to_general.sType                       = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        to_general.oldLayout                   = VK_IMAGE_LAYOUT_UNDEFINED;
        to_general.newLayout                   = VK_IMAGE_LAYOUT_GENERAL;
        to_general.srcAccessMask               = 0u;
        to_general.dstAccessMask               = VK_ACCESS_SHADER_WRITE_BIT;
        to_general.srcQueueFamilyIndex         = VK_QUEUE_FAMILY_IGNORED;
        to_general.dstQueueFamilyIndex         = VK_QUEUE_FAMILY_IGNORED;
        to_general.image                       = slot_c.image;
        to_general.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        to_general.subresourceRange.levelCount = 1u;
        to_general.subresourceRange.layerCount = 1u;
        vkCmdPipelineBarrier(cmd_buf_,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            0u, 0u, nullptr, 0u, nullptr, 1u, &to_general);

        // Dispatch the blend.
        const uint32_t w = pool_->width();
        const uint32_t h = pool_->height();
        (void)blend_pipe_.record_blend(cmd_buf_,
            slot_a.view, slot_b.view, slot_c.view, w, h);

        // Transition C to SHADER_READ_ONLY so the next consumer (Present
        // sampler, or a future tick's blend) finds the image ready to read.
        VkImageMemoryBarrier to_sampled{};
        to_sampled.sType                       = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        to_sampled.oldLayout                   = VK_IMAGE_LAYOUT_GENERAL;
        to_sampled.newLayout                   = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        to_sampled.srcAccessMask               = VK_ACCESS_SHADER_WRITE_BIT;
        to_sampled.dstAccessMask               = VK_ACCESS_SHADER_READ_BIT;
        to_sampled.srcQueueFamilyIndex         = VK_QUEUE_FAMILY_IGNORED;
        to_sampled.dstQueueFamilyIndex         = VK_QUEUE_FAMILY_IGNORED;
        to_sampled.image                       = slot_c.image;
        to_sampled.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        to_sampled.subresourceRange.levelCount = 1u;
        to_sampled.subresourceRange.layerCount = 1u;
        vkCmdPipelineBarrier(cmd_buf_,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT
                | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            0u, 0u, nullptr, 0u, nullptr, 1u, &to_sampled);

        (void)vkEndCommandBuffer(cmd_buf_);

        VkSubmitInfo si{};
        si.sType              = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        si.commandBufferCount = 1u;
        si.pCommandBuffers    = &cmd_buf_;
        (void)vkQueueSubmit(compute_queue_, 1u, &si, fence_);
        // We do NOT wait synchronously — the next tick's vkWaitForFences
        // will cover it, and the PresentNode's own fence guards the GPU-
        // side ordering against present.

        // Update C's metadata to point at the real output slot.
        c.image_index = c_idx;
        c.handle_id   = c_idx;
    }
#endif
    return c;
}

// ─────────────────────────────────────────────────────────────────────────────
// tick
// ─────────────────────────────────────────────────────────────────────────────
std::expected<void, phyriad::Error> FrameInterpolatorNode::tick() noexcept
{
    // Poll the inlet for a new frame (receive() returns RingEmpty when no data).
    auto result = inlet_.receive();
    if (!result.has_value()) {
        // RingEmpty or other transient — skip this tick.
        return {};
    }
    const RenderedFrame& current = *result;

    switch (algo_) {
    case Algorithm::PassThrough:
    default:
        // Pass the frame through unchanged.
        (void)outlet_.publish(current);
        break;

    case Algorithm::LinearBlend:
        if (has_prev_) {
            // Publish: prev_frame_ (A), interpolated (C), current (B).
            (void)outlet_.publish(prev_frame_);
            (void)outlet_.publish(blend_linear(prev_frame_, current));
            (void)outlet_.publish(current);
        } else {
            // First frame — no previous to blend with.
            (void)outlet_.publish(current);
        }
        break;

    case Algorithm::OpticalFlow:
        if (has_prev_) {
            // Publish: prev_frame_ (A), motion-warped midpoint (C), current (B).
            (void)outlet_.publish(prev_frame_);
            (void)outlet_.publish(blend_optical_flow(prev_frame_, current));
            (void)outlet_.publish(current);
        } else {
            // First frame — no previous to derive motion from.
            (void)outlet_.publish(current);
        }
        break;
    }

    prev_frame_ = current;
    has_prev_   = true;
    return {};
}

// ─────────────────────────────────────────────────────────────────────────────
// blend_optical_flow — produces motion-vector-warped interpolated frame.
// ─────────────────────────────────────────────────────────────────────────────
// On Vulkan builds with the OpticalFlowPipeline initialised, this runs
// the two-stage compute (block-match → warp) on the slot images of A and
// B and writes the interpolated frame into a freshly-acquired FramePool
// slot. Falls back to metadata-only interpolation (reusing A's slot)
// when the pipeline is unavailable, matching the LinearBlend pattern.
RenderedFrame FrameInterpolatorNode::blend_optical_flow(
    const RenderedFrame& a,
    const RenderedFrame& b) noexcept
{
    RenderedFrame c{};
    c.cpu_time_ns    = (a.cpu_time_ns + b.cpu_time_ns) / 2u;
    c.gpu_time_ns    = (a.gpu_time_ns + b.gpu_time_ns) / 2u;
    c.present_tsc    = (a.present_tsc + b.present_tsc)  / 2u;
    c.frame_id       = a.frame_id;
    c.image_index    = a.image_index;
    c.handle_id      = a.handle_id;
    c.is_interpolated = 1u;

#ifdef PHYRIAD_BUILD_VULKAN
    if (pool_ && of_pipe_.initialized() && cmd_buf_ != VK_NULL_HANDLE &&
        a.handle_id < render::vulkan::FramePool::kPoolSize &&
        b.handle_id < render::vulkan::FramePool::kPoolSize)
    {
        const uint32_t c_idx = pool_->acquire();
        if (c_idx == UINT32_MAX) return c;   // pool exhausted — return passthrough

        const auto& slot_a = pool_->slot(a.handle_id);
        const auto& slot_b = pool_->slot(b.handle_id);
        auto&       slot_c = pool_->slot(c_idx);

        (void)vkWaitForFences(device_, 1u, &fence_, VK_TRUE, UINT64_MAX);
        (void)vkResetFences(device_, 1u, &fence_);
        (void)vkResetCommandBuffer(cmd_buf_, 0u);

        VkCommandBufferBeginInfo bi{};
        bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        (void)vkBeginCommandBuffer(cmd_buf_, &bi);

        // Transition output slot to GENERAL — same pattern as blend_linear.
        VkImageMemoryBarrier to_general{};
        to_general.sType                       = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        to_general.oldLayout                   = VK_IMAGE_LAYOUT_UNDEFINED;
        to_general.newLayout                   = VK_IMAGE_LAYOUT_GENERAL;
        to_general.srcAccessMask               = 0u;
        to_general.dstAccessMask               = VK_ACCESS_SHADER_WRITE_BIT;
        to_general.srcQueueFamilyIndex         = VK_QUEUE_FAMILY_IGNORED;
        to_general.dstQueueFamilyIndex         = VK_QUEUE_FAMILY_IGNORED;
        to_general.image                       = slot_c.image;
        to_general.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        to_general.subresourceRange.levelCount = 1u;
        to_general.subresourceRange.layerCount = 1u;
        vkCmdPipelineBarrier(cmd_buf_,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            0u, 0u, nullptr, 0u, nullptr, 1u, &to_general);

        // Run the two-stage pipeline (block-match + warp).
        (void)of_pipe_.record_optical_flow(cmd_buf_,
            slot_a.view, slot_b.view, slot_c.view);

        // Output → SHADER_READ_ONLY for downstream consumers.
        VkImageMemoryBarrier to_sampled{};
        to_sampled.sType                       = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        to_sampled.oldLayout                   = VK_IMAGE_LAYOUT_GENERAL;
        to_sampled.newLayout                   = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        to_sampled.srcAccessMask               = VK_ACCESS_SHADER_WRITE_BIT;
        to_sampled.dstAccessMask               = VK_ACCESS_SHADER_READ_BIT;
        to_sampled.srcQueueFamilyIndex         = VK_QUEUE_FAMILY_IGNORED;
        to_sampled.dstQueueFamilyIndex         = VK_QUEUE_FAMILY_IGNORED;
        to_sampled.image                       = slot_c.image;
        to_sampled.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        to_sampled.subresourceRange.levelCount = 1u;
        to_sampled.subresourceRange.layerCount = 1u;
        vkCmdPipelineBarrier(cmd_buf_,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT
                | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            0u, 0u, nullptr, 0u, nullptr, 1u, &to_sampled);

        (void)vkEndCommandBuffer(cmd_buf_);

        VkSubmitInfo si{};
        si.sType              = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        si.commandBufferCount = 1u;
        si.pCommandBuffers    = &cmd_buf_;
        (void)vkQueueSubmit(compute_queue_, 1u, &si, fence_);

        c.image_index = c_idx;
        c.handle_id   = c_idx;
    }
#endif
    return c;
}

// ─────────────────────────────────────────────────────────────────────────────
// on_stop
// ─────────────────────────────────────────────────────────────────────────────
void FrameInterpolatorNode::on_stop() noexcept
{
    has_prev_ = false;
#ifdef PHYRIAD_BUILD_VULKAN
    if (device_ != VK_NULL_HANDLE) {
        // Drain any in-flight compute work before destroying its resources.
        if (fence_ != VK_NULL_HANDLE) {
            (void)vkWaitForFences(device_, 1u, &fence_, VK_TRUE, 1'000'000'000ull);
        }
        // Shutdown both pipelines (only the one matching algo_ is actually
        // initialised; shutdown of an uninitialised one is a no-op).
        blend_pipe_.shutdown(device_);
        of_pipe_.shutdown(device_);
        if (fence_    != VK_NULL_HANDLE) { vkDestroyFence      (device_, fence_,    nullptr); fence_    = VK_NULL_HANDLE; }
        if (cmd_pool_ != VK_NULL_HANDLE) { vkDestroyCommandPool(device_, cmd_pool_, nullptr); cmd_pool_ = VK_NULL_HANDLE; cmd_buf_ = VK_NULL_HANDLE; }
    }
#endif
}

} // namespace phyriad::ui
// Made with my soul - Swately <3
