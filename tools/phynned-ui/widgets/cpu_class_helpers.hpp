// tools/phynned-ui/widgets/cpu_class_helpers.hpp
// Shared helpers for rendering arch-aware UI text in phynned-ui.
//
// The agent publishes `PhynnedSnapshotMini::cpu_class` (uint8_t) reflecting
// the detected CPU class. The UI surfaces architecture-specific messaging
// from this byte without depending on the policy::CpuClass header
// (keeps the UI ↔ policy boundary clean).
//
// Numeric values mirror policy::CpuClass:
//   0=Unknown 1=X3DSingle 2=X3DDual 3=HybridIntel 4=MultiCCXNoX3D 5=SingleCCD
//
#pragma once
#include "../PhynnedAppState.hpp"
#include <phynned/ipc/PhynnedClient.hpp>
#include <phynned/observer/TargetProcess.hpp>
#include <imgui.h>
#include <cstdint>
#include <cstdio>

namespace phynned_ui {

// ── CpuClass numeric constants (mirror policy::CpuClass) ─────────────────
inline constexpr uint8_t kCpuUnknown       = 0u;
inline constexpr uint8_t kCpuX3DSingle     = 1u;
inline constexpr uint8_t kCpuX3DDual       = 2u;
inline constexpr uint8_t kCpuHybridIntel   = 3u;
inline constexpr uint8_t kCpuMultiCCXNoX3D = 4u;
inline constexpr uint8_t kCpuSingleCCD     = 5u;

// ── arch_short_label() - terse name for badges + status bar ──────────────
[[nodiscard]] inline const char*
arch_short_label(uint8_t cpu_class) noexcept {
    switch (cpu_class) {
        case kCpuX3DSingle:     return "AMD X3D / 1 CCD";
        case kCpuX3DDual:       return "AMD X3D / 2 CCDs";
        case kCpuHybridIntel:   return "Intel Hybrid (P+E)";
        case kCpuMultiCCXNoX3D: return "AMD multi-CCD";
        case kCpuSingleCCD:     return "Single CCD";
        case kCpuUnknown:
        default:                return "detecting...";
    }
}

// ── arch_long_label() - full marketing-friendly name ─────────────────────
[[nodiscard]] inline const char*
arch_long_label(uint8_t cpu_class) noexcept {
    switch (cpu_class) {
        case kCpuX3DSingle:
            return "AMD Ryzen with 3D V-Cache (single CCD, e.g. 7800X3D)";
        case kCpuX3DDual:
            return "AMD Ryzen with 3D V-Cache (dual CCD, e.g. 7950X3D / 9950X3D)";
        case kCpuHybridIntel:
            return "Intel hybrid architecture (P-cores + E-cores)";
        case kCpuMultiCCXNoX3D:
            return "AMD Ryzen multi-CCD without V-Cache (e.g. 5950X / 9950X)";
        case kCpuSingleCCD:
            return "Symmetric single-CCD CPU";
        case kCpuUnknown:
        default:
            return "CPU topology not yet detected";
    }
}

// ── what_phynned_does() - narrative one-liner per arch ─────────────────────
// Plain-language description of the optimization strategy for this CPU.
[[nodiscard]] inline const char*
what_phynned_does(uint8_t cpu_class) noexcept {
    switch (cpu_class) {
        case kCpuX3DSingle:
            return "Pins games to the V-Cache cores; evicts background "
                   "tasks that pollute the L3 cache.";
        case kCpuX3DDual:
            return "Pins games to the V-Cache CCD (96 MB L3) and moves "
                   "background tasks to the non-V-Cache CCD.";
        case kCpuHybridIntel:
            return "Pins games to the high-performance P-cores; moves "
                   "background tasks (Discord, Teams, etc.) to E-cores.";
        case kCpuMultiCCXNoX3D:
            return "Isolates games on one CCD; keeps background tasks on "
                   "the other CCD to eliminate cross-CCD migrations.";
        case kCpuSingleCCD:
            return "Your CPU is symmetric - core pinning has negligible "
                   "benefit. Phynned monitors but does not change affinity.";
        case kCpuUnknown:
        default:
            return "Probing CPU topology...";
    }
}

// ── is_asymmetric() - does this arch benefit from Phynned? ─────────────────
// SingleCCD and Unknown return false. Used to gate "Phynned is monitoring
// only" messaging.
[[nodiscard]] inline bool
is_asymmetric(uint8_t cpu_class) noexcept {
    return cpu_class == kCpuX3DSingle    ||
           cpu_class == kCpuX3DDual      ||
           cpu_class == kCpuHybridIntel  ||
           cpu_class == kCpuMultiCCXNoX3D;
}

// ── has_vcache() - true for X3D variants only ────────────────────────────
// Use to gate showing the "V-Cache defense" telemetry (Rule 8 only fires
// on X3D; on Intel/non-X3D AMD that metric is always zero and confusing).
[[nodiscard]] inline bool
has_vcache(uint8_t cpu_class) noexcept {
    return cpu_class == kCpuX3DSingle || cpu_class == kCpuX3DDual;
}

// ── help_marker() - ImGui (?) icon with hover tooltip ────────────────────
// Mirrors the dear_imgui demo HelpMarker pattern. Renders an unobtrusive
// "(?)" that expands to a wrapped tooltip on hover. Use inline next to
// any technical term users may not understand.
//
// Usage:
//   ImGui::Text("Aggregate pressure: %.2f", x);
//   ImGui::SameLine();
//   help_marker("Weighted average migration rate across all targets. "
//               "0 = idle, 1.0 = moderate scheduler thrash, "
//               "1.5+ = severe - Phynned is actively counteracting.");
inline void help_marker(const char* desc) noexcept {
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
        if (ImGui::BeginTooltip()) {
            ImGui::PushTextWrapPos(ImGui::GetFontSize() * 35.0f);
            ImGui::TextUnformatted(desc);
            ImGui::PopTextWrapPos();
            ImGui::EndTooltip();
        }
    }
}

