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

    [[nodiscard]] bool etw_active()            const noexcept { return etw_active_; }
    [[nodiscard]] bool frame_observer_active() const noexcept { return frame_obs_; }

private:
    bool etw_active_{false};
    bool frame_obs_ {false};

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

    // ── Per-PID delta state (previous values for CPU % computation) ────────
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
        // Hot thread identification via DELTA time.
        // We need to know which thread is CURRENTLY hot (highest CPU delta
        // over the recent sample window), not which has the most lifetime
        // accumulated time — the latter biases toward early-spawned threads
        // (loaders) and produces unstable initial classifications.
        //
        // Storage per PidState:
        //   - prev_threads[]: TID + (kernel+user)_100ns snapshot from prior
        //     capture. On new capture, we look up each current thread by TID
        //     and compute the delta. Linear search (n≤64).
        //   - hot_tid_candidate / hot_tid_consecutive: confidence gate.
        //     A new "candidate" must dominate 2 consecutive samples before
        //     we commit it to cached_hot_tid. Prevents thrashing the
        //     differential-pin policy on transient bursts.
        //
        // Memory: 64 entries × 16B = 1 KB per PidState. ×64 max targets
        // = 64 KB total for thread profiling. Negligible.
        static constexpr uint32_t kMaxTrackedThreads = 64u;
        struct ThreadDelta {
            uint32_t tid;
            uint32_t _pad;
            uint64_t prev_total_100ns;   // (kernel + user) at prev capture
        };
        static_assert(sizeof(ThreadDelta) == 16u);
        std::array<ThreadDelta, kMaxTrackedThreads> prev_threads;
        uint32_t n_prev_threads;
        // Confidence machinery:
        uint32_t hot_tid_candidate;      // TID with highest delta last sample
        uint8_t  hot_tid_consecutive;    // # consecutive samples same candidate
        uint8_t  _pad_b1[3];
        // Committed value (only set after kHotConfidenceMin consecutive samples):
        uint32_t cached_hot_tid;

        bool     valid;
    };
    /// Minimum consecutive samples a TID must dominate before we commit it
    /// as the hot_tid. With 500 ms capture cadence, value=2 means ~1 s of
    /// stability required — fast enough for short benchmarks, stable enough
    /// to avoid thrashing.
    static constexpr uint8_t kHotConfidenceMin = 2u;
    static constexpr uint32_t kMaxPidStates = kMaxTargets;
    std::array<PidState, kMaxPidStates> pid_states_{};
    uint32_t n_pid_states_{0u};

    // Bulk-capture throttle counter. Init to interval so the very
    // first sample() triggers a capture (no blind window at startup).
    uint32_t bulk_capture_counter_{5u};
    static constexpr uint32_t kBulkCaptureInterval = 5u;  // 5 ticks = 500 ms

    // ── PID→slot direct-mapped hash ──────────────
    // Replaces the O(n) linear scan that drain_etw_ring() used to do per ETW
    // event. With hundreds–thousands of CSwitch events per tick × n=64 slots,
    // the linear scan was the #1 hot path (~2.5–5 ms/tick).
    //
    // Open addressing with linear probing. Sized 128 (2× pid_states capacity)
    // for ~50% max load factor — collisions are rare and probe chains short.
    // Stores slot index + 1 so 0 means empty (PIDs are uint32 but slot indices
    // fit in 7 bits, leaving room for the empty sentinel).
    static constexpr uint32_t kPidHashSize = 128u;       // power of 2
    static constexpr uint32_t kPidHashMask = kPidHashSize - 1u;
    struct PidHashEntry {
        uint32_t pid;       // 0 = empty slot
        uint8_t  slot_plus_1; // index into pid_states_ + 1; 0 = empty
    };
    std::array<PidHashEntry, kPidHashSize> pid_hash_{};

    // Insert (pid, slot_idx) into pid_hash_. Called from find_or_create.
    // Caller guarantees pid != 0 and !already-present. Probe-chain length is
    // bounded by load factor (max n_pid_states_/kPidHashSize ≈ 50%).
    void pid_hash_insert(uint32_t pid, uint8_t slot_idx) noexcept {
        uint32_t h = pid & kPidHashMask;
        for (uint32_t probe = 0u; probe < kPidHashSize; ++probe) {
            auto& e = pid_hash_[(h + probe) & kPidHashMask];
            if (e.pid == 0u) {
                e.pid = pid;
                e.slot_plus_1 = static_cast<uint8_t>(slot_idx + 1u);
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
