// framework/scheduler/src/Scheduler.cpp
// Greedy V-Cache/NUMA/PCIe-aware placement algorithm — full implementation.
//
// Internal types (not exposed in header):
//
//   PlacementScore    — accumulator for the per-(node, core) score:
//                         add_vcache_match()      +100
//                         add_numa_match()        + 50
//                         add_pcie_proximity()    + 30
//                         add_co_location()       + 60
//                         add_anti_affinity()     - 80  (applied per matching entry)
//                         add_isolation_bonus()   + 20
//                         add_isolation_penalty() - 40
//
//   PlacementContext  — immutable snapshot of the partial plan and per-core
//                       usage counts threaded through each iteration of the
//                       greedy loop; read-only from score_core().
//
//   Scheduler::Impl   — owns PlacementPolicy, implements compute().
//
// Algorithm: greedy first-fit descending. O(N × C) for SIMPLE hints (score_core
// O(1)); ~O(N²·C + N·C²) for DENSE co_locate/anti_affinity hints (score_core
// scans the partial placement + cores per pair) — measured, bench_scheduler_placement §B.
//   For each node 0..node_count-1:
//     1. Fetch its PlacementHint (default if out of hints span).
//     2. Score every eligible core against the hint + current context.
//     3. Assign the highest-scoring core; break ties in favour of lower logical_id
//        (determinism: same inputs → same output regardless of vector order).
//     4. Increment core_usage[assigned_core].
//   End result: PlacementPlan with one NodeAssignment per node.
//

#include <phyriad/scheduler/Scheduler.hpp>
#include <algorithm>
#include <cstdint>
#include <limits>
#include <vector>

namespace phyriad::scheduler {

// ── PlacementScore ────────────────────────────────────────────────────────────
struct PlacementScore {
    int32_t value{};

    constexpr void add_vcache_match()        noexcept { value += 100; }
    constexpr void add_numa_match()          noexcept { value +=  50; }
    constexpr void add_pcie_proximity()      noexcept { value +=  30; }
    constexpr void add_co_location()         noexcept { value +=  60; }
    constexpr void add_anti_affinity()       noexcept { value -=  80; }
    constexpr void add_isolation_bonus()     noexcept { value +=  20; }
    constexpr void add_isolation_penalty()   noexcept { value -=  40; }
};

// ── PlacementContext ──────────────────────────────────────────────────────────
// Snapshot of already-placed nodes and per-core occupancy; read-only in score_core().
struct PlacementContext {
    std::vector<NodeAssignment> const& partial;     // nodes placed so far (0..ni-1)
    std::vector<uint32_t>       const& core_usage;  // core_usage[logical_id] = #nodes
};

// ── Scheduler::Impl ───────────────────────────────────────────────────────────
struct Scheduler::Impl {
    PlacementPolicy policy;

    explicit Impl(PlacementPolicy const& p) noexcept : policy{p} {}

    [[nodiscard]] PlacementPlan compute(
        phyriad::HardwareTopology const& topology,
        uint32_t                       node_count,
        std::span<PlacementHint const> hints,
        ResourceBudget const&          budget) noexcept;

private:
    // Returns true if `core` may be assigned under the given budget.
    // core_usage is needed to enforce allow_ht_siblings == false.
    [[nodiscard]] static bool is_eligible(
        phyriad::CoreInfo         const& core,
        ResourceBudget         const& budget,
        std::vector<uint32_t>  const& core_usage) noexcept;

    // Score a single (hint × core) pair against the current placement context.
    // `all_cores` is topology.cores (needed for CCX lookups of already-placed nodes).
    [[nodiscard]] static int32_t score_core(
        PlacementHint               const& hint,
        phyriad::CoreInfo               const& core,
        PlacementContext             const& ctx,
        std::vector<phyriad::CoreInfo>   const& all_cores,
        phyriad::HardwareTopology        const& topology) noexcept;

