// framework/render/vulkan/include/phyriad/render/vulkan/GpuTelemetry.hpp
// GpuTelemetry — per-frame GPU timestamp query wrapper.
//
// Usage pattern (per frame):
//   GpuTelemetry tel;
//   tel.init(device, phys_device, queue_family);    // once at startup
//
//   // Inside the command buffer recording:
//   tel.begin_frame(cmd_buf);                        // top-of-pipe timestamp
//   // ... render commands ...
//   tel.end_frame(cmd_buf);                          // bottom-of-pipe timestamp
//
//   // After vkQueueSubmit + vkQueueWaitIdle (or appropriate fence):
//   phyriad::render::GpuMetrics m = tel.read_results(device, queue_submit_cpu_ns);
//
// The class is NOT thread-safe. Drive it from the render thread only.
//
// When PHYRIAD_BUILD_VULKAN is not defined all methods are no-ops and
// read_results() returns a zero-filled GpuMetrics.
//
#pragma once
#include <phyriad/render/GpuMetrics.hpp>
#include <cstdint>

#ifdef PHYRIAD_BUILD_VULKAN
#   include <vulkan/vulkan.h>
#endif

namespace phyriad::render::vulkan {

// ─────────────────────────────────────────────────────────────────────────────
// GpuTelemetry
// ─────────────────────────────────────────────────────────────────────────────
class GpuTelemetry {
public:
    // Number of timestamp queries per frame: begin + end = 2.
    static constexpr uint32_t kQueryCount = 2u;

    GpuTelemetry()  noexcept = default;
    ~GpuTelemetry() noexcept;

    GpuTelemetry(GpuTelemetry const&)            = delete;
    GpuTelemetry& operator=(GpuTelemetry const&) = delete;
    GpuTelemetry(GpuTelemetry&&)                 = delete;
    GpuTelemetry& operator=(GpuTelemetry&&)      = delete;

    // ── Lifecycle ─────────────────────────────────────────────────────────────

    /// Initialise the query pool.
    /// @param queue_family  Graphics/compute queue family that will record timestamps.
    /// @returns true on success; false if timestamps are not supported on this device.
#ifdef PHYRIAD_BUILD_VULKAN
    [[nodiscard]] bool init(VkDevice         device,
                            VkPhysicalDevice phys_device,
                            uint32_t         queue_family) noexcept;
    void shutdown(VkDevice device) noexcept;
#else
    [[nodiscard]] bool init(uint32_t /*queue_family*/) noexcept { return false; }
    void shutdown() noexcept {}
#endif

    // ── Per-frame recording ───────────────────────────────────────────────────

    /// Write a TOP_OF_PIPE timestamp into query slot 0.
    /// Call this at the beginning of your command buffer recording.
#ifdef PHYRIAD_BUILD_VULKAN
    void begin_frame(VkCommandBuffer cmd_buf) noexcept;
#else
    void begin_frame(void* /*cmd_buf*/) noexcept {}
#endif

    /// Write a BOTTOM_OF_PIPE timestamp into query slot 1.
    /// Call this at the end of your command buffer recording.
#ifdef PHYRIAD_BUILD_VULKAN
    void end_frame(VkCommandBuffer cmd_buf) noexcept;
#else
    void end_frame(void* /*cmd_buf*/) noexcept {}
#endif

    // ── Result readback ───────────────────────────────────────────────────────

    /// Read back the two timestamps and produce a GpuMetrics snapshot.
    /// @param queue_submit_cpu_ns  CPU clock in nanoseconds at vkQueueSubmit.
    ///
    /// Blocks if the GPU has not finished writing the queries yet.
    /// Call ONLY after the frame fence has been signalled (GPU is idle on the frame).
#ifdef PHYRIAD_BUILD_VULKAN
    [[nodiscard]] phyriad::render::GpuMetrics
    read_results(VkDevice device, uint64_t queue_submit_cpu_ns) noexcept;
#else
    [[nodiscard]] phyriad::render::GpuMetrics
    read_results(uint64_t /*queue_submit_cpu_ns*/) noexcept
    { return {}; }
#endif

    // ── State accessors ───────────────────────────────────────────────────────

    [[nodiscard]] bool initialized()  const noexcept { return initialized_; }
    [[nodiscard]] float timestamp_period_ns() const noexcept { return timestamp_period_ns_; }

private:
#ifdef PHYRIAD_BUILD_VULKAN
    VkQueryPool query_pool_{VK_NULL_HANDLE};
#endif
    float    timestamp_period_ns_{1.0f};  // device ticks → nanoseconds
    bool     initialized_       {false};
    bool     frame_open_        {false};  // begin_frame called, end_frame pending
};

} // namespace phyriad::render::vulkan
// Made with my soul - Swately <3
