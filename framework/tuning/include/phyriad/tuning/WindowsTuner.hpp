// framework/tuning/include/phyriad/tuning/WindowsTuner.hpp
// Windows-specific OS tuning for the Phyriad Runtime.
//
// Two interleaved APIs (both supported simultaneously):
//
//   1. Direct API:
//        WindowsTuner tuner{cfg};
//        tuner.apply();              // timeBeginPeriod + SetPriorityClass + WorkingSet
//        WindowsTuner::set_thread_realtime();
//        tuner.revert();             // timeEndPeriod + NORMAL_PRIORITY_CLASS
//
//   2. Snapshot-aware API (legacy — RESTORED via ITuningProvider):
//        TuningSnapshot snap{default_snapshot_path()};
//        TuningConfig cfg{};
//        if (!tuner.apply_full(snap, cfg)) return err;
//        (void)snap.save();
//        snap.mark_committed();
//      Applies full tuning suite:
//        a. Power scheme → High Performance (GUID-based, crash-safe)
//        b. Timer resolution → 1ms via timeBeginPeriod
//        c. Working set lock → 512MB min / 2GB max via SetProcessWorkingSetSize
//        d. Disable dynamic priority boost via SetProcessPriorityBoost(TRUE)
//      All changes recorded in TuningSnapshot for crash-safe rollback.
//
#pragma once

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#  include <timeapi.h>
#endif

#include "PrivilegeCheck.hpp"
#include "TuningProvider.hpp"
#include "TuningSnapshot.hpp"
#include <cstdint>
#include <expected>

namespace phyriad::tuning {

#ifdef _WIN32

struct WindowsTunerConfig {
    // Direct API knobs
    bool     raise_timer_resolution {true};   // timeBeginPeriod(1)
    bool     set_realtime_class     {true};   // REALTIME_PRIORITY_CLASS
    bool     expand_working_set     {true};   // SetProcessWorkingSetSize
    uint64_t min_working_set_mb     {256};    // minimum resident set size (direct API)
};

class WindowsTuner final : public ITuningProvider {
public:
    explicit WindowsTuner(WindowsTunerConfig cfg = {}) noexcept
        : cfg_(cfg) {}

    ~WindowsTuner() noexcept override { revert(); }

    WindowsTuner(WindowsTuner const&)            = delete;
    WindowsTuner& operator=(WindowsTuner const&) = delete;
    WindowsTuner(WindowsTuner&&) noexcept        = default;
    WindowsTuner& operator=(WindowsTuner&&) noexcept = default;

    // ── Direct API ────────────────────────────────────────────────────────────
    bool apply() noexcept {
        if (applied_) return true;
        const auto info = PrivilegeCheck::probe();
        bool any = false;

        if (cfg_.raise_timer_resolution && info.can_raise_timer_resolution) {
            if (timeBeginPeriod(1) == TIMERR_NOERROR) {
                timer_resolution_raised_ = true;
                any = true;
            }
        }

        if (cfg_.set_realtime_class && info.can_set_rt_prio) {
            const HANDLE proc = GetCurrentProcess();
            if (SetPriorityClass(proc, REALTIME_PRIORITY_CLASS)) {
                realtime_class_set_ = true;
                any = true;
            }
        }

        if (cfg_.expand_working_set) {
            const SIZE_T min_bytes = cfg_.min_working_set_mb * 1024 * 1024;
            const SIZE_T max_bytes = min_bytes * 4;
            SetProcessWorkingSetSize(GetCurrentProcess(), min_bytes, max_bytes);
        }

        applied_ = true;
        return any;
    }

    [[nodiscard]] static bool set_thread_realtime() noexcept {
        return SetThreadPriority(GetCurrentThread(),
                                 THREAD_PRIORITY_TIME_CRITICAL) != 0;
    }

    void revert() noexcept {
        if (!applied_) return;
        if (timer_resolution_raised_) {
            timeEndPeriod(1);
            timer_resolution_raised_ = false;
        }
        if (realtime_class_set_) {
            SetPriorityClass(GetCurrentProcess(), NORMAL_PRIORITY_CLASS);
            realtime_class_set_ = false;
        }
        applied_ = false;
    }

    [[nodiscard]] bool is_applied() const noexcept { return applied_; }

    // ── Snapshot-aware ITuningProvider API (legacy — RESTORED) ────────────────
    // Apply full tuning suite (power scheme / timer / working set / prio boost)
    // and record every change in `snap` for crash-safe rollback.
    [[nodiscard]] std::expected<void, phyriad::Error>
    apply_full(TuningSnapshot& snap,
               TuningConfig const& cfg = {},
               bool dry_run = false) noexcept override;

    void revert_snapshot(TuningSnapshot& snap) noexcept override {
        snap.rollback_all();
    }

    [[nodiscard]] bool
    verify_snapshot(TuningSnapshot const& snap) const noexcept override;

    void reapply_snapshot(TuningSnapshot const& snap) noexcept override;

private:
    WindowsTunerConfig cfg_{};
    bool applied_                {false};
    bool timer_resolution_raised_{false};
    bool realtime_class_set_     {false};
};

#else  // Non-Windows stub

struct WindowsTunerConfig {};

class WindowsTuner final : public ITuningProvider {
public:
    explicit WindowsTuner(WindowsTunerConfig = {}) noexcept {}
    bool apply()  noexcept { return false; }
    void revert() noexcept {}
    static bool set_thread_realtime() noexcept { return false; }
    [[nodiscard]] bool is_applied() const noexcept { return false; }

    [[nodiscard]] std::expected<void, phyriad::Error>
    apply_full(TuningSnapshot&, TuningConfig const& = {},
               bool = false) noexcept override { return {}; }
    void revert_snapshot(TuningSnapshot&) noexcept override {}
    [[nodiscard]] bool verify_snapshot(
        TuningSnapshot const&) const noexcept override { return true; }
    void reapply_snapshot(TuningSnapshot const&) noexcept override {}
};

#endif

} // namespace phyriad::tuning
// Made with my soul - Swately <3
