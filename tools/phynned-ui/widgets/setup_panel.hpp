// tools/phynned-ui/widgets/setup_panel.hpp
// Setup panel - first-time-user friendly checklist + tutorial.
//
// Goal: a brand-new user opens Phynned, clicks "Setup", and within 30
// seconds knows (a) whether their hardware is supported, (b) whether the
// agent is running with the right privileges, (c) what Phynned will do for
// them, and (d) what they should do next.
//
// This panel is intentionally verbose - it trades dashboard density for
// onboarding clarity. After first run, users live in the Dashboard tab.
//
#pragma once
#include "../PhynnedAppState.hpp"
#include "cpu_class_helpers.hpp"
#include <phynned/ipc/PhynnedClient.hpp>
#include <imgui.h>

inline void draw_setup_panel(const PhynnedAppState& s,
                              const phynned::ipc::PhynnedClient* ac) noexcept
{
    using namespace phynned_ui;
    (void)ac;  // not currently used; reserved for future probes

    const ImVec4 kOk    {0.30f, 0.90f, 0.40f, 1.f};
    const ImVec4 kWarn  {0.95f, 0.80f, 0.20f, 1.f};
    const ImVec4 kBad   {0.95f, 0.30f, 0.25f, 1.f};
    const ImVec4 kGray  {0.55f, 0.55f, 0.55f, 1.f};
    const ImVec4 kHero  {0.13f, 0.16f, 0.20f, 1.f};

    ImGui::Spacing();

    // ─────────────────────────────────────────────────────────────────────
    // Header
    // ─────────────────────────────────────────────────────────────────────
    ImGui::SetWindowFontScale(1.35f);
    ImGui::TextUnformatted("Setup & first-run checklist");
    ImGui::SetWindowFontScale(1.0f);
    ImGui::TextDisabled(
        "Five quick checks so Phynned is set up correctly. Green = good. "
        "Orange or red = action needed.");
    ImGui::Spacing();

    // Helper macro-like lambda for a check row
    auto check_row = [&](const char* label,
                         bool ok,
                         const char* ok_text,
                         const char* warn_text,
                         const char* tooltip) noexcept
    {
        const ImVec4 col = ok ? kOk : kWarn;
        ImGui::PushStyleColor(ImGuiCol_Text, col);
        ImGui::TextUnformatted(ok ? "[OK]" : "[!] ");
        ImGui::PopStyleColor();
        ImGui::SameLine();
        ImGui::Text("%s", label);
        ImGui::SameLine();
        if (tooltip) help_marker(tooltip);
        ImGui::Indent();
        ImGui::TextDisabled("%s", ok ? ok_text : warn_text);
        ImGui::Unindent();
        ImGui::Spacing();
    };

    // ─────────────────────────────────────────────────────────────────────
    // Check 1: agent connectivity
    // ─────────────────────────────────────────────────────────────────────
    {
        const bool ok = s.snap.agent_connected != 0u;
        check_row(
            "1. Agent process",
            ok,
            "phynned-agent.exe is running and publishing telemetry.",
            "phynned-agent.exe is NOT running. Launch it from Start menu "
            "(as administrator for full features), or run install.ps1 "
            "to register it as a Windows service.",
            "The agent is the background process that observes games, "
            "decides what to do, and applies affinity changes. The UI "
            "(this window) reads its published state via shared memory."
        );
    }

    // ─────────────────────────────────────────────────────────────────────
    // Check 2: privileges
    // ─────────────────────────────────────────────────────────────────────
    {
        const uint8_t priv = s.snap.privilege_level;
        const bool ok = priv >= 3u;
        const bool partial = priv >= 1u && priv < 3u;
        static const char* kPrivNames[] = {
            "no privileges", "partial", "elevated", "administrator"
        };
        const char* current = (priv < 4u) ? kPrivNames[priv] : "unknown";

        const ImVec4 col = ok ? kOk : (partial ? kWarn : kBad);
        ImGui::PushStyleColor(ImGuiCol_Text, col);
        ImGui::TextUnformatted(ok ? "[OK]" : (partial ? "[!] " : "[X] "));
        ImGui::PopStyleColor();
        ImGui::SameLine();
        ImGui::Text("2. Privileges: %s", current);
        ImGui::SameLine();
        help_marker(
            "Administrator is needed to set affinity on processes owned "
            "by other users and to start an ETW kernel session.");
        ImGui::Indent();
        if (ok) {
            ImGui::TextDisabled(
                "Running as administrator - all features available.");
        } else if (partial) {
            ImGui::TextDisabled(
                "Running with limited privileges. ETW will not be "
                "available; some affinity actions may fail silently.");
        } else {
            ImGui::TextDisabled(
                "Not running as admin. Restart Phynned as administrator "
                "to unlock full functionality.");
        }
        ImGui::Unindent();
        ImGui::Spacing();
    }

    // ─────────────────────────────────────────────────────────────────────
    // Check 3: hardware classification - the big one
    // ─────────────────────────────────────────────────────────────────────
    {
        const bool asymm = is_asymmetric(s.snap.cpu_class);
        const bool unknown = (s.snap.cpu_class == kCpuUnknown);
        const ImVec4 col = unknown ? kGray : (asymm ? kOk : kWarn);

        ImGui::PushStyleColor(ImGuiCol_Text, col);
        ImGui::TextUnformatted(unknown ? "[..]"
                                       : (asymm ? "[OK]" : "[!] "));
        ImGui::PopStyleColor();
        ImGui::SameLine();
        ImGui::Text("3. CPU: %s", arch_long_label(s.snap.cpu_class));

        ImGui::Indent();
        ImGui::PushStyleColor(ImGuiCol_ChildBg, kHero);
        ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 4.f);
        ImGui::BeginChild("##setup_strategy", ImVec2{0, 70.f}, true);
        ImGui::TextDisabled("Strategy for this CPU:");
        ImGui::TextWrapped("%s", what_phynned_does(s.snap.cpu_class));
        if (s.snap.cpu_class == kCpuHybridIntel) {
            ImGui::TextDisabled("Detected: %u P-cores, %u E-cores.",
                                s.snap.p_core_count, s.snap.e_core_count);
        } else if (asymm) {
            ImGui::TextDisabled("Detected CCDs: %u.", s.snap.ccd_count);
        }
        ImGui::EndChild();
        ImGui::PopStyleVar();
        ImGui::PopStyleColor();
        ImGui::Unindent();
        ImGui::Spacing();
    }

    // ─────────────────────────────────────────────────────────────────────
    // Check 4: watchdog
    // ─────────────────────────────────────────────────────────────────────
    {
        const bool ok = s.snap.watchdog_ok != 0u || !s.snap.agent_connected;
        check_row(
            "4. Internal watchdog",
            ok,
            "Agent loop is responsive.",
            "Agent watchdog detected a stall. Restart phynned-agent.",
            "Phynned runs a self-watchdog: if the main loop doesn't tick "
            "within 200 ms, it flags as STALLED and the UI shows red. "
            "Restart the agent process if this triggers."
        );
    }

    // ─────────────────────────────────────────────────────────────────────
    // Check 5: PresentMon (for Benchmark tab)
    // ─────────────────────────────────────────────────────────────────────
    {
        // We can't easily detect PresentMon presence here without a runtime
        // probe; just inform the user.
        ImGui::PushStyleColor(ImGuiCol_Text, kGray);
        ImGui::TextUnformatted("[..]");
        ImGui::PopStyleColor();
        ImGui::SameLine();
        ImGui::Text("5. PresentMon (optional - for Benchmark tab)");
        ImGui::SameLine();
        help_marker(
            "PresentMon is Intel's frame-capture tool. Phynned bundles it "
            "for the A/B/A/B/A benchmark protocol in the Benchmark tab. "
            "If you only want auto-optimization, you don't need it.");
        ImGui::Indent();
        ImGui::TextDisabled(
            "Auto-downloaded at first build. If missing, place "
            "PresentMon.exe next to phynned-ui.exe.");
        ImGui::Unindent();
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // ─────────────────────────────────────────────────────────────────────
    // Next steps / cheat sheet
    // ─────────────────────────────────────────────────────────────────────
    if (ImGui::CollapsingHeader("What to do next", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Indent();
        ImGui::BulletText("Start any single-player game (no anti-cheat - see FAQ).");
        ImGui::BulletText("Go to 'Dashboard' - you should see the game listed.");
        ImGui::BulletText("'Targets' tab shows per-process metrics.");
        ImGui::BulletText("'Policies' tab lets you toggle individual rules.");
        ImGui::BulletText("'Benchmark' tab runs A/B/A/B/A to measure impact.");
        ImGui::Spacing();
        ImGui::TextDisabled(
            "Tip: Phynned defaults are conservative. If a policy hurts "
            "performance, it auto-reverts and bad-lists that exe.");
        ImGui::Unindent();
    }

    if (ImGui::CollapsingHeader("Supported CPU families")) {
        ImGui::Indent();
        ImGui::BulletText("AMD Ryzen 3D V-Cache (any CCD count)");
        ImGui::Text("    7800X3D, 7900X3D, 7950X3D, 9800X3D, 9950X3D");
        ImGui::BulletText("Intel hybrid (12th gen and newer)");
        ImGui::Text("    Alder / Raptor / Meteor / Arrow Lake, P+E");
        ImGui::BulletText("AMD Ryzen multi-CCD without V-Cache");
        ImGui::Text("    5950X, 5900X, 7900X, 7950X, 9900X, 9950X, TR PRO");
        ImGui::BulletText("Symmetric single-CCD CPUs: monitoring only");
        ImGui::TextDisabled(
            "    (no asymmetric topology -> core pinning has no effect)");
        ImGui::Unindent();
    }
}
// Made with my soul - Swately <3
