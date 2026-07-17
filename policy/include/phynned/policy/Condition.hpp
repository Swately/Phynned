// policy/include/phynned/policy/Condition.hpp
// Condition — predicate evaluated against a (TargetProcess, TargetMetrics) pair.
//
// Conditions are POD-compatible for storage in std::array.
// Each condition checks one metric against one threshold.
//
#pragma once

#include <phynned/observer/TargetProcess.hpp>
#include <phynned/observer/TargetMetrics.hpp>
#include <cstdint>

namespace phynned::policy {

/// Which metric this condition checks.
enum class ConditionField : uint8_t {
    TargetKind          = 0,  ///< TargetProcess::kind == value
    MigrationsPerSec    = 1,  ///< TargetMetrics::migrations_per_sec >= threshold
    CpuUsagePct         = 2,  ///< TargetMetrics::cpu_usage_pct >= threshold
    PressureLevel       = 3,  ///< TargetMetrics::pressure_level >= threshold
    FrameTimeP99        = 4,  ///< TargetMetrics::frame_time_p99_ms >= threshold
    TopologyHasVCache   = 5,  ///< hw::v_cache_cores() is non-empty (global)
    TopologyHasECores   = 6,  ///< hw::e_cores() is non-empty (global)
    TopologyMultiCcd    = 7,  ///< CCD count >= 2 (global)
    AnyGameRunning      = 8,  ///< At least one target of kind==Game exists
};

/// Comparison operator for threshold-based conditions.
enum class ConditionOp : uint8_t {
    Equal        = 0,
    NotEqual     = 1,
    GreaterEqual = 2,
    LessEqual    = 3,
    GreaterThan  = 4,
    LessThan     = 5,
};

/// A single condition (16 bytes).
struct alignas(8) Condition {
    ConditionField field;    //  1B
    ConditionOp    op;       //  1B
    uint8_t        _pad[2];  //  2B
    uint32_t       value_u;  //  4B  — integer threshold (cast as needed)
    float          value_f;  //  4B  — float threshold (for CpuUsagePct etc.)
    uint8_t        _pad2[4]; //  4B
};
static_assert(sizeof(Condition) == 16);

/// Evaluate a single condition against target+metrics.
/// `global_flags` encodes topology: bit0=has_vcache, bit1=has_ecores, bit2=multi_ccd.
[[nodiscard]] inline bool evaluate_condition(
    const Condition& c,
    const observer::TargetProcess& target,
    const observer::TargetMetrics& metrics,
    uint8_t global_flags,
    uint8_t any_game_running) noexcept
{
    float lhs = 0.f;
    switch (c.field) {
        case ConditionField::TargetKind:
            lhs = static_cast<float>(static_cast<uint8_t>(target.kind)); break;
        case ConditionField::MigrationsPerSec:
            lhs = static_cast<float>(metrics.migrations_per_sec); break;
        case ConditionField::CpuUsagePct:
            lhs = metrics.cpu_usage_pct; break;
        case ConditionField::PressureLevel:
            lhs = static_cast<float>(metrics.pressure_level); break;
        case ConditionField::FrameTimeP99:
            lhs = metrics.frame_time_p99_ms; break;
        case ConditionField::TopologyHasVCache:
            lhs = (global_flags & 0x1u) ? 1.f : 0.f; break;
        case ConditionField::TopologyHasECores:
            lhs = (global_flags & 0x2u) ? 1.f : 0.f; break;
        case ConditionField::TopologyMultiCcd:
            lhs = (global_flags & 0x4u) ? 1.f : 0.f; break;
        case ConditionField::AnyGameRunning:
            lhs = static_cast<float>(any_game_running); break;
    }

    const float rhs = (c.value_f != 0.f) ? c.value_f
                                          : static_cast<float>(c.value_u);
    switch (c.op) {
        case ConditionOp::Equal:        return lhs == rhs;
        case ConditionOp::NotEqual:     return lhs != rhs;
        case ConditionOp::GreaterEqual: return lhs >= rhs;
        case ConditionOp::LessEqual:    return lhs <= rhs;
        case ConditionOp::GreaterThan:  return lhs >  rhs;
        case ConditionOp::LessThan:     return lhs <  rhs;
    }
    return false;
}

} // namespace phynned::policy
// Made with my soul - Swately <3
