// bench/include/phynned/bench/ABRunner.hpp
// ABRunner — orchestrates an automated A/B performance test.
//
// Designed for the tick-loop architecture of phynned-agent.
// The runner is a state machine: Idle → RecordingA → RecordingB → Done.
// The agent observes the phase and applies/reverts optimizations between A→B.
//
// Usage (tick-loop style):
//   runner.set_target(pid, "game.exe");
//   runner.set_phase_duration_ms(30000);   // 30 s per phase
//   runner.start_phase_a();
//   // each tick (100 ms):
//   if (runner.phase() == ABPhase::RecordingA) {
//       runner.push_metrics_a(metrics_for_pid, tsc);
//       if (runner.phase_a_complete()) {
//           executor.apply(policy);        // <-- agent applies optimisation
//           runner.start_phase_b();
//       }
//   } else if (runner.phase() == ABPhase::RecordingB) {
//       runner.push_metrics_b(metrics_for_pid, tsc);
//   }
//   if (runner.phase() == ABPhase::Done) {
//       DiffReport r = runner.generate_report();
//       executor.revert(pid);              // restore after test
//   }
//
// Threading: single-thread (agent main thread).
// Resource:  ~1 MB pre-allocated (two Baseline objects, 512 KB each).
//            DO NOT put ABRunner on the stack — use as a class member.
// Privilege: None (caller applies optimisations via ActionExecutor).
//
#pragma once

#include <phynned/bench/Baseline.hpp>
#include <phynned/bench/DiffReport.hpp>
#include <phynned/observer/TargetMetrics.hpp>
#include <phyriad/schema/Error.hpp>

#include <cstdint>
#include <expected>

namespace phynned::bench {

// ── Phase enum ──────────────────────────────────────────────────────────────
enum class ABPhase : uint8_t {
    Idle       = 0,   ///< Not running. Call start_phase_a() to begin.
    RecordingA = 1,   ///< Collecting baseline (no Phynned policies applied).
    RecordingB = 2,   ///< Collecting treated data (policies applied by agent).
    Done       = 3,   ///< Both phases complete; generate_report() is valid.
};

// ── ABRunner ─────────────────────────────────────────────────────────────────
class ABRunner {
public:
    ABRunner() noexcept = default;

    ABRunner(ABRunner const&)            = delete;
    ABRunner& operator=(ABRunner const&) = delete;

    // ── Configuration — set before start_phase_a() ──────────────────────
    /// Set the target process. `name` is copied (≤ 31 chars).
    void set_target(uint32_t pid, const char* name) noexcept;

    /// Set recording duration per phase in milliseconds (minimum 5 000 ms).
    void set_phase_duration_ms(uint32_t ms) noexcept;

    // ── Phase control ─────────────────────────────────────────────────────
    /// Begin phase A (baseline without optimisation).
    /// Error: InvalidArgument if phase != Idle.
    [[nodiscard]] std::expected<void, phyriad::Error>
    start_phase_a() noexcept;

    /// Begin phase B (treated with optimisation applied by caller).
    /// Error: InvalidArgument if phase != RecordingA.
    [[nodiscard]] std::expected<void, phyriad::Error>
    start_phase_b() noexcept;

    /// Reset to Idle, discarding any recorded data.
    void reset() noexcept;

    // ── Sample feed — call each tick for the active phase ────────────────
    void push_metrics_a(const observer::TargetMetrics& m, uint64_t tsc) noexcept;
    void push_metrics_b(const observer::TargetMetrics& m, uint64_t tsc) noexcept;

    // ── Status queries ────────────────────────────────────────────────────
    [[nodiscard]] ABPhase  phase()            const noexcept { return phase_; }
    [[nodiscard]] uint32_t target_pid()       const noexcept { return target_pid_; }
    [[nodiscard]] uint32_t phase_duration_ms() const noexcept { return phase_duration_ms_; }

    /// True when phase A has recorded enough data and time has elapsed.
    /// Agent should call start_phase_b() and apply optimisation after this.
    [[nodiscard]] bool phase_a_complete() const noexcept;

    /// True when phase B is done. phase() transitions to Done automatically.
    [[nodiscard]] bool phase_b_complete() const noexcept;

    /// Progress of the current phase: 0.0 (just started) → 1.0 (complete).
    /// Returns 1.0 if Done, 0.0 if Idle.
    [[nodiscard]] float progress() const noexcept;

    /// Sample count in each phase (for progress display).
    [[nodiscard]] uint32_t samples_a() const noexcept { return baseline_a_.sample_count(); }
    [[nodiscard]] uint32_t samples_b() const noexcept { return baseline_b_.sample_count(); }

    // ── Result — valid only when phase() == Done ──────────────────────────
    [[nodiscard]] DiffReport generate_report() const noexcept;

    /// Access underlying baselines for PerceptualMetrics computation.
    [[nodiscard]] const Baseline& baseline_a() const noexcept { return baseline_a_; }
    [[nodiscard]] const Baseline& baseline_b() const noexcept { return baseline_b_; }

private:
    ABPhase  phase_             {ABPhase::Idle};
    uint32_t target_pid_        {0u};
    uint32_t phase_duration_ms_ {30'000u};
    uint64_t phase_start_tsc_a_ {0u};
    uint64_t phase_start_tsc_b_ {0u};
    uint64_t tsc_freq_hz_       {0u};   ///< Cached from calibrate_tsc_freq().
    char     target_name_[32]   {};

    // ── Pre-allocated baselines (512 KB each) ─────────────────────────────
    // Placed last to keep the hot state near the top of the cache line.
    Baseline baseline_a_{};
    Baseline baseline_b_{};
};

} // namespace phynned::bench
// Made with my soul - Swately <3
