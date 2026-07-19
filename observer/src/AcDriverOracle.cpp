// observer/src/AcDriverOracle.cpp
// AcDriverOracle — implementation. Zero-handle AC detection + class oracle (CR1).
//
// Detection is GAME-SPECIFIC: the PRIMARY signal is the known-AC-title map; the
// SECONDARY (unknown title only) is a cautious read of the RUNNING AC drivers/
// services. Neither path opens a handle on any game process.
//

#include <phynned/observer/AcDriverOracle.hpp>

#include <cstring>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#  include <psapi.h>     // EnumDeviceDrivers / GetDeviceDriverBaseNameA
#  include <winsvc.h>    // OpenSCManagerA / EnumServicesStatusExA
#endif

namespace phynned::observer {

// ── Case-insensitive substring match (needle assumed already lowercase) ──────
static char to_lower_ascii(char c) noexcept {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

static bool contains_ci(const char* hay, const char* needle_lc) noexcept {
    if (hay == nullptr || needle_lc == nullptr || *needle_lc == '\0') return false;
    for (const char* h = hay; *h != '\0'; ++h) {
        const char* a = h;
        const char* b = needle_lc;
        while (*a != '\0' && *b != '\0'
               && to_lower_ascii(*a) == *b) {
            ++a; ++b;
        }
        if (*b == '\0') return true;   // consumed the whole needle
    }
    return false;
}

// ── PRIMARY: the known-AC-TITLE map (seeded from the [V]-tagged research) ─────
// Substring needles, lowercase, matched against the foreground game's exe name.
// The title directly names its anti-cheat → this is the robust primary signal.
// First matching row wins. Add rows to extend.
static constexpr AcTitle kKnownTitles[] = {
    // ── (b) ALLOW — Riot Vanguard ───────────────────────────────────────────
    { "league of legends.exe", AcClass::Allow_B        },  // LoL (Vanguard)
    { "valorant",              AcClass::Allow_B        },  // VALORANT-Win64-Shipping.exe
    // ── (a) CLEAN BLOCK — Easy Anti-Cheat ────────────────────────────────────
    { "mcc-win64-shipping.exe", AcClass::CleanBlock_A  },  // Halo: The Master Chief Collection
    { "fortniteclient",         AcClass::CleanBlock_A  },  // FortniteClient-Win64-Shipping.exe
    { "eldenring.exe",          AcClass::CleanBlock_A  },  // Elden Ring (EAC)
    { "armoredcore6.exe",       AcClass::CleanBlock_A  },  // Armored Core VI (EAC)
    // ── (c) SILENT-THEN-PUNISH — Ricochet + kernel BattlEye ──────────────────
    { "cod.exe",                AcClass::SilentPunish_C },  // Call of Duty (Ricochet)
    { "modernwarfare",          AcClass::SilentPunish_C },  // MW/Warzone (Ricochet)
    { "destiny2.exe",           AcClass::SilentPunish_C },  // Destiny 2 (kernel BattlEye)
    // ── UnknownAc — EA anti-cheat (do-not-probe; unverified) ─────────────────
    { "bf6.exe",                AcClass::UnknownAc      },  // Battlefield 6 (EA Javelin AC)
    { "battlefield6",           AcClass::UnknownAc      },  // BF6 alt exe naming
};

// ── SECONDARY: RUNNING driver/service → class map (the fallback signal) ──────
// Substring needles, lowercase. `always_on` marks AC that is resident even with
// no game running (skipped when escalating an unknown title). First match wins.
static constexpr AcSignature kKnownAc[] = {
    // ── (b) ALLOW — Riot Vanguard (ALWAYS-ON: vgk loads at boot) ─────────────
    { "vgk.sys",       AcClass::Allow_B,        true  },  // Vanguard kernel driver (resident)
    { "vgc",           AcClass::Allow_B,        true  },  // Vanguard "vgc" service
    { "vanguard",      AcClass::Allow_B,        true  },  // display-name catch-all
    // ── (a) CLEAN BLOCK — Easy Anti-Cheat (loads with its game) ──────────────
    { "easyanticheat", AcClass::CleanBlock_A,   false },  // EasyAntiCheat.sys / _EOS / service
    // ── (c) SILENT-THEN-PUNISH — kernel BattlEye + Ricochet ──────────────────
    { "bedaisy",       AcClass::SilentPunish_C, false },  // kernel-BattlEye driver BEDaisy.sys
    { "beservice",     AcClass::SilentPunish_C, false },  // BattlEye service
    { "battleye",      AcClass::SilentPunish_C, false },  // display-name catch-all
    { "ricochet",      AcClass::SilentPunish_C, false },  // Activision Ricochet
    // ── UNVERIFIED kernel ACs — do-not-probe (fail-safe UnknownAc) ───────────
    { "gameguard",     AcClass::UnknownAc,      false },  // nProtect GameGuard
    { "npggnt",        AcClass::UnknownAc,      false },  // GameGuard driver npggNT
    { "xigncode",      AcClass::UnknownAc,      false },  // XIGNCODE3
    { "xhunter",       AcClass::UnknownAc,      false },  // XIGNCODE3 driver xhunter1.sys
    { "denuvo",        AcClass::UnknownAc,      false },  // Denuvo Anti-Cheat
    { "javelin",       AcClass::UnknownAc,      false },  // BF6 EA Javelin AC
    { "eaanticheat",   AcClass::UnknownAc,      false },  // EA anti-cheat service
};

// Generic AC markers: an AC-looking name NOT in kKnownAc → unrecognised AC →
// UnknownAc (do-not-probe). Fail-safe by design.
static constexpr const char* kAcMarkers[] = {
    "anticheat",
    "anti-cheat",
    "anti_cheat",
};

// ── match_known ──────────────────────────────────────────────────────────────
const AcSignature* AcDriverOracle::match_known(const char* name) noexcept {
    if (name == nullptr || *name == '\0') return nullptr;
    for (const AcSignature& s : kKnownAc) {
        if (contains_ci(name, s.needle)) return &s;
    }
    return nullptr;
}

// ── classify_name (building block; ignores always_on) ────────────────────────
AcClass AcDriverOracle::classify_name(const char* name) noexcept {
    if (name == nullptr || *name == '\0') return AcClass::None;
    if (const AcSignature* s = match_known(name)) return s->klass;
    for (const char* marker : kAcMarkers) {
        if (contains_ci(name, marker)) return AcClass::UnknownAc;
    }
    return AcClass::None;
}

// ── name_indicates_active_ac — does a RUNNING name imply a live AC game? ─────
bool AcDriverOracle::name_indicates_active_ac(const char* name) noexcept {
    if (const AcSignature* s = match_known(name)) {
        return !s->always_on;   // known AC → escalate unless it is always-resident
    }
    for (const char* marker : kAcMarkers) {  // unrecognised-but-AC-looking → fail-safe
        if (contains_ci(name, marker)) return true;
    }
    return false;
}

// ── classify_title — PRIMARY: the game's exe names its anti-cheat ────────────
AcClass AcDriverOracle::classify_title(const char* game_exe_name) noexcept {
    if (game_exe_name == nullptr || *game_exe_name == '\0') return AcClass::None;
    for (const AcTitle& t : kKnownTitles) {
        if (contains_ci(game_exe_name, t.exe_needle)) return t.klass;
    }
    return AcClass::None;   // unknown title → caller falls back to the secondary
}

// ── severity — restrictiveness rank ──────────────────────────────────────────
uint8_t AcDriverOracle::severity(AcClass c) noexcept {
    switch (c) {
        case AcClass::None:           return 0u;
        case AcClass::Allow_B:        return 1u;
        case AcClass::CleanBlock_A:   return 2u;
        case AcClass::SilentPunish_C: return 3u;
        case AcClass::UnknownAc:      return 4u;
    }
    return 4u;  // unreachable; fail closed
}

// ── classify_names — worst active AC wins (box-wide fold; building block) ────
AcClass AcDriverOracle::classify_names(const std::vector<std::string>& names) noexcept {
    AcClass worst = AcClass::None;
    for (const std::string& n : names) {
        const AcClass c = classify_name(n.c_str());
        if (severity(c) > severity(worst)) worst = c;
    }
    return worst;
}

// ── probe_allowed — the gate ─────────────────────────────────────────────────
bool AcDriverOracle::probe_allowed(AcClass c) noexcept {
    switch (c) {
        case AcClass::None:
        case AcClass::CleanBlock_A:
        case AcClass::Allow_B:
            return true;                 // probe permitted
        case AcClass::SilentPunish_C:
        case AcClass::UnknownAc:
            return false;                // do-not-probe
    }
    return false;                        // fail closed
}

// ── to_string ────────────────────────────────────────────────────────────────
const char* AcDriverOracle::to_string(AcClass c) noexcept {
    switch (c) {
        case AcClass::None:           return "None";
        case AcClass::CleanBlock_A:   return "CleanBlock_A";
        case AcClass::Allow_B:        return "Allow_B";
        case AcClass::SilentPunish_C: return "SilentPunish_C";
        case AcClass::UnknownAc:      return "UnknownAc";
    }
    return "UnknownAc";
}

// ── enumerate_drivers — loaded kernel drivers, zero game handle ──────────────
std::size_t AcDriverOracle::enumerate_drivers(std::vector<std::string>& out) const {
    out.clear();
#ifdef _WIN32
    // Size probe: how many driver base addresses are loaded right now?
    DWORD needed = 0u;
    if (!EnumDeviceDrivers(nullptr, 0u, &needed) || needed == 0u) return 0u;

    std::vector<LPVOID> bases(needed / sizeof(LPVOID) + 1u);
    DWORD got = 0u;
    if (!EnumDeviceDrivers(bases.data(),
                           static_cast<DWORD>(bases.size() * sizeof(LPVOID)),
                           &got)) {
        return 0u;
    }
    const std::size_t count = got / sizeof(LPVOID);
    out.reserve(count);
    char name[256];
    for (std::size_t i = 0u; i < count; ++i) {
        if (bases[i] == nullptr) continue;
        name[0] = '\0';
        if (GetDeviceDriverBaseNameA(bases[i], name, sizeof(name)) > 0u) {
            out.emplace_back(name);
        }
    }
#else
    (void)out;
#endif
    return out.size();
}

// ── enumerate_services — ACTIVELY RUNNING services + drivers, zero game handle ─
// BUG 1 fix: SERVICE_ACTIVE (running only), not SERVICE_STATE_ALL — an installed-
// but-stopped AC service must not count as "AC active."
std::size_t AcDriverOracle::enumerate_services(std::vector<std::string>& out) const {
    out.clear();
#ifdef _WIN32
    SC_HANDLE scm = OpenSCManagerA(nullptr, nullptr, SC_MANAGER_ENUMERATE_SERVICE);
    if (scm == nullptr) return 0u;

    // First pass sizes the buffer (fails with ERROR_MORE_DATA); then loop until
    // drained (EnumServicesStatusExA can page via the resume handle).
    DWORD bytes_needed = 0u;
    DWORD services_returned = 0u;
    DWORD resume = 0u;
    std::vector<BYTE> buf;

    for (;;) {
        BYTE probe = 0;
        BOOL ok = EnumServicesStatusExA(
            scm, SC_ENUM_PROCESS_INFO,
            SERVICE_WIN32 | SERVICE_DRIVER, SERVICE_ACTIVE,   // RUNNING only (BUG 1)
            buf.empty() ? &probe : buf.data(),
            static_cast<DWORD>(buf.size()),
            &bytes_needed, &services_returned, &resume, nullptr);

        if (!ok && GetLastError() == ERROR_MORE_DATA) {
            buf.resize(buf.size() + bytes_needed);
            continue;  // retry with a bigger buffer, same resume point
        }
        if (!ok) break;  // real error → stop with what we have

        const auto* arr =
            reinterpret_cast<const ENUM_SERVICE_STATUS_PROCESSA*>(buf.data());
        for (DWORD i = 0u; i < services_returned; ++i) {
            if (arr[i].lpServiceName != nullptr) {
                out.emplace_back(arr[i].lpServiceName);
            }
        }
        if (resume == 0u) break;  // fully drained
        buf.clear();              // page the next batch
    }
    CloseServiceHandle(scm);
#else
    (void)out;
#endif
    return out.size();
}

// ── decide — the pure decision core (title-map first, running-AC secondary) ──
// Deterministic over its inputs; opens no handle and does no enumeration.
AcClass AcDriverOracle::decide(
    const char* game_exe_name,
    const std::vector<std::string>& running_drivers,
    const std::vector<std::string>& running_services) noexcept {
    // PRIMARY: the title directly names its anti-cheat. If known, that class is
    // authoritative — ignore box-wide driver noise (a resident vgk is irrelevant
    // when the title itself tells us the AC).
    const AcClass t = classify_title(game_exe_name);
    if (t != AcClass::None) return t;

    // SECONDARY: unknown title. Only refuse to probe if a non-always-on AC is
    // actively running (some AC-protected game is live). A lone always-resident
    // driver (vgk) never triggers this (BUG 2 fix).
    for (const std::string& n : running_services) {
        if (name_indicates_active_ac(n.c_str())) return AcClass::UnknownAc;
    }
    for (const std::string& n : running_drivers) {
        if (name_indicates_active_ac(n.c_str())) return AcClass::UnknownAc;
    }
    return AcClass::None;   // no active on-demand AC → probe allowed
}

// ── classify_foreground_game — decide() over the LIVE box enumeration ────────
AcClass AcDriverOracle::classify_foreground_game(const char* game_exe_name) const {
    std::vector<std::string> drivers;    // loaded kernel drivers
    std::vector<std::string> services;   // RUNNING services only (BUG 1)
    enumerate_drivers(drivers);
    enumerate_services(services);
    return decide(game_exe_name, drivers, services);
}

} // namespace phynned::observer
// Made with my soul - Swately <3
