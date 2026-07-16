// apps/ayama/config/include/ayama/config/DefaultPolicyPack.hpp
// DefaultPolicyPack — hardware-specific default policy configuration.
//
// Generates and persists a hardware-appropriate policies.toml on first run
// so that:
//   1. Users can inspect/edit what Ayama is doing.
//   2. The "Reset to defaults" button in the Policies panel has a target.
//   3. The agent's log shows which CPU class was detected and what policy applies.
//
// CPU class → default policy pack:
//
//   AMD X3D (single/dual CCD):
//     PinGameToVCacheCcd = ON  (confidence 90)
//     EvictStreamFromHotCcd = ON (dual only, confidence 80)
//
//   Intel Hybrid (P+E):
//     PinGameToPCores = ON  (confidence 85)
//     PinCommToECores = ON  (confidence 75)
//
//   AMD Multi-CCX no X3D:
//     IsolateGameFromBackground = ON (confidence 80)
//
//   Single CCD (homogeneous):
//     All rules OFF — no affinity change expected to help (< 2% benefit).
//
// The generated TOML includes comments explaining each rule and the detected
// CPU class, making the config file self-documenting.
//
// Usage:
//   DefaultPolicyPack::write_if_missing(topo);   // call at agent start
//
// Threading: single-thread (agent main thread at startup only).
// Privilege: None (file write to %LOCALAPPDATA%\Ayama\).
//
#pragma once

#include <ayama/config/ConfigStore.hpp>
#include <ayama/policy/AutoPolicySelector.hpp>
#include <phyriad/topology/HardwareTopology.hpp>

#include <cstdio>
#include <cstring>

namespace ayama::config {

class DefaultPolicyPack {
public:
    DefaultPolicyPack() = delete;  // static-only API

    // ── Main entry point ──────────────────────────────────────────────────────

    /// Write the default policies.toml for the detected hardware class, but
    /// ONLY if the file does not already exist (first run).
    /// Returns true if a new file was written, false if it already existed or
    /// if writing failed (failures are non-fatal — agent runs with built-ins).
    [[nodiscard]] static bool write_if_missing(
        const phyriad::HardwareTopology& topo) noexcept
    {
        char path[512]{};
        if (!ConfigStore::get_policies_path(path, sizeof(path))) return false;

        // Check if file already exists.
        std::FILE* f = std::fopen(path, "r");
        if (f) { std::fclose(f); return false; }  // already exists → skip

        return write_to(path, topo);
    }

    /// Write the default pack to `path` unconditionally (for "Reset to defaults").
    [[nodiscard]] static bool write_to(
        const char*                  path,
        const phyriad::HardwareTopology& topo) noexcept
    {
        // Detect CPU class.
        policy::AutoPolicySelector sel;
        sel.init_from_topology(topo);
        const policy::CpuClass cpu_class = sel.cpu_class();

        std::FILE* f = std::fopen(path, "w");
        if (!f) {
            std::fprintf(stderr,
                "[DefaultPolicyPack] Cannot write '%s'\n", path);
            return false;
        }

        write_header(f, cpu_class);
        write_rules(f, cpu_class);

        std::fclose(f);
        std::fprintf(stdout,
            "[Ayama] Default policy pack written for CPU class '%s': %s\n",
            sel.class_name(), path);
        return true;
    }

    /// Build an AgentConfig for the detected hardware class (used by unit tests
    /// and the "Reset to defaults" IPC handler without touching the file system).
    [[nodiscard]] static AgentConfig make_config(
        const phyriad::HardwareTopology& topo) noexcept
    {
        policy::AutoPolicySelector sel;
        sel.init_from_topology(topo);

        AgentConfig cfg{};
        cfg.op_mode = OpMode::Auto;
        fill_overrides(cfg, sel.cpu_class());
        return cfg;
    }

private:
    // ── TOML generation ───────────────────────────────────────────────────────

    static void write_header(std::FILE* f,
                             policy::CpuClass cpu_class) noexcept
    {
        const char* class_name = policy::AutoPolicySelector::class_name(cpu_class);
        std::fprintf(f,
            "# Ayama default policy pack — auto-generated on first run.\n"
            "# Edit this file to customise Ayama's behaviour.\n"
            "# Delete this file to regenerate defaults on next agent start.\n"
            "#\n"
            "# Detected CPU class: %s\n"
            "#\n\n"
            "version = 1\n"
            "op_mode = \"auto\"\n\n",
            class_name);
    }

