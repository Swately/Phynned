// bench/src/ABRunner.cpp
// ABRunner — A/B test runner implementation.
//

#include <phynned/bench/ABRunner.hpp>
#include <phyriad/hal/Timestamp.hpp>

#include <algorithm>
#include <cstring>

namespace phynned::bench {

// ── Configuration ────────────────────────────────────────────────────────────

void ABRunner::set_target(uint32_t pid, const char* name) noexcept
{
    target_pid_ = pid;
    if (name) {
        std::strncpy(target_name_, name, sizeof(target_name_) - 1u);
        target_name_[sizeof(target_name_) - 1u] = '\0';
    } else {
        target_name_[0] = '\0';
    }
}

void ABRunner::set_phase_duration_ms(uint32_t ms) noexcept
{
    // Enforce a sane minimum of 5 s so we get enough samples.
    phase_duration_ms_ = (ms < 5'000u) ? 5'000u : ms;
}

// ── Phase control ─────────────────────────────────────────────────────────────

std::expected<void, phyriad::Error> ABRunner::start_phase_a() noexcept
{
    if (phase_ != ABPhase::Idle) {
        return std::unexpected(phyriad::Error{phyriad::ErrorCode::InvalidArgument, 0u, 0u});
    }
    // Calibrate TSC once; result is cached inside calibrate_tsc_freq().
    tsc_freq_hz_ = phyriad::hal::calibrate_tsc_freq();

    baseline_a_.start(target_pid_, phase_duration_ms_);
    phase_start_tsc_a_ = phyriad::hal::rdtsc();
    phase_ = ABPhase::RecordingA;
    return {};
}

std::expected<void, phyriad::Error> ABRunner::start_phase_b() noexcept
{
    if (phase_ != ABPhase::RecordingA) {
        return std::unexpected(phyriad::Error{phyriad::ErrorCode::InvalidArgument, 0u, 0u});
    }
    baseline_a_.stop();
    baseline_b_.start(target_pid_, phase_duration_ms_);
    phase_start_tsc_b_ = phyriad::hal::rdtsc();
    phase_ = ABPhase::RecordingB;
    return {};
}

void ABRunner::reset() noexcept
{
    phase_             = ABPhase::Idle;
    target_pid_        = 0u;
    phase_start_tsc_a_ = 0u;
    phase_start_tsc_b_ = 0u;
    tsc_freq_hz_       = 0u;
    target_name_[0]    = '\0';
    // Use reset() (not stop()) so sample buffers are zeroed — stop() only
    // clears the recording flag and preserves samples for post-run analysis.
    baseline_a_.reset();
    baseline_b_.reset();
}

// ── Sample feed ───────────────────────────────────────────────────────────────

void ABRunner::push_metrics_a(const observer::TargetMetrics& m,
                               uint64_t tsc) noexcept
{
    if (phase_ != ABPhase::RecordingA) return;
    baseline_a_.push_metrics(m, tsc);
    // Auto-detect phase completion (in case agent doesn't poll phase_a_complete()).
    // Do NOT auto-transition: the agent must call start_phase_b() so it can
    // apply the optimisation in between.
}

void ABRunner::push_metrics_b(const observer::TargetMetrics& m,
                               uint64_t tsc) noexcept
{
    if (phase_ != ABPhase::RecordingB) return;
    baseline_b_.push_metrics(m, tsc);
    if (phase_b_complete()) {
        baseline_b_.stop();
        phase_ = ABPhase::Done;
    }
}

// ── Status queries ────────────────────────────────────────────────────────────

bool ABRunner::phase_a_complete() const noexcept
{
    if (phase_ != ABPhase::RecordingA) return false;
    if (tsc_freq_hz_ == 0u)           return false;
    const uint64_t ticks_per_ms = tsc_freq_hz_ / 1'000u;
    if (ticks_per_ms == 0u)           return false;
    const uint64_t elapsed_ms =
        (phyriad::hal::rdtsc() - phase_start_tsc_a_) / ticks_per_ms;
    return elapsed_ms >= phase_duration_ms_
        || baseline_a_.sample_count() >= Baseline::kMaxSamples;
}

bool ABRunner::phase_b_complete() const noexcept
{
    if (phase_ != ABPhase::RecordingB) return false;
    if (tsc_freq_hz_ == 0u)            return false;
    const uint64_t ticks_per_ms = tsc_freq_hz_ / 1'000u;
    if (ticks_per_ms == 0u)            return false;
    const uint64_t elapsed_ms =
        (phyriad::hal::rdtsc() - phase_start_tsc_b_) / ticks_per_ms;
    return elapsed_ms >= phase_duration_ms_
        || baseline_b_.sample_count() >= Baseline::kMaxSamples;
}

float ABRunner::progress() const noexcept
{
    if (tsc_freq_hz_ == 0u) return 0.f;
    const uint64_t ticks_per_ms = tsc_freq_hz_ / 1'000u;
    if (ticks_per_ms == 0u) return 0.f;
    const uint64_t dur_tsc =
        static_cast<uint64_t>(phase_duration_ms_) * ticks_per_ms;
    if (dur_tsc == 0u) return 0.f;

    if (phase_ == ABPhase::RecordingA) {
        const uint64_t elapsed = phyriad::hal::rdtsc() - phase_start_tsc_a_;
        return std::min(1.f,
            static_cast<float>(elapsed) / static_cast<float>(dur_tsc));
    }
    if (phase_ == ABPhase::RecordingB) {
        const uint64_t elapsed = phyriad::hal::rdtsc() - phase_start_tsc_b_;
        return std::min(1.f,
            static_cast<float>(elapsed) / static_cast<float>(dur_tsc));
    }
    return (phase_ == ABPhase::Done) ? 1.f : 0.f;
}

// ── Report generation ─────────────────────────────────────────────────────────

DiffReport ABRunner::generate_report() const noexcept
{
    DiffReport r{};
    r.target_pid = target_pid_;
    r.samples_a  = baseline_a_.sample_count();
    r.samples_b  = baseline_b_.sample_count();
    std::strncpy(r.target_name, target_name_, sizeof(r.target_name) - 1u);
    r.target_name[sizeof(r.target_name) - 1u] = '\0';

    r.phase_a = baseline_a_.summary();
    r.phase_b = baseline_b_.summary();

    // ── Deltas (positive = improvement, "lower is better" metrics) ──────────
    r.frame_time_avg_delta_ms      =
        r.phase_a.frame_time_avg_ms    - r.phase_b.frame_time_avg_ms;
    r.frame_time_p99_delta_ms      =
        r.phase_a.frame_time_p99_ms    - r.phase_b.frame_time_p99_ms;
    r.frame_time_variance_delta_ms =
        r.phase_a.frame_time_variance_avg - r.phase_b.frame_time_variance_avg;
    r.migration_rate_delta =
        static_cast<float>(r.phase_a.total_migrations)
        - static_cast<float>(r.phase_b.total_migrations);
    r.cpu_usage_delta_pct  =
        r.phase_a.avg_cpu_usage_pct - r.phase_b.avg_cpu_usage_pct;

    // % improvement on P99 — primary verdict driver
    if (r.phase_a.frame_time_p99_ms > 0.f) {
        r.frame_time_p99_improvement_pct =
            (r.frame_time_p99_delta_ms / r.phase_a.frame_time_p99_ms) * 100.f;
    }

    // ── Verdict ──────────────────────────────────────────────────────────────
    // Thresholds chosen to match perceptual noticeability (§5.1).
    constexpr float kSignificantPct =  5.0f;  // ≥ 5% P99 improvement
    constexpr float kMarginalPct    =  1.5f;  // ≥ 1.5% improvement
    constexpr float kRegressionPct  = -2.0f;  // ≤ -2% (worsened)

    if (r.samples_a == 0u || r.samples_b == 0u) {
        r.verdict = Verdict::Neutral;
        std::strncpy(r.verdict_text, "Insufficient data", sizeof(r.verdict_text) - 1u);
    } else if (r.frame_time_p99_improvement_pct >= kSignificantPct) {
        r.verdict = Verdict::Improved;
        std::strncpy(r.verdict_text, "Significant improvement", sizeof(r.verdict_text) - 1u);
    } else if (r.frame_time_p99_improvement_pct >= kMarginalPct) {
        r.verdict = Verdict::Marginal;
        std::strncpy(r.verdict_text, "Marginal improvement", sizeof(r.verdict_text) - 1u);
    } else if (r.frame_time_p99_improvement_pct <= kRegressionPct) {
        r.verdict = Verdict::Regression;
        std::strncpy(r.verdict_text, "Regression detected", sizeof(r.verdict_text) - 1u);
    } else {
        r.verdict = Verdict::Neutral;
        std::strncpy(r.verdict_text, "No significant change", sizeof(r.verdict_text) - 1u);
    }
    r.verdict_text[sizeof(r.verdict_text) - 1u] = '\0';

    return r;
}

} // namespace phynned::bench
// Made with my soul - Swately <3
