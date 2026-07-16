// apps/ayama/bench/include/ayama/bench/DiffReport.hpp
// DiffReport — result of an A/B benchmark run.
//
// IPC-safe: trivially_copyable, standard_layout, sizeof ≤ 4096.
// Produced by ABRunner::generate_report() after both phases complete.
//
// Verdict is driven primarily by P99 frametime improvement:
//   Improved   — P99 improved ≥ 5%
//   Marginal   — P99 improved 1.5%–5%
//   Neutral    — P99 within ±1.5%
//   Regression — P99 worsened ≥ 2%
//
#pragma once

#include <ayama/bench/Baseline.hpp>
#include <cstdint>
#include <type_traits>

namespace ayama::bench {

// ── Verdict classification ──────────────────────────────────────────────────
enum class Verdict : uint8_t {
    Neutral    = 0,   ///< No significant change.
    Improved   = 1,   ///< Significant improvement (P99 ≥ +5%).
    Marginal   = 2,   ///< Small improvement (P99 +1.5%–5%).
    Regression = 3,   ///< Performance worsened (P99 ≤ -2%).
};

// ── DiffReport — POD result struct ─────────────────────────────────────────
/// Full A/B test result. Carry-safe: memcpy-able, SHM-safe.
struct alignas(8) DiffReport {
    // ── Raw phase summaries ──────────────────────────────────────────────
    BaselineSummary phase_a;   ///< Without Ayama (baseline).   40B
    BaselineSummary phase_b;   ///< With Ayama applied.         40B

    // ── Delta fields — positive values mean improvement ──────────────────
    /// phase_a.frame_time_avg_ms - phase_b.frame_time_avg_ms
    float frame_time_avg_delta_ms;

    /// phase_a.frame_time_p99_ms - phase_b.frame_time_p99_ms
    float frame_time_p99_delta_ms;

    /// phase_a.frame_time_variance_avg - phase_b.frame_time_variance_avg
    float frame_time_variance_delta_ms;

    /// δP99 / phase_a.frame_time_p99_ms × 100  (positive = better)
    float frame_time_p99_improvement_pct;

    /// phase_a.total_migrations - phase_b.total_migrations  (positive = fewer migs)
    float migration_rate_delta;

    /// phase_a.avg_cpu_usage_pct - phase_b.avg_cpu_usage_pct
    float cpu_usage_delta_pct;

    // ── Verdict ──────────────────────────────────────────────────────────
    Verdict  verdict;          ///< Overall classification.
    uint8_t  _pad0[3];

    // ── Metadata ─────────────────────────────────────────────────────────
    uint32_t target_pid;       ///< PID of the measured process.
    uint32_t samples_a;        ///< Number of samples recorded in phase A.
    uint32_t samples_b;        ///< Number of samples recorded in phase B.
    char     target_name[32];  ///< Process name, null-terminated.
    char     verdict_text[64]; ///< Human-readable verdict string.

    // ── Padding to 8B-aligned total (224 B = 3.5 cache lines) ───────────────
    uint8_t  _pad1[8];
};

static_assert(std::is_trivially_copyable_v<DiffReport>,
    "DiffReport must be trivially copyable (IPC-safe)");
static_assert(std::is_standard_layout_v<DiffReport>,
    "DiffReport must be standard layout (IPC-safe)");
static_assert(sizeof(DiffReport) <= 4096u,
    "DiffReport must fit in one SHM page");

} // namespace ayama::bench
// Made with my soul - Swately <3
