// observer/src/MetricsCollector_win32.cpp
// MetricsCollector — Windows implementation.
//
// ETW for context-switch tracking; phyriad::proc::ProcessMetricsSnapshot (FR-11)
// for bulk CPU/memory metrics in one NtQSI syscall; PDH for GPU usage.
//
// CPU % formula:
//   delta_cpu_100ns = (kernel_time_100ns - prev_kernel) + (user_time_100ns - prev_user)
//   wall_100ns      = (qpc_now - qpc_prev) * 10_000_000 / qpc_freq
//   cpu_pct         = delta_cpu_100ns / wall_100ns * 100.0
//
#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <pdh.h>
#include <evntrace.h>
#include <evntcons.h>
// Note: psapi.h no longer needed — working-set sampled via ProcessMetricsSnapshot.

#include <phynned/observer/MetricsCollector.hpp>
#include <phyriad/schema/Error.hpp>

#include <cstring>
#include <cstdio>
#include <algorithm>
#include <bitset>     // eviction sweep (2026-07-19 soak fix)
#include <phyriad/hal/MemoryOrder.hpp>

// Link: pdh.lib (CMakeLists links it)

namespace phynned::observer {

// ── ETW Provider: Thread context switch (NT Kernel Logger) ─────────────────
// GUID: {9E814AAD-3204-11D2-9A82-006008A86939} = SystemTraceControlGuid
// Thread context-switch opcode: Event class {GUID} opcode = 36 (CSwitch)

// Minimal CSwitch event structure (from WDK Wmicore.mof)
#pragma pack(push, 1)
struct CswitchEvent {
    uint32_t new_thread_id;
    uint32_t old_thread_id;
    int8_t   new_thread_priority;
    int8_t   old_thread_priority;
    uint8_t  previous_c_state;
    int8_t   spare_byte;
    int8_t   old_thread_wait_reason;
    int8_t   old_thread_wait_mode;
    int8_t   old_thread_state;
    int8_t   old_thread_wait_ideal_processor;
    uint32_t new_thread_wait_time;
    uint32_t reserved;
};
#pragma pack(pop)

// ── Fix A (MR-2): read a process's current affinity mask ───────────────────
// One OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION)+GetProcessAffinityMask per
// target, on capture ticks only (cached between). QUERY_LIMITED (0x1000) is the
// least right that lets GetProcessAffinityMask succeed (verified on-box; SET-only
// 0x0200 returns ACCESS_DENIED) and is not cheat-shaped. Degrades to 0 when the
// open fails (PPL / access denied) — the caller reads 0 as "unknown / floating".
// Returns the low 32 bits (TargetMetrics masks are uint32_t; the 7950X3D has 32
// logical cores).
static uint32_t read_process_affinity_mask(uint32_t pid) noexcept {
    if (pid == 0u) return 0u;
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE,
                           static_cast<DWORD>(pid));
    if (!h) return 0u;
    DWORD_PTR proc_mask = 0u, sys_mask = 0u;
    const BOOL ok = GetProcessAffinityMask(h, &proc_mask, &sys_mask);
    CloseHandle(h);
    if (!ok) return 0u;
    return static_cast<uint32_t>(proc_mask & 0xFFFFFFFFull);
}

// ── Constructor / Destructor ──────────────────────────────────────────────
MetricsCollector::MetricsCollector() noexcept {
    etw_ring_.fill(CtxSwEvent{});
    pid_states_.fill(PidState{});
    hot_track_.fill(HotTrackState{});
}

MetricsCollector::~MetricsCollector() noexcept {
    stop();
}

