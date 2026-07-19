// observer/tests/process_observer_test.cpp
// Test: ProcessObserver pattern matching, refresh, expiry.
//
// Does NOT require any specific process to be running.
// Verifies:
//   - POD sizes.
//   - Pattern add/remove.
//   - MASS-router (2026-07-17): refresh() now TRACKS ALL TOUCHABLE processes,
//     decoupled from name patterns. Patterns only gate PLACEMENT
//     (is_placement_eligible), not tracking. Tests 4/5 updated from the old
//     pattern-gate contract ("nonexistent pattern → 0 targets") to the new one.
//   - Linear lookup correctness.
//

#include <phynned/observer/ProcessObserver.hpp>
#include <phynned/observer/TargetProcess.hpp>
#include <phynned/observer/TargetMetrics.hpp>
#include <phynned/policy/PolicyDecision.hpp>

#include <cassert>
#include <cstdio>

int main() {
    // ── Test 1: POD assertions ────────────────────────────────────────────
    {
        static_assert(sizeof(phynned::observer::TargetProcess) == 64u,
            "TargetProcess must be 64B");
        static_assert(sizeof(phynned::observer::TargetMetrics) == 128u,
            "TargetMetrics must be 128B");
        static_assert(sizeof(phynned::policy::PolicyDecision) == 32u,
            "PolicyDecision must be 32B");
        std::printf("[OK] POD sizes correct\n");
    }

    // ── Test 2: ProcessObserver construction ──────────────────────────────
    {
        phynned::observer::ProcessObserver obs;
        assert(obs.target_count() == 0u);
        std::printf("[OK] ProcessObserver default construction\n");
    }

    // ── Test 3: Pattern registration ──────────────────────────────────────
    {
        phynned::observer::ProcessObserver obs;
        obs.add_target_pattern("notepad");
        obs.add_target_pattern("obs64");
        obs.add_target_pattern("notepad");  // duplicate — should not double-add

        // We can't verify count directly without exposing internals,
        // but we can verify refresh() doesn't crash.
        obs.refresh();
        std::printf("[OK] Pattern registration and refresh\n");
    }

    // ── Test 4: Detection is track-all-touchable, NOT pattern-gated ───────
    // MASS-router: even with only a nonexistent pattern registered, refresh()
    // tracks the box's touchable processes (the old contract asserted 0 here).
    {
        phynned::observer::ProcessObserver obs;
        obs.add_target_pattern("__nonexistent_exe_xyz123__");
        obs.refresh();
        // A live box always has ≥1 touchable process; tracking is bounded.
        assert(obs.target_count() > 0u);
        assert(obs.target_count() <= phynned::observer::kMaxTargets);
        // A pattern the process does not match must NOT make it placement-eligible.
        assert(!obs.is_placement_eligible("some_random_untracked_name.exe"));
        std::printf("[OK] Track-all-touchable decoupled from patterns: %u tracked\n",
                    obs.target_count());
    }

    // ── Test 5: Clearing patterns does not stop tracking ──────────────────
    // Patterns now gate PLACEMENT only; clearing them leaves detection intact.
    {
        phynned::observer::ProcessObserver obs;
        obs.add_target_pattern("chrome");
        assert(obs.is_placement_eligible("chrome.exe"));  // pattern matches
        obs.clear_target_patterns();
        obs.refresh();
        assert(obs.target_count() > 0u);                  // still tracking touchable
        assert(!obs.is_placement_eligible("chrome.exe")); // no longer eligible
        std::printf("[OK] Detection survives pattern-clear; placement gate follows patterns\n");
    }

    // ── Test 6: Snapshot capacity ─────────────────────────────────────────
    {
        using phynned::observer::TargetProcess;
        TargetProcess buf[4];
        phynned::observer::ProcessObserver obs;
        obs.refresh();
        const uint32_t n = obs.snapshot(buf, 4u);
        assert(n <= 4u);
        std::printf("[OK] Snapshot capacity bounded: %u entries\n", n);
    }

    std::printf("\n[PASS] process_observer_test\n");
    return 0;
}
// Made with my soul - Swately <3
