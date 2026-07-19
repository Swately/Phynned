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

// W3 user-rule action → label. 0=Never 1=Freq 2=VCache.
inline const char* user_action_str(uint8_t a) noexcept {
    switch (a) {
        case 1:  return "Always -> Freq CCD";
        case 2:  return "Always -> V-Cache CCD";
        default: return "Never optimize";
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

    // ── W4 global use-mode (profile) selector ─────────────────────────────────
    // Monitor = observe/advise only (zero placement). Games = classic game
    // placement, corral off. Games+Corral = today's default. Full is reserved for
    // the MR-3 A/B engine (shown disabled). Radio changes flow through IPC; the
    // agent reverts the appropriate placements + persists to policies.toml.
    {
        const bool can_command = ac->can_send_commands();
        ImGui::TextUnformatted("Use mode (profile):");
        int prof = static_cast<int>(s.snap.profile);
        auto* mac = const_cast<phynned::ipc::PhynnedClient*>(ac);

        if (!can_command) ImGui::BeginDisabled();
        if (ImGui::RadioButton("Monitor - observe & advise only (zero placement)",
                               &prof, 0)) {
            (void)mac->send_command(phynned::ipc::kPhynnedCmdSetProfile, 0u, 0ull);
        }
        if (ImGui::RadioButton("Games - game placement only (corral off)",
                               &prof, 1)) {
            (void)mac->send_command(phynned::ipc::kPhynnedCmdSetProfile, 0u, 1ull);
        }
        if (ImGui::RadioButton(
                "Games + Corral - game placement + background corral (default)",
                &prof, 2)) {
            (void)mac->send_command(phynned::ipc::kPhynnedCmdSetProfile, 0u, 2ull);
        }
        if (!can_command) ImGui::EndDisabled();

        ImGui::BeginDisabled();
        bool full_sel = (prof == 3);
        ImGui::Checkbox("Full - automatic A/B routing (requires the A/B engine, MR-3)",
                        &full_sel);
        ImGui::EndDisabled();

        if (s.snap.profile == 0u) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4{0.45f, 0.80f, 1.00f, 1.f});
            ImGui::TextWrapped(
                "Monitor mode: Phynned observes and advises but changes NO process "
                "affinity anywhere. Telemetry and shadow advice continue.");
            ImGui::PopStyleColor();
        }
    }

    ImGui::Separator();

    // ── MR-2 background corral switch ─────────────────────────────────────────
    // Moves ACTIVE non-game background off the V-Cache CCD onto the Frequency CCD.
    // DEFAULT DRY-RUN: the agent computes/surfaces what it WOULD move (see the
    // "would-corral" marker in the Targets tab) but changes NO real affinity until
    // this switch is turned on — which changes live process affinity.
    {
        const bool live         = (s.snap.corral_live != 0u);
        const bool coexist_block= (s.snap.corral_coexist_block != 0u);
        const bool effective_live = live && !coexist_block;
        const bool can_command  = ac->can_send_commands();

        ImGui::TextUnformatted("Background corral:");
        ImGui::SameLine();
        if (effective_live) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4{0.95f, 0.45f, 0.30f, 1.f});
            ImGui::TextUnformatted("LIVE (changing real process affinity)");
        } else {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4{0.45f, 0.80f, 1.00f, 1.f});
            ImGui::TextUnformatted("DRY-RUN (nothing applied)");
        }
        ImGui::PopStyleColor();

        // W2: keep_placements_on_disable is NOT echoed via SHM (only the @70
        // profile byte was carved), so the UI holds the checkbox state locally
        // for this session. The agent persists it to policies.toml on every
        // SetCorralLive command (arg2 piggyback).
        static bool keep_on_disable = false;
        auto* cmac = const_cast<phynned::ipc::PhynnedClient*>(ac);

        if (!can_command) ImGui::BeginDisabled();
        bool live_toggle = live;
        if (ImGui::Checkbox(
                "Enable background corral (moves active background off the "
                "V-Cache CCD)", &live_toggle)) {
            (void)cmac->send_command(
                phynned::ipc::kPhynnedCmdSetCorralLive, 0u,
                live_toggle ? 1ull : 0ull,
                keep_on_disable ? 1ull : 0ull);
        }
        if (ImGui::Checkbox(
                "Keep placements when disabling (old stop-only behavior)",
                &keep_on_disable)) {
            // Re-send with the current corral state so the agent records the new
            // keep flag without changing the corral on/off state.
            (void)cmac->send_command(
                phynned::ipc::kPhynnedCmdSetCorralLive, 0u,
                live ? 1ull : 0ull,
                keep_on_disable ? 1ull : 0ull);
        }
        if (!can_command) ImGui::EndDisabled();

        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4{0.95f, 0.75f, 0.20f, 1.f});
        ImGui::TextWrapped(
            "WARNING: enabling this changes LIVE process affinity — it hard-pins "
            "active non-game background processes to the frequency CCD to keep the "
            "V-Cache clean. Off by default; leave it in DRY-RUN unless you are "
            "deliberately testing placement.");
        ImGui::PopStyleColor();
        if (coexist_block) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4{0.95f, 0.55f, 0.25f, 1.f});
            ImGui::TextWrapped(
                "Coexistence guard active: a conflicting affinity manager "
                "(Process Lasso) is running, so the corral is held in DRY-RUN "
                "regardless of this switch to avoid flapping. (The AMD 3D V-Cache "
                "service is complementary — it handles games, which the corral "
                "already excludes — so it does NOT trigger this guard.)");
            ImGui::PopStyleColor();
        }
    }

    ImGui::Separator();

    // ── W3 per-process user rules editor ──────────────────────────────────────
    // The agent is the single writer of policies.toml; this table mirrors the
    // published rules and offers removal. Creation happens by right-clicking a
    // row in the Targets tab (the agent resolves the exe name from the PID).
    {
        ImGui::TextUnformatted("Per-process user rules:");
        ImGui::TextDisabled(
            "Create with a right-click on a row in the Targets tab. "
            "Never = never optimize; Freq / V-Cache = always pin to that CCD "
            "(still anti-cheat gated).");

        const auto rules   = ac->user_rules();
        const uint32_t gen = ac->user_rules_generation();

        if (rules.empty()) {
            ImGui::TextDisabled("(no user rules set)");
        } else if (ImGui::BeginTable("##user_rules", 4,
                ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                ImGuiTableFlags_SizingStretchProp)) {
            ImGui::TableSetupColumn("Process", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Path",    ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Rule",    ImGuiTableColumnFlags_WidthFixed, 180.f);
            ImGui::TableSetupColumn("",        ImGuiTableColumnFlags_WidthFixed, 70.f);
            ImGui::TableHeadersRow();

            for (uint32_t i = 0u; i < static_cast<uint32_t>(rules.size()); ++i) {
                const auto& r = rules[i];
                ImGui::PushID(static_cast<int>(i));
                ImGui::TableNextRow();

                ImGui::TableSetColumnIndex(0);
                ImGui::TextUnformatted(r.name[0] ? r.name : "<unnamed>");

                ImGui::TableSetColumnIndex(1);
                if (r.flags & phynned::ipc::kUserRuleFlagHasPath)
                    ImGui::TextUnformatted(r.path);
                else
                    ImGui::TextDisabled("(any)");

                ImGui::TableSetColumnIndex(2);
                ImGui::TextUnformatted(user_action_str(r.action));
                if (r.flags & phynned::ipc::kUserRuleFlagBlockedAc) {
                    ImGui::SameLine();
                    ImGui::PushStyleColor(ImGuiCol_Text,
                                          ImVec4{0.95f, 0.45f, 0.30f, 1.f});
                    ImGui::TextUnformatted("[AC blocked]");
                    ImGui::PopStyleColor();
                }
                if (r.flags & phynned::ipc::kUserRuleFlagFlapWarn) {
                    ImGui::SameLine();
                    ImGui::PushStyleColor(ImGuiCol_Text,
                                          ImVec4{0.95f, 0.75f, 0.20f, 1.f});
                    ImGui::TextUnformatted("[pinned elsewhere]");
                    ImGui::PopStyleColor();
                }

                ImGui::TableSetColumnIndex(3);
                if (ac->can_send_commands() && ImGui::SmallButton("Remove")) {
                    // arg1 = slot index, arg2 = generation (stale ⇒ agent ignores).
                    (void)const_cast<phynned::ipc::PhynnedClient*>(ac)->send_command(
                        phynned::ipc::kPhynnedCmdRemoveProcessRule, 0u,
                        static_cast<uint64_t>(i), static_cast<uint64_t>(gen));
                }
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
    }

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