    // Returns the CCD id to use as candidate filter for `pref`,
    // or UINT32_MAX if the preference degrades to "all cores" (no CCD filter).
    // `vcache_ccd` = ccd_id of any V-Cache core (UINT32_MAX if none).
    // `freq_ccd`   = ccd_id of the CCD with the highest average max_freq_mhz.
    [[nodiscard]] static uint32_t resolve_ccd_filter(
        CcdPreference                    pref,
        std::vector<phyriad::CoreInfo> const& cores,
        uint32_t                          vcache_ccd,
        uint32_t                          freq_ccd) noexcept;
};

// ── is_eligible ───────────────────────────────────────────────────────────────
bool Scheduler::Impl::is_eligible(
    phyriad::CoreInfo         const& core,
    ResourceBudget         const& budget,
    std::vector<uint32_t>  const& core_usage) noexcept
{
    // E-core exclusion
    if (!budget.allow_efficiency_cores && core.is_efficiency_core)
        return false;

    // Logical-id cap (max_cores == UINT32_MAX means "all cores")
    if (budget.max_cores != UINT32_MAX && core.logical_id >= budget.max_cores)
        return false;

    // NUMA cap (max_numa_nodes == UINT32_MAX means "all nodes")
    if (budget.max_numa_nodes != UINT32_MAX && core.numa_node != UINT32_MAX
            && core.numa_node >= budget.max_numa_nodes)
        return false;

    // HT sibling exclusion: if allow_ht_siblings == false and the physical
    // sibling of this logical core is already occupied, skip this core.
    if (!budget.allow_ht_siblings && core.smt_sibling != UINT32_MAX
            && core.smt_sibling < static_cast<uint32_t>(core_usage.size())
            && core_usage[core.smt_sibling] > 0u)
        return false;

    return true;
}

// ── score_core ────────────────────────────────────────────────────────────────
int32_t Scheduler::Impl::score_core(
    PlacementHint               const& hint,
    phyriad::CoreInfo               const& core,
    PlacementContext             const& ctx,
    std::vector<phyriad::CoreInfo>   const& all_cores,
    phyriad::HardwareTopology        const& topology) noexcept
{
    PlacementScore s{};

    // ── V-Cache affinity ──────────────────────────────────────────────────────
    if (hint.prefer_vcache && core.has_v_cache)
        s.add_vcache_match();

    // ── NUMA affinity ─────────────────────────────────────────────────────────
    if (hint.prefer_numa != UINT32_MAX && hint.prefer_numa == core.numa_node)
        s.add_numa_match();

    // ── PCIe GPU proximity ────────────────────────────────────────────────────
    // A "GPU-class" PCIe device has PCI class code 0x0300 (Display Controller).
    // We want a device on the same NUMA node as this candidate core.
    if (hint.prefers_pcie_gpu && core.numa_node != UINT32_MAX) {
        for (auto const& dev : topology.pcie_affinity_map) {
            if (dev.pci_class == 0x0300u && dev.numa_node == core.numa_node) {
                s.add_pcie_proximity();
                break;  // one match is enough — don't double-count
            }
        }
    }

    // ── Co-location: same CCX/CCD as a previously placed node ─────────────────
    if (hint.co_locate_with != UINT32_MAX) {
        for (auto const& assigned : ctx.partial) {
            if (assigned.node_id != hint.co_locate_with) continue;
            if (assigned.core_id == UINT32_MAX) break;

            // Find the CoreInfo for the co-locate target's assigned core.
            for (auto const& co_core : all_cores) {
                if (co_core.logical_id != assigned.core_id) continue;
                if (co_core.ccx_id == core.ccx_id && co_core.ccd_id == core.ccd_id)
                    s.add_co_location();
                break;
            }
            break;
        }
    }

    // ── Anti-affinity: penalise sharing a CCX with conflicting nodes ──────────
    for (uint32_t anti_id : hint.anti_affinity_with) {
        if (anti_id == UINT32_MAX) continue;
        for (auto const& assigned : ctx.partial) {
            if (assigned.node_id != anti_id) continue;
            if (assigned.core_id == UINT32_MAX) break;

            for (auto const& anti_core : all_cores) {
                if (anti_core.logical_id != assigned.core_id) continue;
                // Same CCX → penalise regardless of CCD (CCX is the L3-sharing domain)
                if (anti_core.ccx_id == core.ccx_id)
                    s.add_anti_affinity();
                break;
            }
            break;
        }
    }

    // ── Isolation bonus / penalty ─────────────────────────────────────────────
    if (hint.prefer_isolated_core) {
        uint32_t usage = (core.logical_id < ctx.core_usage.size())
                         ? ctx.core_usage[core.logical_id] : 0u;
        if (usage == 0) s.add_isolation_bonus();
        else            s.add_isolation_penalty();
    }

    return s.value;
}

// ── resolve_ccd_filter ────────────────────────────────────────────────────────
uint32_t Scheduler::Impl::resolve_ccd_filter(
    CcdPreference                    pref,
    std::vector<phyriad::CoreInfo> const& cores,
    uint32_t                          vcache_ccd,
    uint32_t                          freq_ccd) noexcept
{
    switch (pref) {
        case CcdPreference::PreferVCache:
            // Degrade to None if no V-Cache CCD detected.
            return vcache_ccd;
        case CcdPreference::PreferFreq:
            // Degrade to None if only one CCD (freq_ccd == UINT32_MAX).
            return freq_ccd;
        default:
            return UINT32_MAX;  // no filter
    }
}

// ── compute ───────────────────────────────────────────────────────────────────
PlacementPlan Scheduler::Impl::compute(
    phyriad::HardwareTopology const& topology,
    uint32_t                       node_count,
    std::span<PlacementHint const> hints,
    ResourceBudget const&          budget) noexcept
{
    PlacementPlan plan{};

    if (node_count == 0) {
        plan.is_complete = true;
        return plan;
    }

    // Degenerate: no cores probed — return unassigned plan.
    if (topology.cores.empty()) {
        plan.assignments.reserve(node_count);
        for (uint32_t i = 0; i < node_count; ++i)
            plan.assignments.push_back(NodeAssignment{.node_id = i});
        plan.is_complete = false;
        return plan;
    }

    plan.assignments.reserve(node_count);

    // Determine the core_usage vector size from the maximum logical_id present.
    uint32_t max_lid = 0;
    for (auto const& c : topology.cores)
        if (c.logical_id != UINT32_MAX && c.logical_id > max_lid)
            max_lid = c.logical_id;

    std::vector<uint32_t> core_usage(static_cast<std::size_t>(max_lid) + 1u, 0u);

    // ── Pre-compute CCD filter metadata ──────────────────────────────────────
    // vcache_ccd: ccd_id of the first core with has_v_cache == true (UINT32_MAX if none).
    uint32_t vcache_ccd = UINT32_MAX;
    for (auto const& c : topology.cores) {
        if (c.has_v_cache) { vcache_ccd = c.ccd_id; break; }
    }

    // freq_ccd: ccd_id of the CCD with the highest average max_freq_mhz.
    // UINT32_MAX (no filter) when all cores are on a single CCD.
    uint32_t freq_ccd = UINT32_MAX;
    {
        struct CcdFreqAcc { uint32_t ccd_id; uint64_t freq_sum; uint32_t count; };
        std::vector<CcdFreqAcc> accs;
        accs.reserve(8);
        for (auto const& c : topology.cores) {
            if (c.max_freq_mhz == 0) continue;
            bool found = false;
            for (auto& a : accs) {
                if (a.ccd_id == c.ccd_id) { a.freq_sum += c.max_freq_mhz; ++a.count; found = true; break; }
            }
            if (!found) accs.push_back({c.ccd_id, c.max_freq_mhz, 1u});
        }
        if (accs.size() >= 2u) {
            uint32_t best_avg = 0;
            for (auto const& a : accs) {
                const uint32_t avg = static_cast<uint32_t>(a.freq_sum / a.count);
                if (avg > best_avg) { best_avg = avg; freq_ccd = a.ccd_id; }
            }
        }
    }

    // ── Greedy loop ───────────────────────────────────────────────────────────
    for (uint32_t ni = 0; ni < node_count; ++ni) {
        PlacementHint const& hint =
            (ni < static_cast<uint32_t>(hints.size())) ? hints[ni] : PlacementHint{};

        // ── Role-based overrides ──────────────────────────────────────────────
        // Applied on local copies — the caller's hint and budget are never mutated.

        PlacementHint  eff_hint   = hint;
        ResourceBudget eff_budget = budget;

        // UI_MAIN: external node — GraphRuntime drives it via pump_external().
        // No core assignment; do NOT consume a core_usage slot.
        if (eff_hint.role == ThreadRole::UI_MAIN) {
            plan.assignments.push_back(NodeAssignment{
                .node_id  = ni,
                .core_id  = UINT32_MAX,
                .external = true,
            });
            continue;
        }

        // RENDER: GPU-adjacent core preferred (reuses prefers_pcie_gpu scoring, +30).
        if (eff_hint.role == ThreadRole::RENDER)
            eff_hint.prefers_pcie_gpu = true;

        // COMPUTE: V-Cache core, isolated from other nodes.
        if (eff_hint.role == ThreadRole::COMPUTE) {
            eff_hint.prefer_vcache        = true;
            eff_hint.prefer_isolated_core = true;
        }

        // IO: allow E-cores regardless of ResourceBudget setting.
        if (eff_hint.role == ThreadRole::IO)
            eff_budget.allow_efficiency_cores = true;

        PlacementContext ctx{
            .partial    = plan.assignments,
            .core_usage = core_usage,
        };

        // Resolve CCD filter for this node's hint.
        // ccd_filter == UINT32_MAX means "no filter" (all CCDs eligible).
        const uint32_t ccd_filter = resolve_ccd_filter(
            eff_hint.ccd_preference, topology.cores, vcache_ccd, freq_ccd);

        uint32_t best_lid   = UINT32_MAX;
        int32_t  best_score = std::numeric_limits<int32_t>::min();

        // First pass: candidates restricted to the preferred CCD (if any).
        for (auto const& core : topology.cores) {
            if (ccd_filter != UINT32_MAX && core.ccd_id != ccd_filter) continue;
            if (!is_eligible(core, eff_budget, core_usage)) continue;

            int32_t sc = score_core(eff_hint, core, ctx, topology.cores, topology);

            if (best_lid == UINT32_MAX || sc > best_score
                    || (sc == best_score && core.logical_id < best_lid)) {
                best_score = sc;
                best_lid   = core.logical_id;
            }
        }

        // Second pass (fallback): if the preferred CCD had no eligible candidates,
        // re-run over all cores so the plan remains complete.
        if (best_lid == UINT32_MAX && ccd_filter != UINT32_MAX) {
            for (auto const& core : topology.cores) {
                if (!is_eligible(core, eff_budget, core_usage)) continue;

                int32_t sc = score_core(eff_hint, core, ctx, topology.cores, topology);

                if (best_lid == UINT32_MAX || sc > best_score
                        || (sc == best_score && core.logical_id < best_lid)) {
                    best_score = sc;
                    best_lid   = core.logical_id;
                }
            }
        }

        // Last-resort fallback: if still no eligible core, use the first core.
        if (best_lid == UINT32_MAX)
            best_lid = topology.cores.front().logical_id;

        // Resolve NUMA node for the chosen core.
        uint32_t best_numa = UINT32_MAX;
        for (auto const& c : topology.cores) {
            if (c.logical_id == best_lid) {
                best_numa = c.numa_node;
                break;
            }
        }

        // Thread priority: realtime if explicitly requested, V-Cache (latency-critical),
        // or COMPUTE role (forced prefer_vcache on eff_hint already covers this).
        const uint8_t prio =
            (eff_hint.prefer_realtime_priority || eff_hint.prefer_vcache)
            ? uint8_t{2} : uint8_t{0};

        // Hard-pinned for Pinned and StickyThenSteal; advisory for the rest.
        const bool hard = (policy.kind == PlacementPolicyKind::Pinned ||
                           policy.kind == PlacementPolicyKind::StickyThenSteal);

        plan.assignments.push_back(NodeAssignment{
            .node_id         = ni,
            .core_id         = best_lid,
            .numa_node       = best_numa,
            .thread_priority = prio,
            .is_hard_pinned  = hard,
            .irq_routing     = best_numa,   // route IRQs to the same NUMA node
        });

        // Record occupancy for subsequent iterations.
        if (best_lid < core_usage.size())
            ++core_usage[best_lid];
    }

    plan.is_complete = true;
    return plan;
}

// ── Scheduler public interface ────────────────────────────────────────────────
Scheduler::Scheduler(PlacementPolicy const& policy) noexcept
    : impl_{std::make_unique<Impl>(policy)}
{}

// Out-of-line destructor: Impl is fully defined above, so unique_ptr<Impl>::~unique_ptr
// can emit the Impl destructor call here rather than at every call site.
Scheduler::~Scheduler() = default;

Scheduler::Scheduler(Scheduler&&) noexcept            = default;
Scheduler& Scheduler::operator=(Scheduler&&) noexcept = default;

PlacementPlan Scheduler::compute_placement(
    phyriad::HardwareTopology const& topology,
    uint32_t                       node_count,
    std::span<PlacementHint const> hints,
    ResourceBudget const&          budget) noexcept
{
    return impl_->compute(topology, node_count, hints, budget);
}

} // namespace phyriad::scheduler
// Made with my soul - Swately <3
