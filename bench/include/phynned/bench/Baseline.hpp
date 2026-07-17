// bench/include/phynned/bench/Baseline.hpp
// Baseline — record and aggregate performance samples for A/B testing.
//
// Samples are pushed by the agent tick loop and aggregated on demand.
// Pre-allocates kMaxSamples × 64B = 512 KB in the constructor.
//
// Threading: single-thread (agent main thread).
// Resource:  512 KB pre-allocated; no heap after construction.
// Privilege: None.
//
#pragma once

#include <phynned/observer/TargetMetrics.hpp>
#include <array>
#include <cstdint>
#include <cstring>
#include <span>

namespace phynned::bench {

/// One recorded sample — 64 bytes (1 cache line).
struct alignas(64) BaselineSample {
    uint32_t target_pid;              //  4B
    uint32_t _pad0;                   //  4B
    uint64_t sample_tsc;              //  8B
    float    frame_time_avg_ms;       //  4B
    float    frame_time_p99_ms;       //  4B
    float    frame_time_variance_ms;  //  4B
    float    cpu_usage_pct;           //  4B
    uint32_t migrations_per_sec;      //  4B
    uint32_t involuntary_ctxsw_sec;   //  4B
    uint8_t  _pad1[24];               // 24B
};
static_assert(sizeof(BaselineSample) == 64);

/// Aggregate statistics over a recording window.
struct BaselineSummary {
    uint32_t sample_count;
    uint32_t _pad;
    float    frame_time_avg_ms;
    float    frame_time_p99_ms;
    float    frame_time_max_ms;
    float    frame_time_stddev_ms;
    float    avg_cpu_usage_pct;
    uint32_t total_migrations;
    uint32_t total_involuntary_ctxsw;
    float    frame_time_variance_avg;
};

class Baseline {
public:
    static constexpr uint32_t kMaxSamples = 8192u;  // 8192 × 64B = 512 KB

    Baseline() noexcept { samples_.fill(BaselineSample{}); }

    /// Start recording for `pid`. Clears any previous recording.
    void start(uint32_t pid, uint32_t /*duration_ms*/ = 30'000u) noexcept {
        target_pid_  = pid;
        n_samples_   = 0u;
        recording_   = true;
    }

    void stop() noexcept { recording_ = false; }

    /// Fully clear state: target_pid, samples, and recording flag. Unlike
    /// stop(), which preserves samples for post-recording analysis, reset()
    /// returns the Baseline to its default-constructed state.
    void reset() noexcept {
        target_pid_ = 0u;
        n_samples_  = 0u;
        recording_  = false;
    }

    [[nodiscard]] bool     recording()   const noexcept { return recording_; }
    [[nodiscard]] uint32_t sample_count() const noexcept { return n_samples_; }
    [[nodiscard]] uint32_t target_pid()   const noexcept { return target_pid_; }

    /// Push one sample from MetricsCollector output.
    void push_sample(const BaselineSample& s) noexcept {
        if (!recording_ || n_samples_ >= kMaxSamples) return;
        samples_[n_samples_++] = s;
    }

    /// Convenience: push from a TargetMetrics directly.
    void push_metrics(const observer::TargetMetrics& m, uint64_t tsc) noexcept {
        BaselineSample s{};
        s.target_pid             = m.pid;
        s.sample_tsc             = tsc;
        s.frame_time_avg_ms      = m.frame_time_avg_ms;
        s.frame_time_p99_ms      = m.frame_time_p99_ms;
        s.frame_time_variance_ms = m.frame_time_variance_ms;
        s.cpu_usage_pct          = m.cpu_usage_pct;
        s.migrations_per_sec     = m.migrations_per_sec;
        s.involuntary_ctxsw_sec  = m.involuntary_ctxsw_sec;
        push_sample(s);
    }

    /// Aggregate statistics over all recorded samples.
    [[nodiscard]] BaselineSummary summary() const noexcept;

    /// Read-only view of all recorded samples (valid count = sample_count()).
    [[nodiscard]] std::span<const BaselineSample> samples() const noexcept {
        return { samples_.data(), n_samples_ };
    }

private:
    alignas(64) std::array<BaselineSample, kMaxSamples> samples_{};
    uint32_t n_samples_  {0u};
    uint32_t target_pid_ {0u};
    bool     recording_  {false};
};

} // namespace phynned::bench
// Made with my soul - Swately <3
