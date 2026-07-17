// observer/include/phynned/observer/KindOverrides.hpp
// KindOverrides — exe-name -> TargetKind overrides that take precedence
// over the heuristic classifier.
//
// Format (plain text, one entry per line):
//
//   # comments start with '#'
//   <exe-name>=<KindName>
//
//   Valid KindNames: Game Stream Comm Browser Productivity Unknown System
//   (System means "never touch" — equivalent to the OS-process guard.)
//
//   Example:
//     steamwebhelper.exe=Productivity
//     MyIndieGame.exe=Game
//     SomeMisclassifiedThing.exe=System
//
// Loaded at agent startup and reloaded each time the file's modification
// time changes (polled by AgentRuntime alongside the regular tick). The UI
// writes the file via add_override() / save(); the agent calls load() at
// startup and reload_if_changed() each tick.
//
// Threading: not thread-safe. All access happens on the agent main thread.
// Persistence: %LOCALAPPDATA%\Phynned\overrides.txt (Windows) or
//              ~/.config/phynned/overrides.txt (POSIX).
//
#pragma once

#include <phynned/observer/TargetProcess.hpp>
#include <cstdint>
#include <cstdio>
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
#  include <sys/types.h>
#  include <unistd.h>
#  include <pwd.h>
#endif

namespace phynned::observer {

// ── Helpers ─────────────────────────────────────────────────────────────────

[[nodiscard]] inline const char* kind_to_name(TargetKind k) noexcept {
    switch (k) {
        case TargetKind::Unknown:      return "Unknown";
        case TargetKind::Game:         return "Game";
        case TargetKind::Stream:       return "Stream";
        case TargetKind::Comm:         return "Comm";
        case TargetKind::Browser:      return "Browser";
        case TargetKind::Productivity: return "Productivity";
        case TargetKind::System:       return "System";
    }
    return "Unknown";
}

[[nodiscard]] inline bool kind_from_name(const char* s, TargetKind& out) noexcept {
    if (!s) return false;
    if (std::strcmp(s, "Unknown")      == 0) { out = TargetKind::Unknown;      return true; }
    if (std::strcmp(s, "Game")         == 0) { out = TargetKind::Game;         return true; }
    if (std::strcmp(s, "Stream")       == 0) { out = TargetKind::Stream;       return true; }
    if (std::strcmp(s, "Comm")         == 0) { out = TargetKind::Comm;         return true; }
    if (std::strcmp(s, "Browser")      == 0) { out = TargetKind::Browser;      return true; }
    if (std::strcmp(s, "Productivity") == 0) { out = TargetKind::Productivity; return true; }
    if (std::strcmp(s, "System")       == 0) { out = TargetKind::System;       return true; }
    return false;
}

// Path resolver: %LOCALAPPDATA%\Phynned\overrides.txt or POSIX equivalent.
[[nodiscard]] inline std::string overrides_file_path() noexcept {
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
    return dir + "\\overrides.txt";
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
    return dir + "/overrides.txt";
#endif
}

// ── KindOverrides ──────────────────────────────────────────────────────────
class KindOverrides {
public:
    static constexpr uint32_t kMaxOverrides = 64u;

    struct Entry {
        char       exe[40];   // matches TargetProcess::name
        TargetKind kind;
        uint8_t    _pad[7];
    };

    KindOverrides() noexcept = default;

    [[nodiscard]] uint32_t count() const noexcept { return n_; }

    /// Returns the override TargetKind for `exe_name`, or nullopt if no
    /// override exists. O(N) — N <= 64.
    [[nodiscard]] bool lookup(const char* exe_name, TargetKind& out) const noexcept {
        if (!exe_name) return false;
        for (uint32_t i = 0u; i < n_; ++i) {
            if (istr_eq(entries_[i].exe, exe_name)) {
                out = entries_[i].kind;
                return true;
            }
        }
        return false;
    }

    /// Add or update an override (idempotent — same exe replaces).
    /// Returns false if the table is full and the entry is new.
    bool set(const char* exe_name, TargetKind kind) noexcept {
        if (!exe_name || !exe_name[0]) return false;
        for (uint32_t i = 0u; i < n_; ++i) {
            if (istr_eq(entries_[i].exe, exe_name)) {
                entries_[i].kind = kind;
                return true;
            }
        }
        if (n_ >= kMaxOverrides) return false;
        std::strncpy(entries_[n_].exe, exe_name, sizeof(entries_[n_].exe) - 1u);
        entries_[n_].exe[sizeof(entries_[n_].exe) - 1u] = '\0';
        entries_[n_].kind = kind;
        ++n_;
        return true;
    }

    /// Remove an override. Returns true if an entry was removed.
    bool remove(const char* exe_name) noexcept {
        if (!exe_name) return false;
        for (uint32_t i = 0u; i < n_; ++i) {
            if (istr_eq(entries_[i].exe, exe_name)) {
                if (i + 1u < n_) entries_[i] = entries_[n_ - 1u];
                --n_;
                return true;
            }
        }
        return false;
    }

