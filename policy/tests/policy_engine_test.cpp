// policy/tests/policy_engine_test.cpp
// Test: PolicyEngine rule evaluation.
//
// Verifies:
//   - Zero decisions with no rules.
//   - Zero decisions when conditions don't match.
//   - Correct decision emitted when conditions match.
//   - Rule enable/disable.
//

#include <phynned/policy/PolicyEngine.hpp>
#include <phynned/policy/Rule.hpp>
#include <phynned/observer/TargetProcess.hpp>
#include <phynned/observer/TargetMetrics.hpp>

#include <cassert>
#include <cstdio>
#include <cstring>

int main() {
    using namespace phynned::policy;
    using namespace phynned::observer;

    // ── Test 1: PolicyDecision POD ────────────────────────────────────────
    {
        static_assert(sizeof(PolicyDecision) == 32u);
        std::printf("[OK] PolicyDecision is 32B\n");
    }

    // ── Test 2: Empty engine → no decisions ───────────────────────────────
    {
        PolicyEngine eng;

        TargetProcess tp{};
        tp.pid  = 100u;
        tp.kind = TargetKind::Game;

        TargetMetrics tm{};
        tm.pid = 100u;
        tm.migrations_per_sec = 100u;

        PolicyDecision decisions[kMaxDecisionsPerCycle];
        const uint32_t n = eng.evaluate(&tp, 1u, &tm, 1u, decisions);
        assert(n == 0u);
        std::printf("[OK] No decisions from empty engine\n");
    }

    // ── Test 3: Manual rule registration and evaluation ───────────────────
    {
        PolicyEngine eng;

        // Add a rule: if target is Stream, pin to mask 0xFF00
        Rule r = Rule::make(99u, "TestPinStream",
                            ActionKind::PinAffinity, 0xFF00ull, 70u);
        r.conditions[r.n_conditions++] = Condition{
            ConditionField::TargetKind, ConditionOp::Equal,
            {}, static_cast<uint32_t>(TargetKind::Stream), 0.f, {}};

        const bool ok = eng.register_rule(r);
        assert(ok);

        // Target is Game → should not match
        TargetProcess tp_game{};
        tp_game.pid  = 200u;
        tp_game.kind = TargetKind::Game;
        TargetMetrics tm_game{};
        tm_game.pid = 200u;

        PolicyDecision decisions[kMaxDecisionsPerCycle];
        uint32_t n = eng.evaluate(&tp_game, 1u, &tm_game, 1u, decisions);
        assert(n == 0u);
        std::printf("[OK] Game target does not match Stream rule\n");

        // Target is Stream → should match
        TargetProcess tp_stream{};
        tp_stream.pid  = 300u;
        tp_stream.kind = TargetKind::Stream;
        TargetMetrics tm_stream{};
        tm_stream.pid = 300u;

        n = eng.evaluate(&tp_stream, 1u, &tm_stream, 1u, decisions);
        assert(n == 1u);
        assert(decisions[0].target_pid  == 300u);
        assert(decisions[0].rule_id     == 99u);
        assert(decisions[0].core_mask   == 0xFF00ull);
        assert(decisions[0].action_kind == ActionKind::PinAffinity);
        std::printf("[OK] Stream target matches and emits correct decision\n");

        // Disable rule → no match
        eng.disable_rule(99u);
        n = eng.evaluate(&tp_stream, 1u, &tm_stream, 1u, decisions);
        assert(n == 0u);
        std::printf("[OK] Disabled rule produces no decisions\n");

        // Re-enable → matches again
        eng.enable_rule(99u);
        n = eng.evaluate(&tp_stream, 1u, &tm_stream, 1u, decisions);
        assert(n == 1u);
        std::printf("[OK] Re-enabled rule matches again\n");
    }

    // ── Test 4: Multiple targets ───────────────────────────────────────────
    {
        PolicyEngine eng;

        Rule r = Rule::make(1u, "PinAnyGame",
                            ActionKind::PinAffinity, 0xFFull, 80u);
        r.conditions[r.n_conditions++] = Condition{
            ConditionField::TargetKind, ConditionOp::Equal,
            {}, static_cast<uint32_t>(TargetKind::Game), 0.f, {}};
        eng.register_rule(r);

        TargetProcess tps[3]{};
        tps[0].pid = 1u; tps[0].kind = TargetKind::Game;
        tps[1].pid = 2u; tps[1].kind = TargetKind::Comm;
        tps[2].pid = 3u; tps[2].kind = TargetKind::Game;

        TargetMetrics tms[3]{};
        tms[0].pid = 1u;
        tms[1].pid = 2u;
        tms[2].pid = 3u;

        PolicyDecision decisions[kMaxDecisionsPerCycle];
        const uint32_t n = eng.evaluate(tps, 3u, tms, 3u, decisions);
        assert(n == 2u);  // 2 games, 1 comm
        assert(decisions[0].target_pid == 1u || decisions[0].target_pid == 3u);
        std::printf("[OK] Multiple targets: %u decisions for 2 games\n", n);
    }

    std::printf("\n[PASS] policy_engine_test\n");
    return 0;
}
// Made with my soul - Swately <3
