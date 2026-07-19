// observer/include/phynned/observer/TargetProcess.hpp
// TargetProcess — POD descriptor of an observed external process.
//
// IPC-safe: trivially_copyable, standard_layout, 64 bytes (1 cache line).
// Crosses SHM boundary between phynned-agent and UI clients.
//
// Threading: read/write from agent main thread only; UI reads via seqlock SHM.
// Resource:  negligible (64B per slot, max 32 slots = 2 KB).
// Privilege: None (read-only PID enumeration does not require admin).
//
#pragma once
#include <cstdint>

namespace phynned::observer {

/// Classification of a target process by its role.
enum class TargetKind : uint8_t {
    Unknown      = 0,  ///< Unclassified — no action taken in Auto mode.
    Game         = 1,  ///< Fullscreen graphical application (game).
    Stream       = 2,  ///< Streaming encoder (OBS, Streamlabs, XSplit...).
    Comm         = 3,  ///< Communication app (Discord, Teams, Zoom...).
    Browser      = 4,  ///< Web browser.
    Productivity = 5,  ///< Creative/dev tool (Blender, VS, DaVinci...).
    System       = 6,  ///< OS/driver process — never touched by Phynned.
};

/// Lifecycle status of a tracked process.
enum class TargetStatus : uint8_t {
    Running   = 0,
    Suspended = 1,
    Exiting   = 2,
};

/// POD descriptor — 64 bytes, 1 cache line.
struct alignas(8) TargetProcess {
    uint32_t     pid;             //  4B  — process ID
    uint32_t     parent_pid;      //  4B  — parent PID
    uint64_t     start_tsc;       //  8B  — TSC at first observation
    char         name[40];        // 40B  — null-terminated exe short name (ASCII)
    TargetStatus status;          //  1B
    TargetKind   kind;            //  1B
    uint8_t      rules_matched;   //  1B  — bitmask of matching rules (max 8 rules)
    uint8_t      _pad[5];         //  5B  — explicit padding
};
static_assert(sizeof(TargetProcess)         == 64,  "TargetProcess must be 64B");
static_assert(alignof(TargetProcess)        == 8,   "TargetProcess must be 8B-aligned");
static_assert(__is_trivially_copyable(TargetProcess),  "TargetProcess must be trivially copyable");

/// Maximum number of simultaneously observed targets (INTERNAL tracking cap).
///
/// Bump history:
///   - 32 (original): sufficient for casual user with 1 game + 2-3 background apps
///   - 64 (bug #18): a single real streamer (Chrome 8 + Code 8 + Discord 6 + msedgewebview2 6
///     + obs64 1 + ...) consumed all 32 slots before the game (Minecraft with high PID)
///     could enter the table. With 64 slots + per-name cap=8, 8 unique exe names fit.
///   - 1024 (MASS-router, BR1, 2026-07-17): detection inverted from pattern-gated to
///     track-all-touchable. Hundreds of background processes are now OBSERVED (not
///     necessarily placed). Sized to phyriad::proc::kMaxProcesses (1024) so a busy box
///     is never truncated. This is an INTERNAL cap only — the agent↔UI SHM view stays
///     bounded at `kMaxShmTargets` (below), decoupling mass-scale from the UI contract.
///
/// NOTE: this cap no longer sizes the SHM `targets[]`/`metrics[]` arrays — those use
/// `kMaxShmTargets`. Widening kMaxTargets therefore does NOT change the SHM layout size.
inline constexpr uint32_t kMaxTargets = 1024u;

/// Maximum number of targets PUBLISHED into the agent↔UI shared memory (the UI
/// contract). The UI needs only a bounded "top-N interesting" view — it never needs
/// the full mass set. Kept at 64 so the SHM layout (PhynnedShmLayout) is byte-for-byte
/// unchanged across the MASS widening:
///   targets[64]  = 64 × 64  = 4096 B (exactly 1 page)
///   metrics[64]  = 64 × 128 = 8192 B (2 pages)
/// The agent selects the top-`kMaxShmTargets` (placed/active/highest-activity) each tick
/// and copies only those into the SHM; the full 1024-deep set is tracked internally.
///
/// NOT bound by `phyriad::schema::PodMessage` 4096B limit — el SHM layout no es
/// PodMessage (no cruza `phyriad::ipc::Ring<T>` ni `phyriad::transport::Latest<T>`).
inline constexpr uint32_t kMaxShmTargets = 64u;

} // namespace phynned::observer
// Made with my soul - Swately <3