// ── start() ───────────────────────────────────────────────────────────────
std::expected<void, phyriad::Error> MetricsCollector::start() noexcept {
    // ── ProcessMetricsSnapshot (FR-11) ────────────────────────────────────
    // One NtQuerySystemInformation call per tick instead of N × OpenProcess
    // + GetProcessTimes.  ~10× faster at 32 targets.
    auto snap = phyriad::proc::ProcessMetricsSnapshot::create();
    if (snap.has_value()) {
        snapshot_ = std::move(*snap);
        // Warmup capture: stabilises internal buffer size (avoids growth on
        // the first hot-path tick).
        (void)snapshot_->capture();
    } else {
        std::fprintf(stderr,
            "[MetricsCollector] ProcessMetricsSnapshot::create() failed "
            "(code=%u) — sample() will produce zero metrics.\n",
            static_cast<unsigned>(snap.error().code));
        // Non-fatal: sample() guards on snapshot_.has_value().
    }

    // ── PDH query for GPU usage ────────────────────────────────────────────
    PDH_HQUERY query;
    if (PdhOpenQueryW(NULL, 0, &query) == ERROR_SUCCESS) {
        pdh_query_ = query;
    }

    // ── ETW session via FR-6 (phyriad::etw::SessionManager) ───────────────────
    // Requiere admin. Sin admin → start() falla con PermissionDenied;
    // marcamos etw_active_=false y el agente sigue en modo degradado
    // (CPU% se mide vía ProcessMetricsSnapshot, migrations_per_sec quedan en 0).
    static const phyriad::etw::ProviderSpec kProviders[] = {
        // Context-switch events — clave para migrations_per_sec.
        // Keyword 0x10 ≈ CSwitch (provider-specific). TRACE_LEVEL_INFORMATION es OK.
        { phyriad::etw::providers::kKernelContextSwitch,
          TRACE_LEVEL_INFORMATION, 0x10u, 0u },
        // Process + Thread events para TID→PID mapping (futuro FR-13).
        { phyriad::etw::providers::kKernelProcess,
          TRACE_LEVEL_INFORMATION, 0x10u, 0u },
        { phyriad::etw::providers::kKernelThread,
          TRACE_LEVEL_INFORMATION, 0x10u, 0u },
    };
    const std::span<const phyriad::etw::ProviderSpec> providers_span{
        kProviders, sizeof(kProviders)/sizeof(kProviders[0])};

    const auto r_start = etw_session_.start(
        "PhynnedKernel.v1",            // session name (overridable cuando reinicio)
        providers_span,
        64u,                         // buffer_size_kb
        16u);                        // max_buffers

    if (!r_start.has_value()) {
        std::fprintf(stderr,
            "[MetricsCollector] phyriad::etw::SessionManager::start() failed "
            "(code=%u). ETW disabled; metrics se degradan (migrations_per_sec=0).\n",
            static_cast<unsigned>(r_start.error().code));
        etw_active_ = false;
        frame_obs_  = false;
        // Non-fatal: agent sigue corriendo en modo polling-only.
        return {};
    }

    // Sesión OK — arrancar consumer thread que dispara etw_record_callback.
    const auto r_consumer = etw_session_.start_consumer(
        &MetricsCollector::etw_record_callback, this);
    if (!r_consumer.has_value()) {
        std::fprintf(stderr,
            "[MetricsCollector] ETW start_consumer() failed (code=%u). "
            "Stopping session, marking degraded.\n",
            static_cast<unsigned>(r_consumer.error().code));
        etw_session_.stop();
        etw_active_ = false;
        frame_obs_  = false;
        return {};
    }

    etw_active_ = true;
    frame_obs_  = false;  // PresentMon provider hookup en un pass futuro.
    std::fprintf(stdout,
        "[MetricsCollector] ETW session 'PhynnedKernel.v1' running "
        "(%zu providers).\n",
        providers_span.size());

    return {};
}

// ── stop() ────────────────────────────────────────────────────────────────
void MetricsCollector::stop() noexcept {
    // Detiene primero el consumer thread + sesión ETW (bloquea hasta exit).
    // RAII: el destructor del SessionManager también lo haría, pero stop()
    // se llama explícitamente desde el flujo de shutdown ordenado.
    etw_session_.stop();

    if (pdh_query_) {
        PDH_HQUERY q = reinterpret_cast<PDH_HQUERY>(
            reinterpret_cast<uintptr_t>(pdh_query_));
        PdhCloseQuery(q);
        pdh_query_ = nullptr;
    }
    snapshot_.reset();    // release ProcessMetricsSnapshot buffer
    etw_active_ = false;
}

