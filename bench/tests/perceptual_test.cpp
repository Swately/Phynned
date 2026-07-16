// apps/ayama/bench/tests/perceptual_test.cpp
// Test: compute_perceptual() correctness.
//
// Verifies:
//   - Empty baseline → all-zero PerceptualReport.
//   - Single sample with known values → correct metric derivation.
//   - Stutter counting: samples where avg > 2× median are flagged.
//   - Hitch counting: samples where P99 > 50ms are flagged.
//   - smoothness_score: 100 when all avg frametimes are identical.
//
// All inputs synthetic — no real process or ETW needed.
//

#include <ayama/bench/PerceptualMetrics.hpp>
#include <ayama/bench/Baseline.hpp>

#include <cassert>
#include <cstdio>
#include <cmath>

static bool near(float a, float b, float eps = 0.5f) {
    return std::fabs(a - b) < eps;
}

int main() {
    using namespace ayama::bench;

    // ── Test 1: PerceptualReport POD ─────────────────────────────────────────
    {
        static_assert(std::is_trivially_copyable_v<PerceptualReport>);
        static_assert(std::is_standard_layout_v<PerceptualReport>);
        static_assert(sizeof(PerceptualReport) % 8u == 0u);
        std::printf("[OK] PerceptualReport is trivially copyable, 8B-aligned size\n");
    }

    // ── Test 2: Empty baseline → zero report ─────────────────────────────────
    {
        Baseline b;
        // never started — sample_count = 0
        const PerceptualReport r = compute_perceptual(b);
        assert(r.sample_count  == 0u);
        assert(r.stutter_count == 0u);
        assert(r.hitch_count   == 0u);
        assert(near(r.smoothness_score, 0.0f));
        std::printf("[OK] Empty baseline → zero report\n");
    }

    // ── Test 3: Single perfect sample (8.33ms avg ≈ 120 FPS) ─────────────────
    {
        Baseline b;
        b.start(1u);
        BaselineSample s{};
        s.target_pid            = 1u;
        s.frame_time_avg_ms     = 8.33f;
        s.frame_time_p99_ms     = 9.0f;
        s.frame_time_variance_ms = 0.1f;
        b.push_sample(s);
        b.stop();

        const PerceptualReport r = compute_perceptual(b);
        assert(r.sample_count == 1u);
        assert(r.target_pid   == 1u);
        assert(r.hitch_count  == 0u);    // P99 < 50ms
        assert(r.stutter_count == 0u);   // single sample, no stutter possible
        // median_fps ≈ 1000/8.33 ≈ 120
        assert(r.median_fps > 100.0f && r.median_fps < 140.0f);
        // smoothness: single sample → stddev = 0 → score = 100
        assert(near(r.smoothness_score, 100.0f, 1.0f));
        std::printf("[OK] Single perfect sample: median_fps=%.1f smooth=%.1f\n",
            r.median_fps, r.smoothness_score);
    }

    // ── Test 4: Hitch detection (P99 > 50ms) ─────────────────────────────────
    {
        Baseline b;
        b.start(2u);

        // Push 10 normal + 2 hitch samples
        for (int i = 0; i < 10; ++i) {
            BaselineSample s{};
            s.target_pid        = 2u;
            s.frame_time_avg_ms = 16.0f;
            s.frame_time_p99_ms = 18.0f;    // below 50ms — not a hitch
            b.push_sample(s);
        }
        {
            BaselineSample s{};
            s.target_pid        = 2u;
            s.frame_time_avg_ms = 60.0f;
            s.frame_time_p99_ms = 80.0f;    // above 50ms — hitch!
            b.push_sample(s);
        }
        {
            BaselineSample s{};
            s.target_pid        = 2u;
            s.frame_time_avg_ms = 55.0f;
            s.frame_time_p99_ms = 70.0f;    // above 50ms — hitch!
            b.push_sample(s);
        }
        b.stop();

        const PerceptualReport r = compute_perceptual(b);
        assert(r.sample_count == 12u);
        assert(r.hitch_count  == 2u);
        assert(near(r.hitching_index, 2.0f / 12.0f, 0.01f));
        std::printf("[OK] Hitch detection: %u hitches / %u samples  idx=%.3f\n",
            r.hitch_count, r.sample_count, r.hitching_index);
    }

    // ── Test 5: Stutter detection (avg > 2× median) ───────────────────────────
    {
        Baseline b;
        b.start(3u);

        // 8 normal frames at 16ms, 2 stutter frames at 50ms (> 2×16=32ms)
        for (int i = 0; i < 8; ++i) {
            BaselineSample s{};
            s.target_pid        = 3u;
            s.frame_time_avg_ms = 16.0f;
            s.frame_time_p99_ms = 18.0f;
            b.push_sample(s);
        }
        for (int i = 0; i < 2; ++i) {
            BaselineSample s{};
            s.target_pid        = 3u;
            s.frame_time_avg_ms = 50.0f;    // > 2 × 16 = 32ms → stutter
            s.frame_time_p99_ms = 55.0f;
            b.push_sample(s);
        }
        b.stop();

        const PerceptualReport r = compute_perceptual(b);
        // median avg is ~16ms, stutters are at 50ms (> 32ms threshold)
        assert(r.stutter_count >= 2u);
        std::printf("[OK] Stutter detection: %u stutters (expected ≥ 2)\n",
            r.stutter_count);
    }

    // ── Test 6: Perfect smoothness (all identical avg frametimes) ────────────
    {
        Baseline b;
        b.start(4u);
        for (int i = 0; i < 20; ++i) {
            BaselineSample s{};
            s.target_pid        = 4u;
            s.frame_time_avg_ms = 10.0f;    // exactly 100 FPS, perfectly steady
            s.frame_time_p99_ms = 11.0f;
            b.push_sample(s);
        }
        b.stop();

        const PerceptualReport r = compute_perceptual(b);
        // stddev = 0 → cv = 0 → smoothness = 100
        assert(near(r.smoothness_score, 100.0f, 1.0f));
        std::printf("[OK] Perfect smoothness: score=%.1f\n", r.smoothness_score);
    }

    // ── Test 7: fps_1pct_low ≤ median_fps ────────────────────────────────────
    {
        Baseline b;
        b.start(5u);
        for (int i = 0; i < 30; ++i) {
            BaselineSample s{};
            s.target_pid        = 5u;
            s.frame_time_avg_ms = 8.0f + static_cast<float>(i % 5) * 1.0f;
            s.frame_time_p99_ms = s.frame_time_avg_ms + 2.0f;
            b.push_sample(s);
        }
        b.stop();

        const PerceptualReport r = compute_perceptual(b);
        // 1% low FPS must be ≤ median FPS (worst frame is slower than typical)
        assert(r.fps_1pct_low <= r.median_fps + 0.1f);
        std::printf("[OK] fps_1pct_low(%.1f) ≤ median_fps(%.1f)\n",
            r.fps_1pct_low, r.median_fps);
    }

    std::printf("\n[PASS] perceptual_test\n");
    return 0;
}
// Made with my soul - Swately <3
