// framework/schema/tests/schema_hash_conformance_test.cpp
//
// CONFORMANCE GATE for the consteval schema fingerprint (§W.1 / CPU_SUBSTRATE_PRIOR_ART.md).
//
// schema_hash<T>() is ABI-LOAD-BEARING: it is baked into every binary and a stale
// attacher fails at startup on mismatch. The property that MUST hold is STABILITY —
// the hash of a fixed input must NOT silently drift across builds/commits (a drift
// invalidates every persistent / cross-process artifact). This gate pins that.
//
// It is NOT an upstream-xxHash-parity test: xxh3_inline.hpp is a derived consteval
// PORT (the long-input path is a simplified single-block variant — see its header),
// and schema_hash needs only a stable, deterministic, collision-resistant 128-bit
// fingerprint, NOT bit-compatibility with upstream. Goldens below were captured on
// the x86 ABI (kDestructivePad=128, kPhyriadHalVersion=7); a deliberate ABI change
// (HAL version bump, pad change) is EXPECTED to fail this gate — re-capture then.

#include <phyriad/schema/SchemaHash.hpp>
#include <phyriad/schema/xxh3_inline.hpp>
#include <phyriad/hal/Cacheline.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>

namespace {

struct ConfTagA {};
struct ConfTagB {};

int g_fail = 0;
#define EXPECT(cond)                                                       \
    do {                                                                   \
        if (!(cond)) { std::printf("  FAIL: %s\n", #cond); ++g_fail; }     \
    } while (0)

// Mirror real usage: XXH3State always backs xxh3_128 with a 512-byte buffer
// (kBufCap), filling the first `len` bytes — chunked reads land in the zero pad.
consteval std::array<std::byte, 512> ramp(std::size_t len) {
    std::array<std::byte, 512> a{};
    for (std::size_t i = 0; i < len; ++i)
        a[i] = static_cast<std::byte>((i * 31u + 7u) & 0xFFu);
    return a;
}

// ── Level 1 — raw xxh3_128 goldens, PLATFORM-INDEPENDENT (pure byte hashing,
//    no kDestructivePad/version mixed in). One per length path (≤16 / 17-128 /
//    129-240 / >240). Compile-time: a drift in the hash engine is a BUILD FAILURE
//    on every platform. ────────────────────────────────────────────────────────
constexpr auto kA8   = ramp(8);
constexpr auto kA64  = ramp(64);
constexpr auto kA200 = ramp(200);
constexpr auto kA400 = ramp(400);

static_assert(phyriad::schema::xxh3::xxh3_128(kA8, 8).low64  == 0x46412f58a35d35faull &&
              phyriad::schema::xxh3::xxh3_128(kA8, 8).high64 == 0x8cc97267805472a5ull,
              "xxh3_128 (len<=16 path) drifted — the hash engine changed");
static_assert(phyriad::schema::xxh3::xxh3_128(kA64, 64).low64  == 0x4895504d0434b973ull &&
              phyriad::schema::xxh3::xxh3_128(kA64, 64).high64 == 0xeb15410129689e9cull,
              "xxh3_128 (17-128 path) drifted — the hash engine changed");
static_assert(phyriad::schema::xxh3::xxh3_128(kA200, 200).low64  == 0xef245aca40c4ad36ull &&
              phyriad::schema::xxh3::xxh3_128(kA200, 200).high64 == 0xc3fd354e3edee50cull,
              "xxh3_128 (129-240 path) drifted — the hash engine changed");
static_assert(phyriad::schema::xxh3::xxh3_128(kA400, 400).low64  == 0x5a1518e68a1edb56ull &&
              phyriad::schema::xxh3::xxh3_128(kA400, 400).high64 == 0xcbc57f3298caf70eull,
              "xxh3_128 (long/>240 path) drifted — the hash engine changed");

}  // namespace

int main() {
    using namespace phyriad::schema;

    std::printf("=== schema_hash conformance gate ===\n");
    std::printf("  ABI: kDestructivePad=%zu  kPhyriadHalVersion=%u\n",
                phyriad::hal::kDestructivePad,
                static_cast<unsigned>(kPhyriadHalVersion));
    std::printf("  Level 1 (raw xxh3_128, 4 length paths): pinned at COMPILE time"
                " (static_assert) — reaching here means they held.\n");

    // ── Portable invariants (hold on EVERY ABI) ──────────────────────────────
    EXPECT(!schema_hash<ConfTagA>().is_zero());
    EXPECT(!schema_hash<ConfTagB>().is_zero());
    EXPECT(schema_hash<ConfTagA>() != schema_hash<ConfTagB>());   // distinctness
    EXPECT(schema_hash<ConfTagA>() == schema_hash<ConfTagA>());   // determinism

    // ── Level 2 — schema_hash<T> goldens, PLATFORM-DEPENDENT (mixes
    //    kDestructivePad + kPhyriadHalVersion). Checked only on the ABI the
    //    goldens were captured on; other ABIs skip (Level 1 still pins the engine,
    //    the invariants above still hold). ─────────────────────────────────────
    if (phyriad::hal::kDestructivePad == 128u && kPhyriadHalVersion == 7u) {
        EXPECT((schema_hash<ConfTagA>() == Hash128{0x3b6a3caf0548de81ull, 0xe2e29ded0d64c438ull}));
        EXPECT((schema_hash<ConfTagB>() == Hash128{0x1542b2d613553ab7ull, 0x1b5b6eae401425a1ull}));
        std::printf("  Level 2 (schema_hash<T> goldens): checked for ABI pad=128 ver=7.\n");
    } else {
        std::printf("  Level 2 (schema_hash<T> goldens): SKIPPED — different ABI"
                    " (the Level-1 engine pin + invariants still apply).\n");
    }

    if (g_fail == 0) {
        std::printf("PASS — schema fingerprint stable (no drift).\n");
        return 0;
    }
    std::printf("FAIL — %d conformance check(s) failed. If this is a DELIBERATE ABI\n"
                "change, bump kPhyriadHalVersion and re-capture the goldens; otherwise\n"
                "the hash engine drifted and every persistent artifact is invalidated.\n",
                g_fail);
    return 1;
}
