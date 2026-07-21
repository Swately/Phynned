// observer/include/phynned/observer/MetricsCollector.hpp
// MetricsCollector — non-invasive performance sampling for observed processes.
//
// For each target PID, computes TargetMetrics using:
//   1. phyriad::proc::ProcessMetricsSnapshot (FR-11) — one NtQSI syscall for ALL
//      processes → kernel/user CPU time, working set, thread count.
//      Replaces the old per-PID OpenProcess + GetProcessTimes loop (~10× faster
//      at 32 targets).
//   2. ETW context-switch events — per-thread CPU migration tracking (ring).
//   3. PDH GPU counters — GPU usage % per PID (PDH access, no admin needed).
//   4. PresentMon ETW provider — frame timing for D3D/Vulkan apps (admin).
//
// Migration note: GetProcessTimes per-PID removed in favour of FR-11.
//   Per-PID: N × OpenProcess + GetProcessTimes ≈ 300 µs for 32 targets.
//   Bulk:    1 × NtQSI capture ≈ 80-150 µs, then extract ≈ 600 ns. ~10× gain.
//
// Threading: single-thread (agent main thread). ETW callback runs on ETW
//            thread but only pushes to a lock-free ring.
// Resource:  ProcessMetricsSnapshot has one ~256 KB OS-query buffer (stable
//            after 1-2 calls); ETW ring is pre-allocated. Zero steady-state alloc.
// Privilege: Admin for ETW and PresentMon. Degrades to NT polling otherwise.
//
#pragma once

#include <phynned/observer/TargetMetrics.hpp>
#include <phynned/observer/TargetProcess.hpp>
#include <phyriad/process/ProcessMetricsSnapshot.hpp>  // FR-11
#include <phyriad/etw/SessionManager.hpp>              // FR-6 (Win32 + no-op stubs en Linux)
#include <phyriad/schema/Error.hpp>

#include <array>
#include <atomic>
#include <cstdint>
#include <expected>
#include <optional>
#include <phyriad/hal/MemoryOrder.hpp>

namespace phynned::observer {

class MetricsCollector {
public:
    MetricsCollector() noexcept;
    ~MetricsCollector() noexcept;

    MetricsCollector(MetricsCollector const&)            = delete;
    MetricsCollector& operator=(MetricsCollector const&) = delete;

    /// Start ETW session (requires admin on Windows) and initialise the
    /// ProcessMetricsSnapshot.  On failure: returns error; subsequent sample()
    /// uses a fallback path where process metrics are zeroed.
    [[nodiscard]] std::expected<void, phyriad::Error> start() noexcept;

    /// Stop and clean up ETW session.
    void stop() noexcept;

    /// Sample metrics for `n` processes identified by `pids`.
    /// Writes results to `out_metrics[0..n-1]`.
    /// noexcept: any failure is silently reflected as zero/stale fields.
    void sample(const uint32_t* pids, uint32_t n,
                TargetMetrics* out_metrics) noexcept;

    /// AC-QUIET mode (2026-07-21 BF6 soak fix): when a kernel anti-cheat game
    /// is running, every OpenProcess the agent makes is inspected by the AC
    /// driver system-wide — a per-tick handle storm stalls the agent AND
    /// starves the game (BF6 → 6 fps, main thread stalled 10-14 s). In this
    /// mode the collector skips the per-process affinity-mask OpenProcess (Fix
    /// A): current_core_mask is emitted as 0 (the corral is suspended anyway,
    /// so the field is not needed). The light bulk NtQSI metrics path (one
    /// syscall, no per-process handle) still runs so the UI stays live.
    void set_low_handle_mode(bool on) noexcept { low_handle_mode_ = on; }
    [[nodiscard]] bool low_handle_mode() const noexcept { return low_handle_mode_; }

    [[nodiscard]] bool etw_active()            const noexcept { return etw_active_; }
    [[nodiscard]] bool frame_observer_active() const noexcept { return frame_obs_; }

private:
    bool etw_active_{false};
    bool frame_obs_ {false};
    bool low_handle_mode_{false};  // AC-quiet: skip Fix A OpenProcess (see setter)

