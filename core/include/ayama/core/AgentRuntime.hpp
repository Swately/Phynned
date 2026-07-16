// apps/ayama/core/include/ayama/core/AgentRuntime.hpp
// AgentRuntime — main lifecycle controller for ayama-agent.
//
// Owns the main processing loop: enumerate targets → sample metrics →
// evaluate policies → apply actions → publish to SHM.
//
// Starts with the Idle workload state and transitions based on active targets.
// Shuts down cleanly on stop() / SIGINT / WM_QUIT: reverts all actions first.
//
// Threading: AgentRuntime::run() is blocking — call from main thread.
//            stop() is signal-safe; may be called from any thread.
// Resource:  < 20 MB RSS idle, < 50 MB RSS active.
// Privilege: Admin recommended for ETW (FR-6) + cross-process affinity
//            (FR-3 hw::set_process_affinity / FR-9 hw::set_process_priority).
//            Degrades gracefully without admin (observe-only mode).
//
#pragma once

#include <ayama/core/AdaptiveTick.hpp>
#include <phyriad/schema/Error.hpp>

#include <atomic>
#include <cstdint>
#include <expected>

namespace ayama::core {

/// Configuration for AgentRuntime.
struct AgentConfig {
    /// Shared memory name published for UI clients.
    char     shm_name[40]         {"Local\\AyamaAgent.v1"};

    /// Require admin at startup; fail to start if not elevated.
    /// When false: start in degraded mode without admin capabilities.
    bool     require_admin        {false};

    /// Publish state to SHM (disable for unit tests that don't need UI).
    bool     enable_shm_publish   {true};

    /// Pin this process's own threads to "low-value" cores at startup.
    /// See AYAMA_IMPLEMENTATION_STRATEGIES.md §3.2.
    bool     self_pin_to_slow_cores {true};

    /// Start with policy application ENABLED rather than the safe-default
    /// paused state. Intended for headless / service-mode deployments
    /// where there is no UI to click Start from. The standard
    /// double-clicked-from-Explorer path leaves this false and relies on
    /// the UI's Start button to opt in.
    bool     start_active           {false};

    uint8_t  _pad[4]              {};
};
static_assert(sizeof(AgentConfig) <= 64u);

/// Main agent lifecycle class.
/// Instantiate once in main(); call start() then run().
class AgentRuntime {
public:
    explicit AgentRuntime(AgentConfig cfg = {}) noexcept;
    ~AgentRuntime() noexcept;

    AgentRuntime(AgentRuntime const&)            = delete;
    AgentRuntime& operator=(AgentRuntime const&) = delete;
    AgentRuntime(AgentRuntime&&)                 = delete;
    AgentRuntime& operator=(AgentRuntime&&)      = delete;

    /// Initialize subsystems (ETW, SHM, topology probe).
    /// On admin absence: returns Ok but logs warning; continues in degraded mode.
    [[nodiscard]] std::expected<void, phyriad::Error> start() noexcept;

    /// Blocking main loop. Returns when stop() is called or fatal error occurs.
    /// Reverts all actions before returning.
    void run() noexcept;

    /// Request graceful shutdown. Signal-safe.
    void stop() noexcept;

    // ── Introspection (for tests and UI) ──────────────────────────────────
    [[nodiscard]] bool          running()          const noexcept;
    [[nodiscard]] WorkloadState workload_state()   const noexcept;
    [[nodiscard]] uint32_t      tick_count()       const noexcept;
    [[nodiscard]] uint32_t      active_targets()   const noexcept;
    [[nodiscard]] bool          is_admin()         const noexcept;
    [[nodiscard]] bool          etw_active()       const noexcept;

private:
    struct Impl;
    Impl* impl_{nullptr};
};

} // namespace ayama::core
// Made with my soul - Swately <3
