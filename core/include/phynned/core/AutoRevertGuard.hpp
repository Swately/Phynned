// core/include/phynned/core/AutoRevertGuard.hpp
// AutoRevertGuard — 30s post-policy monitoring with automatic revert.
//
// After a policy is applied to a target, Phynned monitors frame-time variance
// for 30 seconds. Every 5 seconds it checks:
//
//   if (current_variance > baseline_variance × 1.20f)
//       → auto-revert + mark exe as "bad" in PerGameMemory
//
// Threshold 20% because variance is noisy; avoids false positives.
// If the monitoring window passes without regression: policy is considered
// good and is persisted by the caller to memory.toml.
//
// Threading: single-thread (agent main thread). on_tick() called each tick.
// Resource:  ~64B per monitored entry × kMaxTargets; no heap.
// Privilege: None (revert calls go through ActionExecutor which handles perms).
//
#pragma once

#include <phynned/core/AdaptiveTick.hpp>
#include <cstdint>
#include <cstring>
#include <array>
#include <cstdio>

// Forward declarations — avoid including heavy headers here.
namespace phynned::action { class ActionExecutor; }
namespace phynned::learn  { class PerGameMemory; }

namespace phynned::core {

// ── MonitorEntry ─────────────────────────────────────────────────────────────
struct alignas(8) MonitorEntry {
    uint32_t pid;                  //  4B
    uint32_t _pad0;                //  4B
    char     exe_name[40];         // 40B
    float    baseline_variance_ms; //  4B — variance at policy-apply time
    uint8_t  _pad1[4];             //  4B
    uint64_t monitoring_until_tsc; //  8B — stop monitoring after this TSC
    uint64_t last_check_tsc;       //  8B — last time we compared variance
    bool     active;               //  1B
    bool     reverted;             //  1B — true if auto-reverted
    uint8_t  _pad2[6];             //  6B
};                                 // = 80B (10 × 8)
static_assert(sizeof(MonitorEntry) == 80u);
static_assert(alignof(MonitorEntry) == 8u);

// ── AutoRevertGuard ───────────────────────────────────────────────────────────
class AutoRevertGuard {
public:
    static constexpr uint32_t kMaxMonitored    = 32u;

    /// Monitoring window in TSC ticks (30s at TSC freq Hz).
    static constexpr uint64_t kMonitorSeconds  = 30u;

    /// Check interval in TSC ticks (every 5s).
    static constexpr uint64_t kCheckSeconds    = 5u;

    /// Variance regression threshold: 20% worse than baseline.
    static constexpr float    kRegressionFactor = 1.20f;

    AutoRevertGuard() noexcept = default;

    AutoRevertGuard(const AutoRevertGuard&)            = delete;
    AutoRevertGuard& operator=(const AutoRevertGuard&) = delete;

    /// Set dependencies. Must be called before any on_policy_applied / on_tick.
    void set_dependencies(action::ActionExecutor* executor,
                          learn::PerGameMemory*   per_game,
                          uint64_t                tsc_freq) noexcept
    {
        executor_ = executor;
        per_game_ = per_game;
        tsc_freq_ = (tsc_freq > 0u) ? tsc_freq : 2'000'000'000ull;
    }

    /// Called immediately after a policy is applied to a process.
    /// Captures baseline variance and starts the monitoring window.
    void on_policy_applied(uint32_t    pid,
                           const char* exe_name,
                           float       current_variance_ms,
                           uint64_t    now_tsc) noexcept;

    /// Called each tick for each active target.
    /// Checks variance every kCheckSeconds; auto-reverts on regression.
    void on_tick(uint32_t pid,
                 float    current_variance_ms,
                 uint64_t now_tsc) noexcept;

    /// Called when a target process exits. Cleans up monitoring entry.
    void on_target_exited(uint32_t pid) noexcept;

    /// Returns true if the monitoring window is still active for `pid`.
    [[nodiscard]] bool is_monitoring(uint32_t pid) const noexcept;

    /// Returns true if the policy was auto-reverted for `pid`.
    [[nodiscard]] bool was_reverted(uint32_t pid) const noexcept;

    [[nodiscard]] uint32_t monitored_count() const noexcept { return n_entries_; }

private:
    alignas(64) std::array<MonitorEntry, kMaxMonitored> entries_{};
    uint32_t n_entries_{0u};

    action::ActionExecutor* executor_{nullptr};
    learn::PerGameMemory*   per_game_{nullptr};
    uint64_t                tsc_freq_{2'000'000'000ull};

    MonitorEntry* find_entry(uint32_t pid) noexcept;
};

} // namespace phynned::core
// Made with my soul - Swately <3
