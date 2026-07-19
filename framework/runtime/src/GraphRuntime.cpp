// framework/runtime/src/GraphRuntime.cpp
// GraphRuntime PIMPL implementation.
//
// The Impl struct holds:
//   - std::vector<NodeHandle>   — all nodes, indexed by node_id
//   - std::vector<WireHandle>   — all wires, indexed by wire_id
//   - PerformanceProfile        — tuning knobs
//   - std::atomic<bool>         — stop flag for run() AND worker loops
//   - std::vector<uint32_t>     — topological tick order
//   - HeartbeatArray            — per-node cache-line-padded heartbeat counters
//   - std::vector<std::thread>  — worker threads (multi-threaded mode only)
//   - std::vector<uint32_t>     — assigned_cores from Scheduler.compute_placement
//   - std::vector<uint8_t>      — per-node thread priorities (0=normal, 2=RT)
//   - std::vector<NodeId>       — external (UI_MAIN) node ids for pump_external
//
// Two execution modes (selected by profile.enable_cpu_pinning):
//
//   Multi-threaded (legacy model, restored):
//     start() spawns one worker thread per non-UI_MAIN node. Each worker:
//       1. Pins itself via hw::pin_current_thread(assigned_cores_[nid])
//       2. Optionally elevates to RT via hw::elevate_thread_rt() if prio>=2
//       3. Loops: tick() → heartbeats.tick() → wait_policy on RingEmpty
//       4. Honours FR-7 pause flag (skips tick when paused)
//     UI_MAIN nodes are pumped externally via pump_external().
//
//   Single-thread (Phase 1.B, preserved):
//     start() only initialises nodes; run() drives the round-robin tick loop
//     on the caller's thread. Used by runtime_test and run_graph_for/n_ticks.
//

#include <phyriad/runtime/GraphRuntime.hpp>
#include <phyriad/api/GraphDSL.hpp>
#include <phyriad/api/NodeHandle.hpp>
#include <phyriad/api/WireHandle.hpp>
#include <phyriad/api/NodeRegistry.hpp>
#include <phyriad/api/WireRegistry.hpp>
#include <phyriad/schema/Error.hpp>
#include <phyriad/schema/SchemaDescriptor.hpp>
#include <phyriad/scheduler/Scheduler.hpp>
#include <phyriad/topology/HardwareTopology.hpp>
#include <phyriad/hal/MemoryOrder.hpp>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <thread>
#include <vector>

namespace phyriad::runtime {

// ── Wait policy helper ───────────────────────────────────────────────────────
// Apply a back-pressure wait based on RingWaitMode. Hot-path conscious:
//   BUSY_SPIN — no wait (caller continues to spin)
//   PAUSE     — single PAUSE / YIELD instruction
//   YIELD     — std::this_thread::yield()
//   BACKOFF   — yield() then OS-specific SwitchToThread / sched_yield
//   SLEEPING  — sleep_for(10µs) — coarse but power-friendly
namespace {

[[gnu::always_inline]] inline void apply_wait(RingWaitMode mode) noexcept {
    switch (mode) {
        case RingWaitMode::BUSY_SPIN:
            break;  // tight loop
        case RingWaitMode::PAUSE:
#if defined(_M_X64) || defined(__x86_64__) || defined(__i386__) || defined(_M_IX86)
#  if defined(_MSC_VER)
            _mm_pause();
#  else
            __builtin_ia32_pause();
#  endif
#elif defined(__aarch64__) || defined(_M_ARM64)
            asm volatile("yield" ::: "memory");
#endif
            break;
        case RingWaitMode::YIELD:
            std::this_thread::yield();
            break;
        case RingWaitMode::BACKOFF:
#ifdef _WIN32
            SwitchToThread();
#else
            std::this_thread::yield();
#endif
            break;
        case RingWaitMode::SLEEPING:
            std::this_thread::sleep_for(std::chrono::microseconds(10));
            break;
    }
}

} // anonymous namespace

// ── Impl ──────────────────────────────────────────────────────────────────────
struct GraphRuntime::Impl {
    std::vector<api::NodeHandle>   nodes{};
    std::vector<api::WireHandle>   wires{};
    std::vector<uint32_t>          tick_order{};   // topological order
    PerformanceProfile             profile{};
    std::atomic<bool>              stop_flag{false};
    bool                           started{false};
    // FR-7: per-node pause flags — unique_ptr avoids vector realloc moving atomics.
    std::unique_ptr<std::atomic<bool>[]> paused_{};
    uint32_t                             paused_count_{0u};

