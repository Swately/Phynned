// action/tests/selective_revert_test.cpp
// §5 test 3 (R3): selective revert by rule_id — a mixed active set of a
// corral placement (rule_id == kCorralRuleId == 20) and a game placement
// (rule_id == 1) reverts ONLY the corral action; the game pin is untouched.
//
// Windows-only: spawns two benign child processes (ping) and pins each. Setting
// affinity on our OWN children needs no admin. Skipped gracefully on non-Windows
// or if a spawn fails.
//

#include <phynned/action/ActionExecutor.hpp>
#include <phynned/policy/PolicyDecision.hpp>

#include <cassert>
#include <cstdio>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#endif

// The corral rule-id, mirrored from AgentRuntime.cpp (kept in sync by contract).
static constexpr uint32_t kCorralRuleId = 20u;
static constexpr uint32_t kGameRuleId   = 1u;

int main() {
    using namespace phynned::action;
    using namespace phynned::policy;

#ifdef _WIN32
    auto spawn = [](PROCESS_INFORMATION& pi) -> bool {
        STARTUPINFOW si{}; si.cb = sizeof(si);
        wchar_t cmd[] = L"ping -n 30 127.0.0.1";
        return CreateProcessW(nullptr, cmd, nullptr, nullptr, FALSE,
                              CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi) != 0;
    };

    PROCESS_INFORMATION pi_corral{};
    PROCESS_INFORMATION pi_game{};
    const bool s1 = spawn(pi_corral);
    const bool s2 = spawn(pi_game);
    if (!s1 || !s2) {
        std::printf("[SKIP] selective_revert_test: could not spawn children\n");
        if (s1) { TerminateProcess(pi_corral.hProcess, 0u); CloseHandle(pi_corral.hThread); CloseHandle(pi_corral.hProcess); }
        if (s2) { TerminateProcess(pi_game.hProcess, 0u);   CloseHandle(pi_game.hThread);   CloseHandle(pi_game.hProcess); }
        std::printf("\n[PASS] selective_revert_test (skipped)\n");
        return 0;
    }

    const uint32_t corral_pid = static_cast<uint32_t>(pi_corral.dwProcessId);
    const uint32_t game_pid   = static_cast<uint32_t>(pi_game.dwProcessId);

    ActionExecutor exec;

    // Apply a corral placement (rule 20) and a game placement (rule 1).
    {
        PolicyDecision d{};
        d.action_kind = ActionKind::PinAffinity;
        d.core_mask   = 0x3ull;         // cores 0-1
        d.target_pid  = corral_pid;
        d.rule_id     = kCorralRuleId;
        const auto r = exec.apply(d, "ping.exe", 0ull);
        assert(r.has_value());
    }
    {
        PolicyDecision d{};
        d.action_kind = ActionKind::PinAffinity;
        d.core_mask   = 0x3ull;
        d.target_pid  = game_pid;
        d.rule_id     = kGameRuleId;
        const auto r = exec.apply(d, "ping.exe", 0ull);
        assert(r.has_value());
    }

    assert(exec.active_count() == 2u);
    assert(exec.active_applied_mask(corral_pid) == 0x3ull);
    assert(exec.active_applied_mask(game_pid)   == 0x3ull);
    std::printf("[OK] applied 2 actions (corral rule=%u, game rule=%u)\n",
                kCorralRuleId, kGameRuleId);

    // Selective revert: ONLY the corral action.
    const uint32_t reverted = exec.revert_by_rule_id(kCorralRuleId);
    assert(reverted == 1u);
    assert(exec.active_count() == 1u);
    // Corral action gone; game pin still active.
    assert(exec.active_applied_mask(corral_pid) == 0ull);
    assert(exec.active_applied_mask(game_pid)   == 0x3ull);
    std::printf("[OK] revert_by_rule_id(%u) reverted only the corral action; "
                "game pin intact\n", kCorralRuleId);

    // Reverting a rule-id with no matching action is a safe no-op.
    const uint32_t none = exec.revert_by_rule_id(kCorralRuleId);
    assert(none == 0u);
    assert(exec.active_count() == 1u);
    std::printf("[OK] repeat selective revert is a no-op\n");

    // ── exe-filter variant (user-rule removal path) ───────────────────────
    // Two user pins (rule 21) on different exe names; the filtered revert
    // undoes ONLY the matching exe (case-insensitively) and leaves the other.
    static constexpr uint32_t kUserRuleId = 21u;
    {
        // One user pin suffices: filter MISMATCH is proven against this entry,
        // filter MATCH against the same entry, and "others intact" against the
        // still-active game pin (one action per pid — can't stack a second
        // user pin on game_pid).
        PolicyDecision d{};
        d.action_kind = ActionKind::PinAffinity;
        d.core_mask   = 0x3ull;
        d.target_pid  = corral_pid;      // reuse the already-reverted child
        d.rule_id     = kUserRuleId;
        const auto r = exec.apply(d, "alpha.exe", 0ull);
        assert(r.has_value());
    }
    assert(exec.active_count() == 2u);   // game pin + alpha user pin
    // Filter that matches nothing → no-op.
    assert(exec.revert_by_rule_id(kUserRuleId, "beta.exe") == 0u);
    assert(exec.active_count() == 2u);
    // Case-insensitive match → reverts exactly the alpha pin.
    assert(exec.revert_by_rule_id(kUserRuleId, "ALPHA.EXE") == 1u);
    assert(exec.active_count() == 1u);
    assert(exec.active_applied_mask(corral_pid) == 0ull);
    assert(exec.active_applied_mask(game_pid)   == 0x3ull);
    std::printf("[OK] exe-filtered revert: wrong exe no-op, ci-match reverts, "
                "others intact\n");

    // Cleanup.
    exec.revert_all();
    TerminateProcess(pi_corral.hProcess, 0u);
    TerminateProcess(pi_game.hProcess, 0u);
    CloseHandle(pi_corral.hThread); CloseHandle(pi_corral.hProcess);
    CloseHandle(pi_game.hThread);   CloseHandle(pi_game.hProcess);
#else
    std::printf("[SKIP] selective_revert_test: Windows-only\n");
#endif

    std::printf("\n[PASS] selective_revert_test\n");
    return 0;
}
// Made with my soul - Swately <3
