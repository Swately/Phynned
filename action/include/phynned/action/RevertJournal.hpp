// action/include/phynned/action/RevertJournal.hpp
// RevertJournal — crash-durable write-ahead journal of affinity placements (DR1).
//
// The problem (verified [V1] SOTA_SAFETY_COMPAT F17): a CPU-affinity / CPU-Set
// placement is a property of the TARGET process and PERSISTS after the setter
// (the agent) dies. The RAII `~ActionExecutor -> revert_all` does NOT run on
// `taskkill /f` / a crash, so a mass run that placed N processes then got killed
// orphans all N placements. And because revert-to-captured-prev cannot tell "my
// own placement" from "a crashed prior instance's residual", the current code
// deliberately reverts to all-cores (ActionExecutor.hpp:138-154) — which cannot
// restore a process's TRUE prior mask.
//
// This journal is the fix. It records every placement to disk with a write-ahead
// discipline:
//   record_pending(...)  writes a PENDING record and FlushFileBuffers()es it to
//                        the platter BEFORE returning — so the record survives a
//                        crash in the window between this call and the Set*.
//   mark_applied(key)    flips PENDING -> APPLIED after the Set* succeeded.
//   mark_reverted(key)   flips -> REVERTED after a successful revert.
//   recover()            (agent start / dead-man) returns every surviving
//                        PENDING+APPLIED record so the caller can restore each
//                        live process to its CAPTURED prev_mask — reconciled
//                        against the live process by process_still_matches().
//
// Because the journal stores the captured prev_mask AND authoritatively
// distinguishes "my placement" (a live journalled record) from "a crashed
// instance's residual" (a stale record, dropped by the pid-recycle guard),
// revert-to-captured-prev becomes safe. That is the DR1 unlock; the actual
// revert-line change in ActionExecutor is the S3 integration ordered AFTER this
// module (CR1 build-ordering note) and is NOT done here.
//
// Key scheme: records are keyed on (exe-identity + process creation-time), NOT
// raw pid. The creation-time (a FILETIME from GetProcessTimes) is what survives
// pid recycle: a recycled pid points at a process with a DIFFERENT creation-time,
// so a stale record cannot make us clobber an unrelated process. (pid, creation)
// is the RevertKey; exe-name is stored for audit + a second reconciliation check.
//
// Design freedom: Phynned is NOT bound to Phyriad's engineering dogmas
// (framework/VENDORED.md) — std::vector / heap / std::string / std::FILE are all
// fine here; simplest correct code wins.
//
// Threading: single-thread (agent main thread). NOT re-entrant.
// Privilege: file write only; the pid-recycle guard opens targets with
//            PROCESS_QUERY_LIMITED_INFORMATION (the lightest query right).
//
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace phynned::action {

// ── Record lifecycle ──────────────────────────────────────────────────────────
enum class RevertStatus : uint32_t {
    Free     = 0u,  ///< never-written slot (all-zero); never a transition result
    Pending  = 1u,  ///< write-ahead: recorded (and flushed) BEFORE the Set*
    Applied  = 2u,  ///< the Set* succeeded
    Reverted = 3u,  ///< the placement was restored (terminal)
};

[[nodiscard]] const char* to_string(RevertStatus s) noexcept;

// ── RevertKey — the identity handle used to flip a record's status ────────────
// (pid, creation_time) is unique across pid recycle; it is what mark_applied /
// mark_reverted use to locate the record they mutate.
struct RevertKey {
    uint32_t pid;
    uint64_t creation_time;   ///< FILETIME (100ns ticks since 1601-01-01)
};

// ── RevertRecord — one recovered placement (what recover() hands back) ────────
struct RevertRecord {
    uint32_t     pid;
    uint64_t     creation_time;   ///< the pid-recycle discriminator
    uint64_t     prev_mask;       ///< CAPTURED prior affinity — the revert target
    uint64_t     new_mask;        ///< the mask Phynned applied
    uint64_t     wall_time_ms;    ///< when the record was written (ms since epoch)
    RevertStatus status;          ///< Pending or Applied (recover() drops the rest)
    char         exe_name[64];    ///< basename, null-terminated (audit + reconcile)
};

// ── RevertJournal ─────────────────────────────────────────────────────────────
class RevertJournal {
public:
    RevertJournal() noexcept = default;
    ~RevertJournal() noexcept;

    RevertJournal(RevertJournal const&)            = delete;
    RevertJournal& operator=(RevertJournal const&) = delete;

    // ── Lifecycle ─────────────────────────────────────────────────────────────
    /// Default journal path: %LOCALAPPDATA%\Phynned\revert_journal\journal.bin
    /// (POSIX fallback: $HOME/.local/share/phynned/revert_journal/journal.bin).
    /// Creates the containing directory as a side effect.
    [[nodiscard]] static std::string default_path();

    /// Open (or create) the journal at `path`. Rebuilds the in-memory offset
    /// index from any existing on-disk records. Returns true on success.
    [[nodiscard]] bool open(const char* path) noexcept;

    void close() noexcept;
    [[nodiscard]] bool is_open() const noexcept { return file_ != nullptr; }
    [[nodiscard]] const std::string& path() const noexcept { return path_; }

    // ── Write-ahead record API ────────────────────────────────────────────────
    /// Write a PENDING record and FlushFileBuffers() it to disk BEFORE returning.
    /// This is the write-ahead property: the record is durable before the caller
    /// performs the Set*, so a crash in that window cannot orphan an unrecorded
    /// placement. `prev_mask` is the CAPTURED prior affinity (the revert target).
    void record_pending(uint32_t    pid,
                        const char* exe_name,
                        uint64_t    creation_time,
                        uint64_t    prev_mask,
                        uint64_t    new_mask) noexcept;

    /// Flip the record for `key` PENDING -> APPLIED (after the Set* succeeded).
    void mark_applied(const RevertKey& key) noexcept;

    /// Flip the record for `key` -> REVERTED (after a successful revert). Safe to
    /// call on an already-REVERTED record (idempotent no-op).
    void mark_reverted(const RevertKey& key) noexcept;

    // ── Recovery ──────────────────────────────────────────────────────────────
    /// Read the journal and return every surviving PENDING+APPLIED record (the
    /// crash-in-window PENDING case is included deliberately). Deduplicated by
    /// key, keeping the latest on-disk state. REVERTED / Free / torn records are
    /// dropped. Called FIRST on agent start (before the tick loop) and by the
    /// external dead-man; the caller reverts each record whose live process still
    /// matches (see process_still_matches) and marks it reverted.
    [[nodiscard]] std::vector<RevertRecord> recover() noexcept;

    // ── pid-recycle guard ─────────────────────────────────────────────────────
    /// True iff a LIVE process currently has pid == r.pid AND a creation-time
    /// equal to r.creation_time AND (when r.exe_name is set) a matching exe
    /// basename. A recycled pid fails the creation-time check → the record is
    /// stale and must be DROPPED, never reverted (it would clobber an unrelated
    /// process). Opens the target with PROCESS_QUERY_LIMITED_INFORMATION only.
    [[nodiscard]] static bool process_still_matches(const RevertRecord& r) noexcept;

    // ── Helpers (also usable by callers / the dead-man / tests) ───────────────
    /// Creation-time (packed FILETIME) of a live pid, or 0 if unavailable.
    [[nodiscard]] static uint64_t query_creation_time(uint32_t pid) noexcept;
    /// Exe basename of a live pid into `out` (bounded). Returns true on success.
    [[nodiscard]] static bool query_exe_basename(uint32_t pid,
                                                 char* out, std::size_t n) noexcept;
    /// Wall-clock milliseconds since the Unix epoch.
    [[nodiscard]] static uint64_t now_wall_ms() noexcept;

private:
    // In-memory offset index: the latest on-disk byte offset per key. Speeds the
    // live mark_* path; the dead-man (fresh instance) rebuilds it from disk in
    // open(), and mark_* falls back to a file scan if a key is absent.
    struct IndexEntry {
        uint32_t pid;
        uint64_t creation_time;
        long     offset;
    };

    void   flush_durable() noexcept;
    long   append_record(const void* rec) noexcept;      // returns byte offset
    void   rebuild_index() noexcept;
    long   find_offset(const RevertKey& key) noexcept;   // -1 if not found
    void   set_status(const RevertKey& key, RevertStatus to) noexcept;

    std::FILE*              file_ = nullptr;
    std::string             path_;
    std::vector<IndexEntry> index_;
};

} // namespace phynned::action
// Made with my soul - Swately <3
