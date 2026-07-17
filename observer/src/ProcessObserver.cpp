// observer/src/ProcessObserver.cpp
// ProcessObserver — platform-agnostic implementation.
//

#include <phynned/observer/ProcessObserver.hpp>
#include <phyriad/process/ProcessEnumerator.hpp>
#include <algorithm>
#include <cstring>
#include <cctype>

namespace phynned::observer {

// ── Constructor / Destructor ──────────────────────────────────────────────
ProcessObserver::ProcessObserver() noexcept {
    targets_.fill(TargetProcess{});
    patterns_.fill(Pattern{});
}

ProcessObserver::~ProcessObserver() noexcept = default;

// ── Pattern management ────────────────────────────────────────────────────
void ProcessObserver::add_target_pattern(const char* pattern) noexcept {
    if (!pattern || n_patterns_ >= kMaxPatterns) return;
    // Check for duplicate
    for (uint32_t i = 0u; i < n_patterns_; ++i) {
        if (patterns_[i].active &&
            std::strncmp(patterns_[i].text, pattern, sizeof(Pattern::text)) == 0) {
            return;  // already registered
        }
    }
    // Find a free slot (inactive pattern or past end)
    for (uint32_t i = 0u; i < n_patterns_; ++i) {
        if (!patterns_[i].active) {
            std::strncpy(patterns_[i].text, pattern, sizeof(Pattern::text) - 1u);
            patterns_[i].text[sizeof(Pattern::text) - 1u] = '\0';
            patterns_[i].active = true;
            return;
        }
    }
    std::strncpy(patterns_[n_patterns_].text, pattern, sizeof(Pattern::text) - 1u);
    patterns_[n_patterns_].text[sizeof(Pattern::text) - 1u] = '\0';
    patterns_[n_patterns_].active = true;
    ++n_patterns_;
}

void ProcessObserver::remove_target_pattern(const char* pattern) noexcept {
    for (uint32_t i = 0u; i < n_patterns_; ++i) {
        if (patterns_[i].active &&
            std::strncmp(patterns_[i].text, pattern, sizeof(Pattern::text)) == 0) {
            patterns_[i].active = false;
            return;
        }
    }
}

void ProcessObserver::clear_target_patterns() noexcept {
    for (uint32_t i = 0u; i < n_patterns_; ++i)
        patterns_[i].active = false;
    n_patterns_ = 0u;
}

// ── Snapshot ──────────────────────────────────────────────────────────────
uint32_t ProcessObserver::snapshot(TargetProcess* out, uint32_t max) const noexcept {
    const uint32_t n = std::min(n_targets_, max);
    std::memcpy(out, targets_.data(), n * sizeof(TargetProcess));
    return n;
}

uint32_t ProcessObserver::target_pids(uint32_t* out_pids, uint32_t max) const noexcept {
    const uint32_t n = std::min(n_targets_, max);
    for (uint32_t i = 0u; i < n; ++i)
        out_pids[i] = targets_[i].pid;
    return n;
}

// ── Pattern matching (case-insensitive substring) ─────────────────────────
bool ProcessObserver::matches_any_pattern(const char* exe_name) const noexcept {
    // Lowercase copy of exe_name for matching
    char lower[64]{};
    for (uint32_t i = 0u; i < sizeof(lower) - 1u && exe_name[i]; ++i)
        lower[i] = static_cast<char>(std::tolower(
            static_cast<unsigned char>(exe_name[i])));

    for (uint32_t i = 0u; i < n_patterns_; ++i) {
        if (!patterns_[i].active) continue;
        // Lowercase the pattern for comparison
        char pat_lower[40]{};
        for (uint32_t j = 0u; j < sizeof(pat_lower) - 1u && patterns_[i].text[j]; ++j)
            pat_lower[j] = static_cast<char>(std::tolower(
                static_cast<unsigned char>(patterns_[i].text[j])));

        if (std::strstr(lower, pat_lower) != nullptr) return true;
    }
    return false;
}

// ── Target table management ───────────────────────────────────────────────
// Per-exe-name cap: many modern processes (Chrome, VS Code, Discord,
// msedgewebview2) spawn 5-15 helpers/tabs with the same .exe name. Without
// a cap they fill all kMaxTargets slots before the real game target
// (e.g. javaw.exe with a high PID) is detected.
//
// Policy: track up to `kMaxPerName=8` entries per unique exe-name.
// Cap raised over time:
//   - 2 (initial): sufficient for Chrome/Code/Edge
//   - 4 (bug #15): to support 3+ OBS instances in multi-stream setups
//   - 8 (bug #17): for Discord which spawns 6+ helpers (multi-stream test
//     Twitch+Discord — 2 late Discord helpers were escaping the pin)
//
// With cap=8 and kMaxTargets=32, up to 4 fully-occupied unique exe names fit.
// Typical streamer: javaw(2) + obs64(1) + Discord(6-8) + Chrome(several) +
// Code(several) = ~20-25 slots used, leaving headroom.
void ProcessObserver::update_target(uint32_t pid, uint32_t parent_pid,
                                     const char* exe_name) noexcept {
    static constexpr uint32_t kMaxPerName = 8u;

    // Check if already tracked + count how many with same name.
    uint32_t same_name_count = 0u;
    for (uint32_t i = 0u; i < n_targets_; ++i) {
        if (targets_[i].pid == pid) {
            targets_[i].status = TargetStatus::Running;
            return;  // already known — refresh status only
        }
        if (std::strncmp(targets_[i].name, exe_name, sizeof(targets_[0].name)) == 0) {
            ++same_name_count;
        }
    }

    // Drop if we already have kMaxPerName instances of this exe.
    if (same_name_count >= kMaxPerName) return;

    // Drop if global cap reached.
    if (n_targets_ >= kMaxTargets) return;

    TargetProcess& t = targets_[n_targets_++];
    t.pid        = pid;
    t.parent_pid = parent_pid;
    t.start_tsc  = 0u;  // filled by MetricsCollector on first sample
    t.status     = TargetStatus::Running;
    t.kind       = TargetKind::Unknown;  // classified by AutoPolicySelector
    t.rules_matched = 0u;
    std::strncpy(t.name, exe_name, sizeof(t.name) - 1u);
    t.name[sizeof(t.name) - 1u] = '\0';
}

void ProcessObserver::expire_dead_targets() noexcept {
    // Compact: remove any target not refreshed this cycle (status != Running)
    uint32_t write = 0u;
    for (uint32_t i = 0u; i < n_targets_; ++i) {
        if (targets_[i].status == TargetStatus::Running) {
            if (write != i) targets_[write] = targets_[i];
            ++write;
        }
    }
    n_targets_ = write;
}

// ── refresh() — cross-platform via FR-4 ──────────────────────────────────
// Migrated from platform-specific CreateToolhelp32Snapshot
// (Windows) / /proc walk (Linux) to phyriad::proc::enumerate_processes(). Same
// observable behavior — pattern-match per process, update target table,
// expire dead entries — but using the unified zero-alloc Phyriad API.
//
// Buffer: thread_local to avoid heap (kMaxProcesses × 80 B = ~80 KB on stack
// would be wasteful; static is fine since refresh() is called from a single
// thread). Sized to phyriad::proc::kMaxProcesses (1024) to cover busy systems
// without truncation.
void ProcessObserver::refresh() noexcept {
    // Mark all current targets as Exiting (will be cleared if still alive)
    for (uint32_t i = 0u; i < n_targets_; ++i)
        targets_[i].status = TargetStatus::Exiting;

    static thread_local phyriad::proc::ProcessEntry s_entries[phyriad::proc::kMaxProcesses];
    const uint32_t n = phyriad::proc::enumerate_processes(s_entries,
                                                       phyriad::proc::kMaxProcesses);
    for (uint32_t i = 0u; i < n; ++i) {
        const auto& e = s_entries[i];
        if (matches_any_pattern(e.name)) {
            update_target(e.pid, e.parent_pid, e.name);
        }
    }

    expire_dead_targets();
}

} // namespace phynned::observer
// Made with my soul - Swately <3
