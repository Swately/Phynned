// apps/ayama/observer/include/ayama/observer/ClassificationCache.hpp
// ClassificationCache — TTL wrapper around ProcessClassifier.
//
// Re-verifies classification every 60 seconds; trusts cache otherwise.
// Linear search is intentional: N < 128 fits in L1, ~16 ns worst case.
//
// Usage:
//   ClassificationCache cache;
//   cache.set_classifier(&my_classifier);
//   cache.set_tsc_freq(phyriad::hal::calibrate_tsc_freq());
//
//   // Each tick:
//   TargetKind k = cache.classify_cached(exe_name, fresh_info, now_tsc);
//
// Threading: single-thread (agent main thread).
// Resource:  ~7 KB for 128-entry table; no heap.
// Privilege: None (privilege needed only by classifier's check_d3d_vk).
//
#pragma once

#include <ayama/observer/TargetProcess.hpp>
#include <ayama/observer/ProcessClassifier.hpp>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <array>

namespace ayama::observer {

// ── Cache entry — 64 bytes ───────────────────────────────────────────────────
// PID added as primary key. Previously the cache was keyed by
// exe_name alone, which collided for processes that share an exe name but
// behave differently. Concrete case: Hogwarts Legacy ships TWO processes
// named `HogwartsLegacy.exe` — a 1-thread launcher stub + the real 294-
// thread game. Cache-by-name made the launcher's classification (Unknown
// due to 1 thread) "stick" for both processes, hiding the real game.
struct alignas(8) ClassificationCacheEntry {
    uint32_t   pid;                  //  4B — primary key
    uint32_t   _pad0;                //  4B
    char       exe_name[40];         // 40B — diagnostic + evict_by_name
    TargetKind kind;                 //  1B
    uint8_t    _pad[3];              //  3B
    uint64_t   last_verified_tsc;    //  8B — TSC timestamp of last verify
};                                   // = 60B → padded to 64B
static_assert(sizeof(ClassificationCacheEntry) == 64u);

// ── ClassificationCache ──────────────────────────────────────────────────────
class ClassificationCache {
public:
    static constexpr uint32_t kMaxEntries    = 128u;
    static constexpr uint64_t kTtlSeconds    = 60u;   // re-verify after 60s

    ClassificationCache() noexcept = default;

    ClassificationCache(const ClassificationCache&)            = delete;
    ClassificationCache& operator=(const ClassificationCache&) = delete;

    /// Set the underlying classifier. Must be called before classify_cached().
    void set_classifier(ProcessClassifier* c) noexcept { classifier_ = c; }

    /// Set the TSC frequency (Hz) for TTL computation.
    void set_tsc_freq(uint64_t freq) noexcept {
        tsc_freq_ = (freq > 0u) ? freq : 2'000'000'000ull;
    }

