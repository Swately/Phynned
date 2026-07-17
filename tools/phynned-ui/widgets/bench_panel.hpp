// tools/phynned-ui/widgets/bench_panel.hpp
// Benchmark panel - A/B/A/B/A test runner.
//
// Integrates PresentMon capture, phynned-cli import, and agent pause/resume
// via IPC into a single workflow.
//
// User flow:
//   1. Pick target from active processes (or type PID manually)
//   2. Choose duration (default 30 s for quick tests; 90 s for serious)
//   3. Click "Single capture" or "A/B/A/B/A protocol"
//   4. Watch progress, see results in the runs table
//   5. After 5-run: tail the bench_multi log for verdict
#pragma once
#include "../PhynnedAppState.hpp"
#include "../BenchRunner.hpp"
#include "../ReportExporter.hpp"
#include "settings_panel.hpp"   // current_settings() for bench_output_dir
#include <phynned/ipc/PhynnedClient.hpp>
#include <phyriad/ui/text/Utf8.hpp>   // canonical UTF-8 / UTF-16 BOM decoder
#include <imgui.h>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#endif

namespace phynned::ui {

// Read a log file and return its contents as UTF-8, transparently handling
// UTF-16 LE/BE BOMs and UTF-8 BOM. Empty string on failure.
//
// This is a thin alias for phyriad::ui::text::read_file_utf8, kept here for
// the existing bench-panel call sites. The canonical implementation lives in
// <phyriad/ui/text/Utf8.hpp>; new code should call that directly.
inline std::string read_log_utf8(const char* path) noexcept {
    return phyriad::ui::text::read_file_utf8(path);
}



inline const char* bench_state_label(BenchRunner::State s) noexcept {
    switch (s) {
        case BenchRunner::State::Idle:             return "Idle";
        case BenchRunner::State::CapturingRun:     return "Capturing (PresentMon)";
        case BenchRunner::State::ImportingRun:     return "Importing CSV";
        case BenchRunner::State::BetweenRuns:      return "Cooldown / waiting agent";
        case BenchRunner::State::AggregatingStats: return "Computing bench multi";
        case BenchRunner::State::Done:             return "Done";
        case BenchRunner::State::Failed:           return "Failed";
    }
    return "?";
}

inline ImVec4 bench_state_color(BenchRunner::State s) noexcept {
    switch (s) {
        case BenchRunner::State::Idle:             return ImVec4{0.55f, 0.55f, 0.55f, 1.f};
        case BenchRunner::State::CapturingRun:     return ImVec4{0.95f, 0.80f, 0.20f, 1.f};
        case BenchRunner::State::ImportingRun:     return ImVec4{0.40f, 0.65f, 0.95f, 1.f};
        case BenchRunner::State::BetweenRuns:      return ImVec4{0.60f, 0.60f, 0.95f, 1.f};
        case BenchRunner::State::AggregatingStats: return ImVec4{0.40f, 0.65f, 0.95f, 1.f};
        case BenchRunner::State::Done:             return ImVec4{0.30f, 0.90f, 0.40f, 1.f};
        case BenchRunner::State::Failed:           return ImVec4{0.95f, 0.30f, 0.25f, 1.f};
    }
    return ImVec4{1, 1, 1, 1};
}

} // namespace phynned::ui

// Bench runner is shared between draw frames - file-static instance.
// One BenchRunner per phynned-ui process is sufficient (single-window app).
inline phynned::ui::BenchRunner& bench_runner_instance() noexcept {
    static phynned::ui::BenchRunner inst;
    return inst;
}

// Cached "located on disk" flags + paths - checked once at first draw.
struct BenchPanelPaths {
    char pm_exe[260];
    char cli_exe[260];
    char out_dir[260];
    bool pm_ok;
    bool cli_ok;
    bool checked;
};
inline BenchPanelPaths& bench_paths_instance() noexcept {
    static BenchPanelPaths p{};
    return p;
}

