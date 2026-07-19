// config/include/phynned/config/ConfigStore.hpp
// ConfigStore — load/save Phynned configuration from TOML files.
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
//   Windows: %LOCALAPPDATA%\Phynned\policies.toml
//   Linux:   ~/.config/phynned/policies.toml
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

namespace phynned::config {

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
    Auto   = 0,   ///< Phynned decides and applies automatically.
    Assist = 1,   ///< Phynned notifies; user approves via UI.
    Manual = 2,   ///< User drives all decisions. Phynned just observes.
};

// ── Profile (W4 — global use-mode selector) ──────────────────────────────────
/// The four use-modes. GamesCorral is the default so a fresh install (or a v1
/// config with no `profile` key) behaves byte-identically to the pre-use-modes
/// build. Full is reserved for MR-3 (the A/B engine); it parses and the agent
/// falls back to GamesCorral behaviour with a log line.
enum class Profile : uint8_t {
    Monitor     = 0,  ///< observe + advise only — ZERO placement anywhere (R6).
    Games       = 1,  ///< classic game placement only; corral off.
    GamesCorral = 2,  ///< today's behaviour (game placement + background corral).
    Full        = 3,  ///< reserved (MR-3); falls back to GamesCorral for now.
};

// ── ProcessRule (W3 — one persistent per-process user rule) ──────────────────
/// A user-authored rule matched by exe name (required) and, optionally, full
/// path. `path[0] == '\0'` means "match any path". action selects the intent.
struct ProcessRule {
    char    name[64];   ///< exe basename (required), null-terminated. Match key.
    char    path[260];  ///< optional full path; "" = match any path.
    uint8_t action;     ///< 0 = Never (veto), 1 = Freq CCD, 2 = V-Cache CCD.
    uint8_t _pad[3];
};
static_assert(sizeof(ProcessRule) == 328u);
static_assert(std::is_trivially_copyable_v<ProcessRule>);

/// ProcessRule::action values (kept in sync with ipc::UserRuleShm::action).
enum class RuleAction : uint8_t {
    Never  = 0,   ///< user veto — never optimise this process (precedence #2).
    Freq   = 1,   ///< user forces the Frequency CCD (still AC-gated + journaled).
    VCache = 2,   ///< user forces the V-Cache CCD (still AC-gated + journaled).
};

// ── AgentConfig — full user-editable configuration ───────────────────────────
struct AgentConfig {
    static constexpr uint32_t kVersion         = 2u;   ///< bumped for use-modes (W2)
    static constexpr uint32_t kMaxRules        = 32u;
    static constexpr uint32_t kMaxProcessRules = 128u;

    uint32_t       version     {kVersion};
    OpMode         op_mode     {OpMode::Auto};
    uint8_t        _pad[3]     {};
    uint32_t       n_overrides {0u};
    PolicyOverride overrides[kMaxRules] {};

    // ── Use-modes (W2 persistence) — every field defaults so a v1 file that
    //    lacks them loads to the pre-use-modes behaviour (back-compat, R2). ──
    Profile        profile               {Profile::GamesCorral};
    bool           corral_enabled        {false};  ///< [corral] enabled (survives restart)
    bool           corral_keep_on_disable{false};  ///< keep placements when the corral is disabled
    uint8_t        _pad2[1]              {};
    uint32_t       n_process_rules       {0u};
    ProcessRule    process_rules[kMaxProcessRules] {};
};
static_assert(std::is_trivially_copyable_v<AgentConfig>);

// ── ConfigStore ──────────────────────────────────────────────────────────────
class ConfigStore {
public:
    ConfigStore() = delete;  // static-only API

    // ── Path resolution ───────────────────────────────────────────────────
    /// Resolve the default configuration directory into `out` (null-terminated).
    /// Returns false if the environment variable is not set or buffer too small.
    ///   Windows: %LOCALAPPDATA%\Phynned\
    ///   Other:   ~/.config/phynned/
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

    /// Find the ProcessRule matching (exe_name[, full_path]) using the W3
    /// matching contract: case-insensitive exe-name compare; if the stored
    /// rule's `path` is non-empty, the caller-supplied `full_path` must ALSO
    /// match case-insensitively. `full_path` may be nullptr (name-only match
    /// against a name-only rule). Returns the FIRST matching rule, or nullptr.
    [[nodiscard]] static const ProcessRule*
    find_process_rule(const AgentConfig& cfg,
                      const char* exe_name,
                      const char* full_path = nullptr) noexcept;
};

} // namespace phynned::config
// Made with my soul - Swately <3
