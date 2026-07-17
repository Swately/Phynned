// tools/phynned-ui/widgets/actions_panel.hpp
// Actions panel — history of applied and reverted actions.
//
// Reads action_log[] directly from PhynnedClient (SPMC ring buffer).
// Shows: timestamp, PID, rule, action, success/fail, state (active/reverted).
// "Revert All" button sends an IPC command.
#pragma once
#include "../PhynnedAppState.hpp"
#include <phynned/ipc/PhynnedClient.hpp>
#include <phynned/action/ActionLog.hpp>
#include <phyriad/hal/Timestamp.hpp>
#include <imgui.h>
#include <cstdio>
#include <cstring>

// ── Format a TSC delta as a human-readable age string ────────────────────
//   "now"      — < 1s
//   "Xs ago"   — < 60s
//   "X min ago"— < 60min
//   "Xh ago"   — < 24h
//   "Xd ago"   — anything older
// Uses the cached TSC frequency to convert. Falls back to "?" if freq is
// zero or the entry timestamp is in the future (clock skew / agent restart).
inline void format_tsc_age(uint64_t entry_tsc, uint64_t now_tsc,
                            uint64_t tsc_freq,
                            char* buf, size_t bufsize) noexcept
{
    if (tsc_freq == 0u || entry_tsc == 0u || entry_tsc > now_tsc) {
        std::snprintf(buf, bufsize, "?");
        return;
    }
    const uint64_t delta = now_tsc - entry_tsc;
    const uint64_t sec   = delta / tsc_freq;
    if (sec < 1u)        std::snprintf(buf, bufsize, "now");
    else if (sec < 60u)  std::snprintf(buf, bufsize, "%us ago",
                            static_cast<uint32_t>(sec));
    else if (sec < 3600u) std::snprintf(buf, bufsize, "%u min ago",
                            static_cast<uint32_t>(sec / 60u));
    else if (sec < 86400u) std::snprintf(buf, bufsize, "%uh ago",
                            static_cast<uint32_t>(sec / 3600u));
    else                 std::snprintf(buf, bufsize, "%ud ago",
                            static_cast<uint32_t>(sec / 86400u));
}

// UI-side filter cursor — entries with tsc_applied <= this are hidden.
// Clear button sets this to the current agent TSC. Persistent across
// frames; reset on UI restart (this is purely a view filter — the
// underlying SHM ring is not modified).
inline uint64_t& actions_clear_cursor() noexcept {
    static uint64_t cursor = 0u;
    return cursor;
}

