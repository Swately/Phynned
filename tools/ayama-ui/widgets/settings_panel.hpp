// apps/ayama/tools/ayama-ui/widgets/settings_panel.hpp
// Settings tab — UI-side user preferences (toasts, paths, etc).
//
// Edits are auto-saved on every change so the user doesn't have to hit
// an explicit Save button — closing the window mid-edit doesn't lose
// anything. The agent never reads these (toast filtering happens UI-side
// in AyamaLogicNode; benchmark paths are UI-only).
//
#pragma once
#include "../AyamaAppState.hpp"
#include "../UserSettings.hpp"
#include "cpu_class_helpers.hpp"   // help_marker()
#include <imgui.h>
#include <cstdio>
#include <cstring>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#  include <shobjidl.h>     // IFileDialog (modern Vista+ folder picker)
#  include <objbase.h>      // CoInitializeEx / CoCreateInstance
#endif

namespace ayama_ui {

// File-static state — a single instance lives for the UI's lifetime.
// Lazy-loaded on first draw; auto-saved on every modification.
struct SettingsUiState {
    UserSettings settings{};
    bool         loaded {false};
};
inline SettingsUiState& settings_ui() noexcept {
    static SettingsUiState st;
    return st;
}

// Accessor used by toast triggers in AyamaLogicNode to gate notifications.
[[nodiscard]] inline const UserSettings& current_settings() noexcept {
    auto& st = settings_ui();
    if (!st.loaded) {
        st.settings = load_user_settings();
        st.loaded = true;
    }
    return st.settings;
}

#ifdef _WIN32
// Open the Vista+ "pick a folder" dialog and write the chosen path
// (UTF-8) into `out_buf`. Returns true if the user picked a folder,
// false if they cancelled or the dialog couldn't be created. We
// CoInitializeEx with APARTMENTTHREADED — the UI thread already lives
// in that apartment (GLFW + ImGui), so this is a no-op on second call.
inline bool pick_folder_dialog(char* out_buf, size_t buf_size) noexcept {
    if (!out_buf || buf_size == 0u) return false;
    out_buf[0] = '\0';

    HRESULT hr = CoInitializeEx(nullptr,
        COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    const bool we_inited = SUCCEEDED(hr);  // false also OK if already inited

    IFileDialog* dlg = nullptr;
    HRESULT r = CoCreateInstance(CLSID_FileOpenDialog, nullptr,
        CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg));
    if (FAILED(r) || !dlg) {
        if (we_inited) CoUninitialize();
        return false;
    }

    // FOS_PICKFOLDERS turns the file open dialog into a folder picker.
    DWORD opts = 0;
    dlg->GetOptions(&opts);
    dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM
                         | FOS_PATHMUSTEXIST);
    dlg->SetTitle(L"Choose benchmark output folder");

    bool picked = false;
    r = dlg->Show(nullptr);
    if (SUCCEEDED(r)) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dlg->GetResult(&item)) && item) {
            PWSTR wpath = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &wpath))
                && wpath)
            {
                const int n = WideCharToMultiByte(CP_UTF8, 0, wpath, -1,
                    out_buf, static_cast<int>(buf_size), nullptr, nullptr);
                if (n > 0) picked = true;
                CoTaskMemFree(wpath);
            }
            item->Release();
        }
    }
    dlg->Release();
    if (we_inited) CoUninitialize();
    return picked;
}
#endif

