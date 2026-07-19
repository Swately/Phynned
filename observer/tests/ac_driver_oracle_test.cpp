// observer/tests/ac_driver_oracle_test.cpp
// Test: AcDriverOracle — zero-handle, GAME-SPECIFIC AC detection + class oracle (CR1).
//
// Does NOT require any game to be running (the whole point is zero handles).
// Verifies:
//   (1) the real zero-handle surface works on THIS box — enumerates loaded
//       kernel drivers + RUNNING services system-wide, prints the counts
//       (expect >100 drivers), AND proves the box-wide-fold bug is gone: with
//       no known game in the foreground and no active on-demand AC, the box
//       must classify as None (a dormant EA service / a resident vgk must NOT
//       force do-not-probe),
//   (2) the PRIMARY known-AC-title map: the operator's four games + a CoD + an
//       unknown non-AC title,
//   (3) the SECONDARY driver/service → class building block (synthetic inputs),
//   (4) probe_allowed() returns the correct bool per class.
//
// Returns nonzero on any failure.
//

#include <phynned/observer/AcDriverOracle.hpp>

#include <cstdio>
#include <string>
#include <vector>

using phynned::observer::AcClass;
using phynned::observer::AcDriverOracle;

static int g_failures = 0;

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            std::printf("[FAIL] %s:%d  %s\n", __FILE__, __LINE__, #cond);    \
            ++g_failures;                                                    \
        }                                                                    \
    } while (0)

