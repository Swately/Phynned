// framework/tuning/include/phyriad/tuning/TuningDaemon.hpp
// Background tuning daemon — monitors and re-applies OS tuning as needed.
//
// The TuningDaemon runs a low-priority background thread with two operation
// modes (both supported simultaneously):
//
//   1. Direct mode (preserved):
//        TuningDaemon daemon;
//        daemon.start(my_windows_tuner);  // polls is_applied() + apply()
//      Used for simple "did the OS revert our settings?" monitoring.
//
//   2. Snapshot-aware mode (legacy — RESTORED):
//        TuningDaemon daemon;
//        daemon.start_with_snapshot_recovery(*provider, snapshot_path);
//      On startup:
//        - If `snapshot_path` exists → treat as orphaned crash snapshot,
//          rollback automatically before applying fresh tunings.
//      During run:
//        - Periodically calls `provider.verify_snapshot()` and
//          `provider.reapply_snapshot()` on regression. This catches OS
//          actions that revert specific records (e.g. Windows restoring the
//          balanced power scheme after sleep/resume).
//
#pragma once
#include "PrivilegeCheck.hpp"
#include "WindowsTuner.hpp"
#include "LinuxTuner.hpp"
#include "TuningProvider.hpp"
#include "TuningSnapshot.hpp"
#include <atomic>
#include <chrono>
#include <filesystem>
#include <memory>
#include <thread>
#include <phyriad/hal/MemoryOrder.hpp>

namespace phyriad::tuning {

class TuningDaemon {
public:
    TuningDaemon() noexcept = default;
    ~TuningDaemon() noexcept { stop(); }

    TuningDaemon(TuningDaemon const&)            = delete;
    TuningDaemon& operator=(TuningDaemon const&) = delete;
    TuningDaemon(TuningDaemon&&)                 = delete;
    TuningDaemon& operator=(TuningDaemon&&)      = delete;

    // Start the daemon against a WindowsTuner instance.
    // The tuner must outlive the daemon.
    void start(WindowsTuner& tuner, uint32_t poll_ms = 5000u) noexcept {
        if (running_.load()) return;
        tuner_win_   = &tuner;
        poll_ms_     = poll_ms;
        hal::seq_store_release(running_, true);
        thread_ = std::thread([this]() noexcept { loop_win(); });
    }

    // Start the daemon against a LinuxTuner instance.
    void start(LinuxTuner& tuner, uint32_t poll_ms = 5000u) noexcept {
        if (running_.load()) return;
        tuner_lin_   = &tuner;
        poll_ms_     = poll_ms;
        hal::seq_store_release(running_, true);
        thread_ = std::thread([this]() noexcept { loop_lin(); });
    }

    // ── Snapshot-aware mode (legacy — RESTORED) ───────────────────────────────
    // Start the daemon against an ITuningProvider with full snapshot lifecycle:
    //   1. If `snapshot_path` exists on disk, treat it as an orphaned crash
    //      snapshot — load + rollback_all() before applying fresh tunings.
    //      This is the boot-time recovery feature of the legacy 3-level
    //      rollback strategy.
    //   2. Apply fresh tunings via `provider.apply_full(*current_snap_, cfg)`.
    //   3. Periodically poll `verify_snapshot()` — on regression call
    //      `reapply_snapshot()` to restore our settings without re-recording.
    void start_with_snapshot_recovery(
        ITuningProvider&            provider,
        std::filesystem::path       snapshot_path = default_snapshot_path(),
        TuningConfig const&         cfg           = {},
        uint32_t                    poll_ms       = 5000u) noexcept
    {
        if (running_.load()) return;

        // ── Boot-time orphan recovery ──
        // If a snapshot file exists from a previous (crashed) session, roll
        // back the recorded changes before applying fresh ones. This is the
        // critical production-safety feature of the legacy three-level
        // rollback strategy.
        std::error_code ec{};
        if (std::filesystem::exists(snapshot_path, ec) && !ec) {
            if (auto orphan = TuningSnapshot::load(snapshot_path); orphan) {
                orphan->rollback_all();
                std::filesystem::remove(snapshot_path, ec);
            }
        }

        // Apply fresh tunings into a new snapshot.
        current_snap_ = std::make_unique<TuningSnapshot>(snapshot_path);
        if (!provider.apply_full(*current_snap_, cfg)) {
            // apply_full failed — RAII destructor of current_snap_ rolls back
            // any partial changes when we reset it.
            current_snap_.reset();
            return;
        }
        (void)current_snap_->save();   // persist for crash recovery

        provider_     = &provider;
        poll_ms_      = poll_ms;
        hal::seq_store_release(running_, true);
        thread_ = std::thread([this]() noexcept { loop_snapshot(); });
    }

    void stop() noexcept {
        hal::seq_store_release(running_, false);
        if (thread_.joinable()) thread_.join();
        tuner_win_ = nullptr;
        tuner_lin_ = nullptr;
        // Clean shutdown: mark the snapshot committed so RAII does NOT roll
        // back (caller is responsible for explicitly reverting if desired).
        if (current_snap_) {
            current_snap_->mark_committed();
            // Delete the persisted file — the process exited cleanly so the
            // boot-time recovery should not see an orphan.
            std::error_code ec{};
            std::filesystem::remove(current_snap_->path(), ec);
        }
        current_snap_.reset();
        provider_ = nullptr;
    }

    [[nodiscard]] bool running() const noexcept {
        return hal::seq_load_acquire(running_);
    }

    // Number of times tuning was re-applied due to detected loss.
    [[nodiscard]] uint64_t reapply_count() const noexcept {
        return hal::stat_load_relaxed(reapply_count_);
    }

private:
    std::thread                       thread_{};
    std::atomic<bool>                 running_{false};
    WindowsTuner*                     tuner_win_{nullptr};
    LinuxTuner*                       tuner_lin_{nullptr};
    ITuningProvider*                  provider_{nullptr};
    std::unique_ptr<TuningSnapshot>   current_snap_{};
    uint32_t                          poll_ms_{5000};
    std::atomic<uint64_t>             reapply_count_{0};

    void loop_win() noexcept {
        while (hal::seq_load_acquire(running_)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(poll_ms_));
            if (!tuner_win_) continue;
            if (!tuner_win_->is_applied()) {
                tuner_win_->apply();
                hal::stat_fetch_add_relaxed(reapply_count_, 1);
            }
        }
    }

    void loop_lin() noexcept {
        while (hal::seq_load_acquire(running_)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(poll_ms_));
            if (!tuner_lin_) continue;
            if (!tuner_lin_->is_applied()) {
                tuner_lin_->apply();
                hal::stat_fetch_add_relaxed(reapply_count_, 1);
            }
        }
    }

    // Snapshot-aware monitor loop: periodically verify the recorded tunings
    // still hold; reapply via `reapply_snapshot()` on regression. Does NOT
    // call apply_full() — we re-apply existing records, not add new ones.
    void loop_snapshot() noexcept {
        while (hal::seq_load_acquire(running_)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(poll_ms_));
            if (!provider_ || !current_snap_) continue;

            if (!provider_->verify_snapshot(*current_snap_)) {
                provider_->reapply_snapshot(*current_snap_);
                hal::stat_fetch_add_relaxed(reapply_count_, 1);
            }
        }
    }
};

} // namespace phyriad::tuning
// Made with my soul - Swately <3
