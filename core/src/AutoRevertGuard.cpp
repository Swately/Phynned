// apps/ayama/core/src/AutoRevertGuard.cpp
// AutoRevertGuard — implementation.
//
#include <ayama/core/AutoRevertGuard.hpp>
#include <ayama/action/ActionExecutor.hpp>
#include <ayama/learn/PerGameMemory.hpp>

#include <cstring>
#include <cstdio>

namespace ayama::core {

// ── find_entry ────────────────────────────────────────────────────────────────
MonitorEntry* AutoRevertGuard::find_entry(uint32_t pid) noexcept
{
    for (uint32_t i = 0u; i < n_entries_; ++i) {
        if (entries_[i].pid == pid) return &entries_[i];
    }
    return nullptr;
}

// ── on_policy_applied ─────────────────────────────────────────────────────────
void AutoRevertGuard::on_policy_applied(uint32_t    pid,
                                        const char* exe_name,
                                        float       current_variance_ms,
                                        uint64_t    now_tsc) noexcept
{
    // Find or create entry.
    MonitorEntry* e = find_entry(pid);
    if (e == nullptr) {
        if (n_entries_ >= kMaxMonitored) return;  // no space — skip monitoring
        e = &entries_[n_entries_++];
    }

    e->pid                  = pid;
    std::strncpy(e->exe_name, exe_name, sizeof(e->exe_name) - 1u);
    e->exe_name[sizeof(e->exe_name) - 1u] = '\0';

    // Capture baseline variance at apply time.
    e->baseline_variance_ms  = (current_variance_ms > 0.0f) ? current_variance_ms : 1.0f;

    // Set monitoring window.
    e->monitoring_until_tsc  = now_tsc + (tsc_freq_ * kMonitorSeconds);
    e->last_check_tsc        = now_tsc;
    e->active                = true;
    e->reverted              = false;

    // frame-time variance is only collected for
    // foreground D3D/Vulkan games (PresentMon-based pipeline). Comm/Stream/
    // Browser apps will always have variance=0 and the "Baseline 0.00 ms"
    // logs flood stdout when Discord spawns ~6 helper PIDs (or msedgewebview
    // hits 10+). Only log when we have a meaningful baseline.
    if (current_variance_ms > 0.0f) {
        std::fprintf(stdout,
            "[Ayama][AutoRevert] Monitoring %s (PID %u) for %llu s. "
            "Baseline variance=%.2f ms\n",
            exe_name, pid,
            static_cast<unsigned long long>(kMonitorSeconds),
            current_variance_ms);
    }
}

// ── on_tick ───────────────────────────────────────────────────────────────────
void AutoRevertGuard::on_tick(uint32_t pid,
                               float    current_variance_ms,
                               uint64_t now_tsc) noexcept
{
    MonitorEntry* e = find_entry(pid);
    if (e == nullptr || !e->active) return;

    // Past monitoring window → mark done, no regression.
    if (now_tsc >= e->monitoring_until_tsc) {
        e->active = false;
        // Only log if the baseline was meaningful (frame variance > 0 at
        // apply time). Otherwise this is a Comm/Stream PID whose "no
        // regression detected" is trivially true and just noise.
        if (e->baseline_variance_ms > 1.0f + 0.0001f) {
            std::fprintf(stdout,
                "[Ayama][AutoRevert] Monitoring window expired for %s (PID %u) "
                "— no regression detected.\n",
                e->exe_name, pid);
        }
        return;
    }

    // Check every kCheckSeconds.
    const uint64_t check_interval_tsc = tsc_freq_ * kCheckSeconds;
    if (now_tsc - e->last_check_tsc < check_interval_tsc) return;
    e->last_check_tsc = now_tsc;

    // Compare current variance to baseline.
    const float threshold = e->baseline_variance_ms * kRegressionFactor;
    if (current_variance_ms > threshold) {
        // Regression detected — auto-revert.
        std::fprintf(stderr,
            "[Ayama][AutoRevert] REGRESSION for %s (PID %u): "
            "variance %.2f ms > threshold %.2f ms (baseline × %.2f). "
            "Auto-reverting.\n",
            e->exe_name, pid,
            current_variance_ms, threshold, kRegressionFactor);

        if (executor_ != nullptr) {
            executor_->revert(pid);
        }

        // Mark as bad in per-game memory so we don't re-apply.
        if (per_game_ != nullptr) {
            per_game_->mark_bad(e->exe_name, "regression_detected");
        }

        e->active   = false;
        e->reverted = true;
    }
}

// ── on_target_exited ──────────────────────────────────────────────────────────
void AutoRevertGuard::on_target_exited(uint32_t pid) noexcept
{
    for (uint32_t i = 0u; i < n_entries_; ++i) {
        if (entries_[i].pid == pid) {
            // Swap with last to keep array compact.
            entries_[i] = entries_[--n_entries_];
            return;
        }
    }
}

// ── Query helpers ─────────────────────────────────────────────────────────────
bool AutoRevertGuard::is_monitoring(uint32_t pid) const noexcept
{
    for (uint32_t i = 0u; i < n_entries_; ++i) {
        if (entries_[i].pid == pid) return entries_[i].active;
    }
    return false;
}

bool AutoRevertGuard::was_reverted(uint32_t pid) const noexcept
{
    for (uint32_t i = 0u; i < n_entries_; ++i) {
        if (entries_[i].pid == pid) return entries_[i].reverted;
    }
    return false;
}

} // namespace ayama::core
// Made with my soul - Swately <3
