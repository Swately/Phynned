// core/include/phynned/core/UserRuleLookup.hpp
// find_user_rule_for — W3 per-process rule lookup with LAZY path resolution.
//
// ConfigStore::find_process_rule is pure: a rule that CARRIES a path (the
// hand-edited disambiguation contract, strategies §1) only matches when the
// caller supplies the process's full image path. The agent's hot paths don't
// have that path, and passing nullptr would leave every path-carrying rule
// silently inert — for a NEVER rule that is fail-UNSAFE for the user's veto
// intent (the corral would still touch the process the user tried to protect).
//
// This helper closes that hole: name-only rules match with zero syscalls; only
// when a same-name PATH-carrying rule exists does it resolve the full image
// path (one PROCESS_QUERY_LIMITED_INFORMATION open — the same benign tier as
// get_process_short_name; typically 0 such rules exist, so steady-state handle
// traffic is unchanged) and retry the match.
//
// AC discipline (precedence #1 safety > #2/#3 user, frozen in the master
// plan): NO handle is EVER opened on a do-not-probe title (SilentPunish_C /
// UnknownAc per the PRIMARY title map) — a user rule cannot relax the
// zero-handle rule. Such a rule simply never matches; do-not-probe titles are
// unplaceable by every other path anyway (§6 gate, corral game-exclusion).
//
// Threading: call from the agent main thread (reads AgentConfig, no shared
// mutable state). Privilege: QUERY_LIMITED open only, and only as described.
//
#pragma once

#include <phynned/config/ConfigStore.hpp>
#include <phynned/observer/AcDriverOracle.hpp>

#include <cstdint>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#endif

namespace phynned::core {

namespace detail {

/// Case-insensitive ASCII compare (portable; matches ConfigStore's semantics).
[[nodiscard]] inline bool rule_ci_equal(const char* a, const char* b) noexcept {
    if (!a || !b) return false;
    while (*a && *b) {
        char ca = *a, cb = *b;
        if (ca >= 'A' && ca <= 'Z') ca = static_cast<char>(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z') cb = static_cast<char>(cb - 'A' + 'a');
        if (ca != cb) return false;
        ++a; ++b;
    }
    return *a == *b;
}

/// Resolve a PID's full image path (narrow, non-ASCII → '?').
/// Same QUERY_LIMITED tier and narrowing as get_process_short_name.
[[nodiscard]] inline bool
get_process_full_path(uint32_t pid, char* out, uint32_t out_cap) noexcept {
#ifdef _WIN32
    if (out_cap == 0u) return false;
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE,
                           static_cast<DWORD>(pid));
    if (!h) return false;
    wchar_t wpath[MAX_PATH]{};
    DWORD sz = MAX_PATH;
    const BOOL ok = QueryFullProcessImageNameW(h, 0u, wpath, &sz);
    CloseHandle(h);
    if (!ok || sz == 0u) return false;
    uint32_t out_i = 0u;
    const wchar_t* p = wpath;
    while (*p != L'\0' && out_i < out_cap - 1u) {
        out[out_i++] = (*p < 0x80) ? static_cast<char>(*p) : '?';
        ++p;
    }
    out[out_i] = '\0';
    return out_i > 0u;
#else
    (void)pid; (void)out; (void)out_cap;
    return false;   // path rules are a Windows-only contract today
#endif
}

} // namespace detail

/// Find the user ProcessRule governing (exe_name, pid), resolving the image
/// path lazily iff a same-name path-carrying rule exists. Returns nullptr when
/// no rule matches (or when resolution is forbidden/failed — fail-safe: no
/// match ⇒ no user placement; the un-vetoed automatic paths carry their own
/// safety gates).
[[nodiscard]] inline const config::ProcessRule*
find_user_rule_for(const config::AgentConfig& cfg,
                   const char* exe_name, uint32_t pid) noexcept {
    // Fast path: rules without a path match by name alone — zero syscalls.
    if (const config::ProcessRule* r =
            config::ConfigStore::find_process_rule(cfg, exe_name, nullptr))
        return r;

    // Any same-name rule carrying a path? (Otherwise: no rule at all.)
    bool path_rule = false;
    for (uint32_t i = 0u; i < cfg.n_process_rules; ++i) {
        if (cfg.process_rules[i].path[0] != '\0' &&
            detail::rule_ci_equal(cfg.process_rules[i].name, exe_name)) {
            path_rule = true;
            break;
        }
    }
    if (!path_rule) return nullptr;

    // Zero-handle AC discipline: a do-not-probe title never gets ANY handle,
    // user rule or not (safety outranks the user layers).
    const observer::AcClass k =
        observer::AcDriverOracle::classify_title(exe_name);
    if (k == observer::AcClass::SilentPunish_C ||
        k == observer::AcClass::UnknownAc)
        return nullptr;

    char full[260]{};
    if (!detail::get_process_full_path(pid, full, sizeof(full)))
        return nullptr;
    return config::ConfigStore::find_process_rule(cfg, exe_name, full);
}

} // namespace phynned::core
// Made with my soul - Swately <3
