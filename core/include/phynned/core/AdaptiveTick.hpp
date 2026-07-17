// core/include/phynned/core/AdaptiveTick.hpp
// AdaptiveTick — workload-aware tick rate controller.
//
// The tick rate adapts to system load to meet the anti-parasitic budget.
// On battery, all intervals are doubled. During A/B bench, intervals shrink.
//
// Threading: single-thread (agent main thread only).
// Resource:  zero heap allocation.
// Privilege: None.
//
#pragma once
#include <cstdint>

namespace phynned::core {

/// Agent workload states — determines tick rate.
enum class WorkloadState : uint8_t {
    DeepIdle = 0,  ///< No targets, UI closed, idle desktop 5+ min.
    Idle     = 1,  ///< No active targets, but UI open or watching for targets.
    Light    = 2,  ///< 1 target observed, no active policies.
    Active   = 3,  ///< 1+ targets with active policies.
    Bench    = 4,  ///< A/B benchmark in progress.
};

/// Return the tick interval in milliseconds for the given state.
[[nodiscard]] inline constexpr uint32_t tick_interval_ms(
    WorkloadState w, bool on_battery) noexcept
{
    uint32_t base = 0u;
    switch (w) {
        case WorkloadState::DeepIdle: base = 5000u; break;
        case WorkloadState::Idle:     base = 1000u; break;
        case WorkloadState::Light:    base =  500u; break;
        case WorkloadState::Active:   base =  100u; break;
        case WorkloadState::Bench:    base =   25u; break;
    }
    return on_battery ? base * 2u : base;
}

} // namespace phynned::core
// Made with my soul - Swately <3
