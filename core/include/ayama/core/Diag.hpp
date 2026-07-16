// apps/ayama/core/include/ayama/core/Diag.hpp
// Diag — crash-diagnostic state shared between AgentRuntime and main.
//
// AgentRuntime::run() writes `last_phase` and `last_tick` at each phase
// transition. main.cpp's UnhandledExceptionFilter reads them to surface
// "where did we crash?" without needing a debugger attached.
//
// Phase enumeration mirrors the comments in AgentRuntime::run().
//
// Threading: atomic so the crash handler on any thread can read a consistent
// snapshot. Cost: one relaxed store per phase boundary (~16/tick).
#pragma once

#include <atomic>
#include <cstdint>

namespace ayama::core::diag {

enum Phase : uint32_t {
    PhaseIdle             = 0u,
    PhaseTickStart        = 1u,
    PhaseIpcCommand       = 2u,
    PhasePowerCheck       = 3u,
    PhaseIdleCheck        = 4u,
    PhaseForegroundTick   = 5u,
    PhaseAutoDiscover     = 6u,
    PhaseProcessRefresh   = 7u,
    PhaseMetricsSample    = 8u,
    PhaseClassify         = 9u,
    PhaseWorkloadState    = 10u,
    PhaseEtwTierUpdate    = 11u,
    PhasePolicyEvaluate   = 12u,
    PhaseApplyDecisions   = 13u,
    PhaseAuditDrain       = 14u,
    PhaseShmPublish       = 15u,
    PhaseSelfMonitor      = 16u,
    PhaseWakeWait         = 17u,
};

inline const char* phase_name(uint32_t p) noexcept {
    switch (p) {
        case PhaseIdle:             return "Idle";
        case PhaseTickStart:        return "TickStart";
        case PhaseIpcCommand:       return "IpcCommand";
        case PhasePowerCheck:       return "PowerCheck";
        case PhaseIdleCheck:        return "IdleCheck";
        case PhaseForegroundTick:   return "ForegroundTick";
        case PhaseAutoDiscover:     return "AutoDiscover";
        case PhaseProcessRefresh:   return "ProcessRefresh";
        case PhaseMetricsSample:    return "MetricsSample";
        case PhaseClassify:         return "Classify";
        case PhaseWorkloadState:    return "WorkloadState";
        case PhaseEtwTierUpdate:    return "EtwTierUpdate";
        case PhasePolicyEvaluate:   return "PolicyEvaluate";
        case PhaseApplyDecisions:   return "ApplyDecisions";
        case PhaseAuditDrain:       return "AuditDrain";
        case PhaseShmPublish:       return "ShmPublish";
        case PhaseSelfMonitor:      return "SelfMonitor";
        case PhaseWakeWait:         return "WakeWait";
        default:                    return "?";
    }
}

// Definitions live in AgentRuntime.cpp (one TU owns the storage).
extern std::atomic<uint32_t> g_last_phase;
extern std::atomic<uint32_t> g_last_tick;
// Per-target diagnostic context for ApplyDecisions (which target/rule when crash).
extern std::atomic<uint32_t> g_last_apply_pid;
extern std::atomic<uint32_t> g_last_apply_rule;
extern std::atomic<uint32_t> g_last_apply_tid;

} // namespace ayama::core::diag
// Made with my soul - Swately <3