    /// Classify with caching. If cache hit and within TTL: return cached kind.
    /// Otherwise: call classifier_, update cache, return fresh kind.
    ///
    /// **Monotonic policy:** Once classified as Game/Stream/
    /// Comm/Browser/Productivity, NEVER demoted to Unknown on re-verify.
    /// Reason: the classifier depends on `is_foreground` and `foreground_for_sec`
    /// which are invalidated the moment the user alt-tabs away from the app —
    /// causing a spurious downgrade. Process identity (loaded DLLs, exe_name)
    /// is stable; downgrading "Game→Unknown" because the user went to cmd is
    /// incorrect. If the app is truly not a game, the classifier would have
    /// detected it as Browser/Comm/etc. directly by name.
    [[nodiscard]] TargetKind classify_cached(uint32_t           pid,
                                             const char*        exe_name,
                                             const ProcessInfo& fresh_info,
                                             uint64_t           now_tsc) noexcept
    {
        if (classifier_ == nullptr) return TargetKind::Unknown;

        // Linear search by PID (primary key). Fits in L1 for n < 128.
        for (uint32_t i = 0u; i < n_entries_; ++i) {
            auto& e = entries_[i];
            if (e.pid == pid) {
                // Cache hit. TTL is 60s for stable kinds, but only 5s for
                // Unknown entries — so newly-observed processes get re-checked
                // quickly as metrics warm up (cpu_usage_pct is 0 on the first
                // sample because the delta needs a prior measurement).
                const uint64_t ttl_sec =
                    (e.kind == TargetKind::Unknown) ? 5ull : kTtlSeconds;
                const uint64_t ttl_tsc = tsc_freq_ * ttl_sec;
                if (now_tsc - e.last_verified_tsc < ttl_tsc) {
                    return e.kind;   // Within TTL → trust.
                }
                // Stale → re-verify.
                const TargetKind fresh = classifier_->classify(fresh_info);

                // Monotonic upgrade policy: prefer the STRONGER classification.
                // Order of "strength" (most specific → most useful for Ayama):
                //   System > Stream > Comm > Game > Browser > Productivity > Unknown
                // In practice: if cached is != Unknown and fresh is Unknown,
                // PRESERVE cached. This avoids the "alt-tab downgrade" bug.
                if (e.kind != TargetKind::Unknown &&
                    e.kind != TargetKind::System &&
                    fresh   == TargetKind::Unknown)
                {
                    // Keep prior classification; just refresh timestamp.
                    e.last_verified_tsc = now_tsc;
                    return e.kind;
                }

                // Diagnostic log: print ONLY on kind transitions. Previously
                // we also logged every Unknown-stays-Unknown re-verify, but
                // with ~30 msedgewebview2 / Discord helpers / steamservice
                // PIDs that get re-classified every 5s, the log volume
                // drowned all relevant output. A stuck-Unknown target
                // is rare enough that you can investigate it on demand
                // (read e.kind via UI) instead of stdout-spamming.
                static constexpr const char* kKindNames[] = {
                    "Unknown", "Game", "Stream", "Comm",
                    "Browser", "Productivity", "System"
                };
                const bool kind_changed = (fresh != e.kind);
                if (kind_changed) {
                    const uint32_t pi = static_cast<uint32_t>(e.kind);
                    const uint32_t ni = static_cast<uint32_t>(fresh);
                    const char* prev = (pi < 7u) ? kKindNames[pi] : "?";
                    const char* next = (ni < 7u) ? kKindNames[ni] : "?";
                    std::fprintf(stdout,
                        "[Ayama][Classify] %-32s [pid=%u] | %s → %s | "
                        "fs=%d fg=%us cpu=%.1f%% threads=%u d3d=%d\n",
                        exe_name, pid, prev, next,
                        fresh_info.is_fullscreen ? 1 : 0,
                        fresh_info.foreground_for_sec,
                        static_cast<double>(fresh_info.cpu_usage_pct),
                        fresh_info.thread_count,
                        fresh_info.uses_d3d_or_vk ? 1 : 0);
                }

                e.kind             = fresh;
                e.last_verified_tsc = now_tsc;
                return e.kind;
            }
        }

        // Not cached → classify + insert.
        const TargetKind k = classifier_->classify(fresh_info);

        if (n_entries_ < kMaxEntries) {
            auto& e = entries_[n_entries_++];
            e.pid = pid;                           // primary key
            std::strncpy(e.exe_name, exe_name, sizeof(e.exe_name) - 1u);
            e.exe_name[sizeof(e.exe_name) - 1u] = '\0';
            e.kind             = k;
            e.last_verified_tsc = now_tsc;
        }

        // Diagnostic log: first classification per PID. Skip Unknown
        // (boring; helpers/launchers/system procs). Only interesting kinds
        // (Game/Stream/Comm/Browser/Productivity) get a one-line log.
        if (k != TargetKind::Unknown) {
            static constexpr const char* kKindNames[] = {
                "Unknown", "Game", "Stream", "Comm",
                "Browser", "Productivity", "System"
            };
            const uint32_t ki = static_cast<uint32_t>(k);
            const char* kname = (ki < 7u) ? kKindNames[ki] : "?";
            std::fprintf(stdout,
                "[Ayama][Classify] %-32s [pid=%u] → %-12s | "
                "fs=%d fg=%us cpu=%.1f%% threads=%u d3d=%d\n",
                exe_name, pid, kname,
                fresh_info.is_fullscreen ? 1 : 0,
                fresh_info.foreground_for_sec,
                static_cast<double>(fresh_info.cpu_usage_pct),
                fresh_info.thread_count,
                fresh_info.uses_d3d_or_vk ? 1 : 0);
        }

        return k;
    }

    /// Pure cache lookup, no classification.
    /// Used by AgentRuntime on non-classify ticks to recover the kind without
    /// rebuilding ProcessInfo. Cheap O(n) over ≤128 entries.
    /// Keyed by PID (post-cache-by-pid refactor); falls back to exe_name
    /// match for legacy callers that don't have a PID (rare).
    [[nodiscard]] TargetKind lookup_kind(uint32_t pid,
                                          const char* exe_name = nullptr) const noexcept {
        for (uint32_t i = 0u; i < n_entries_; ++i) {
            if (entries_[i].pid == pid) return entries_[i].kind;
        }
        if (exe_name) {
            for (uint32_t i = 0u; i < n_entries_; ++i) {
                if (std::strcmp(entries_[i].exe_name, exe_name) == 0)
                    return entries_[i].kind;
            }
        }
        return TargetKind::Unknown;
    }

    /// Evict a specific exe name from the cache. Removes ALL entries matching
    /// the name (since post-PID-refactor, multiple entries may share a name).
    void evict(const char* exe_name) noexcept {
        uint32_t w = 0u;
        for (uint32_t i = 0u; i < n_entries_; ++i) {
            if (std::strcmp(entries_[i].exe_name, exe_name) != 0) {
                if (w != i) entries_[w] = entries_[i];
                ++w;
            }
        }
        n_entries_ = w;
    }

    /// Evict the entry matching this PID (called when target disappears).
    void evict_pid(uint32_t pid) noexcept {
        for (uint32_t i = 0u; i < n_entries_; ++i) {
            if (entries_[i].pid == pid) {
                entries_[i] = entries_[--n_entries_];
                return;
            }
        }
    }

    /// Clear all cache entries.
    void clear() noexcept { n_entries_ = 0u; }

    [[nodiscard]] uint32_t entry_count() const noexcept { return n_entries_; }

private:
    alignas(64) std::array<ClassificationCacheEntry, kMaxEntries> entries_{};
    uint32_t          n_entries_  {0u};
    uint64_t          tsc_freq_   {2'000'000'000ull};
    ProcessClassifier* classifier_{nullptr};
};

} // namespace ayama::observer
// Made with my soul - Swately <3
