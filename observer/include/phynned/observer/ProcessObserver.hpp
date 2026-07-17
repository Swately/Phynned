// observer/include/phynned/observer/ProcessObserver.hpp
// ProcessObserver — enumerate and track target processes by name pattern.
//
// Maintains an up-to-date table of observed processes. Patterns are
// case-insensitive substring matches on the executable short name
// (e.g. "notepad" matches "Notepad.exe").
//
// On Windows: uses CreateToolhelp32Snapshot + Process32FirstW/NextW.
// On Linux:   reads /proc/*/exe and /proc/*/status.
//
// Threading: single-thread (agent main thread). refresh() is NOT thread-safe.
// Resource:  zero heap after construction (std::array pre-allocated).
//            refresh() takes ~1–3 ms with 200 active processes.
// Privilege: No admin needed for snapshot enumeration.
//            PROCESS_QUERY_INFORMATION access for detailed info is usually
//            available without admin for non-protected processes.
//
#pragma once

#include <phynned/observer/TargetProcess.hpp>
#include <cstdint>
#include <array>

namespace phynned::observer {

/// Maximum number of name patterns that can be registered.
inline constexpr uint32_t kMaxPatterns = 64u;

class ProcessObserver {
public:
    ProcessObserver() noexcept;
    ~ProcessObserver() noexcept;

    ProcessObserver(ProcessObserver const&)            = delete;
    ProcessObserver& operator=(ProcessObserver const&) = delete;

    // ── Pattern management ────────────────────────────────────────────────
    /// Register an executable name pattern (case-insensitive substring).
    /// E.g. "notepad", "obs64", "cyberpunk2077".
    void add_target_pattern(const char* pattern) noexcept;
    void remove_target_pattern(const char* pattern) noexcept;
    void clear_target_patterns() noexcept;

    // ── Refresh & snapshot ────────────────────────────────────────────────
    /// Re-enumerate all processes on the system and update the target table.
    /// Cheap: ~1–3 ms; call every tick.
    void refresh() noexcept;

    /// Copy the current target table into `out`.
    /// Returns the number of entries written (≤ min(max, kMaxTargets)).
    [[nodiscard]] uint32_t snapshot(TargetProcess* out, uint32_t max) const noexcept;

    /// Count of currently active (running) targets.
    [[nodiscard]] uint32_t target_count() const noexcept { return n_targets_; }

    /// Fill `out_pids` with the PIDs of the current targets.
    [[nodiscard]] uint32_t target_pids(uint32_t* out_pids, uint32_t max) const noexcept;

private:
    // ── Pre-allocated storage ──────────────────────────────────────────────
    alignas(64) std::array<TargetProcess, kMaxTargets> targets_{};
    uint32_t n_targets_{0u};

    struct Pattern {
        char text[40];
        bool active;
    };
    std::array<Pattern, kMaxPatterns> patterns_{};
    uint32_t n_patterns_{0u};

    // ── Internal helpers ───────────────────────────────────────────────────
    [[nodiscard]] bool matches_any_pattern(const char* exe_name) const noexcept;
    void update_target(uint32_t pid, uint32_t parent_pid,
                       const char* exe_name) noexcept;
    void expire_dead_targets() noexcept;
    // Platform-specific refresh_win32/refresh_linux removed — refresh() now
    // uses phyriad::proc::enumerate_processes (FR-4) which handles both
    // platforms internally.
};

} // namespace phynned::observer
// Made with my soul - Swately <3
