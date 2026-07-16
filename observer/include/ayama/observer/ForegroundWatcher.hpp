// apps/ayama/observer/include/ayama/observer/ForegroundWatcher.hpp
// ForegroundWatcher — O(1) per-tick foreground window + fullscreen detection.
//
// Avoids EnumWindows (expensive). Instead:
//   - Calls GetForegroundWindow() once per tick.
//   - If HWND changed: queries PID, checks window rect vs monitor rect.
//   - Tracks how long each PID has been the foreground process.
//
// On non-Windows platforms the class compiles but all methods are no-ops.
//
// Threading: single-thread (agent main thread).
// Resource:  ~64B state; Win32 calls are ~1 µs each.
// Privilege: None.
//
#pragma once

#include <cstdint>
#include <cwchar>   // wcsncmp for window-class check

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace ayama::observer {

class ForegroundWatcher {
public:
    ForegroundWatcher() noexcept = default;

    ForegroundWatcher(const ForegroundWatcher&)            = delete;
    ForegroundWatcher& operator=(const ForegroundWatcher&) = delete;

    /// Call once per tick from the agent main thread.
    /// Updates internal state; O(1) per tick.
    ///
    /// **Cumulative tracking:** foreground time accumulated PER-PID (not per-HWND).
    /// Previously the counter was reset on every HWND change, making it impossible
    /// for the classifier to see "foreground_for_sec > 30" if the user alt-tabbed
    /// even briefly. Now a process can accumulate foreground time even when it
    /// briefly loses focus.
    void on_tick(uint32_t tick_interval_ms) noexcept {
#ifdef _WIN32
        const HWND fg = GetForegroundWindow();
        if (fg == nullptr) {
            foreground_pid_        = 0u;
            foreground_fullscreen_ = false;
            last_fg_               = nullptr;
            return;
        }

        // If the HWND changed, re-query PID + fullscreen. If only the HWND
        // changed but the PID is the same (e.g. game switching internal sub-window),
        // maintain foreground tracking without reset.
        DWORD new_pid_raw = 0u;
        if (fg != last_fg_) {
            GetWindowThreadProcessId(fg, &new_pid_raw);
            last_fg_ = fg;

            // Fullscreen check: combines window style + area coverage.
            //
            // The previous version used only area coverage (≥ 95% of monitor)
            // which produced false positives for maximized normal windows
            // (Steam UI, Chrome maximized — they have a title bar and cover
            // ~97-98% of the monitor, so they were flagged).
            //
            // New logic: window must EITHER fit the monitor exactly (rare
            // edge case for exclusive fullscreen) OR have a borderless style
            // (no WS_CAPTION title bar, or WS_POPUP) AND cover ≥95% of the
            // monitor.
            //
            // Style breakdown:
            //   - Exclusive fullscreen           → typically WS_POPUP, no caption
            //   - Borderless windowed (Hogwarts) → WS_POPUP, no caption
            //   - Maximized normal app (Steam)   → WS_OVERLAPPEDWINDOW + WS_MAXIMIZE
            //                                       (HAS caption — now correctly
            //                                       rejected)
            //   - Windowed game                  → WS_OVERLAPPEDWINDOW with caption
            //                                       (correctly rejected — user
            //                                       deliberately chose windowed)
            RECT rect{};
            GetWindowRect(fg, &rect);
            const HMONITOR mon = MonitorFromWindow(fg, MONITOR_DEFAULTTONEAREST);
            MONITORINFO mi{};
            mi.cbSize = sizeof(mi);
            if (GetMonitorInfoW(mon, &mi)) {
                const long long mon_w = mi.rcMonitor.right  - mi.rcMonitor.left;
                const long long mon_h = mi.rcMonitor.bottom - mi.rcMonitor.top;
                const long long win_w = (rect.right > rect.left)
                                        ? (rect.right - rect.left) : 0;
                const long long win_h = (rect.bottom > rect.top)
                                        ? (rect.bottom - rect.top) : 0;
                const long long mon_area = mon_w * mon_h;
                const long long win_area = win_w * win_h;

                const bool fits_monitor =
                    rect.left   == mi.rcMonitor.left   &&
                    rect.top    == mi.rcMonitor.top    &&
                    rect.right  == mi.rcMonitor.right  &&
                    rect.bottom == mi.rcMonitor.bottom;

                const LONG_PTR style = GetWindowLongPtrW(fg, GWL_STYLE);
                const bool ws_popup    = (style & WS_POPUP)     != 0;
                const bool has_caption = (style & WS_CAPTION)   != 0;
                const bool has_thick   = (style & WS_THICKFRAME) != 0;

                // Window class name: Chromium-based apps (Steam UI since
                // 2023, Discord, VS Code, all Electron apps) use the class
                // name pattern "Chrome_WidgetWin_*". They draw their own
                // title bar so WS_CAPTION is missing — that fooled the
                // earlier check into thinking they were borderless games.
                //
                // Real games NEVER use Chromium for their main window.
                wchar_t class_name[64]{};
                GetClassNameW(fg, class_name, 64);
                const bool is_chromium_class =
                    (std::wcsncmp(class_name, L"Chrome_WidgetWin_", 17) == 0);

                // Borderless game heuristic:
                //   WS_POPUP set (most fullscreen/borderless games), OR
                //   (no caption AND no thick frame) — covers borderless
                //   games that omit WS_POPUP but also aren't resizable.
                // The !has_thick clause specifically rules out custom-title-
                // bar resizable windows like Steam UI / Discord maximized.
                const bool borderless =
                    ws_popup || (!has_caption && !has_thick);

                const bool large_area =
                    (mon_area > 0) && (win_area * 100 >= mon_area * 95);

                // Final gate: exclude Chromium-class windows ALWAYS, even
                // if they pass other checks. This is the definitive
                // anti-false-positive for Steam UI / Discord / Electron.
                foreground_fullscreen_ = !is_chromium_class &&
                    (fits_monitor || (borderless && large_area));
            } else {
                foreground_fullscreen_ = false;
            }
        } else {
            new_pid_raw = static_cast<DWORD>(foreground_pid_);
        }
        foreground_pid_ = static_cast<uint32_t>(new_pid_raw);

        // Acumular el tick en la tabla per-PID. Buscar el slot o crear uno.
        for (uint32_t i = 0u; i < n_pid_entries_; ++i) {
            if (pid_table_[i].pid == foreground_pid_) {
                pid_table_[i].total_ms += tick_interval_ms;
                pid_table_[i].last_seen_ms_counter = tick_counter_;
                ++tick_counter_;
                return;
            }
        }
        // Slot nuevo. Si table llena, evict la entrada LRU.
        uint32_t slot = n_pid_entries_;
        if (n_pid_entries_ < kMaxPidEntries) {
            ++n_pid_entries_;
        } else {
            // Find LRU slot.
            uint64_t oldest_lru = pid_table_[0].last_seen_ms_counter;
            slot = 0u;
            for (uint32_t i = 1u; i < n_pid_entries_; ++i) {
                if (pid_table_[i].last_seen_ms_counter < oldest_lru) {
                    oldest_lru = pid_table_[i].last_seen_ms_counter;
                    slot = i;
                }
            }
        }
        pid_table_[slot].pid                  = foreground_pid_;
        pid_table_[slot].total_ms             = tick_interval_ms;
        pid_table_[slot].last_seen_ms_counter = tick_counter_;
        ++tick_counter_;
#else
        (void)tick_interval_ms;
#endif
    }

    /// PID of the current foreground process. 0 if none.
    [[nodiscard]] uint32_t foreground_pid() const noexcept {
        return foreground_pid_;
    }

    /// Cumulative foreground time for the CURRENT foreground PID (ms).
    /// Survives alt-tabs: una vez que un PID ha estado foreground N ms, esa
    /// suma persiste aunque el user vaya a otras ventanas y vuelva.
    [[nodiscard]] uint32_t foreground_for_ms() const noexcept {
        for (uint32_t i = 0u; i < n_pid_entries_; ++i) {
            if (pid_table_[i].pid == foreground_pid_)
                return pid_table_[i].total_ms;
        }
        return 0u;
    }

    /// Cumulative foreground time for the CURRENT foreground PID (seconds).
    [[nodiscard]] uint32_t foreground_for_sec() const noexcept {
        return foreground_for_ms() / 1000u;
    }

    /// Cumulative foreground time for any tracked PID (0 if not tracked).
    /// Útil para clasificar procesos que no son foreground AHORA pero lo fueron.
    [[nodiscard]] uint32_t cumulative_foreground_ms(uint32_t pid) const noexcept {
        for (uint32_t i = 0u; i < n_pid_entries_; ++i) {
            if (pid_table_[i].pid == pid) return pid_table_[i].total_ms;
        }
        return 0u;
    }

    /// True if the foreground window covers the entire primary/nearest monitor.
    [[nodiscard]] bool is_foreground_fullscreen() const noexcept {
        return foreground_fullscreen_;
    }

    /// True if `pid` is the current foreground process.
    [[nodiscard]] bool is_foreground(uint32_t pid) const noexcept {
        return foreground_pid_ != 0u && foreground_pid_ == pid;
    }

private:
    // ── Per-PID cumulative foreground tracking ────────────────────────────
    struct PidEntry {
        uint32_t pid;                       // 0 = empty slot
        uint32_t total_ms;                  // cumulative foreground time
        uint64_t last_seen_ms_counter;      // for LRU eviction
    };
    static constexpr uint32_t kMaxPidEntries = 32u;
    PidEntry pid_table_[kMaxPidEntries]{};
    uint32_t n_pid_entries_  {0u};
    uint64_t tick_counter_   {0ull};

    uint32_t foreground_pid_       {0u};
    bool     foreground_fullscreen_{false};
    uint8_t  _pad[3]               {};

#ifdef _WIN32
    HWND     last_fg_              {nullptr};
#endif
};

} // namespace ayama::observer
// Made with my soul - Swately <3
