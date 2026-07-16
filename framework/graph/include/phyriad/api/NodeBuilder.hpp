// framework/graph/include/phyriad/api/NodeBuilder.hpp
// Node-side DSL helpers: WirePolicy enum, HasOutput/HasInput type concepts,
// and the placement:: namespace of PlacementHint factory functions.
//
// These are building blocks consumed by DslGraphBuilder (GraphDSL.hpp).
// Users import them via `import phyriad.dsl;` or `#include <phyriad/api/GraphDSL.hpp>`.
//
// placement:: factories (all return scheduler::PlacementHint):
//   vcache_pinned()         — prefer AMD 3D V-Cache cores (+100)
//   numa(n)                 — prefer NUMA node n (+50)
//   realtime()              — set thread_priority = realtime (2)
//   gpu_affine()            — prefer PCIe GPU-adjacent core (+30)
//   isolated()              — prefer unoccupied core, penalise busy ones
//   co_locate_with(id)      — prefer same CCX/CCD as node id (+60)
//
// Combinations compose: placement::vcache_pinned() then set .prefer_numa manually.
//
// §3.H of PHASE_H_IMPLEMENTATION_PATTERNS.md

#pragma once
#include <phyriad/scheduler/Placement.hpp>
#include <cstdint>
#include <concepts>

namespace phyriad::api {

// ── WirePolicy ────────────────────────────────────────────────────────────────
// Liveness policy for each wire — maps to §1.6 of PHASE_H_ABSTRACTION_LAYERS.md.
enum class WirePolicy : uint8_t {
    StrictWaitOrEvict = 0,  // DEFAULT: evict slow consumer after timeout
    OverwriteOnDeath  = 1,  // overwrite slots on consumer death (telemetry)
    BlockUntilEvicted = 2,  // never overwrite — block writer (correctness-critical)
};

namespace detail {
// ── Internal type-probe concepts ─────────────────────────────────────────────
// Used by DslGraphBuilder::node<N>() to discover type hashes at compile time.

template <typename N>
concept HasOutputType = requires { typename N::output_type; };

template <typename N>
concept HasInputType = requires { typename N::input_type; };

} // namespace detail

} // namespace phyriad::api

// ── placement namespace ───────────────────────────────────────────────────────
// Factory functions for PlacementHint.  Lives in phyriad::placement so DSL code
// reads naturally: .node<MD>("md", placement::vcache_pinned())
namespace phyriad::placement {

[[nodiscard]] inline scheduler::PlacementHint vcache_pinned() noexcept {
    scheduler::PlacementHint h{};
    h.prefer_vcache          = true;
    return h;
}

[[nodiscard]] inline scheduler::PlacementHint numa(uint32_t node_id) noexcept {
    scheduler::PlacementHint h{};
    h.prefer_numa            = node_id;
    return h;
}

[[nodiscard]] inline scheduler::PlacementHint realtime() noexcept {
    scheduler::PlacementHint h{};
    h.prefer_realtime_priority = true;
    return h;
}

[[nodiscard]] inline scheduler::PlacementHint gpu_affine() noexcept {
    scheduler::PlacementHint h{};
    h.prefers_pcie_gpu       = true;
    return h;
}

[[nodiscard]] inline scheduler::PlacementHint isolated() noexcept {
    scheduler::PlacementHint h{};
    h.prefer_isolated_core   = true;
    return h;
}

[[nodiscard]] inline scheduler::PlacementHint co_locate_with(uint32_t node_id) noexcept {
    scheduler::PlacementHint h{};
    h.co_locate_with         = node_id;
    return h;
}

} // namespace phyriad::placement
// Made with my soul - Swately <3
