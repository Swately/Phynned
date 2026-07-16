// apps/ayama/observer/include/ayama/observer/TargetProcess.hpp
// TargetProcess — POD descriptor of an observed external process.
//
// IPC-safe: trivially_copyable, standard_layout, 64 bytes (1 cache line).
// Crosses SHM boundary between ayama-agent and UI clients.
//
// Threading: read/write from agent main thread only; UI reads via seqlock SHM.
// Resource:  negligible (64B per slot, max 32 slots = 2 KB).
// Privilege: None (read-only PID enumeration does not require admin).
//
#pragma once
#include <cstdint>

namespace ayama::observer {

/// Classification of a target process by its role.
enum class TargetKind : uint8_t {
    Unknown      = 0,  ///< Unclassified — no action taken in Auto mode.
    Game         = 1,  ///< Fullscreen graphical application (game).
    Stream       = 2,  ///< Streaming encoder (OBS, Streamlabs, XSplit...).
    Comm         = 3,  ///< Communication app (Discord, Teams, Zoom...).
    Browser      = 4,  ///< Web browser.
    Productivity = 5,  ///< Creative/dev tool (Blender, VS, DaVinci...).
    System       = 6,  ///< OS/driver process — never touched by Ayama.
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

/// Maximum number of simultaneously observed targets.
///
/// Bump history:
///   - 32 (original): sufficient for casual user with 1 game + 2-3 background apps
///   - 64 (bug #18): a single real streamer (Chrome 8 + Code 8 + Discord 6 + msedgewebview2 6
///     + obs64 1 + ...) consumed all 32 slots before the game (Minecraft with high PID)
///     could enter the table. With 64 slots + per-name cap=8, 8 unique exe names fit.
///
/// Constraint: SHM layout (AyamaShmLayout) uses kMaxTargets for
/// `targets[]` and `metrics[]` arrays. At 64 slots:
///   targets[64]  = 64 × 64  = 4096 B (exactly 1 page)
///   metrics[64]  = 64 × 128 = 8192 B (2 pages)
/// Total SHM usado: ~20 KB de 1 MB mapping → 2% utilizado, sobra mucho.
///
/// NOT bound by `phyriad::schema::PodMessage` 4096B limit — el SHM layout no es
/// PodMessage (no cruza `phyriad::ipc::Ring<T>` ni `phyriad::transport::Latest<T>`).
inline constexpr uint32_t kMaxTargets = 64u;

} // namespace ayama::observer
// Made with my soul - Swately <3
