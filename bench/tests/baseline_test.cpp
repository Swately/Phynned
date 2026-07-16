// apps/ayama/bench/tests/baseline_test.cpp
// Test: Baseline push_sample, summary statistics, capacity enforcement.
//
// Does NOT require admin or a running process. All inputs are synthetic.
//

#include <ayama/bench/Baseline.hpp>
#include <ayama/observer/TargetMetrics.hpp>

#include <cassert>
#include <cstdio>
#include <cmath>

static bool near(float a, float b, float eps = 0.01f) {
    return std::fabs(a - b) < eps;
}

int main() {
    using namespace ayama::bench;
    using namespace ayama::observer;

    // ── Test 1: POD sizes ────────────────────────────────────────────────────
    {
        static_assert(sizeof(BaselineSample) == 64u,
            "BaselineSample must be 64B (one cache line)");
        std::printf("[OK] BaselineSample is 64B\n");
    }

    // ── Test 2: Default construction ──────────────────────────────────────────
    {
        Baseline b;
        assert(!b.recording());
        assert(b.sample_count() == 0u);
        assert(b.target_pid()   == 0u);
        std::printf("[OK] Default construction — not recording, zero samples\n");
    }

    // ── Test 3: Start / stop ──────────────────────────────────────────────────
    {
        Baseline b;
        b.start(42u);
        assert(b.recording());
        assert(b.target_pid() == 42u);
        assert(b.sample_count() == 0u);

        b.stop();
        assert(!b.recording());
        std::printf("[OK] start() / stop() lifecycle\n");
    }

    // ── Test 4: push_sample while not recording is ignored ────────────────────
    {
        Baseline b;
        // not started — push_sample must be a no-op
        BaselineSample s{};
        s.target_pid         = 1u;
        s.frame_time_avg_ms  = 16.0f;
        b.push_sample(s);
        assert(b.sample_count() == 0u);
        std::printf("[OK] push_sample ignored when not recording\n");
    }

    // ── Test 5: push_sample and summary correctness ───────────────────────────
    {
        Baseline b;
        b.start(99u);

        // Push 4 samples with known values.
        for (uint32_t i = 1u; i <= 4u; ++i) {
            BaselineSample s{};
            s.target_pid             = 99u;
            s.frame_time_avg_ms      = static_cast<float>(i) * 4.0f;  // 4,8,12,16
            s.frame_time_p99_ms      = static_cast<float>(i) * 5.0f;  // 5,10,15,20
            s.frame_time_variance_ms = static_cast<float>(i) * 1.0f;  // 1,2,3,4
            s.cpu_usage_pct          = 20.0f;
            s.migrations_per_sec     = i * 10u;
            s.involuntary_ctxsw_sec  = i * 5u;
            b.push_sample(s);
        }

        assert(b.sample_count() == 4u);

        b.stop();
        const BaselineSummary sum = b.summary();

        // avg of {4,8,12,16} = 10.0
        assert(near(sum.frame_time_avg_ms, 10.0f));
        assert(sum.sample_count == 4u);
        assert(sum.total_migrations == (10u + 20u + 30u + 40u));
        assert(sum.total_involuntary_ctxsw == (5u + 10u + 15u + 20u));
        assert(near(sum.avg_cpu_usage_pct, 20.0f));

        std::printf("[OK] push_sample + summary: avg=%.2f migrations=%u\n",
            sum.frame_time_avg_ms, sum.total_migrations);
    }

    // ── Test 6: push_metrics convenience wrapper ──────────────────────────────
    {
        Baseline b;
        b.start(77u);

        TargetMetrics m{};
        m.pid                    = 77u;
        m.frame_time_avg_ms      = 8.33f;  // ~120 FPS
        m.frame_time_p99_ms      = 12.0f;
        m.frame_time_variance_ms = 0.5f;
        m.cpu_usage_pct          = 30.0f;
        m.migrations_per_sec     = 5u;
        m.involuntary_ctxsw_sec  = 2u;

        b.push_metrics(m, 1'000'000'000ull);
        assert(b.sample_count() == 1u);

        b.stop();
        const BaselineSummary sum = b.summary();
        assert(near(sum.frame_time_avg_ms, 8.33f, 0.1f));
        std::printf("[OK] push_metrics wrapper: avg=%.2f\n", sum.frame_time_avg_ms);
    }

    // ── Test 7: Capacity cap (kMaxSamples) ────────────────────────────────────
    {
        Baseline b;
        b.start(1u);

        // Push exactly kMaxSamples + 10 — only kMaxSamples should be recorded.
        BaselineSample s{};
        s.target_pid = 1u;
        s.frame_time_avg_ms = 16.0f;
        for (uint32_t i = 0u; i < Baseline::kMaxSamples + 10u; ++i) {
            b.push_sample(s);
        }

        assert(b.sample_count() == Baseline::kMaxSamples);
        std::printf("[OK] Capacity cap enforced at %u samples\n", Baseline::kMaxSamples);
    }

    // ── Test 8: samples() span ────────────────────────────────────────────────
    {
        Baseline b;
        b.start(5u);
        BaselineSample s{};
        s.target_pid         = 5u;
        s.frame_time_avg_ms  = 33.3f;
        b.push_sample(s);
        b.stop();

        const auto sp = b.samples();
        assert(sp.size() == 1u);
        assert(near(sp[0].frame_time_avg_ms, 33.3f));
        std::printf("[OK] samples() span: %zu entries\n", sp.size());
    }

    std::printf("\n[PASS] baseline_test\n");
    return 0;
}
// Made with my soul - Swately <3
