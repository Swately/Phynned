// bench/src/PerceptualMetrics.cpp
// PerceptualMetrics — compute_perceptual() implementation.
//
// Algorithm:
//   1. Copy frame_time_avg_ms and frame_time_p99_ms from the Baseline samples
//      into file-static sort buffers (avoid heap, avoid stack overflow).
//   2. Sort each buffer independently.
//   3. Compute stutter count, hitch count, smoothness, and FPS percentiles
//      using the sorted buffers and a second pass over the raw samples.
//
// NOT reentrant (uses file-static buffers). Correctness requires single-thread
// access — guaranteed by the agent's tick-loop architecture.
//

#include <phynned/bench/PerceptualMetrics.hpp>
#include <algorithm>
#include <cmath>

namespace phynned::bench {

// ── File-static sort buffers (64 KB total, in .bss) ─────────────────────────
// Two arrays of Baseline::kMaxSamples floats. Used sequentially, not concurrently.
static float s_avg_sorted[Baseline::kMaxSamples];
static float s_p99_sorted[Baseline::kMaxSamples];

// ── compute_perceptual ───────────────────────────────────────────────────────
PerceptualReport compute_perceptual(const Baseline& b) noexcept
{
    PerceptualReport r{};

    const auto raw   = b.samples();
    const uint32_t n = static_cast<uint32_t>(raw.size());

    if (n == 0u) return r;

    r.sample_count = n;
    r.target_pid   = b.target_pid();

    // ── Pass 1: populate sort buffers + basic aggregates ──────────────────
    float    sum_avg  = 0.f;
    float    sum_sq   = 0.f;
    uint32_t hitch_count = 0u;

    for (uint32_t i = 0u; i < n; ++i) {
        const float avg = raw[i].frame_time_avg_ms;
        const float p99 = raw[i].frame_time_p99_ms;

        s_avg_sorted[i] = avg;
        s_p99_sorted[i] = p99;

        sum_avg += avg;

        // Hitch: any tick where the worst frame > 50 ms (< 20 FPS peak spike)
        if (p99 > 50.f) ++hitch_count;
    }

    const float mean_avg = sum_avg / static_cast<float>(n);

    // ── Pass 2: stddev + stutter count ────────────────────────────────────
    for (uint32_t i = 0u; i < n; ++i) {
        const float d = raw[i].frame_time_avg_ms - mean_avg;
        sum_sq += d * d;
    }
    const float stddev = (n > 1u)
        ? std::sqrt(sum_sq / static_cast<float>(n - 1u))
        : 0.f;

    // ── Sort buffers ──────────────────────────────────────────────────────
    std::sort(s_avg_sorted, s_avg_sorted + n);
    std::sort(s_p99_sorted, s_p99_sorted + n);

    // ── Median avg frametime ──────────────────────────────────────────────
    const float median_avg = s_avg_sorted[n / 2u];

    // ── Stutter count: ticks where avg frametime > 2× median ──────────────
    uint32_t stutter_count = 0u;
    if (median_avg > 0.f) {
        const float threshold = 2.f * median_avg;
        for (uint32_t i = 0u; i < n; ++i) {
            if (raw[i].frame_time_avg_ms > threshold) ++stutter_count;
        }
    }

    // ── Hitching index ────────────────────────────────────────────────────
    r.hitch_count    = hitch_count;
    r.stutter_count  = stutter_count;
    r.hitching_index = static_cast<float>(hitch_count) / static_cast<float>(n);

    // ── Smoothness score: 100 × (1 – stddev/mean) clamped [0, 100] ────────
    const float cv = (mean_avg > 0.f) ? stddev / mean_avg : 1.f;
    r.smoothness_score = std::max(0.f, std::min(100.f, 100.f * (1.f - cv)));

    // ── FPS percentiles ───────────────────────────────────────────────────
    // s_p99_sorted is sorted ascending. The 99th percentile is near the top.
    // idx_99  ≈ position of the 99th percentile value (worst 1% of P99 windows).
    // idx_999 = n-1 = absolute maximum (worst single window = ≈ 0.1% low).
    const uint32_t idx_99  = (n >= 100u) ? (n * 99u / 100u) : (n - 1u);
    const uint32_t idx_999 = n - 1u;

    const float p99_of_p99  = s_p99_sorted[idx_99];
    const float p999_of_p99 = s_p99_sorted[idx_999];   // = max P99

    r.fps_1pct_low  = (p99_of_p99  > 0.f) ? 1000.f / p99_of_p99  : 0.f;
    r.fps_01pct_low = (p999_of_p99 > 0.f) ? 1000.f / p999_of_p99 : 0.f;
    r.median_fps    = (median_avg  > 0.f) ? 1000.f / median_avg  : 0.f;

    return r;
}

} // namespace phynned::bench
// Made with my soul - Swately <3
