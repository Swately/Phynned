// action/tests/action_executor_test.cpp
// Test: ActionExecutor apply + revert logic.
//
// Verifies:
//   - ActionLogEntry POD size.
//   - apply() with Revert kind is a no-op when nothing active.
//   - revert_all() is idempotent.
//   - Log ring push/drain.
//
// NOTE: apply(PinAffinity) for real requires admin + a running target process.
//       Integration tests for that run as part of T5 in the live environment.
//

#include <phynned/action/ActionExecutor.hpp>
#include <phynned/action/ActionLog.hpp>
#include <phynned/policy/PolicyDecision.hpp>

#include <cassert>
#include <cstdio>

int main() {
    using namespace phynned::action;
    using namespace phynned::policy;

    // ── Test 1: ActionLogEntry POD ────────────────────────────────────────
    {
        static_assert(sizeof(ActionLogEntry) == 56u, "ActionLogEntry must be 56B");
        static_assert(__is_trivially_copyable(ActionLogEntry));
        std::printf("[OK] ActionLogEntry is 56B trivially copyable\n");
    }

    // ── Test 2: ActionLogRing push + multi-reader cursor drain ───────────
    // ActionLogRing is now phyriad::ipc::Ring<ActionLogEntry, 128> (FR-12 migration).
    // Producers use push_unchecked() (circular/overwrite); consumers use the
    // external-cursor API write_cursor() + peek_at() to avoid touching the
    // SPSC read_seq_ (which would interfere with the other consumer).
    {
        ActionLogRing ring;
        ActionLogEntry e1{};
        e1.target_pid = 42u;
        e1.success    = 1u;
        (void)ring.push_unchecked(e1);

        ActionLogEntry out[4]{};
        uint64_t cursor = 0ull;
        const uint64_t head = ring.write_cursor();
        uint32_t n = 0u;
        while (cursor < head && n < 4u) {
            ActionLogEntry e{};
            if (ring.peek_at(cursor, e)) out[n++] = e;
            ++cursor;
        }
        assert(n == 1u);
        assert(out[0].target_pid == 42u);
        assert(out[0].success    == 1u);
        std::printf("[OK] ActionLogRing push_unchecked + peek_at works\n");
    }

    // ── Test 3: ActionExecutor construction ───────────────────────────────
    {
        ActionExecutor exec;
        assert(exec.active_count() == 0u);
        std::printf("[OK] ActionExecutor default construction\n");
    }

    // ── Test 4: revert_all() on empty executor ────────────────────────────
    {
        ActionExecutor exec;
        exec.revert_all();  // must not crash
        assert(exec.active_count() == 0u);
        std::printf("[OK] revert_all() on empty executor is safe\n");
    }

    // ── Test 5: apply(Revert) with no active action ───────────────────────
    {
        ActionExecutor exec;
        PolicyDecision d{};
        d.target_pid  = 9999u;
        d.action_kind = ActionKind::Revert;
        const auto r = exec.apply(d);
        assert(r.has_value());  // revert no-op is OK
        assert(exec.active_count() == 0u);
        std::printf("[OK] apply(Revert) with no active action is no-op\n");
    }

    // ── Test 6: snapshot_log returns empty initially ───────────────────────
    {
        ActionExecutor exec;
        ActionLogEntry buf[8];
        const uint32_t n = exec.snapshot_log(buf, 8u);
        assert(n == 0u);
        std::printf("[OK] snapshot_log() empty on new executor\n");
    }

    std::printf("\n[PASS] action_executor_test\n");
    return 0;
}
// Made with my soul - Swately <3
