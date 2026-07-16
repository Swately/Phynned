// apps/ayama/config/include/ayama/config/ConfigStore.hpp
// ConfigStore — load/save Ayama configuration from TOML files.
//
// Handles two configuration files:
//   policies.toml — user overrides for policy rules (enabled, confidence, mask).
//
// TOML format supported (subset):
//   version   = <integer>
//   op_mode   = "<string>"   — "auto" | "assist" | "manual"
//   [[rule]]
//   name       = "<string>"
//   enabled    = <bool>
//   confidence = <integer>   — 0-100; 255 means "use rule default"
//   core_mask  = <integer>   — 0 means "use rule default"
//
// Default path (resolved lazily):
//   Windows: %LOCALAPPDATA%\Ayama\policies.toml
//   Linux:   ~/.config/ayama/policies.toml
//
// Threading: not thread-safe. Call from agent main thread at startup/shutdown.
// Resource:  allocates during load/save only (not in hot path).
//            All returned structs are fixed-size, stack/class-friendly.
// Privilege: None (file access only).
//
#pragma once

#include <phyriad/schema/Error.hpp>
#include <cstdint>
#include <cstring>
#include <expected>
#include <type_traits>

namespace ayama::config {

// ── PolicyOverride — one rule's user-configurable settings ──────────────────
struct alignas(8) PolicyOverride {
    char     name[40];         ///< Rule name (matches Rule::name). Null-terminated.
    bool     enabled;          ///< Enable/disable this rule.
    uint8_t  confidence;       ///< Override confidence 0-100. 255 = use rule default.
    uint8_t  _pad[2];
    uint64_t core_mask;        ///< Override core mask. 0 = use rule default.
};
static_assert(sizeof(PolicyOverride) == 56u);
static_assert(std::is_trivially_copyable_v<PolicyOverride>);

// ── Op mode ──────────────────────────────────────────────────────────────────
enum class OpMode : uint8_t {
    Auto   = 0,   ///< Ayama decides and applies automatically.
    Assist = 1,   ///< Ayama notifies; user approves via UI.
    Manual = 2,   ///< User drives all decisions. Ayama just observes.
};

// ── AgentConfig — full user-editable configuration ───────────────────────────
struct AgentConfig {
    static constexpr uint32_t kVersion   = 1u;
    static constexpr uint32_t kMaxRules  = 32u;

    uint32_t       version     {kVersion};
    OpMode         op_mode     {OpMode::Auto};
    uint8_t        _pad[3]     {};
    uint32_t       n_overrides {0u};
    PolicyOverride overrides[kMaxRules] {};
};
static_assert(std::is_trivially_copyable_v<AgentConfig>);

// ── ConfigStore ──────────────────────────────────────────────────────────────
class ConfigStore {
public:
    ConfigStore() = delete;  // static-only API

    // ── Path resolution ───────────────────────────────────────────────────
    /// Resolve the default configuration directory into `out` (null-terminated).
    /// Returns false if the environment variable is not set or buffer too small.
    ///   Windows: %LOCALAPPDATA%\Ayama\
    ///   Other:   ~/.config/ayama/
    [[nodiscard]] static bool
    get_config_dir(char* out, uint32_t max_len) noexcept;

    /// Resolve the default policies.toml path into `out`.
    [[nodiscard]] static bool
    get_policies_path(char* out, uint32_t max_len) noexcept;

    // ── Load ──────────────────────────────────────────────────────────────
    /// Load policies.toml from `path`. If path is nullptr, uses the default.
    /// Returns default AgentConfig if the file does not exist (first run).
    [[nodiscard]] static std::expected<AgentConfig, phyriad::Error>
    load_policies(const char* path = nullptr) noexcept;

    // ── Save ──────────────────────────────────────────────────────────────
    /// Save cfg to policies.toml at `path`. If path is nullptr, uses the default.
    /// Creates the directory if it does not exist.
    [[nodiscard]] static std::expected<void, phyriad::Error>
    save_policies(const AgentConfig& cfg,
                  const char* path = nullptr) noexcept;

    // ── Merge helpers ─────────────────────────────────────────────────────
    /// Find the PolicyOverride for the given rule name.
    /// Returns nullptr if no override exists for that rule.
    [[nodiscard]] static const PolicyOverride*
    find_override(const AgentConfig& cfg, const char* rule_name) noexcept;
};

} // namespace ayama::config
// Made with my soul - Swately <3
