// framework/scheduler/include/phyriad/scheduler/Scheduler.hpp
// V-Cache / NUMA / PCIe-aware greedy placement engine — thin PIMPL header.
//
// Scheduler owns an Impl (defined entirely in Scheduler.cpp) so that:
//   - PlacementScore and PlacementContext never appear in binary ABI
//   - The full HardwareTopology probe result stays opaque to callers
//   - Recompiling Scheduler.cpp never forces re-link of dependent TUs
//
// Scoring summary (full details in Scheduler.cpp):
//   +100  prefer_vcache      && core carries AMD 3D V-Cache
//   + 50  prefer_numa        && core is on the preferred NUMA node
//   + 30  prefers_pcie_gpu   && GPU-class PCIe device is NUMA-adjacent to core
//   + 60  co_locate_with     && core shares CCX/CCD with the referenced node
//   - 80  anti_affinity_with && core shares CCX with a conflicting node (per entry)
//   + 20  prefer_isolated_core && core currently unoccupied
//   - 40  prefer_isolated_core && core already occupied
//
// Complexity: O(N × C) (N = node_count, C = topology.cores.size()) for SIMPLE
// hints (prefer_vcache / prefer_isolated_core, where score_core() is O(1)). The
// DENSE-hint path (co_locate_with / anti_affinity_with) makes score_core() scan
// the partial placement + cores per pair, raising the total to ~O(N²·C + N·C²)
// — MEASURED (bench_scheduler_placement §B: ns/pair rises with both N and C;
// N=1024·C=64 ≈ 55 ms). This is a BUILD-TIME / init-once path over "tens, not
// millions" of nodes (see Placement.hpp), so the super-linear term is not a
// steady-state concern at realistic graph sizes. Greedy (first-fit descending
// score), deterministic, non-allocating beyond the output PlacementPlan vector.
//

#pragma once
#include "Placement.hpp"
#include "PlacementPolicy.hpp"
#include <phyriad/topology/HardwareTopology.hpp>
#include <memory>
#include <span>

namespace phyriad::scheduler {

class Scheduler {
public:
    // Construct with an explicit policy. Default = StickyThenSteal(idle=100µs).
    explicit Scheduler(PlacementPolicy const& policy = {}) noexcept;

    // Destructor must be defined in Scheduler.cpp after Impl is complete (PIMPL rule).
    ~Scheduler();

    // Non-copyable — Impl holds mutable placement state.
    Scheduler(Scheduler const&)            = delete;
    Scheduler& operator=(Scheduler const&) = delete;

    // Movable — unique_ptr move handles Impl lifetime.
    Scheduler(Scheduler&&) noexcept;
    Scheduler& operator=(Scheduler&&) noexcept;

    // ── compute_placement ─────────────────────────────────────────────────────
    // Greedy multi-criteria placement of node_count nodes onto topology.
    //
    // hints[i] applies to node i. If hints.size() < node_count the remaining
    // nodes receive a default-constructed PlacementHint{} (all "don't care").
    //
    // budget restricts the eligible core pool (max_cores, NUMA constraints,
    // E-core exclusion). Default budget = all cores eligible.
    //
    // Returns a PlacementPlan with one NodeAssignment per node.
    // plan.is_complete == true iff every node received a valid core_id.
    // Deterministic: identical inputs always produce identical output.
    [[nodiscard]] PlacementPlan compute_placement(
        phyriad::HardwareTopology const& topology,
        uint32_t                       node_count,
        std::span<PlacementHint const> hints  = {},
        ResourceBudget const&          budget = {}) noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace phyriad::scheduler
// Made with my soul - Swately <3
