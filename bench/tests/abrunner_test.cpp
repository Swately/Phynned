// bench/tests/abrunner_test.cpp
// Test: ABRunner phase state-machine, sample feed, report generation.
//
// All inputs are synthetic; no real process or ETW required.
//

#include <phynned/bench/ABRunner.hpp>
#include <phynned/bench/DiffReport.hpp>
#include <phynned/observer/TargetMetrics.hpp>

#include <cassert>
#include <cstdio>
#include <cstring>
#include <cmath>

static bool near(float a, float b, float eps = 0.1f) {
    return std::fabs(a - b) < eps;
}

static phynned::observer::TargetMetrics make_metrics(
    uint32_t pid,
    float avg_ms,
    float p99_ms,
    float var_ms,
    float cpu_pct = 25.0f)
{
    phynned::observer::TargetMetrics m{};
    m.pid                    = pid;
    m.frame_time_avg_ms      = avg_ms;
    m.frame_time_p99_ms      = p99_ms;
    m.frame_time_variance_ms = var_ms;
    m.cpu_usage_pct          = cpu_pct;
    m.migrations_per_sec     = 10u;
    m.involuntary_ctxsw_sec  = 5u;
    return m;
}

int main() {
    using namespace phynned::bench;
    using namespace phynned::observer;

    // ── Test 1: DiffReport POD ────────────────────────────────────────────────
    {
        static_assert(std::is_trivially_copyable_v<DiffReport>);
        static_assert(sizeof(DiffReport) <= 4096u);
        std::printf("[OK] DiffReport trivially copyable, size=%zu\n",
            sizeof(DiffReport));
    }

    // ── Test 2: Default construction → Idle ───────────────────────────────────
    {
        ABRunner r;
        assert(r.phase()      == ABPhase::Idle);
        assert(r.target_pid() == 0u);
        assert(r.samples_a()  == 0u);
        assert(r.samples_b()  == 0u);
        std::printf("[OK] Default construction is Idle\n");
    }

    // ── Test 3: start_phase_a() from wrong state → error ──────────────────────
    {
        ABRunner r;
        r.set_target(1u, "game.exe");
        // first start_phase_a must succeed
        auto res = r.start_phase_a();
        assert(res.has_value());
        assert(r.phase() == ABPhase::RecordingA);

        // second start_phase_a must fail (not Idle)
        auto res2 = r.start_phase_a();
        assert(!res2.has_value());
        std::printf("[OK] start_phase_a() from non-Idle → error\n");
    }

    // ── Test 4: start_phase_b() requires RecordingA ───────────────────────────
    {
        ABRunner r;
        r.set_target(2u, "test.exe");
        auto res = r.start_phase_b();
        assert(!res.has_value());   // must fail — not in RecordingA
        std::printf("[OK] start_phase_b() from Idle → error\n");
    }

    // ── Test 5: Full A→B phase transition with synthetic samples ─────────────
    {
        ABRunner r;
        r.set_target(100u, "game.exe");
        r.set_phase_duration_ms(5000u);   // 5 seconds — enough samples will gate

        auto res_a = r.start_phase_a();
        assert(res_a.has_value());
        assert(r.phase() == ABPhase::RecordingA);

        // Feed samples (we don't have real TSC so use synthetic).
        // phase_a_complete() also checks elapsed time (tsc_freq-based), so
        // we push enough samples to exceed the minimum count check.
        // We cannot bypass the time check without TSC, so we just feed and
        // verify the sample count grows.
        const uint64_t fake_tsc = 0ull;
        for (uint32_t i = 0u; i < 50u; ++i) {
            r.push_metrics_a(make_metrics(100u, 16.0f, 20.0f, 0.5f), fake_tsc);
        }
        assert(r.samples_a() == 50u);
        std::printf("[OK] Phase A: %u samples recorded\n", r.samples_a());

        // Manually advance to phase B by calling start_phase_b()
        // (bypasses time gate — valid because caller decides when to switch).
        auto res_b = r.start_phase_b();
        assert(res_b.has_value());
        assert(r.phase() == ABPhase::RecordingB);

        for (uint32_t i = 0u; i < 50u; ++i) {
            r.push_metrics_b(make_metrics(100u, 14.0f, 17.0f, 0.3f), fake_tsc);
        }
        assert(r.samples_b() == 50u);
        std::printf("[OK] Phase B: %u samples recorded\n", r.samples_b());

        // generate_report() while not Done should still not crash.
        // (ABRunner may return partial or empty report.)
        // We just verify it doesn't crash.
        (void)r.generate_report();
        std::printf("[OK] generate_report() while in RecordingB: no crash\n");
    }

    // ── Test 6: reset() returns to Idle ───────────────────────────────────────
    {
        ABRunner r;
        r.set_target(55u, "foo.exe");
        r.start_phase_a();
        r.push_metrics_a(make_metrics(55u, 10.0f, 12.0f, 0.2f), 0ull);

        r.reset();
        assert(r.phase()     == ABPhase::Idle);
        assert(r.samples_a() == 0u);
        assert(r.samples_b() == 0u);
        std::printf("[OK] reset() returns to Idle, clears samples\n");
    }

    // ── Test 7: progress() values ─────────────────────────────────────────────
    {
        ABRunner r;
        assert(near(r.progress(), 0.0f));   // Idle → 0.0

        r.set_target(10u, "prog.exe");
        r.set_phase_duration_ms(5000u);
        r.start_phase_a();
        // Immediately after starting: progress should be >= 0 and <= 1
        const float p = r.progress();
        assert(p >= 0.0f && p <= 1.0f);
        std::printf("[OK] progress() in [0,1]: %.3f\n", p);
    }

    // ── Test 8: baseline_a/b accessors ───────────────────────────────────────
    {
        ABRunner r;
        r.set_target(20u, "x.exe");
        r.start_phase_a();
        r.push_metrics_a(make_metrics(20u, 8.0f, 10.0f, 0.1f), 0ull);

        const Baseline& ba = r.baseline_a();
        assert(ba.sample_count() == 1u);
        assert(ba.target_pid()   == 20u);
        std::printf("[OK] baseline_a() accessor: 1 sample\n");
    }

    std::printf("\n[PASS] abrunner_test\n");
    return 0;
}
// Made with my soul - Swately <3
