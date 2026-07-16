// framework/ui/include/phyriad/ui/types/GpuMetrics.hpp
// Convenience alias: re-exports phyriad::render::GpuMetrics as phyriad::ui::GpuMetrics.
//
// The canonical definition lives in <phyriad/render/GpuMetrics.hpp> so that
// phyriad_render_vulkan (which produces GpuMetrics) can include it without
// depending on phyriad_ui and creating a circular link dependency.
//
// UI-layer consumers that prefer the phyriad::ui namespace can include this header.
// Both names refer to the same underlying type.
//
#pragma once
#include <phyriad/render/GpuMetrics.hpp>

namespace phyriad::ui {

// Type alias: phyriad::ui::GpuMetrics == phyriad::render::GpuMetrics.
using GpuMetrics = phyriad::render::GpuMetrics;

} // namespace phyriad::ui
// Made with my soul - Swately <3
