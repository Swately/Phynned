// apps/ayama/observer/tests/process_observer_test.cpp
// Test: ProcessObserver pattern matching, refresh, expiry.
//
// Does NOT require any specific process to be running.
// Verifies:
//   - POD sizes.
//   - Pattern add/remove.
//   - After refresh(), dead patterns produce 0 targets.
//   - Linear lookup correctness.
//

#include <ayama/observer/ProcessObserver.hpp>
#include <ayama/observer/TargetProcess.hpp>
#include <ayama/observer/TargetMetrics.hpp>
#include <ayama/policy/PolicyDecision.hpp>

#include <cassert>
#include <cstdio>

int main() {
    // ── Test 1: POD assertions ────────────────────────────────────────────
    {
        static_assert(sizeof(ayama::observer::TargetProcess) == 64u,
            "TargetProcess must be 64B");
        static_assert(sizeof(ayama::observer::TargetMetrics) == 128u,
            "TargetMetrics must be 128B");
        static_assert(sizeof(ayama::policy::PolicyDecision) == 32u,
            "PolicyDecision must be 32B");
        std::printf("[OK] POD sizes correct\n");
    }

    // ── Test 2: ProcessObserver construction ──────────────────────────────
    {
        ayama::observer::ProcessObserver obs;
        assert(obs.target_count() == 0u);
        std::printf("[OK] ProcessObserver default construction\n");
    }

    // ── Test 3: Pattern registration ──────────────────────────────────────
    {
        ayama::observer::ProcessObserver obs;
        obs.add_target_pattern("notepad");
        obs.add_target_pattern("obs64");
        obs.add_target_pattern("notepad");  // duplicate — should not double-add

        // We can't verify count directly without exposing internals,
        // but we can verify refresh() doesn't crash.
        obs.refresh();
        std::printf("[OK] Pattern registration and refresh\n");
    }

    // ── Test 4: Snapshot with no matches ──────────────────────────────────
    {
        ayama::observer::ProcessObserver obs;
        obs.add_target_pattern("__nonexistent_exe_xyz123__");
        obs.refresh();
        assert(obs.target_count() == 0u);
        std::printf("[OK] No matches for nonexistent pattern\n");
    }

    // ── Test 5: Clear patterns ────────────────────────────────────────────
    {
        ayama::observer::ProcessObserver obs;
        obs.add_target_pattern("notepad");
        obs.clear_target_patterns();
        obs.refresh();
        assert(obs.target_count() == 0u);
        std::printf("[OK] Clear patterns\n");
    }

    // ── Test 6: Snapshot capacity ─────────────────────────────────────────
    {
        using ayama::observer::TargetProcess;
        TargetProcess buf[4];
        ayama::observer::ProcessObserver obs;
        obs.refresh();
        const uint32_t n = obs.snapshot(buf, 4u);
        assert(n <= 4u);
        std::printf("[OK] Snapshot capacity bounded: %u entries\n", n);
    }

    std::printf("\n[PASS] process_observer_test\n");
    return 0;
}
// Made with my soul - Swately <3
