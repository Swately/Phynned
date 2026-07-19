// observer/include/phynned/observer/AcDriverOracle.hpp
// AcDriverOracle — the ZERO-HANDLE anti-cheat detection + class oracle (CR1).
//
// The safety veto for the mass-router. It answers ONE question — "is it safe to
// open a PROCESS_SET_INFORMATION handle on the foreground game and place its
// affinity?" — WITHOUT ever opening a handle on any game process.
//
// Detection is GAME-SPECIFIC, keyed on the foreground game's own exe identity
// (supplied by the caller from the ETW birth event), NOT box-wide:
//
//   PRIMARY   — a known-AC-TITLE map (`classify_title`): the game's exe name
//               directly names which anti-cheat protects it. This is robust
//               because the title, not the box, tells you the AC.
//   SECONDARY — for an UNKNOWN foreground title only, a cautious read of the
//               RUNNING AC services/drivers: escalate to do-not-probe only when
//               a NON-always-on AC service/driver is actively running (evidence
//               some AC-protected game is live). A lone always-resident driver
//               like Vanguard's `vgk` (loaded at boot, resident with no Riot
//               title open) must NOT force the whole box to do-not-probe.
//
// Why box-wide was wrong (both verified on the operator's box 2026-07-17):
//   BUG 1 — SERVICE_STATE_ALL counted INSTALLED-BUT-STOPPED AC services
//           (EAAntiCheatService/EasyAntiCheat_EOS/vgc all Stopped, no game
//           running) → false do-not-probe for the entire system. Fixed by
//           enumerating RUNNING services only (SERVICE_ACTIVE).
//   BUG 2 — `vgk` (Vanguard kernel driver) is Running at all times with no Riot
//           game open (boot-resident by design) → a box-wide fold read that as
//           "AC active" and refused to route everything. Fixed by making the
//           gate game-specific (title map) and flagging always-resident AC.
//
// Zero-handle contract (the whole point): this module NEVER calls OpenProcess.
// The game exe name is read as a plain string; the process is never touched.
//
// Threading: enumeration is done per call; the maps are static const. Safe from
//            any thread. Privilege: none — driver + service enumeration is
//            readable unprivileged (verified on-box: >200 drivers, zero handle).
//
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace phynned::observer {

// ── AcClass — the anti-cheat behaviour class that drives the probe decision ──
/// Seeded from SOTA_ANTICHEAT_AFFINITY.md's (a)/(b)/(c) classification.
///   None           — no AC on this title → probing allowed.
///   CleanBlock_A   — (a): handle stripped, SetProcessAffinityMask returns a
///                    catchable ACCESS_DENIED. The failure is the signal → probe
///                    allowed (EAC — Halo MCC, Fortnite, Elden Ring).
///   Allow_B        — (b): the affinity op succeeds, no punishment → probe
///                    allowed (Vanguard — LoL/VALORANT; VAC; base BattlEye).
///   SilentPunish_C — (c): the open/affinity call succeeds locally with NO error
///                    but is monitored + reported server-side → DO NOT PROBE
///                    (Ricochet — CoD; kernel-BattlEye — Destiny 2).
///   UnknownAc      — an AC protects the title but its affinity behaviour is
///                    unverified/unrecognised → fail-safe DO NOT PROBE
///                    (EA anti-cheat / BF6 Javelin, GameGuard, XIGNCODE3, ...).
enum class AcClass : uint8_t {
    None           = 0,
    CleanBlock_A   = 1,
    Allow_B        = 2,
    SilentPunish_C = 3,
    UnknownAc      = 4,
};

// ── AcTitle — one row of the known-AC-TITLE map (the PRIMARY signal) ─────────
/// `exe_needle` is a lowercase substring matched case-insensitively against the
/// foreground game's exe name (e.g. "valorant" matches "VALORANT-Win64-Shipping.exe").
/// The title directly names its anti-cheat. Extend by adding rows.
struct AcTitle {
    const char* exe_needle;   ///< lowercase; case-insensitive substring of the game exe
    AcClass     klass;
};

// ── AcSignature — one row of the RUNNING driver/service → class map (SECONDARY) ─
/// `needle` is a lowercase substring matched case-insensitively against a
/// driver base name (e.g. "vgk.sys") or a RUNNING service name (e.g. "beservice").
/// `always_on` marks anti-cheat that is resident regardless of whether its game
/// is running (Vanguard's vgk loads at boot); an always_on match does NOT by
/// itself imply the foreground title is protected, so the secondary skips it
/// when escalating an unknown title.
struct AcSignature {
    const char* needle;      ///< lowercase; case-insensitive substring
    AcClass     klass;
    bool        always_on;   ///< resident regardless of its game running
};

// ── AcDriverOracle ───────────────────────────────────────────────────────────
/// Zero-handle AC detection + class oracle. No handle is ever opened on a game.
class AcDriverOracle {
public:
    AcDriverOracle() noexcept = default;

    // ── PRIMARY — known-AC-title map ─────────────────────────────────────────
    /// Map the foreground game's exe name to its anti-cheat class via the static
    /// title table. Returns None when the title is not a known AC title (which
    /// then triggers the secondary in classify_foreground_game).
    [[nodiscard]] static AcClass classify_title(const char* game_exe_name) noexcept;

    // ── THE DECISION — title-map first, running-AC secondary as fallback ─────
    /// PRIMARY: if `game_exe_name` is a known AC title, return its class and
    /// IGNORE the box-wide driver/service noise (a resident vgk does not matter
    /// when the title itself tells us the AC). SECONDARY: if the title is
    /// UNKNOWN, escalate to UnknownAc (do-not-probe) only when a non-always-on
    /// AC service/driver is actively RUNNING; otherwise None (probe allowed).
    /// Never opens a handle on the game. This is decide() fed with the live box
    /// enumeration (enumerate_drivers + enumerate_services).
    [[nodiscard]] AcClass classify_foreground_game(const char* game_exe_name) const;

    /// Pure decision core (deterministic, box-independent, testable): PRIMARY
    /// title map, else SECONDARY over the PROVIDED running-driver + running-
    /// service name lists. `running_*` must be the ACTIVE/loaded names only.
    /// No handle opened; no enumeration — pure over its inputs.
    [[nodiscard]] static AcClass decide(
        const char* game_exe_name,
        const std::vector<std::string>& running_drivers,
        const std::vector<std::string>& running_services) noexcept;

    // ── THE GATE ─────────────────────────────────────────────────────────────
    /// true  → a least-privilege PROCESS_SET_INFORMATION probe is permitted
    ///         (None / CleanBlock_A / Allow_B).
    /// false → do-not-probe: never open a handle on the title
    ///         (SilentPunish_C / UnknownAc).
    [[nodiscard]] static bool probe_allowed(AcClass c) noexcept;

    /// Human-readable class name (for the audit log / test output).
    [[nodiscard]] static const char* to_string(AcClass c) noexcept;

    // ── SECONDARY building blocks (zero-handle system-wide enumeration) ──────
    /// Fill `out` with the base names of every LOADED kernel driver
    /// (EnumDeviceDrivers + GetDeviceDriverBaseNameA). Returns the count.
    std::size_t enumerate_drivers(std::vector<std::string>& out) const;

    /// Fill `out` with the service names of every ACTIVELY RUNNING service+driver
    /// (SCM EnumServicesStatusExA, SERVICE_ACTIVE — installed-but-stopped
    /// services are deliberately excluded; see BUG 1). Returns the count.
    std::size_t enumerate_services(std::vector<std::string>& out) const;

    /// Map ONE driver/service name to a class:
    ///   - a known AC name        → its mapped class,
    ///   - an AC-looking-but-unmapped name (contains a generic AC marker such
    ///     as "anticheat") → UnknownAc (fail-safe),
    ///   - anything else          → None.
    /// Pure over the name; ignores always_on. A building block / test hook.
    [[nodiscard]] static AcClass classify_name(const char* name) noexcept;

    /// Fold classify_name over a list and return the MOST RESTRICTIVE class
    /// found (fail-safe fold). NOTE: this is a box-wide fold and is NOT the gate
    /// — kept as a building block only. The gate is classify_foreground_game.
    [[nodiscard]] static AcClass classify_names(
        const std::vector<std::string>& names) noexcept;

private:
    /// Restrictiveness rank for the fail-safe fold: None < Allow_B <
    /// CleanBlock_A < SilentPunish_C < UnknownAc.
    [[nodiscard]] static uint8_t severity(AcClass c) noexcept;

    /// Find the known-AC signature row for a driver/service name, or nullptr.
    [[nodiscard]] static const AcSignature* match_known(const char* name) noexcept;

    /// True when a name denotes an anti-cheat that, if actively running,
    /// implies an AC-protected game is live: a known non-always-on AC, or an
    /// unrecognised-but-AC-looking name (fail-safe). Always-on AC (vgk) → false.
    [[nodiscard]] static bool name_indicates_active_ac(const char* name) noexcept;
};

} // namespace phynned::observer
// Made with my soul - Swately <3
