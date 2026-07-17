// tools/phynned-ui/widgets/advanced_panel.hpp
// Advanced panel — power-user controls and diagnostics.
//
// Shows:
//   - Raw IPC/SHM diagnostics (magic, seq, agent PID)
//   - Tick rate override
//   - Manual force-pin / force-revert per PID
//   - Config file path + memory.toml location
//   - Version / build info
#pragma once
#include "../PhynnedAppState.hpp"
#include <phynned/ipc/PhynnedClient.hpp>
#include <phynned/version.hpp>
#include <phyriad/version.hpp>
#include <imgui.h>
#include <cstdio>

inline void draw_advanced_panel(const PhynnedAppState& s,
                                 const phynned::ipc::PhynnedClient* ac) noexcept
{
    ImGui::Spacing();

    // ── IPC / SHM status ─────────────────────────────────────────────────────
    ImGui::Text("IPC / Shared memory");
    ImGui::Separator();
    ImGui::Indent();

    if (ac == nullptr) {
        ImGui::TextDisabled("PhynnedClient: not initialized");
    } else if (!ac->is_connected()) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4{0.95f,0.30f,0.25f,1.f});
        ImGui::TextUnformatted("PhynnedClient: disconnected");
        ImGui::PopStyleColor();
        ImGui::TextDisabled("SHM name: Local\\PhynnedAgent.v1");
        ImGui::TextDisabled("Ensure phynned-agent.exe is running (as admin).");
    } else {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4{0.30f,0.90f,0.40f,1.f});
        ImGui::TextUnformatted("PhynnedClient: connected");
        ImGui::PopStyleColor();
        ImGui::Text("SHM name:     Local\\PhynnedAgent.v1");
        ImGui::Text("Agent PID:    %u", s.agent_pid);
        ImGui::Text("SHM size:     1 MB");
        ImGui::Text("Layout:       ShmHeader[128] + StateHeader[128] + "
                    "targets[32×64] + metrics[32×128] + decisions[16×32] "
                    "+ ActionLogRing");
        ImGui::Spacing();
        ImGui::Text("Targets in SHM:   %u", s.snap.target_count);
        ImGui::Text("Decisions/cycle:  %u", s.snap.decision_count);
        ImGui::Text("Last sync TSC:    %llu",
            static_cast<unsigned long long>(s.snap.last_sync_tsc));
    }

    ImGui::Unindent();
    ImGui::Spacing();

    // ── Agent config paths ────────────────────────────────────────────────────
    ImGui::Text("Configuration files");
    ImGui::Separator();
    ImGui::Indent();
    ImGui::TextDisabled("Policy config:  %%LOCALAPPDATA%%\\Phynned\\policies.toml");
    ImGui::TextDisabled("Memory store:   %%LOCALAPPDATA%%\\Phynned\\memory.toml");
    ImGui::TextDisabled("Audit log:      %%LOCALAPPDATA%%\\Phynned\\audit.bin");
    ImGui::TextDisabled("Classifier:     %%LOCALAPPDATA%%\\Phynned\\classifier.toml");
    ImGui::Unindent();
    ImGui::Spacing();

    // ── Manual override (informational only) ──────────────────────────────────
    // The actual force-pin path requires bi-directional IPC (UI -> agent
    // command channel). v1.0 ships the read-only SHM publisher only; the
    // command channel will land in a follow-up release. For now we document
    // the workaround so power users aren't left wondering where the knob is.
    ImGui::Text("Manual override (power user)");
    ImGui::Separator();
    ImGui::Indent();
    ImGui::TextWrapped(
        "In-UI manual pinning is not available in this release. To force "
        "a specific affinity mask on a process today, edit the policy "
        "config and restart the agent, or use the phynned-cli tool:");
    ImGui::Spacing();
    ImGui::Indent();
    ImGui::TextDisabled("phynned-cli pin --pid <PID> --mask <HEX>");
    ImGui::TextDisabled("phynned-cli revert --pid <PID>");
    ImGui::Unindent();
    ImGui::Spacing();
    ImGui::TextDisabled(
        "A bi-directional command channel (Apply/Revert buttons here) is "
        "on the roadmap for the next release.");
    ImGui::Unindent();
    ImGui::Spacing();

    // ── Resource limits display ───────────────────────────────────────────────
    ImGui::Text("Anti-parasitic budget");
    ImGui::Separator();
    ImGui::Indent();
    ImGui::TextDisabled("CPU idle target: < 0.1%%  hard limit: < 0.3%%");
    ImGui::TextDisabled("CPU active target: < 0.5%%  hard limit: < 1.0%%");
    ImGui::TextDisabled("RAM idle target: < 15 MB  hard limit: < 20 MB");
    ImGui::TextDisabled("RAM active target: < 30 MB  hard limit: < 50 MB");
    ImGui::TextDisabled("Network: 0 bytes (Phynned is 100%% offline)");
    ImGui::Spacing();
    // Current values from state
    ImGui::Text("Current agent CPU:  %.2f %%", s.self_cpu_pct);
    ImGui::Text("Current agent RAM:  %.1f MB", s.self_rss_mb);
    ImGui::Text("Current tick:       %u ms",   s.tick_interval_ms);
    ImGui::Text("Self-pinned core:   %u",       s.self_pin_core);
    ImGui::Unindent();
    ImGui::Spacing();

    // ── Version info ──────────────────────────────────────────────────────────
    ImGui::Text("Version info");
    ImGui::Separator();
    ImGui::Indent();
    ImGui::Text("phynned-ui:    Phynned %s", PHYNNED_VERSION_STRING);
    ImGui::Text("agent:       %s",
                s.agent_version[0] ? s.agent_version : "unknown");
    ImGui::TextDisabled("Phyriad framework: %s", PHYRIAD_VERSION_STRING);
    ImGui::TextDisabled("Build:           C++23 / MinGW-w64 / Windows");
    ImGui::TextDisabled("SHM protocol:    PhynnedProtocol v1 (seqlock)");
    ImGui::Unindent();
}
// Made with my soul - Swately <3
