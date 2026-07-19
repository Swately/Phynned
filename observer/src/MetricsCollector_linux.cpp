// observer/src/MetricsCollector_linux.cpp
// MetricsCollector — Linux implementation.
//
// Uses phyriad::proc::ProcessMetricsSnapshot (FR-11) for bulk CPU/memory
// metrics (reads /proc/<pid>/stat for each tracked PID).
//
// ETW is Windows-only; context-switch migration tracking on Linux is a
// stub (planned: perf_event_open in Phase 2).
//
#ifndef _WIN32

#include <phynned/observer/MetricsCollector.hpp>
#include <phyriad/schema/Error.hpp>

#include <cstdio>
#include <cstring>
#include <time.h>   // clock_gettime(CLOCK_MONOTONIC)

namespace phynned::observer {

// ── Constructor / Destructor ──────────────────────────────────────────────
MetricsCollector::MetricsCollector() noexcept {
    etw_ring_.fill(CtxSwEvent{});
    pid_states_.fill(PidState{});
}

MetricsCollector::~MetricsCollector() noexcept {
    stop();
}

// ── start() ───────────────────────────────────────────────────────────────
std::expected<void, phyriad::Error> MetricsCollector::start() noexcept {
    // ETW is Windows-only; leave etw_active_ = false on Linux.
    etw_active_ = false;
    frame_obs_  = false;

    // ProcessMetricsSnapshot (FR-11) — /proc-based bulk capture.
    auto snap = phyriad::proc::ProcessMetricsSnapshot::create();
    if (snap.has_value()) {
        snapshot_ = std::move(*snap);
        // Warmup: populates internal buffer so the first hot-path tick doesn't
        // pay the /proc walk overhead at an unexpected moment.
        (void)snapshot_->capture();
    } else {
        std::fprintf(stderr,
            "[MetricsCollector] ProcessMetricsSnapshot::create() failed "
            "(code=%u) — sample() will produce zero metrics.\n",
            static_cast<unsigned>(snap.error().code));
    }
    return {};
}

// ── stop() ────────────────────────────────────────────────────────────────
void MetricsCollector::stop() noexcept {
    snapshot_.reset();
    etw_active_ = false;
}

// ── PidState management ───────────────────────────────────────────────────
// Consistent with win32 — hash lookup for O(1) avg.
// (drain_etw_ring is a no-op on Linux but pid_hash_ still keeps sample()
//  fast for symmetry and to avoid divergence.)
MetricsCollector::PidState*
MetricsCollector::find_or_create_pid_state(uint32_t pid) noexcept {
    if (pid == 0u) return nullptr;

    const int32_t slot = pid_hash_lookup(pid);
    if (slot >= 0) {
        PidState& s = pid_states_[static_cast<uint32_t>(slot)];
        if (s.valid && s.pid == pid) return &s;
    }

    if (n_pid_states_ >= kMaxPidStates) return nullptr;
    const uint32_t new_idx = n_pid_states_++;
    PidState& s = pid_states_[new_idx];
    s = PidState{};
    s.pid   = pid;
    s.valid = true;
    pid_hash_insert(pid, static_cast<uint16_t>(new_idx));
    return &s;
}

// ── ETW ring drain — no-op on Linux ──────────────────────────────────────
void MetricsCollector::drain_etw_ring() noexcept {}

// ── sample() ──────────────────────────────────────────────────────────────
void MetricsCollector::sample(const uint32_t* pids, uint32_t n,
                               TargetMetrics* out_metrics) noexcept {
    if (!pids || !out_metrics || n == 0u) return;

    // ── 1. Bulk capture (FR-11) ───────────────────────────────────────────
    // Throttled to every kBulkCaptureInterval ticks
    // (~500 ms) to cut the /proc walk cost. On intermediate ticks we emit
    // cached CPU%/thread_count from PidState.
    const bool do_capture =
        (++bulk_capture_counter_ >= kBulkCaptureInterval);
    if (do_capture) bulk_capture_counter_ = 0u;

    // Off-stack scratch (Impl member) — avoids a 64 KB stack frame at MASS cap.
    phyriad::proc::ProcessMetrics* bulk = bulk_scratch_.data();
    bool snapshot_ok = false;

    if (do_capture && snapshot_.has_value()) {
        if (snapshot_->capture().has_value()) {
            (void)snapshot_->extract(pids, n, bulk);
            snapshot_ok = true;
        }
    }

    // ── 2. Wall-clock denominator (CLOCK_MONOTONIC, nanoseconds) ─────────
    struct timespec ts_now{};
    clock_gettime(CLOCK_MONOTONIC, &ts_now);
    // Express in 100ns units for consistency with kernel_time_100ns.
    const uint64_t wall_now_100ns =
        static_cast<uint64_t>(ts_now.tv_sec) * 10'000'000ull +
        static_cast<uint64_t>(ts_now.tv_nsec) / 100ull;

    // ── 3. Fill TargetMetrics per-PID ─────────────────────────────────────
    for (uint32_t i = 0u; i < n; ++i) {
        const uint32_t pid = pids[i];
        TargetMetrics& out = out_metrics[i];
        std::memset(&out, 0, sizeof(out));
        out.pid = pid;

        PidState* state = find_or_create_pid_state(pid);
        if (!state) continue;

        if (snapshot_ok) {
            const phyriad::proc::ProcessMetrics& pm = bulk[i];

            // CPU % from 100ns-unit deltas
            const uint64_t cpu_delta_100ns =
                (pm.kernel_time_100ns - state->prev_kernel_100ns) +
                (pm.user_time_100ns   - state->prev_user_100ns);

            if (state->prev_wall_ticks != 0u) {
                const uint64_t wall_delta_100ns =
                    wall_now_100ns - state->prev_wall_ticks;

                out.cpu_usage_pct = (wall_delta_100ns > 0u)
                    ? static_cast<float>(
                        static_cast<double>(cpu_delta_100ns) /
                        static_cast<double>(wall_delta_100ns) * 100.0)
                    : 0.f;
                if (out.cpu_usage_pct > 100.f * 256.f)
                    out.cpu_usage_pct = 0.f;  // clamp on bad delta
            }

            state->prev_kernel_100ns = pm.kernel_time_100ns;
            state->prev_user_100ns   = pm.user_time_100ns;
            state->prev_wall_ticks   = wall_now_100ns;  // only on capture
            out.observed_threads     = pm.thread_count;

            state->cached_cpu_pct       = out.cpu_usage_pct;
            state->cached_thread_count  = pm.thread_count;
        } else {
            // Non-capture tick: emit cached values.
            out.cpu_usage_pct    = state->cached_cpu_pct;
            out.observed_threads = state->cached_thread_count;
        }

        // Migration rate — perf_event_open stub; always 0 on Linux for now.
        const uint32_t mig_delta = state->migration_count
                                 - state->prev_migration_count;
        out.migrations_per_sec       = mig_delta;
        state->prev_migration_count  = state->migration_count;

        // Pressure heuristic (CPU-only on Linux, no ETW migration data)
        if (out.cpu_usage_pct > 80.f) {
            out.pressure_level = 2u;
        } else if (out.cpu_usage_pct > 50.f) {
            out.pressure_level = 1u;
        } else {
            out.pressure_level = 0u;
        }
    }
}

} // namespace phynned::observer
#endif // !_WIN32
// Made with my soul - Swately <3
