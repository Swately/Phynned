// policy/include/phynned/policy/Rule.hpp
// Rule — named policy rule: N conditions → one action decision.
//
// All conditions must be satisfied (AND logic) for the rule to fire.
// Rules are stored in a std::array inside PolicyEngine (no heap).
//
#pragma once

#include <phynned/policy/Condition.hpp>
#include <phynned/policy/PolicyDecision.hpp>
#include <cstdint>
#include <cstring>

namespace phynned::policy {

/// Maximum conditions per rule.
inline constexpr uint32_t kMaxConditionsPerRule = 4u;

/// Maximum rules in the PolicyEngine.
inline constexpr uint32_t kMaxRules = 32u;

/// A policy rule — conditions + action template.
struct Rule {
    // ── Identity ──────────────────────────────────────────────────────────
    uint32_t id;           ///< Unique rule ID (assigned by PolicyEngine).
    char     name[40];     ///< Human-readable rule name.
    bool     enabled;      ///< Whether this rule participates in evaluation.
    uint8_t  _pad[3];

    // ── Conditions (all must pass) ─────────────────────────────────────────
    Condition conditions[kMaxConditionsPerRule];
    uint32_t  n_conditions;

    // ── Action template ────────────────────────────────────────────────────
    ActionKind action_kind;    ///< What to do when conditions match.
    uint8_t    confidence;     ///< 0..100 — overrides default.
    uint8_t    _pad2[2];
    uint64_t   core_mask;      ///< For PinAffinity: target core mask.
    uint32_t   priority_class; ///< For SetPriority: NORMAL_PRIORITY_CLASS etc.
    uint8_t    _pad3[4];

    /// Convenience constructor for a named rule.
    static Rule make(uint32_t id, const char* name,
                     ActionKind kind = ActionKind::None,
                     uint64_t mask = 0ull,
                     uint8_t conf = 80u) noexcept
    {
        Rule r{};
        r.id = id;
        std::strncpy(r.name, name, sizeof(r.name) - 1u);
        r.enabled      = true;
        r.action_kind  = kind;
        r.core_mask    = mask;
        r.confidence   = conf;
        return r;
    }
};

} // namespace phynned::policy
// Made with my soul - Swately <3
