// framework/tuning/include/phyriad/tuning/TuningProvider.hpp
// Abstract interface for platform-specific OS tuning providers.
//
// ITuningProvider is the abstract base; LinuxTuner and WindowsTuner are the
// concrete implementations compiled per platform. ITuningProvider::make()
// returns the appropriate concrete type at runtime.
//
// Usage:
//   auto tuner = tuning::ITuningProvider::make();
//   tuning::TuningSnapshot snap{tuning::default_snapshot_path()};
//
//   if (auto ok = tuner->apply_full(snap); !ok) {
//       // snap destructor rolls back whatever was partially applied
//       return ok.error();
//   }
//   (void)snap.save();      // persist for daemon / crash recovery
//   snap.mark_committed();  // suppress RAII rollback
//
//   TuningDaemon daemon;
//   daemon.start_with_snapshot_recovery(*tuner, snap.path());
//
//  TuningConfig + abstract ITuningProvider interface for snapshot-aware tuning.
#pragma once
#include "TuningSnapshot.hpp"
#include <phyriad/schema/Error.hpp>
#include <cstdint>
#include <expected>
#include <memory>

namespace phyriad::tuning {

// ── ScopeRollback<Fn> ─────────────────────────────────────────────────────────
// RAII utility: invokes `fn` on destruction UNLESS commit() was called.
// Used by tuners to ensure partial-apply failures roll back individual ops.
//
//   ScopeRollback rb{[&] { revert_this_change(); }};
//   if (!try_apply_change()) return err;
//   rb.commit();
template <typename Fn>
class ScopeRollback {
    Fn   fn_;
    bool committed_{false};
public:
    explicit ScopeRollback(Fn&& f) noexcept : fn_(std::move(f)) {}
    ~ScopeRollback() noexcept { if (!committed_) fn_(); }

    ScopeRollback(ScopeRollback const&)            = delete;
    ScopeRollback& operator=(ScopeRollback const&) = delete;
    ScopeRollback(ScopeRollback&&)                 = delete;
    ScopeRollback& operator=(ScopeRollback&&)      = delete;

    void commit() noexcept { committed_ = true; }
};

// CTAD helper for lambda-typed rollbacks.
template <typename Fn> ScopeRollback(Fn&&) -> ScopeRollback<Fn>;

// ── TuningConfig ─────────────────────────────────────────────────────────────
// Unified config for both Linux and Windows tuners. Fields with platform
// prefix are honored only by the matching tuner; the other ignores them.
struct TuningConfig {
    // Linux tunings
    bool     apply_hugepages    {true};   // THP defrag → "defer+madvise"
    bool     apply_governor     {true};   // CPU freq governor → "performance"
    bool     disable_cstates    {true};   // Disable C-state 1+
    bool     reroute_irqs       {true};   // Move IRQs off hot cores
    uint32_t irq_target_cpu_mask{0x1u};   // Bitmask of CPUs for IRQ routing

    // Windows tunings
    bool apply_power_scheme    {true};    // Activate High/Ultimate Performance
    bool apply_timer_resolution{true};    // timeBeginPeriod(1) — 1ms timer
    bool apply_working_set     {true};    // SetProcessWorkingSetSize lock-in
    bool disable_priority_boost{true};    // Disable dynamic CPU boost
};

// ── ITuningProvider ──────────────────────────────────────────────────────────
// Abstract base for snapshot-aware platform tuning. The concrete LinuxTuner /
// WindowsTuner classes implement this interface in addition to their direct API
// (`apply()` / `revert()` / `is_applied()`).
class ITuningProvider {
public:
    virtual ~ITuningProvider() noexcept = default;

    // Apply all platform tunings according to cfg. Populates `snapshot` with
    // before/after records for every change. If dry_run, prints intended
    // changes to stderr and returns without modifying the OS. Returns an
    // error on the first failure; the snapshot's RAII destructor reverts any
    // partial changes if the caller does not mark_committed().
    [[nodiscard]] virtual std::expected<void, phyriad::Error>
    apply_full(TuningSnapshot&    snapshot,
               TuningConfig const& cfg     = {},
               bool                dry_run = false) noexcept = 0;

    // Reverse all changes recorded in `snapshot` (LIFO order). Delegates to
    // snapshot.rollback_all() plus any platform bookkeeping.
    virtual void revert_snapshot(TuningSnapshot& snapshot) noexcept = 0;

    // Verify that every sysfs/registry write in `snapshot` still matches the
    // recorded new_value (i.e., the OS has not reverted our settings).
    [[nodiscard]] virtual bool
    verify_snapshot(TuningSnapshot const& snapshot) const noexcept = 0;

    // Re-apply every new_value from `snapshot` without adding new records.
    // Used by TuningDaemon when verify_snapshot() returns false.
    virtual void reapply_snapshot(TuningSnapshot const& snapshot) noexcept = 0;

    // Factory: creates the platform-appropriate concrete tuner.
    // Defined in src/PlatformTunerFactory.cpp (chooses LinuxTuner or WindowsTuner).
    [[nodiscard]] static std::unique_ptr<ITuningProvider> make() noexcept;
};

} // namespace phyriad::tuning
// Made with my soul - Swately <3