// ── PidState management ───────────────────────────────────────────────────
// Now uses pid_hash_ for O(1) lookup. Linear scan
// kept ONLY as cold-path fallback in the (impossible) case of hash desync.
MetricsCollector::PidState*
MetricsCollector::find_or_create_pid_state(uint32_t pid) noexcept {
    if (pid == 0u) return nullptr;

    // Fast path: hash lookup.
    const int32_t slot = pid_hash_lookup(pid);
    if (slot >= 0) {
        PidState& s = pid_states_[static_cast<uint32_t>(slot)];
        if (s.valid && s.pid == pid) return &s;
        // Slot exists in hash but state was invalidated — fall through to
        // re-create (rare; happens if PID was recycled).
    }

    // Eviction fix (2026-07-19): pop a freed slot before growing the
    // high-water mark — without this the table saturated permanently after
    // 1024 distinct PIDs and every new process went metrics-blind.
    uint32_t new_idx;
    if (n_free_slots_ > 0u) {
        new_idx = free_slots_[--n_free_slots_];
    } else if (n_pid_states_ < kMaxPidStates) {
        new_idx = n_pid_states_++;
    } else {
        return nullptr;
    }
    PidState& s = pid_states_[new_idx];
    s = PidState{};
    s.pid   = pid;
    s.valid = true;
    pid_hash_insert(pid, static_cast<uint16_t>(new_idx));
    return &s;
}

// ── evict_absent_pids — long-run eviction sweep (2026-07-19 soak fix) ──────
// `pids[0..n)` is the authoritative live tracked set sample() was handed this
// tick. Any valid PidState not in it belongs to a process that died or left
// the touchable set → evict (tombstone hash, free hot-track slot, push the
// PidState slot on the free list). If the process comes back it is simply
// re-created fresh — which is also the CORRECT answer for a recycled PID.
void MetricsCollector::evict_absent_pids(const uint32_t* pids,
                                          uint32_t n) noexcept {
    std::bitset<kMaxPidStates> live{};
    for (uint32_t i = 0u; i < n; ++i) {
        const int32_t s = pid_hash_lookup(pids[i]);
        if (s >= 0) live.set(static_cast<size_t>(s));
    }
    for (uint32_t s = 0u; s < n_pid_states_; ++s) {
        PidState& st = pid_states_[s];
        if (!st.valid || live.test(s)) continue;
        pid_hash_remove(st.pid);
        release_hot_track(st);
        st = PidState{};   // valid=false
        if (n_free_slots_ < kMaxPidStates)
            free_slots_[n_free_slots_++] = static_cast<uint16_t>(s);
    }
}

// ── acquire_hot_track — lazily claim a fat hot-thread slot (routed few) ─────
// MASS-router: only busy multi-threaded processes reach here (gated by
// kHotTrackMinThreads in sample()), so the 128-slot pool never starves a game.
MetricsCollector::HotTrackState*
MetricsCollector::acquire_hot_track(PidState& s) noexcept {
    if (s.hot_slot_plus_1 != 0u) {
        HotTrackState& hs = hot_track_[s.hot_slot_plus_1 - 1u];
        if (hs.owner_pid == s.pid) return &hs;
        // Slot was recycled to another PID — drop the stale handle, re-claim.
        s.hot_slot_plus_1 = 0u;
    }
    // Eviction fix (2026-07-19): freed slots are reusable — previously the
    // pool saturated permanently after 128 distinct busy processes and every
    // later game silently lost hot-thread tracking.
    uint32_t idx;
    if (n_hot_free_ > 0u) {
        idx = hot_free_[--n_hot_free_];
    } else if (n_hot_track_ < kMaxHotTrackStates) {
        idx = n_hot_track_++;
    } else {
        return nullptr;  // pool full → skip (graceful)
    }
    HotTrackState& hs = hot_track_[idx];
    hs = HotTrackState{};
    hs.owner_pid = s.pid;
    s.hot_slot_plus_1 = idx + 1u;
    return &hs;
}

