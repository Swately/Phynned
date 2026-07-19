# PHYNNED USE MODES — MASTER PLAN (T1)

**Date:** 2026-07-19 · **Status:** APPROVED (operator chose full scope 1-4 + name-with-optional-path matching)
**Origin:** operator directives after the MR-2 live-enable session (GREEN 2026-07-19):
(a) corral switch-off must REVERT by default, with an option for the old stop-only behavior;
(b) users need persistent per-process customization (e.g. a "never optimize this" list);
(c) surface Phynned's use cases as explicit usage options.

## Use-case analysis (what this plan serves)

| UC | Persona | Needs | Gap closed here |
|---|---|---|---|
| UC1 | Classic gamer | game→V-Cache, zero interaction, AC-safe | none (works today) |
| UC2 | Gamer + clean machine | UC1 + corral | corral switch persistence |
| UC3 | Workstation (no games) | heavy app on right CCD | manual Always→CCD rules (A/B engine is MR-3) |
| UC4 | Cautious user | "never touch THESE processes" | persistent user Never list |
| UC5 | Observer | telemetry only, zero changes | explicit Monitor profile |
| UC6 | Power user | own per-process rules | Always→Freq / Always→VCache rules |

## Deliverables (4 waves)

1. **W1 — Corral off→revert.** Disabling the corral (IPC or config) reverts all
   corral-originated placements (selective: `rule_id == kCorralRuleId` only — game pins
   untouched). Old behavior available via `[corral] keep_placements_on_disable = true`.
2. **W2 — Persistence.** `policies.toml` **v2**: `[corral] enabled` (switch survives agent
   restart), `keep_placements_on_disable`, `profile`. v1 files load unchanged (back-compat).
   Agent = single writer; UI changes flow through IPC commands, agent saves.
3. **W3 — Per-process user rules.** Persistent `[[process]]` entries: `name` (required),
   `path` (optional — empty matches any path), `action = "never" | "freq" | "vcache"`,
   cap 128. Created from the UI by right-clicking a Targets row (PID-based command; agent
   resolves name+path itself). Editor + removal in the Policies tab. Rules published to SHM
   for UI display.
4. **W4 — Global profile selector.** `profile = monitor | games | games_corral | full`.
   Monitor = observe+advise, zero placement. Games = classic game placement only, corral off.
   GamesCorral = today's behavior. Full = reserved for MR-3 (parses, falls back to
   games_corral with a log line).

## Precedence (top always wins; frozen)

```
1. Safety      — AC-gate, system denylist, self-managed-mask   ← NOT user-overridable
2. User Never  — user veto over any optimization
3. User Always — user-forced CCD placement (still AC-gated, still journaled)
4. Automatic   — game rules, corral, future A/B routing
```

## Risks (mitigation AS CODE, verified in this pass — no `open` risk at commit)

| ID | Risk | Mitigation (code) | Verification |
|---|---|---|---|
| R1 | User Always rule bypasses the AC gate (M1 veto broken) | user-pin pass calls the SAME probe/oracle path as §6; class-(c)/Refused → refuse + visible reason | unit test: Game target + oracle (c) → zero apply |
| R2 | TOML v2 breaks v1 configs | v2 loader defaults all new fields; version field respected | fixture test: v1 file loads, fields = defaults |
| R3 | off→revert also reverts game pins | selective revert filters `rule_id == kCorralRuleId` | unit test: mixed active actions → only corral reverted |
| R4 | Always rule flap-wars a process pinned by another tool | non-full mask not ours → skip + warn log + UI badge (never fight) | unit test: pre-pinned mask → zero apply + flag set |
| R5 | SHM rules block shifts baseline ring offsets | static_asserts on new layout; agent+UI rebuild together (established practice) | build-time asserts + live smoke |
| R6 | Monitor profile still places | single placement master-gate checked in §3c corral, §5 policy feed, user-pin pass | live smoke: profile=monitor → n_active_actions stays 0 |

## Milestone gate (verify-before-claim)

Build green + full ctest no-regression + live smoke on the box: create a rule via right-click
→ TOML written → restart agent → rule survives and applies; corral ON→OFF reverts (system
affinity sweep returns to baseline); profile=monitor produces zero applies.

// Made with my soul - Swately <3
