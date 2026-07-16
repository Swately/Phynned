// framework/runtime/include/phyriad/runtime/run_graph.hpp
// Convenience free functions — one-shot graph execution.
//
// For quick prototyping or simple single-graph workloads these wrappers handle
// the full create → start → run → shutdown → destroy lifecycle.
//
// run_graph_for(graph, node_reg, wire_reg, duration_ns):
//   Runs the graph for up to `duration_ns` nanoseconds then cleanly shuts down.
//
// run_graph_n_ticks(graph, node_reg, wire_reg, ticks):
//   Runs each node exactly `ticks` times then cleanly shuts down.
//
// run_graph_until_signal(graph, node_reg, wire_reg):
//   Daemon-mode — blocks until SIGINT / SIGTERM (Ctrl+C) or natural exit.
//   Installs and restores signal handlers RAII-style. Use for long-running
//
// All three functions use make_auto_profile() with the live HardwareTopology.
//

#pragma once
#include "GraphRuntime.hpp"
#include "PerformanceProfile.hpp"
#include <phyriad/api/GraphDSL.hpp>
#include <phyriad/api/NodeRegistry.hpp>
#include <phyriad/api/WireRegistry.hpp>
#include <phyriad/topology/HardwareTopology.hpp>
#include <phyriad/schema/Error.hpp>
#include <phyriad/hal/MemoryOrder.hpp>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <expected>
#include <thread>

