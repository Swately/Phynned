// ipc/include/phynned/ipc/PhynnedProtocol.hpp
// PhynnedProtocol — shared memory layout between phynned-agent and UI clients.
//
// Layout (1 MB total):
//   [0..127]            PhynnedShmHeader   (magic, version, agent_pid, seqlock)
//   [128..255]          PhynnedStateHeader (counts, flags)
//   [256..4351]         TargetProcess[64]   (64 × 64  = 4096 B)
//   [4352..12543]       TargetMetrics[64]   (64 × 128 = 8192 B)
//   [12544..13055]      PolicyDecision[16]  (16 × 32  = 512  B)
//   [13056..20607]      ActionLogRing       (phyriad::ipc::Ring<ActionLogEntry,128>
//                                            = 3×128B atomics + 128×56B slots
//                                            = 384 + 7168 = 7552 B)
//   [20608..20671]      PhynnedCommandSlot   (64 B — UI→agent IPC commands)
//   [20672..45767]      UserRulesBlock       (8 + 128×196 = 25096 B — W3 per-process
//                                            user rules published for UI display)
//   [45768..1048575]    Baseline ring buffer (~1 MB - 45 KB)
//
// USE-MODES layout growth (2026-07-19): UserRulesBlock was appended AFTER
// command_slot. Every offset ≤ 20671 (header/state/targets/metrics/decisions/
// action_log/command_slot) is byte-for-byte unchanged; only the conceptual
// "baseline ring start" (which is just the tail of the mapping, never a struct
// member) moves from 20672 to 45768. sizeof(PhynnedShmLayout) stays well under
// the 1 MB mapping. Agent + UI rebuild together (established practice).
//
// SHM view sized by kMaxShmTargets (64) — the bounded agent↔UI contract. This is
// DECOUPLED from the internal tracking cap observer::kMaxTargets (now 1024 for the
// MASS-router): the agent tracks up to 1024 processes internally but publishes only the
// top-kMaxShmTargets into this layout, so the SHM size/offsets are byte-for-byte
// unchanged across the MASS widening.
// ABI version stays at 1 because clients re-read the whole struct via ShmRegion — they
// don't have hardcoded offsets. If client compatibility breaks, bump kPhynnedShmVersion.
//
// Seqlock pattern:
//   Agent writes: seq++ (now odd) → memcpy → seq++ (now even).
//   UI reads:     read seq → memcpy → read seq again. Retry if changed.
//
// Threading: agent = single writer; UI = multiple readers (no lock).
// Privilege: CreateFileMapping (agent), OpenFileMapping (clients) — no admin.
//
#pragma once

#include <phynned/observer/TargetProcess.hpp>
#include <phynned/observer/TargetMetrics.hpp>
#include <phynned/policy/PolicyDecision.hpp>
#include <phynned/action/ActionLog.hpp>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <phyriad/hal/MemoryOrder.hpp>

