// framework/ui/src/PresentNode.cpp
// PresentNode — terminal node that presents frames to the display.
//
#include <phyriad/ui/PresentNode.hpp>
#include <phyriad/hal/Timestamp.hpp>
#include <cstdio>
#include <cstring>

namespace phyriad::ui {

// ─────────────────────────────────────────────────────────────────────────────
// Destructor
// ─────────────────────────────────────────────────────────────────────────────
PresentNode::~PresentNode() noexcept
{
    on_stop();
}

// ─────────────────────────────────────────────────────────────────────────────
// bind_vulkan
// ─────────────────────────────────────────────────────────────────────────────
#ifdef PHYRIAD_BUILD_VULKAN
void PresentNode::bind_vulkan(render::vulkan::FramePool* pool,
                               render::vulkan::Swapchain* swapchain,
                               VkDevice                   device,
                               VkQueue                    present_queue) noexcept
{
    pool_          = pool;
    swapchain_     = swapchain;
    device_        = device;
    present_queue_ = present_queue;
}
#endif

// ─────────────────────────────────────────────────────────────────────────────
// on_start
// ─────────────────────────────────────────────────────────────────────────────
std::expected<void, phyriad::Error> PresentNode::on_start() noexcept
{
#ifdef PHYRIAD_BUILD_VULKAN
    if (device_ != VK_NULL_HANDLE) {
        VkFenceCreateInfo fence_info{};
        fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        fence_info.flags = VK_FENCE_CREATE_SIGNALED_BIT;

        for (uint32_t i = 0u; i < kFenceCount; ++i) {
            if (vkCreateFence(device_, &fence_info, nullptr, &fences_[i]) != VK_SUCCESS) {
                return std::unexpected(phyriad::Error{phyriad::ErrorCode::ResourceInitFailed});
            }
        }
        fences_initialized_ = true;
    }
#endif
    return {};
}

// ─────────────────────────────────────────────────────────────────────────────
// tick
// ─────────────────────────────────────────────────────────────────────────────
std::expected<void, phyriad::Error> PresentNode::tick() noexcept
{
    // Poll for a rendered frame.
    auto result = inlet_.receive();
    if (!result.has_value()) return {};

    const RenderedFrame& rf = *result;
    if (rf.handle_id == UINT32_MAX) return {};  // invalid frame — skip

#ifdef PHYRIAD_BUILD_VULKAN
    // ── Vulkan present path ───────────────────────────────────────────────────
    if (pool_ && swapchain_ && device_ != VK_NULL_HANDLE) {
        const uint32_t slot = rf.handle_id;

        // Wait for any previous use of this fence to complete.
        if (fences_initialized_ && slot < kFenceCount) {
            vkWaitForFences(device_, 1u, &fences_[slot], VK_TRUE, UINT64_MAX);
            vkResetFences  (device_, 1u, &fences_[slot]);
        }

        // Present to swapchain via vkQueuePresentKHR.
        // (Full implementation deferred — Swapchain::present() encapsulates this.)
        // For now, release the pool slot immediately after the "present".
        if (pool_) pool_->release(slot);
    }
#endif

    // Build and publish RenderStats.
    render::RenderStats stats{};
    stats.frame_id     = frame_counter_++;
    stats.present_tsc  = phyriad::hal::rdtsc();

    // Lazily acquire the calibrated TSC frequency on the first frame. The
    // calibration costs one ~10 ms sleep and is cached by the HAL, so all
    // future PresentNode instances reuse the same result. Until the first
    // frame is published, prev_present_tsc_ is zero — emit 0 ms to signal
    // "no delta available yet" rather than a garbage value.
    if (tsc_freq_ == 0u) [[unlikely]] {
        tsc_freq_ = phyriad::hal::calibrate_tsc_freq();
    }
    if (prev_present_tsc_ != 0u && tsc_freq_ > 0u) {
        const uint64_t delta = stats.present_tsc - prev_present_tsc_;
        // delta * 1000 / tsc_freq → ms with sub-ms resolution. We promote
        // to double before the division to keep the fractional ms (cheap on
        // x86; this isn't a hot loop — runs once per presented frame).
        stats.frame_time_ms = static_cast<float>(
            static_cast<double>(delta) * 1000.0 /
            static_cast<double>(tsc_freq_));
    } else {
        stats.frame_time_ms = 0.f;
    }
    prev_present_tsc_ = stats.present_tsc;

    (void)outlet_.publish(stats);
    return {};
}

// ─────────────────────────────────────────────────────────────────────────────
// on_stop
// ─────────────────────────────────────────────────────────────────────────────
void PresentNode::on_stop() noexcept
{
#ifdef PHYRIAD_BUILD_VULKAN
    if (fences_initialized_ && device_ != VK_NULL_HANDLE) {
        for (uint32_t i = 0u; i < kFenceCount; ++i) {
            if (fences_[i] != VK_NULL_HANDLE) {
                vkDestroyFence(device_, fences_[i], nullptr);
                fences_[i] = VK_NULL_HANDLE;
            }
        }
        fences_initialized_ = false;
    }
#endif
    frame_counter_ = 0u;
}

} // namespace phyriad::ui
// Made with my soul - Swately <3