// ── TID→PID cache lookup ──────────────────────────────────────────────────
uint32_t MetricsCollector::lookup_pid_for_tid(uint32_t tid) const noexcept {
    if (tid == 0u) return 0u;
    const auto& slot = tid_pid_cache_[tid & kTidPidCacheMask];
    // Load tid first; if it matches request, the pid stored is valid.
    // Acquire on tid pairs with release on the producer's tid store.
    const uint32_t stored_tid = phyriad::hal::seq_load_acquire(slot.tid);
    if (stored_tid != tid) return 0u;  // cache miss / collision / cleared
    return phyriad::hal::stat_load_relaxed(slot.pid);
}

// ── ETW ring drain ────────────────────────────────────────────────────────
// Solo atribuye migraciones a PIDs YA registrados en pid_states_ (i.e., los
// que `sample()` está observando).
//
// Post §11.5: el callback ya descarta eventos con pid=0, así que aquí solo
// procesamos events con PID válido en el cache. No retry-on-miss porque el
// callback no nos pasa events sin PID.
void MetricsCollector::drain_etw_ring() noexcept {
    const uint64_t write_head = phyriad::hal::seq_load_acquire(etw_write_);
    while (etw_read_ < write_head) {
        const CtxSwEvent& ev = etw_ring_[etw_read_ & (kEtwRingCap - 1u)];
        ++etw_read_;

        // O(1) hash lookup instead of O(n) linear scan.
        // With hundreds-thousands of CSwitch events/tick × n=64 slots this
        // was the #1 hot path (~2.5-5 ms/tick → 5-10% CPU). Now O(1) avg.
        //
        // Find-only: do not create PidState for non-target PIDs (guard against
        // inflation of pid_states_ with system PIDs).
        const int32_t slot = pid_hash_lookup(ev.pid);
        if (slot < 0) continue;
        PidState& state = pid_states_[static_cast<uint32_t>(slot)];
        if (state.valid && ev.old_cpu != ev.new_cpu) {
            ++state.migration_count;
        }
    }
}

