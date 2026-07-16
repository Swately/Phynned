// framework/scheduler/include/phyriad/scheduler/Placement.hpp
// Per-node placement hints and output assignment types consumed by Scheduler.
//
// PlacementHint — caller-supplied per-node preferences (all optional).
//   Scores applied by the greedy algorithm in Scheduler.cpp:
//     prefer_vcache             → +100 if core has AMD 3D V-Cache
//     prefer_numa               → + 50 if core is on the specified NUMA node
//     prefers_pcie_gpu          → + 30 if a GPU-class PCIe device is NUMA-adjacent
//     co_locate_with            → + 60 if core shares CCX/CCD with referenced node
//     anti_affinity_with[k]     → - 80 per matching node on same CCX
//     prefer_isolated_core      → + 20 if core unoccupied, - 40 if already in use
//
// ThreadRole — semantic role of the node's thread.
//   Scheduler applies role-specific overrides on top of per-field hints:
//     UI_MAIN  → external node (GraphRuntime does not spawn a thread)
//     RENDER   → forces prefers_pcie_gpu = true (+30 GPU-proximity score)
//     COMPUTE  → forces prefer_vcache = true (+100) + prefer_isolated_core = true
//     IO       → overrides ResourceBudget to allow E-cores even when excluded
//
// ResourceBudget — constraints on the eligible core pool.
//
// NodeAssignment — one per node in the output PlacementPlan:
//   core_id         → logical core ID for SetThreadAffinityMask / pthread_setaffinity_np
//   numa_node       → NUMA node of the assigned core (for arena allocation)
//   thread_priority → 0=normal, 1=high, 2=realtime
//   is_hard_pinned  → false for SoftAffine policy (advisory, OS may migrate)
//   external        → true iff GraphRuntime must NOT spawn a thread for this node
//   irq_routing     → NUMA node hint for MSI-X/IRQ routing; UINT32_MAX = no hint
//
// PlacementPlan — vector of NodeAssignment + is_complete flag.
//

#pragma once
#include <cstdint>
#include <type_traits>
#include <vector>

namespace phyriad::scheduler {

// ── ThreadRole ────────────────────────────────────────────────────────────────
// Semantic role of the node's execution thread. Informs both Scheduler
// (core selection) and GraphRuntime (thread spawn vs. external pump,
// Watchdog timeout selection).
//
// Watchdog timeout mapping (applied in GraphRuntime::start()):
//   GENERIC → 5 ms   UI_MAIN → 50 ms   RENDER → 33 ms
//   LOGIC   → 5 ms   COMPUTE → 10 ms   IO     → 100 ms
enum class ThreadRole : uint8_t {
    GENERIC = 0,   // default — no special treatment
    UI_MAIN = 1,   // external node: GraphRuntime registers for pump_external(), no thread
    RENDER  = 2,   // high-priority; GPU-adjacent core preferred; Watchdog 33 ms
    LOGIC   = 3,   // P-core preferred; no V-Cache; Watchdog 5 ms
    COMPUTE = 4,   // V-Cache + isolated core forced; Watchdog 10 ms
    IO      = 5,   // E-core allowed regardless of ResourceBudget; Watchdog 100 ms
};

// ── CcdPreference ─────────────────────────────────────────────────────────────
// Requested CCD-level filter applied before per-core scoring.
// Narrows the candidate pool to the preferred CCD; falls back to all cores
// if the preferred CCD has no eligible candidates under the ResourceBudget.
//
//   None        → no CCD filter; scoring runs on all eligible cores (default).
//   PreferVCache→ restrict candidates to the CCD carrying AMD 3D V-Cache.
//                 On non-X3D CPUs or CPUs with no V-Cache cores, degrades to None.
//   PreferFreq  → restrict candidates to the CCD with the highest average
//                 max_freq_mhz. On CPUs with a single CCD, degrades to None.
//

enum class CcdPreference : uint8_t {
    None        = 0,
    PreferVCache= 1,
    PreferFreq  = 2,
};

// ── PlacementHint ─────────────────────────────────────────────────────────────
// All fields default to "don't care". Unused fields cost nothing in scoring.
struct PlacementHint {
    // Flag-based preferences (1B each, 4 booleans = 4B, naturally aligns prefer_numa).
    bool     prefer_vcache{false};            // +100: prefer AMD 3D V-Cache cores
    bool     prefer_isolated_core{false};     // +20/-40: penalize already-occupied cores
    bool     prefers_pcie_gpu{false};         // +30: co-locate with GPU PCIe device
    bool     prefer_realtime_priority{false}; // output NodeAssignment.thread_priority = 2