namespace phynned::ipc {

inline constexpr uint32_t kPhynnedShmMagic   = 0x41594D41u;  // 'AYMA'
inline constexpr uint32_t kPhynnedShmVersion = 1u;
inline constexpr uint64_t kPhynnedShmSize    = 1u * 1024u * 1024u;  // 1 MB

/// Shared memory header (128 bytes, first thing in the mapping).
struct alignas(64) PhynnedShmHeader {
    uint32_t            magic;          //  4B  — 'AYMA' sentinel
    uint32_t            version;        //  4B
    std::atomic<uint32_t> agent_pid;   //  4B
    std::atomic<uint32_t> seq;         //  4B  — seqlock counter (odd=writing)
    uint8_t             _pad[112];     //112B  — fill to 128B
};
static_assert(sizeof(PhynnedShmHeader) == 128);

/// High-level counts / flags (128 bytes).
struct alignas(64) PhynnedStateHeader {
    uint32_t n_targets;          //  4B  @ 0
    uint32_t n_decisions;        //  4B  @ 4
    uint32_t n_active_actions;   //  4B  @ 8
    uint8_t  agent_connected;    //  1B  @ 12 — 1 when agent is up
    uint8_t  privilege_level;    //  1B  @ 13 — 0=none, 1=admin-no-ETW, 2=admin+ETW
    uint8_t  etw_active;         //  1B  @ 14
    uint8_t  bench_phase;        //  1B  @ 15 — 0=idle, 1=A, 2=B, 3=done
    uint32_t total_migrations;   //  4B  @ 16 — sum across all targets
    float    aggregate_pressure; //  4B  @ 20 — 0.0..1.0 avg pressure
    uint64_t last_publish_tsc;   //  8B  @ 24
    // ── Extended fields added in v1 (previously _pad) ─────────────────────
    uint32_t bad_count;          //  4B  @ 32 — per-game bad-list entry count
    uint8_t  deep_idle;          //  1B  @ 36 — 1 if WorkloadState::DeepIdle
    uint8_t  watchdog_ok;        //  1B  @ 37 — 1 normally; 0 if stall detected
    // ── CCD Load Defense telemetry ────────────────────────────
    uint8_t  _pad1[2];           //  2B  @ 38 — align next uint32_t to 40
    uint32_t ccd_defense_count;  //  4B  @ 40 — # processes evicted from V-Cache CCD
    float    ccd_defense_cpu_pct;//  4B  @ 44 — summed CPU% of those processes
    // ── Hardware classification (publication v1.0) ────────────────────────
    // Written once at agent startup via set_hw_classification(); does not
    // change tick-to-tick. UI uses these bytes to render arch-aware text
    // (X3D vs HybridIntel vs MultiCCXNoX3D vs SingleCCD) without having to
    // depend on the policy header.
    uint8_t  cpu_class;          //  1B  @ 48 — policy::CpuClass enum value:
                                 //         0=Unknown, 1=X3DSingle, 2=X3DDual,
                                 //         3=HybridIntel, 4=MultiCCXNoX3D, 5=SingleCCD
    uint8_t  ccd_count;          //  1B  @ 49 — # of CCDs detected (Intel = 1)
    uint8_t  p_core_count;       //  1B  @ 50 — # P-cores (Intel hybrid only; 0 otherwise)
    uint8_t  e_core_count;       //  1B  @ 51 — # E-cores (Intel hybrid only; 0 otherwise)
    // ── Runtime control state ─────────────────────────────────
    uint8_t  policies_paused;    //  1B  @ 52 — 1 if executor.apply() is gated
                                 //              off (Start/Pause/Reset UI flow)
    uint8_t  _pad2a[3];          //  3B  @ 53 — align next float to 4-byte boundary
    // ── Agent self-resource accounting (SelfMonitor → SHM) ─────
    // Published every ~500ms by AgentRuntime; ~0 during DeepIdle, single-
    // digit % during active classification. The status bar surfaces these
    // so the user can verify Phynned isn't burning their CPU.
    float    self_cpu_pct;       //  4B  @ 56 — agent process CPU% (0..100)
    float    self_rss_mb;        //  4B  @ 60 — agent process RSS in megabytes
    // ── MASS-router detection scale (2026-07-17) ──────────────
    // The agent tracks up to observer::kMaxTargets (1024) processes internally;
    // only the top n_targets (≤ kMaxShmTargets = 64) are copied into targets[]
    // above. n_tracked_total is the FULL internal count so the UI can show the
    // true detection scale ("showing N of M"). Carved from the former _pad2b
    // reserved space → PhynnedStateHeader stays 128B and offsets 0..63 are
    // unchanged (SHM size/offset-identical; agent + UI rebuild together).
    uint32_t n_tracked_total;    //  4B  @ 64 — total touchable processes observed
    // ── MR-2 background corral mode (2026-07-17) ──────────────
    // Carved from the former _pad2b reserved space → PhynnedStateHeader stays 128B
    // and offsets 0..67 are unchanged (SHM size/offset-identical).
    uint8_t  corral_live;        //  1B  @ 68 — 1 if the LIVE corral switch is ON
                                 //              (0 = DRY-RUN, the safe default)
    uint8_t  corral_coexist_block;// 1B  @ 69 — 1 if a coexistence optimizer (AMD
                                 //              3D V-Cache service / Process Lasso)
                                 //              is detected → forced DRY-RUN (E5)
    // ── W4 global profile selector (use-modes, 2026-07-19) ────────────
    // Carved from the former _pad2b reserved space → PhynnedStateHeader stays 128B
    // and every offset 0..69 is unchanged (SHM size/offset-identical). This is the
    // ONLY new state-header field for the use-modes work; keep_placements_on_disable
    // is deliberately NOT published (the UI keeps its own checkbox state) so no
    // second byte is carved.
    uint8_t  profile;            //  1B  @ 70 — config::Profile: 0=Monitor 1=Games
                                 //              2=GamesCorral(default) 3=Full
    uint8_t  _pad2b[57];         // 57B  @ 71 — fill to 128B
};
static_assert(sizeof(PhynnedStateHeader) == 128);
static_assert(offsetof(PhynnedStateHeader, corral_live)         == 68);
static_assert(offsetof(PhynnedStateHeader, corral_coexist_block) == 69);
static_assert(offsetof(PhynnedStateHeader, profile)             == 70);
static_assert(offsetof(PhynnedStateHeader, policies_paused) == 52);
static_assert(offsetof(PhynnedStateHeader, self_cpu_pct)    == 56);
static_assert(offsetof(PhynnedStateHeader, self_rss_mb)     == 60);
static_assert(offsetof(PhynnedStateHeader, ccd_defense_count)   == 40);
static_assert(offsetof(PhynnedStateHeader, ccd_defense_cpu_pct) == 44);
static_assert(offsetof(PhynnedStateHeader, cpu_class)           == 48);
static_assert(offsetof(PhynnedStateHeader, ccd_count)           == 49);
static_assert(offsetof(PhynnedStateHeader, n_tracked_total)     == 64);

// ── PhynnedCommandSlot ─────────────────────────────────────────────────────
// UI → agent command channel.
//
// Synchronization: client writes args first, then bumps `seq` with release.
// Agent polls `seq` with acquire; when it differs from `last_processed_seq_`,
// processes the command and stores the seq back as ack.
//
// kPhynnedCmdNone is the initial state. Commands are idempotent — re-issuing
// the same command with the same seq is a no-op (agent only acts on new seq).
//
// Threading: writer = UI thread of one client; reader = agent main thread.
// Multiple clients sharing the slot would race — single-writer assumption
// holds because typically only one Phynned UI instance runs per session.
enum PhynnedCmdKind : uint32_t {
    kPhynnedCmdNone                  = 0u,
    kPhynnedCmdPausePolicies         = 1u,  ///< Revert all active, suspend new applies
    kPhynnedCmdResumePolicies        = 2u,  ///< Re-enable applies
    kPhynnedCmdForceRevertAll        = 3u,  ///< Revert all but do NOT suspend (single-shot)
    kPhynnedCmdSetDifferentialPin    = 4u,  ///< Toggle Rule 7 differential pin mode.
                                          ///<  arg1 = 0 → disable (default Rule 1).
                                          ///<  arg1 = 1 → enable (differential pin).
    kPhynnedCmdSetCorralLive         = 5u,  ///< MR-2: toggle the background corral
                                          ///<  LIVE switch (moves active non-game
                                          ///<  background off the V-Cache CCD).
                                          ///<  arg1 = 0 → DRY-RUN (default, safe).
                                          ///<  arg1 = 1 → LIVE (applies real affinity).
                                          ///<  arg2 = keep_placements_on_disable
                                          ///<        (0/1) — piggybacked so the UI
                                          ///<        "keep on disable" checkbox rides
                                          ///<        the same command (W2).
    // ── Use-modes (W2/W3/W4, 2026-07-19) ──────────────────────────────
    kPhynnedCmdSetProcessRule        = 6u,  ///< W3: create/update a per-process user
                                          ///<  rule. target_pid = the process to name
                                          ///<  (agent resolves exe name); arg1 = action
                                          ///<  (0=Never 1=Freq 2=VCache). Duplicate name
                                          ///<  updates the action in place. Agent saves
                                          ///<  policies.toml (single writer).
    kPhynnedCmdRemoveProcessRule     = 7u,  ///< W3: remove a user rule. arg1 = SHM slot
                                          ///<  index; arg2 = rules-block generation
                                          ///<  (stale generation ⇒ ignored). Agent saves.
    kPhynnedCmdSetProfile            = 8u,  ///< W4: set the global profile. arg1 =
                                          ///<  config::Profile (0=Monitor 1=Games
                                          ///<  2=GamesCorral 3=Full). Leaving the corral
                                          ///<  reverts corral placements; →Monitor also
                                          ///<  reverts user pins. Agent saves.
};

struct alignas(64) PhynnedCommandSlot {
    std::atomic<uint64_t> seq;            //  8B  — bumped by client on submit
    std::atomic<uint64_t> ack;            //  8B  — bumped by agent after processing
    uint32_t              cmd_kind;       //  4B  — PhynnedCmdKind
    uint32_t              target_pid;     //  4B  — optional argument
    uint64_t              arg1;           //  8B
    uint64_t              arg2;           //  8B
    uint8_t               _pad[24];       // 24B  — fill to 64B
};
static_assert(sizeof(PhynnedCommandSlot) == 64);

// ── UserRulesBlock (W3 — per-process user rules published for the UI) ────────
// The agent is the SINGLE writer of policies.toml; it mirrors the active user
// rules here each publish so the UI can render the rules editor + per-row badges
// WITHOUT reading the config file. Trivially copyable POD, seqlock-wrapped like
// the rest of the layout.
inline constexpr uint32_t kMaxUserRulesShm = 128u;  ///< matches config::kMaxProcessRules

/// Per-rule flag bits carried in UserRuleShm::flags.
inline constexpr uint8_t kUserRuleFlagHasPath   = 0x01u; ///< rule has a non-empty path
inline constexpr uint8_t kUserRuleFlagBlockedAc = 0x02u; ///< R1: an apply was refused by the AC gate
inline constexpr uint8_t kUserRuleFlagFlapWarn  = 0x04u; ///< R4: skipped — pinned by another manager

/// One published user rule (196 B). action: 0=Never 1=Freq 2=VCache.
struct UserRuleShm {
    char    name[64];   //  64B — exe basename, null-terminated (match key)
    char    path[128];  // 128B — optional full path ("" = match any path)
    uint8_t action;     //   1B — 0=Never 1=Freq 2=VCache
    uint8_t flags;      //   1B — kUserRuleFlag* bits (has_path / blocked_by_ac / flap_warn)
    uint8_t _pad[2];    //   2B — fill to 196B
};
static_assert(sizeof(UserRuleShm) == 196);

/// Published user-rules table. `generation` bumps on every mutation so a stale
/// UI RemoveProcessRule (slot index) can be rejected by the agent.
struct UserRulesBlock {
    uint32_t    n_rules;                       //  4B
    uint32_t    generation;                    //  4B
    UserRuleShm rules[kMaxUserRulesShm];       // 128 × 196 = 25088 B
};
static_assert(sizeof(UserRulesBlock) == 8u + 128u * 196u);

/// Complete shared memory layout — accessed via pointer to mapped region.
struct PhynnedShmLayout {
    PhynnedShmHeader   header;                                      // 128B @ 0
    PhynnedStateHeader state;                                       // 128B @ 128
    observer::TargetProcess  targets[observer::kMaxShmTargets];  //4096B @ 256   (64 × 64)
    observer::TargetMetrics  metrics[observer::kMaxShmTargets];  //8192B @ 4352  (64 × 128)
    policy::PolicyDecision   decisions[policy::kMaxDecisionsPerCycle]; //512B @ 12544
    action::ActionLogRing    action_log;                          //~7552B @ 13056
    PhynnedCommandSlot         command_slot;                        //  64B @ 20608
    UserRulesBlock             user_rules;                          //25096B @ 20672
    // Baseline ring fills the rest of the 1 MB mapping (starts @ 45768).
};
// Offsets fixed by kMaxShmTargets=64 (unchanged by the MASS kMaxTargets widening).
// command_slot offset added; user_rules appended AFTER it (all ≤20671 unchanged).
static_assert(offsetof(PhynnedShmLayout, targets)      ==    256u);
static_assert(offsetof(PhynnedShmLayout, metrics)      ==   4352u);
static_assert(offsetof(PhynnedShmLayout, decisions)    ==  12544u);
static_assert(offsetof(PhynnedShmLayout, command_slot) ==  20608u);
static_assert(offsetof(PhynnedShmLayout, user_rules)   ==  20672u);
static_assert(sizeof(PhynnedShmLayout)  <  kPhynnedShmSize);  // R5: fits the 1 MB mapping

// ── Seqlock helpers ───────────────────────────────────────────────────────
/// Begin a write transaction: seq becomes odd.
inline void shm_write_begin(PhynnedShmHeader* h) noexcept {
    h->seq.fetch_add(1u, std::memory_order_acq_rel);  // HAL: acq_rel fetch_add — synchronising counter
}
/// End a write transaction: seq becomes even.
inline void shm_write_end(PhynnedShmHeader* h) noexcept {
    h->seq.fetch_add(1u, std::memory_order_acq_rel);  // HAL: acq_rel fetch_add — synchronising counter
}
/// Read the state header consistently (retry up to 4 times).
/// Returns true if a consistent read was obtained.
inline bool shm_read_consistent(const PhynnedShmHeader* h,
                                 const PhynnedStateHeader* src,
                                 PhynnedStateHeader* dst) noexcept {
    for (int attempt = 0; attempt < 4; ++attempt) {
        const uint32_t s0 = phyriad::hal::seq_load_acquire(h->seq);
        if (s0 & 1u) continue;  // writer in progress
        std::memcpy(dst, src, sizeof(*dst));
        const uint32_t s1 = phyriad::hal::seq_load_acquire(h->seq);
        if (s0 == s1) return true;
    }
    return false;  // use stale data
}

} // namespace phynned::ipc
// Made with my soul - Swately <3
