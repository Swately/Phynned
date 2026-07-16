// apps/ayama/policy/src/PolicyEngine.cpp
// PolicyEngine — implementation.
//

#include <ayama/policy/PolicyEngine.hpp>
#include <phyriad/topology/HardwareTopology.hpp>

#include <cstring>
#include <algorithm>

#ifdef _WIN32
#include <cstring>  // _stricmp via <string.h> on MinGW
#endif

namespace ayama::policy {

// ── Protected processes for CCD Load Defense ────
// Hard-coded list of processes that must NEVER be moved out of the
// V-Cache CCD by Rule 8, even when they consume CPU during gameplay.
// Categories:
//   - Ayama's own processes (would create self-pin recursion)
//   - Kernel-mode anti-cheat user-mode helpers (moving them may trigger
//     anti-cheat tampering detection)
//   - Critical OS services that perform real-time work
//
// Case-insensitive exact-match on basename.
static bool is_protected_from_ccd_defense(const char* exe_name) noexcept {
    if (!exe_name || exe_name[0] == '\0') return false;
    static constexpr const char* kProtected[] = {
        // ── Ayama's own processes ───────────────────────────────────────
        "ayama-agent.exe",
        "ayama-ui.exe",
        "ayama-cli.exe",
        // ── Anti-cheat user-mode helpers ────────────────────────────────
        // Touching these risks triggering kernel-mode anti-cheat tampering
        // detection. Even though they're typically Browser/Productivity
        // class, we skip them defensively.
        "vgc.exe",            // Riot Vanguard helper
        "vgtray.exe",         // Riot Vanguard tray
        "EasyAntiCheat.exe",  // EAC
        "EACLauncher.exe",
        "BEService.exe",      // BattlEye
        "BEClient.exe",
        "RICOCHET.exe",       // CoD Ricochet
        // ── OS services that should not be displaced ────────────────────
        "lsass.exe",          // Local Security Authority — moving breaks auth
        "csrss.exe",          // Client/Server Runtime Subsystem — kernel-adjacent
        "services.exe",       // Service Control Manager
        "winlogon.exe",       // Logon process
        "smss.exe",           // Session Manager
        "wininit.exe",        // Windows initialization
        // ── Audio engine (real-time scheduling, sensitive to interference)
        "audiodg.exe",        // Windows audio device graph isolation
    };
    for (const char* p : kProtected) {
#ifdef _WIN32
        if (_stricmp(exe_name, p) == 0) return true;
#else
        if (std::strcmp(exe_name, p) == 0) return true;
#endif
    }
    return false;
}

// Maximum eviction emissions per evaluate() cycle. Prevents pin avalanche
// when many low-CPU background processes simultaneously qualify. Tuned
// empirically — 8 covers typical "Discord + Steam + Chrome + a few helpers"
// without flooding the decision buffer.
static constexpr uint32_t kMaxCcdDefenseEmissions = 8u;

// Minimum CPU% for a process to be eligible for eviction. Below this,
// even being on V-Cache CCD is unlikely to contend meaningfully. Set to
// 0.5% as a balance between catching real contention and ignoring idle
// helpers that briefly touch CPU.
static constexpr float    kCcdDefenseCpuThresholdPct = 0.5f;

PolicyEngine::PolicyEngine() noexcept {
    rules_.fill(Rule{});
}

// ── Rule management ───────────────────────────────────────────────────────
Rule* PolicyEngine::find_rule(uint32_t id) noexcept {
    for (uint32_t i = 0u; i < n_rules_; ++i)
        if (rules_[i].id == id) return &rules_[i];
    return nullptr;
}

bool PolicyEngine::register_rule(const Rule& r) noexcept {
    if (n_rules_ >= kMaxRules) return false;
    rules_[n_rules_++] = r;
    return true;
}

void PolicyEngine::enable_rule(uint32_t rule_id) noexcept {
    if (auto* r = find_rule(rule_id)) r->enabled = true;
}

void PolicyEngine::disable_rule(uint32_t rule_id) noexcept {
    if (auto* r = find_rule(rule_id)) r->enabled = false;
}

// ── Default rules ─────────────────────────────────────────────────────────
void PolicyEngine::register_default_rules(
    const phyriad::HardwareTopology& topo) noexcept
{
    // Compute global_flags from topology
    global_flags_ = 0u;
    const auto vcache = phyriad::hw::v_cache_cores();
    const auto ecores = phyriad::hw::e_cores();
    if (!vcache.empty())              global_flags_ |= 0x1u;  // has_vcache
    if (!ecores.empty())              global_flags_ |= 0x2u;  // has_ecores
    if (topo.ccd_count() >= 2u)       global_flags_ |= 0x4u;  // multi_ccd

    // ── Rule 1: PinGameToVCacheCcd (AMD X3D only) ────────────────────────
    if (!vcache.empty()) {
        uint64_t vcache_mask = 0ull;
        for (uint32_t cid : vcache)
            vcache_mask |= (1ull << cid);

        Rule r = Rule::make(kRuleIdPinGameToVCache,
                            "PinGameToVCacheCcd",
                            ActionKind::PinAffinity,
                            vcache_mask, 90u);

        // Condition 0: target kind == Game
        r.conditions[r.n_conditions++] = Condition{
            ConditionField::TargetKind, ConditionOp::Equal,
            {}, static_cast<uint32_t>(observer::TargetKind::Game), 0.f, {}};

        // Condition 1: topology has V-Cache
        r.conditions[r.n_conditions++] = Condition{
            ConditionField::TopologyHasVCache, ConditionOp::Equal,
            {}, 1u, 0.f, {}};

        register_rule(r);
    }

    // ── Rule 2: PinGameToPCores (Intel hybrid only) ──────────────────────
    if (!ecores.empty()) {
        // P-core mask = all logical cores EXCLUDING e-cores
        uint64_t all_mask = (topo.logical_core_count() < 64u)
            ? ((1ull << topo.logical_core_count()) - 1ull)
            : ~0ull;
        uint64_t ecore_mask = 0ull;
        for (uint32_t eid : ecores) ecore_mask |= (1ull << eid);
        const uint64_t pcore_mask = all_mask & ~ecore_mask;

        Rule r = Rule::make(kRuleIdPinGameToPCores,
                            "PinGameToPCores",
                            ActionKind::PinAffinity,
                            pcore_mask, 85u);
        r.conditions[r.n_conditions++] = Condition{
            ConditionField::TargetKind, ConditionOp::Equal,
            {}, static_cast<uint32_t>(observer::TargetKind::Game), 0.f, {}};
        r.conditions[r.n_conditions++] = Condition{
            ConditionField::TopologyHasECores, ConditionOp::Equal,
            {}, 1u, 0.f, {}};

        register_rule(r);
    }

    // ── Rule 3: EvictStreamFromHotCcd (AMD X3D, any topology with partial V-Cache)
    //
    // The previous condition was `vcache.empty() == false && multi_ccd == true`.
    // Bug: topology detector reports 1 CCD on some 7950X3D units (bug #6),
    // causing this rule to never register on that hardware — exactly where it
    // is most needed (game + multi-stream).
    //
    // New condition: `non_vcache_mask != 0` — only register the rule if there
    // are cores WITHOUT V-Cache to evict Streams to. Covers:
    //   - 7800X3D (single CCD, all cores have V-Cache): non_vcache=0 → NO rule
    //   - 7950X3D (dual CCD, half with V-Cache): non_vcache=0xFFFF0000 → rule ✓
    //   - Any future AMD with partial V-Cache: handled correctly
    if (!vcache.empty()) {
        const uint64_t all_mask = (topo.logical_core_count() < 64u)
            ? ((1ull << topo.logical_core_count()) - 1ull)
            : ~0ull;
        uint64_t vcache_mask = 0ull;
        for (uint32_t cid : vcache) vcache_mask |= (1ull << cid);
        const uint64_t non_vcache_mask = all_mask & ~vcache_mask;

        if (non_vcache_mask != 0ull) {
            Rule r = Rule::make(kRuleIdEvictStreamFromHotCcd,
                                "EvictStreamFromHotCcd",
                                ActionKind::PinAffinity,
                                non_vcache_mask, 80u);
            r.conditions[r.n_conditions++] = Condition{
                ConditionField::TargetKind, ConditionOp::Equal,
                {}, static_cast<uint32_t>(observer::TargetKind::Stream), 0.f, {}};
            r.conditions[r.n_conditions++] = Condition{
                ConditionField::AnyGameRunning, ConditionOp::Equal,
                {}, 1u, 0.f, {}};
            r.conditions[r.n_conditions++] = Condition{
                ConditionField::TopologyHasVCache, ConditionOp::Equal,
                {}, 1u, 0.f, {}};

            register_rule(r);
        }
    }

    // ── Rule 6: EvictCommFromHotCcd (AMD X3D, multi-stream + comm scenarios)
    //
    // For streamer setups: Discord/Teams/Zoom running alongside a game.
    // Discord on X3D V-Cache CCD competes with the game thread.
    // Evicting them to the non-V-Cache CCD frees bandwidth for the game.
    //
    // Same logic as Rule 3 (EvictStreamFromHotCcd) but for TargetKind::Comm.
    if (!vcache.empty()) {
        const uint64_t all_mask = (topo.logical_core_count() < 64u)
            ? ((1ull << topo.logical_core_count()) - 1ull)
            : ~0ull;
        uint64_t vcache_mask = 0ull;
        for (uint32_t cid : vcache) vcache_mask |= (1ull << cid);
        const uint64_t non_vcache_mask = all_mask & ~vcache_mask;

        if (non_vcache_mask != 0ull) {
            Rule r = Rule::make(kRuleIdEvictCommFromHotCcd,
                                "EvictCommFromHotCcd",
                                ActionKind::PinAffinity,
                                non_vcache_mask, 70u);  // slightly lower than stream
            r.conditions[r.n_conditions++] = Condition{
                ConditionField::TargetKind, ConditionOp::Equal,
                {}, static_cast<uint32_t>(observer::TargetKind::Comm), 0.f, {}};
            r.conditions[r.n_conditions++] = Condition{
                ConditionField::AnyGameRunning, ConditionOp::Equal,
                {}, 1u, 0.f, {}};
            r.conditions[r.n_conditions++] = Condition{
                ConditionField::TopologyHasVCache, ConditionOp::Equal,
                {}, 1u, 0.f, {}};

            register_rule(r);
        }
    }

    // ── Rule 4: IsolateGameFromBackground (Intel hybrid: E-cores for Comm)
    if (!ecores.empty()) {
        uint64_t ecore_mask = 0ull;
        for (uint32_t eid : ecores) ecore_mask |= (1ull << eid);

        Rule r = Rule::make(kRuleIdIsolateGameFromBg,
                            "PinCommToECores",
                            ActionKind::PinAffinity,
                            ecore_mask, 75u);
        r.conditions[r.n_conditions++] = Condition{
            ConditionField::TargetKind, ConditionOp::Equal,
            {}, static_cast<uint32_t>(observer::TargetKind::Comm), 0.f, {}};
        r.conditions[r.n_conditions++] = Condition{
            ConditionField::AnyGameRunning, ConditionOp::Equal,
            {}, 1u, 0.f, {}};

        register_rule(r);
    }

    // ── Rule 8: CCD Load Defense ──────────────────
    //
    // Generalization of Rules 3/4/6 (which only evict specific kinds —
    // Stream/Comm). Rule 8 evicts ANY non-Game non-System process from
    // the V-Cache CCD when it consumes meaningful CPU% during gameplay,
    // freeing V-Cache resources (shared L3, memory bandwidth on CCD0)
    // for the game.
    //
    // Selection is **scored** in evaluate() (top-K by CPU%), not naive
    // emit-for-all, to avoid pin avalanche when many background processes
    // are slightly active. K = kMaxCcdDefenseEmissions per cycle.
    //
    // Lower priority (60) than Rules 3/4/6 (70-80) so that when a Comm
    // or Stream process matches multiple rules, the more specific one
    // wins. ActionExecutor's find_active() dedup handles the conflict.
    //
    // Only registers when hardware has V-Cache AND is multi-CCD —
    // single-CCD AMD X3D (e.g. 7800X3D) has nothing to defend.
    if (!vcache.empty() && topo.ccd_count() >= 2u) {
        const uint64_t all_mask = (topo.logical_core_count() < 64u)
            ? ((1ull << topo.logical_core_count()) - 1ull)
            : ~0ull;
        uint64_t vcache_mask = 0ull;
        for (uint32_t cid : vcache) vcache_mask |= (1ull << cid);
        const uint64_t non_vcache_mask = all_mask & ~vcache_mask;

        if (non_vcache_mask != 0ull) {
            Rule r = Rule::make(kRuleIdCcdLoadDefense,
                                "CcdLoadDefense",
                                ActionKind::PinAffinity,
                                non_vcache_mask, 60u);
            // Note: Rule 8 uses NO conditions stored in the Rule struct.
            // The selection logic is handled specially in evaluate() because
            // it requires scoring + top-K filtering across ALL non-Game
            // targets, which the standard rule iteration cannot express.
            // The Rule struct is registered only to expose its name +
            // core_mask + priority for UI display and revert audit.
            register_rule(r);
        }
    }
}

// ── evaluate() ────────────────────────────────────────────────────────────
uint32_t PolicyEngine::evaluate(
    const observer::TargetProcess* targets, uint32_t n_targets,
    const observer::TargetMetrics* metrics, uint32_t n_metrics,
    PolicyDecision* out_decisions) noexcept
{
    if (!targets || !metrics || !out_decisions ||
        n_targets == 0u || n_rules_ == 0u) return 0u;

    const uint32_t n = std::min(n_targets, n_metrics);

    // Compute any_game_running flag
    uint8_t any_game = 0u;
    for (uint32_t i = 0u; i < n; ++i) {
        if (targets[i].kind == observer::TargetKind::Game) {
            any_game = 1u;
            break;
        }
    }

    uint32_t n_decisions = 0u;

    for (uint32_t ti = 0u; ti < n; ++ti) {
        const observer::TargetProcess& tp = targets[ti];
        const observer::TargetMetrics& tm = metrics[ti];

        for (uint32_t ri = 0u; ri < n_rules_; ++ri) {
            const Rule& rule = rules_[ri];
            if (!rule.enabled) continue;
            if (rule.action_kind == ActionKind::None) continue;
            // Rule 8 (CCD Load Defense) is handled in a separate scoring
            // pass below — it has no stored conditions because selection
            // requires top-K filtering across all candidates.
            if (rule.id == kRuleIdCcdLoadDefense) continue;

            // Check all conditions (AND logic)
            bool match = true;
            for (uint32_t ci = 0u; ci < rule.n_conditions && match; ++ci) {
                match = evaluate_condition(rule.conditions[ci], tp, tm,
                                          global_flags_, any_game);
            }

            if (!match) continue;

            // Emit decision
            if (n_decisions >= kMaxDecisionsPerCycle) break;

            // when differential pin mode is enabled
            // AND this is the Rule-1 V-Cache pin for a Game with a stable
            // hot_tid identified, override rule_id with Rule 7. Downstream
            // ActionExecutor routes Rule 7 through apply_differential_pin
            // (releases process, pins only the hot thread to V-Cache).
            //
            // This is the SINGLE point of switching between modes — no
            // duplicate decisions emitted. Keeps PolicyDecision IPC layout
            // unchanged (still 32B; rule_id alone differentiates).
            uint32_t emitted_rule_id = rule.id;
            if (differential_pin_enabled_
                && rule.id == kRuleIdPinGameToVCache
                && tp.kind == observer::TargetKind::Game
                && tm.hot_tid != 0u)
            {
                emitted_rule_id = kRuleIdPinHotThreadDifferential;
            }

            PolicyDecision& d = out_decisions[n_decisions++];
            d.target_pid     = tp.pid;
            d.rule_id        = emitted_rule_id;
            d.action_kind    = rule.action_kind;
            d.confidence     = rule.confidence;
            d._pad[0]        = 0u;
            d._pad[1]        = 0u;
            d.core_mask      = rule.core_mask;
            d.priority_class = rule.priority_class;
            d.decided_tsc    = 0ull;  // filled by ActionExecutor
        }
    }

    // ── CCD Load Defense scoring pass ─────────────
    //
    // Selection process:
    //   1. Filter: must have any_game running; rule must be registered/enabled
    //   2. For each target:
    //      - Skip Game and System kinds (Game IS the protected workload;
    //        System is OS infrastructure)
    //      - Skip if CPU% below threshold (idle process, doesn't contend)
    //      - Skip if exe is in the protected list (Ayama, anti-cheat, etc.)
    //      - Skip if a decision is ALREADY emitted for this PID by an
    //        earlier rule (Rules 3/4/6 are more specific; let them win)
    //   3. Score by CPU% (higher = more contention pressure)
    //   4. Sort descending and emit the top K
    //
    // The emit-LAST ordering ensures that more specific rules (Rule 3
    // for Stream, Rule 6 for Comm, etc.) take precedence via the
    // "already emitted for this PID" guard. Rule 8 fills only the gap.
    last_ccd_defense_count_   = 0u;
    last_ccd_defense_cpu_pct_ = 0.0f;
    {
        const Rule* defense = find_rule(kRuleIdCcdLoadDefense);
        // find_rule is non-const member; we call const-safe path via
        // const_cast through const member. find_rule itself returns a
        // mutable pointer — we only read here so this is fine.
        if (defense && defense->enabled && any_game == 1u
            && n_decisions < kMaxDecisionsPerCycle)
        {
            // Buffer of candidate (target_idx, score) pairs. n ≤ kMaxTargets.
            struct Candidate {
                uint32_t target_idx;
                float    score;
            };
            // Use observer::kMaxTargets as the upper bound — matches the
            // metrics_buf size in AgentRuntime.
            Candidate candidates[observer::kMaxTargets];
            uint32_t  n_candidates = 0u;

            for (uint32_t i = 0u; i < n; ++i) {
                const observer::TargetProcess& tp = targets[i];
                const observer::TargetMetrics& tm = metrics[i];

                // Filter: skip Game (the workload we're protecting) and
                // System (OS infrastructure that's already exempted).
                if (tp.kind == observer::TargetKind::Game) continue;
                if (tp.kind == observer::TargetKind::System) continue;

                // CPU threshold: ignore idle/near-idle processes that
                // don't contribute meaningfully to CCD contention.
                if (tm.cpu_usage_pct < kCcdDefenseCpuThresholdPct) continue;

                // Protected list: Ayama's own, anti-cheat, critical OS.
                if (is_protected_from_ccd_defense(tp.name)) continue;

                // Skip if a more specific rule already emitted a decision
                // for this PID earlier in this evaluate() cycle.
                bool already_handled = false;
                for (uint32_t k = 0u; k < n_decisions; ++k) {
                    if (out_decisions[k].target_pid == tp.pid) {
                        already_handled = true;
                        break;
                    }
                }
                if (already_handled) continue;

                candidates[n_candidates].target_idx = i;
                candidates[n_candidates].score      = tm.cpu_usage_pct;
                ++n_candidates;
            }

            // Sort descending by score (highest CPU% first). std::sort is
            // O(n log n) and n ≤ 64 here, so the cost is negligible
            // (~hundreds of compares per tick).
            std::sort(candidates, candidates + n_candidates,
                [](const Candidate& a, const Candidate& b) noexcept {
                    return a.score > b.score;
                });

            // Emit top K decisions, respecting both kMaxCcdDefenseEmissions
            // and the overall PolicyDecision buffer cap.
            const uint32_t k_max = std::min(n_candidates,
                                            kMaxCcdDefenseEmissions);
            float total_cpu = 0.0f;
            for (uint32_t k = 0u; k < k_max &&
                    n_decisions < kMaxDecisionsPerCycle; ++k)
            {
                const observer::TargetProcess& tp =
                    targets[candidates[k].target_idx];

                PolicyDecision& d = out_decisions[n_decisions++];
                d.target_pid     = tp.pid;
                d.rule_id        = kRuleIdCcdLoadDefense;
                d.action_kind    = defense->action_kind;  // PinAffinity
                d.confidence     = defense->confidence;
                d._pad[0]        = 0u;
                d._pad[1]        = 0u;
                d.core_mask      = defense->core_mask;    // non_vcache_mask
                d.priority_class = defense->priority_class;
                d.decided_tsc    = 0ull;

                total_cpu += candidates[k].score;
                ++last_ccd_defense_count_;
            }
            last_ccd_defense_cpu_pct_ = total_cpu;
        }
    }

    return n_decisions;
}

} // namespace ayama::policy
// Made with my soul - Swately <3
