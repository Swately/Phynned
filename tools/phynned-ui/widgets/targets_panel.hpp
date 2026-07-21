// tools/phynned-ui/widgets/targets_panel.hpp
// Targets panel — table of processes observed by Phynned.
//
// Reads a SEQLOCK-CONSISTENT COPY of targets[]/metrics[] each frame via
// PhynnedClient::read_snapshot (~12KB memcpy at 60Hz — negligible). Never
// sorts/renders the live SHM spans: the 0.2.0 soak crash was root-caused to
// std::sort over live memory the agent rewrites mid-frame (unstable
// comparator = heap-corrupting UB).
//
// Columns: PID | Name | Kind | CPU% | Migrations/s | Frame P99 | Pressure |
//          Affinity | WS(MB) | Advice
//
// MR-1 (shadow router): the WS + Advice columns surface the read-only per-process
// CCD recommendation. Advice is a SHADOW recommendation only — nothing is placed
// by it (the green "Affinity" column is the only column reflecting a real placement,
// done by the separate AC-gated game path).
#pragma once
#include "../PhynnedAppState.hpp"
#include <phynned/ipc/PhynnedClient.hpp>
#include <phynned/observer/TargetProcess.hpp>
#include <phynned/observer/TargetMetrics.hpp>
#include <phynned/observer/KindOverrides.hpp>
#include <imgui.h>
#include <cstdio>
#include <cstring>
#include <span>
#include <algorithm>
#include <vector>

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

// Case-insensitive ASCII exe-name compare (matches the agent's rule matching).
inline bool ci_eq_name(const char* a, const char* b) noexcept {
    if (!a || !b) return false;
    while (*a != '\0' && *b != '\0') {
        char ca = (*a >= 'A' && *a <= 'Z') ? static_cast<char>(*a + 32) : *a;
        char cb = (*b >= 'A' && *b <= 'Z') ? static_cast<char>(*b + 32) : *b;
        if (ca != cb) return false;
        ++a; ++b;
    }
    return *a == *b;
}

// Pressure level color
inline ImVec4 pressure_color(uint8_t lvl) noexcept {
    switch (lvl) {
        case 2:  return ImVec4{0.95f, 0.30f, 0.25f, 1.f};  // red
        case 1:  return ImVec4{0.95f, 0.80f, 0.20f, 1.f};  // yellow
        default: return ImVec4{0.30f, 0.90f, 0.40f, 1.f};  // green
    }
}

