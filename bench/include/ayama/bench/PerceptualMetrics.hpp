// apps/ayama/bench/include/ayama/bench/PerceptualMetrics.hpp
// PerceptualMetrics — human-perceptual performance scores from Baseline data.
//
// Converts raw frame timing samples into metrics that reflect what a player
// actually perceives, as opposed to averages that can obscure stutters.
//
// Metrics computed:
//   stutter_count   — tick windows where avg frametime > 2× median avg frametime.
//                     A stutter is a frame that takes more than twice the typical
//                     frame duration; these are clearly visible to the human eye.
//
//   hitch_count     — tick windows where P99 frametime > 50 ms (< 20 FPS worst
//                     frame). Hitches are full freezes — more severe than stutters.
//
//   hitching_index  — hitch_count / sample_count  (0.0 = never, 1.0 = always).
//
//   smoothness_score — 100 × (1 – cv), where cv = stddev/mean of per-tick avg
//                     frametimes. Clamped [0, 100]. 100 = perfectly steady,
//                     0 = wildly variable.
//
//   fps_1pct_low    — 1000 / P99(per-tick P99 frametime). Equivalent to the
//                     "1% low" metric popularized by CapFrameX. Lower is worse.
//
//   fps_01pct_low   — 1000 / max(per-tick P99 frametime). Corresponds to the
//                     absolute worst measured frame cluster. Lower is worse.
//
//   median_fps      — 1000 / median(per-tick avg frametime). Stable FPS estimate
//                     resistant to brief spikes.
//
// Usage:
//   PerceptualReport pr = compute_perceptual(runner.baseline_a());
//
// Threading: not thread-safe; call from a single thread after recording ends.
// Resource:  uses two file-static float[kMaxSamples] buffers for sorting (64 KB).
//            NOT reentrant.
//
#pragma once

#include <ayama/bench/Baseline.hpp>
#include <cstdint>
#include <type_traits>

namespace ayama::bench {

// ── PerceptualReport — IPC-safe result ──────────────────────────────────────
struct alignas(8) PerceptualReport {
    // ── Stutter / hitch ───────────────────────────────────────────────────
    uint32_t stutter_count;     ///< Tick windows with avg frametime > 2× median.
    uint32_t hitch_count;       ///< Tick windows with P99 frametime > 50 ms.
    float    hitching_index;    ///< hitch_count / sample_count  [0, 1].

    // ── Smoothness ────────────────────────────────────────────────────────
    float    smoothness_score;  ///< 0–100. Higher = smoother. 100 = perfect.

    // ── FPS percentiles ───────────────────────────────────────────────────
    float    fps_1pct_low;      ///< FPS at 1% low  (1000 / P99 of P99s).
    float    fps_01pct_low;     ///< FPS at 0.1% low (1000 / max P99).
    float    median_fps;        ///< FPS at median  (1000 / median avg frametime).

    // ── Meta ──────────────────────────────────────────────────────────────
    uint32_t sample_count;      ///< Number of tick samples analysed.
    uint32_t target_pid;        ///< PID of the process this report covers.
    uint8_t  _pad[4];
};

static_assert(sizeof(PerceptualReport) % 8u == 0u,
    "PerceptualReport must be 8-byte aligned in size");
static_assert(std::is_trivially_copyable_v<PerceptualReport>,
    "PerceptualReport must be trivially copyable");
static_assert(std::is_standard_layout_v<PerceptualReport>,
    "PerceptualReport must be standard layout");

// ── compute_perceptual() ─────────────────────────────────────────────────────
/// Compute perceptual metrics from a completed Baseline recording.
/// If `b` has zero samples all fields are zero-initialised.
[[nodiscard]] PerceptualReport
compute_perceptual(const Baseline& b) noexcept;

} // namespace ayama::bench
// Made with my soul - Swately <3