inline void draw_actions_panel(const PhynnedAppState& s,
                                const phynned::ipc::PhynnedClient* ac) noexcept
{
    ImGui::Spacing();

    if (ac == nullptr || !s.snap.agent_connected) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4{0.95f, 0.75f, 0.20f, 1.f});
        ImGui::TextWrapped("Phynned agent is not running. "
                           "Launch phynned-agent.exe to see action history.");
        ImGui::PopStyleColor();
        return;
    }

    const auto log = ac->action_log();
    const uint64_t now_tsc   = s.snap.last_sync_tsc;
    const uint64_t tsc_freq  = phyriad::hal::calibrate_tsc_freq();

    // ── Header line ──────────────────────────────────────────────────────────
    ImGui::Text("Action log");
    ImGui::SameLine();
    ImGui::TextDisabled("(Total applied: %u)", s.snap.action_count);

    // Clear button — UI-side view filter, doesn't touch the agent's ring.
    // Useful to wipe the table after testing without restarting the agent.
    ImGui::SameLine();
    if (ImGui::SmallButton("Clear log")) {
        actions_clear_cursor() = now_tsc;
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(
            "Hide all current log entries. New actions logged after this\n"
            "click will still appear. The agent's audit-log on disk is\n"
            "unaffected — this is a view filter only.");
    }

    ImGui::Spacing();
    ImGui::TextDisabled(
        "Tip: press Ctrl+Alt+R (global hotkey) or run "
        "'phynned-cli revert --all' to undo every active policy.");

    ImGui::Separator();

    // ── Action log table ──────────────────────────────────────────────────────
    if (log.empty()) {
        ImGui::TextDisabled("No actions have been applied yet.");
        ImGui::Spacing();
        ImGui::TextDisabled(
            "Actions appear here when Phynned pins a process "
            "or changes its priority class.");
        return;
    }

    ImGui::Text("Action log (%zu entries):", log.size());

    constexpr ImGuiTableFlags kFlags =
        ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
        ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp;
    const float row_h = ImGui::GetTextLineHeightWithSpacing();

    if (ImGui::BeginTable("##actlog", 7, kFlags, ImVec2{0.f, row_h * 14.f}))
    {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("PID",        ImGuiTableColumnFlags_WidthFixed, 70.f);
        ImGui::TableSetupColumn("Rule",       ImGuiTableColumnFlags_WidthFixed, 55.f);
        ImGui::TableSetupColumn("Prev mask",  ImGuiTableColumnFlags_WidthFixed, 90.f);
        ImGui::TableSetupColumn("New mask",   ImGuiTableColumnFlags_WidthFixed, 90.f);
        ImGui::TableSetupColumn("OK",         ImGuiTableColumnFlags_WidthFixed, 30.f);
        ImGui::TableSetupColumn("State",      ImGuiTableColumnFlags_WidthFixed, 70.f);
        ImGui::TableSetupColumn("Age",        ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();

        // Display in reverse order (most recent first).
        // The log is a circular ring; we display the visible window.
        const uint64_t clear_cursor = actions_clear_cursor();
        const std::size_t n = log.size();
        for (std::size_t rev = 0u; rev < n; ++rev) {
            const std::size_t idx = n - 1u - rev;
            const auto& e = log[idx];
            if (e.target_pid == 0u) continue;  // empty slot
            // UI-side filter: hide entries logged before the user clicked
            // "Clear log".
            if (e.tsc_applied != 0u && e.tsc_applied <= clear_cursor) continue;

            ImGui::TableNextRow();

            ImGui::TableSetColumnIndex(0);
            ImGui::Text("%u", e.target_pid);

            ImGui::TableSetColumnIndex(1);
            ImGui::Text("#%u", e.rule_id);

            ImGui::TableSetColumnIndex(2);
            if (e.prev_affinity_mask)
                ImGui::Text("0x%08llX",
                    static_cast<unsigned long long>(e.prev_affinity_mask));
            else
                ImGui::TextDisabled("--");

            ImGui::TableSetColumnIndex(3);
            if (e.new_affinity_mask)
                ImGui::Text("0x%08llX",
                    static_cast<unsigned long long>(e.new_affinity_mask));
            else
                ImGui::TextDisabled("--");

            ImGui::TableSetColumnIndex(4);
            if (e.success) {
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4{0.3f,0.9f,0.4f,1.f});
                ImGui::TextUnformatted("[v]");
                ImGui::PopStyleColor();
            } else {
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4{0.9f,0.3f,0.3f,1.f});
                ImGui::TextUnformatted("[x]");
                ImGui::PopStyleColor();
            }

            ImGui::TableSetColumnIndex(5);
            if (e.tsc_reverted != 0u) {
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4{0.55f,0.55f,0.55f,1.f});
                ImGui::TextUnformatted("reverted");
                ImGui::PopStyleColor();
            } else {
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4{0.3f,0.9f,0.4f,1.f});
                ImGui::TextUnformatted("active");
                ImGui::PopStyleColor();
            }

            ImGui::TableSetColumnIndex(6);
            // Human-readable age relative to the agent's last published TSC.
            char age_buf[24]{};
            format_tsc_age(e.tsc_applied, now_tsc, tsc_freq,
                           age_buf, sizeof(age_buf));
            // For reverted entries, also show how long ago THAT happened.
            if (e.tsc_reverted != 0u) {
                char rev_age_buf[24]{};
                format_tsc_age(e.tsc_reverted, now_tsc, tsc_freq,
                               rev_age_buf, sizeof(rev_age_buf));
                ImGui::Text("applied %s / reverted %s",
                            age_buf, rev_age_buf);
            } else {
                ImGui::TextUnformatted(age_buf);
            }
        }

        ImGui::EndTable();
    }

    ImGui::Spacing();
    ImGui::TextDisabled(
        "Ages are computed against the agent's last published TSC "
        "(updated each tick, ~100 ms).");
}
// Made with my soul - Swately <3
