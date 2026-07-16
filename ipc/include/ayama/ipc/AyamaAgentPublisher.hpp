// apps/ayama/ipc/include/ayama/ipc/AyamaAgentPublisher.hpp
// AyamaAgentPublisher — write side of the agent's shared memory segment.
//
// Creates the named FileMapping (Windows) / POSIX shared memory and publishes
// TargetProcess[], TargetMetrics[], PolicyDecision[], and AyamaStateHeader
// each tick via a seqlock.
//
// The ActionLogRing in SHM is kept up-to-date by draining new entries from
// the ActionExecutor's internal log into SHM — no full-ring memcpy per tick.
//
// Seqlock pattern:
//   shm_write_begin() → bulk memcpy → shm_write_end()
//   UI readers spin-retry when seq is odd or changes mid-read.
//
// Threading: all methods called from the agent main thread only (one writer).
// Privilege: CreateFileMapping / shm_open — no admin required.
// Resource:  one 1 MB mapping for the lifetime of the agent.
//
#pragma once

#include <ayama/ipc/AyamaProtocol.hpp>
#include <ayama/action/ActionLog.hpp>
#include <phyriad/schema/Error.hpp>

#include <cstdint>
#include <expected>

namespace ayama::ipc {

class AyamaAgentPublisher {
public:
    AyamaAgentPublisher()  noexcept = default;
    ~AyamaAgentPublisher() noexcept { close(); }

    AyamaAgentPublisher(AyamaAgentPublisher const&)            = delete;
    AyamaAgentPublisher& operator=(AyamaAgentPublisher const&) = delete;
    AyamaAgentPublisher(AyamaAgentPublisher&&)                 = delete;
    AyamaAgentPublisher& operator=(AyamaAgentPublisher&&)      = delete;

    // ── Lifecycle ─────────────────────────────────────────────────────────

    /// Create the named shared memory and initialise the header.
    /// `agent_pid` is written into AyamaShmHeader::agent_pid.
    /// Idempotent: returns Ok immediately if already open.
    [[nodiscard]] std::expected<void, phyriad::Error>
    open(const char* shm_name, uint32_t agent_pid) noexcept;

    /// Flush `agent_connected = 0`, unmap the view, and release the handle.
    /// Safe to call on a publisher that was never opened.
    void close() noexcept;

    [[nodiscard]] bool is_open() const noexcept { return layout_ != nullptr; }

    // ── Per-tick publish ──────────────────────────────────────────────────

    /// Publish a complete snapshot.  Must be called from the agent thread.
    ///
    /// Drains any new ActionLogEntries from `exec_log` into the SHM ring first
    /// (before the seqlock window), then seqlock-copies targets/metrics/
    /// decisions/state.
    ///
    /// Parameters:
    ///   privilege_level   — 0=none, 1=admin-no-ETW, 2=admin+ETW
    ///   etw_active        — 1 if ETW session is running
    ///   targets           — pointer to TargetProcess array (n_targets entries)
    ///   metrics           — pointer to TargetMetrics array (n_targets entries, may be null)
    ///   decisions         — pointer to PolicyDecision array (n_decisions entries, may be null)
    ///   n_active_actions  — count of currently active (unrevereted) actions
    ///   total_migrations  — sum of migrations_per_sec across all targets
    ///   aggregate_pressure — 0.0..1.0 average pressure across targets
    ///   now_tsc           — current TSC (stored as last_publish_tsc)
    ///   exec_log          — ActionExecutor's internal log ring (read-only cursor drain)
    ///   bad_count         — PerGameMemory bad-list entry count
    ///   deep_idle         — 1 if WorkloadState::DeepIdle
    ///   watchdog_ok       — 1 normally; 0 if internal watchdog detected a stall
    ///   ccd_defense_count — # processes evicted from V-Cache CCD
    ///                       (Rule 8) in the most recent evaluate() cycle
    ///   ccd_defense_cpu_pct — summed CPU% of those evicted processes
    void publish(
        uint8_t                         privilege_level,
        uint8_t                         etw_active,
        const observer::TargetProcess*  targets,
        uint32_t                        n_targets,
        const observer::TargetMetrics*  metrics,
        const policy::PolicyDecision*   decisions,
        uint32_t                        n_decisions,
        uint32_t                        n_active_actions,
        uint32_t                        total_migrations,
        float                           aggregate_pressure,
        uint64_t                        now_tsc,
        const action::ActionLogRing&    exec_log,
        uint32_t                        bad_count,
        uint8_t                         deep_idle,
        uint8_t                         watchdog_ok,
        uint32_t                        ccd_defense_count,
        float                           ccd_defense_cpu_pct
    ) noexcept;

    /// Write bench_phase into the state header (called by ABRunner integration).
    /// 0=idle, 1=phase-A running, 2=phase-B running, 3=done.
    void set_bench_phase(uint8_t phase) noexcept;

    /// Write hardware classification into the state header.
    /// Called once at agent startup (post-topology-probe). Values are static.
    ///
    /// cpu_class     — policy::CpuClass enum value (0..5)
    /// ccd_count     — # of CCDs (Intel = 1)
    /// p_core_count  — # P-cores (Intel hybrid only; 0 elsewhere)
    /// e_core_count  — # E-cores (Intel hybrid only; 0 elsewhere)
    void set_hw_classification(uint8_t cpu_class,
                               uint8_t ccd_count,
                               uint8_t p_core_count,
                               uint8_t e_core_count) noexcept;

    /// Write the policies_paused flag into the state header. Called by
    /// AgentRuntime whenever the gate flips (Pause / Resume / ForceRevert
    /// IPC command, --start-active CLI flag at startup, etc.). Lets the
    /// UI render Start vs Pause button state accurately.
    void set_policies_paused(uint8_t paused) noexcept;

    /// Write the agent's own resource usage into the state header.
    /// Called by AgentRuntime after each SelfMonitor sample (~500 ms).
    /// Surfaces in the UI status bar so users can confirm Ayama isn't
    /// burning CPU on their machine.
    void set_self_resources(float cpu_pct, float rss_mb) noexcept;

    /// Direct pointer to the command slot.
    /// Used by the agent main loop to poll for new UI commands each tick.
    /// Returns nullptr if not open. NOT seqlock-protected — synchronization
    /// is done via the slot's own atomic seq/ack fields.
    [[nodiscard]] AyamaCommandSlot* command_slot() noexcept {
        return layout_ ? &layout_->command_slot : nullptr;
    }

private:
    AyamaShmLayout* layout_{nullptr};

    /// Tracks how many entries have already been drained from exec_log.
    uint64_t action_read_cursor_{0ull};

    void mark_disconnected() noexcept;

#ifdef _WIN32
    void* file_handle_{nullptr};
    void* map_handle_ {nullptr};
#else
    int  shm_fd_  {-1};
    char shm_name_[64]{};
#endif
};

} // namespace ayama::ipc
// Made with my soul - Swately <3
