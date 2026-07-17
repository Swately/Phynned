// tools/phynned-ui/widgets/targets_panel.hpp
// Targets panel — table of processes observed by Phynned.
//
// Reads targets[] and metrics[] DIRECTLY from PhynnedClient (not the Ring)
// to avoid copying the large arrays (~6KB) each frame.
//
// Columns: PID | Name | Kind | CPU% | Migrations/s | Frame P99 | Pressure | Affinity
#pragma once
#include "../PhynnedAppState.hpp"
#include <phynned/ipc/PhynnedClient.hpp>
#include <phynned/observer/TargetProcess.hpp>
#include <phynned/observer/TargetMetrics.hpp>
#include <phynned/observer/KindOverrides.hpp>
#include <imgui.h>
#include <cstdio>
#include <cstring>

namespace {

// TargetKind names matching the enum (§TargetProcess.hpp)
inline const char* target_kind_str(uint8_t k) noexcept {
    switch (k) {
        case 1: return "Game";
        case 2: return "Stream";
        case 3: return "Comm";
        case 4: return "Browser";
        case 5: return "Productivity";
        case 6: return "System";
        default: return "Unknown";
    }
}

// UI-side overrides store. Lazy-loaded on first draw, auto-saved on every
// change. The agent has its own KindOverrides instance and reloads from
// disk when the file's mtime changes — so any change made here is picked
// up on the agent's next tick.
struct OverridesUiState {
    phynned::observer::KindOverrides table{};
    bool                            loaded{false};
    int                             new_kind_idx{1};  // default to "Game"
    char                            new_exe[40]{};
};
inline OverridesUiState& overrides_ui() noexcept {
    static OverridesUiState st;
    return st;
}

// Pressure level color
inline ImVec4 pressure_color(uint8_t lvl) noexcept {
    switch (lvl) {
        case 2:  return ImVec4{0.95f, 0.30f, 0.25f, 1.f};  // red
        case 1:  return ImVec4{0.95f, 0.80f, 0.20f, 1.f};  // yellow
        default: return ImVec4{0.30f, 0.90f, 0.40f, 1.f};  // green
    }
}

} // anonymous namespace

