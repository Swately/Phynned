// framework/ui/include/phyriad/ui/types/RenderedFrame.hpp
// RenderedFrame — mensaje POD producido por RenderNode y consumido por
// FrameInterpolatorNode → PresentNode.
//
// Regla de oro (PHYRIAD_IMPLEMENTATION_STRATEGIES §8.4):
//   NUNCA pasar VkImage directamente en mensajes POD.
//   Siempre usar un índice en un pool pre-asignado (FramePool).
//   `handle_id` es el índice en FramePool::slot[] — el pool es el owner.
//
// Layout (40 bytes, alignas(8)):
//   [0..7]   handle_id      — índice de slot en FramePool (UINT32_MAX = inválido)
//   [8..11]  image_index    — índice del swapchain image
//   [12..15] frame_id       — contador monótonamente creciente
//   [16..23] cpu_time_ns    — tiempo de CPU del frame (nanosegundos)
//   [24..31] gpu_time_ns    — tiempo de GPU del frame (0 si telemetría no disponible)
//   [32..39] present_tsc    — TSC del momento del present
//   [40..40] is_interpolated — 1 si frame fue interpolado por FrameInterpolator
//   [41..43] _pad[3]
//   [44..47] _pad2
//   ← espera 40 bytes según la especificación
//
// Nota: el plan especifica sizeof == 40u.  Se usa un layout compacto con
//   reordenamiento de campos para evitar padding implícito.
//
#pragma once
#include <cstdint>
#include <type_traits>

namespace phyriad::ui {

// ─────────────────────────────────────────────────────────────────────────────
// RenderedFrame — 40 bytes, standard layout, trivially copyable.
// ─────────────────────────────────────────────────────────────────────────────
struct alignas(8) RenderedFrame {
    uint64_t cpu_time_ns   {0};        // [0..7]   — nanosegundos de CPU (frame)
    uint64_t gpu_time_ns   {0};        // [8..15]  — nanosegundos de GPU (0 si N/A)
    uint64_t present_tsc   {0};        // [16..23] — TSC en el momento de present
    uint32_t handle_id     {UINT32_MAX};// [24..27] — slot en FramePool; UINT32_MAX=inválido
    uint32_t image_index   {0};        // [28..31] — índice en swapchain
    uint32_t frame_id      {0};        // [32..35] — contador monótonamente creciente
    uint8_t  is_interpolated{0};       // [36]     — 1 si interpolado
    uint8_t  _pad[3]       {};         // [37..39]
};
static_assert(sizeof(RenderedFrame)  == 40u,
    "RenderedFrame must be 40 bytes");
static_assert(alignof(RenderedFrame) == 8u,
    "RenderedFrame must be 8-byte aligned");
static_assert(std::is_trivially_copyable_v<RenderedFrame>,
    "RenderedFrame must be trivially copyable");
static_assert(std::is_standard_layout_v<RenderedFrame>,
    "RenderedFrame must be standard layout");

} // namespace phyriad::ui
// Made with my soul - Swately <3