// ── format_optimization_summary() - "what's happening right now" line ────
// Combines per-kind target counts + arch into a single user-friendly sentence.
// Caller supplies a fixed-size buffer.
//
//   "Pinning 1 game to V-Cache CCD; 8 background apps on non-V-Cache CCD"
//   "Pinning 2 games to P-cores; 5 background apps on E-cores"
//   "Idle - no games detected (12 background apps observed)"
//   "Idle - no games detected"
//
// Per-kind counts come from the PhynnedClient targets[] array, so callers
// must pass it; falls back to a basic non-breakdown summary when ac is
// null or disconnected.
inline void format_optimization_summary(
    const PhynnedAppState& s,
    const phynned::ipc::PhynnedClient* ac,
    char* buf, size_t bufsize) noexcept
{
    const uint32_t total_targets = s.snap.target_count;
    const uint32_t actions       = s.snap.action_count;
    if (!s.snap.agent_connected) {
        std::snprintf(buf, bufsize, "Agent not running");
        return;
    }

    // Count by TargetKind. Treat Game as the primary optimisation target
    // ("pinned") and everything else that Phynned actively manipulates as
    // "background apps" ("delegated to E-cores / non-V-Cache CCD"):
    //   Stream/Comm/Browser/Productivity → background
    //   Unknown → still observed but no action ("monitoring")
    //   System → never touched (excluded from both counts)
    uint32_t n_games      = 0u;
    uint32_t n_background = 0u;
    uint32_t n_monitor    = 0u;
    if (ac && ac->is_connected()) {
        for (const auto& t : ac->targets()) {
            switch (t.kind) {
                case phynned::observer::TargetKind::Game:
                    ++n_games;
                    break;
                case phynned::observer::TargetKind::Stream:
                case phynned::observer::TargetKind::Comm:
                case phynned::observer::TargetKind::Browser:
                case phynned::observer::TargetKind::Productivity:
                    ++n_background;
                    break;
                case phynned::observer::TargetKind::Unknown:
                    ++n_monitor;
                    break;
                case phynned::observer::TargetKind::System:
                    break;  // never counted
            }
        }
    } else {
        // No client access; can't break down — treat all as monitored.
        n_monitor = total_targets;
    }

    if (n_games == 0u && n_background == 0u) {
        if (n_monitor > 0u) {
            std::snprintf(buf, bufsize,
                "Idle - no games detected (%u background apps observed)",
                n_monitor);
        } else {
            std::snprintf(buf, bufsize, "Idle - no games detected");
        }
        return;
    }

    const char* game_word = (n_games == 1u) ? "game" : "games";
    const char* bg_word   = (n_background == 1u) ? "background app"
                                                 : "background apps";

    // Per-architecture phrasing. The "pin" half always references games;
    // the "delegate" half references background apps. Hide whichever side
    // has zero entries to avoid awkward "0 background apps".
    switch (s.snap.cpu_class) {
        case kCpuX3DDual:
            if (n_games > 0u && n_background > 0u) {
                std::snprintf(buf, bufsize,
                    "Pinning %u %s to V-Cache CCD; %u %s on non-V-Cache CCD "
                    "(%u actions)",
                    n_games, game_word, n_background, bg_word, actions);
            } else if (n_games > 0u) {
                std::snprintf(buf, bufsize,
                    "Pinning %u %s to V-Cache CCD (%u actions)",
                    n_games, game_word, actions);
            } else {
                std::snprintf(buf, bufsize,
                    "Evicting %u %s to non-V-Cache CCD (no games active)",
                    n_background, bg_word);
            }
            break;
        case kCpuX3DSingle:
            if (n_games > 0u) {
                std::snprintf(buf, bufsize,
                    "Pinning %u %s to V-Cache cores (%u actions)",
                    n_games, game_word, actions);
            } else {
                std::snprintf(buf, bufsize,
                    "Observing %u %s (no games active)",
                    n_background, bg_word);
            }
            break;
        case kCpuHybridIntel:
            if (n_games > 0u && n_background > 0u) {
                std::snprintf(buf, bufsize,
                    "Pinning %u %s to P-cores; %u %s on E-cores "
                    "(%u actions)",
                    n_games, game_word, n_background, bg_word, actions);
            } else if (n_games > 0u) {
                std::snprintf(buf, bufsize,
                    "Pinning %u %s to P-cores (%u actions)",
                    n_games, game_word, actions);
            } else {
                std::snprintf(buf, bufsize,
                    "Delegating %u %s to E-cores (no games active)",
                    n_background, bg_word);
            }
            break;
        case kCpuMultiCCXNoX3D:
            if (n_games > 0u) {
                std::snprintf(buf, bufsize,
                    "Isolating %u %s on CCD0; %u %s on CCD1 (%u actions)",
                    n_games, game_word, n_background, bg_word, actions);
            } else {
                std::snprintf(buf, bufsize,
                    "Observing %u %s (no games active)",
                    n_background, bg_word);
            }
            break;
        case kCpuSingleCCD:
            std::snprintf(buf, bufsize,
                "Monitoring %u %s (CPU is symmetric - no affinity applied)",
                n_games + n_background,
                ((n_games + n_background) == 1u) ? "process" : "processes");
            break;
        case kCpuUnknown:
        default:
            std::snprintf(buf, bufsize,
                "Tracking %u %s + %u %s (CPU class pending)",
                n_games, game_word, n_background, bg_word);
            break;
    }
}

} // namespace phynned_ui
// Made with my soul - Swately <3
