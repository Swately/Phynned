// framework/profile/tests/performance_profile_test.cpp
// PerformanceProfile presets + auto-tuning — CANONICAL profile-pillar test.
//
// Tests the profile pillar at its canonical home (phyriad::profile, via
// <phyriad/profile/PerformanceProfile.hpp>), linking ONLY phyriad_profile (which
// brings topology + scheduler transitively). The legacy runtime re-export shim
// (<phyriad/runtime/PerformanceProfile.hpp> → phyriad::runtime) is compile-tested
// by the runtime library's own use of it; this test owns the pillar's coverage.
//   - POWER_SAVE / BALANCED / LATENCY / THROUGHPUT presets
//   - profile_name() round-trip
//   - PerformanceProfile::make(AUTO/CUSTOM) flow
//   - make_auto_profile() sizing (node_profiles / ring_profiles)

#include <phyriad/profile/PerformanceProfile.hpp>
#include <phyriad/scheduler/Placement.hpp>
#include <phyriad/topology/HardwareTopology.hpp>
#include <cstdio>
#include <cstring>
#include <string_view>
#include <vector>

namespace pf  = phyriad::profile;
namespace sch = phyriad::scheduler;

static int g_pass{0};
static int g_fail{0};

#define SECTION(msg) std::printf("  § %s\n", (msg))
#define EXPECT(cond)                                                           \
    do {                                                                       \
        if (cond) { ++g_pass; }                                                \
        else {                                                                 \
            ++g_fail;                                                          \
            std::printf("  [FAIL] %s:%d: %s\n", __FILE__, __LINE__, #cond);   \
        }                                                                      \
    } while (false)

// ── §1 POWER_SAVE preset ─────────────────────────────────────────────────────
static void test_power_save() {
    SECTION("Test 1: PerformanceProfile::make(POWER_SAVE)");
    auto p = pf::PerformanceProfile::make(pf::ProfileKind::POWER_SAVE);
    EXPECT(p.kind == pf::ProfileKind::POWER_SAVE);
    // Power-save deliberately avoids busy-spinning + pinning.
    EXPECT(p.enable_cpu_pinning == false);
    EXPECT(p.ring_consumer_wait != pf::RingWaitMode::BUSY_SPIN);
}

// ── §2 BALANCED preset ───────────────────────────────────────────────────────
static void test_balanced() {
    SECTION("Test 2: PerformanceProfile::make(BALANCED)");
    auto p = pf::PerformanceProfile::make(pf::ProfileKind::BALANCED);
    EXPECT(p.kind == pf::ProfileKind::BALANCED);
    // BALANCED enables affinity pinning but uses a cooperative wait policy.
    EXPECT(p.enable_cpu_pinning == true);
}

// ── §3 LATENCY preset ────────────────────────────────────────────────────────
static void test_latency() {
    SECTION("Test 3: PerformanceProfile::make(LATENCY) — tight tail-latency");
    auto p = pf::PerformanceProfile::make(pf::ProfileKind::LATENCY);
    EXPECT(p.kind == pf::ProfileKind::LATENCY);
    EXPECT(p.enable_cpu_pinning == true);
    // LATENCY enables RT priority elevation for time-critical work.
    EXPECT(p.elevate_rt_priority == true);
}

// ── §4 THROUGHPUT preset ─────────────────────────────────────────────────────
static void test_throughput() {
    SECTION("Test 4: PerformanceProfile::make(THROUGHPUT) — busy-spin consumer");
    auto p = pf::PerformanceProfile::make(pf::ProfileKind::THROUGHPUT);
    EXPECT(p.kind == pf::ProfileKind::THROUGHPUT);
    EXPECT(p.enable_cpu_pinning == true);
    EXPECT(p.ring_consumer_wait == pf::RingWaitMode::BUSY_SPIN);
}

// ── §5 profile_name() round-trip ─────────────────────────────────────────────
static void test_profile_name_roundtrip() {
    SECTION("Test 5: profile_name() returns a canonical string for every kind");
    using pf::ProfileKind;
    EXPECT(std::strcmp(pf::profile_name(ProfileKind::POWER_SAVE), "power_save") == 0);
    EXPECT(std::strcmp(pf::profile_name(ProfileKind::BALANCED),   "balanced")   == 0);
    EXPECT(std::strcmp(pf::profile_name(ProfileKind::LATENCY),    "latency")    == 0);
    EXPECT(std::strcmp(pf::profile_name(ProfileKind::THROUGHPUT), "throughput") == 0);
    EXPECT(std::strcmp(pf::profile_name(ProfileKind::AUTO),       "auto")       == 0);
    EXPECT(std::strcmp(pf::profile_name(ProfileKind::CUSTOM),     "custom")     == 0);
}

// ── §6 make(AUTO) — sets kind=AUTO ───────────────────────────────────────────
static void test_make_auto() {
    SECTION("Test 6: PerformanceProfile::make(AUTO) → kind=AUTO");
    auto p = pf::PerformanceProfile::make(pf::ProfileKind::AUTO);
    EXPECT(p.kind == pf::ProfileKind::AUTO);
}

// ── §7 make_auto_profile sizes node/ring vectors ─────────────────────────────
static void test_make_auto_profile_sizing() {
    SECTION("Test 7: make_auto_profile populates node_profiles + ring_profiles");
    auto topo = phyriad::HardwareTopology::probe().value_or(phyriad::HardwareTopology{});
    std::vector<sch::PlacementHint> hints(4u);

    auto p = pf::make_auto_profile(topo, hints, /*n_nodes=*/4u, /*n_wires=*/6u);
    EXPECT(p.node_profiles.size() == 4u);
    EXPECT(p.ring_profiles.size() == 6u);
    EXPECT(p.kind == pf::ProfileKind::AUTO);

    // Sensible defaults for ring profile.
    for (auto const& rp : p.ring_profiles) {
        EXPECT(rp.capacity >= 16u);
        EXPECT((rp.capacity & (rp.capacity - 1u)) == 0u);  // power of 2
    }
}

// ── §8 manual field edit does NOT auto-flip kind ─────────────────────────────
static void test_manual_edit_keeps_kind() {
    SECTION("Test 8: a direct field edit (no normalize) keeps kind == BALANCED");
    auto p = pf::PerformanceProfile::make(pf::ProfileKind::BALANCED);
    EXPECT(p.kind == pf::ProfileKind::BALANCED);

    // Direct manual edit: kind stays BALANCED (the user wrote the field; only
    // normalize() relabels a non-AUTO profile to CUSTOM — not a bare assignment).
    p.yield_sleep_ns = 1234u;
    EXPECT(p.kind == pf::ProfileKind::BALANCED);
    EXPECT(p.yield_sleep_ns == 1234u);
}

// ── main ──────────────────────────────────────────────────────────────────────
int main() {
    std::printf("[performance_profile_test] phyriad_profile — preset semantics (canonical home)\n");
    std::printf("----------------------------------------------------------------\n");

    test_power_save();
    test_balanced();
    test_latency();
    test_throughput();
    test_profile_name_roundtrip();
    test_make_auto();
    test_make_auto_profile_sizing();
    test_manual_edit_keeps_kind();

    std::printf("----------------------------------------------------------------\n");
    const int total = g_pass + g_fail;
    if (g_fail == 0)
        std::printf("[OK] %d/%d checks passed\n", g_pass, total);
    else
        std::printf("[FAIL] %d/%d checks FAILED\n", g_fail, total);
    return g_fail ? 1 : 0;
}
// Made with my soul - Swately <3
