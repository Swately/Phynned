// apps/ayama/policy/include/ayama/policy/AutoPolicySelector.hpp
// AutoPolicySelector — hardware × process-kind → action decision table.
//
// Implements §9.2 of AYAMA_MASTER_PLAN: the Auto Mode pipeline that chooses
// which CPU affinity action to apply based on:
//   (1) CPU class detected from hardware topology (one-shot at startup), and
//   (2) Process kind (Game / Stream / Comm / Productivity / Browser / Unknown).
//
// Decision table (§9.2):
//
//   CPU class        | Game  | Stream    | Comm     | Productivity
//   -----------------+-------+-----------+----------+-------------
//   X3D-single       | CCD0  | (no act.) | (no act.)| (no act.)
//   X3D-dual         | CCD0  | CCD1      | CCD1     | CCD1
//   Hybrid-Intel     | P     | E         | E        | E
//   Multi-CCX no-X3D | CCX0  | CCX1      | CCX1     | (no act.)
//   Single-CCD       | — no action (< 2% benefit) —
//
// Per-game memory shortcut:
//   If a fresh LearnedEntry exists for (exe, hw_id), the cached best_core_mask
//   is used directly (from_memory = true) instead of the table.
//   Stale entries (age > max_age_days) fall through to table logic.
//   user_locked entries are always honoured.
//
// Threading: single-thread (agent main thread).
// Resource:  zero heap after init; all masks are uint64_t computed once.
// Privilege: None (topology is already probed by startup code).
//
// §9.2, ayama::policy
#pragma once

#include <ayama/policy/PolicyDecision.hpp>
#include <ayama/observer/TargetProcess.hpp>
#include <ayama/learn/PerGameMemory.hpp>
#include <phyriad/topology/HardwareTopology.hpp>
#include <phyriad/stigmergy/Classifier.hpp>   // §P-0.5.3 — stigmergic interface

#include <cstdint>
#include <cstring>

