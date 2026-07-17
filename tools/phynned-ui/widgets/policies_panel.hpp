// tools/phynned-ui/widgets/policies_panel.hpp
// Policies panel - list of active rules and decisions from the current cycle.
//
// Reads decisions[] directly from PhynnedClient.
// Shows PolicyEngine rules and allows enable/disable.
#pragma once
#include "../PhynnedAppState.hpp"
#include <phynned/ipc/PhynnedClient.hpp>
#include <phynned/policy/PolicyDecision.hpp>
#include <imgui.h>
#include <cstdio>

namespace {

inline const char* action_kind_str(uint8_t k) noexcept {
    switch (k) {
        case 1: return "PinAffinity";
        case 2: return "SetPriority";
        case 3: return "Revert";
        default: return "Unknown";
    }
}

} // anonymous namespace

inline void draw_policies_panel(const PhynnedAppState& s,
                                 const phynned::ipc::PhynnedClient* ac) noexcept
{
    ImGui::Spacing();

    if (ac == nullptr || !s.snap.agent_connected) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4{0.95f, 0.75f, 0.20f, 1.f});
        ImGui::TextWrapped("Phynned agent is not running. "
                           "Launch phynned-agent.exe to see active policies.");
        ImGui::PopStyleColor();
        return;
    }

    const auto decisions = ac->decisions();

    // ── Operation mode selector ───────────────────────────────────────────────
    ImGui::Text("Operation mode:");
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4{0.4f, 0.8f, 1.f, 1.f});
    static const char* kModes[] = { "Auto", "Assist", "Manual" };
    ImGui::TextUnformatted(s.op_mode < 3u ? kModes[s.op_mode] : "?");
    ImGui::PopStyleColor();
    ImGui::SameLine();
    ImGui::TextDisabled("  (mode changes via agent config - restart required)");

    ImGui::Separator();

    // ── Built-in default rules (read-only display) ────────────────────────────
    ImGui::Text("Default policy rules (auto-selected by hardware topology):");
    ImGui::Spacing();

    if (ImGui::BeginTable("##rules", 3,
        ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
        ImGuiTableFlags_SizingStretchProp))
    {
        ImGui::TableSetupColumn("Rule",        ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Condition",   ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Action",      ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();

        struct DefaultRule { const char* name; const char* cond; const char* act; };
        static constexpr DefaultRule kDefaults[] = {
            { "PinGameToVCacheCcd",
              "Kind=Game AND topology has V-Cache",
              "Pin to V-Cache CCD cores" },
            { "PinGameToPCores",
              "Kind=Game AND topology has E-cores",
              "Pin to P-cores only" },
            { "EvictStreamFromHotCcd",
              "Kind=Stream AND any Game running",
              "Pin to non-V-Cache CCD / E-cores" },
            { "PinCommToECores",
              "Kind=Comm AND topology has E-cores",
              "Pin to E-cores" },
        };

        for (const auto& r : kDefaults) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0); ImGui::TextUnformatted(r.name);
            ImGui::TableSetColumnIndex(1); ImGui::TextUnformatted(r.cond);
            ImGui::TableSetColumnIndex(2); ImGui::TextUnformatted(r.act);
        }

        ImGui::EndTable();
    }

    ImGui::Separator();

    // ── Active decisions this cycle ───────────────────────────────────────────
    ImGui::Text("Decisions in last cycle: %u",
                static_cast<uint32_t>(decisions.size()));
    ImGui::Spacing();

    if (decisions.empty()) {
        ImGui::TextDisabled("No policy decisions were generated this cycle.");
        ImGui::TextDisabled(
            "This means no targets matched the active rules, "
            "or no targets are running.");
        return;
    }

    if (ImGui::BeginTable("##decisions", 5,
        ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
        ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp,
        ImVec2{0.f, ImGui::GetTextLineHeightWithSpacing() * 8.f}))
    {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Target PID",   ImGuiTableColumnFlags_WidthFixed,  80.f);
        ImGui::TableSetupColumn("Rule ID",      ImGuiTableColumnFlags_WidthFixed,  65.f);
        ImGui::TableSetupColumn("Action",       ImGuiTableColumnFlags_WidthFixed,  90.f);
        ImGui::TableSetupColumn("Core mask",    ImGuiTableColumnFlags_WidthFixed,  90.f);
        ImGui::TableSetupColumn("Confidence",   ImGuiTableColumnFlags_WidthFixed,  75.f);
        ImGui::TableHeadersRow();

        for (const auto& d : decisions) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::Text("%u", d.target_pid);
            ImGui::TableSetColumnIndex(1);
            ImGui::Text("#%u", d.rule_id);
            ImGui::TableSetColumnIndex(2);
            // d.action_kind es phynned::policy::ActionKind (enum class uint8_t)
            ImGui::TextUnformatted(
                action_kind_str(static_cast<uint8_t>(d.action_kind)));
            ImGui::TableSetColumnIndex(3);
            if (d.core_mask != 0u)
                ImGui::Text("0x%08llX", static_cast<unsigned long long>(d.core_mask));
            else
                ImGui::TextDisabled("--");
            ImGui::TableSetColumnIndex(4);
            // Confidence bar
            const float conf = static_cast<float>(d.confidence) / 100.f;
            char buf[8];
            std::snprintf(buf, sizeof(buf), "%u%%", d.confidence);
            ImGui::ProgressBar(conf, ImVec2{-1.f, 0.f}, buf);
        }

        ImGui::EndTable();
    }

    ImGui::Spacing();
    ImGui::TextDisabled(
        "This release ships the built-in rules in read-only display "
        "mode. Per-rule enable/disable and custom rules are managed "
        "via the policies.toml config file (Advanced tab shows the "
        "path). An in-UI Manual mode is on the roadmap for a follow-up "
        "release.");
}
// Made with my soul - Swately <3
