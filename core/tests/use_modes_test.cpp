// core/tests/use_modes_test.cpp
// §5 tests 4 & 5: the two use-modes SAFETY invariants, exercised on the pure
// evaluate_user_pin decision + the real AcProbe gate.
//
//   Test 4 (R1) — a user "Always" pin on a Game routes through the SAME §6 AC
//                 probe/oracle gate as automatic placement, and a do-not-probe
//                 (class-c) verdict → zero apply.
//   Test 5 (R4) — a user pin on a process already pinned by another manager
//                 (foreign, non-full mask) is skipped with the flap-warn flag.
//
// Pure, no live agent, no admin. evaluate_user_pin is a value function; AcProbe's
// refused path opens ZERO handles (class-c short-circuits before OpenProcess).
//

#include <phynned/core/UserPinRule.hpp>
#include <phynned/core/UserRuleLookup.hpp>
#include <phynned/action/AcProbe.hpp>
#include <phynned/observer/AcDriverOracle.hpp>
#include <phynned/observer/TargetProcess.hpp>
#include <phynned/observer/TargetMetrics.hpp>
#include <phynned/learn/PerGameMemory.hpp>

#include <cassert>
#include <cstdio>
#include <cstring>

using phynned::core::evaluate_user_pin;
using phynned::core::UserPinDecision;
using phynned::observer::TargetKind;
using phynned::observer::TargetMetrics;

// The 7950X3D dual-CCD split used throughout Phynned.
static constexpr uint32_t kVCache = 0x0000FFFFu;
static constexpr uint32_t kSystem = 0xFFFFFFFFu;
static constexpr uint64_t kFreq   = 0xFFFF0000ull;

// ProcessRule::action values.
static constexpr uint8_t kFreqAct   = 1u;
static constexpr uint8_t kVCacheAct = 2u;

