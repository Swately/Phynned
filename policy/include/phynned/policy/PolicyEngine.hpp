// policy/include/phynned/policy/PolicyEngine.hpp
// PolicyEngine — evaluate rules against target state and emit decisions.
//
// Rules are stored in a fixed-size std::array. Evaluation is O(n_rules × n_targets).
// For n_rules=32 and n_targets=32 with kMaxConditionsPerRule=4, this is
// 32×32×4 = ~4096 comparisons per tick — negligible.
//
// Threading: single-thread (agent main thread). evaluate() is NOT re-entrant.
// Resource:  zero heap allocation; all storage in-class.
// Privilege: None (topology flags are pre-computed at startup).
//
#pragma once

#include <phynned/policy/Rule.hpp>
#include <phynned/policy/Condition.hpp>
#include <phynned/policy/PolicyDecision.hpp>
#include <phynned/observer/TargetProcess.hpp>
#include <phynned/observer/TargetMetrics.hpp>

#include <array>
#include <cstdint>

// Forward declare topology type
namespace phyriad { struct HardwareTopology; }

namespace phynned::policy {

class PolicyEngine {
public:
    PolicyEngine() noexcept;

    // ── Rule management ────────────────────────────────────────────────────
    /// Register built-in rules based on the detected hardware topology.
    void register_default_rules(const phyriad::HardwareTopology& topo) noexcept;

    /// Register a custom rule. Returns false if the table is full.
    [[nodiscard]] bool register_rule(const Rule& r) noexcept;

    void enable_rule(uint32_t rule_id) noexcept;
    void disable_rule(uint32_t rule_id) noexcept;

    [[nodiscard]] uint32_t rule_count() const noexcept { return n_rules_; }
    [[nodiscard]] const Rule* rules()   const noexcept { return rules_.data(); }

    // ── Differential pin mode (Rule 7) ─────────────────────────
    /// When enabled, for any Game target with a stable hot_tid (from
    /// MetricsCollector's delta-time identification), evaluate() emits
    /// `kRuleIdPinHotThreadDifferential` (Rule 7) INSTEAD of the standard
    /// `kRuleIdPinGameToVCache` (Rule 1). The downstream ActionExecutor
    /// handles Rule 7 via `apply_differential_pin` — pins the hot thread
    /// to V-Cache CCD while RELEASING the process to use all cores
    /// (workers spread across both CCDs).
    ///
    /// Default OFF for safety (matches existing test baselines). UI can
    /// toggle via the IPC command channel before running A/B/A/B/A.
    void set_differential_pin_enabled(bool enabled) noexcept {
        differential_pin_enabled_ = enabled;
    }
    [[nodiscard]] bool differential_pin_enabled() const noexcept {
        return differential_pin_enabled_;
    }

    // ── Public rule IDs (consumed by AgentRuntime for dispatch) ────────────
    // Exposed because AgentRuntime needs to dispatch Rule 7 to
    // ActionExecutor::apply_differential_pin instead of apply().
    // The others remain private (internal to PolicyEngine evaluation).
    static constexpr uint32_t kRuleIdPinHotThreadDifferential = 7u;

    // CCD Load Defense rule ID exposed so the UI /
    // telemetry layer can identify decisions emitted by this rule for
    // separate aggregation ("vcache defended N processes for X% CPU").
    static constexpr uint32_t kRuleIdCcdLoadDefense           = 8u;

    // ── CCD Load Defense telemetry ───────────────────────────────
    /// Last evaluation's CCD-defense aggregate: number of background
    /// processes evicted from V-Cache CCD during the most recent
    /// evaluate() call, and their summed CPU%. Published to SHM by
    /// AgentRuntime for UI display ("Phynned is defending V-Cache from
    /// 5 processes consuming 12.3% CPU").
    [[nodiscard]] uint32_t last_ccd_defense_count() const noexcept {
        return last_ccd_defense_count_;
    }
    [[nodiscard]] float last_ccd_defense_cpu_pct() const noexcept {
        return last_ccd_defense_cpu_pct_;
    }

    // ── Evaluation ────────────────────────────────────────────────────────
    /// Evaluate all enabled rules against the current targets and metrics.
    /// Writes up to kMaxDecisionsPerCycle decisions to `out_decisions`.
    /// Returns the number of decisions written.
    ///
    /// noexcept: any internal error produces 0 decisions.
    [[nodiscard]] uint32_t evaluate(
        const observer::TargetProcess* targets, uint32_t n_targets,
        const observer::TargetMetrics* metrics, uint32_t n_metrics,
        PolicyDecision* out_decisions) noexcept;

private:
    alignas(64) std::array<Rule, kMaxRules> rules_{};
    uint32_t n_rules_{0u};

    // Pre-computed topology flags (set once in register_default_rules).
    uint8_t global_flags_{0u};  // bit0=has_vcache, bit1=has_ecores, bit2=multi_ccd

    // ── Default rule IDs ───────────────────────────────────────────────────
    static constexpr uint32_t kRuleIdPinGameToVCache        = 1u;
    static constexpr uint32_t kRuleIdPinGameToPCores        = 2u;
    static constexpr uint32_t kRuleIdEvictStreamFromHotCcd  = 3u;
    static constexpr uint32_t kRuleIdIsolateGameFromBg      = 4u;
    static constexpr uint32_t kRuleIdRevertOnExit           = 5u;
    static constexpr uint32_t kRuleIdEvictCommFromHotCcd    = 6u;   // §11 Pedrogas
    // Note: kRuleIdPinHotThreadDifferential = 7u is declared PUBLIC above
    // (consumed by AgentRuntime for dispatch).

    // Differential pin mode toggle. Disabled by default — matches the
    // empirical baseline established by reports 1-9.
    bool differential_pin_enabled_{false};

    // CCD Load Defense telemetry counters. Updated by every evaluate()
    // call. Defaults to zero on construction; only meaningful after first
    // evaluate() returns.
    uint32_t last_ccd_defense_count_  {0u};
    float    last_ccd_defense_cpu_pct_{0.0f};

    Rule* find_rule(uint32_t id) noexcept;
};

} // namespace phynned::policy
// Made with my soul - Swately <3
