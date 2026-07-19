// config/tests/config_test.cpp
// Use-modes config tests (W2 persistence + W3 matching).
//
// Covers §5 tests 1 & 2 of PHYNNED_USE_MODES_IMPLEMENTATION_STRATEGIES:
//   1. v2 round-trip (save → load, fields byte-equal) + v1 fixture back-compat (R2).
//   2. process-rule matching — name-only, name+path, case-insensitivity.
//
// Hand-rolled asserts, no framework (matches the existing test style).
//

#include <phynned/config/ConfigStore.hpp>

#include <cassert>
#include <cstdio>
#include <cstring>

using namespace phynned::config;

static const char* kV2Path = "test_use_modes_v2.toml";
static const char* kV1Path = "test_use_modes_v1.toml";

int main() {
    // ── Test 1: v2 round-trip ─────────────────────────────────────────────
    {
        AgentConfig cfg{};
        cfg.op_mode                = OpMode::Manual;
        cfg.profile                = Profile::Games;
        cfg.corral_enabled         = true;
        cfg.corral_keep_on_disable = true;

        // One policy override (proves the existing [[rule]] path still saves).
        cfg.n_overrides = 1u;
        std::snprintf(cfg.overrides[0].name, sizeof(cfg.overrides[0].name),
                      "PinGameToVCacheCcd");
        cfg.overrides[0].enabled    = true;
        cfg.overrides[0].confidence = 77u;
        cfg.overrides[0].core_mask  = 0xF0ull;

        // Two user process rules: name-only Never + name+path VCache.
        cfg.n_process_rules = 2u;
        std::snprintf(cfg.process_rules[0].name, 64, "handbrake.exe");
        cfg.process_rules[0].path[0] = '\0';
        cfg.process_rules[0].action  = static_cast<uint8_t>(RuleAction::Never);
        std::snprintf(cfg.process_rules[1].name, 64, "game.exe");
        std::snprintf(cfg.process_rules[1].path, 260, "C:\\Games\\game.exe");
        cfg.process_rules[1].action  = static_cast<uint8_t>(RuleAction::VCache);

        auto sr = ConfigStore::save_policies(cfg, kV2Path);
        assert(sr.has_value());

        auto lr = ConfigStore::load_policies(kV2Path);
        assert(lr.has_value());
        const AgentConfig& got = *lr;

        assert(got.version == 2u);                       // save always writes v2
        assert(got.op_mode == OpMode::Manual);
        assert(got.profile == Profile::Games);
        assert(got.corral_enabled         == true);
        assert(got.corral_keep_on_disable == true);

        assert(got.n_overrides == 1u);
        assert(std::strcmp(got.overrides[0].name, "PinGameToVCacheCcd") == 0);
        assert(got.overrides[0].enabled    == true);
        assert(got.overrides[0].confidence == 77u);
        assert(got.overrides[0].core_mask  == 0xF0ull);

        assert(got.n_process_rules == 2u);
        assert(std::strcmp(got.process_rules[0].name, "handbrake.exe") == 0);
        assert(got.process_rules[0].path[0] == '\0');
        assert(got.process_rules[0].action == static_cast<uint8_t>(RuleAction::Never));
        assert(std::strcmp(got.process_rules[1].name, "game.exe") == 0);
        assert(std::strcmp(got.process_rules[1].path, "C:\\Games\\game.exe") == 0);
        assert(got.process_rules[1].action == static_cast<uint8_t>(RuleAction::VCache));

        std::remove(kV2Path);
        std::printf("[OK] v2 round-trip: all fields preserved\n");
    }

    // ── Test 1b: v1 fixture back-compat (R2) ──────────────────────────────
    {
        // A literal pre-use-modes (v1) file: no profile / [corral] / [[process]].
        std::FILE* f = std::fopen(kV1Path, "w");
        assert(f != nullptr);
        std::fprintf(f,
            "# legacy v1 config\n"
            "version = 1\n"
            "op_mode = \"auto\"\n"
            "\n"
            "[[rule]]\n"
            "name       = \"PinGameToVCacheCcd\"\n"
            "enabled    = true\n"
            "confidence = 90\n"
            "core_mask  = 0\n");
        std::fclose(f);

        auto lr = ConfigStore::load_policies(kV1Path);
        assert(lr.has_value());
        const AgentConfig& got = *lr;

        // v1 parses; every new field takes its default (byte-identical behaviour).
        assert(got.version == 1u);
        assert(got.op_mode == OpMode::Auto);
        assert(got.profile == Profile::GamesCorral);   // default
        assert(got.corral_enabled          == false);  // default
        assert(got.corral_keep_on_disable  == false);  // default
        assert(got.n_process_rules         == 0u);     // default
        // The legacy rule still loads.
        assert(got.n_overrides == 1u);
        assert(std::strcmp(got.overrides[0].name, "PinGameToVCacheCcd") == 0);
        assert(got.overrides[0].confidence == 90u);

        std::remove(kV1Path);
        std::printf("[OK] v1 back-compat: legacy file loads, new fields defaulted\n");
    }

    // ── Test 2: process-rule matching ─────────────────────────────────────
    {
        AgentConfig cfg{};
        cfg.n_process_rules = 2u;
        std::snprintf(cfg.process_rules[0].name, 64, "handbrake.exe");
        cfg.process_rules[0].path[0] = '\0';                  // matches any path
        cfg.process_rules[0].action  = static_cast<uint8_t>(RuleAction::Freq);
        std::snprintf(cfg.process_rules[1].name, 64, "game.exe");
        std::snprintf(cfg.process_rules[1].path, 260, "C:\\A\\game.exe");
        cfg.process_rules[1].action  = static_cast<uint8_t>(RuleAction::VCache);

        // Name-only rule: matches by name regardless of path.
        assert(ConfigStore::find_process_rule(cfg, "handbrake.exe", nullptr)
               == &cfg.process_rules[0]);
        // Case-insensitive name compare.
        assert(ConfigStore::find_process_rule(cfg, "HANDBRAKE.EXE", nullptr)
               == &cfg.process_rules[0]);
        assert(ConfigStore::find_process_rule(cfg, "handbrake.exe",
                                              "D:\\anywhere\\handbrake.exe")
               == &cfg.process_rules[0]);

        // Rule with a path: name alone (no path supplied) does NOT match.
        assert(ConfigStore::find_process_rule(cfg, "game.exe", nullptr)
               == nullptr);
        // Full path matches (exact).
        assert(ConfigStore::find_process_rule(cfg, "game.exe", "C:\\A\\game.exe")
               == &cfg.process_rules[1]);
        // Full path matches (case-insensitive).
        assert(ConfigStore::find_process_rule(cfg, "game.exe", "c:\\a\\GAME.exe")
               == &cfg.process_rules[1]);
        // Path mismatch → no match.
        assert(ConfigStore::find_process_rule(cfg, "game.exe", "C:\\B\\game.exe")
               == nullptr);
        // Unknown name → no match.
        assert(ConfigStore::find_process_rule(cfg, "notfound.exe", nullptr)
               == nullptr);

        std::printf("[OK] matching: name-only, name+path, case-insensitive\n");
    }

    std::printf("\n[PASS] config_test\n");
    return 0;
}
// Made with my soul - Swately <3