inline void draw_settings_panel() noexcept
{
    auto& st = settings_ui();
    if (!st.loaded) {
        st.settings = load_user_settings();
        st.loaded = true;
    }
    auto& s = st.settings;

    ImGui::Spacing();
    ImGui::SetWindowFontScale(1.3f);
    ImGui::TextUnformatted("Settings");
    ImGui::SetWindowFontScale(1.0f);
    ImGui::TextDisabled(
        "All changes are saved automatically. Toast preferences take "
        "effect on the next event; path changes apply on next launch.");
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // ── Toast notifications ──────────────────────────────────────────────
    if (ImGui::CollapsingHeader("Toast notifications",
                                 ImGuiTreeNodeFlags_DefaultOpen))
    {
        ImGui::Indent();
        ImGui::TextDisabled(
            "Windows tray balloons that appear when notable events happen.");
        ImGui::Spacing();

        bool changed = false;

        changed |= ImGui::Checkbox(
            "Game detected##t_game", &s.toast_game_detected);
        ImGui::SameLine();
        ayama_ui::help_marker(
            "Shows when Ayama classifies a process as a game and starts\n"
            "optimizing it. Filters out background apps, browsers, and\n"
            "launcher helpers — only true Games trigger this.");

        changed |= ImGui::Checkbox(
            "Regression detected##t_reg", &s.toast_regression);
        ImGui::SameLine();
        ayama_ui::help_marker(
            "Fires when AutoRevertGuard automatically reverts a policy\n"
            "because frame times got worse. Always recommended on — this\n"
            "is the user-visible signal that Ayama caught itself misfiring.");

        changed |= ImGui::Checkbox(
            "Agent connected##t_conn", &s.toast_agent_connected);
        ImGui::SameLine();
        ayama_ui::help_marker(
            "Shows the first time the UI connects to the background agent\n"
            "in this session. Fires once per launch, not per reconnect.");

        changed |= ImGui::Checkbox(
            "Each optimization applied##t_opt", &s.toast_optimization_applied);
        ImGui::SameLine();
        ayama_ui::help_marker(
            "Pops a toast for every individual Pin / SetPriority action.\n"
            "Useful for debugging policies; noisy in normal use. Default off.");

        if (changed) (void)save_user_settings(s);

        ImGui::Spacing();
        ImGui::TextDisabled(
            "Tip: silence them all from Windows Settings -> Notifications -> "
            "Ayama if you want to keep the agent running without any pop-ups.");
        ImGui::Unindent();
        ImGui::Spacing();
    }

    // ── Paths ────────────────────────────────────────────────────────────
    if (ImGui::CollapsingHeader("Paths",
                                 ImGuiTreeNodeFlags_DefaultOpen))
    {
        ImGui::Indent();
        ImGui::TextDisabled(
            "Where Ayama writes its benchmark CSV files and reports.");
        ImGui::Spacing();

        ImGui::TextUnformatted("Benchmark output directory:");
        // Wider input + Browse + Reset buttons on the same row. Browse
        // opens the Vista+ folder picker so the user doesn't have to
        // type or paste a path. Reset wipes the override back to default.
        ImGui::SetNextItemWidth(-165.f);
        if (ImGui::InputText("##bench_dir", s.bench_output_dir,
                             sizeof(s.bench_output_dir)))
        {
            (void)save_user_settings(s);
        }
        ImGui::SameLine();
#ifdef _WIN32
        if (ImGui::Button("Browse...##bench_dir_browse")) {
            char picked[260]{};
            if (pick_folder_dialog(picked, sizeof(picked))) {
                std::strncpy(s.bench_output_dir, picked,
                             sizeof(s.bench_output_dir) - 1u);
                s.bench_output_dir[sizeof(s.bench_output_dir) - 1u] = '\0';
                (void)save_user_settings(s);
            }
        }
        ImGui::SameLine();
#endif
        if (ImGui::Button("Reset##bench_dir_reset")) {
            s.bench_output_dir[0] = '\0';
            (void)save_user_settings(s);
        }
        ImGui::TextDisabled(
            "Leave empty for default: %%TEMP%%\\ayama-bench (Windows) or\n"
            "$TMPDIR/ayama-bench (POSIX). The directory is created if it\n"
            "does not exist when the Benchmark tab needs it.");
        ImGui::Unindent();
        ImGui::Spacing();
    }

    // ── Config-file locations (read-only, informational) ─────────────────
    if (ImGui::CollapsingHeader("Config-file locations (read-only)")) {
        ImGui::Indent();
        ImGui::TextDisabled(
            "These files are managed by Ayama and live in your user\n"
            "profile. You can edit them by hand if you know what you're\n"
            "doing; the agent reloads on file-modification.");
        ImGui::Spacing();
        ImGui::BulletText("UI settings: %%LOCALAPPDATA%%\\Ayama\\settings.txt");
        ImGui::BulletText("Policy config: %%LOCALAPPDATA%%\\Ayama\\policies.toml");
        ImGui::BulletText("Kind overrides: %%LOCALAPPDATA%%\\Ayama\\overrides.txt");
        ImGui::BulletText("Per-game memory: %%LOCALAPPDATA%%\\Ayama\\memory.toml");
        ImGui::BulletText("Audit log: %%LOCALAPPDATA%%\\Ayama\\audit.bin");
        ImGui::Unindent();
        ImGui::Spacing();
    }
}

} // namespace ayama_ui
// Made with my soul - Swately <3