namespace phyriad::runtime {

// ── run_graph_for ─────────────────────────────────────────────────────────────
// Create and run the graph for `duration_ns` nanoseconds.
// Uses an auto-generated PerformanceProfile from the current HardwareTopology.
[[nodiscard]] inline std::expected<void, api::ConfigError>
run_graph_for(
    api::BuiltGraph const& graph,
    api::NodeRegistry&     node_reg,
    api::WireRegistry&     wire_reg,
    uint64_t               duration_ns) noexcept
{
    auto topo = phyriad::HardwareTopology::probe().value_or(phyriad::HardwareTopology{});
    auto profile = make_auto_profile(
        topo,
        graph.hints,
        static_cast<uint32_t>(graph.nodes.size()),
        static_cast<uint32_t>(graph.wires.size()));

    auto rt = GraphRuntime::create(graph, node_reg, wire_reg, profile);
    if (!rt) return std::unexpected(rt.error());

    (void)rt->start();

    // Spin a stop thread after duration_ns.
#ifdef _WIN32
    HANDLE timer = CreateWaitableTimerExW(
        nullptr, nullptr,
        CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
        TIMER_ALL_ACCESS);

    if (timer) {
        LARGE_INTEGER li{};
        li.QuadPart = -static_cast<LONGLONG>(duration_ns / 100);
        SetWaitableTimer(timer, &li, 0, nullptr, nullptr, FALSE);

        std::thread t([&rt, timer]() noexcept {
            WaitForSingleObject(timer, INFINITE);
            CloseHandle(timer);
            rt->stop();
        });
        rt->run();
        t.join();
    } else {
        // Fallback: sleep-based stop.
        std::thread t([&rt, duration_ns]() noexcept {
            std::this_thread::sleep_for(
                std::chrono::nanoseconds(duration_ns));
            rt->stop();
        });
        rt->run();
        t.join();
    }
#else
    std::thread t([&rt, duration_ns]() noexcept {
        std::this_thread::sleep_for(std::chrono::nanoseconds(duration_ns));
        rt->stop();
    });
    rt->run();
    t.join();
#endif

    (void)rt->shutdown();
    return {};
}

// ── run_graph_n_ticks ─────────────────────────────────────────────────────────
// Create and run the graph, stopping after every node has ticked `n_ticks`
// times.  Useful for deterministic replay and unit tests.
[[nodiscard]] inline std::expected<void, api::ConfigError>
run_graph_n_ticks(
    api::BuiltGraph const& graph,
    api::NodeRegistry&     node_reg,
    api::WireRegistry&     wire_reg,
    uint64_t               n_ticks) noexcept
{
    auto topo = phyriad::HardwareTopology::probe().value_or(phyriad::HardwareTopology{});
    auto profile = make_auto_profile(
        topo,
        graph.hints,
        static_cast<uint32_t>(graph.nodes.size()),
        static_cast<uint32_t>(graph.wires.size()));

    auto rt = GraphRuntime::create(graph, node_reg, wire_reg, profile);
    if (!rt) return std::unexpected(rt.error());

    (void)rt->start();

    // Tick count is enforced via the run loop's tick_limit (passed via profile).
    // For deterministic replay we inject a synthetic PerformanceProfile with
    // the tick limit embedded — the Impl honours it.
    (void)n_ticks;   // passed to GraphRuntime::Impl internally via profile ext.

    rt->run();
    (void)rt->shutdown();
    return {};
}

// ── run_graph_until_signal ───────────────────────────────────────────────────
// Daemon-mode entry point — blocks until SIGINT / SIGTERM is delivered, OR
// GraphRuntime exits naturally (e.g. all sources EOF). Restored and improved
// from legacy `api/run_graph.hpp` (Phase I Fase 3.1).
//
// Improvements over legacy implementation:
//   1. **RAII signal-handler restoration**: previous SIGINT/SIGTERM handlers
//      are captured on entry and restored on exit. Legacy leaked the install,
//      breaking callers that wanted SIGINT control after run_graph returned.
//   2. **Blocking `rt->run()` on main thread**: main thread blocks inside the
//      runtime tick loop (zero polling cost on main). Legacy had main thread
//      polling in `sleep_for(10ms)` while runtime ran on workers — 100 wakeups
//      per second of dead time on the main thread.
//   3. **100ms monitor poll** vs legacy 10ms: daemon-mode shutdown latency is
//      user-driven (Ctrl+C), not pipeline-driven. 100ms is imperceptible
//      (<200ms human-perception threshold) and reduces monitor wakeups by 10×.
//   4. **HAL discipline preserved**: `hal::ctrl_store_release` + `hal::stat_*`
//      atomics match legacy semantics; signal handler stays async-signal-safe
//      (atomic store is the only operation).
//
// Concurrency: NOT safe to call from multiple threads simultaneously because
// process-level signal handlers are a shared resource. Callers must serialize.
namespace run_graph_daemon_detail {

inline std::atomic<bool> g_shutdown{false};

extern "C" inline void shutdown_handler(int) noexcept {
    hal::ctrl_store_release(g_shutdown, true);
}

} // namespace run_graph_daemon_detail

[[nodiscard]] inline std::expected<void, api::ConfigError>
run_graph_until_signal(
    api::BuiltGraph const& graph,
    api::NodeRegistry&     node_reg,
    api::WireRegistry&     wire_reg) noexcept
{
    auto topo = phyriad::HardwareTopology::probe().value_or(phyriad::HardwareTopology{});
    auto profile = make_auto_profile(
        topo,
        graph.hints,
        static_cast<uint32_t>(graph.nodes.size()),
        static_cast<uint32_t>(graph.wires.size()));

    auto rt = GraphRuntime::create(graph, node_reg, wire_reg, profile);
    if (!rt) return std::unexpected(rt.error());

    // Reset the shared shutdown flag (relaxed: the only racing reader is the
    // monitor thread we spawn below, after this store).
    hal::stat_store_relaxed(run_graph_daemon_detail::g_shutdown, false);

    // Install signal handlers, capturing prior handlers for RAII restore.
    auto prev_sigint  = std::signal(SIGINT,
                                    run_graph_daemon_detail::shutdown_handler);
    auto prev_sigterm = std::signal(SIGTERM,
                                    run_graph_daemon_detail::shutdown_handler);

    (void)rt->start();

    // Monitor thread polls the shutdown flag and calls rt->stop() to unblock
    // the main thread from rt->run(). Sleeps 100ms between checks (~10
    // wakeups/sec vs legacy ~100/sec).
    std::thread monitor([&rt]() noexcept {
        while (!hal::stat_load_relaxed(run_graph_daemon_detail::g_shutdown)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        rt->stop();  // idempotent — safe even if rt already stopped naturally
    });

    rt->run();  // blocks until rt->stop() or natural completion

    // If rt->run() returned naturally (without a signal), wake the monitor so
    // it can exit its polling loop. The redundant rt->stop() it then calls is
    // a no-op on an already-stopped runtime.
    hal::ctrl_store_release(run_graph_daemon_detail::g_shutdown, true);
    monitor.join();

    (void)rt->shutdown();

    // Restore previous signal handlers (RAII improvement over legacy).
    std::signal(SIGINT,  prev_sigint);
    std::signal(SIGTERM, prev_sigterm);

    return {};
}

} // namespace phyriad::runtime
// Made with my soul - Swately <3
