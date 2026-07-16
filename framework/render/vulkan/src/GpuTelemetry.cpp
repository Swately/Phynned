// framework/render/vulkan/src/GpuTelemetry.cpp
// GpuTelemetry implementation — VK_QUERY_TYPE_TIMESTAMP query pool.
//
// Strategy:
//   One VkQueryPool with kQueryCount=2 queries per frame.
//   Query 0: VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT   — frame work start
//   Query 1: VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT — frame work end
//
//   GPU frame time (ns) = (t1 - t0) * timestamp_period_ns
//
//   timestamp_period_ns is obtained from VkPhysicalDeviceProperties.limits.timestampPeriod.
//   It is in nanoseconds per GPU tick and is device-specific (typically 1 ns for
//   NVIDIA and AMD discrete GPUs; may differ on mobile or iGPUs).
//
#ifdef PHYRIAD_BUILD_VULKAN
#include <phyriad/render/vulkan/GpuTelemetry.hpp>
#include <vulkan/vulkan.h>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <algorithm>

namespace phyriad::render::vulkan {

// ─────────────────────────────────────────────────────────────────────────────
// Destructor
// ─────────────────────────────────────────────────────────────────────────────
GpuTelemetry::~GpuTelemetry() noexcept
{
    // Caller must call shutdown(device) before destruction.
    // If they forgot, log and continue (no VkDevice reference stored).
    if (initialized_) {
        std::fprintf(stderr,
            "[GpuTelemetry] WARNING: destroyed while initialized — "
            "call shutdown(device) first to avoid a VkQueryPool leak.\n");
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// init
// ─────────────────────────────────────────────────────────────────────────────
bool GpuTelemetry::init(VkDevice         device,
                         VkPhysicalDevice phys_device,
                         uint32_t         queue_family) noexcept
{
    if (initialized_) return true;   // idempotent

    // ── Check timestamp support ───────────────────────────────────────────────
    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(phys_device, &props);

    // timestampComputeAndGraphics: true when timestamps are supported on the
    // graphics/compute queue. If false, timestamps are queue-family–specific;
    // check VkQueueFamilyProperties.timestampValidBits.
    if (props.limits.timestampPeriod == 0.0f) {
        std::fprintf(stderr,
            "[GpuTelemetry] Device does not support timestamps (timestampPeriod==0).\n");
        return false;
    }

    // Per-queue family: check timestampValidBits ≥ 36 (Vulkan mandates ≥ 36
    // for valid timestamps; 0 means timestamps not supported on this family).
    uint32_t qf_count = 0u;
    vkGetPhysicalDeviceQueueFamilyProperties(phys_device, &qf_count, nullptr);
    if (queue_family >= qf_count) {
        std::fprintf(stderr,
            "[GpuTelemetry] Invalid queue_family=%u (device has %u).\n",
            queue_family, qf_count);
        return false;
    }
    // Temporarily allocate on the stack for small queue family counts;
    // fall back to heap for larger counts.
    constexpr uint32_t kMaxStackFamilies = 8u;
    VkQueueFamilyProperties stack_props[kMaxStackFamilies];
    VkQueueFamilyProperties* qf_props = stack_props;
    VkQueueFamilyProperties* heap_props = nullptr;
    if (qf_count > kMaxStackFamilies) {
        heap_props = new (std::nothrow) VkQueueFamilyProperties[qf_count];
        if (!heap_props) {
            std::fprintf(stderr, "[GpuTelemetry] OOM querying queue families.\n");
            return false;
        }
        qf_props = heap_props;
    }
    vkGetPhysicalDeviceQueueFamilyProperties(phys_device, &qf_count, qf_props);
    const uint32_t valid_bits = qf_props[queue_family].timestampValidBits;
    delete[] heap_props;

    if (valid_bits == 0u) {
        std::fprintf(stderr,
            "[GpuTelemetry] Queue family %u does not support timestamps.\n",
            queue_family);
        return false;
    }

    timestamp_period_ns_ = props.limits.timestampPeriod;

    // ── Create query pool ─────────────────────────────────────────────────────
    VkQueryPoolCreateInfo pool_info{};
    pool_info.sType      = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
    pool_info.queryType  = VK_QUERY_TYPE_TIMESTAMP;
    pool_info.queryCount = kQueryCount;

    if (vkCreateQueryPool(device, &pool_info, nullptr, &query_pool_) != VK_SUCCESS) {
        std::fprintf(stderr, "[GpuTelemetry] vkCreateQueryPool failed.\n");
        return false;
    }

    initialized_ = true;
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// shutdown
// ─────────────────────────────────────────────────────────────────────────────
void GpuTelemetry::shutdown(VkDevice device) noexcept
{
    if (!initialized_) return;
    if (query_pool_ != VK_NULL_HANDLE) {
        vkDestroyQueryPool(device, query_pool_, nullptr);
        query_pool_ = VK_NULL_HANDLE;
    }
    initialized_  = false;
    frame_open_   = false;
}

// ─────────────────────────────────────────────────────────────────────────────
// begin_frame
// ─────────────────────────────────────────────────────────────────────────────
void GpuTelemetry::begin_frame(VkCommandBuffer cmd_buf) noexcept
{
    if (!initialized_) return;

    // Reset both queries at the beginning of the command buffer so that
    // the previous frame's results do not linger.
    vkCmdResetQueryPool(cmd_buf, query_pool_, 0u, kQueryCount);

    // Record the "frame start" timestamp at the very top of the pipeline.
    vkCmdWriteTimestamp(cmd_buf,
                        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                        query_pool_,
                        0u);   // query slot 0
    frame_open_ = true;
}

// ─────────────────────────────────────────────────────────────────────────────
// end_frame
// ─────────────────────────────────────────────────────────────────────────────
void GpuTelemetry::end_frame(VkCommandBuffer cmd_buf) noexcept
{
    if (!initialized_ || !frame_open_) return;

    // Record the "frame end" timestamp after all rendering work is done.
    vkCmdWriteTimestamp(cmd_buf,
                        VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                        query_pool_,
                        1u);   // query slot 1
    frame_open_ = false;
}

// ─────────────────────────────────────────────────────────────────────────────
// read_results
// ─────────────────────────────────────────────────────────────────────────────
phyriad::render::GpuMetrics
GpuTelemetry::read_results(VkDevice device,
                             uint64_t queue_submit_cpu_ns) noexcept
{
    phyriad::render::GpuMetrics metrics{};
    metrics.queue_submit_ns = queue_submit_cpu_ns;

    if (!initialized_ || frame_open_) {
        // frame_open_ being true means end_frame() was not called — no valid data.
        return metrics;
    }

    // ── Read back the two timestamp values ────────────────────────────────────
    // VK_QUERY_RESULT_64_BIT ensures 64-bit readback regardless of device.
    // VK_QUERY_RESULT_WAIT_BIT makes this call block until the GPU writes the
    // results; this is safe to call after the frame fence has been signalled.
    uint64_t timestamps[kQueryCount]{UINT64_MAX, UINT64_MAX};
    VkResult res = vkGetQueryPoolResults(
        device,
        query_pool_,
        0u,                    // first query
        kQueryCount,           // query count
        sizeof(timestamps),    // data size in bytes
        timestamps,            // out buffer
        sizeof(uint64_t),      // stride
        VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);

    if (res != VK_SUCCESS) {
        // VK_NOT_READY can occur if called before the fence — log and return zeros.
        std::fprintf(stderr,
            "[GpuTelemetry] vkGetQueryPoolResults returned %d.\n",
            static_cast<int>(res));
        return metrics;
    }

    // ── Convert ticks → nanoseconds ───────────────────────────────────────────
    const uint64_t t_begin = timestamps[0];
    const uint64_t t_end   = timestamps[1];

    if (t_end >= t_begin) {
        const uint64_t ticks = t_end - t_begin;
        // timestamp_period_ns_ is in ns/tick (float, device-specific).
        metrics.gpu_frame_time_ns = static_cast<uint64_t>(
            static_cast<double>(ticks) * static_cast<double>(timestamp_period_ns_));
    }

    return metrics;
}

} // namespace phyriad::render::vulkan

#endif // PHYRIAD_BUILD_VULKAN
// Made with my soul - Swately <3