    HeartbeatArray             heartbeats_{};       // per-node cache-line-padded
    std::vector<std::thread>   workers_{};          // one per non-UI_MAIN node
    std::vector<uint32_t>      assigned_cores_{};   // logical_id per node, UINT32_MAX=unpinned
    std::vector<uint8_t>       thread_priorities_{};// 0=normal, 1=high, 2=RT
    std::vector<NodeId>        external_node_ids_{};// UI_MAIN nodes (pump_external)
    std::atomic<bool>          quiescing_{false};   // signal that drain has started
    bool                       multi_threaded_{false}; // honors profile.enable_cpu_pinning

    explicit Impl(PerformanceProfile p) noexcept
        : profile(std::move(p)) {}

    ~Impl() noexcept {
        // Workers MUST already be joined by stop()/shutdown() before destruction.
        // Defensive: if user destroys without shutdown, signal + join here.
        hal::ctrl_store_release(stop_flag, true);
        for (auto& t : workers_) {
            if (t.joinable()) t.join();
        }
        // Order matters: destroy nodes BEFORE wires.
        // Inlet<T>::~Inlet() calls disconnect() → ring->unsubscribe(handle), which
        // writes to the Ring's reader-bitmap + status. If wires were freed first,
        // that unsubscribe would write into freed heap memory (use-after-free).
        for (auto& n : nodes) n.destroy();
        for (auto& w : wires) w.destroy();
    }

