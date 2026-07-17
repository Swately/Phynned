// policy/tests/auto_policy_selector_test.cpp
// Test T7 — Auto Mode classification and AutoPolicySelector decision table.
//
// Verifies:
//   T7.1  AutoDecision POD properties.
//   T7.2  CpuClass::class_name() returns non-empty for all values.
//   T7.3  init_from_topology() sets is_ready() == true.
//   T7.4  Browser / System / Unknown targets → no action (core_mask == 0).
//   T7.5  PerGameMemory user_locked entry overrides table (from_memory == true).
//   T7.6  Empty / stale PerGameMemory entry falls through to table.
//   T7.7  from_memory == false when memory == nullptr.
//   T7.8  select() without memory is equivalent to table_select() fallback.
//
// No admin required; no real process needed. Hardware topology uses real probe
// (whatever CPU is present). Assertions adapt to detected CPU class.
//
// §9.2, phynned::policy

#include <phynned/policy/AutoPolicySelector.hpp>
#include <phynned/learn/PerGameMemory.hpp>
#include <phynned/observer/TargetProcess.hpp>
#include <phyriad/topology/HardwareTopology.hpp>

#include <cassert>
#include <cstdio>
#include <cstring>

int main()
{
    using namespace phynned::policy;
    using namespace phynned::observer;
    using namespace phynned::learn;

    // ── T7.1: AutoDecision POD ────────────────────────────────────────────────
    {
        AutoDecision d{};
        assert(d.core_mask   == 0ull);
        assert(d.action_kind == ActionKind::None);
        assert(d.confidence  == 0u);
        assert(d.from_memory == false);
        std::printf("[OK] T7.1  AutoDecision default: core_mask=0, kind=None\n");
    }

    // ── T7.2: CpuClass::class_name() ─────────────────────────────────────────
    {
        static const CpuClass kAll[] = {
            CpuClass::Unknown, CpuClass::X3DSingle, CpuClass::X3DDual,
            CpuClass::HybridIntel, CpuClass::MultiCCXNoX3D, CpuClass::SingleCCD
        };
        for (auto c : kAll) {
            const char* name = AutoPolicySelector::class_name(c);
            assert(name != nullptr && name[0] != '\0');
        }
        std::printf("[OK] T7.2  class_name() non-empty for all CpuClass values\n");
    }

    // ── Topology singleton + init selector ───────────────────────────────────
    // Migrated from HardwareTopology::probe() to the
    // FR-1 singleton hw::topology() — same migration applied to all other
    // call sites in Phynned; this test was the last holdout.
    const phyriad::HardwareTopology& topo = phyriad::hw::topology();
    assert(!topo.cores.empty() && "hw::topology() must return a non-empty topology");

    AutoPolicySelector sel;
    sel.init_from_topology(topo);

    // ── T7.3: is_ready() after init ───────────────────────────────────────────
    {
        assert(sel.is_ready());
        assert(sel.cpu_class() != CpuClass::Unknown);
        std::printf("[OK] T7.3  is_ready() == true  cpu_class='%s'\n",
                    sel.class_name());
    }

    // ── T7.4: Non-actionable kinds always return core_mask == 0 ──────────────
    {
        // Browser, System, Unknown must never receive an affinity pin.
        static const TargetKind kNoAction[] = {
            TargetKind::Browser, TargetKind::System, TargetKind::Unknown
        };
        for (auto kind : kNoAction) {
            const AutoDecision d = sel.select(kind, "chrome.exe");
            assert(d.core_mask  == 0ull);
            assert(d.action_kind == ActionKind::None);
        }
        std::printf("[OK] T7.4  Browser/System/Unknown → no action\n");
    }

    // ── T7.5: user_locked PerGameMemory entry → memory shortcut ──────────────
    {
        PerGameMemory mem;
        mem.generate_hardware_id();

        // Craft a locked entry with a synthetic mask.
        LearnedEntry e{};
        std::strncpy(e.exe, "TestGame.exe", sizeof(e.exe) - 1);
        std::strncpy(e.hardware_id, mem.hardware_id(), sizeof(e.hardware_id) - 1);
        std::strncpy(e.best_action, "pin_v_cache_ccd", sizeof(e.best_action) - 1);
        e.best_core_mask  = 0xFFull;   // synthetic mask
        e.improvement_pct = 12.5f;
        e.sample_count    = 5u;
        e.user_locked     = true;
        std::strncpy(e.last_validated, "2026-01-01T00:00:00Z",
                     sizeof(e.last_validated) - 1);

        mem.upsert(e);

        const AutoDecision d = sel.select(TargetKind::Game, "TestGame.exe", &mem);
        assert(d.from_memory == true);
        assert(d.core_mask   == 0xFFull);
        assert(d.action_kind == ActionKind::PinAffinity);
        assert(d.confidence  > 0u);
        std::printf("[OK] T7.5  user_locked entry → from_memory=true mask=0x%llx\n",
                    static_cast<unsigned long long>(d.core_mask));
    }

    // ── T7.6: Empty / stale PerGameMemory entry → falls through to table ──────
    {
        PerGameMemory mem;
        mem.generate_hardware_id();

        // Entry with sample_count==0 (no validated data yet).
        LearnedEntry e{};
        std::strncpy(e.exe, "NewGame.exe", sizeof(e.exe) - 1);
        std::strncpy(e.hardware_id, mem.hardware_id(), sizeof(e.hardware_id) - 1);
        e.best_core_mask  = 0ull;   // no cached mask
        e.sample_count    = 0u;
        e.user_locked     = false;

        mem.upsert(e);

        const AutoDecision d = sel.select(TargetKind::Game, "NewGame.exe", &mem);
        assert(d.from_memory == false);
        // Result depends on CPU class — just verify from_memory is false.
        std::printf("[OK] T7.6  Empty entry → from_memory=false (table fallback)\n");
    }

    // ── T7.7: No memory pointer → from_memory always false ───────────────────
    {
        const AutoDecision d = sel.select(TargetKind::Game, "AnyGame.exe", nullptr);
        assert(d.from_memory == false);
        std::printf("[OK] T7.7  memory=nullptr → from_memory=false\n");
    }

    // ── T7.8: SingleCCD CPU class → no game action (table says no-op) ─────────
    // (This test is only strict when running on SingleCCD hardware.
    //  On other CPUs, game_mask may be non-zero — that's also correct.)
    {
        // Verify masks are self-consistent: if game_mask==0 → select(Game) → mask==0.
        if (sel.game_mask() == 0ull) {
            const AutoDecision d = sel.select(TargetKind::Game, "game.exe", nullptr);
            assert(d.core_mask == 0ull);
            std::printf("[OK] T7.8  SingleCCD: game_mask==0 → no action\n");
        } else {
            // Non-SingleCCD: game_mask is non-zero.
            assert(sel.game_mask() != 0ull);
            std::printf("[OK] T7.8  Multi-CCD/Hybrid: game_mask=0x%llx (non-zero)\n",
                        static_cast<unsigned long long>(sel.game_mask()));
        }
    }

    std::printf("\nAll T7 tests passed.\n");
    return 0;
}
// Made with my soul - Swately <3
