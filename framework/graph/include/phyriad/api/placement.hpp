// framework/graph/include/phyriad/api/placement.hpp
// Constexpr factory helpers for PlacementHint — ergonomic API for callers who
// do not want to construct PlacementHint field-by-field.
//
// Each helper returns a fully-configured PlacementHint for a specific
// ThreadRole.  All functions are constexpr and noexcept: zero overhead at
// call sites, usable in static initialisers.
//
// Usage:
//   using namespace phyriad::api::placement;
//   PlacementHint hints[] = { ui_main(), render_pcie_gpu(0), logic(), compute_vcache() };
//   auto plan = sched.compute_placement(topo, 4, hints);
//
// Scheduler role-override summary (see Scheduler.cpp for full details):
//   ui_main()         → ThreadRole::UI_MAIN — external node, no thread spawned
//   render_pcie_gpu() → ThreadRole::RENDER  — GPU-adjacent core, +30 pcie score
//   logic()           → ThreadRole::LOGIC   — P-core preferred, no forced overrides
//   compute_vcache()  → ThreadRole::COMPUTE — V-Cache + isolated core forced
//   io()              → ThreadRole::IO      — E-cores allowed regardless of budget
//   generic()         → ThreadRole::GENERIC — no overrides, raw hint fields used
//

#pragma once
#include <phyriad/scheduler/Placement.hpp>

namespace phyriad::api::placement {

// Returns a default hint with no overrides.
[[nodiscard]] constexpr phyriad::scheduler::PlacementHint generic() noexcept
{
    return phyriad::scheduler::PlacementHint{};
}

// Returns a hint for the UI main thread node.
// GraphRuntime will NOT spawn a thread — the node is driven via
// GraphRuntime::pump_external() called from the application main loop.
[[nodiscard]] constexpr phyriad::scheduler::PlacementHint ui_main() noexcept
{
    phyriad::scheduler::PlacementHint h{};
    h.role = phyriad::scheduler::ThreadRole::UI_MAIN;
    return h;
}

// Returns a hint that co-locates the render thread with a specific GPU
// PCIe device by NUMA proximity.
//
// gpu_idx — zero-based index into the PCIe device enumeration order
//           (matching topology.pcie_affinity_map for GPU-class devices).
//           Pass 0 for the first / only GPU (default).
[[nodiscard]] constexpr phyriad::scheduler::PlacementHint render_pcie_gpu(
    uint32_t gpu_idx = 0) noexcept
{
    phyriad::scheduler::PlacementHint h{};
    h.role             = phyriad::scheduler::ThreadRole::RENDER;
    h.prefers_pcie_gpu = true;
    h.pcie_gpu_index   = gpu_idx;
    return h;
}

// Returns a hint for an application logic thread.
// Prefers P-cores; does not force V-Cache or isolation.
[[nodiscard]] constexpr phyriad::scheduler::PlacementHint logic() noexcept
{
    phyriad::scheduler::PlacementHint h{};
    h.role = phyriad::scheduler::ThreadRole::LOGIC;
    return h;
}

// Returns a hint for a compute-heavy thread that benefits from AMD 3D V-Cache.
// Scheduler forces prefer_vcache = true (+100) and prefer_isolated_core = true.
[[nodiscard]] constexpr phyriad::scheduler::PlacementHint compute_vcache() noexcept
{
    phyriad::scheduler::PlacementHint h{};
    h.role                = phyriad::scheduler::ThreadRole::COMPUTE;
    h.prefer_vcache       = true;
    h.prefer_isolated_core = true;
    return h;
}

// Returns a hint for an I/O-bound thread.
// E-cores are allowed regardless of ResourceBudget::allow_efficiency_cores.
[[nodiscard]] constexpr phyriad::scheduler::PlacementHint io() noexcept
{
    phyriad::scheduler::PlacementHint h{};
    h.role = phyriad::scheduler::ThreadRole::IO;
    return h;
}

} // namespace phyriad::api::placement
// Made with my soul - Swately <3
