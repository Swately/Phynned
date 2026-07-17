// config/src/ConfigStore.cpp
// ConfigStore — TOML load/save implementation.
//
// Parser notes:
//   - Lines beginning with '#' or blank are silently skipped.
//   - '[[rule]]' opens a new PolicyOverride accumulator.
//   - 'key = value' lines update the most recent accumulator (or root).
//   - String values must be double-quoted: name = "PinGameToVCacheCcd".
//   - Bool values: true | false (lowercase).
//   - Integer values: decimal only, no separators.
//   - No multiline, no inline tables, no arrays of inline tables — only the
//     subset required by Phynned's own TOML schema.
//

#include <phynned/config/ConfigStore.hpp>
#include <phynned/version.hpp>

#include <cstdio>
#include <cstring>
#include <cctype>
#include <cstdlib>

// ── Platform helpers ──────────────────────────────────────────────────────────
#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#  include <shlobj.h>    // SHGetFolderPathA
#  include <sys/stat.h>
#  include <direct.h>    // _mkdir
#else
#  include <sys/stat.h>
#  include <pwd.h>
#  include <unistd.h>
#endif

namespace phynned::config {

// ── Internal helpers ──────────────────────────────────────────────────────────
namespace {

// Trim leading/trailing ASCII whitespace in-place.
// Returns pointer to the first non-space character.
[[nodiscard]] static const char* ltrim(const char* p) noexcept
{
    while (*p && (unsigned char)*p <= ' ') ++p;
    return p;
}

// Copy at most dst_max-1 chars from src, then null-terminate.
static void safe_strncpy(char* dst, const char* src, uint32_t dst_max) noexcept
{
    if (dst_max == 0u) return;
    std::strncpy(dst, src, dst_max - 1u);
    dst[dst_max - 1u] = '\0';
}

// Extract the quoted string value from a line like: key = "value"
// Writes into out (null-terminated, max out_max bytes).
// Returns true on success.
[[nodiscard]] static bool extract_string(const char* rhs,
                                          char* out,
                                          uint32_t out_max) noexcept
{
    const char* p = ltrim(rhs);
    if (*p != '"') return false;
    ++p;  // skip opening quote
    const char* end = std::strchr(p, '"');
    if (!end) return false;
    const uint32_t len = static_cast<uint32_t>(end - p);
    const uint32_t copy_len = (len < out_max - 1u) ? len : out_max - 1u;
    std::memcpy(out, p, copy_len);
    out[copy_len] = '\0';
    return true;
}

// Parse a decimal integer from rhs. Returns 0 on failure.
[[nodiscard]] static uint64_t extract_uint(const char* rhs) noexcept
{
    const char* p = ltrim(rhs);
    if (!std::isdigit((unsigned char)*p)) return 0u;
    char* end = nullptr;
    return static_cast<uint64_t>(std::strtoull(p, &end, 10));
}

// Parse a bool from rhs. Returns true if "true", false otherwise.
[[nodiscard]] static bool extract_bool(const char* rhs) noexcept
{
    const char* p = ltrim(rhs);
    return std::strncmp(p, "true", 4) == 0;
}

// Parse op_mode from string.
[[nodiscard]] static OpMode parse_op_mode(const char* str) noexcept
{
    if (std::strcmp(str, "assist") == 0) return OpMode::Assist;
    if (std::strcmp(str, "manual") == 0) return OpMode::Manual;
    return OpMode::Auto;
}

// Convert OpMode to its string representation.
[[nodiscard]] static const char* op_mode_str(OpMode m) noexcept
{
    switch (m) {
        case OpMode::Assist: return "assist";
        case OpMode::Manual: return "manual";
        default:             return "auto";
    }
}

// Ensure a directory exists. Creates it if absent (single level only).
[[nodiscard]] static bool ensure_dir(const char* path) noexcept
{
#ifdef _WIN32
    const DWORD attr = GetFileAttributesA(path);
    if (attr != INVALID_FILE_ATTRIBUTES) return true;  // already exists
    return ::CreateDirectoryA(path, nullptr) != 0;
#else
    struct stat st{};
    if (::stat(path, &st) == 0) return true;
    return ::mkdir(path, 0755) == 0;
#endif
}

} // namespace

// ── get_config_dir ────────────────────────────────────────────────────────────
bool ConfigStore::get_config_dir(char* out, uint32_t max_len) noexcept
{
    if (!out || max_len == 0u) return false;

#ifdef _WIN32
    // %LOCALAPPDATA%\Phynned (directory)
    char appdata[MAX_PATH]{};
    if (::GetEnvironmentVariableA("LOCALAPPDATA", appdata, MAX_PATH) == 0) {
        // Fallback: SHGetFolderPath
        if (FAILED(::SHGetFolderPathA(nullptr, CSIDL_LOCAL_APPDATA,
                                       nullptr, 0, appdata))) {
            return false;
        }
    }
    const int n = std::snprintf(out, max_len, "%s\\Phynned\\", appdata);
    return (n > 0 && static_cast<uint32_t>(n) < max_len);
#else
    const char* home = ::getenv("HOME");
    if (!home) {
        const struct passwd* pw = ::getpwuid(::getuid());
        if (!pw) return false;
        home = pw->pw_dir;
    }
    const int n = std::snprintf(out, max_len, "%s/.config/phynned/", home);
    return (n > 0 && static_cast<uint32_t>(n) < max_len);
#endif
}

// ── get_policies_path ─────────────────────────────────────────────────────────
bool ConfigStore::get_policies_path(char* out, uint32_t max_len) noexcept
{
    char dir[512]{};
    if (!get_config_dir(dir, sizeof(dir))) return false;
#ifdef _WIN32
    const int n = std::snprintf(out, max_len, "%spolicies.toml", dir);
#else
    const int n = std::snprintf(out, max_len, "%spolicies.toml", dir);
#endif
    return (n > 0 && static_cast<uint32_t>(n) < max_len);
}

// ── load_policies ─────────────────────────────────────────────────────────────
std::expected<AgentConfig, phyriad::Error>
ConfigStore::load_policies(const char* path) noexcept
{
    AgentConfig cfg{};

    // Resolve path
    char resolved[512]{};
    if (path) {
        safe_strncpy(resolved, path, sizeof(resolved));
    } else {
        if (!get_policies_path(resolved, sizeof(resolved))) {
            return cfg;  // Return default config if path resolution fails.
        }
    }

    std::FILE* f = std::fopen(resolved, "r");
    if (!f) {
        // File not found → return default config (first-run behaviour).
        return cfg;
    }

    // ── Line-by-line TOML parse ──────────────────────────────────────────
    char line[256]{};
    bool in_rule = false;
    PolicyOverride cur{};

    auto commit_rule = [&]() noexcept {
        if (!in_rule) return;
        if (cur.name[0] != '\0' && cfg.n_overrides < AgentConfig::kMaxRules) {
            cfg.overrides[cfg.n_overrides++] = cur;
        }
        cur = PolicyOverride{};
        cur.confidence = 255u;  // sentinel: "use rule default"
        in_rule = false;
    };

    // Initialize sentinel on first rule entry.
    cur.confidence = 255u;

    while (std::fgets(line, sizeof(line), f)) {
        // Strip trailing newline / CR
        const uint32_t llen = static_cast<uint32_t>(std::strlen(line));
        if (llen > 0u && (line[llen - 1u] == '\n' || line[llen - 1u] == '\r'))
            line[llen - 1u] = '\0';
        if (llen > 1u && line[llen - 2u] == '\r')
            line[llen - 2u] = '\0';

        const char* p = ltrim(line);

        // Skip blank lines and comments
        if (*p == '\0' || *p == '#') continue;

        // Section header: [[rule]]
        if (std::strncmp(p, "[[rule]]", 8) == 0) {
            commit_rule();  // Commit previous rule (if any)
            in_rule = true;
            cur.confidence = 255u;
            continue;
        }

        // Find '=' separator
        const char* eq = std::strchr(p, '=');
        if (!eq) continue;

        // Extract key (trim trailing spaces)
        char key[64]{};
        {
            const char* key_start = p;
            const char* key_end   = eq;
            while (key_end > key_start &&
                   (unsigned char)*(key_end - 1) <= ' ') --key_end;
            const uint32_t klen = static_cast<uint32_t>(key_end - key_start);
            const uint32_t copy = (klen < sizeof(key) - 1u) ? klen : sizeof(key) - 1u;
            std::memcpy(key, key_start, copy);
            key[copy] = '\0';
        }

        const char* rhs = eq + 1u;  // value side (still has leading spaces)

        // ── Root-level keys ──────────────────────────────────────────────
        if (!in_rule) {
            if (std::strcmp(key, "version") == 0) {
                cfg.version = static_cast<uint32_t>(extract_uint(rhs));
            } else if (std::strcmp(key, "op_mode") == 0) {
                char mode_str[16]{};
                if (extract_string(rhs, mode_str, sizeof(mode_str))) {
                    cfg.op_mode = parse_op_mode(mode_str);
                }
            }
        }

        // ── Rule-level keys ──────────────────────────────────────────────
        else {
            if (std::strcmp(key, "name") == 0) {
                extract_string(rhs, cur.name, sizeof(cur.name));
            } else if (std::strcmp(key, "enabled") == 0) {
                cur.enabled = extract_bool(rhs);
            } else if (std::strcmp(key, "confidence") == 0) {
                const uint64_t v = extract_uint(rhs);
                cur.confidence = (v <= 100u) ? static_cast<uint8_t>(v) : 255u;
            } else if (std::strcmp(key, "core_mask") == 0) {
                // core_mask may be hex (0x...) or decimal
                const char* vp = ltrim(rhs);
                char* end = nullptr;
                if (vp[0] == '0' && (vp[1] == 'x' || vp[1] == 'X')) {
                    cur.core_mask = std::strtoull(vp, &end, 16);
                } else {
                    cur.core_mask = std::strtoull(vp, &end, 10);
                }
            }
        }
    }

    commit_rule();  // Commit last rule if file ended in [[rule]] section.
    std::fclose(f);
    return cfg;
}

// ── save_policies ─────────────────────────────────────────────────────────────
std::expected<void, phyriad::Error>
ConfigStore::save_policies(const AgentConfig& cfg,
                            const char* path) noexcept
{
    // Resolve path
    char resolved[512]{};
    if (path) {
        safe_strncpy(resolved, path, sizeof(resolved));
    } else {
        if (!get_policies_path(resolved, sizeof(resolved))) {
            return std::unexpected(phyriad::Error{phyriad::ErrorCode::IoError, 0u, 0u});
        }
    }

    // Ensure config directory exists
    {
        char dir[512]{};
        get_config_dir(dir, sizeof(dir));
        if (dir[0] != '\0') {
            // Strip trailing path separator for ensure_dir
            char dir_no_sep[512]{};
            safe_strncpy(dir_no_sep, dir, sizeof(dir_no_sep));
            const uint32_t dlen = static_cast<uint32_t>(std::strlen(dir_no_sep));
            if (dlen > 0u && (dir_no_sep[dlen - 1u] == '/' ||
                               dir_no_sep[dlen - 1u] == '\\')) {
                dir_no_sep[dlen - 1u] = '\0';
            }
            ensure_dir(dir_no_sep);
        }
    }

    std::FILE* f = std::fopen(resolved, "w");
    if (!f) {
        return std::unexpected(phyriad::Error{phyriad::ErrorCode::IoError, 0u, 0u});
    }

    // ── Write TOML ────────────────────────────────────────────────────────
    std::fprintf(f,
        "# Phynned policies configuration\n"
        "# Generated by Phynned v%s — edit with care.\n"
        "# op_mode: \"auto\" | \"assist\" | \"manual\"\n"
        "\n"
        "version = %u\n"
        "op_mode = \"%s\"\n"
        "\n",
        PHYNNED_VERSION_STRING, cfg.version, op_mode_str(cfg.op_mode));

    for (uint32_t i = 0u; i < cfg.n_overrides; ++i) {
        const PolicyOverride& o = cfg.overrides[i];
        if (o.name[0] == '\0') continue;

        std::fprintf(f, "[[rule]]\n");
        std::fprintf(f, "name       = \"%s\"\n", o.name);
        std::fprintf(f, "enabled    = %s\n",     o.enabled ? "true" : "false");

        if (o.confidence != 255u) {
            std::fprintf(f, "confidence = %u\n", static_cast<unsigned>(o.confidence));
        } else {
            std::fprintf(f, "confidence = 255  # use rule default\n");
        }

        if (o.core_mask != 0ull) {
            std::fprintf(f, "core_mask  = 0x%llx\n",
                         static_cast<unsigned long long>(o.core_mask));
        } else {
            std::fprintf(f, "core_mask  = 0    # use rule default\n");
        }
        std::fprintf(f, "\n");
    }

    std::fclose(f);
    return {};
}

// ── find_override ─────────────────────────────────────────────────────────────
const PolicyOverride*
ConfigStore::find_override(const AgentConfig& cfg,
                            const char* rule_name) noexcept
{
    if (!rule_name) return nullptr;
    for (uint32_t i = 0u; i < cfg.n_overrides; ++i) {
        if (std::strcmp(cfg.overrides[i].name, rule_name) == 0) {
            return &cfg.overrides[i];
        }
    }
    return nullptr;
}

} // namespace phynned::config
// Made with my soul - Swately <3