inline void bench_paths_resolve_(BenchPanelPaths& p) noexcept {
    if (p.checked) return;
    p.checked = true;

#ifdef _WIN32
    // Get directory of phynned-ui.exe.
    wchar_t wself[MAX_PATH]{};
    DWORD n = GetModuleFileNameW(nullptr, wself, MAX_PATH);
    if (n == 0u || n >= MAX_PATH) return;
    // Strip filename to get dir.
    for (int i = static_cast<int>(n) - 1; i >= 0; --i) {
        if (wself[i] == L'\\' || wself[i] == L'/') {
            wself[i + 1] = L'\0';
            break;
        }
    }
    char self_dir[260]{};
    const size_t wlen = std::wcslen(wself);
    for (size_t i = 0; i <= wlen && i < 259u; ++i)
        self_dir[i] = (wself[i] < 0x80) ? static_cast<char>(wself[i]) : '?';

    // PresentMon: search the same canonical locations the AgentLauncher
    // uses. With the mayo-2026 distribution layout, PresentMon lives in
    // `runtime/` next to phynned-cli + phynned-agent. Older flat builds
    // (where PresentMon was a sibling of phynned-ui.exe) still work via
    // candidate (2).
    {
        char pm_candidates[3][260]{};
        std::snprintf(pm_candidates[0], sizeof(pm_candidates[0]),
                      "%sruntime\\PresentMon.exe", self_dir);  // (1) new
        std::snprintf(pm_candidates[1], sizeof(pm_candidates[1]),
                      "%sPresentMon.exe", self_dir);            // (2) flat
        std::snprintf(pm_candidates[2], sizeof(pm_candidates[2]),
                      "%s..\\bin\\PresentMon.exe", self_dir);   // (3) future
        p.pm_ok = false;
        p.pm_exe[0] = '\0';
        for (int ci = 0; ci < 3; ++ci) {
            const DWORD a = GetFileAttributesA(pm_candidates[ci]);
            if (a != INVALID_FILE_ATTRIBUTES &&
                !(a & FILE_ATTRIBUTE_DIRECTORY))
            {
                std::strncpy(p.pm_exe, pm_candidates[ci], sizeof(p.pm_exe) - 1u);
                p.pm_exe[sizeof(p.pm_exe) - 1u] = '\0';
                p.pm_ok = true;
                break;
            }
        }
        if (!p.pm_ok) {
            std::strncpy(p.pm_exe, pm_candidates[0], sizeof(p.pm_exe) - 1u);
            p.pm_exe[sizeof(p.pm_exe) - 1u] = '\0';
        }
    }

    // phynned-cli: search the same canonical locations as PresentMon
    // (runtime/ first, then sibling, then legacy split build tree).
    {
        char cli_candidates[4][260]{};
        std::snprintf(cli_candidates[0], sizeof(cli_candidates[0]),
                      "%sruntime\\phynned-cli.exe", self_dir);          // (1) new
        std::snprintf(cli_candidates[1], sizeof(cli_candidates[1]),
                      "%sphynned-cli.exe", self_dir);                    // (2) flat
        std::snprintf(cli_candidates[2], sizeof(cli_candidates[2]),
                      "%s..\\phynned-cli\\phynned-cli.exe", self_dir);     // (3) dev tree
        std::snprintf(cli_candidates[3], sizeof(cli_candidates[3]),
                      "%s..\\bin\\phynned-cli.exe", self_dir);           // (4) future
        p.cli_ok = false;
        p.cli_exe[0] = '\0';
        for (int ci = 0; ci < 4; ++ci) {
            const DWORD a = GetFileAttributesA(cli_candidates[ci]);
            if (a != INVALID_FILE_ATTRIBUTES &&
                !(a & FILE_ATTRIBUTE_DIRECTORY))
            {
                std::strncpy(p.cli_exe, cli_candidates[ci], sizeof(p.cli_exe) - 1u);
                p.cli_exe[sizeof(p.cli_exe) - 1u] = '\0';
                p.cli_ok = true;
                break;
            }
        }
        if (!p.cli_ok) {
            std::strncpy(p.cli_exe, cli_candidates[0], sizeof(p.cli_exe) - 1u);
            p.cli_exe[sizeof(p.cli_exe) - 1u] = '\0';
        }
    }

    // Output dir: user-configured (Settings tab) wins. Falls back to
    // %TEMP%\phynned-bench. Either way, the directory is created if missing.
    const auto& settings = phynned_ui::current_settings();
    if (settings.bench_output_dir[0] != '\0') {
        std::strncpy(p.out_dir, settings.bench_output_dir,
                     sizeof(p.out_dir) - 1u);
        p.out_dir[sizeof(p.out_dir) - 1u] = '\0';
        CreateDirectoryA(p.out_dir, nullptr);
    } else {
        char tmp[260]{};
        const DWORD tn = GetTempPathA(sizeof(tmp), tmp);
        if (tn > 0u && tn < sizeof(tmp)) {
            std::snprintf(p.out_dir, sizeof(p.out_dir),
                          "%sphynned-bench", tmp);
            CreateDirectoryA(p.out_dir, nullptr);
        } else {
            std::strncpy(p.out_dir, "C:\\Temp\\phynned-bench",
                         sizeof(p.out_dir) - 1u);
            CreateDirectoryA(p.out_dir, nullptr);
        }
    }
#endif
}

