// framework/tuning/include/phyriad/tuning/TuningSnapshot.hpp
// Persistent rollback record for OS tuning operations.
//
// Implements three-level rollback strategy:
//
//   Level 1 — In-memory RAII
//     TuningSnapshot destructor calls rollback_all() unless mark_committed() was
//     called. Protects against partial-failure during apply().
//
//   Level 2 — Persistent snapshot
//     save() atomically writes the snapshot to disk (temp file + rename).
//     On clean shutdown, mark_committed() + snapshot deletion is expected.
//     If the process crashes, the file remains for daemon recovery.
//
//   Level 3 — Boot-time daemon
//     TuningDaemon::start_with_snapshot_recovery() reads the snapshot path
//     on startup. If a file is present it is treated as an orphaned session
//     snapshot and rolled back automatically before applying fresh tunings.
//
// Usage (normal apply path):
//   TuningSnapshot snap{tuning::default_snapshot_path()};
//   auto ok = tuner->apply(snap);          // tuner populates snap
//   if (!ok) return;                       // RAII: snap destructor rolls back
//   snap.save();                           // persist for daemon / crash recovery
//   snap.mark_committed();                 // suppress RAII rollback
//
// Usage (daemon orphan recovery):
//   auto snap = TuningSnapshot::load(path).value();
//   snap.rollback_all();                   // restores OS state
//   // snap destructor is now a no-op (rollback_all sets committed)
//
// File format: 64-byte header (magic="TUNG", version=1, record_count) + N × 384B TuningRecord.
//
//  rollback is essential production safety for OS tuning operations.
#pragma once
#include <phyriad/schema/Error.hpp>
#include <phyriad/hal/Cacheline.hpp>
#include <phyriad/hal/MemoryOrder.hpp>
#include <atomic>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <string_view>
#include <type_traits>
#include <vector>

namespace phyriad::tuning {

// ── TuningOp ─────────────────────────────────────────────────────────────────
// Operation tag — determines how rollback_all() reverses a record.
enum class TuningOp : uint8_t {
    SysfsWrite   = 0,  // Linux: write string to sysfs/procfs file
    WinRegistry  = 1,  // Windows: write REG_SZ to HKLM registry value
    WinPowercfg  = 2,  // Windows: restore active power scheme GUID via powercfg
    WinTimer     = 3,  // Windows: timeEndPeriod(old_period_ms)
    WinWorkSet   = 4,  // Windows: SetProcessWorkingSetSize(old_min, old_max)
    WinPrioBoost = 5,  // Windows: SetProcessPriorityBoost(old_state)
};

// ── TuningRecord ─────────────────────────────────────────────────────────────
// Fixed-size POD describing a single tuning change.
// sizeof == 384 — direct binary serialisation without padding overhead.
struct TuningRecord {
    TuningOp op{};
    uint8_t  _pad[3]{};
    char     target[252]{};    // sysfs path OR registry "KEY:ValueName"
    char     old_value[64]{};  // value before the change (used by rollback)
    char     new_value[64]{};  // value after the change (used by daemon re-verify)
};
static_assert(sizeof(TuningRecord) == 384,
    "TuningRecord must be exactly 384B for direct binary serialisation");
static_assert(std::is_trivially_copyable_v<TuningRecord>);

// ── default_snapshot_path ────────────────────────────────────────────────────
// Cross-platform default location for the snapshot file.
//   Windows: %ProgramData%\Phyriad\tuning_snapshot.bin
//   POSIX:   /var/lib/phyriad/tuning_snapshot.bin (may require root to write)
[[nodiscard]] std::filesystem::path default_snapshot_path() noexcept;

// ── TuningSnapshot ───────────────────────────────────────────────────────────
class TuningSnapshot {
public:
    explicit TuningSnapshot(std::filesystem::path path) noexcept;
    ~TuningSnapshot() noexcept;

    TuningSnapshot(TuningSnapshot const&)            = delete;
    TuningSnapshot& operator=(TuningSnapshot const&) = delete;
    TuningSnapshot(TuningSnapshot&& o) noexcept;
    TuningSnapshot& operator=(TuningSnapshot&& o) noexcept;

    // Append a record (called by platform tuners after each successful change).
    void add_record(TuningOp           op,
                    std::string_view   target,
                    std::string_view   old_value,
                    std::string_view   new_value) noexcept;

    // Atomically persist to disk (writes .tmp then atomic-rename).
    [[nodiscard]] std::expected<void, phyriad::Error> save() const noexcept;

    // Load and validate a snapshot file.
    // Returns IoError if file is missing; SchemaMismatch if corrupt.
    // The returned snapshot is NOT committed — caller must explicitly call
    // mark_committed() or rollback_all().
    [[nodiscard]] static std::expected<TuningSnapshot, phyriad::Error>
    load(std::filesystem::path path) noexcept;

    // Reverse all changes in LIFO order. Automatically sets committed=true to
    // prevent double-rollback.
    void rollback_all() noexcept;

    // Suppress the RAII rollback (tunings should remain active after this call).
    void mark_committed() noexcept;

    [[nodiscard]] bool                              is_committed()  const noexcept;
    [[nodiscard]] std::vector<TuningRecord> const&  records()       const noexcept;
    [[nodiscard]] std::filesystem::path     const&  path()          const noexcept;

private:
    std::vector<TuningRecord>   records_{};
    std::filesystem::path       path_{};
    alignas(hal::kDestructivePad) std::atomic<bool> committed_{false};
};

} // namespace phyriad::tuning
// Made with my soul - Swately <3