// MR-1 shadow-router advice → short label + color.
// advice_ccd: 0 = none/LeaveAlone, 1 = VCache, 2 = Frequency,
//             3 = WouldCorral (MR-2: the corral WOULD move this to the Freq CCD).
inline const char* advice_ccd_str(uint8_t ccd) noexcept {
    switch (ccd) {
        case 1:  return "V-Cache";
        case 2:  return "Freq";
        case 3:  return "Corral!";  // MR-2 would-corral candidate
        default: return "--";
    }
}
inline ImVec4 advice_ccd_color(uint8_t ccd) noexcept {
    switch (ccd) {
        case 1:  return ImVec4{0.45f, 0.75f, 1.00f, 1.f};  // cyan-blue = cache-sensitive
        case 2:  return ImVec4{1.00f, 0.65f, 0.25f, 1.f};  // orange   = clock-bound
        case 3:  return ImVec4{0.95f, 0.45f, 0.30f, 1.f};  // red-orange = would-corral
        default: return ImVec4{0.55f, 0.55f, 0.55f, 1.f};  // dim      = no advice
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

    // CRASH FIX (2026-07-19, root-caused from the WER dump): NEVER sort/render
    // the live SHM spans — the agent rewrites them mid-frame and an unstable
    // std::sort comparator is heap-corrupting UB. Copy once per frame under
    // the publisher's seqlock and work exclusively on the copy.
    static phynned::ipc::PhynnedClient::UiSnapshot snap_buf;
    (void)ac->read_snapshot(snap_buf);
    const std::span<const phynned::observer::TargetProcess>
        targets{snap_buf.targets, snap_buf.n};
    const std::span<const phynned::observer::TargetMetrics>
        metrics{snap_buf.metrics, snap_buf.n};

    if (targets.empty()) {
        ImGui::TextDisabled("No processes observed yet.");
        ImGui::Spacing();
        ImGui::TextDisabled(
            "Phynned now observes every touchable process on the system and\n"
            "routes the ones that benefit onto the right CCD. If this stays\n"
            "empty, the agent may still be starting or lacks the rights to\n"
            "enumerate processes.");
        return;
    }

    // MASS-router: separate ROUTED (Phynned applied an affinity) from
    // OBSERVED-only (tracked but untouched — the mass herd). allowed_core_mask
    // != 0 ⇒ Phynned constrained this process's cores this cycle.
    uint32_t n_routed = 0u;
    for (const auto& m : metrics)
        if (m.allowed_core_mask != 0u) ++n_routed;

    const uint32_t n_shown   = static_cast<uint32_t>(targets.size());
    const uint32_t n_tracked = s.snap.total_tracked;  // full internal count
    if (n_tracked > n_shown)
        ImGui::Text("Observing %u processes  ·  showing top %u  ·  %u routed",
                    n_tracked, n_shown, n_routed);
    else
        ImGui::Text("Observing %u process(es)  ·  %u routed", n_shown, n_routed);
    ImGui::TextDisabled(
        "Routed = Phynned is actively placing it (green affinity below).  "
        "Observed = tracked only.  "
        "Advice = SHADOW recommendation (would route to V-Cache/Freq) — "
        "nothing is placed by it.");
    ImGui::TextDisabled(
        "CPU%% is PER-CORE: 100%% = one full core saturated. Task Manager "
        "divides by all 32 logical cores, so it shows ~1/32 of this number.  "
        "Click a column header to sort.");
    ImGui::Separator();

    // ── Table ─────────────────────────────────────────────────────────────────
    constexpr ImGuiTableFlags kTableFlags =
        ImGuiTableFlags_Borders    | ImGuiTableFlags_RowBg  |
        ImGuiTableFlags_ScrollY    | ImGuiTableFlags_Resizable |
        ImGuiTableFlags_SizingStretchProp |
        ImGuiTableFlags_Sortable   | ImGuiTableFlags_SortMulti;

    const float row_height = ImGui::GetTextLineHeightWithSpacing();
    const float table_height = row_height * 12.f;

    if (ImGui::BeginTable("##targets", 10, kTableFlags,
                          ImVec2{0.f, table_height}))
    {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("PID",        ImGuiTableColumnFlags_WidthFixed, 70.f);
        ImGui::TableSetupColumn("Name",       ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Kind",       ImGuiTableColumnFlags_WidthFixed, 80.f);
        ImGui::TableSetupColumn("CPU%",       ImGuiTableColumnFlags_WidthFixed |
            ImGuiTableColumnFlags_DefaultSort | ImGuiTableColumnFlags_PreferSortDescending, 60.f);
        ImGui::TableSetupColumn("Mig/s",      ImGuiTableColumnFlags_WidthFixed, 60.f);
        ImGui::TableSetupColumn("P99 ms",     ImGuiTableColumnFlags_WidthFixed, 65.f);
        ImGui::TableSetupColumn("Pressure",   ImGuiTableColumnFlags_WidthFixed, 70.f);
        ImGui::TableSetupColumn("Affinity",   ImGuiTableColumnFlags_WidthFixed, 90.f);
        ImGui::TableSetupColumn("WS(MB)",     ImGuiTableColumnFlags_WidthFixed, 65.f);
        ImGui::TableSetupColumn("Advice",     ImGuiTableColumnFlags_WidthFixed, 80.f);
        ImGui::TableHeadersRow();

        // ── Client-side sort (clicking a header now reorders the rows) ────────
        // targets[] and metrics[] are parallel; we sort an index array and
        // render through it. Re-sorted every frame (≤64 rows → negligible).
        const std::size_t n = targets.size();
        static std::vector<int> order;
        order.resize(n);
        for (std::size_t i = 0u; i < n; ++i) order[i] = static_cast<int>(i);

        if (ImGuiTableSortSpecs* ss = ImGui::TableGetSortSpecs()) {
            if (ss->SpecsCount > 0) {
                auto mval = [&](int idx, int col) -> double {
                    const bool has_m = (static_cast<std::size_t>(idx) < metrics.size());
                    const auto* m = has_m ? &metrics[idx] : nullptr;
                    switch (col) {
                        case 0: return static_cast<double>(targets[idx].pid);
                        case 2: return static_cast<double>(targets[idx].kind);
                        case 3: return m ? m->cpu_usage_pct : 0.0;
                        case 4: return m ? static_cast<double>(m->migrations_per_sec) : 0.0;
                        case 5: return m ? m->frame_time_p99_ms : 0.0;
                        case 6: return m ? static_cast<double>(m->pressure_level) : 0.0;
                        case 7: return m ? static_cast<double>(m->allowed_core_mask) : 0.0;
                        case 8: return m ? static_cast<double>(m->working_set_mb) : 0.0;
                        case 9: return m ? static_cast<double>(m->advice_ccd) : 0.0;
                        default: return 0.0;
                    }
                };
                std::sort(order.begin(), order.end(), [&](int a, int b) {
                    for (int s = 0; s < ss->SpecsCount; ++s) {
                        const ImGuiTableColumnSortSpecs& sp = ss->Specs[s];
                        const int col = sp.ColumnIndex;
                        int cmp = 0;
                        if (col == 1) {  // Name column — string compare
                            cmp = std::strcmp(targets[a].name, targets[b].name);
                        } else {
                            const double va = mval(a, col), vb = mval(b, col);
                            cmp = (va < vb) ? -1 : (va > vb) ? 1 : 0;
                        }
                        if (cmp != 0)
                            return (sp.SortDirection == ImGuiSortDirection_Ascending)
                                   ? (cmp < 0) : (cmp > 0);
                    }
                    return a < b;  // stable tiebreak
                });
            }
        }

        for (std::size_t k = 0u; k < n; ++k) {
            const std::size_t i = static_cast<std::size_t>(order[k]);
            const auto& t = targets[i];
            ImGui::TableNextRow();

            // PID
            ImGui::TableSetColumnIndex(0);
            ImGui::Text("%u", t.pid);

            // Name — right-click opens the W3 per-process user-rule menu.
            ImGui::TableSetColumnIndex(1);
            {
                char sel_id[80];
                std::snprintf(sel_id, sizeof(sel_id), "%s##trow%u",
                              t.name[0] ? t.name : "<unknown>",
                              static_cast<unsigned>(i));
                ImGui::Selectable(sel_id, false,
                                  ImGuiSelectableFlags_SpanAllColumns);
                char ctx_id[24];
                std::snprintf(ctx_id, sizeof(ctx_id), "##tctx%u",
                              static_cast<unsigned>(i));
                if (ImGui::BeginPopupContextItem(ctx_id)) {
                    auto* mac = const_cast<phynned::ipc::PhynnedClient*>(ac);
                    ImGui::Text("User rule for %s (pid %u):",
                                t.name[0] ? t.name : "<unknown>", t.pid);
                    ImGui::Separator();
                    if (ImGui::MenuItem("Never optimize")) {
                        (void)mac->send_command(
                            phynned::ipc::kPhynnedCmdSetProcessRule, t.pid, 0ull);
                    }
                    if (ImGui::MenuItem("Always -> Freq CCD")) {
                        (void)mac->send_command(
                            phynned::ipc::kPhynnedCmdSetProcessRule, t.pid, 1ull);
                    }
                    if (ImGui::MenuItem("Always -> V-Cache CCD")) {
                        (void)mac->send_command(
                            phynned::ipc::kPhynnedCmdSetProcessRule, t.pid, 2ull);
                    }
                    ImGui::Separator();
                    // Clear rule: find the published slot by exe name.
                    const auto urules = ac->user_rules();
                    int slot = -1;
                    for (uint32_t r = 0u;
                         r < static_cast<uint32_t>(urules.size()); ++r) {
                        if (ci_eq_name(urules[r].name, t.name)) {
                            slot = static_cast<int>(r); break;
                        }
                    }
                    if (slot < 0) ImGui::BeginDisabled();
                    if (ImGui::MenuItem("Clear rule")) {
                        (void)mac->send_command(
                            phynned::ipc::kPhynnedCmdRemoveProcessRule, 0u,
                            static_cast<uint64_t>(slot),
                            static_cast<uint64_t>(ac->user_rules_generation()));
                    }
                    if (slot < 0) ImGui::EndDisabled();
                    ImGui::EndPopup();
                }
            }

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

                // Affinity mask (hex) — green when Phynned is routing this proc;
                // "observed" (dim) when it is tracked but untouched.
                ImGui::TableSetColumnIndex(7);
                if (m.allowed_core_mask != 0u) {
                    ImGui::PushStyleColor(ImGuiCol_Text,
                                          ImVec4{0.30f, 0.90f, 0.40f, 1.f});
                    ImGui::Text("0x%08X", m.allowed_core_mask);
                    ImGui::PopStyleColor();
                } else {
                    ImGui::TextDisabled("observed");
                }

                // Working set (MB) — the RouteAdvisor's 32/96 MB proxy input.
                ImGui::TableSetColumnIndex(8);
                if (m.working_set_mb > 0u)
                    ImGui::Text("%u", m.working_set_mb);
                else
                    ImGui::TextDisabled("--");

                // Advice (MR-1 shadow router) — the recommended CCD, colored.
                // This is ADVICE ONLY: nothing is placed by it. Hover shows the
                // confidence + a reminder that it is a shadow recommendation.
                // W3: if the process has a user rule, its badge takes this cell
                // (a REAL user decision, distinct from the shadow advice).
                ImGui::TableSetColumnIndex(9);
                const phynned::ipc::UserRuleShm* urule = nullptr;
                {
                    const auto url = ac->user_rules();
                    for (const auto& u : url) {
                        if (ci_eq_name(u.name, t.name)) { urule = &u; break; }
                    }
                }
                if (urule != nullptr) {
                    const uint8_t ua = urule->action;
                    const ImVec4 ucol = (ua == 1u)
                        ? ImVec4{1.00f, 0.65f, 0.25f, 1.f}      // Freq (orange)
                        : (ua == 2u)
                            ? ImVec4{0.45f, 0.75f, 1.00f, 1.f}  // VCache (cyan)
                            : ImVec4{0.85f, 0.45f, 0.90f, 1.f}; // never (violet)
                    ImGui::PushStyleColor(ImGuiCol_Text, ucol);
                    ImGui::TextUnformatted(ua == 1u ? "user:Freq"
                                         : ua == 2u ? "user:VCache" : "never");
                    ImGui::PopStyleColor();
                    if (urule->flags & phynned::ipc::kUserRuleFlagBlockedAc) {
                        ImGui::SameLine();
                        ImGui::PushStyleColor(ImGuiCol_Text,
                                              ImVec4{0.95f, 0.30f, 0.25f, 1.f});
                        ImGui::TextUnformatted("AC");
                        ImGui::PopStyleColor();
                        if (ImGui::IsItemHovered())
                            ImGui::SetTooltip("Refused by the anti-cheat gate "
                                              "(R1) — no placement applied.");
                    }
                    if (urule->flags & phynned::ipc::kUserRuleFlagFlapWarn) {
                        ImGui::SameLine();
                        ImGui::PushStyleColor(ImGuiCol_Text,
                                              ImVec4{0.95f, 0.75f, 0.20f, 1.f});
                        ImGui::TextUnformatted("!pin");
                        ImGui::PopStyleColor();
                        if (ImGui::IsItemHovered())
                            ImGui::SetTooltip("Skipped — pinned by another "
                                              "affinity manager (R4 flap guard).");
                    }
                } else if (m.advice_ccd == 3u) {
                    // MR-2 would-corral marker. In DRY-RUN nothing is applied; the
                    // Policies tab shows whether the corral is DRY-RUN or LIVE.
                    ImGui::PushStyleColor(ImGuiCol_Text, advice_ccd_color(3u));
                    ImGui::TextUnformatted(advice_ccd_str(3u));
                    ImGui::PopStyleColor();
                    if (ImGui::IsItemHovered()) {
                        ImGui::SetTooltip(
                            "Would-corral: this active background process is on the\n"
                            "V-Cache CCD and is not cache-advised, so the corral WOULD\n"
                            "move it to the frequency CCD (mask 0xFFFF0000) to keep the\n"
                            "V-Cache clean. Applied only if the corral switch is LIVE\n"
                            "(Policies tab) — DRY-RUN by default = nothing placed.\n"
                            "Current affinity 0x%08X, working set %u MB.",
                            m.current_core_mask, m.working_set_mb);
                    }
                } else if (m.advice_ccd != 0u) {
                    ImGui::PushStyleColor(ImGuiCol_Text,
                                          advice_ccd_color(m.advice_ccd));
                    ImGui::TextUnformatted(advice_ccd_str(m.advice_ccd));
                    ImGui::PopStyleColor();
                    if (ImGui::IsItemHovered()) {
                        ImGui::SetTooltip(
                            "Shadow recommendation: route to %s (confidence %u%%).\n"
                            "Heuristic proxy from working set (%u MB) vs the 32/96 MB\n"
                            "L3 boundaries — nothing is placed. The A/B arbiter (MR-3)\n"
                            "is the ground truth.",
                            advice_ccd_str(m.advice_ccd),
                            m.advice_confidence, m.working_set_mb);
                    }
                } else {
                    ImGui::TextDisabled("--");
                }
            } else {
                // No metrics yet for this target
                for (int c = 3; c <= 9; ++c) {
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
