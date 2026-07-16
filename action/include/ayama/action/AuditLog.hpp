// apps/ayama/action/include/ayama/action/AuditLog.hpp
// AuditLog — append-only binary audit trail for all actions applied/reverted.
//
// Written by AgentRuntime each tick by draining new entries from the
// ActionExecutor's ActionLogRing and persisting them to disk.
//
// File format: sequence of AuditRecord (96 bytes each), no header.
// Readers use file size / 96 to determine record count.
// The file is appended to — never overwritten. On process start, the
// cursor begins at 0; old entries accumulate indefinitely until the user
// runs `ayama-cli memory clear-audit` (not yet implemented).
//
// Default path (resolved by AgentRuntime via ConfigStore):
//   Windows:  %LOCALAPPDATA%\Ayama\audit.bin
//   Linux:    ~/.config/ayama/audit.bin
//
// Threading: not thread-safe; call from the agent main thread only.
// Privilege: None (file write only).
// Resource:  one file handle; 96B per action (100 actions/day ≈ 9.6 KB/day).
//
#pragma once

#include <ayama/action/ActionLog.hpp>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>   // GetSystemTimeAsFileTime, GetCurrentProcessId
#endif

namespace ayama::action {

// ── AuditRecord — one persisted action event ─────────────────────────────────

/// One persisted audit record — 96 bytes.
/// Aligned to 8B so file position is always a multiple of 8.
struct alignas(8) AuditRecord {
    // ── Timestamps ────────────────────────────────────────────────────────
    uint64_t wall_time_ms;      //  8B  — wall-clock milliseconds since Unix epoch
    uint64_t tsc_applied;       //  8B  — TSC when the action was applied
    uint64_t tsc_reverted;      //  8B  — 0 if still active

    // ── Target ────────────────────────────────────────────────────────────
    uint32_t target_pid;        //  4B
    uint32_t rule_id;           //  4B

    // ── Affinity / priority ───────────────────────────────────────────────
    uint64_t prev_affinity_mask;//  8B
    uint64_t new_affinity_mask; //  8B
    uint32_t prev_priority_class; //4B
    uint32_t new_priority_class;  //4B

    // ── Event classification ──────────────────────────────────────────────
    char     event_type[8];     //  8B  — "APPLY\0\0\0" or "REVERT\0\0"
    uint8_t  success;           //  1B

    // ── Agent metadata ────────────────────────────────────────────────────
    uint32_t agent_pid;         //  4B  — agent's own PID (identifies session)
    uint8_t  _pad[19];          // 19B  — fill to 96B
};
static_assert(sizeof(AuditRecord) == 96u, "AuditRecord must be 96B");
static_assert(alignof(AuditRecord) == 8u);

// ── AuditLog ─────────────────────────────────────────────────────────────────

/// Append-only binary audit log for persisting ActionLogEntries to disk.
class AuditLog {
public:
    AuditLog() noexcept = default;
    ~AuditLog() noexcept { close(); }

    AuditLog(AuditLog const&)            = delete;
    AuditLog& operator=(AuditLog const&) = delete;

    // ── Lifecycle ─────────────────────────────────────────────────────────

    /// Open `path` in append+binary mode. Creates file if not present.
    /// `agent_pid` is stored in every record to identify the session.
    /// Returns true on success.
    [[nodiscard]] bool open(const char* path, uint32_t agent_pid) noexcept {
        if (file_) return true;   // already open

#ifdef _WIN32
        if (fopen_s(&file_, path, "ab") != 0) file_ = nullptr;
#else
        file_ = std::fopen(path, "ab");
#endif
        if (!file_) {
            std::fprintf(stderr,
                "[AuditLog] Cannot open '%s' for append.\n", path);
            return false;
        }
        agent_pid_ = agent_pid;
        std::fprintf(stdout, "[AuditLog] Opened: %s\n", path);
        return true;
    }

    /// Close and flush. Safe to call on a log that was never opened.
    void close() noexcept {
        if (file_) {
            std::fflush(file_);
            std::fclose(file_);
            file_ = nullptr;
        }
    }

    [[nodiscard]] bool is_open() const noexcept { return file_ != nullptr; }

    // ── Write ─────────────────────────────────────────────────────────────

    /// Persist one ActionLogEntry as an AuditRecord.
    /// `event_type` should be "APPLY" or "REVERT".
    void write(const ActionLogEntry& e, const char* event_type) noexcept {
        if (!file_) return;

        AuditRecord rec{};
        rec.wall_time_ms         = current_wall_time_ms();
        rec.tsc_applied          = e.tsc_applied;
        rec.tsc_reverted         = e.tsc_reverted;
        rec.target_pid           = e.target_pid;
        rec.rule_id              = e.rule_id;
        rec.prev_affinity_mask   = e.prev_affinity_mask;
        rec.new_affinity_mask    = e.new_affinity_mask;
        rec.prev_priority_class  = e.prev_priority_class;
        rec.new_priority_class   = e.new_priority_class;
        rec.success              = e.success;
        rec.agent_pid            = agent_pid_;

        // Copy event type (up to 7 chars + null).
        std::strncpy(rec.event_type, event_type, 7);
        rec.event_type[7] = '\0';

        std::fwrite(&rec, sizeof(rec), 1u, file_);
        // No flush per write — OS write-back is fine for audit (fclose flushes all).
    }

    /// Drain new entries from `ring` since `*cursor` and write each to disk.
    /// Uses the multi-reader write_cursor()/peek_at() API so this cursor is
    /// fully independent from AyamaAgentPublisher's cursor.
    /// Updates `*cursor`.  Returns number of entries written.
    uint32_t drain_and_write(const ActionLogRing& ring, uint64_t* cursor) noexcept {
        if (!file_ || !cursor) return 0u;

        const uint64_t head = ring.write_cursor();
        uint32_t written = 0u;

        while (*cursor < head) {
            // Guard against overrun: skip entries the producer has wrapped over.
            if (head - *cursor > kActionLogCap) {
                *cursor = head - kActionLogCap;
            }
            ActionLogEntry e{};
            if (ring.peek_at(*cursor, e) && e.target_pid != 0u) {
                const char* ev = (e.tsc_reverted != 0u) ? "REVERT" : "APPLY";
                write(e, ev);
                ++written;
            }
            ++(*cursor);
        }
        return written;
    }

private:
    std::FILE* file_      {nullptr};
    uint32_t   agent_pid_ {0u};
    uint64_t   cursor_    {0ull};   // internal cursor (not always used)

    /// Current wall-clock time in milliseconds since Unix epoch.
    static uint64_t current_wall_time_ms() noexcept {
#ifdef _WIN32
        FILETIME ft{};
        GetSystemTimeAsFileTime(&ft);
        // FILETIME is in 100ns intervals since 1601-01-01.
        // Unix epoch offset: 11644473600 seconds.
        uint64_t t = (static_cast<uint64_t>(ft.dwHighDateTime) << 32) |
                      ft.dwLowDateTime;
        t /= 10000u;          // 100ns → ms
        t -= 11644473600000ull; // subtract epoch offset
        return t;
#else
        struct timespec ts{};
        clock_gettime(CLOCK_REALTIME, &ts);
        return static_cast<uint64_t>(ts.tv_sec) * 1000u +
               static_cast<uint64_t>(ts.tv_nsec) / 1'000'000u;
#endif
    }
};

} // namespace ayama::action
// Made with my soul - Swately <3
