// framework/render/include/phyriad/render/RenderStats.hpp
// phyriad::render render statistics message — 64 bytes, alignof=8.
//
// Published by RenderNode at end of each frame. Allows AppLogicNode to track
// performance and adapt rendering quality (LOD, batch size, etc.).
//
// Layout (64 bytes total):
//   [0]  frame_id       : uint64_t — monotonically increasing frame counter
//   [8]  present_tsc    : uint64_t — __rdtsc() just before Present/SwapBuffers
//   [16] cpu_time_ns    : uint64_t — time from NewFrame to end of draw calls (ns)
//   [24] gpu_time_ns    : uint64_t — GPU frame time (0 if query not available)
//   [32] draw_calls     : uint32_t — ImGui draw calls in the frame
//   [36] vertices       : uint32_t — total ImGui vertices submitted
//   [40] indices        : uint32_t — total ImGui indices submitted
//   [44] fps_ema        : float    — exponential moving average of FPS
//   [48] frame_time_ms  : float    — last frame wall-clock time in milliseconds
//   [52] dropped_frames : uint32_t — frames where vsync deadline was missed
//   [56] _pad[8]        : uint8_t  — reserved for future fields
//

#pragma once
#include <phyriad/schema/PodMessage.hpp>
#include <cstdint>

namespace phyriad::render {

struct RenderStats {
    uint64_t frame_id;         // offset 0  — monotone frame counter
    uint64_t present_tsc;      // offset 8  — __rdtsc at Present/SwapBuffers
    uint64_t cpu_time_ns;      // offset 16 — CPU draw time (ns)
    uint64_t gpu_time_ns;      // offset 24 — GPU frame time (0 if unavailable)
    uint32_t draw_calls;       // offset 32 — ImGui draw calls
    uint32_t vertices;         // offset 36 — ImGui total vertices
    uint32_t indices;          // offset 40 — ImGui total indices
    float    fps_ema;          // offset 44 — EMA of frames-per-second
    float    frame_time_ms;    // offset 48 — last frame wall time (ms)
    uint32_t dropped_frames;   // offset 52 — missed vsync deadlines
    uint8_t  _pad[8];          // offset 56 — reserved
    // total = 64 bytes
};

static_assert(sizeof(RenderStats)  == 64u,  "RenderStats must be exactly 64 bytes");
static_assert(alignof(RenderStats) == 8u,   "RenderStats must be alignof 8");
static_assert(schema::PodMessage<RenderStats>, "RenderStats must satisfy PodMessage");

} // namespace phyriad::render
// Made with my soul - Swately <3
