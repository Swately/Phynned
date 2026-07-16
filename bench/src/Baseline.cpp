// apps/ayama/bench/src/Baseline.cpp
// Baseline — summary aggregation.
//

#include <ayama/bench/Baseline.hpp>
#include <algorithm>
#include <cmath>

namespace ayama::bench {

BaselineSummary Baseline::summary() const noexcept {
    BaselineSummary r{};
    if (n_samples_ == 0u) return r;

    r.sample_count = n_samples_;

    // Compute averages and max
    float sum_avg   = 0.f, sum_p99 = 0.f, sum_var = 0.f, sum_cpu = 0.f;
    float max_p99   = 0.f;
    uint64_t sum_mig = 0ull, sum_ctxsw = 0ull;

    for (uint32_t i = 0u; i < n_samples_; ++i) {
        const BaselineSample& s = samples_[i];
        sum_avg   += s.frame_time_avg_ms;
        sum_p99   += s.frame_time_p99_ms;
        sum_var   += s.frame_time_variance_ms;
        sum_cpu   += s.cpu_usage_pct;
        sum_mig   += s.migrations_per_sec;
        sum_ctxsw += s.involuntary_ctxsw_sec;
        if (s.frame_time_p99_ms > max_p99) max_p99 = s.frame_time_p99_ms;
    }

    const float n = static_cast<float>(n_samples_);
    r.frame_time_avg_ms      = sum_avg / n;
    r.frame_time_p99_ms      = sum_p99 / n;
    r.frame_time_max_ms      = max_p99;
    r.avg_cpu_usage_pct      = sum_cpu / n;
    r.frame_time_variance_avg= sum_var / n;
    r.total_migrations       = static_cast<uint32_t>(sum_mig);
    r.total_involuntary_ctxsw= static_cast<uint32_t>(sum_ctxsw);

    // Compute stddev of frame_time_avg across samples
    float var = 0.f;
    const float mean = r.frame_time_avg_ms;
    for (uint32_t i = 0u; i < n_samples_; ++i) {
        const float d = samples_[i].frame_time_avg_ms - mean;
        var += d * d;
    }
    r.frame_time_stddev_ms = (n > 1.f) ? std::sqrt(var / (n - 1.f)) : 0.f;

    return r;
}

} // namespace ayama::bench
// Made with my soul - Swately <3