    // ── Bulk process snapshot (FR-11) ──────────────────────────────────────
    // Initialised in start(); capture() called once per sample() invocation.
    // std::optional<> because create() can fail (rare OS error).
    std::optional<phyriad::proc::ProcessMetricsSnapshot> snapshot_{};

    // ── ETW ring buffer for context-switch events ──────────────────────────
    // ETW callback pushes; sample() drains.
    struct CtxSwEvent {
        uint32_t thread_id;
        uint32_t pid;
        uint32_t old_cpu;
        uint32_t new_cpu;
        uint64_t tsc;
    };
    static constexpr uint32_t kEtwRingCap = 4096u;
    alignas(64) std::array<CtxSwEvent, kEtwRingCap> etw_ring_{};
    std::atomic<uint64_t> etw_write_{0ull};
    uint64_t              etw_read_ {0ull};

    // ── Diagnóstico de overhead ETW (§11.5) ────────────────────────────────
    // Contadores atomicos para verificar reducción de overhead:
    //   etw_cswitch_total_   : todos los CSwitch events recibidos
    //   etw_cswitch_pushed_  : los que SÍ se pushean al ring (PID matched)
    //   etw_cswitch_skipped_ : los descartados por PID = 0 (no tracked)
    // Ratio típico esperado: pushed/total ≈ 5-10% en sistema con < 10 targets.
    std::atomic<uint64_t> etw_cswitch_total_  {0ull};
    std::atomic<uint64_t> etw_cswitch_pushed_ {0ull};
    std::atomic<uint64_t> etw_cswitch_skipped_{0ull};

public:
    /// Diagnostic — number of CSwitch ETW events seen, pushed, skipped.
    [[nodiscard]] uint64_t etw_cswitch_total()   const noexcept {
        return phyriad::hal::stat_load_relaxed(etw_cswitch_total_);
    }
    [[nodiscard]] uint64_t etw_cswitch_pushed()  const noexcept {
        return phyriad::hal::stat_load_relaxed(etw_cswitch_pushed_);
    }
    [[nodiscard]] uint64_t etw_cswitch_skipped() const noexcept {
        return phyriad::hal::stat_load_relaxed(etw_cswitch_skipped_);
    }
private:

    // ── TID → PID cache (workaround temporal hasta FR-13) ─────────────────
    // Direct-mapped cache lock-free indexada por (tid & kMask). Poblada desde
    // el provider Microsoft-Windows-Kernel-Thread (opcode 1 Thread Start,
    // opcode 3 DCStart rundown). Limpiada por opcodes 2/4 (Thread End/DCEnd).
    //
    // Capacidad 4096 entries × 8 B = 32 KB — caben holgadamente en cache L1.
    // Colisiones: política "last writer wins" (sin chaining). En sistema con
    // < ~2000 threads las colisiones son raras; cualquier miss devuelve pid=0
    // y el CSwitch event correspondiente no se atribuye (no rompe, no inventa).
    //
    // FR-13 (`phyriad::etw::CtxSwitchSource`) reemplazará esta tabla con un
    // hash O(1) verificado contra TID actual + rundown integrado.
    static constexpr uint32_t kTidPidCacheSize = 4096u;
    static constexpr uint32_t kTidPidCacheMask = kTidPidCacheSize - 1u;
    struct alignas(8) TidPidEntry {
        std::atomic<uint32_t> tid{0u};   // 0 = empty slot
        std::atomic<uint32_t> pid{0u};
    };
    static_assert(sizeof(TidPidEntry) == 8u);
    alignas(64) std::array<TidPidEntry, kTidPidCacheSize> tid_pid_cache_{};

    /// Look up PID for a TID. Returns 0 if not cached (caller should ignore
    /// the event). Called from sample() / drain_etw_ring (consumer thread).
    [[nodiscard]] uint32_t lookup_pid_for_tid(uint32_t tid) const noexcept;

