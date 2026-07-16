// apps/ayama/core/include/ayama/core/IdleWatch.hpp
// IdleWatch — desktop idle detection for DeepIdle transition.
//
// On Windows: uses GetLastInputInfo() to detect when the user hasn't
// touched mouse/keyboard for 5+ minutes.
// On Linux:  stub (always returns false — to be extended with X11/Wayland).
//
// The DeepIdle state (tick = 5000ms) is entered when:
//   idle_watch.desktop_idle_5min() && n_active_targets == 0
//
// Threading: single-thread (agent main thread).
// Resource:  ~8B state; Win32 call ~1 µs.
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

namespace ayama::core {

class IdleWatch {
public:
    IdleWatch() noexcept = default;

    IdleWatch(const IdleWatch&)            = delete;
    IdleWatch& operator=(const IdleWatch&) = delete;

    /// Returns true if the desktop has been idle for >= 5 minutes.
    [[nodiscard]] bool desktop_idle_5min() noexcept {
#ifdef _WIN32
        LASTINPUTINFO lii{};
        lii.cbSize = sizeof(lii);
        if (GetLastInputInfo(&lii)) {
            const DWORD elapsed_ms = GetTickCount() - lii.dwTime;
            return elapsed_ms >= kIdleThresholdMs;
        }
#endif
        return false;
    }

    /// Returns the milliseconds since last user input.
    [[nodiscard]] uint32_t idle_duration_ms() noexcept {
#ifdef _WIN32
        LASTINPUTINFO lii{};
        lii.cbSize = sizeof(lii);
        if (GetLastInputInfo(&lii)) {
            return static_cast<uint32_t>(GetTickCount() - lii.dwTime);
        }
#endif
        return 0u;
    }

private:
    static constexpr uint32_t kIdleThresholdMs = 300'000u;  // 5 minutes
};

} // namespace ayama::core
// Made with my soul - Swately <3