int main() {
    // ── Test 5 (R4): flap guard ───────────────────────────────────────────
    {
        // Process is on a foreign, restricted mask (0x3) and we hold no action of
        // our own → another manager owns it → skip + flap_warn, never fight.
        TargetMetrics tm{};
        tm.current_core_mask = 0x3u;                 // foreign, non-full
        const UserPinDecision d = evaluate_user_pin(
            kFreqAct, TargetKind::Comm, tm, /*our_active_mask=*/0ull,
            kVCache, kSystem, kFreq);
        assert(d.flap_warn == true);
        assert(d.would_pin == false);
        std::printf("[OK] R4 flap guard: foreign mask -> skip + flap_warn\n");

        // Clean case: process is on the full system mask, we hold nothing →
        // no foreign manager → would_pin, no flap.
        TargetMetrics tm2{};
        tm2.current_core_mask = kSystem;
        const UserPinDecision d2 = evaluate_user_pin(
            kFreqAct, TargetKind::Comm, tm2, /*our_active_mask=*/0ull,
            kVCache, kSystem, kFreq);
        assert(d2.would_pin == true);
        assert(d2.flap_warn == false);
        assert(d2.needs_ac_gate == false);           // Comm is not a game
        assert(d2.target_mask == kFreq);
        std::printf("[OK] R4 clean: full-system mask -> would_pin, no flap\n");

        // Idempotent: we already hold the target mask → no-op (skip re-probe).
        TargetMetrics tm3{};
        tm3.current_core_mask = kVCache;
        const UserPinDecision d3 = evaluate_user_pin(
            kVCacheAct, TargetKind::Comm, tm3,
            /*our_active_mask=*/static_cast<uint64_t>(kVCache),
            kVCache, kSystem, kFreq);
        assert(d3.already_placed == true);
        assert(d3.would_pin == false);
        std::printf("[OK] idempotent: already-placed by our rule -> no-op\n");
    }

    // ── Test 4 (R1): AC gate routing + do-not-probe → zero apply ───────────
    {
        // A Game (or unknown-class) user pin MUST route through the AC gate.
        TargetMetrics tm{};
        tm.current_core_mask = kSystem;
        const UserPinDecision game = evaluate_user_pin(
            kVCacheAct, TargetKind::Game, tm, /*our_active_mask=*/0ull,
            kVCache, kSystem, kFreq);
        assert(game.would_pin == true);
        assert(game.needs_ac_gate == true);          // Game → AC gate required
        const UserPinDecision unknown = evaluate_user_pin(
            kVCacheAct, TargetKind::Unknown, tm, 0ull, kVCache, kSystem, kFreq);
        assert(unknown.needs_ac_gate == true);       // unknown class → AC gate too
        // A known non-game kind does NOT force the gate.
        const UserPinDecision comm = evaluate_user_pin(
            kVCacheAct, TargetKind::Comm, tm, 0ull, kVCache, kSystem, kFreq);
        assert(comm.needs_ac_gate == false);
        std::printf("[OK] R1 routing: Game/Unknown -> AC gate; Comm -> no gate\n");

        // The gate itself: a class-c (SilentPunish_C, do-not-probe) title is
        // Refused, opening ZERO handles. "cod.exe" is a known class-c title.
        phynned::observer::AcDriverOracle oracle;
        assert(phynned::observer::AcDriverOracle::probe_allowed(
                   phynned::observer::AcDriverOracle::classify_title("cod.exe"))
               == false);

        phynned::learn::PerGameMemory mem;
        mem.generate_hardware_id();
        const auto pr = phynned::action::AcProbe::probe_and_label(
            /*pid=*/0xFFFFFFu, "cod.exe", oracle, mem, /*audit=*/nullptr);
        assert(pr == phynned::action::ProbeResult::Refused_DoNotProbe);

        // The user-pin pass's exact skip condition → NO apply for this game.
        const bool would_apply =
            !(pr == phynned::action::ProbeResult::Refused_DoNotProbe ||
              pr == phynned::action::ProbeResult::Blocked            ||
              pr == phynned::action::ProbeResult::AlreadyLabeledBlocked);
        assert(would_apply == false);
        std::printf("[OK] R1 gate: class-c game -> Refused (zero handles) -> "
                    "zero apply\n");
    }

    // ── Test 6: path-carrying rules resolve LAZILY at runtime ─────────────
    // A rule with a non-empty path must match a LIVE process (this test's own
    // pid — a real, running process with a knowable image path), and the AC
    // zero-handle discipline must veto resolution for do-not-probe titles.
#ifdef _WIN32
    {
        using phynned::config::AgentConfig;
        using phynned::config::RuleAction;
        using phynned::core::find_user_rule_for;

        char self_path[260]{};
        const DWORD n = GetModuleFileNameA(nullptr, self_path,
                                           sizeof(self_path));
        assert(n > 0u && n < sizeof(self_path));
        const char* self_base = self_path;
        for (const char* p = self_path; *p; ++p)
            if (*p == '\\' || *p == '/') self_base = p + 1;
        const uint32_t self_pid =
            static_cast<uint32_t>(GetCurrentProcessId());

        // (a) Path rule with OUR real path → resolves and matches.
        AgentConfig cfg{};
        cfg.n_process_rules = 1u;
        std::strncpy(cfg.process_rules[0].name, self_base,
                     sizeof(cfg.process_rules[0].name) - 1u);
        std::strncpy(cfg.process_rules[0].path, self_path,
                     sizeof(cfg.process_rules[0].path) - 1u);
        cfg.process_rules[0].action =
            static_cast<uint8_t>(RuleAction::Never);
        assert(find_user_rule_for(cfg, self_base, self_pid)
               == &cfg.process_rules[0]);
        std::printf("[OK] path rule: live pid resolves + matches (veto holds)\n");

        // (b) Same rule, WRONG path → resolves but does not match.
        cfg.process_rules[0].path[0] = 'Z';   // corrupt the drive letter
        assert(find_user_rule_for(cfg, self_base, self_pid) == nullptr);
        std::printf("[OK] path rule: wrong path -> no match (fail-safe)\n");

        // (c) AC guard: a do-not-probe title (class-c "cod.exe") never gets a
        // handle — even when the rule's path WOULD match this live pid. If the
        // guard were broken, resolution would return our path and match.
        AgentConfig cfg2{};
        cfg2.n_process_rules = 1u;
        std::strncpy(cfg2.process_rules[0].name, "cod.exe",
                     sizeof(cfg2.process_rules[0].name) - 1u);
        std::strncpy(cfg2.process_rules[0].path, self_path,
                     sizeof(cfg2.process_rules[0].path) - 1u);
        cfg2.process_rules[0].action =
            static_cast<uint8_t>(RuleAction::Freq);
        assert(find_user_rule_for(cfg2, "cod.exe", self_pid) == nullptr);
        std::printf("[OK] path rule: do-not-probe title -> zero-handle veto "
                    "wins over user rule\n");
    }
#endif

    std::printf("\n[PASS] use_modes_test\n");
    return 0;
}
// Made with my soul - Swately <3