inline void draw_targets_panel(const PhynnedAppState& s,
                                const phynned::ipc::PhynnedClient* ac) noexcept
{
    ImGui::Spacing();

    if (ac == nullptr || !s.snap.agent_connected) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4{0.95f, 0.75f, 0.20f, 1.f});
        ImGui::TextWrapped("Phynned agent is not running. "
                           "Launch phynned-agent.exe to enable target tracking.");
        ImGui::PopStyleColor();
        return;
    }

    const auto targets = ac->targets();
    const auto metrics = ac->metrics();

    if (targets.empty()) {
        ImGui::TextDisabled("No targets currently observed.");
        ImGui::Spacing();
        ImGui::TextDisabled(
            "Phynned watches for games, streaming apps and comms tools.\n"
            "Launch a game or OBS to see them here.");
        return;
    }

    ImGui::Text("%u target(s) observed:", static_cast<uint32_t>(targets.size()));
    ImGui::Separator();

    // ── Table ─────────────────────────────────────────────────────────────────
    constexpr ImGuiTableFlags kTableFlags =
        ImGuiTableFlags_Borders    | ImGuiTableFlags_RowBg  |
        ImGuiTableFlags_ScrollY    | ImGuiTableFlags_Resizable |
        ImGuiTableFlags_SizingStretchProp;

    const float row_height = ImGui::GetTextLineHeightWithSpacing();
    const float table_height = row_height * 12.f;

    if (ImGui::BeginTable("##targets", 8, kTableFlags,
                          ImVec2{0.f, table_height}))
    {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("PID",        ImGuiTableColumnFlags_WidthFixed, 70.f);
        ImGui::TableSetupColumn("Name",       ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Kind",       ImGuiTableColumnFlags_WidthFixed, 80.f);
        ImGui::TableSetupColumn("CPU%",       ImGuiTableColumnFlags_WidthFixed, 60.f);
        ImGui::TableSetupColumn("Mig/s",      ImGuiTableColumnFlags_WidthFixed, 60.f);
        ImGui::TableSetupColumn("P99 ms",     ImGuiTableColumnFlags_WidthFixed, 65.f);
        ImGui::TableSetupColumn("Pressure",   ImGuiTableColumnFlags_WidthFixed, 70.f);
        ImGui::TableSetupColumn("Affinity",   ImGuiTableColumnFlags_WidthFixed, 90.f);
        ImGui::TableHeadersRow();

        for (std::size_t i = 0u; i < targets.size(); ++i) {
            const auto& t = targets[i];
            ImGui::TableNextRow();

            // PID
            ImGui::TableSetColumnIndex(0);
            ImGui::Text("%u", t.pid);

            // Name
            ImGui::TableSetColumnIndex(1);
            ImGui::TextUnformatted(t.name[0] ? t.name : "<unknown>");

            // Kind — t.kind es phynned::observer::TargetKind (enum class uint8_t)
            ImGui::TableSetColumnIndex(2);
            ImGui::TextUnformatted(
                target_kind_str(static_cast<uint8_t>(t.kind)));

            if (i < metrics.size()) {
                const auto& m = metrics[i];

                // CPU%
                ImGui::TableSetColumnIndex(3);
                ImGui::Text("%.1f", m.cpu_usage_pct);

                // Migrations/s — color-coded
                ImGui::TableSetColumnIndex(4);
                {
                    const ImVec4 col = (m.migrations_per_sec > 50u)
                        ? ImVec4{0.95f,0.30f,0.25f,1.f}
                        : (m.migrations_per_sec > 10u)
                            ? ImVec4{0.95f,0.80f,0.20f,1.f}
                            : ImVec4{0.30f,0.90f,0.40f,1.f};
                    ImGui::PushStyleColor(ImGuiCol_Text, col);
                    ImGui::Text("%u", m.migrations_per_sec);
                    ImGui::PopStyleColor();
                }

                // Frame P99
                ImGui::TableSetColumnIndex(5);
                if (m.frame_time_p99_ms > 0.f)
                    ImGui::Text("%.1f", m.frame_time_p99_ms);
                else
                    ImGui::TextDisabled("--");

                // Pressure
                ImGui::TableSetColumnIndex(6);
                {
                    ImGui::PushStyleColor(ImGuiCol_Text, pressure_color(m.pressure_level));
                    static const char* kPressNames[] = {"Low", "Med", "High"};
                    ImGui::TextUnformatted(m.pressure_level < 3u
                        ? kPressNames[m.pressure_level] : "?");
                    ImGui::PopStyleColor();
                }

                // Affinity mask (hex)
                ImGui::TableSetColumnIndex(7);
                if (m.allowed_core_mask != 0u)
                    ImGui::Text("0x%08X", m.allowed_core_mask);
                else
                    ImGui::TextDisabled("default");
            } else {
                // No metrics yet for this target
                for (int c = 3; c <= 7; ++c) {
                    ImGui::TableSetColumnIndex(c);
                    ImGui::TextDisabled("--");
                }
            }
        }

        ImGui::EndTable();
    }

    // ── Window titles (optional detail) ──────────────────────────────────────
    if (!metrics.empty()) {
        ImGui::Spacing();
        ImGui::TextDisabled("Window titles:");
        ImGui::Indent();
        for (std::size_t i = 0u; i < metrics.size(); ++i) {
            if (metrics[i].window_title[0]) {
                ImGui::Text("PID %u: %s", metrics[i].pid, metrics[i].window_title);
            }
        }
        ImGui::Unindent();
    }

    // ── Manual TargetKind overrides ──────────────────────────────────────────
    //
    // Lets the user correct misclassifications. The override is keyed by
    // exe name (not PID) so it survives game launches and persists across
    // reboots. Writes go to %LOCALAPPDATA%\Phynned\overrides.txt — the agent
    // polls this file's mtime and reloads within ~100 ms.
    //
    // Two entry points:
    //   1. "Override" button on each row of the targets table above
    //      (one-click for a process Phynned already sees).
    //   2. The "Add override by exe name" form below (for processes the
    //      user knows by name even if Phynned hasn't observed one yet).
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    if (ImGui::CollapsingHeader("Manual classification overrides",
                                 ImGuiTreeNodeFlags_DefaultOpen)) {
        auto& ov = overrides_ui();
        if (!ov.loaded) {
            (void)ov.table.load();
            ov.loaded = true;
        }

        ImGui::TextWrapped(
            "Use these overrides to correct misclassifications. The agent "
            "consults the overrides table BEFORE its built-in heuristics, "
            "and reloads the file automatically when you save changes here.");
        ImGui::Spacing();

        // ── Quick-pick from currently observed targets ──────────────────
        ImGui::TextUnformatted("Quick override from observed targets:");
        for (std::size_t i = 0u; i < targets.size(); ++i) {
            const auto& t = targets[i];
            if (t.name[0] == '\0') continue;
            ImGui::PushID(static_cast<int>(i));
            ImGui::AlignTextToFramePadding();
            ImGui::Text("%-32s [%s]", t.name,
                        target_kind_str(static_cast<uint8_t>(t.kind)));
            ImGui::SameLine();
            static const char* kKindLabels[] = {
                "Unknown", "Game", "Stream", "Comm",
                "Browser", "Productivity", "System (never touch)"
            };
            int sel = static_cast<int>(t.kind);
            ImGui::SetNextItemWidth(140.f);
            if (ImGui::Combo("##qov_k", &sel, kKindLabels,
                              IM_ARRAYSIZE(kKindLabels))) {
                phynned::observer::TargetKind nk =
                    static_cast<phynned::observer::TargetKind>(sel);
                ov.table.set(t.name, nk);
                (void)ov.table.save();
            }
            ImGui::PopID();
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // ── Add by exe name (no observed instance required) ─────────────
        ImGui::TextUnformatted("Add override by exe name:");
        ImGui::SetNextItemWidth(220.f);
        ImGui::InputText("##ov_new_exe", ov.new_exe, sizeof(ov.new_exe));
        ImGui::SameLine();
        static const char* kAddKinds[] = {
            "Unknown", "Game", "Stream", "Comm",
            "Browser", "Productivity", "System (never touch)"
        };
        ImGui::SetNextItemWidth(140.f);
        ImGui::Combo("##ov_new_kind", &ov.new_kind_idx, kAddKinds,
                      IM_ARRAYSIZE(kAddKinds));
        ImGui::SameLine();
        const bool can_add = (ov.new_exe[0] != '\0');
        if (!can_add) ImGui::BeginDisabled();
        if (ImGui::Button("Add##ov_add")) {
            ov.table.set(ov.new_exe,
                static_cast<phynned::observer::TargetKind>(ov.new_kind_idx));
            (void)ov.table.save();
            ov.new_exe[0] = '\0';
        }
        if (!can_add) ImGui::EndDisabled();

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // ── Current overrides table ─────────────────────────────────────
        if (ov.table.count() == 0u) {
            ImGui::TextDisabled("(no overrides set)");
        } else {
            ImGui::Text("Active overrides (%u):", ov.table.count());
            if (ImGui::BeginTable("##ov_table", 3,
                    ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                    ImGuiTableFlags_SizingStretchProp))
            {
                ImGui::TableSetupColumn("Exe name", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableSetupColumn("Kind",     ImGuiTableColumnFlags_WidthFixed, 110.f);
                ImGui::TableSetupColumn("",         ImGuiTableColumnFlags_WidthFixed, 70.f);
                ImGui::TableHeadersRow();

                const auto* data = ov.table.data();
                // Snapshot count up-front; remove() shrinks the table.
                const uint32_t n = ov.table.count();
                for (uint32_t i = 0u; i < n; ++i) {
                    ImGui::PushID(static_cast<int>(i));
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::TextUnformatted(data[i].exe);
                    ImGui::TableSetColumnIndex(1);
                    ImGui::TextUnformatted(
                        phynned::observer::kind_to_name(data[i].kind));
                    ImGui::TableSetColumnIndex(2);
                    if (ImGui::SmallButton("Remove")) {
                        ov.table.remove(data[i].exe);
                        (void)ov.table.save();
                        ImGui::PopID();
                        break;  // table mutated; restart next frame
                    }
                    ImGui::PopID();
                }
                ImGui::EndTable();
            }
        }

        ImGui::Spacing();
        ImGui::TextDisabled(
            "Stored at: %%LOCALAPPDATA%%\\Phynned\\overrides.txt");
    }
}
// Made with my soul - Swately <3
