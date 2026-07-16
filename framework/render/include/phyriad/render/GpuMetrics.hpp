// framework/render/include/phyriad/render/GpuMetrics.hpp
// GpuMetrics — POD snapshot of per-frame GPU queue telemetry.
//
// Produced by phyriad::render::vulkan::GpuTelemetry and stored in AppState for
// the UIThreadNode to display real GPU timing information.
//
// This header lives in the render pillar (not ui/) so that phyriad_render_vulkan
// can include it without a circular dependency on phyriad_ui.
// The ui/types alias header re-exports this type as phyriad::ui::GpuMetrics for
// consumers that prefer the UI namespace.
//
// Layout (32 bytes, alignas(8)):
//   [0..7]   gpu_frame_time_ns          — GPU frame duration (begin→end timestamp)
//   [8..15]  queue_submit_ns            — CPU monotonic ns at vkQueueSubmit
//   [16..19] pending_queue_depth        — inflight command buffers in flight
//   [20..23] async_compute_overlap_pct  — async compute overlap coverage (0–100)
//   [24]     vrr_active                 — 1 when VRR/G-Sync/FreeSync is active
//   [25..31] _pad
//

#pragma once
#include <cstdint>
#include <type_traits>

namespace phyriad::render {

// ─────────────────────────────────────────────────────────────────────────────
// GpuMetrics
// ─────────────────────────────────────────────────────────────────────────────
struct alignas(8) GpuMetrics {
    uint64_t gpu_frame_time_ns        {0};   // [0..7]   GPU begin→end TSC delta (ns)
    uint64_t queue_submit_ns          {0};   // [8..15]  CPU clock at vkQueueSubmit (ns)
    uint32_t pending_queue_depth      {0};   // [16..19] command buffers in flight
    uint32_t async_compute_overlap_pct{0};   // [20..23] async compute coverage (0–100)
    uint8_t  vrr_active               {0};   // [24]     VRR/G-Sync/FreeSync active
    uint8_t  _pad[7]                  {};    // [25..31]
};

static_assert(sizeof(GpuMetrics)  == 32u,
    "GpuMetrics must be 32 bytes");
static_assert(alignof(GpuMetrics) == 8u,
    "GpuMetrics must be 8-byte aligned");
static_assert(std::is_trivially_copyable_v<GpuMetrics>,
    "GpuMetrics must be trivially copyable");
static_assert(std::is_standard_layout_v<GpuMetrics>,
    "GpuMetrics must be standard layout");

} // namespace phyriad::render
// Made with my soul - Swately <3
