// framework/tuning/include/phyriad/tuning/LinuxTuner.hpp
// Linux-specific OS tuning for the Phyriad Runtime.
//
// Two interleaved APIs (both supported simultaneously):
//
//   1. Direct API:
//        LinuxTuner tuner{cfg};
//        tuner.apply();              // mlockall + RLIMIT_RTPRIO
//        tuner.set_thread_realtime();// SCHED_FIFO on current thread
//        tuner.revert();             // munlockall
//
//   2. Snapshot-aware API (legacy — RESTORED via ITuningProvider):
//        TuningSnapshot snap{default_snapshot_path()};
//        TuningConfig cfg{};
//        if (!tuner.apply_full(snap, cfg)) return err;
//        (void)snap.save();
//        snap.mark_committed();
//      Applies full tuning suite:
//        a. THP defrag → "defer+madvise"
//        b. CPU freq governor → "performance" (per-CPU)
//        c. Disable C-states 1+ (per-CPU per-state)
//        d. IRQ rerouting → /proc/irq/*/smp_affinity
//      All sysfs writes are recorded in TuningSnapshot for crash-safe rollback.
//
#pragma once

#include "PrivilegeCheck.hpp"
#include "TuningProvider.hpp"
#include "TuningSnapshot.hpp"
#include <cstdint>
#include <expected>

#ifndef _WIN32
#  include <pthread.h>
#  include <sched.h>
#  include <sys/mman.h>
#  include <sys/resource.h>
#endif

namespace phyriad::tuning {

#ifndef _WIN32

struct LinuxTunerConfig {
    // Direct API knobs
    bool use_sched_fifo      {true};   // SCHED_FIFO for worker threads
    bool mlock_all           {true};   // mlockall — pin all pages in RAM
    bool disable_rt_throttle {false};  // /proc: requires root; default off
    int  fifo_priority       {50};     // SCHED_FIFO priority (1-99)
};

class LinuxTuner final : public ITuningProvider {
public:
    explicit LinuxTuner(LinuxTunerConfig cfg = {}) noexcept
        : cfg_(cfg) {}

    ~LinuxTuner() noexcept override { revert(); }

    LinuxTuner(LinuxTuner const&)            = delete;
    LinuxTuner& operator=(LinuxTuner const&) = delete;
    LinuxTuner(LinuxTuner&&) noexcept        = default;
    LinuxTuner& operator=(LinuxTuner&&) noexcept = default;

    // ── Direct API ───────────────────────────────────────────────────────────
    bool apply() noexcept {
        if (applied_) return true;
        const auto info = PrivilegeCheck::probe();
        bool any = false;

        if (cfg_.mlock_all && info.can_lock_pages) {
            if (mlockall(MCL_CURRENT | MCL_FUTURE) == 0) {
                mlocked_ = true;
                any = true;
            }
        }

        // Raise RLIMIT_RTPRIO if possible.
        if (info.can_set_rt_prio) {
            struct rlimit rl{};
            if (getrlimit(RLIMIT_RTPRIO, &rl) == 0) {
                if (rl.rlim_max > 0 || rl.rlim_max == RLIM_INFINITY) {
                    rl.rlim_cur = (rl.rlim_max == RLIM_INFINITY) ? 99u
                                                                  : rl.rlim_max;
                    setrlimit(RLIMIT_RTPRIO, &rl);
                    any = true;
                }
            }
        }

        applied_ = true;
        return any;
    }

    [[nodiscard]] bool set_thread_realtime() noexcept {
        if (!cfg_.use_sched_fifo) return false;
        struct sched_param sp{};
        sp.sched_priority = cfg_.fifo_priority;
        return pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp) == 0;
    }

    void revert() noexcept {
        if (!applied_) return;
        if (mlocked_) {
            munlockall();
            mlocked_ = false;
        }
        applied_ = false;
    }

    [[nodiscard]] bool is_applied() const noexcept { return applied_; }

    // ── Snapshot-aware ITuningProvider API (legacy — RESTORED) ───────────────
    // Apply full tuning suite (THP / governor / C-states / IRQ) and record
    // every change in `snap` for crash-safe rollback. Caller must invoke
    // `snap.save()` and `snap.mark_committed()` after success.
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
    LinuxTunerConfig cfg_{};
    bool applied_{false};
    bool mlocked_{false};
};

#else  // Windows stub for Linux-specific tuner

struct LinuxTunerConfig {};

class LinuxTuner final : public ITuningProvider {
public:
    explicit LinuxTuner(LinuxTunerConfig = {}) noexcept {}
    bool apply()  noexcept { return false; }
    void revert() noexcept {}
    bool set_thread_realtime() noexcept { return false; }
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
