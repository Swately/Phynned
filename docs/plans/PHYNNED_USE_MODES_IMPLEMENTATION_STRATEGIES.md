# PHYNNED USE MODES — IMPLEMENTATION STRATEGIES (T1)

Companion to `PHYNNED_USE_MODES_MASTER_PLAN.md`. Concrete mechanics, anchored in current code.

## §1 Config layer (`config/` — ConfigStore v2)

- `AgentConfig` (config::) gains:
  ```
  Profile  profile {Profile::GamesCorral};   // enum uint8: Monitor=0 Games=1 GamesCorral=2 Full=3
  bool     corral_enabled {false};
  bool     corral_keep_on_disable {false};
  uint32_t n_process_rules {0};
  ProcessRule process_rules[kMaxProcessRules /*128*/];
  ```
  `ProcessRule { char name[64]; char path[260]; uint8_t action /*0=Never 1=Freq 2=VCache*/; }`
  — trivially copyable, static_asserted.
- TOML v2 (same hand-rolled subset parser style as v1):
  ```toml
  version = 2
  op_mode = "auto"
  profile = "games_corral"
  [corral]
  enabled = false
  keep_placements_on_disable = false
  [[process]]
  name = "handbrake.exe"
  path = ""            # optional; empty = match any path
  action = "freq"      # never | freq | vcache
  ```
- **Back-compat (R2):** `version = 1` files parse; every new field takes its default.
  Save always writes v2. Fixture test with a literal v1 string.
- Matching: case-insensitive exe-name compare; if `path[0] != '\0'`, full-path
  case-insensitive compare must ALSO pass. Right-click rule creation stores name + resolved
  path but leaves `path=""` in the match sense (path recorded in a comment-like `path` field
  only if the user later hand-edits it to non-empty? NO — simpler contract: creation stores
  `path=""`; hand-editing the TOML sets a path when disambiguation is needed. Document in
  the TOML header comment the agent writes.)

## §2 Agent runtime (`core/AgentRuntime.cpp`)

- **Startup:** `corral_live = file_cfg.corral_enabled || cfg.corral_live_default` (CLI
  `--corral-live` self-test override keeps working). Profile loaded; op_mode untouched.
- **Placement master-gate (R6):** `const bool placement_allowed = profile != Monitor;`
  checked at the three feed points: §5 eligible→policy feed, §3c corral gate
  (`corral_apply = corral_live && !coexist && placement_allowed && profile == GamesCorral`),
  and the new user-pin pass. Observation/shadow/telemetry NEVER gated.
- **IPC transitions (command handler):**
  - `kPhynnedCmdSetCorralLive` 1→0: if `!corral_keep_on_disable` → selective revert of all
    active actions with `rule_id == kCorralRuleId` (R3). Then persist `corral_enabled` via
    `ConfigStore::save_policies` (agent single writer).
  - New `kPhynnedCmdSetProcessRule = 6`: `target_pid` + `arg1 = action(0/1/2)`. Agent
    resolves name from its tracked state (fallback `QueryFullProcessImageNameW` for path
    display). Duplicate name ⇒ update action in place. Save TOML.
  - New `kPhynnedCmdRemoveProcessRule = 7`: `arg1 = SHM slot index`, `arg2 = generation`
    (stale generation ⇒ ignore). Save TOML.
  - New `kPhynnedCmdSetProfile = 8`: `arg1 = profile`. Profile leaving GamesCorral while
    corral placements are active ⇒ same selective revert path as corral-off. Save TOML.
- **User-pin pass (new, in the §3c region, runs when `placement_allowed && !policies_paused`):**
  for each tracked target matching an Always rule and not already on the target mask:
  - Safety first (R1): if `kind == Game` or unknown-class exe → run the SAME §6
    probe/oracle gate; Refused/Blocked ⇒ no apply, set per-rule `blocked_by_ac` flag
    (SHM) + one log line. NEVER bypassed.
  - Flap guard (R4): current mask ≠ full-system AND pid has no active action of ours ⇒
    skip + `flap_warn` flag + log. Never fight another manager.
  - Apply via the EXISTING `executor.apply` (journal + revert machinery),
    `d.rule_id = kUserRuleId` (new constant ≠ kCorralRuleId), confidence 100.
  - Never rules: checked in `evaluate_corral` candidates, in the §5 eligible feed, and in
    the user-pin pass — placement excluded, observation continues.
- **Revert-all paths** (ForceRevertAll, shutdown) unchanged — they cover user pins too
  (they revert everything active; journal captured-prev).

## §3 IPC/SHM (`ipc/PhynnedProtocol.hpp`)

- `PhynnedStateHeader`: carve `uint8_t profile` @70 from `_pad2b` (128B intact,
  static_assert offset). corral_live/corral_coexist stay @68/69.
- **New `UserRulesBlock` appended after `command_slot`:**
  ```
  struct UserRuleShm { char name[64]; char path[128]; uint8_t action;
                       uint8_t flags; /*bit0 has_path, bit1 blocked_by_ac, bit2 flap_warn*/
                       uint8_t _pad[2]; };                  // 196 B
  struct UserRulesBlock { uint32_t n_rules; uint32_t generation; UserRuleShm rules[128]; };
  ```
  Baseline ring start shifts (R5): update the baseline-ring offset derivation +
  static_asserts; agent and UI always rebuild together (established practice, ABI v1
  clients re-read via struct). `shm_peek` offsets ≤197 unaffected.
- Publisher: copy rules + counts each publish (seqlock already wraps the whole layout write).

## §4 UI (`tools/phynned-ui/`)

- **targets_panel:** `BeginPopupContextItem` per row → "Never optimize", "Always → Freq CCD",
  "Always → V-Cache CCD", "Clear rule" (sends Set/Remove commands by PID / slot lookup via
  name in the SHM rules table). Rule badge in the Advice cell: `user:Freq` / `user:VCache` /
  `never` (distinct color), `⚠ AC` when blocked_by_ac, `⚠ pinned elsewhere` for flap_warn.
- **policies_panel:** profile radio (Monitor / Games / Games+Corral; Full shown disabled
  "requires the A/B engine (MR-3)") → kPhynnedCmdSetProfile. Corral block: switch now
  persistent (state echoes `[corral] enabled`), plus small checkbox "Keep placements when
  disabling (old behavior)" → needs a config-write command: reuse SetCorralLive `arg2` =
  keep_on_disable (0/1) piggyback. Rules editor table (name, path-if-set, action, Remove).
- **PhynnedAppState/LogicNode:** mirror profile + rules block into the snapshot (extend
  snapshot struct; keep 4-byte alignment discipline; static_asserts).

## §5 Tests (all in existing test dirs, ctest-registered)

1. config: v2 round-trip (save→load byte-equal fields) + v1 fixture back-compat (R2).
2. config: matching — name-only, name+path, case-insensitivity.
3. runtime: selective revert — mixed kCorralRuleId + game-rule actions → only corral reverted (R3).
4. runtime: user-pin AC gate — Game + oracle class (c) → zero applies, flag set (R1).
5. runtime: flap guard — pre-pinned foreign mask → zero applies, flag set (R4).
6. layout: static_asserts compile (R5) — implicit in build.

## §6 Delegation + verification contract

Implementer: single Opus agent, this document is binding. Use existing pillars/idioms
(no reinvented serialization; hand-rolled TOML subset extends the EXISTING parser).
Authorship signature on every touched file. Build green + full ctest + the 6 tests above
before reporting. Supervisor (session) then runs the live smoke of the master-plan gate
first-hand before anything is claimed done.

// Made with my soul - Swately <3
