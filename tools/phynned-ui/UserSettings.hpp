// tools/phynned-ui/UserSettings.hpp
// UserSettings — UI-side user preferences (separate from agent config).
//
// Stored as plain text key=value at:
//   Windows: %LOCALAPPDATA%\Phynned\settings.txt
//   POSIX:   ~/.config/phynned/settings.txt
//
// Hot-saved on every edit so that closing the window mid-edit doesn't
// lose the setting. Loaded once at UI startup. Defaults are documented
// inline alongside each field.
//
#pragma once
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#  include <shlobj.h>
#  include <knownfolders.h>
#else
#  include <sys/stat.h>
#  include <unistd.h>
#  include <pwd.h>
#endif

namespace phynned_ui {

struct UserSettings {
    // ── Toast notification preferences ───────────────────────────────────
    // Each toggle gates one category of Windows tray balloon. Default-on
    // for the high-signal ones (game detection, regression), default-off
    // for the chatty ones (every applied action).
    bool toast_game_detected      {true};   ///< "Cyberpunk2077.exe detected"
    bool toast_regression         {true};   ///< "Policy reverted, perf got worse"
    bool toast_agent_connected    {true};   ///< First connect of the session
    bool toast_optimization_applied{false}; ///< Each Pin/SetPriority lands

    // ── Paths ────────────────────────────────────────────────────────────
    // Empty string = use the default (computed at runtime per-platform).
    // The bench panel uses this if non-empty; otherwise falls back to
    // %TEMP%\phynned-bench on Windows.
    char bench_output_dir[260]    {};
};

namespace detail {

inline std::string settings_file_path() noexcept {
#ifdef _WIN32
    wchar_t* appdata = nullptr;
    if (SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &appdata)
            != S_OK || !appdata)
    {
        if (appdata) CoTaskMemFree(appdata);
        return {};
    }
    const int len = WideCharToMultiByte(CP_UTF8, 0, appdata, -1,
                                         nullptr, 0, nullptr, nullptr);
    std::string base(static_cast<size_t>(len > 0 ? len - 1 : 0), '\0');
    if (len > 0) {
        WideCharToMultiByte(CP_UTF8, 0, appdata, -1,
                            base.data(), len, nullptr, nullptr);
    }
    CoTaskMemFree(appdata);
    if (base.empty()) return {};
    const std::string dir = base + "\\Phynned";
    CreateDirectoryA(dir.c_str(), nullptr);
    return dir + "\\settings.txt";
#else
    const char* xdg = std::getenv("XDG_CONFIG_HOME");
    std::string base;
    if (xdg && xdg[0]) base = xdg;
    else {
        const char* home = std::getenv("HOME");
        if (!home || !home[0]) {
            if (auto* pw = getpwuid(getuid())) home = pw->pw_dir;
        }
        if (!home) return {};
        base = std::string(home) + "/.config";
    }
    const std::string dir = base + "/phynned";
    mkdir(base.c_str(), 0755);
    mkdir(dir.c_str(), 0755);
    return dir + "/settings.txt";
#endif
}

inline bool parse_bool(const char* s) noexcept {
    if (!s) return false;
    return (s[0] == '1') || (s[0] == 't') || (s[0] == 'T') ||
           (s[0] == 'y') || (s[0] == 'Y');
}

} // namespace detail

[[nodiscard]] inline UserSettings load_user_settings() noexcept {
    UserSettings s{};
    const std::string path = detail::settings_file_path();
    if (path.empty()) return s;
    FILE* f = std::fopen(path.c_str(), "r");
    if (!f) return s;
    char line[512];
    while (std::fgets(line, sizeof(line), f)) {
        size_t L = std::strlen(line);
        while (L > 0u && (line[L-1] == '\n' || line[L-1] == '\r')) {
            line[--L] = '\0';
        }
        if (L == 0u || line[0] == '#') continue;
        char* eq = std::strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        const char* k = line;
        const char* v = eq + 1;
        if      (std::strcmp(k, "toast_game_detected") == 0)
            s.toast_game_detected = detail::parse_bool(v);
        else if (std::strcmp(k, "toast_regression") == 0)
            s.toast_regression = detail::parse_bool(v);
        else if (std::strcmp(k, "toast_agent_connected") == 0)
            s.toast_agent_connected = detail::parse_bool(v);
        else if (std::strcmp(k, "toast_optimization_applied") == 0)
            s.toast_optimization_applied = detail::parse_bool(v);
        else if (std::strcmp(k, "bench_output_dir") == 0) {
            std::strncpy(s.bench_output_dir, v, sizeof(s.bench_output_dir) - 1u);
            s.bench_output_dir[sizeof(s.bench_output_dir) - 1u] = '\0';
        }
    }
    std::fclose(f);
    return s;
}

inline bool save_user_settings(const UserSettings& s) noexcept {
    const std::string path = detail::settings_file_path();
    if (path.empty()) return false;
    FILE* f = std::fopen(path.c_str(), "w");
    if (!f) return false;
    std::fprintf(f,
        "# Phynned UI user settings (auto-generated; safe to edit by hand)\n"
        "# Toast toggles: 1/true/yes/y to enable, anything else to disable.\n"
        "toast_game_detected=%d\n"
        "toast_regression=%d\n"
        "toast_agent_connected=%d\n"
        "toast_optimization_applied=%d\n"
        "# Empty bench_output_dir = use platform default (%%TEMP%%\\phynned-bench\n"
        "# on Windows, $TMPDIR/phynned-bench on POSIX).\n"
        "bench_output_dir=%s\n",
        s.toast_game_detected ? 1 : 0,
        s.toast_regression ? 1 : 0,
        s.toast_agent_connected ? 1 : 0,
        s.toast_optimization_applied ? 1 : 0,
        s.bench_output_dir);
    std::fclose(f);
    return true;
}

} // namespace phynned_ui
// Made with my soul - Swately <3