// ── Main sample() ─────────────────────────────────────────────────────────
void MetricsCollector::sample(const uint32_t* pids, uint32_t n,
                               TargetMetrics* out_metrics) noexcept {
    if (!pids || !out_metrics || n == 0u) return;

    // ── 1. Drain ETW ring (context-switch migration counts) ───────────────
    // Always — cheap now with O(1) hash lookup.
    drain_etw_ring();

    // ── 1b. Long-run eviction sweep (2026-07-19 soak fix) ─────────────────
    // ~every 30 s: drop PidStates for PIDs no longer in the tracked set so
    // hours of process churn can never saturate the 1024-slot table.
    if (++evict_sweep_counter_ >= kEvictSweepInterval) {
        evict_sweep_counter_ = 0u;
        evict_absent_pids(pids, n);
    }

    // ── 2. Bulk capture (FR-11) — ONE syscall for all processes ──────────
    // Throttled to every kBulkCaptureInterval ticks
    // (500 ms). Previously called every 100 ms = 10×/sec for hundreds of
    // processes. CPU%/thread_count change slowly enough that 500 ms staleness
    // is fine; on intermediate ticks we emit cached values from PidState.
    // Migrations remain fresh (drained every tick from ETW above).
    const bool do_capture =
        (++bulk_capture_counter_ >= kBulkCaptureInterval);
    if (do_capture) bulk_capture_counter_ = 0u;

    // Off-stack scratch (Impl member) — avoids a 64 KB stack frame at MASS cap.
    phyriad::proc::ProcessMetrics* bulk = bulk_scratch_.data();
    bool snapshot_ok = false;

    if (do_capture && snapshot_.has_value()) {
        const auto cap_result = snapshot_->capture();
        if (cap_result.has_value()) {
            (void)snapshot_->extract(pids, n, bulk);
            snapshot_ok = true;
        }
    }

    // ── 3. Wall-clock denominator for CPU % ──────────────────────────────
    LARGE_INTEGER now_qpc, freq_qpc;
    QueryPerformanceCounter(&now_qpc);
    QueryPerformanceFrequency(&freq_qpc);
    const uint64_t wall_now_ticks = static_cast<uint64_t>(now_qpc.QuadPart);
    const double   qpc_freq       = static_cast<double>(freq_qpc.QuadPart);

    // ── 4. Fill TargetMetrics per-PID ─────────────────────────────────────
    for (uint32_t i = 0u; i < n; ++i) {
        const uint32_t pid = pids[i];
        TargetMetrics& out = out_metrics[i];
        std::memset(&out, 0, sizeof(out));
        out.pid = pid;

        PidState* state = find_or_create_pid_state(pid);
        if (!state) continue;

        if (snapshot_ok) {
            const phyriad::proc::ProcessMetrics& pm = bulk[i];

            // ── PID-REUSE GUARD (2026-07-19 soak fix) ─────────────────────
            // Per-process CPU times are MONOTONIC for a live process. A fresh
            // sample BELOW our stored baseline means Windows recycled this pid
            // onto a NEW process (the LoL relaunch pattern) — every cached
            // field belongs to the dead one, and the unsigned delta below
            // would wrap to garbage. Reset to a fresh state (slot + hash entry
            // kept; stale hot-track handle released). Costs one 0% tick for
            // the reborn pid — correct, since we have no real baseline yet.
            if (state->prev_wall_ticks != 0u &&
                (pm.kernel_time_100ns < state->prev_kernel_100ns ||
                 pm.user_time_100ns   < state->prev_user_100ns)) {
                const uint32_t keep_pid = state->pid;
                release_hot_track(*state);
                *state = PidState{};
                state->pid   = keep_pid;
                state->valid = true;
            }

            // ── CPU % from 100ns-unit deltas (delta spans the throttle window) ─
            const uint64_t cpu_delta_100ns =
                (pm.kernel_time_100ns - state->prev_kernel_100ns) +
                (pm.user_time_100ns   - state->prev_user_100ns);

            if (state->prev_wall_ticks != 0u) {
                const uint64_t wall_delta_ticks =
                    wall_now_ticks - state->prev_wall_ticks;
                // Convert QPC delta → 100ns units  (×10,000,000 / freq)
                const double wall_100ns =
                    static_cast<double>(wall_delta_ticks) *
                    10'000'000.0 / qpc_freq;

                out.cpu_usage_pct = (wall_100ns > 0.0)
                    ? static_cast<float>(
                        static_cast<double>(cpu_delta_100ns) / wall_100ns * 100.0)
                    : 0.f;
                // Clamp: rare QPC jitter or short-lived process bursts
                if (out.cpu_usage_pct > 100.f * 64.f)  // 64 logical cores max
                    out.cpu_usage_pct = 0.f;
            }

            state->prev_kernel_100ns = pm.kernel_time_100ns;
            state->prev_user_100ns   = pm.user_time_100ns;
            // Only update wall baseline on capture ticks so next
            // delta spans the full throttle window (500 ms) and CPU% math
            // remains correct.
            state->prev_wall_ticks   = wall_now_ticks;

            // ── Thread count ──────────────────────────────────────────────
            out.observed_threads     = pm.thread_count;

            // ── Working set (RSS) in MB — MR-1 shadow-router input ────────
            // Already sampled by ProcessMetricsSnapshot (FR-11); surface it into
            // TargetMetrics so the RouteAdvisor can position it vs the 32/96MB L3
            // boundaries. Integer MB is plenty for the proxy (no sub-MB precision
            // needed). Bytes→MB via >>20.
            out.working_set_mb =
                static_cast<uint32_t>(pm.working_set_bytes >> 20u);

            // ── Current affinity mask (Fix A, MR-2) — one QUERY_LIMITED handle
            // per target on the 500 ms capture cadence; cached between. Feeds the
            // RouteAdvisor already_there test + the MR-2 corral V-Cache predicate.
            // COST GATE (M3 "lo más barato"): only open the handle for ACTIVE
            // processes (>= 1% CPU). The idle mass herd — the hundreds of tracked
            // processes — never pays for the syscall; they get 0 (which the corral
            // reads as "not a candidate" anyway, since a corral candidate must be
            // >= kCorralActiveCpuPct = 3%). This keeps the per-tick handle count
            // O(active few) instead of O(all touchable).
            static constexpr float kCurrentMaskCpuFloor = 1.0f;
            out.current_core_mask =
                (out.cpu_usage_pct >= kCurrentMaskCpuFloor)
                    ? read_process_affinity_mask(pid)
                    : 0u;

            // Update cache for non-capture ticks.
            state->cached_cpu_pct           = out.cpu_usage_pct;
            state->cached_thread_count      = pm.thread_count;
            state->cached_working_set_mb    = out.working_set_mb;
            state->cached_current_core_mask = out.current_core_mask;
        } else {
            // ── Non-capture tick: emit cached values ──────────
            out.cpu_usage_pct     = state->cached_cpu_pct;
            out.observed_threads  = state->cached_thread_count;
            out.working_set_mb    = state->cached_working_set_mb;
            out.current_core_mask = state->cached_current_core_mask;
        }

        // ── Migration rate (delta from last sample) — always fresh ────────
        const uint32_t mig_delta = state->migration_count
                                 - state->prev_migration_count;
        out.migrations_per_sec       = mig_delta;
        state->prev_migration_count  = state->migration_count;

        // ── Pressure heuristic — uses latest (fresh or cached) values ────
        if (out.migrations_per_sec > 50u || out.cpu_usage_pct > 80.f) {
            out.pressure_level = 2u;  // red
        } else if (out.migrations_per_sec > 20u || out.cpu_usage_pct > 50.f) {
            out.pressure_level = 1u;  // yellow
        } else {
            out.pressure_level = 0u;  // green
        }

        // ── GPU usage via PDH (sampled at lower rate; reads last cached value)
        // TODO: integrate PDH counter per-PID in Phase 2.

        // ── Hot thread identification ─────────────────────────
        // DELTA-time heuristic with 2-sample confidence gate:
        //
        //   On each capture tick:
        //     1. extract_threads(pid) → current thread snapshot.
        //     2. For each current thread, find its TID in prev_threads[].
        //        If found, delta = current.total - prev.total. If not (newly
        //        spawned), skip — no delta available yet.
        //     3. Find the TID with MAX delta. That's the candidate hot thread.
        //     4. If candidate matches state->hot_tid_candidate from prior
        //        capture, increment hot_tid_consecutive. Else reset.
        //     5. Once hot_tid_consecutive >= kHotConfidenceMin, commit to
        //        state->cached_hot_tid (this is what consumers see).
        //     6. Update prev_threads[] = current threads for next sample.
        //
        // Why delta and not absolute time? Absolute time picks the thread
        // with the most LIFETIME CPU consumption, which biases toward early-
        // spawned loader threads. Delta picks the currently-busy thread,
        // which is what we actually want to pin to V-Cache.
        //
        // Cost: O(threads × prev_threads). Both ≤ 64 → 4096 compares per
        // PID worst case (~12 µs). Negligible on 500 ms cadence.
        //
        // On non-capture ticks we emit the cached value so consumers see
        // a stable TID across throttled bulk-capture intervals.
        // MASS-router: gate the FAT hot-thread tracking on thread count. Only
        // busy multi-threaded processes (>= kHotTrackMinThreads) claim a slot from
        // the bounded hot_track_ pool; the idle mass herd never allocates one (its
        // hot_tid stays 0, which is fine — hot_tid is consumed ONLY by the Game
        // differential-pin rule, and games always clear the thread bar). The
        // algorithm below is byte-for-byte the pre-MASS logic, just reading/writing
        // the pooled HotTrackState `hs` instead of inline PidState fields.
        HotTrackState* hs =
            (out.observed_threads >= kHotTrackMinThreads)
                ? acquire_hot_track(*state)
                : nullptr;

        if (hs && snapshot_ok && snapshot_.has_value()) {
            constexpr uint32_t kMaxThreadsPerProcess =
                HotTrackState::kMaxTrackedThreads;  // 64
            phyriad::proc::ThreadEntry threads[kMaxThreadsPerProcess];
            const uint32_t n_threads = snapshot_->extract_threads(
                pid, threads, kMaxThreadsPerProcess);

            // Step 2-3: compute deltas and find max-delta TID.
            uint32_t max_delta_tid = 0u;
            uint64_t max_delta_100ns = 0u;
            for (uint32_t ti = 0u; ti < n_threads; ++ti) {
                const auto& te = threads[ti];
                const uint64_t cur_total = te.kernel_time_100ns
                                         + te.user_time_100ns;
                // Linear search for this TID in prev snapshot.
                for (uint32_t pi = 0u; pi < hs->n_prev_threads; ++pi) {
                    if (hs->prev_threads[pi].tid == te.tid) {
                        const uint64_t prev_total =
                            hs->prev_threads[pi].prev_total_100ns;
                        if (cur_total > prev_total) {
                            const uint64_t d = cur_total - prev_total;
                            if (d > max_delta_100ns) {
                                max_delta_100ns = d;
                                max_delta_tid   = te.tid;
                            }
                        }
                        break;
                    }
                }
            }

            // Step 4-5: confidence gate.
            if (max_delta_tid != 0u) {
                if (max_delta_tid == hs->hot_tid_candidate) {
                    if (hs->hot_tid_consecutive < 255u) {
                        ++hs->hot_tid_consecutive;
                    }
                } else {
                    hs->hot_tid_candidate   = max_delta_tid;
                    hs->hot_tid_consecutive = 1u;
                }
                if (hs->hot_tid_consecutive >= kHotConfidenceMin) {
                    hs->cached_hot_tid = max_delta_tid;
                }
            }
            out.hot_tid = hs->cached_hot_tid;

            // Step 6: update prev_threads snapshot for next sample.
            const uint32_t store_n = (n_threads < kMaxThreadsPerProcess)
                                     ? n_threads : kMaxThreadsPerProcess;
            for (uint32_t ti = 0u; ti < store_n; ++ti) {
                hs->prev_threads[ti].tid = threads[ti].tid;
                hs->prev_threads[ti].prev_total_100ns =
                    threads[ti].kernel_time_100ns
                  + threads[ti].user_time_100ns;
            }
            hs->n_prev_threads = store_n;
        } else if (hs) {
            // Non-capture tick for a tracked-hot process: emit cached hot_tid so
            // consumers see a stable value across throttled bulk-capture intervals.
            out.hot_tid = hs->cached_hot_tid;
        }
        // else: herd process (or pool exhausted) — out.hot_tid stays 0 (memset).
    }
}