    // ── SLIM per-PID process-level state (~56 B) — one per TRACKED PID ─────
    // MASS-router (2026-07-17, BR1): at track-all-touchable scale (kMaxTargets =
    // 1024) the old fat PidState (~1 KB, dominated by the 64-entry per-thread
    // hot-thread array) would cost ~1 MB for state the herd never needs. The fat
    // per-thread tracking now lives in the separate `hot_track_` pool, claimed
    // lazily only for the routing-relevant few. This slim record — process-level
    // CPU%, thread count, migrations — is what EVERY observed PID needs.
    // prev_kernel_100ns / prev_user_100ns mirror ProcessMetrics::kernel/user
    // time fields (100ns units since process creation). prev_wall_ticks is a
    // QueryPerformanceCounter snapshot for wall-time denominator.
    struct PidState {
        uint32_t pid;
        uint64_t prev_kernel_100ns;  // ProcessMetrics::kernel_time_100ns at t-1
        uint64_t prev_user_100ns;    // ProcessMetrics::user_time_100ns   at t-1
        uint64_t prev_wall_ticks;    // QPC ticks at t-1 (wall time denominator)
        uint32_t migration_count;
        uint32_t prev_migration_count;
        // Cached metrics emitted on non-capture ticks.
        // Bulk capture is throttled to every kBulkCaptureInterval ticks; on
        // intermediate ticks we reuse the last computed CPU%/thread_count
        // (500 ms staleness is well within tolerance for classification,
        // pressure heuristic, and UI display).
        float    cached_cpu_pct;
        uint32_t cached_thread_count;
        // Working set (RSS) in MB, from ProcessMetrics::working_set_bytes. Cached
        // like cpu_pct/thread_count so non-capture ticks emit a stable value. Feeds
        // TargetMetrics::working_set_mb (surfaced 2026-07-17, MR-1 shadow router).
        uint32_t cached_working_set_mb;
        // Current process affinity mask (Fix A, MR-2 2026-07-17). Read once per
        // capture tick via OpenProcess(QUERY_LIMITED)+GetProcessAffinityMask and
        // cached so non-capture ticks emit a stable value. Feeds
        // TargetMetrics::current_core_mask → the RouteAdvisor already_there test and
        // the MR-2 corral "is this active bg process on the V-Cache CCD?" predicate.
        // 0 = unknown (open denied / PPL / non-Windows).
        uint32_t cached_current_core_mask;
        // Handle into the fat hot-thread tracking pool (hot_track_). 0 = none;
        // otherwise (slot index + 1). Claimed lazily on the first capture tick a
        // process shows >= kHotTrackMinThreads threads (games/heavy apps only).
        uint32_t hot_slot_plus_1;
        bool     valid;
    };
    static_assert(sizeof(PidState) <= 64u,
        "PidState must stay slim (~56B) — fat per-thread state lives in hot_track_");

    // ── FAT hot-thread tracking (~1 KB) — pooled, the routed few only ─────
    // Hot thread identification via DELTA time. We need which thread is CURRENTLY
    // hot (highest CPU delta over the recent window), not which has the most
    // lifetime accumulated time — the latter biases toward early-spawned loader
    // threads and produces unstable classifications.
    //   - prev_threads[]: TID + (kernel+user)_100ns snapshot from the prior
    //     capture. On a new capture we look up each current thread by TID and
    //     compute the delta. Linear search (n ≤ 64).
    //   - hot_tid_candidate / hot_tid_consecutive: a confidence gate — a new
    //     candidate must dominate kHotConfidenceMin consecutive samples before we
    //     commit it to cached_hot_tid, preventing differential-pin thrashing.
    struct HotTrackState {
        uint32_t owner_pid;              // 0 = free slot
        static constexpr uint32_t kMaxTrackedThreads = 64u;
        struct ThreadDelta {
            uint32_t tid;
            uint32_t _pad;
            uint64_t prev_total_100ns;   // (kernel + user) at prev capture
        };
        static_assert(sizeof(ThreadDelta) == 16u);
        std::array<ThreadDelta, kMaxTrackedThreads> prev_threads;
        uint32_t n_prev_threads;
        uint32_t hot_tid_candidate;      // TID with highest delta last sample
        uint8_t  hot_tid_consecutive;    // # consecutive samples same candidate
        uint8_t  _pad_b1[3];
        uint32_t cached_hot_tid;         // committed only after kHotConfidenceMin
    };

    /// Minimum consecutive samples a TID must dominate before we commit it
    /// as the hot_tid. With 500 ms capture cadence, value=2 means ~1 s of
    /// stability required — fast enough for short benchmarks, stable enough
    /// to avoid thrashing.
    static constexpr uint8_t kHotConfidenceMin = 2u;