int main() {
    AcDriverOracle oracle;

    // ── (1) Real zero-handle surface on THIS box ──────────────────────────────
    {
        std::vector<std::string> drivers;
        std::vector<std::string> services;
        const std::size_t nd = oracle.enumerate_drivers(drivers);
        const std::size_t ns = oracle.enumerate_services(services);  // RUNNING only

        std::printf("[INFO] zero-handle surface: %zu loaded drivers, %zu RUNNING "
                    "services (no game handle opened)\n", nd, ns);

        // The zero-handle surface must actually work: a live Windows box has
        // hundreds of loaded drivers. <100 means the enumeration is broken.
        CHECK(nd > 100u);

        // Live decision for an unknown foreground title on THIS box. Its value
        // is box-state-dependent (it is None iff no on-demand AC is actively
        // running right now), so we PRINT it and show WHY rather than hard-
        // asserting a value — the deterministic decide() tests in (3) prove the
        // bug fixes box-independently. We DO assert internal consistency: the
        // live path must equal decide() over the same live enumeration.
        const AcClass here = oracle.classify_foreground_game("some_unknown_game.exe");
        CHECK(here == AcDriverOracle::decide("some_unknown_game.exe", drivers, services));
        std::printf("[INFO] this box, unknown foreground title -> %s (probe_allowed=%s)\n",
                    AcDriverOracle::to_string(here),
                    AcDriverOracle::probe_allowed(here) ? "true" : "false");
        // AC-related names the oracle sees on this box (for transparency). NOTE:
        // listing != escalating — an always-on driver like vgk.sys is shown but
        // does NOT trigger the secondary (decide() skips always-on AC, BUG 2).
        std::printf("[INFO] AC-related names visible to the oracle (class shown):\n");
        bool any = false;
        for (const auto& n : services) {
            const AcClass c = AcDriverOracle::classify_name(n.c_str());
            if (c != AcClass::None)
                { std::printf("        svc: %-24s [%s]\n", n.c_str(), AcDriverOracle::to_string(c)); any = true; }
        }
        for (const auto& n : drivers) {
            const AcClass c = AcDriverOracle::classify_name(n.c_str());
            if (c != AcClass::None)
                { std::printf("        drv: %-24s [%s]\n", n.c_str(), AcDriverOracle::to_string(c)); any = true; }
        }
        if (!any) std::printf("        (none)\n");
    }

    // ── (2) PRIMARY — known-AC-title map (the operator's games + more) ────────
    {
        // Halo MCC → EAC → CleanBlock_A (probe allowed)
        CHECK(AcDriverOracle::classify_title("MCC-Win64-Shipping.exe") == AcClass::CleanBlock_A);
        // League of Legends → Vanguard → Allow_B (probe allowed)
        CHECK(AcDriverOracle::classify_title("League of Legends.exe")  == AcClass::Allow_B);
        CHECK(AcDriverOracle::classify_title("VALORANT-Win64-Shipping.exe") == AcClass::Allow_B);
        // Battlefield 6 → EA anti-cheat → UnknownAc (do-not-probe)
        CHECK(AcDriverOracle::classify_title("bf6.exe")               == AcClass::UnknownAc);
        // Fortnite → EAC → CleanBlock_A (probe allowed)
        CHECK(AcDriverOracle::classify_title("FortniteClient-Win64-Shipping.exe") == AcClass::CleanBlock_A);
        // Call of Duty → Ricochet → SilentPunish_C (do-not-probe)
        CHECK(AcDriverOracle::classify_title("cod.exe")              == AcClass::SilentPunish_C);
        CHECK(AcDriverOracle::classify_title("Destiny2.exe")         == AcClass::SilentPunish_C);
        // Unknown, non-AC title → None (probe allowed) — the title map alone.
        CHECK(AcDriverOracle::classify_title("notepad.exe")          == AcClass::None);
        CHECK(AcDriverOracle::classify_title("MyIndieGame.exe")      == AcClass::None);

        // Full decision path: a known title is authoritative regardless of the
        // box's resident drivers. LoL → Allow_B even though vgk is resident.
        CHECK(oracle.classify_foreground_game("League of Legends.exe") == AcClass::Allow_B);
        CHECK(oracle.classify_foreground_game("cod.exe")               == AcClass::SilentPunish_C);
        CHECK(oracle.classify_foreground_game("bf6.exe")               == AcClass::UnknownAc);

        std::printf("[OK] PRIMARY known-AC-title map\n");
    }

    // ── (2b) decide() — the pure decision core, DETERMINISTIC (box-independent) ─
    // Proves both bug fixes without depending on what happens to be running.
    {
        const std::vector<std::string> none{};
        const std::vector<std::string> resident_only = { "vgk.sys", "nvlddmkm.sys", "ntoskrnl.exe" };
        const std::vector<std::string> eac_running   = { "easyanticheat_eos" };
        const std::vector<std::string> ea_running    = { "eaanticheat.sys" };
        const std::vector<std::string> ea_svc        = { "EAAntiCheatService" };

        // The coordinator's exact requirement, made deterministic:
        // unknown title + NO active AC running → None (probe allowed).
        CHECK(AcDriverOracle::decide("some_unknown_game.exe", none, none) == AcClass::None);

        // BUG 2: unknown title + a LONE always-resident driver (vgk) running,
        // no matching Riot title in foreground → must NOT be refused → None.
        CHECK(AcDriverOracle::decide("some_unknown_game.exe", resident_only, none) == AcClass::None);

        // Unknown title + an on-demand AC actively running → do-not-probe.
        CHECK(AcDriverOracle::decide("some_unknown_game.exe", none, eac_running) == AcClass::UnknownAc);
        CHECK(AcDriverOracle::decide("some_unknown_game.exe", ea_running, ea_svc) == AcClass::UnknownAc);

        // PRIMARY authority (BUG 2 essence): a KNOWN title's class wins over any
        // box noise — LoL stays Allow_B even with a BE driver loaded.
        CHECK(AcDriverOracle::decide("League of Legends.exe", { "bedaisy.sys" }, none) == AcClass::Allow_B);
        // Known do-not-probe title with an empty box still refuses (title map).
        CHECK(AcDriverOracle::decide("bf6.exe", none, none) == AcClass::UnknownAc);

        std::printf("[OK] decide() deterministic decision core (BUG 1 + BUG 2 fixes)\n");
    }

    // ── (3) SECONDARY — driver/service → class building block (synthetic) ─────
    {
        // vgk.sys → Allow_B (Vanguard)
        CHECK(AcDriverOracle::classify_name("vgk.sys") == AcClass::Allow_B);
        CHECK(AcDriverOracle::classify_name("VGK.SYS") == AcClass::Allow_B);  // case-insensitive
        // EAC driver → CleanBlock_A
        CHECK(AcDriverOracle::classify_name("EasyAntiCheat.sys") == AcClass::CleanBlock_A);
        CHECK(AcDriverOracle::classify_name("EasyAntiCheat_EOS.sys") == AcClass::CleanBlock_A);
        // BEDaisy.sys → SilentPunish_C (kernel BattlEye, do-not-probe)
        CHECK(AcDriverOracle::classify_name("BEDaisy.sys") == AcClass::SilentPunish_C);
        CHECK(AcDriverOracle::classify_name("BEService")   == AcClass::SilentPunish_C);
        // BF6 Javelin + GameGuard → UnknownAc (do-not-probe)
        CHECK(AcDriverOracle::classify_name("javelin.sys") == AcClass::UnknownAc);
        CHECK(AcDriverOracle::classify_name("GameGuard")   == AcClass::UnknownAc);
        // Unknown, non-AC "foo.sys" → None (probe allowed)
        CHECK(AcDriverOracle::classify_name("foo.sys")     == AcClass::None);
        CHECK(AcDriverOracle::classify_name("nvlddmkm.sys") == AcClass::None);
        // Unknown BUT AC-looking → UnknownAc (fail-safe, do-not-probe)
        CHECK(AcDriverOracle::classify_name("mysteryanticheat.sys") == AcClass::UnknownAc);
        CHECK(AcDriverOracle::classify_name("Some-Anti-Cheat.sys")  == AcClass::UnknownAc);

        // A "no ac" driver list folds to None.
        std::vector<std::string> no_ac = { "foo.sys", "ntoskrnl.exe", "nvlddmkm.sys" };
        CHECK(AcDriverOracle::classify_names(no_ac) == AcClass::None);
        // Fail-safe fold: worst active AC wins. Vanguard(b)+BEDaisy(c) → C.
        std::vector<std::string> mixed = { "vgk.sys", "BEDaisy.sys", "foo.sys" };
        CHECK(AcDriverOracle::classify_names(mixed) == AcClass::SilentPunish_C);

        std::printf("[OK] SECONDARY driver->class building block (synthetic inputs)\n");
    }

    // ── (4) probe_allowed() per class ─────────────────────────────────────────
    {
        CHECK(AcDriverOracle::probe_allowed(AcClass::None)           == true);
        CHECK(AcDriverOracle::probe_allowed(AcClass::CleanBlock_A)   == true);
        CHECK(AcDriverOracle::probe_allowed(AcClass::Allow_B)        == true);
        CHECK(AcDriverOracle::probe_allowed(AcClass::SilentPunish_C) == false);
        CHECK(AcDriverOracle::probe_allowed(AcClass::UnknownAc)      == false);
        std::printf("[OK] probe_allowed() per class\n");
    }

    if (g_failures == 0) {
        std::printf("\n[PASS] ac_driver_oracle_test\n");
        return 0;
    }
    std::printf("\n[FAIL] ac_driver_oracle_test: %d failure(s)\n", g_failures);
    return 1;
}
// Made with my soul - Swately <3
