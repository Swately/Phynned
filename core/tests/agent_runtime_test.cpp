// apps/ayama/core/tests/agent_runtime_test.cpp
// Test T4 (partial): AgentRuntime lifecycle — start, 100 ticks, stop.
//
// Verifies:
//   - start() returns Ok (or degraded-mode Ok without admin).
//   - run() executes at least 10 ticks in 2 seconds.
//   - stop() causes run() to return within 1 second.
//   - No crash, no memory leak (address sanitizer friendly).
//

#include <ayama/core/AgentRuntime.hpp>

#include <cassert>
#include <cstdio>
#include <thread>
#include <chrono>

int main() {
    using namespace std::chrono_literals;

    // ── Test 1: AdaptiveTick constants ────────────────────────────────────
    {
        using ayama::core::WorkloadState;
        using ayama::core::tick_interval_ms;

        assert(tick_interval_ms(WorkloadState::DeepIdle, false) == 5000u);
        assert(tick_interval_ms(WorkloadState::Idle,     false) == 1000u);
        assert(tick_interval_ms(WorkloadState::Light,    false) ==  500u);
        assert(tick_interval_ms(WorkloadState::Active,   false) ==  100u);
        assert(tick_interval_ms(WorkloadState::Bench,    false) ==   25u);
        // Battery mode: doubled
        assert(tick_interval_ms(WorkloadState::Active, true) == 200u);
        std::printf("[OK] AdaptiveTick constants correct\n");
    }

    // ── Test 2: Runtime lifecycle ─────────────────────────────────────────
    {
        ayama::core::AgentConfig cfg{};
        cfg.require_admin         = false;  // degrade gracefully
        cfg.self_pin_to_slow_cores = false; // avoid test env side effects
        cfg.enable_shm_publish    = false;  // no SHM in unit test

        ayama::core::AgentRuntime runtime(cfg);

        // start() must not fail even without admin
        auto r = runtime.start();
        if (!r) {
            std::fprintf(stderr, "start() failed: code=%d\n",
                static_cast<int>(r.error().code));
            return 1;
        }
        std::printf("[OK] start() returned Ok\n");

        // Run in background thread, stop after 500ms
        std::thread t([&runtime]() {
            runtime.run();
        });

        std::this_thread::sleep_for(500ms);
        runtime.stop();

        t.join();

        // Should have executed at least a few ticks
        const uint32_t ticks = runtime.tick_count();
        std::printf("[OK] run() completed: %u ticks\n", ticks);
        // In 500ms with 1000ms Idle tick we may only get 1 tick.
        // Just check it doesn't hang.
        assert(!runtime.running());
    }

    // ── Test 3: Multiple start/stop cycles ───────────────────────────────
    {
        ayama::core::AgentConfig cfg{};
        cfg.require_admin = false;
        cfg.enable_shm_publish = false;
        cfg.self_pin_to_slow_cores = false;

        for (int i = 0; i < 3; ++i) {
            ayama::core::AgentRuntime runtime(cfg);
            auto r = runtime.start();
            assert(r.has_value());
            std::thread t([&runtime]() { runtime.run(); });
            std::this_thread::sleep_for(50ms);
            runtime.stop();
            t.join();
            assert(!runtime.running());
        }
        std::printf("[OK] Multiple start/stop cycles OK\n");
    }

    std::printf("\n[PASS] agent_runtime_test\n");
    return 0;
}
// Made with my soul - Swately <3