    /// Iterate all entries (UI uses this to render the override table).
    [[nodiscard]] const Entry* data() const noexcept { return entries_; }

    // ── Disk persistence ─────────────────────────────────────────────────
    /// Load from `overrides_file_path()`. Returns the number of entries
    /// loaded (0 if file missing or empty). Existing entries are cleared.
    uint32_t load() noexcept {
        n_ = 0u;
        const std::string path = overrides_file_path();
        if (path.empty()) return 0u;
        FILE* f = std::fopen(path.c_str(), "r");
        if (!f) return 0u;
        char line[128];
        while (std::fgets(line, sizeof(line), f) && n_ < kMaxOverrides) {
            // Strip trailing newline.
            size_t L = std::strlen(line);
            while (L > 0u && (line[L-1] == '\n' || line[L-1] == '\r'
                              || line[L-1] == ' ' || line[L-1] == '\t')) {
                line[--L] = '\0';
            }
            // Skip blanks + comments.
            if (L == 0u || line[0] == '#') continue;
            char* eq = std::strchr(line, '=');
            if (!eq) continue;
            *eq = '\0';
            const char* exe  = line;
            const char* kstr = eq + 1;
            TargetKind k;
            if (kind_from_name(kstr, k)) {
                std::strncpy(entries_[n_].exe, exe, sizeof(entries_[n_].exe) - 1u);
                entries_[n_].exe[sizeof(entries_[n_].exe) - 1u] = '\0';
                entries_[n_].kind = k;
                ++n_;
            }
        }
        std::fclose(f);
        cache_mtime();
        return n_;
    }

    /// Persist current overrides to disk. Returns true on success.
    bool save() const noexcept {
        const std::string path = overrides_file_path();
        if (path.empty()) return false;
        FILE* f = std::fopen(path.c_str(), "w");
        if (!f) return false;
        std::fprintf(f,
            "# Phynned manual TargetKind overrides\n"
            "# Format: <exe-name>=<Kind>\n"
            "# Kinds: Game Stream Comm Browser Productivity Unknown System\n"
            "# 'System' means \"never touch\" — equivalent to the OS-process guard.\n"
            "\n");
        for (uint32_t i = 0u; i < n_; ++i) {
            std::fprintf(f, "%s=%s\n", entries_[i].exe,
                         kind_to_name(entries_[i].kind));
        }
        std::fclose(f);
        return true;
    }

    /// Returns true and reloads from disk if the file's mtime has changed
    /// since last load(). Cheap to call every tick — only stat()'s the file.
    bool reload_if_changed() noexcept {
        const std::string path = overrides_file_path();
        if (path.empty()) return false;
#ifdef _WIN32
        WIN32_FILE_ATTRIBUTE_DATA fad{};
        if (!GetFileAttributesExA(path.c_str(), GetFileExInfoStandard, &fad)) {
            return false;
        }
        const uint64_t mtime =
            (static_cast<uint64_t>(fad.ftLastWriteTime.dwHighDateTime) << 32) |
             static_cast<uint64_t>(fad.ftLastWriteTime.dwLowDateTime);
#else
        struct stat st{};
        if (::stat(path.c_str(), &st) != 0) return false;
        const uint64_t mtime = static_cast<uint64_t>(st.st_mtime);
#endif
        if (mtime != last_mtime_) {
            (void)load();
            return true;
        }
        return false;
    }

private:
    Entry    entries_[kMaxOverrides]{};
    uint32_t n_{0u};
    uint64_t last_mtime_{0u};

    void cache_mtime() noexcept {
        const std::string path = overrides_file_path();
#ifdef _WIN32
        WIN32_FILE_ATTRIBUTE_DATA fad{};
        if (GetFileAttributesExA(path.c_str(), GetFileExInfoStandard, &fad)) {
            last_mtime_ =
                (static_cast<uint64_t>(fad.ftLastWriteTime.dwHighDateTime) << 32) |
                 static_cast<uint64_t>(fad.ftLastWriteTime.dwLowDateTime);
        }
#else
        struct stat st{};
        if (::stat(path.c_str(), &st) == 0) {
            last_mtime_ = static_cast<uint64_t>(st.st_mtime);
        }
#endif
    }

    static bool istr_eq(const char* a, const char* b) noexcept {
#ifdef _WIN32
        return ::_stricmp(a, b) == 0;
#else
        while (*a && *b) {
            unsigned char ca = static_cast<unsigned char>(*a);
            unsigned char cb = static_cast<unsigned char>(*b);
            if ((ca|0x20) != (cb|0x20)) return false;
            ++a; ++b;
        }
        return *a == '\0' && *b == '\0';
#endif
    }
};

} // namespace phynned::observer
// Made with my soul - Swately <3