    uint32_t prefer_numa{UINT32_MAX};         // +50: preferred NUMA node (UINT32_MAX = any)
    uint32_t co_locate_with{UINT32_MAX};      // +60: node_id to share CCX/CCD with

    // Up to 4 nodes that this node should avoid sharing a CCX with (-80 each).
    uint32_t anti_affinity_with[4]{
        UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX
    };

    // PCIe GPU index for co-location (used by phyriad::placement:: factories).
    // UINT32_MAX = first available GPU; other values select by device enumeration order.
    uint32_t pcie_gpu_index{UINT32_MAX};

    // Semantic thread role — drives Scheduler overrides and GraphRuntime behaviour.
    ThreadRole role{ThreadRole::GENERIC};

    // CCD-level filter applied before scoring.
    // None = use all eligible cores (backward-compatible default).
    CcdPreference ccd_preference{CcdPreference::None};

    uint8_t _pad[3]{};  // explicit padding to keep sizeof predictable
};
static_assert(std::is_trivially_copyable_v<PlacementHint>,
    "PlacementHint must be trivially copyable for span slicing");

// ── ResourceBudget ────────────────────────────────────────────────────────────
// Restricts the set of cores the scheduler may assign to.
struct ResourceBudget {
    // Only cores with logical_id < max_cores are eligible.
    // UINT32_MAX = all online cores.
    uint32_t max_cores{UINT32_MAX};

    // Only cores with numa_node < max_numa_nodes are eligible.
    // UINT32_MAX = all NUMA nodes.
    uint32_t max_numa_nodes{UINT32_MAX};

    bool    allow_ht_siblings{true};       // allow two nodes on HT siblings of same phys core
    bool    allow_efficiency_cores{true};  // allow placement on Intel E-cores / ARM LITTLE
    uint8_t _pad[2]{};
};
static_assert(std::is_trivially_copyable_v<ResourceBudget>);

// ── NodeAssignment ────────────────────────────────────────────────────────────
// Output descriptor for one node. Applied by Block G (Orchestration) when
// launching worker threads.
struct NodeAssignment {
    uint32_t node_id{UINT32_MAX};        // index into the graph's node list
    uint32_t core_id{UINT32_MAX};        // logical core ID (UINT32_MAX = unassigned)
    uint32_t numa_node{UINT32_MAX};      // NUMA node of core_id

    uint8_t  thread_priority{0};         // 0=SCHED_OTHER/NORMAL, 1=high, 2=SCHED_FIFO/TIME_CRITICAL
    bool     is_hard_pinned{true};       // false → advisory; OS allowed to migrate
    bool     external{false};            // true → GraphRuntime must NOT spawn a thread;
                                         //         node is driven via pump_external()
    uint8_t  _pad[1]{};

    uint32_t irq_routing{UINT32_MAX};    // preferred NUMA node for MSI-X routing; UINT32_MAX = no change
};
static_assert(std::is_trivially_copyable_v<NodeAssignment>);

// ── PlacementPlan ─────────────────────────────────────────────────────────────
// Result of Scheduler::compute_placement().
// assignments[i].node_id == i for all valid entries (direct-indexed by node_id).
// is_complete == true iff every requested node received a valid core_id.
struct PlacementPlan {
    std::vector<NodeAssignment> assignments;
    bool is_complete{false};

    // O(n) lookup by node_id. Plan size is bounded by node_count (tens, not millions).
    [[nodiscard]] NodeAssignment const* find(uint32_t node_id) const noexcept {
        for (auto const& a : assignments) {
            if (a.node_id == node_id) return &a;
        }
        return nullptr;
    }
};

} // namespace phyriad::scheduler
// Made with my soul - Swately <3
