// apps/ayama/observer/include/ayama/observer/EtwProviderSet.hpp
// EtwProviderSet — dynamic ETW provider tier selection.
//
// Governs which ETW provider groups are active at each workload tier.
// The EtwSessionManager compares the desired tier (derived from WorkloadState)
// with the currently active tier and calls MetricsCollector::start() /
// MetricsCollector::stop() only when a tier boundary is crossed.
//
// Tier table:
//
//  WorkloadState │ EtwProviderSet │ Active providers
//  ──────────────┼────────────────┼─────────────────────────────────────────────
//  DeepIdle      │ None           │ (all ETW stopped — near-zero overhead)
//  Idle          │ CpuOnly        │ NT/CSWITCH — context-switch events only
//  Light         │ CpuOnly        │ NT/CSWITCH — context-switch events only
//  Active        │ Full           │ CpuOnly + PresentMon frame-timing events
//  Bench         │ Full           │ CpuOnly + PresentMon frame-timing events
//
// Hysteresis: transitions are gated on a configurable tick counter so a
// brief spike in workload does not cause rapid enable/disable churn.
//
// Threading: all methods called from the agent main thread.  NOT thread-safe.
#pragma once

#include <ayama/core/AdaptiveTick.hpp>    // WorkloadState
#include <ayama/observer/MetricsCollector.hpp>

#include <cstdint>
#include <cstdio>

namespace ayama::observer {

// ── Provider-set tiers ────────────────────────────────────────────────────────

/// Which ETW provider group is currently active.
enum class EtwProviderSet : uint8_t {
    None    = 0,  ///< All ETW stopped. Near-zero overhead. (DeepIdle)
    CpuOnly = 1,  ///< NT/CSWITCH — context-switch events only. (Idle, Light)
    Full    = 2,  ///< CpuOnly + PresentMon frame timing. (Active, Bench)
};

/// Map a WorkloadState to the desired EtwProviderSet tier.
[[nodiscard]] inline EtwProviderSet
provider_set_for(core::WorkloadState state) noexcept {
    switch (state) {
        using enum core::WorkloadState;
        case DeepIdle: return EtwProviderSet::None;
        case Idle:     return EtwProviderSet::CpuOnly;
        case Light:    return EtwProviderSet::CpuOnly;
        case Active:   return EtwProviderSet::Full;
        case Bench:    return EtwProviderSet::Full;
    }
    return EtwProviderSet::CpuOnly;   // safe default
}

// ── EtwSessionManager ─────────────────────────────────────────────────────────

/// Manages ETW provider lifecycle based on workload tier transitions.
///
/// Call on_workload_changed() once per tick. It applies hysteresis (a
/// configurable settle count) before transitioning between tiers to avoid
/// rapid churn on brief workload spikes.
///
/// Currently CpuOnly and Full both use MetricsCollector::start() because
/// MetricsCollector does not yet support partial-provider sessions.
/// The tier distinction is retained so future versions can enable PresentMon
/// separately without changing call sites.
class EtwSessionManager {
public:
    /// `settle_ticks` — how many consecutive ticks a new tier must be
    /// requested before the transition is committed (default: 3 ticks).
    explicit EtwSessionManager(uint32_t settle_ticks = 3u) noexcept
        : settle_ticks_{settle_ticks}
    {}

    /// Call once per tick.  `desired` is derived from the current WorkloadState
    /// via `provider_set_for()`.  May call `collector.start()` / `stop()`.
    void on_workload_changed(EtwProviderSet desired,
                             MetricsCollector& collector) noexcept {
        if (desired == desired_) {
            pending_count_ = 0u;   // stable — reset hysteresis
            return;
        }

        // New desired tier: start counting settle ticks.
        if (desired != pending_tier_) {
            pending_tier_  = desired;
            pending_count_ = 0u;
        }

        ++pending_count_;
        if (pending_count_ < settle_ticks_) {
            return;   // not settled yet — hold current tier
        }

        // Commit the transition.
        desired_      = desired;
        pending_tier_ = desired;
        pending_count_ = 0u;
        apply_tier(desired, collector);
    }

    /// Force immediate transition to `tier` (called from start() and tests).
    void force_tier(EtwProviderSet tier, MetricsCollector& collector) noexcept {
        desired_       = tier;
        pending_tier_  = tier;
        pending_count_ = 0u;
        apply_tier(tier, collector);
    }

    [[nodiscard]] EtwProviderSet current()  const noexcept { return current_; }
    [[nodiscard]] bool           etw_active() const noexcept {
        return current_ != EtwProviderSet::None;
    }

private:
    EtwProviderSet current_      {EtwProviderSet::None};
    EtwProviderSet desired_      {EtwProviderSet::None};
    EtwProviderSet pending_tier_ {EtwProviderSet::None};
    uint32_t       pending_count_{0u};
    uint32_t       settle_ticks_ {3u};

    void apply_tier(EtwProviderSet tier, MetricsCollector& collector) noexcept {
        if (tier == current_) return;

        const EtwProviderSet prev = current_;
        current_ = tier;

        switch (tier) {
            case EtwProviderSet::None:
                if (prev != EtwProviderSet::None) {
                    collector.stop();
                    std::fprintf(stdout,
                        "[EtwSessionManager] ETW stopped (DeepIdle).\n");
                }
                break;

            case EtwProviderSet::CpuOnly:
                if (prev == EtwProviderSet::None) {
                    const auto r = collector.start();
                    if (!r) {
                        std::fprintf(stderr,
                            "[EtwSessionManager] ETW start failed "
                            "(code=%d) — falling back.\n",
                            static_cast<int>(r.error().code));
                        current_ = EtwProviderSet::None;
                    } else {
                        std::fprintf(stdout,
                            "[EtwSessionManager] ETW started (CpuOnly).\n");
                    }
                }
                // Full → CpuOnly: MetricsCollector has no partial-disable yet;
                // leave session running (PresentMon events are just ignored).
                break;

            case EtwProviderSet::Full:
                if (prev == EtwProviderSet::None) {
                    const auto r = collector.start();
                    if (!r) {
                        std::fprintf(stderr,
                            "[EtwSessionManager] ETW start failed "
                            "(code=%d) — falling back.\n",
                            static_cast<int>(r.error().code));
                        current_ = EtwProviderSet::None;
                    } else {
                        std::fprintf(stdout,
                            "[EtwSessionManager] ETW started (Full).\n");
                    }
                }
                // CpuOnly → Full: same session, PresentMon events now accepted.
                break;
        }
    }
};

} // namespace ayama::observer
// Made with my soul - Swately <3