    static void write_rules(std::FILE* f,
                            policy::CpuClass cpu_class) noexcept
    {
        switch (cpu_class) {
        case policy::CpuClass::X3DSingle:
            write_rule(f, "PinGameToVCacheCcd", true, 90u,
                "Pin game threads to V-Cache CCD (lower frame time P99).");
            write_rule(f, "PinGameToPCores",    false, 85u,
                "Disabled — Intel P-core rule, not applicable to AMD X3D.");
            break;

        case policy::CpuClass::X3DDual:
            write_rule(f, "PinGameToVCacheCcd", true, 90u,
                "Pin game threads to V-Cache CCD (CCD0 with 96MB L3).");
            write_rule(f, "EvictStreamFromHotCcd", true, 80u,
                "Move stream/OBS encoder off the V-Cache CCD onto CCD1.");
            write_rule(f, "PinGameToPCores", false, 85u,
                "Disabled — Intel P-core rule, not applicable to AMD X3D.");
            break;

        case policy::CpuClass::HybridIntel:
            write_rule(f, "PinGameToPCores", true, 85u,
                "Pin game threads to P-cores (high-performance cores).");
            write_rule(f, "PinCommToECores", true, 75u,
                "Move Discord/Teams/Zoom to E-cores (efficiency cores).");
            write_rule(f, "PinGameToVCacheCcd", false, 90u,
                "Disabled — AMD V-Cache rule, not applicable to Intel.");
            break;

        case policy::CpuClass::MultiCCXNoX3D:
            write_rule(f, "IsolateGameFromBackground", true, 80u,
                "Pin game to CCX0; background tasks to CCX1+ (reduces migrations).");
            write_rule(f, "PinGameToVCacheCcd", false, 90u,
                "Disabled — no V-Cache on this CPU.");
            write_rule(f, "PinGameToPCores",    false, 85u,
                "Disabled — no Intel E-cores on this CPU.");
            break;

        case policy::CpuClass::SingleCCD:
        default:
            // All rules off — single homogeneous CCD, no benefit expected.
            std::fprintf(f,
                "# No affinity rules enabled.\n"
                "# Your CPU has a single homogeneous CCD; core pinning provides\n"
                "# less than 2%% improvement on this topology. Ayama will monitor\n"
                "# but not change process affinity by default.\n"
                "#\n"
                "# If you want to test manual rules, switch to Manual mode and\n"
                "# add custom [[rule]] entries below.\n\n");
            break;
        }
    }

    static void write_rule(std::FILE* f,
                           const char* name,
                           bool        enabled,
                           uint8_t     confidence,
                           const char* comment) noexcept
    {
        std::fprintf(f,
            "# %s\n"
            "[[rule]]\n"
            "name       = \"%s\"\n"
            "enabled    = %s\n"
            "confidence = %u\n"
            "core_mask  = 0\n\n",
            comment, name, enabled ? "true" : "false",
            static_cast<unsigned>(confidence));
    }

    // ── AgentConfig population ────────────────────────────────────────────────

    static void fill_overrides(AgentConfig& cfg,
                               policy::CpuClass cpu_class) noexcept
    {
        auto add = [&](const char* name, bool enabled, uint8_t conf) {
            if (cfg.n_overrides >= AgentConfig::kMaxRules) return;
            PolicyOverride& o = cfg.overrides[cfg.n_overrides++];
            std::strncpy(o.name, name, sizeof(o.name) - 1u);
            o.enabled    = enabled;
            o.confidence = conf;
            o.core_mask  = 0ull;
        };

        switch (cpu_class) {
        case policy::CpuClass::X3DSingle:
            add("PinGameToVCacheCcd",       true,  90u);
            break;
        case policy::CpuClass::X3DDual:
            add("PinGameToVCacheCcd",       true,  90u);
            add("EvictStreamFromHotCcd",    true,  80u);
            break;
        case policy::CpuClass::HybridIntel:
            add("PinGameToPCores",          true,  85u);
            add("PinCommToECores",          true,  75u);
            break;
        case policy::CpuClass::MultiCCXNoX3D:
            add("IsolateGameFromBackground", true,  80u);
            break;
        case policy::CpuClass::SingleCCD:
        case policy::CpuClass::Unknown:
        default:
            // No overrides — all rules disabled by default.
            break;
        }
    }
};

} // namespace ayama::config
// Made with my soul - Swately <3