namespace ayama::policy {

// ── CpuClass ─────────────────────────────────────────────────────────────────

/// CPU topology class, detected once at startup.
enum class CpuClass : uint8_t {
    Unknown         = 0, ///< Detection failed or selector not initialised.
    X3DSingle       = 1, ///< AMD X3D, single CCD (7800X3D, 5800X3D, …)
    X3DDual         = 2, ///< AMD X3D, dual CCD   (7950X3D, 7900X3D, …)
    HybridIntel     = 3, ///< Intel Alder Lake+ with P + E cores.
    MultiCCXNoX3D   = 4, ///< AMD multi-CCD/CCX, no V-Cache (5950X, TRx40, …)
    SingleCCD       = 5, ///< Homogeneous single CCD — no action warranted.
};

// ── AutoDecision ─────────────────────────────────────────────────────────────

/// Result of AutoPolicySelector::select().
struct AutoDecision {
    uint64_t   core_mask    {0ull};              ///< 0 → no action (OS default).
    ActionKind action_kind  {ActionKind::None};  ///< None when core_mask == 0.
    uint8_t    confidence   {0u};                ///< 0..100; 0 when no action.
    bool       from_memory  {false};             ///< true if sourced from PerGameMemory.
};

// ── AutoPolicySignal — bundled input for the Classifier<S,A> interface ───────
//
// Bundles the three select() args into a single Signal
// so AutoPolicySelector can satisfy `phyriad::stigmergy::Classifier<Sig,Action>`.
// The classic non-virtual select() remains the recommended hot-path entry
// point; decide() is just the stigmergic-pattern adapter.
struct AutoPolicySignal {
    observer::TargetKind        kind   {observer::TargetKind::Unknown};
    const char*                 exe    {nullptr};
    const learn::PerGameMemory* memory {nullptr};
};

// ── AutoPolicySelector ────────────────────────────────────────────────────────

class AutoPolicySelector final
    : public phyriad::stigmergy::Classifier<AutoPolicySignal, AutoDecision>
{
public:
    AutoPolicySelector() noexcept = default;

    AutoPolicySelector(const AutoPolicySelector&)            = delete;
    AutoPolicySelector& operator=(const AutoPolicySelector&) = delete;

    // ── Initialisation ────────────────────────────────────────────────────

    /// One-shot call. Classifies the CPU, then pre-computes core masks.
    /// Must be called before any select() call.
    void init_from_topology(const phyriad::HardwareTopology& topo) noexcept
    {
        cpu_class_ = classify(topo);
        build_masks(topo, cpu_class_);
    }

    [[nodiscard]] CpuClass   cpu_class() const noexcept { return cpu_class_; }
    [[nodiscard]] bool       is_ready()  const noexcept { return cpu_class_ != CpuClass::Unknown; }

    /// Human-readable label for logging / UI.
    [[nodiscard]] static const char* class_name(CpuClass c) noexcept
    {
        switch (c) {
            case CpuClass::X3DSingle:     return "AMD X3D (single CCD)";
            case CpuClass::X3DDual:       return "AMD X3D (dual CCD)";
            case CpuClass::HybridIntel:   return "Intel hybrid (P+E)";
            case CpuClass::MultiCCXNoX3D: return "AMD multi-CCX (no X3D)";
            case CpuClass::SingleCCD:     return "Single-CCD (no action)";
            default:                      return "Unknown";
        }
    }

    [[nodiscard]] const char* class_name() const noexcept {
        return class_name(cpu_class_);
    }

    // ── Decision making ───────────────────────────────────────────────────

    /// Select the action for a given (kind, exe) pair.
    ///
    /// \param kind     Process classification from ProcessObserver.
    /// \param exe      Executable short name (e.g. "Cyberpunk2077.exe").
    /// \param memory   Optional PerGameMemory for cached-entry shortcut.
    ///                 Pass nullptr to always use the table.
    ///
    /// Returns an AutoDecision with core_mask == 0 when no action is warranted.
    [[nodiscard]] AutoDecision select(
        observer::TargetKind        kind,
        const char*                 exe,
        const learn::PerGameMemory* memory = nullptr) const noexcept
    {
        // ── 1. Per-game memory shortcut (§9.3) ─────────────────────────────
        if (memory && exe && exe[0] != '\0') {
            const learn::LearnedEntry* e = memory->find(exe);
            if (e && e->sample_count > 0u && e->best_core_mask != 0ull) {
                // Honour user-locked or fresh validated entries.
                if (e->user_locked ||
                    !learn::PerGameMemory::needs_revalidation(*e, 30u))
                {
                    AutoDecision d;
                    d.core_mask   = e->best_core_mask;
                    d.action_kind = ActionKind::PinAffinity;
                    d.confidence  = 95u;
                    d.from_memory = true;
                    return d;
                }
            }
        }

        // ── 2. Decision table (§9.2) ───────────────────────────────────────
        return table_select(kind);
    }

    /// §P-0.5.3 — stigmergy::Classifier<AutoPolicySignal, AutoDecision> interface.
    /// Forwards to the non-virtual select() so the virtual path has zero
    /// divergence from the hot path. `final` + LTO devirtualizes when the
    /// concrete type is known at the call site.
    [[nodiscard]] AutoDecision decide(AutoPolicySignal const& s) noexcept override {
        return select(s.kind, s.exe, s.memory);
    }

    // ── Mask accessors (for debug / Advanced panel) ───────────────────────

    [[nodiscard]] uint64_t game_mask()       const noexcept { return mask_game_; }
    [[nodiscard]] uint64_t background_mask() const noexcept { return mask_bg_; }

private:
    CpuClass cpu_class_ {CpuClass::Unknown};
    uint64_t mask_game_ {0ull};  ///< Core mask for game threads.
    uint64_t mask_bg_   {0ull};  ///< Core mask for background (stream/comm/prod).

    // ── classify() ────────────────────────────────────────────────────────

    static CpuClass classify(const phyriad::HardwareTopology& topo) noexcept
    {
        const auto vcache = phyriad::hw::v_cache_cores();
        const auto ecores = phyriad::hw::e_cores();

        // Count distinct CCD IDs from the core list.
        uint32_t max_ccd = 0u;
        for (const auto& c : topo.cores)
            if (c.ccd_id > max_ccd) max_ccd = c.ccd_id;
        const uint32_t n_ccd = topo.cores.empty() ? 0u : max_ccd + 1u;

        // Intel hybrid takes precedence over AMD checks.
        if (!ecores.empty()) {
            return CpuClass::HybridIntel;
        }

        if (!vcache.empty()) {
            return (n_ccd >= 2u) ? CpuClass::X3DDual : CpuClass::X3DSingle;
        }

        if (n_ccd >= 2u) {
            return CpuClass::MultiCCXNoX3D;
        }

        return CpuClass::SingleCCD;
    }

    // ── build_masks() ─────────────────────────────────────────────────────

    void build_masks(const phyriad::HardwareTopology& topo, CpuClass c) noexcept
    {
        mask_game_ = 0ull;
        mask_bg_   = 0ull;

        // Helper: build a mask from a vector of logical core IDs.
        auto make_mask = [](const auto& ids) -> uint64_t {
            uint64_t m = 0ull;
            for (uint32_t id : ids)
                if (id < 64u) m |= (1ull << id);
            return m;
        };

        switch (c) {
        case CpuClass::X3DSingle:
            // Game → V-Cache CCD. Background → no action (single CCD).
            mask_game_ = make_mask(phyriad::hw::v_cache_cores());
            mask_bg_   = 0ull;
            break;

        case CpuClass::X3DDual: {
            // Game → V-Cache CCD (CCD0).
            // Background → non-V-Cache cores (CCD1).
            const auto vcache = phyriad::hw::v_cache_cores();
            mask_game_ = make_mask(vcache);

            // Build full-CPU mask then subtract the V-Cache cores.
            const uint32_t lc = static_cast<uint32_t>(topo.logical_core_count());
            const uint64_t all_mask = (lc < 64u)
                ? ((1ull << lc) - 1ull)
                : ~0ull;
            mask_bg_ = all_mask & ~mask_game_;
            break;
        }

        case CpuClass::HybridIntel:
            // Game → P-cores. Background → E-cores.
            mask_game_ = make_mask(phyriad::hw::p_cores());
            mask_bg_   = make_mask(phyriad::hw::e_cores());
            break;

        case CpuClass::MultiCCXNoX3D: {
            // Game → CCD/CCX 0 cores. Background → CCD/CCX 1+ cores.
            mask_game_ = make_mask(phyriad::hw::ccd_cores(0u));

            // Build CCD1+ mask from the topology cores array.
            uint64_t bg = 0ull;
            uint32_t max_ccd = 0u;
            for (const auto& core : topo.cores)
                if (core.ccd_id > max_ccd) max_ccd = core.ccd_id;

            for (uint32_t cid = 1u; cid <= max_ccd; ++cid) {
                const auto cores_in_ccd = phyriad::hw::ccd_cores(cid);
                for (uint32_t id : cores_in_ccd)
                    if (id < 64u) bg |= (1ull << id);
            }
            mask_bg_ = bg;
            break;
        }

        case CpuClass::SingleCCD:
        case CpuClass::Unknown:
        default:
            // No affinity change warranted.
            mask_game_ = 0ull;
            mask_bg_   = 0ull;
            break;
        }

        (void)topo; // silence unused-parameter warning in some branches
    }

    // ── table_select() ────────────────────────────────────────────────────

    [[nodiscard]] AutoDecision table_select(observer::TargetKind kind) const noexcept
    {
        AutoDecision d;

        const bool is_game = (kind == observer::TargetKind::Game);
        const bool is_bg   = (kind == observer::TargetKind::Stream   ||
                              kind == observer::TargetKind::Comm      ||
                              kind == observer::TargetKind::Productivity);

        if (!is_game && !is_bg) {
            // Browser / System / Unknown → no action.
            return d;
        }

        const uint64_t chosen_mask = is_game ? mask_game_ : mask_bg_;
        if (chosen_mask == 0ull) {
            return d;  // Table says "no action" for this (class, kind) combo.
        }

        d.core_mask   = chosen_mask;
        d.action_kind = ActionKind::PinAffinity;
        d.confidence  = is_game ? 85u : 75u;
        d.from_memory = false;
        return d;
    }
};

} // namespace ayama::policy
// Made with my soul - Swately <3