    // Slim state: one per observed PID (mass scale).
    static constexpr uint32_t kMaxPidStates = kMaxTargets;  // 1024
    std::array<PidState, kMaxPidStates> pid_states_{};
    uint32_t n_pid_states_{0u};

    // Fat hot-thread pool: only the busy, routing-relevant few. A process claims a
    // slot lazily (acquire_hot_track) on its first capture tick with
    // >= kHotTrackMinThreads threads; the idle mass herd never allocates one. 128
    // comfortably exceeds the count of concurrently-busy multi-threaded desktop
    // processes, so a game always gets a slot (its hot_tid path is unchanged).
    static constexpr uint32_t kMaxHotTrackStates = 128u;
    static constexpr uint32_t kHotTrackMinThreads = 8u;
    std::array<HotTrackState, kMaxHotTrackStates> hot_track_{};
    uint32_t n_hot_track_{0u};

    // ── Long-run eviction (2026-07-19 soak fix) ──────────────────────────
    // n_pid_states_ / n_hot_track_ were APPEND-ONLY: after 1024 distinct PIDs
    // (hours of browser/launcher churn — LoL relaunches a same-name game exe
    // with a fresh PID every match) find_or_create returned nullptr forever,
    // leaving every NEW process metrics-blind = the soak's "erratic
    // detection". sample() receives the full live tracked set each tick, so
    // every kEvictSweepInterval ticks we mark the tracked slots and evict the
    // rest: hash entry tombstoned (linear-probe chains stay connected), the
    // hot-track slot freed, and the PidState slot pushed on a free list that
    // find_or_create pops before growing the high-water mark.
    static constexpr uint32_t kEvictSweepInterval = 30u;   // ~30 s at 1 Hz
    static constexpr uint32_t kPidTombstone = 0xFFFFFFFFu; // never a real PID
    uint32_t evict_sweep_counter_{0u};
    std::array<uint16_t, kMaxPidStates>      free_slots_{};
    uint32_t n_free_slots_{0u};
    std::array<uint16_t, kMaxHotTrackStates> hot_free_{};
    uint32_t n_hot_free_{0u};

    /// Evict every valid PidState whose pid is NOT in `pids[0..n)`.
    void evict_absent_pids(const uint32_t* pids, uint32_t n) noexcept;

    /// Release `s`'s hot-track slot (if owned) back to the hot free list.
    void release_hot_track(PidState& s) noexcept {
        if (s.hot_slot_plus_1 == 0u) return;
        const uint32_t hs = s.hot_slot_plus_1 - 1u;
        if (hs < kMaxHotTrackStates && hot_track_[hs].owner_pid == s.pid) {
            hot_track_[hs].owner_pid = 0u;
            if (n_hot_free_ < kMaxHotTrackStates)
                hot_free_[n_hot_free_++] = static_cast<uint16_t>(hs);
        }
        s.hot_slot_plus_1 = 0u;
    }

    /// Tombstone `pid`'s hash entry (keeps probe chains connected — a plain
    /// clear would orphan every entry that probed past this slot).
    void pid_hash_remove(uint32_t pid) noexcept {
        if (pid == 0u) return;
        const uint32_t h = pid & kPidHashMask;
        for (uint32_t probe = 0u; probe < kPidHashSize; ++probe) {
            auto& e = pid_hash_[(h + probe) & kPidHashMask];
            if (e.pid == 0u) return;               // not present
            if (e.pid == pid) {
                e.pid = kPidTombstone;
                e.slot_plus_1 = 0u;
                return;
            }
        }
    }

    // Off-stack scratch for the per-tick bulk NtQSI/proc extract (kMaxTargets ×
    // 64B = 64 KB) — an Impl member, not a sample()-local, so the MASS cap does
    // not put a 64 KB array on the run() thread stack.
    std::array<phyriad::proc::ProcessMetrics, kMaxTargets> bulk_scratch_{};

    /// Return the hot-track slot for `s`, claiming a free/new one on first use.
    /// Returns nullptr when the pool is exhausted (hot-thread tracking is simply
    /// skipped for that PID → hot_tid stays 0; graceful, never fatal).
    HotTrackState* acquire_hot_track(PidState& s) noexcept;