    // Build topological tick order via Kahn's algorithm.
    // Falls back to identity order if there are no wires (pure source graph).
    void build_tick_order(
        std::span<schema::WireDescriptor const> wire_descs,
        uint32_t node_count) noexcept
    {
        tick_order.clear();
        tick_order.reserve(node_count);

        if (node_count == 0) return;

        std::vector<uint32_t> indegree(node_count, 0u);
        std::vector<std::vector<uint32_t>> adj(node_count);

        for (auto const& w : wire_descs) {
            if (w.src_node_id < node_count && w.dst_node_id < node_count) {
                ++indegree[w.dst_node_id];
                adj[w.src_node_id].push_back(w.dst_node_id);
            }
        }

        std::vector<uint32_t> queue;
        queue.reserve(node_count);
        for (uint32_t i = 0; i < node_count; ++i)
            if (indegree[i] == 0) queue.push_back(i);

        for (std::size_t qi = 0; qi < queue.size(); ++qi) {
            const uint32_t node = queue[qi];
            tick_order.push_back(node);
            for (uint32_t dst : adj[node]) {
                if (--indegree[dst] == 0) queue.push_back(dst);
            }
        }

        // If cycle detected (queue didn't drain all nodes), fall back to identity.
        if (tick_order.size() < node_count) {
            tick_order.clear();
            for (uint32_t i = 0; i < node_count; ++i)
                tick_order.push_back(i);
        }
    }
};

// ── Factory ───────────────────────────────────────────────────────────────────
std::expected<GraphRuntime, api::ConfigError>
GraphRuntime::create(
    api::BuiltGraph const&     graph,
    api::NodeRegistry&         node_reg,
    api::WireRegistry&         wire_reg,
    PerformanceProfile const&  profile) noexcept
{
    const uint32_t n_nodes = static_cast<uint32_t>(graph.nodes.size());
    const uint32_t n_wires = static_cast<uint32_t>(graph.wires.size());

    GraphRuntime rt{};
    rt.impl_ = std::make_unique<Impl>(profile);
    auto& impl = *rt.impl_;

    // ── Instantiate nodes ─────────────────────────────────────────────────────
    impl.nodes.reserve(n_nodes);
    for (auto const& nd : graph.nodes) {
        // Name is null-terminated in the descriptor char array.
        auto result = node_reg.instantiate(
            std::string_view{nd.name, std::strlen(nd.name)},
            nd.node_id);

        if (!result) {
            return std::unexpected(api::ConfigError::make(
                api::ConfigErrorCode::UnknownNodeName,
                "Cannot instantiate node: " + std::string{nd.name}));
        }
        impl.nodes.push_back(std::move(*result));
    }

    // ── Create wire handles ───────────────────────────────────────────────────
    impl.wires.reserve(n_wires);
    for (auto const& wd : graph.wires) {
        auto result = wire_reg.create_wire(
            wd.message_hash,
            wd.wire_id,
            wd.src_node_id,
            wd.dst_node_id,
            wd.capacity == 0 ? 64u : wd.capacity);

        if (!result) {
            return std::unexpected(api::ConfigError::make(
                api::ConfigErrorCode::ValidationFailed,
                "Cannot create wire"));
        }
        impl.wires.push_back(std::move(*result));
    }

    // ── Connect ports ─────────────────────────────────────────────────────────
    // For each wire: connect outlet of src node, connect inlet of dst node.
    // NodeHandle exposes outlet_at/inlet_at via the handle's outlet_fn/inlet_fn.
    for (std::size_t wi = 0; wi < n_wires; ++wi) {
        auto const& wd   = graph.wires[wi];
        auto&       wire = impl.wires[wi];

        // Ask the src node for its outlet ptr at the given outlet index.
        if (wd.src_node_id < n_nodes) {
            void* outlet_ptr =
                impl.nodes[wd.src_node_id].outlet_at(wd.src_outlet);
            wire.connect_outlet(outlet_ptr);
        }

        // Ask the dst node for its inlet ptr at the given inlet index.
        if (wd.dst_node_id < n_nodes) {
            void* inlet_ptr =
                impl.nodes[wd.dst_node_id].inlet_at(wd.dst_inlet);
            wire.connect_inlet(inlet_ptr);
        }
    }

    // ── Build tick order ──────────────────────────────────────────────────────
    impl.build_tick_order(
        std::span<schema::WireDescriptor const>{
            graph.wires.data(), graph.wires.size()},
        n_nodes);

    // ── FR-7: per-node pause flags (all false = running) ─────────────────────
    // make_unique<T[]>(n) value-initialises; atomic<bool>{} zero-inits to false.
    impl.paused_       = std::make_unique<std::atomic<bool>[]>(n_nodes);
    impl.paused_count_ = n_nodes;

    // ── Heartbeat array — one cache-line-padded slot per node ─────────────────
    impl.heartbeats_ = HeartbeatArray{n_nodes};

    // ── Scheduler placement plan — assigns each node to a logical core ───────
    // Probe topology + run greedy V-Cache/NUMA-aware placement on the hints
    // already embedded in the graph (see api::placement::vcache_pinned() etc.).
    // Output: assigned_cores_[nid] = logical_id, thread_priorities_[nid] = 0/1/2.
    // UI_MAIN nodes are tracked separately for pump_external() dispatch.
    impl.assigned_cores_.assign(n_nodes, UINT32_MAX);
    impl.thread_priorities_.assign(n_nodes, uint8_t{0});

    if (n_nodes > 0u) {
        auto topo = phyriad::HardwareTopology::probe()
                        .value_or(phyriad::HardwareTopology{});

        phyriad::scheduler::Scheduler sched{};  // default policy = StickyThenSteal
        const auto plan = sched.compute_placement(
            topo,
            n_nodes,
            std::span<phyriad::scheduler::PlacementHint const>{
                graph.hints.data(), graph.hints.size()});

        for (auto const& a : plan.assignments) {
            if (a.node_id >= n_nodes) continue;
            impl.assigned_cores_[a.node_id]    = a.core_id;
            impl.thread_priorities_[a.node_id] = a.thread_priority;
            if (a.external) impl.external_node_ids_.push_back(a.node_id);
        }
    }

    // ── Multi-threaded mode decision ─────────────────────────────────────────
    impl.multi_threaded_ = profile.enable_cpu_pinning;

    return rt;
}

// ── Lifecycle ─────────────────────────────────────────────────────────────────
GraphRuntime::GraphRuntime() noexcept = default;
GraphRuntime::~GraphRuntime() noexcept = default;

GraphRuntime::GraphRuntime(GraphRuntime&&) noexcept = default;
GraphRuntime& GraphRuntime::operator=(GraphRuntime&&) noexcept = default;

std::expected<void, Error> GraphRuntime::start() noexcept {
    if (!impl_) return {};
    auto& impl = *impl_;

    // on_start in topological order.
    // NodeHandle::start() is void; try_start errors are silently discarded
    // inside the handle.  For production, upgrade to a returning variant.
    for (uint32_t nid : impl.tick_order) {
        if (nid < impl.nodes.size())
            impl.nodes[nid].start();
    }
    impl.started = true;

    // ── Multi-threaded mode: spawn one worker thread per non-UI_MAIN node ────
    // Single-thread mode (profile.enable_cpu_pinning == false): no workers
    // spawned; caller drives the tick loop via run() on their own thread.
    if (!impl.multi_threaded_) return {};

    const uint32_t n_nodes = static_cast<uint32_t>(impl.nodes.size());
    impl.workers_.reserve(n_nodes);

    // Identify UI_MAIN nodes — those are pumped externally, no thread spawned.
    auto is_external = [&](uint32_t nid) noexcept -> bool {
        for (NodeId ext : impl.external_node_ids_) {
            if (ext == nid) return true;
        }
        return false;
    };

    for (uint32_t nid : impl.tick_order) {
        if (nid >= n_nodes) continue;
        if (is_external(nid)) continue;  // UI_MAIN → pump_external()

        impl.workers_.emplace_back([&impl, nid]() noexcept {
            // ── Pin this thread to its scheduler-assigned core ───────────────
            const uint32_t core_id = impl.assigned_cores_[nid];
            if (core_id != UINT32_MAX) {
                (void)phyriad::hw::pin_current_thread(core_id);
            }

            // ── Elevate to RT priority if hint requested (prio==2) ───────────
            if (impl.profile.elevate_rt_priority &&
                impl.thread_priorities_[nid] >= 2u)
            {
                (void)phyriad::hw::elevate_thread_rt(/*time_critical=*/true);
            }

            // ── Worker tick loop ────────────────────────────────────────────
            // Hot loop: HAL discipline via hal::ctrl_load_acquire on stop_flag,
            // pause via hal::stat_load_relaxed for FR-7 (cheaper than acquire).
            auto& nh = impl.nodes[nid];
            const RingWaitMode wait_mode = impl.profile.ring_consumer_wait;

            while (!hal::ctrl_load_acquire(impl.stop_flag)) {
                // FR-7: honour pause flag without blocking other workers.
                if (hal::seq_load_acquire(impl.paused_[nid])) {
                    apply_wait(RingWaitMode::YIELD);
                    continue;
                }

                auto r = nh.tick();
                if (r.has_value()) {
                    impl.heartbeats_.tick(nid);
                } else {
                    const auto ec = r.error().code;
                    if (ec == ErrorCode::RingEmpty ||
                        ec == ErrorCode::RingFull   ||
                        ec == ErrorCode::InvalidHandle)
                    {
                        // Normal back-pressure — apply profile wait policy.
                        apply_wait(wait_mode);
                        continue;
                    }
                    // Fatal error — log and signal global stop.
                    std::fprintf(stderr,
                        "[GraphRuntime] node %u fatal error: code=%u\n",
                        nid, static_cast<unsigned>(ec));
                    hal::ctrl_store_release(impl.stop_flag, true);
                    break;
                }
            }
        });
    }

    return {};
}

void GraphRuntime::run() noexcept {
    if (!impl_ || !impl_->started) return;
    auto& impl = *impl_;

    // ── Multi-threaded mode: workers already tick nodes autonomously ──
    // run() must NOT also tick — that would race against worker threads on
    // shared node state. Instead block until stop_flag, then return so the
    // caller (e.g. Supervisor::start()'s monitor thread) can proceed to
    // join workers in shutdown(). Sleep 100µs between checks — low CPU
    // overhead, sub-perceptible shutdown latency.
    if (impl.multi_threaded_) {
        while (!hal::ctrl_load_acquire(impl.stop_flag)) {
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
        return;
    }

    // Do NOT reset stop_flag here.  stop() may have already set it before
    // this thread was scheduled.  Resetting would create a race where a
    // stop() call before run() starts goes unnoticed.

    // ── Outer-loop pacing (profile.target_loop_hz, 0 = unpaced) ──────────
    // Caps the whole single-threaded loop (poll → logic → render → present)
    // at a fixed rate. Without it the idle branch only yields, which on a
    // quiet box returns immediately → the loop spins a full core even when
    // the UI's data changes once per second (measured 107% of a core on the
    // Phynned dashboard; 60 Hz pacing is imperceptible there).
    using pace_clock = std::chrono::steady_clock;
    const bool paced = impl.profile.target_loop_hz > 0u;
    const auto period = paced
        ? std::chrono::nanoseconds(1'000'000'000ull /
                                   impl.profile.target_loop_hz)
        : std::chrono::nanoseconds{0};
    auto next_frame = pace_clock::now();

    while (!hal::ctrl_load_acquire(impl.stop_flag)) {
        if (paced) {
            next_frame += period;
            const auto now = pace_clock::now();
            if (next_frame > now) {
                std::this_thread::sleep_until(next_frame);
            } else {
                next_frame = now;   // fell behind — don't accumulate debt
            }
        }

        bool any_work = false;

        for (uint32_t nid : impl.tick_order) {
            if (nid >= impl.nodes.size()) continue;
            // FR-7: skip paused nodes.
            if (hal::seq_load_acquire(impl.paused_[nid])) continue;
            auto r = impl.nodes[nid].tick();
            if (r.has_value()) {
                any_work = true;
                impl.heartbeats_.tick(nid);  // emit liveness signal
            } else {
                // RingEmpty is normal back-pressure; other errors are fatal.
                if (r.error().code != ErrorCode::RingEmpty &&
                    r.error().code != ErrorCode::InvalidHandle) {
                    hal::ctrl_store_release(impl.stop_flag, true);
                    return;
                }
            }
        }

        // If no node produced work, yield to avoid 100% CPU on idle.
        if (!any_work) {
#ifdef _WIN32
            SwitchToThread();
#else
            std::this_thread::yield();
#endif
        }
    }
}

void GraphRuntime::stop() noexcept {
    if (!impl_) return;
    // Use HAL release-store so workers observe via ctrl_load_acquire on next iter.
    hal::ctrl_store_release(impl_->stop_flag, true);
    hal::ctrl_store_release(impl_->quiescing_, true);
}

std::expected<void, Error> GraphRuntime::shutdown() noexcept {
    if (!impl_) return {};
    auto& impl = *impl_;

    // ── Multi-threaded mode: signal stop and join workers ──────────────────
    // Workers may still be in their tick loop; ensure stop_flag is set then
    // join. Safe to call shutdown() multiple times — workers_ becomes empty
    // after the first join pass.
    hal::ctrl_store_release(impl.stop_flag, true);
    for (auto& t : impl.workers_) {
        if (t.joinable()) t.join();
    }
    impl.workers_.clear();

    // on_stop in reverse topological order — must run AFTER workers join so
    // the node's resources aren't released while its worker still touches them.
    for (auto it = impl.tick_order.rbegin(); it != impl.tick_order.rend(); ++it) {
        uint32_t nid = *it;
        if (nid < impl.nodes.size())
            impl.nodes[nid].stop();
    }
    impl.started = false;
    return {};
}

// ── FR-7: Node hot-restart ────────────────────────────────────────────────────
std::expected<void, Error> GraphRuntime::pause_node(NodeId id) noexcept {
    if (!impl_ || id >= impl_->paused_count_)
        return std::unexpected(Error{ErrorCode::InvalidNodeId});
    hal::seq_store_release(impl_->paused_[id], true);
    return {};
}

std::expected<void, Error> GraphRuntime::resume_node(NodeId id) noexcept {
    if (!impl_ || id >= impl_->paused_count_)
        return std::unexpected(Error{ErrorCode::InvalidNodeId});
    hal::seq_store_release(impl_->paused_[id], false);
    return {};
}

std::expected<void, Error> GraphRuntime::restart_node(NodeId id) noexcept {
    if (!impl_ || id >= impl_->paused_count_)
        return std::unexpected(Error{ErrorCode::InvalidNodeId});

    auto& impl = *impl_;
    // Pause first so the run loop skips tick() during stop/start.
    impl.paused_[id].store(true, std::memory_order_seq_cst);  // HAL: seq_cst — cross-thread handshake
    // on_stop then on_start while the run loop skips this node.
    impl.nodes[id].stop();
    impl.nodes[id].start();
    // Resume — node re-enters the tick loop on the next iteration.
    hal::seq_store_release(impl.paused_[id], false);
    return {};
}

bool GraphRuntime::is_node_paused(NodeId id) const noexcept {
    if (!impl_ || id >= impl_->paused_count_) return false;
    return hal::stat_load_relaxed(impl_->paused_[id]);
}

// ── Observers ─────────────────────────────────────────────────────────────────
bool GraphRuntime::running() const noexcept {
    if (!impl_) return false;
    // HAL discipline: ctrl_load_acquire matches stop()/shutdown() release-store.
    return impl_->started &&
           !hal::ctrl_load_acquire(impl_->stop_flag);
}

uint32_t GraphRuntime::node_count() const noexcept {
    return impl_ ? static_cast<uint32_t>(impl_->nodes.size()) : 0u;
}

uint32_t GraphRuntime::wire_count() const noexcept {
    return impl_ ? static_cast<uint32_t>(impl_->wires.size()) : 0u;
}

// In multi-threaded mode the runtime does NOT spawn a worker thread for nodes
// whose PlacementHint.role == ThreadRole::UI_MAIN. Those nodes are driven from
// the application's main loop (typically GLFW poll + render loop) via these
// helpers. Each call invokes the node's tick() once and updates its heartbeat.
//
// Concurrency: pump_external() is intentionally NOT thread-safe — UI_MAIN nodes
// are expected to be ticked from a single application thread. The runtime's
// stop_flag and quiescing_ are checked HAL-acquire so a stop() from any thread
// promptly causes pump_external to return ShuttingDown.

std::expected<void, Error> GraphRuntime::pump_external(NodeId id) noexcept {
    if (!impl_) {
        return std::unexpected(Error{
            .code = ErrorCode::InvalidHandle,
            .source_node_id = id, .timestamp_ns = 0});
    }
    auto& impl = *impl_;

    if (hal::ctrl_load_acquire(impl.quiescing_)) {
        return std::unexpected(Error{
            .code = ErrorCode::ShuttingDown,
            .source_node_id = id, .timestamp_ns = 0});
    }

    // Verify id is registered as an external (UI_MAIN) node.
    bool found_external = false;
    for (NodeId ext : impl.external_node_ids_) {
        if (ext == id) { found_external = true; break; }
    }
    if (!found_external) {
        return std::unexpected(Error{
            .code = ErrorCode::InvalidNodeId,
            .source_node_id = id, .timestamp_ns = 0});
    }

    if (id >= impl.nodes.size()) {
        return std::unexpected(Error{
            .code = ErrorCode::InvalidNodeId,
            .source_node_id = id, .timestamp_ns = 0});
    }

    // FR-7: paused external nodes silently skip pump (no error).
    if (hal::seq_load_acquire(impl.paused_[id])) return {};

    auto r = impl.nodes[id].tick();
    if (!r) {
        const auto ec = r.error().code;
        // Empty / full transports are normal — caller can poll again.
        if (ec == ErrorCode::RingEmpty || ec == ErrorCode::RingFull) return {};
        // Fatal — signal global stop and surface the error.
        hal::ctrl_store_release(impl.stop_flag, true);
        return std::unexpected(r.error());
    }

    impl.heartbeats_.tick(id);
    return {};
}

std::expected<void, Error> GraphRuntime::pump_all_external() noexcept {
    if (!impl_) {
        return std::unexpected(Error{
            .code = ErrorCode::InvalidHandle,
            .source_node_id = 0, .timestamp_ns = 0});
    }
    auto& impl = *impl_;

    if (hal::ctrl_load_acquire(impl.quiescing_)) {
        return std::unexpected(Error{
            .code = ErrorCode::ShuttingDown,
            .source_node_id = 0, .timestamp_ns = 0});
    }

    for (NodeId id : impl.external_node_ids_) {
        auto r = pump_external(id);
        if (!r) {
            // Propagate first fatal error; transient RingEmpty/Full are masked
            // inside pump_external() so they don't surface here.
            return r;
        }
    }
    return {};
}

// Provides direct access to the per-node heartbeat counter array. External
// monitors (orchestration::Watchdog, telemetry exporters, liveness probes)
// read counters via heartbeats().read(idx) without disturbing the hot path.
HeartbeatArray const& GraphRuntime::heartbeats() const noexcept {
    // Pre-condition: caller has a valid GraphRuntime (impl_ non-null).
    // If impl_ is null (default-constructed runtime), return a static empty
    // array — safer than dereferencing nullptr.
    static const HeartbeatArray empty{};
    return impl_ ? impl_->heartbeats_ : empty;
}

HeartbeatArray& GraphRuntime::heartbeats() noexcept {
    static HeartbeatArray empty{};
    return impl_ ? impl_->heartbeats_ : empty;
}

} // namespace phyriad::runtime
// Made with my soul - Swately <3
