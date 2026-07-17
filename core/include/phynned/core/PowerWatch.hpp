// core/include/phynned/core/PowerWatch.hpp
// PowerWatch — battery and AC power status query.
//
// Polled every ~10 ticks to detect battery transitions.
// Drives tick-rate doubling in battery mode and bench blocking.
//
// Threading: single-thread (agent main thread).
// Resource:  zero heap; system call is ~1 µs.
// Privilege: None.
//
#pragma once
#include <cstdint>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace phynned::core {

class PowerWatch {
public:
    PowerWatch() noexcept { refresh(); }

    /// Refresh power status from OS.
    void refresh() noexcept {
#ifdef _WIN32
        SYSTEM_POWER_STATUS ps{};
        if (GetSystemPowerStatus(&ps)) {
            on_battery_  = (ps.ACLineStatus == 0);
            battery_pct_ = ps.BatteryLifePercent;   // 255 = unknown
        }
#endif
    }

    [[nodiscard]] bool    on_battery()   const noexcept { return on_battery_;  }
    [[nodiscard]] uint8_t battery_pct()  const noexcept { return battery_pct_; }

private:
    bool    on_battery_  {false};
    uint8_t battery_pct_ {255u};
};

} // namespace phynned::core
// Made with my soul - Swately <3
