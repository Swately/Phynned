// framework/runtime/include/phyriad/runtime/GraphRuntime.hpp
// Phyriad graph runtime executor.
//
// GraphRuntime wires up a BuiltGraph (from the DSL) and executes its nodes.
// Two execution modes are supported, selected by `PerformanceProfile`:
//
//   1. Multi-threaded (legacy model — restored from core/runtime/):
//      profile.enable_cpu_pinning = true → start() spawns one std::thread per
//      non-UI_MAIN node. Each worker is pinned to its assigned core via
//      hw::pin_current_thread() and (optionally) elevated to RT priority via
//      hw::elevate_thread_rt(). Maximises throughput on multi-core hardware.
//      UI_MAIN nodes are pumped externally via pump_external(id).
//
//   2. Single-thread (Phase 1.B addition — kept for tests/correctness):
//      profile.enable_cpu_pinning = false → start() only initialises nodes;
//      caller drives the tick loop via run() on their own thread. Useful for
//      deterministic replay tests and the runtime pillar test.
//
// Lifecycle:
//   1. GraphRuntime::create(graph, node_reg, wire_reg, profile)
//      → allocates nodes + wires, connects ports, computes Scheduler placement.
//   2. GraphRuntime::start()  — calls on_start() on every node; in multi-
//      threaded mode also spawns worker threads.
//   3. Multi-threaded mode: workers run autonomously until stop().
//      Single-thread mode: caller invokes run() to tick.
//   4. GraphRuntime::stop()   — signals workers / run loop to exit.
//   5. GraphRuntime::shutdown() — joins workers, calls on_stop(), frees wires.
//
// FR-7 hot-restart (pause/resume/restart_node) works in both modes.
//
// PIMPL pattern hides Impl internals (thread pool, scheduler plan, heartbeats).
//

#pragma once
#include <phyriad/api/GraphDSL.hpp>
#include <phyriad/api/NodeRegistry.hpp>
#include <phyriad/api/WireRegistry.hpp>
#include <phyriad/graph/GraphValidator.hpp>
#include <phyriad/scheduler/Placement.hpp>
#include <phyriad/schema/Error.hpp>
#include "PerformanceProfile.hpp"
#include "Heartbeat.hpp"

#include <atomic>
#include <cstdint>
#include <expected>
#include <memory>
#include <span>
#include <thread>
#include <vector>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#endif

namespace phyriad::runtime {

// ── Forward declaration of Impl (defined in GraphRuntime.cpp) ─────────────────
class GraphRuntime {
public:
    // ── Factory ───────────────────────────────────────────────────────────────
    // Validates, allocates, and wires a complete graph.
    // Returns the running runtime on success or a ConfigError on failure.
    [[nodiscard]] static std::expected<GraphRuntime, api::ConfigError>
    create(
        api::BuiltGraph const&         graph,
        api::NodeRegistry&             node_reg,
        api::WireRegistry&             wire_reg,
        PerformanceProfile const&      profile) noexcept;

    // ── Lifecycle ─────────────────────────────────────────────────────────────
    // Definitions live in GraphRuntime.cpp where Impl is complete.
    // Do NOT add '= default' here — unique_ptr<Impl> requires Impl complete.
    GraphRuntime() noexcept;
    ~GraphRuntime() noexcept;

    GraphRuntime(GraphRuntime&&) noexcept;
    GraphRuntime& operator=(GraphRuntime&&) noexcept;

    GraphRuntime(GraphRuntime const&)            = delete;
    GraphRuntime& operator=(GraphRuntime const&) = delete;

    // Start all nodes (calls on_start() in topological order).
    [[nodiscard]] std::expected<void, Error> start() noexcept;

    // Run the tick loop on the caller's thread until stop() is called.
    // Blocks.  Returns when the last node completes its final tick.
    void run() noexcept;

    // Signal the run loop to exit.  Safe to call from any thread.
    void stop() noexcept;

    // Stop all nodes cleanly (calls on_stop() in reverse topological order).
    [[nodiscard]] std::expected<void, Error> shutdown() noexcept;

    // ── FR-7: Node hot-restart (pause / resume / restart) ─────────────────────

    /// Pause a node: its tick() is skipped by the run loop until resume_node().
    /// Thread-safe — safe to call while run() is active on another thread.
    /// Returns InvalidNodeId if id ≥ node_count().
    [[nodiscard]] std::expected<void, Error>
    pause_node(NodeId id) noexcept;

    /// Resume a previously paused node.
    /// Idempotent — safe to call even if not paused.
    /// Returns InvalidNodeId if id ≥ node_count().
    [[nodiscard]] std::expected<void, Error>
    resume_node(NodeId id) noexcept;

    /// Soft-restart: pause → on_stop() → on_start() → resume.
    /// The run loop will not tick this node during the stop/start sequence.
    /// Returns InvalidNodeId if id ≥ node_count().
    [[nodiscard]] std::expected<void, Error>
    restart_node(NodeId id) noexcept;

    /// Returns true if the node is currently paused.
    /// Returns false for out-of-range ids.
    [[nodiscard]] bool is_node_paused(NodeId id) const noexcept;

    // UI_MAIN-role nodes are NOT spawned as worker threads in multi-threaded
    // mode — the application drives them from its main loop (e.g. the GLFW
    // event-poll loop) via these methods. Each call invokes the node's tick()
    // once and updates the heartbeat counter.
    //
    // Returns InvalidNodeId if id ≥ node_count() or if the node is not
    // registered as an external (UI_MAIN) node.
    // Returns ShuttingDown if stop() has been called.
    [[nodiscard]] std::expected<void, Error>
    pump_external(NodeId id) noexcept;

    /// Pump every UI_MAIN node once. Convenience for apps with multiple
    /// external nodes; calls pump_external() on each in registration order.
    [[nodiscard]] std::expected<void, Error>
    pump_all_external() noexcept;

    // ── Observers ─────────────────────────────────────────────────────────────
    [[nodiscard]] bool running() const noexcept;
    [[nodiscard]] uint32_t node_count() const noexcept;
    [[nodiscard]] uint32_t wire_count() const noexcept;

    /// Direct access to the per-node heartbeat array (cache-line padded slots).
    /// Workers tick their own slot after every successful tick(); external
    /// monitors (orchestration::Watchdog, telemetry) read via heartbeats().read().
    [[nodiscard]] HeartbeatArray const& heartbeats() const noexcept;
    [[nodiscard]] HeartbeatArray&       heartbeats() noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace phyriad::runtime
// Made with my soul - Swately <3
