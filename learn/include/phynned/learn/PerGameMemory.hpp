// learn/include/phynned/learn/PerGameMemory.hpp
// PerGameMemory — persist and recall per-game learned optimisations.
//
// Stores a table of (exe, hardware_id) → LearnedEntry results so that
// the agent can immediately apply the best-known policy on a game's next run,
// skipping the 30s A/B discovery phase.
//
// Persistence:  %LOCALAPPDATA%\Phynned\memory.toml
// Format:       TOML (hand-parsed, same subset as ConfigStore)
// Capacity:     up to kMaxEntries = 256 game entries
//
// Lifecycle:
//   1. load()         — at agent start; failures are non-fatal (empty table)
//   2. find(exe)      — at target detection; instant lookup (linear scan)
//   3. upsert(entry)  — after a successful A/B run validates a policy
//   4. save()         — after upsert and on clean shutdown
//
// The hardware_id is generated once at startup via generate_hardware_id()
// using CPU topology (AMD X3D / Intel Hybrid detection).
//
// Threading: single-thread (agent main thread).
// Resource:  256 × 192B = ~48 KB fixed allocation; no heap after load().
// Privilege: file system read/write only.
//
#pragma once

#include <phynned/learn/LearnedEntry.hpp>
#include <phyriad/schema/Error.hpp>

#include <cstdint>
#include <cstring>
#include <expected>

namespace phynned::learn {

// ── BadEntry — regression blacklist ─────────────────────────────────────────
/// Entry in the bad-list: exes where Phynned detected a regression.
struct alignas(8) BadEntry {
    char    exe[64];            // 64B — null-terminated exe name
    char    reason[32];         // 32B — "regression_detected", "user_rejected", …
    char    last_attempted[32]; // 32B — ISO-8601 timestamp (approximate)
};                              // = 128B
static_assert(sizeof(BadEntry) == 128u);

class PerGameMemory {
public:
    static constexpr uint32_t kMaxEntries    = 256u;
    static constexpr uint32_t kMaxBadEntries = 64u;
    static constexpr uint32_t kVersion       = 1u;

    PerGameMemory() noexcept = default;

    PerGameMemory(PerGameMemory const&)            = delete;
    PerGameMemory& operator=(PerGameMemory const&) = delete;

    // ── Hardware fingerprint ──────────────────────────────────────────────
    /// Generate and cache the hardware ID for this machine.
    /// Detects AMD X3D, Intel Hybrid, or generic CPU class.
    /// Must be called once before load()/upsert().
    void generate_hardware_id() noexcept;

    [[nodiscard]] const char* hardware_id() const noexcept {
        return hardware_id_;
    }

    // ── Persistence ───────────────────────────────────────────────────────
    /// Load memory.toml from `path`. If path is nullptr, uses the default.
    /// Non-fatal: if the file does not exist, starts with an empty table.
    [[nodiscard]] std::expected<void, phyriad::Error>
    load(const char* path = nullptr) noexcept;

    /// Save memory.toml to `path`. If path is nullptr, uses the default.
    /// Creates the %LOCALAPPDATA%\Phynned\ directory if it does not exist.
    [[nodiscard]] std::expected<void, phyriad::Error>
    save(const char* path = nullptr) noexcept;

    // ── Table access ──────────────────────────────────────────────────────
    /// Find the learned entry for the given executable name and the current
    /// hardware_id. Case-insensitive on Windows. Returns nullptr if not found.
    [[nodiscard]] const LearnedEntry* find(const char* exe) const noexcept;
    [[nodiscard]] LearnedEntry*       find(const char* exe)       noexcept;

    /// Insert or update an entry. Matching is by (exe, hardware_id).
    /// Silently drops the entry if the table is full.
    void upsert(const LearnedEntry& e) noexcept;

    /// Remove entry by exe name. No-op if not found.
    void remove(const char* exe) noexcept;

    // ── Bad-list management ───────────────────────────────────────────────
    /// Mark an exe as bad — Phynned will not re-apply policies to it.
    /// `reason` may be "regression_detected", "user_rejected", etc.
    void mark_bad(const char* exe, const char* reason = "regression_detected") noexcept;

    /// Returns true if this exe is on the bad-list.
    [[nodiscard]] bool is_bad(const char* exe) const noexcept;

    /// Remove an exe from the bad-list (e.g. user requests re-try).
    void clear_bad(const char* exe) noexcept;

    /// Clear entire bad-list.
    void clear_all_bad() noexcept;

    [[nodiscard]] uint32_t        bad_count()         const noexcept { return n_bad_; }
    [[nodiscard]] const BadEntry* bad_entry(uint32_t i) const noexcept {
        return (i < n_bad_) ? &bad_[i] : nullptr;
    }

    // ── Re-validation strategy (§8.3) ────────────────────────────────────
    /// Returns true if `e.last_validated` is more than `max_age_days` old
    /// (compared to the current system date), or if the timestamp is missing/
    /// unparseable.  Conservative: returns true on parse failure.
    ///
    /// The caller should treat stale entries as unconfirmed: run a new A/B
    /// test rather than applying the cached policy blindly.
    [[nodiscard]] static bool needs_revalidation(
        const LearnedEntry& e,
        uint32_t            max_age_days = 30u) noexcept;

    /// Expire all entries older than `max_age_days` by zeroing their
    /// `sample_count` and clearing `best_action`. The exe key is preserved
    /// so the entry is reused when the game runs again.
    /// Returns the number of entries invalidated.
    uint32_t expire_stale_entries(uint32_t max_age_days = 30u) noexcept;

    // ── Iteration ────────────────────────────────────────────────────────
    [[nodiscard]] uint32_t           count()         const noexcept { return n_entries_; }
    [[nodiscard]] const LearnedEntry* entry(uint32_t i) const noexcept {
        return (i < n_entries_) ? &entries_[i] : nullptr;
    }

private:
    char         hardware_id_[64] {};
    uint32_t     version_         {kVersion};
    uint32_t     n_entries_       {0u};
    uint32_t     n_bad_           {0u};
    LearnedEntry entries_[kMaxEntries] {};
    BadEntry     bad_[kMaxBadEntries]  {};

    [[nodiscard]] bool resolve_path(char* out, uint32_t max_len,
                                    const char* override_path) const noexcept;

    [[nodiscard]] bool exe_match(const char* a, const char* b) const noexcept;
};

} // namespace phynned::learn
// Made with my soul - Swately <3