// ── ETW callback (runs on ETW consumer thread) ────────────────────────────
//
// Contract (phyriad::etw::SessionManager):
//   - Llamado por el consumer thread del SessionManager, una vez por evento.
//   - DEBE ser lightweight: parse + push/update + return.
//   - NO locks, NO allocaciones, NO logs (excepto warnings raros).
//
// Eventos que procesamos:
//   1. Kernel-Dispatcher / opcode 36 (CSwitch) — push CtxSwEvent al ring.
//   2. Kernel-Thread / opcode 1 (Start) o 3 (DCStart rundown):
//        Layout: UserData[0..3]=ProcessId, UserData[4..7]=ThreadId
//        → poblar tid_pid_cache_[tid & mask] = {tid, pid}
//   3. Kernel-Thread / opcode 2 (End) o 4 (DCEnd):
//        → limpiar tid_pid_cache_[tid & mask] si coincide con tid
//   4. Resto: ignored.
//
// El opcode CSwitch lee del cache (lookup_pid_for_tid) DENTRO del callback
// para que `slot.pid` ya esté resuelto cuando drain_etw_ring lo procese.
// Si el cache aún no está poblado (race entre CSwitch y Thread Start),
// `slot.pid = 0` y `drain_etw_ring` reintenta el lookup más tarde.
void MetricsCollector::etw_record_callback(const EVENT_RECORD& rec,
                                            void* user_ctx) noexcept
{
    auto* self = static_cast<MetricsCollector*>(user_ctx);
    if (!self) return;

    const uint8_t opcode = rec.EventHeader.EventDescriptor.Opcode;

    // ── Kernel-Thread events: poblar / limpiar TID→PID cache ──────────
    // GUID match: rec.EventHeader.ProviderId == kKernelThread guid (no
    // necesitamos compararla — los providers que enabled vienen
    // pre-filtrados por kernel).
    if (opcode == 1u || opcode == 3u) {
        // Thread_TypeGroup1 (Start o DCStart).
        // Layout mínimo: ProcessId (4 B) + ThreadId (4 B) al inicio de UserData.
        if (rec.UserDataLength >= 8u) {
            const auto* data = static_cast<const uint8_t*>(rec.UserData);
            uint32_t pid = 0u, tid = 0u;
            std::memcpy(&pid, data + 0u, sizeof(uint32_t));
            std::memcpy(&tid, data + 4u, sizeof(uint32_t));
            if (tid != 0u) {
                auto& slot = self->tid_pid_cache_[tid & kTidPidCacheMask];
                phyriad::hal::stat_store_relaxed(slot.pid, pid);
                phyriad::hal::seq_store_release(slot.tid, tid);  // commit
            }
        }
        return;
    }
    if (opcode == 2u || opcode == 4u) {
        // Thread_TypeGroup1 (End o DCEnd) — limpiar slot si coincide.
        if (rec.UserDataLength >= 8u) {
            const auto* data = static_cast<const uint8_t*>(rec.UserData);
            uint32_t tid = 0u;
            std::memcpy(&tid, data + 4u, sizeof(uint32_t));
            if (tid != 0u) {
                auto& slot = self->tid_pid_cache_[tid & kTidPidCacheMask];
                uint32_t expected = tid;
                // CAS-like: only clear if the slot still holds OUR tid (otra
                // entrada pudo haber sobreescrito por colisión).
                if (slot.tid.compare_exchange_strong(
                        expected, 0u, std::memory_order_acq_rel,  // HAL: explicit ordering — see surrounding context
                        std::memory_order_relaxed)) {  // HAL: relaxed — secondary atomic in compound op
                    phyriad::hal::stat_store_relaxed(slot.pid, 0u);
                }
            }
        }
        return;
    }

    // ── Kernel-Dispatcher CSwitch event (opcode 36) ───────────────────
    constexpr uint8_t kCswitchOpcode = 36u;
    if (opcode != kCswitchOpcode) return;
    if (rec.UserDataLength < sizeof(CswitchEvent)) return;

    // Cuenta TODOS los CSwitch events (diagnóstico).
    phyriad::hal::stat_fetch_add_relaxed(self->etw_cswitch_total_, 1ull);

    const auto* cs = reinterpret_cast<const CswitchEvent*>(rec.UserData);
    const uint32_t pid = self->lookup_pid_for_tid(cs->new_thread_id);

    // §11.5 OPTIMIZACIÓN: drop early si el TID no está en el cache.
    // En un sistema típico ~90% de los CSwitch events son para procesos que
    // NO trackeamos (kernel threads, system services, otros procesos no-target).
    // Sus TIDs nunca llegan al tid_pid_cache_ porque no recibimos Thread Start
    // de ellos hasta DCStart rundown — y aunque lleguen, no aparecen en
    // pid_states_ así que drain_etw_ring los descartaría de todas formas.
    //
    // Skip al callback level evita:
    //   - 1× atomic fetch_add (etw_write_)
    //   - 1× memwrite al ring slot
    //   - work del drain_etw_ring posterior
    //
    // El retry-on-miss en drain (race CSwitch-vs-ThreadStart) se pierde,
    // pero ese race es < 1 ms y los pocos events perdidos no afectan la
    // métrica migrations_per_sec en agregado.
    if (pid == 0u) {
        phyriad::hal::stat_fetch_add_relaxed(self->etw_cswitch_skipped_, 1ull);
        return;
    }

    // Push al ring lock-free (manual array + atomic write cursor).
    const uint64_t w = self->etw_write_.fetch_add(1ull, std::memory_order_acq_rel);  // HAL: acq_rel fetch_add — synchronising counter
    CtxSwEvent& slot = self->etw_ring_[w & (kEtwRingCap - 1u)];
    slot.thread_id = cs->new_thread_id;
    slot.pid       = pid;
    slot.old_cpu   = 0u;   // (CSwitch event no incluye OldCpu directamente)
    slot.new_cpu   = rec.BufferContext.ProcessorIndex;
    slot.tsc       = static_cast<uint64_t>(rec.EventHeader.TimeStamp.QuadPart);

    phyriad::hal::stat_fetch_add_relaxed(self->etw_cswitch_pushed_, 1ull);
}

} // namespace phynned::observer
#endif // _WIN32
// Made with my soul - Swately <3