inline void draw_bench_panel(const PhynnedAppState& s,
                              const phynned::ipc::PhynnedClient* ac) noexcept
{
    ImGui::Spacing();

    auto& runner = bench_runner_instance();
    auto& paths  = bench_paths_instance();
    bench_paths_resolve_(paths);

    // Configure runner once paths are known + client available.
    // (Client comes in as const* - we need writable for send_command. The UI
    // owns the underlying PhynnedClient via PhynnedLogicNode; the const here is
    // for the read-only views. Cast is safe in single-window UI context.)
    auto* writable_client = const_cast<phynned::ipc::PhynnedClient*>(ac);
    runner.set_presentmon_path(paths.pm_exe);
    runner.set_phynned_cli_path(paths.cli_exe);
    runner.set_output_dir(paths.out_dir);
    runner.set_client(writable_client);

    runner.tick();  // advance state machine each frame

    // ── Agent gate ─────────────────────────────────────────────────────────
    if (ac == nullptr || !s.snap.agent_connected) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4{0.95f, 0.75f, 0.20f, 1.f});
        ImGui::TextWrapped("Phynned agent is not running. "
                           "Single capture works without the agent, but the "
                           "A/B/A/B/A protocol requires it for pause/resume.");
        ImGui::PopStyleColor();
        ImGui::Spacing();
    }

    // ── Tool availability ──────────────────────────────────────────────────
    if (!paths.pm_ok || !paths.cli_ok) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4{0.95f, 0.40f, 0.30f, 1.f});
        if (!paths.pm_ok) {
            ImGui::Text("PresentMon.exe NOT FOUND at: %s", paths.pm_exe);
            ImGui::TextWrapped("Re-run CMake configure with internet access "
                               "to auto-download, or place PresentMon.exe "
                               "next to phynned-ui.exe manually.");
        }
        if (!paths.cli_ok) {
            ImGui::Text("phynned-cli.exe NOT FOUND at: %s", paths.cli_exe);
        }
        ImGui::PopStyleColor();
        ImGui::Separator();
    }

    // ── Methodology note ──────────────────────────────────────────────────
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4{0.55f, 0.55f, 0.55f, 1.f});
    ImGui::TextWrapped(
        "A/B/A/B/A: 5 sequential captures. Runs 1/3/5 = baseline (agent "
        "policies paused). Runs 2/4 = treated (agent active). Stats: "
        "phynned-cli bench multi computes 95%% CIs and significance.");
    ImGui::PopStyleColor();
    ImGui::Separator();

    // ── State indicator ────────────────────────────────────────────────────
    const auto rstate = runner.state();
    ImGui::PushStyleColor(ImGuiCol_Text, phynned::ui::bench_state_color(rstate));
    ImGui::Text("State: %s", phynned::ui::bench_state_label(rstate));
    ImGui::PopStyleColor();
    if (rstate != phynned::ui::BenchRunner::State::Idle) {
        ImGui::SameLine();
        ImGui::TextDisabled("(run %u of %u)",
                            runner.run_index() + 1u, runner.n_runs());
    }

    // ── Configuration block (only when idle) ───────────────────────────────
    static char     s_target_name[64] = "";
    static int      s_phase_duration  = 30;  // seconds

    if (rstate == phynned::ui::BenchRunner::State::Idle ||
        rstate == phynned::ui::BenchRunner::State::Done ||
        rstate == phynned::ui::BenchRunner::State::Failed)
    {
        ImGui::Spacing();
        ImGui::Text("Process name:");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(200.f);
        ImGui::InputText("##bench_name", s_target_name, sizeof(s_target_name));

        ImGui::Text("Run duration:");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(120.f);
        ImGui::SliderInt("seconds##bench_dur", &s_phase_duration, 10, 300);

        ImGui::Spacing();

        // Quick-pick from current targets
        if (ac) {
            const auto targets = ac->targets();
            if (!targets.empty()) {
                ImGui::TextDisabled("Quick-pick from active targets:");
                ImGui::Indent();
                for (const auto& t : targets) {
                    char btn_label[96];
                    std::snprintf(btn_label, sizeof(btn_label),
                                  "%s (PID %u)##qp%u", t.name, t.pid, t.pid);
                    if (ImGui::SmallButton(btn_label)) {
                        std::strncpy(s_target_name, t.name,
                                     sizeof(s_target_name) - 1u);
                    }
                }
                ImGui::Unindent();
            }
        }

        ImGui::Separator();

        // ── Differential pin mode toggle ──────────
        // When enabled, the agent's PolicyEngine emits Rule 7
        // (PinHotThreadDifferential) for Game targets with stable hot_tid,
        // INSTEAD of Rule 1 (process-wide V-Cache pin). Differential pin:
        //   - Process affinity → FULL system mask (workers free to use any core)
        //   - Hot thread affinity → V-Cache CCD mask (only hot thread pinned)
        // Hypothesis: well-threaded engines (UE5, RAGE) benefit because
        // workers gain access to all 16 cores instead of being constrained
        // to the V-Cache CCD.
        //
        // Toggle is sent to agent via IPC command (kPhynnedCmdSetDifferentialPin).
        // Agent reverts any active actions on toggle to avoid mixed state.
        {
            static bool s_diff_pin_enabled = false;
            const bool can_toggle = (ac != nullptr) && s.snap.agent_connected
                                    && writable_client
                                    && writable_client->can_send_commands();
            if (!can_toggle) ImGui::BeginDisabled();
            if (ImGui::Checkbox("Differential pin mode (experimental - Rule 7)",
                                 &s_diff_pin_enabled))
            {
                if (writable_client) {
                    (void)writable_client->send_command(
                        phynned::ipc::kPhynnedCmdSetDifferentialPin,
                        /*target_pid=*/0u,
                        /*arg1=*/s_diff_pin_enabled ? 1ull : 0ull);
                }
            }
            if (!can_toggle) ImGui::EndDisabled();

            // Inline hint
            if (s_diff_pin_enabled) {
                ImGui::SameLine();
                ImGui::PushStyleColor(ImGuiCol_Text,
                                       ImVec4{0.30f, 0.65f, 0.95f, 1.f});
                ImGui::TextUnformatted("ON - Rule 7 active");
                ImGui::PopStyleColor();
            }
            ImGui::TextDisabled(
                "Pins the hot thread to V-Cache CCD while releasing workers "
                "to all cores. Test on Halo 2 first (known SIGNIFICANT case) "
                "to verify no regression; then on NULL games (RDR2, Minecraft "
                "modded) to test if the new mode unlocks them.");
        }

        ImGui::Separator();

        // Start buttons
        const bool tools_ok = paths.pm_ok && paths.cli_ok;
        const bool target_ok = (s_target_name[0] != '\0');
        const bool can_single = tools_ok && target_ok;
        const bool can_ababa  = can_single && ac && s.snap.agent_connected;

        if (!can_single) ImGui::BeginDisabled();
        if (ImGui::Button("Single capture")) {
            (void)runner.start_single(s_target_name,
                                       static_cast<uint32_t>(s_phase_duration));
        }
        if (!can_single) ImGui::EndDisabled();

        ImGui::SameLine();

        if (!can_ababa) ImGui::BeginDisabled();
        ImGui::PushStyleColor(ImGuiCol_Button,
                               ImVec4{0.15f, 0.50f, 0.15f, 1.f});
        if (ImGui::Button("Run A/B/A/B/A protocol (5 runs)")) {
            (void)runner.start_ababa(s_target_name,
                                      static_cast<uint32_t>(s_phase_duration));
        }
        ImGui::PopStyleColor();
        if (!can_ababa) ImGui::EndDisabled();

        if (!target_ok) {
            ImGui::SameLine();
            ImGui::TextDisabled("(enter or pick a process name first)");
        }
    } else {
        // Active run - show progress + cancel
        if (rstate == phynned::ui::BenchRunner::State::CapturingRun) {
            char buf[64];
            std::snprintf(buf, sizeof(buf), "%.0f / %u s",
                          runner.run_progress() *
                              static_cast<float>(runner.run_duration()),
                          runner.run_duration());
            ImGui::ProgressBar(runner.run_progress(),
                                ImVec2{-1.f, 0.f}, buf);
        } else {
            ImGui::ProgressBar(-1.f * static_cast<float>(ImGui::GetTime()),
                                ImVec2{-1.f, 0.f}, "Working...");
        }

        ImGui::Spacing();
        if (ImGui::Button("Cancel")) {
            runner.cancel();
        }
    }

    // ── Runs table ─────────────────────────────────────────────────────────
    ImGui::Separator();
    ImGui::Text("Captured runs:");
    if (ImGui::BeginTable("##runs", 4,
            ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg))
    {
        ImGui::TableSetupColumn("#",        ImGuiTableColumnFlags_WidthFixed, 30.f);
        ImGui::TableSetupColumn("Type",     ImGuiTableColumnFlags_WidthFixed, 80.f);
        ImGui::TableSetupColumn("Status",   ImGuiTableColumnFlags_WidthFixed, 90.f);
        ImGui::TableSetupColumn("Output CSV", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();

        const auto* runs = runner.runs();
        for (uint32_t i = 0u; i < runner.n_runs(); ++i) {
            const auto& r = runs[i];
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::Text("%u", i + 1u);
            ImGui::TableSetColumnIndex(1);
            ImGui::TextColored(
                r.is_baseline ? ImVec4{0.95f, 0.80f, 0.20f, 1.f}
                              : ImVec4{0.40f, 0.65f, 0.95f, 1.f},
                "%s", r.is_baseline ? "baseline" : "treated");
            ImGui::TableSetColumnIndex(2);
            if (r.completed && r.import_exit_code == 0) {
                ImGui::TextColored(ImVec4{0.30f, 0.90f, 0.40f, 1.f}, "OK");
            } else if (r.last_error[0]) {
                ImGui::TextColored(ImVec4{0.95f, 0.30f, 0.25f, 1.f},
                                   "ERR");
            } else if (i == runner.run_index() &&
                       rstate != phynned::ui::BenchRunner::State::Idle &&
                       rstate != phynned::ui::BenchRunner::State::Done) {
                ImGui::TextColored(ImVec4{0.95f, 0.80f, 0.20f, 1.f}, "...");
            } else {
                ImGui::TextDisabled("pending");
            }
            ImGui::TableSetColumnIndex(3);
            ImGui::TextDisabled("%s", r.bench_csv_path);
        }
        ImGui::EndTable();
    }

    // ── Aggregate output (after Done) ──────────────────────────────────────
    if (rstate == phynned::ui::BenchRunner::State::Done) {
        const char* log_path = runner.aggregate_log_path();
        if (log_path && log_path[0]) {
            ImGui::Separator();
            ImGui::Text("bench multi output:");
            std::string contents = phynned::ui::read_log_utf8(log_path);
            if (!contents.empty()) {
                ImGui::InputTextMultiline(
                    "##bench_multi_log",
                    contents.data(), contents.size() + 1u,
                    ImVec2{-1.f, 240.f},
                    ImGuiInputTextFlags_ReadOnly);
                ImGui::TextDisabled("Log: %s", log_path);
            } else {
                ImGui::TextDisabled("(log not readable)");
            }
        }

        // ── Report exporter ──────────────────────
        // Auto-suggest the report path (Option 3 from refinement plan:
        // pre-populated + user-editable), generate the .md on click.
        ImGui::Separator();
        ImGui::Text("Export report:");

        // Path text input is sticky across draws - initialized once per
        // Done state. If the user clicks "Suggest path" it gets reset.
        static char s_export_path[260]{};
        static uint32_t s_last_suggested_for_run_count = 0u;
        static char s_export_status[256]{};
        static ImVec4 s_export_status_color{0.55f, 0.55f, 0.55f, 1.f};

        // Auto-fill the suggested path the first time we enter Done state
        // for this runner instance (n_runs is a stable signature).
        if (s_export_path[0] == '\0' ||
            runner.n_runs() != s_last_suggested_for_run_count)
        {
            phynned::ui::suggest_report_path(runner.process_name(),
                                           s_export_path,
                                           sizeof(s_export_path));
            s_last_suggested_for_run_count = runner.n_runs();
        }

        ImGui::SetNextItemWidth(-160.f);  // leave room for buttons
        ImGui::InputText("##export_path", s_export_path, sizeof(s_export_path));
        ImGui::SameLine();
        if (ImGui::SmallButton("Suggest")) {
            phynned::ui::suggest_report_path(runner.process_name(),
                                           s_export_path,
                                           sizeof(s_export_path));
            s_export_status[0] = '\0';
        }
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4{0.15f, 0.50f, 0.15f, 1.f});
        if (ImGui::Button("Export .md")) {
            const bool ok = phynned::ui::generate_md_report(
                runner.runs(),
                runner.n_runs(),
                runner.process_name(),
                runner.protocol(),
                runner.aggregate_log_path(),
                s_export_path);
            if (ok) {
                std::snprintf(s_export_status, sizeof(s_export_status),
                              "Report written: %s", s_export_path);
                s_export_status_color = ImVec4{0.30f, 0.90f, 0.40f, 1.f};
            } else {
                std::snprintf(s_export_status, sizeof(s_export_status),
                              "Failed to write: %s "
                              "(check path permissions / directory exists)",
                              s_export_path);
                s_export_status_color = ImVec4{0.95f, 0.30f, 0.25f, 1.f};
            }
        }
        ImGui::PopStyleColor();

        if (s_export_status[0] != '\0') {
            ImGui::PushStyleColor(ImGuiCol_Text, s_export_status_color);
            ImGui::TextWrapped("%s", s_export_status);
            ImGui::PopStyleColor();
        }
        ImGui::TextDisabled(
            "Template fills objective data (per-run numbers, hardware, "
            "aggregate log). Summary/Interpretation/Conclusion are blanks "
            "for you to write afterwards.");
    }

    // ── Error display ──────────────────────────────────────────────────────
    if (rstate == phynned::ui::BenchRunner::State::Failed) {
        ImGui::Separator();
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4{0.95f, 0.30f, 0.25f, 1.f});
        ImGui::TextWrapped("Error: %s", runner.last_error());
        ImGui::PopStyleColor();

        // Surface the failed run's last_error + PresentMon log path so the
        // user can read PresentMon's stderr directly (it's the most useful
        // diagnostic for "no frames captured", "not admin", etc.).
        const auto* runs = runner.runs();
        for (uint32_t i = 0u; i < runner.n_runs(); ++i) {
            if (runs[i].last_error[0]) {
                ImGui::TextDisabled("Run %u failed: %s",
                                    i + 1u, runs[i].last_error);
                // Best-effort: read the .log file alongside the PresentMon
                // CSV. PresentMonSpawner replaces .csv with .log; but
                // runs[i].pm_csv_path is ".pm.csv", so we replace the LAST
                // .csv with .log.
                char log_path[260];
                std::snprintf(log_path, sizeof(log_path), "%s",
                              runs[i].pm_csv_path);
                char* dot = std::strrchr(log_path, '.');
                if (dot && std::strcmp(dot, ".csv") == 0) {
                    std::strcpy(dot, ".log");
                }
                std::string contents = phynned::ui::read_log_utf8(log_path);
                if (!contents.empty()) {
                    ImGui::TextDisabled("PresentMon log:");
                    ImGui::InputTextMultiline(
                        "##pm_log",
                        contents.data(), contents.size() + 1u,
                        ImVec2{-1.f, 160.f},
                        ImGuiInputTextFlags_ReadOnly);
                    ImGui::TextDisabled("(%s)", log_path);
                }
            }
        }
    }

    ImGui::Separator();
    ImGui::Spacing();
    ImGui::TextDisabled("Output dir: %s", paths.out_dir);
}
// Made with my soul - Swately <3
