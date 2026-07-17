// tools/phynned-ui/widgets/dashboard_panel.hpp
// Dashboard panel - default view of the Auto mode.
//
// Layout:
//   - Hero card at top: plain-language status + detected architecture
//   - X3D-only telemetry (CCD defense) hidden on non-X3D arch
//   - Tooltips on technical terms via help_marker()
//   - "Advanced details" collapsibles for power-user fields
//
// Contract:
//   - Receives PhynnedAppState (published by PhynnedLogicNode each frame)
//   - Receives PhynnedClient* for direct SHM array access in panels
//   - If ac == nullptr || !ac->is_connected(): graceful fallback
//   - Zero dynamic allocation in draw loop
#pragma once
#include "../PhynnedAppState.hpp"
#include "cpu_class_helpers.hpp"
#include <phynned/ipc/PhynnedClient.hpp>
#include <imgui.h>
#include <cstdio>

inline void draw_dashboard_panel(const PhynnedAppState& s,
                                  const phynned::ipc::PhynnedClient* ac) noexcept
{
    using namespace phynned_ui;

    ImGui::Spacing();

    // ─────────────────────────────────────────────────────────────────────
    // Control bar — Start / Pause / Reset
    //
    // Safety-first: Phynned defaults to PAUSED. Nothing is applied to any
    // process until the user explicitly clicks Start. This protects users
    // who launch Phynned out of curiosity while a kernel-anticheat game is
    // running — the CPU-affinity changes Phynned would otherwise make are
    // gated behind this opt-in.
    //
    // Commands go through PhynnedClient::send_command(), which writes to a
    // dedicated SHM slot the agent polls every tick. The status indicator
    // shows the AGENT's actual policies_paused value (round-tripped
    // through SHM), not just what we last asked for — so a wedged agent
    // can't lie about its state.
    // ─────────────────────────────────────────────────────────────────────
    if (ac && ac->is_connected()) {
        const bool paused = (s.snap.policies_paused != 0u);
        const bool can_command = ac->can_send_commands();

        // Status text
        if (paused) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4{0.95f, 0.80f, 0.20f, 1.f});
            ImGui::TextUnformatted("Status: PAUSED");
            ImGui::PopStyleColor();
            ImGui::SameLine();
            ImGui::TextDisabled(
                "(no optimizations are being applied)");
        } else {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4{0.30f, 0.90f, 0.40f, 1.f});
            ImGui::TextUnformatted("Status: ACTIVE");
            ImGui::PopStyleColor();
            ImGui::SameLine();
            ImGui::TextDisabled("(optimizations are being applied)");
        }
        ImGui::Spacing();

        // Start button — green; only enabled when paused.
        const bool start_enabled = can_command && paused;
        if (!start_enabled) ImGui::BeginDisabled();
        ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4{0.18f, 0.55f, 0.25f, 1.f});
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4{0.25f, 0.70f, 0.32f, 1.f});
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4{0.30f, 0.80f, 0.38f, 1.f});
        if (ImGui::Button("Start", ImVec2{110.f, 36.f})) {
            (void)const_cast<phynned::ipc::PhynnedClient*>(ac)
                ->send_command(phynned::ipc::kPhynnedCmdResumePolicies);
        }
        ImGui::PopStyleColor(3);
        if (!start_enabled) ImGui::EndDisabled();
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(
                "Begin applying CPU affinity / priority optimizations.\n"
                "Make sure no kernel-anticheat games are running first.");
        }

        ImGui::SameLine();

        // Pause button — yellow; only enabled when active.
        const bool pause_enabled = can_command && !paused;
        if (!pause_enabled) ImGui::BeginDisabled();
        ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4{0.65f, 0.55f, 0.15f, 1.f});
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4{0.80f, 0.65f, 0.20f, 1.f});
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4{0.90f, 0.75f, 0.25f, 1.f});
        if (ImGui::Button("Pause", ImVec2{110.f, 36.f})) {
            (void)const_cast<phynned::ipc::PhynnedClient*>(ac)
                ->send_command(phynned::ipc::kPhynnedCmdPausePolicies);
        }
        ImGui::PopStyleColor(3);
        if (!pause_enabled) ImGui::EndDisabled();
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(
                "Revert all active optimizations and stop applying new ones.\n"
                "Click Start to resume.");
        }

        ImGui::SameLine();

        // Reset button — red; always enabled (forces revert even if paused).
        if (!can_command) ImGui::BeginDisabled();
        ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4{0.65f, 0.18f, 0.18f, 1.f});
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4{0.80f, 0.22f, 0.22f, 1.f});
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4{0.90f, 0.30f, 0.30f, 1.f});
        if (ImGui::Button("Reset", ImVec2{110.f, 36.f})) {
            (void)const_cast<phynned::ipc::PhynnedClient*>(ac)
                ->send_command(phynned::ipc::kPhynnedCmdForceRevertAll);
        }
        ImGui::PopStyleColor(3);
        if (!can_command) ImGui::EndDisabled();
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(
                "Revert every active optimization right now and stay paused.\n"
                "Use after the Start button if something feels off — Phynned\n"
                "leaves processes in a clean state.");
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
    }

    // ─────────────────────────────────────────────────────────────────────
    // Agent not running - friendly onboarding fallback
    // ─────────────────────────────────────────────────────────────────────
    if (ac == nullptr || !s.snap.agent_connected) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4{0.95f, 0.75f, 0.20f, 1.f});
        ImGui::SetWindowFontScale(1.4f);
        ImGui::TextUnformatted("Welcome to Phynned");
        ImGui::SetWindowFontScale(1.0f);
        ImGui::PopStyleColor();
        ImGui::Spacing();
        ImGui::TextWrapped(
            "The Phynned agent is not currently running. "
            "Once you launch phynned-agent.exe, this dashboard refreshes "
            "automatically.");
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
        ImGui::TextDisabled("How to start the agent:");
        ImGui::BulletText("Run as administrator for ETW + cross-process affinity.");
        ImGui::BulletText("Or use 'install.ps1' to register it as a Windows service.");
        ImGui::Spacing();
        ImGui::TextDisabled("Tip: open the 'Setup' tab for a step-by-step checklist.");
        return;
    }

    // ─────────────────────────────────────────────────────────────────────
    // Hero card - what Phynned is doing, right now, in plain language
    // ─────────────────────────────────────────────────────────────────────
    {
        const ImVec4 kHeroBg {0.13f, 0.16f, 0.20f, 1.f};
        ImGui::PushStyleColor(ImGuiCol_ChildBg, kHeroBg);
        ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 6.f);
        ImGui::BeginChild("##hero", ImVec2{0, 110.f}, true);

        // Big status line. Pass the client so the summary can break down
        // counts per-kind (games vs background apps vs monitored) rather
        // than reporting the misleading raw target_count.
        char summary[200];
        format_optimization_summary(s, ac, summary, sizeof(summary));
        ImGui::SetWindowFontScale(1.25f);
        const ImVec4 kHeroOk     {0.55f, 0.95f, 0.65f, 1.f};
        const ImVec4 kHeroIdle   {0.75f, 0.75f, 0.80f, 1.f};
        const bool active = (s.snap.target_count > 0u) &&
                            is_asymmetric(s.snap.cpu_class);
        ImGui::PushStyleColor(ImGuiCol_Text, active ? kHeroOk : kHeroIdle);
        ImGui::TextUnformatted(summary);
        ImGui::PopStyleColor();
        ImGui::SetWindowFontScale(1.0f);

        ImGui::Spacing();

        // Subline: detected architecture + strategy
        ImGui::TextDisabled("CPU detected: %s", arch_long_label(s.snap.cpu_class));
        ImGui::SameLine();
        help_marker(
            "Phynned auto-detects your CPU topology at startup. The strategy "
            "below is chosen based on which asymmetric features are present "
            "(V-Cache CCD, P/E cores, or multi-CCD). For symmetric CPUs, "
            "Phynned monitors but does not change process affinity.");

        ImGui::TextWrapped("Strategy: %s", what_phynned_does(s.snap.cpu_class));

        ImGui::EndChild();
        ImGui::PopStyleVar();
        ImGui::PopStyleColor();
    }

    ImGui::Spacing();

    // ─────────────────────────────────────────────────────────────────────
    // "Currently optimized" mini-list — always visible, focused on games.
    //
    // The user opens the dashboard primarily to answer one question: "is
    // MY game being optimized RIGHT NOW?". This box answers that without
    // requiring the user to click into the collapsible below. Shows ONLY
    // games with an active action (cross-referencing PhynnedClient targets
    // against the action_log for unreverted entries on the same PID).
    // Empty state explicitly says "no games being optimized" so the user
    // doesn't confuse "Phynned is idle" with "Phynned is broken".
    // ─────────────────────────────────────────────────────────────────────
    if (ac && ac->is_connected()) {
        const auto targets = ac->targets();
        const auto log     = ac->action_log();
        // Build active-pid set from the action log.
        //
        // The log is a CIRCULAR ring whose slots are stored by physical
        // position, not chronologically — naive "walk last to first"
        // breaks down because revert entries can sit at lower indices
        // than their corresponding apply entry after wrap-around.
        //
        // Correct algorithm: for each entry with target_pid == pid,
        // track the LARGEST event timestamp (max(tsc_applied,
        // tsc_reverted)). The entry with the largest event-tsc is the
        // most recent action on that PID; its tsc_reverted field tells
        // us whether the action is currently applied (0) or reverted.
        auto pid_has_active_action = [&](uint32_t pid) noexcept -> bool {
            uint64_t latest_tsc = 0u;
            bool active = false;
            for (std::size_t i = 0u; i < log.size(); ++i) {
                const auto& e = log[i];
                if (e.target_pid != pid) continue;
                const uint64_t evt_tsc = (e.tsc_reverted != 0u)
                    ? e.tsc_reverted : e.tsc_applied;
                if (evt_tsc > latest_tsc) {
                    latest_tsc = evt_tsc;
                    active = (e.tsc_reverted == 0u);
                }
            }
            return active;
        };

        // Compact fixed-height card. Estimated row height ~22 px; budget
        // ~6 rows (title + 4-5 games + maybe a hint) before scrolling
        // kicks in. Anything more than that lives in the collapsible
        // below. This keeps the metrics row at the bottom of the
        // window visible without scrolling.
        const float kCardHeight = 132.f;
        const ImVec4 kBoxBg {0.10f, 0.14f, 0.11f, 1.f};
        ImGui::PushStyleColor(ImGuiCol_ChildBg, kBoxBg);
        ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 6.f);
        ImGui::BeginChild("##nowopt", ImVec2{0, kCardHeight}, true);

        ImGui::TextUnformatted("Currently optimized");
        ImGui::SameLine();
        if (s.snap.policies_paused) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4{0.95f, 0.80f, 0.20f, 1.f});
            ImGui::TextUnformatted("(paused - waiting for Start)");
            ImGui::PopStyleColor();
        }
        ImGui::Separator();

        uint32_t shown = 0u;
        for (const auto& t : targets) {
            if (t.kind != phynned::observer::TargetKind::Game) continue;
            if (!pid_has_active_action(t.pid))               continue;
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4{0.55f, 0.95f, 0.65f, 1.f});
            ImGui::Bullet();
            ImGui::SameLine();
            ImGui::TextUnformatted(t.name);
            ImGui::PopStyleColor();
            ImGui::SameLine();
            ImGui::TextDisabled("(PID %u)", t.pid);
            ++shown;
        }

        if (shown == 0u) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4{0.65f, 0.65f, 0.65f, 1.f});
            // Single-line message when paused vs idle so the box stays
            // compact regardless of state.
            if (s.snap.policies_paused) {
                ImGui::TextUnformatted(
                    "Click Start above to begin optimizing detected games.");
            } else if (s.snap.action_count > 0u) {
                // Agent reports active actions but no game PID matched
                // — likely a non-Game kind being optimized (rare). Hint
                // the user toward the full list.
                ImGui::Text(
                    "%u active action(s) on non-Game processes. "
                    "See the full breakdown below.", s.snap.action_count);
            } else {
                ImGui::TextUnformatted(
                    "No games detected yet. Launch one and Phynned "
                    "will pick it up automatically.");
            }
            ImGui::PopStyleColor();
        }

        ImGui::EndChild();
        ImGui::PopStyleVar();
        ImGui::PopStyleColor();
        ImGui::Spacing();
    }

    // ─────────────────────────────────────────────────────────────────────
    // Per-process breakdown (collapsible, non-invasive)
    //
    // Surfaces WHICH processes are in each TargetKind bucket so the user
    // can spot misclassification (e.g. steamwebhelper.exe showing up as
    // "Game") and report it. Read-only — no controls here. Manual
    // overrides go through the Targets tab.
    // ─────────────────────────────────────────────────────────────────────
    if (ac && ac->is_connected() && ac->targets().size() > 0u) {
        const auto targets = ac->targets();
        // Bucket-count pass; cheap (n <= 64).
        uint32_t n_games = 0u, n_bg = 0u, n_monitor = 0u;
        for (const auto& t : targets) {
            switch (t.kind) {
                case phynned::observer::TargetKind::Game:
                    ++n_games; break;
                case phynned::observer::TargetKind::Stream:
                case phynned::observer::TargetKind::Comm:
                case phynned::observer::TargetKind::Browser:
                case phynned::observer::TargetKind::Productivity:
                    ++n_bg; break;
                case phynned::observer::TargetKind::Unknown:
                    ++n_monitor; break;
                case phynned::observer::TargetKind::System:
                    break;
            }
        }

        char header[80];
        std::snprintf(header, sizeof(header),
            "Optimized processes (%u game%s, %u background, %u monitored)",
            n_games, n_games == 1u ? "" : "s", n_bg, n_monitor);

        if (ImGui::CollapsingHeader(header)) {
            ImGui::Indent();

            auto list_kind = [&](const char* label,
                                 phynned::observer::TargetKind k,
                                 ImVec4 color) noexcept
            {
                bool any = false;
                for (const auto& t : targets) {
                    if (t.kind != k) continue;
                    if (!any) {
                        ImGui::PushStyleColor(ImGuiCol_Text, color);
                        ImGui::TextUnformatted(label);
                        ImGui::PopStyleColor();
                        any = true;
                    }
                    ImGui::BulletText("%s  (PID %u)", t.name, t.pid);
                }
            };

            // Games first (highest visual priority — these are the ones
            // being pinned to V-Cache / P-cores, the user's primary
            // concern). Then background apps Phynned is delegating away.
            // Monitored last as the "everything else" footnote.
            list_kind("Games (pinned to V-Cache / P-cores)",
                      phynned::observer::TargetKind::Game,
                      ImVec4{0.55f, 0.95f, 0.65f, 1.f});
            list_kind("Streaming encoders",
                      phynned::observer::TargetKind::Stream,
                      ImVec4{0.85f, 0.65f, 0.95f, 1.f});
            list_kind("Communication apps",
                      phynned::observer::TargetKind::Comm,
                      ImVec4{0.85f, 0.85f, 0.55f, 1.f});
            list_kind("Browsers",
                      phynned::observer::TargetKind::Browser,
                      ImVec4{0.55f, 0.75f, 0.95f, 1.f});
            list_kind("Productivity / launcher helpers",
                      phynned::observer::TargetKind::Productivity,
                      ImVec4{0.75f, 0.75f, 0.75f, 1.f});
            list_kind("Monitored (no action)",
                      phynned::observer::TargetKind::Unknown,
                      ImVec4{0.55f, 0.55f, 0.55f, 1.f});

            ImGui::Spacing();
            ImGui::TextDisabled(
                "See something miscategorized? Open the Targets tab to "
                "override its kind manually.");

            ImGui::Unindent();
        }
        ImGui::Spacing();
    }

    // ─────────────────────────────────────────────────────────────────────
    // Key metrics row (privilege, watchdog, op mode)
    // ─────────────────────────────────────────────────────────────────────
    {
        const ImVec4 kGreen  {0.30f, 0.90f, 0.40f, 1.f};
        const ImVec4 kYellow {0.95f, 0.80f, 0.20f, 1.f};
        const ImVec4 kRed    {0.95f, 0.30f, 0.25f, 1.f};

        // Privilege
        static const char* kPrivNames[] = {
            "No privileges", "Partial", "Elevated", "Administrator"
        };
        static const ImVec4 kPrivColors[] = { kRed, kYellow, kYellow, kGreen };
        const uint8_t priv = s.snap.privilege_level < 4u ? s.snap.privilege_level : 0u;

        ImGui::PushStyleColor(ImGuiCol_Text, kPrivColors[priv]);
        ImGui::Text("Privileges: %s", kPrivNames[priv]);
        ImGui::PopStyleColor();
        ImGui::SameLine();
        help_marker(
            "Administrator privileges are needed for ETW (low-level event "
            "tracing) and cross-process affinity changes. Without admin, "
            "Phynned can still observe - but cannot apply policy changes to "
            "processes owned by other users.");
        if (priv < 3u) {
            ImGui::SameLine();
            ImGui::TextDisabled("- restart as admin for full features");
        }

        // Watchdog
        ImGui::PushStyleColor(ImGuiCol_Text,
            s.snap.watchdog_ok ? kGreen : kRed);
        ImGui::Text("Watchdog: %s",
            s.snap.watchdog_ok ? "healthy" : "STALLED - restart agent");
        ImGui::PopStyleColor();

        // Operation mode
        static const char* kModeNames[] = { "Auto", "Assist", "Manual" };
        const char* mode_str = (s.op_mode < 3u) ? kModeNames[s.op_mode] : "?";
        ImGui::Text("Mode: %s", mode_str);
        ImGui::SameLine();
        help_marker(
            "Auto: Phynned applies policies automatically when it detects a "
            "qualifying process.\n"
            "Assist: same detection, but Phynned asks before applying.\n"
            "Manual: detection only - you apply policies via the CLI.");
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // ─────────────────────────────────────────────────────────────────────
    // Active optimization - counts + pressure
    // ─────────────────────────────────────────────────────────────────────
    ImGui::TextUnformatted("Activity (current cycle)");
    ImGui::Indent();
    {
        ImGui::Text("Games detected:   %u", s.snap.target_count);
        ImGui::Text("Decisions made:   %u", s.snap.decision_count);
        ImGui::Text("Policies active:  %u", s.snap.action_count);

        if (s.snap.bad_count > 0u) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4{0.95f, 0.80f, 0.20f, 1.f});
            ImGui::Text("Reverted exes:    %u", s.snap.bad_count);
            ImGui::PopStyleColor();
            ImGui::SameLine();
            help_marker(
                "Processes where a policy caused a measurable performance "
                "regression. Phynned auto-reverts and bad-lists them so it "
                "won't try the same policy again on this game. "
                "Check memory.toml to clear the bad-list.");
        }

        // Pressure - only meaningful on asymmetric CPUs
        if (is_asymmetric(s.snap.cpu_class)) {
            const float pressure = s.snap.aggregate_pressure;
            const ImVec4 pres_color = (pressure >= 1.5f)
                ? ImVec4{0.95f,0.30f,0.25f,1.f}
                : (pressure >= 0.8f) ? ImVec4{0.95f,0.80f,0.20f,1.f}
                                     : ImVec4{0.30f,0.90f,0.40f,1.f};
            ImGui::PushStyleColor(ImGuiCol_Text, pres_color);
            ImGui::Text("Migration pressure: %.2f", pressure);
            ImGui::PopStyleColor();
            ImGui::SameLine();
            help_marker(
                "Weighted average rate at which game threads are migrating "
                "between cores. 0 = stable, 0.8+ = noticeable scheduler "
                "thrash, 1.5+ = severe (Phynned actively re-pins).");
            ImGui::SameLine();
            ImGui::TextDisabled("(%u migrations/s total)",
                                s.snap.total_migrations_obs);
        }

        // X3D-only telemetry
        if (has_vcache(s.snap.cpu_class)) {
            if (s.snap.ccd_defense_count > 0u) {
                ImGui::PushStyleColor(ImGuiCol_Text,
                                      ImVec4{0.30f, 0.65f, 0.95f, 1.f});
                ImGui::Text("V-Cache defense:  %u processes moved, %.1f%% CPU freed",
                            s.snap.ccd_defense_count,
                            static_cast<double>(s.snap.ccd_defense_cpu_pct));
                ImGui::PopStyleColor();
                ImGui::SameLine();
                help_marker(
                    "Rule 8 (CCD Load Defense) - when background processes "
                    "land on the V-Cache CCD and accumulate enough CPU load "
                    "to hurt your game, Phynned evicts them. Only fires on "
                    "AMD X3D dual-CCD hardware.");
            } else {
                ImGui::TextDisabled(
                    "V-Cache defense:  0 (no qualifying background load)");
            }
        }
    }
    ImGui::Unindent();

    // Top targets list
    if (s.snap.target_count > 0u) {
        ImGui::Spacing();
        ImGui::TextDisabled("Top targets (by activity):");
        ImGui::Indent();
        for (uint32_t i = 0u; i < 5u && s.snap.top_target_pids[i] != 0u; ++i) {
            ImGui::Text("  PID %-8u  %s",
                        s.snap.top_target_pids[i],
                        s.snap.top_target_names[i][0]
                            ? s.snap.top_target_names[i]
                            : "<unknown>");
        }
        ImGui::Unindent();
    }

    // ─────────────────────────────────────────────────────────────────────
    // Advanced details (collapsed by default)
    // ─────────────────────────────────────────────────────────────────────
    ImGui::Spacing();
    if (ImGui::CollapsingHeader("Advanced details")) {
        ImGui::Indent();

        // Subsystem status
        const ImVec4 kOk   {0.30f, 0.90f, 0.40f, 1.f};
        const ImVec4 kOff  {0.55f, 0.55f, 0.55f, 1.f};

        ImGui::PushStyleColor(ImGuiCol_Text, s.etw_active ? kOk : kOff);
        ImGui::Text("ETW session: %s",
                    s.etw_active ? "active" : "inactive");
        ImGui::PopStyleColor();
        ImGui::SameLine();
        help_marker(
            "Event Tracing for Windows - low-level kernel events used to "
            "detect thread migrations + context switches. Requires admin.");

        ImGui::PushStyleColor(ImGuiCol_Text,
            s.frame_observer_active ? kOk : kOff);
        ImGui::Text("Frame observer: %s",
                    s.frame_observer_active ? "active" : "inactive");
        ImGui::PopStyleColor();

        // Tick interval
        char tick_buf[32];
        if (s.tick_interval_ms >= 1000u)
            std::snprintf(tick_buf, sizeof(tick_buf),
                          "%u s", s.tick_interval_ms / 1000u);
        else
            std::snprintf(tick_buf, sizeof(tick_buf),
                          "%u ms", s.tick_interval_ms);
        ImGui::Text("Tick interval: %s", tick_buf);

        if (s.self_pin_core != 0xFFFFFFFFu)
            ImGui::Text("Self-pinned to: core %u", s.self_pin_core);

        if (s.snap.deep_idle) {
            ImGui::TextDisabled("Workload state: DeepIdle (>=5 min no targets)");
        }

        // CPU/RAM budget
        const float cpu = s.self_cpu_pct;
        const ImVec4 cpu_color = (cpu > 1.0f) ? ImVec4{0.95f,0.30f,0.25f,1.f}
                               : (cpu > 0.3f) ? ImVec4{0.95f,0.80f,0.20f,1.f}
                                              : kOk;
        ImGui::PushStyleColor(ImGuiCol_Text, cpu_color);
        ImGui::Text("Agent CPU: %.2f %%", cpu);
        ImGui::PopStyleColor();
        ImGui::SameLine();
        help_marker(
            "Phynned's own CPU usage. Budget: <0.3% when idle, <1.0% when "
            "actively classifying. If you see >1% sustained, file an "
            "issue - it shouldn't happen.");

        const float ram = s.self_rss_mb;
        const ImVec4 ram_color = (ram > 50.f) ? ImVec4{0.95f,0.30f,0.25f,1.f}
                               : (ram > 20.f) ? ImVec4{0.95f,0.80f,0.20f,1.f}
                                              : kOk;
        ImGui::PushStyleColor(ImGuiCol_Text, ram_color);
        ImGui::Text("Agent RAM: %.1f MB", ram);
        ImGui::PopStyleColor();
        ImGui::SameLine();
        help_marker("Budget: <20 MB idle, <50 MB active.");

        // Topology details
        ImGui::Spacing();
        ImGui::TextDisabled("Detected topology:");
        ImGui::BulletText("CCDs: %u", s.snap.ccd_count);
        if (s.snap.cpu_class == kCpuHybridIntel) {
            ImGui::BulletText("P-cores: %u", s.snap.p_core_count);
            ImGui::BulletText("E-cores: %u", s.snap.e_core_count);
        }
        ImGui::BulletText("Agent PID: %u", s.agent_pid);
        ImGui::BulletText("Agent build: %s",
                          s.agent_version[0] ? s.agent_version : "unknown");

        ImGui::Unindent();
    }

    // ─────────────────────────────────────────────────────────────────────
    // Error message (if any)
    // ─────────────────────────────────────────────────────────────────────
    if (s.last_error[0] != '\0') {
        ImGui::Separator();
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4{0.95f,0.30f,0.25f,1.f});
        ImGui::TextWrapped("Error: %s", s.last_error);
        ImGui::PopStyleColor();
    }
}
// Made with my soul - Swately <3