    // Bulk-capture throttle counter. Init to interval so the very
    // first sample() triggers a capture (no blind window at startup).
    uint32_t bulk_capture_counter_{5u};
    static constexpr uint32_t kBulkCaptureInterval = 5u;  // 5 ticks = 500 ms

    // ── PID→slot direct-mapped hash ──────────────
    // Replaces the O(n) linear scan that drain_etw_ring() used to do per ETW
    // event. With hundreds–thousands of CSwitch events per tick × n=64 slots,
    // the linear scan was the #1 hot path (~2.5–5 ms/tick).
    //
    // Open addressing with linear probing. Sized 2048 (2× pid_states capacity of
    // 1024) for ~50% max load factor — collisions are rare and probe chains short.
    // Stores slot index + 1 so 0 means empty.
    //
    // MASS-router (2026-07-17, BR1): slot_plus_1 widened uint8_t→uint16_t. At the
    // old uint8_t width the map physically truncated past 254 distinct PIDs
    // (slot_idx + 1 had to fit in 255); at track-all-touchable scale that silently
    // dropped hundreds of processes from the O(1) metrics map. uint16_t holds the
    // full 1024-deep slot space.
    static constexpr uint32_t kPidHashSize = 2048u;      // power of 2, 2× kMaxPidStates
    static constexpr uint32_t kPidHashMask = kPidHashSize - 1u;
    struct PidHashEntry {
        uint32_t pid;         // 0 = empty slot
        uint16_t slot_plus_1; // index into pid_states_ + 1; 0 = empty
    };
    std::array<PidHashEntry, kPidHashSize> pid_hash_{};

    // Insert (pid, slot_idx) into pid_hash_. Called from find_or_create.
    // Caller guarantees pid != 0 and !already-present. Probe-chain length is
    // bounded by load factor (max n_pid_states_/kPidHashSize ≈ 50%).
    void pid_hash_insert(uint32_t pid, uint16_t slot_idx) noexcept {
        uint32_t h = pid & kPidHashMask;
        for (uint32_t probe = 0u; probe < kPidHashSize; ++probe) {
            auto& e = pid_hash_[(h + probe) & kPidHashMask];
            // Reuse tombstoned slots (eviction fix) as well as empty ones.
            if (e.pid == 0u || e.pid == kPidTombstone) {
                e.pid = pid;
                e.slot_plus_1 = static_cast<uint16_t>(slot_idx + 1u);
                return;
            }
        }
        // Table full — should never happen since size > kMaxPidStates.
    }

    // Lookup pid → slot index. Returns -1 if not found. Called from
    // drain_etw_ring on the hot path; expected O(1) average.
    [[nodiscard]] int32_t pid_hash_lookup(uint32_t pid) const noexcept {
        if (pid == 0u) return -1;
        const uint32_t h = pid & kPidHashMask;
        for (uint32_t probe = 0u; probe < kPidHashSize; ++probe) {
            const auto& e = pid_hash_[(h + probe) & kPidHashMask];
            if (e.pid == 0u) return -1;       // empty slot → terminated probe chain
            if (e.pid == pid) return static_cast<int32_t>(e.slot_plus_1) - 1;
        }
        return -1;
    }

    // ── Internal helpers ───────────────────────────────────────────────────
    PidState* find_or_create_pid_state(uint32_t pid) noexcept;
    void      drain_etw_ring() noexcept;

    // ── ETW real session (FR-6) — hookup completo del callback ───────────
    // phyriad::etw::SessionManager NO es movible — direct member, no optional<>.
    // En Linux compila como no-op stub que retorna Unavailable de start().
    phyriad::etw::SessionManager etw_session_{};

    /// Callback que recibe EVENT_RECORDs del consumer thread de ETW.
    /// MUST ser lightweight: parsea el evento y empuja a `etw_ring_`.
    /// Cualquier trabajo pesado va en la próxima sample().
#ifdef _WIN32
    static void etw_record_callback(const EVENT_RECORD& rec, void* user_ctx) noexcept;

    // PDH query handle
    void* pdh_query_{nullptr};
#endif
};

} // namespace phynned::observer
// Made with my soul - Swately <3
