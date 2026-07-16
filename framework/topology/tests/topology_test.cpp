// framework/topology/tests/topology_test.cpp
// Test suite para phyriad_topology — pillar topology.
//
// Tests:
//   1.  CacheInfo::probe() — valid L1/L2/L3 sizes
//   2.  HardwareTopology::probe() — succeeds, non-empty cores
//   3.  enumerate_cores() — non-empty, logical IDs in order
//   4.  numa_node_count() — at least 1 node
//   5.  numa_cores(0) — at least 1 core in NUMA node 0
//   6.  p_cores() — non-empty on any CPU (all are P-cores on non-hybrid)
//   7.  pin_current_thread — returns true for logical core 0
//   8.  optimal_producer_consumer_pair — returns valid pair
//   9.  detect_system_memory_mb — returns > 0
//   10. CpuFeatures — detect_x86_features() returns a value (no crash)
//   11. VCacheDetector — vcache_logical_ids() returns without crash
//   12. HybridCoreDetector — detect() returns without crash
//   13. PCIeAffinityProbe — probe() returns without crash
//   14. physical_core_count <= logical_core_count
//   15. allocate_cores(1) — returns exactly 1 core
//

#include <phyriad/topology/HardwareTopology.hpp>
#include <phyriad/topology/CacheInfo.hpp>
#include <phyriad/topology/CpuFeatures.hpp>
#include <phyriad/topology/VCacheDetector.hpp>
#include <phyriad/topology/HybridCoreDetector.hpp>
#include <phyriad/topology/PCIeAffinityProbe.hpp>

#include <cassert>
#include <cstdio>
#include <type_traits>
#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#else
#  include <unistd.h>
#endif

namespace topo = phyriad::topology;

// ─────────────────────────────────────────────────────────────────────────────
// Micro-test framework
// ─────────────────────────────────────────────────────────────────────────────
static int g_tests_run    = 0;
static int g_tests_failed = 0;

#define EXPECT(cond)                                                        \
    do {                                                                    \
        ++g_tests_run;                                                      \
        if (!(cond)) {                                                      \
            ++g_tests_failed;                                               \
            std::fprintf(stderr, "  [FAIL] %s:%d: %s\n",                  \
                         __FILE__, __LINE__, #cond);                        \
        }                                                                   \
    } while(0)

#define SECTION(name) std::puts("  § " name)

// ─────────────────────────────────────────────────────────────────────────────
// §1 — CacheInfo::probe()
// ─────────────────────────────────────────────────────────────────────────────
static void test_cache_info() {
    SECTION("Test 1: CacheInfo::probe() — valid cache sizes");

    const auto ci = phyriad::CacheInfo::probe();

    // L1d must be > 0 on any real CPU (absolute minimum 8 KB).
    EXPECT(ci.l1d_bytes > 0u);
    // L2 ≥ L1 on all x86/ARM CPUs since Pentium Pro.
    EXPECT(ci.l2_bytes >= ci.l1d_bytes);
    // L3 is 0 on embedded CPUs without L3; on desktop/server always > 0.
    // We test ≥ 0 (trivially true) and just report — no hard failure for L3==0.
    std::printf("    L1d=%u KB  L2=%u KB  L3=%u KB  line=%u B  destructive=%u B\n",
                ci.l1d_bytes / 1024u,
                ci.l2_bytes  / 1024u,
                ci.l3_bytes  / 1024u,
                ci.line_size,
                ci.destructive_size);
    EXPECT(ci.line_size >= 16u);  // minimum cache line: MIPS32 = 16 B
    EXPECT(ci.line_size <= 256u); // maximum sane cache line
}

// ─────────────────────────────────────────────────────────────────────────────
// §2 — HardwareTopology::probe()
// ─────────────────────────────────────────────────────────────────────────────
static void test_hardware_topology_probe() {
    SECTION("Test 2: HardwareTopology::probe() — succeeds, non-empty cores");

    auto result = phyriad::HardwareTopology::probe();
    EXPECT(result.has_value());
    if (!result.has_value()) {
        std::fprintf(stderr, "    probe() error: %s\n", result.error().c_str());
        return;
    }

    const auto& t = *result;
    EXPECT(!t.cores.empty());
    EXPECT(t.total_ram_bytes > 0u);

    std::printf("    logical_cores=%u  physical_cores=%u  numa_nodes=%zu  gpus=%zu\n",
                t.logical_core_count(),
                t.physical_core_count(),
                t.numa_nodes.size(),
                t.gpus.size());
    std::printf("    total_RAM=%llu MB  free_RAM=%llu MB\n",
                (unsigned long long)(t.total_ram_bytes / (1024ULL * 1024ULL)),
                (unsigned long long)(t.free_ram_bytes  / (1024ULL * 1024ULL)));
}

// ─────────────────────────────────────────────────────────────────────────────
// §3 — enumerate_cores()
// ─────────────────────────────────────────────────────────────────────────────
static void test_enumerate_cores() {
    SECTION("Test 3: enumerate_cores() — non-empty, IDs ordered");

    const auto cores = phyriad::hw::enumerate_cores();
    EXPECT(!cores.empty());

    // Logical IDs must be in ascending order (sorted by probe_cores_platform_).
    for (std::size_t i = 1; i < cores.size(); ++i) {
        EXPECT(cores[i].logical_id > cores[i - 1].logical_id);
    }

    // Every core has a valid logical_id (not UINT32_MAX).
    for (const auto& c : cores) {
        EXPECT(c.logical_id != UINT32_MAX);
    }

    std::printf("    %zu logical cores enumerated\n", cores.size());

    // Singleton: second call returns same size.
    const auto cores2 = phyriad::hw::enumerate_cores();
    EXPECT(cores2.size() == cores.size());
}

// ─────────────────────────────────────────────────────────────────────────────
// §4 — numa_node_count()
// ─────────────────────────────────────────────────────────────────────────────
static void test_numa_node_count() {
    SECTION("Test 4: numa_node_count() — at least 1");

    const uint32_t nc = phyriad::hw::numa_node_count();
    EXPECT(nc >= 1u);
    std::printf("    NUMA nodes: %u\n", nc);
}

// ─────────────────────────────────────────────────────────────────────────────
// §5 — numa_cores(0)
// ─────────────────────────────────────────────────────────────────────────────
static void test_numa_cores() {
    SECTION("Test 5: numa_cores(0) — at least 1 core in node 0");

    const auto nc = phyriad::hw::numa_cores(0u);
    EXPECT(!nc.empty());
    std::printf("    cores in NUMA node 0: %zu\n", nc.size());
}

// ─────────────────────────────────────────────────────────────────────────────
// §6 — p_cores()
// ─────────────────────────────────────────────────────────────────────────────
static void test_p_cores() {
    SECTION("Test 6: p_cores() — non-empty (all CPUs have at least 1 P-core)");

    const auto pc = phyriad::hw::p_cores();
    EXPECT(!pc.empty());

    const auto ec = phyriad::hw::e_cores();
    std::printf("    P-cores: %zu  E-cores: %zu\n", pc.size(), ec.size());

    // If hybrid: p_cores + e_cores ≤ total physical cores (not ht_siblings).
    // On non-hybrid: e_cores is empty.
    if (!ec.empty()) {
        const auto all = phyriad::hw::enumerate_cores();
        uint32_t phys_count = 0;
        for (const auto& c : all)
            if (!c.is_ht_sibling) ++phys_count;
        EXPECT(pc.size() + ec.size() <= phys_count);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// §7 — pin_current_thread
// ─────────────────────────────────────────────────────────────────────────────
static void test_pin_current_thread() {
    SECTION("Test 7: pin_current_thread(0) — returns true");

    // Pin to core 0 — must succeed on any machine with at least 1 core.
    const bool ok = phyriad::hw::pin_current_thread(0u);
    EXPECT(ok);

    // Restore: pin to all cores by re-enumerating and setting a wide mask.
    // (We leave thread pinned to core 0 for the rest of the test — acceptable.)
    std::puts("    pinned to logical core 0 (OK)");
}

// ─────────────────────────────────────────────────────────────────────────────
// §8 — optimal_producer_consumer_pair
// ─────────────────────────────────────────────────────────────────────────────
static void test_optimal_pair() {
    SECTION("Test 8: optimal_producer_consumer_pair — valid IDs");

    const auto [prod, cons] = phyriad::hw::optimal_producer_consumer_pair(false);

    const auto cores = phyriad::hw::enumerate_cores();
    const uint32_t max_id = cores.back().logical_id;

    EXPECT(prod <= max_id);
    EXPECT(cons <= max_id);

    std::printf("    optimal pair: prod=%u  cons=%u\n", prod, cons);

    // Also test with prefer_v_cache_ccd=true (no crash even if no V-Cache).
    const auto [prod2, cons2] = phyriad::hw::optimal_producer_consumer_pair(true);
    EXPECT(prod2 <= max_id);
    EXPECT(cons2 <= max_id);
}

// ─────────────────────────────────────────────────────────────────────────────
// §9 — detect_system_memory_mb
// ─────────────────────────────────────────────────────────────────────────────
static void test_detect_system_memory() {
    SECTION("Test 9: detect_system_memory_mb() — > 0");

    const uint32_t mb = phyriad::hw::detect_system_memory_mb();
    EXPECT(mb > 0u);
    std::printf("    system memory: %u MiB\n", mb);
}

// ─────────────────────────────────────────────────────────────────────────────
// §10 — CpuFeatures
// ─────────────────────────────────────────────────────────────────────────────
static void test_cpu_features() {
    SECTION("Test 10: detect_x86_features() — no crash, prints ISA flags");

    const auto f = topo::detect_x86_features();

    std::printf("    SSE4.1=%d  SSE4.2=%d  AVX=%d  AVX2=%d  AVX512F=%d\n",
                (int)f.sse4_1, (int)f.sse4_2, (int)f.avx, (int)f.avx2, (int)f.avx512f);
    std::printf("    POPCNT=%d  BMI1=%d  BMI2=%d  F16C=%d  FMA=%d\n",
                (int)f.popcnt, (int)f.bmi1, (int)f.bmi2, (int)f.f16c, (int)f.fma);

    // At minimum: on x86-64 SSE2 is baseline (always true since AMD64).
    // We only assert "no crash" — ISA varies by machine.
    EXPECT(true); // smoke test: if we got here, it worked
}

// ─────────────────────────────────────────────────────────────────────────────
// §11 — VCacheDetector
// ─────────────────────────────────────────────────────────────────────────────
static void test_vcache_detector() {
    SECTION("Test 11: VCacheDetector::vcache_logical_ids() — no crash");

    const auto ids = topo::VCacheDetector::vcache_logical_ids();
    std::printf("    V-Cache cores: %zu (0 on non-X3D)\n", ids.size());

    // Cross-check: v_cache_cores() hw:: wrapper matches detector.
    const auto hw_ids = phyriad::hw::v_cache_cores();
    EXPECT(hw_ids.size() == ids.size());

    EXPECT(true); // smoke test
}

// ─────────────────────────────────────────────────────────────────────────────
// §12 — HybridCoreDetector
// ─────────────────────────────────────────────────────────────────────────────
static void test_hybrid_core_detector() {
    SECTION("Test 12: HybridCoreDetector::detect() — no crash");

    const auto result = topo::HybridCoreDetector::detect();
    std::printf("    core_types entries: %zu\n", result.core_types.size());

    // Each entry must have a valid logical_id.
    for (const auto& ct : result.core_types) {
        EXPECT(ct.logical_id != UINT32_MAX);
    }

    EXPECT(true); // smoke test
}

// ─────────────────────────────────────────────────────────────────────────────
// §13 — PCIeAffinityProbe
// ─────────────────────────────────────────────────────────────────────────────
static void test_pcie_affinity() {
    SECTION("Test 13: PCIeAffinityProbe::probe() — no crash");

    const auto devices = topo::PCIeAffinityProbe::probe();
    std::printf("    PCIe devices with NUMA affinity: %zu\n", devices.size());

    EXPECT(true); // smoke test: no crash
}

// ─────────────────────────────────────────────────────────────────────────────
// §14 — physical_core_count <= logical_core_count
// ─────────────────────────────────────────────────────────────────────────────
static void test_core_counts() {
    SECTION("Test 14: physical_core_count <= logical_core_count");

    auto result = phyriad::HardwareTopology::probe();
    if (!result.has_value()) {
        std::puts("    (skipped: probe() failed)");
        return;
    }

    const auto& t = *result;
    EXPECT(t.physical_core_count() <= t.logical_core_count());
    EXPECT(t.physical_core_count() >= 1u);
    EXPECT(t.logical_core_count()  >= 1u);
}

// ─────────────────────────────────────────────────────────────────────────────
// §15 — allocate_cores(1)
// ─────────────────────────────────────────────────────────────────────────────
static void test_allocate_cores() {
    SECTION("Test 15: allocate_cores(1) — returns exactly 1 core");

    auto result = phyriad::HardwareTopology::probe();
    if (!result.has_value()) {
        std::puts("    (skipped: probe() failed)");
        return;
    }

    const auto& t = *result;
    const auto allocated = t.allocate_cores(1u);
    EXPECT(allocated.size() == 1u);

    // The returned logical ID must be a known core.
    if (!allocated.empty()) {
        const auto all = phyriad::hw::enumerate_cores();
        const uint32_t max_id = all.back().logical_id;
        EXPECT(allocated[0] <= max_id);
    }

    // allocate_cores(0) — must return empty
    const auto zero = t.allocate_cores(0u);
    EXPECT(zero.empty());
}

// ─────────────────────────────────────────────────────────────────────────────
// §16 — FR-2: HardwareTopology::ccd_count()
// ─────────────────────────────────────────────────────────────────────────────
static void test_ccd_count() {
    SECTION("Test 16 (FR-2): ccd_count() — noexcept, consistent with cores");

    // noexcept check (compile-time).
    {
        auto result = phyriad::HardwareTopology::probe();
        if (!result.has_value()) { std::puts("    (skipped: probe failed)"); return; }
        const phyriad::HardwareTopology& t = *result;
        static_assert(noexcept(t.ccd_count()), "ccd_count() must be noexcept");
        EXPECT(t.ccd_count() >= 1u);

        // Manual cross-check: max ccd_id + 1 must equal ccd_count().
        uint32_t max_ccd = 0u;
        for (const auto& c : t.cores)
            if (c.ccd_id > max_ccd) max_ccd = c.ccd_id;
        const uint32_t expected = t.cores.empty() ? 0u : max_ccd + 1u;
        EXPECT(t.ccd_count() == expected);

        std::printf("    ccd_count=%u  (cores=%zu)\n",
                    t.ccd_count(), t.cores.size());
    }

    // Sentinel: default-constructed topology → ccd_count() == 0.
    {
        phyriad::HardwareTopology empty{};
        EXPECT(empty.ccd_count() == 0u);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// §17 — FR-1: hw::topology() singleton accessor
// ─────────────────────────────────────────────────────────────────────────────
static void test_topology_singleton() {
    SECTION("Test 17 (FR-1): hw::topology() — singleton, noexcept, O(1) on re-call");

    const phyriad::HardwareTopology& t1 = phyriad::hw::topology();
    const phyriad::HardwareTopology& t2 = phyriad::hw::topology();

    // Same object (pointer identity).
    EXPECT(&t1 == &t2);

    // If probe succeeded, topology must be non-empty.
    if (phyriad::hw::last_probe_error().empty()) {
        EXPECT(!t1.cores.empty());
        std::printf("    topology().cores=%zu  ccd_count=%u\n",
                    t1.cores.size(), t1.ccd_count());
    } else {
        // Probe failed — cores must be empty (sentinel).
        EXPECT(t1.cores.empty());
        std::printf("    probe failed (expected on some CI environments): %.*s\n",
                    static_cast<int>(phyriad::hw::last_probe_error().size()),
                    phyriad::hw::last_probe_error().data());
    }

    // last_probe_error() is empty == success indicator.
    const bool error_empty = phyriad::hw::last_probe_error().empty();
    std::printf("    last_probe_error empty=%d\n", (int)error_empty);
}

// ─────────────────────────────────────────────────────────────────────────────
// §18 — FR-3: hw::set_process_affinity / get_process_affinity
// ─────────────────────────────────────────────────────────────────────────────
static void test_process_affinity() {
    SECTION("Test 18 (FR-3): get/set_process_affinity — roundtrip on own PID");

#ifdef _WIN32
    const uint32_t own_pid = static_cast<uint32_t>(GetCurrentProcessId());
#else
    const uint32_t own_pid = static_cast<uint32_t>(getpid());
#endif

    // Read current affinity.
    auto get_r = phyriad::hw::get_process_affinity(own_pid);
    EXPECT(get_r.has_value());
    if (!get_r.has_value()) return;

    const uint64_t original_mask = *get_r;
    EXPECT(original_mask != 0ull);
    std::printf("    original affinity mask: 0x%llx\n",
                static_cast<unsigned long long>(original_mask));

    // Build a single-core mask using the first set bit of the original.
    uint64_t single_bit = 0ull;
    for (uint32_t i = 0; i < 64u; ++i) {
        if (original_mask & (1ull << i)) { single_bit = (1ull << i); break; }
    }
    EXPECT(single_bit != 0ull);

    // Apply single-core mask — should succeed.
    auto set_r = phyriad::hw::set_process_affinity(own_pid, single_bit);
    EXPECT(set_r.has_value());
    if (!set_r.has_value()) return;
    const uint64_t returned_prev = *set_r;
    EXPECT(returned_prev == original_mask);  // set() returns prev mask

    // Read back — must match what we set.
    auto readback = phyriad::hw::get_process_affinity(own_pid);
    EXPECT(readback.has_value());
    if (readback.has_value()) EXPECT(*readback == single_bit);

    // Restore original.
    auto restore_r = phyriad::hw::set_process_affinity(own_pid, original_mask);
    EXPECT(restore_r.has_value());

    // mask == 0 must be rejected.
    auto zero_r = phyriad::hw::set_process_affinity(own_pid, 0ull);
    EXPECT(!zero_r.has_value());
    if (!zero_r.has_value()) {
        EXPECT(zero_r.error().code == phyriad::ErrorCode::InvalidArgument);
    }

    std::puts("    roundtrip: set→read→restore OK");
}

// ─────────────────────────────────────────────────────────────────────────────
// main
// ─────────────────────────────────────────────────────────────────────────────
int main() {
    std::puts("[topology_test] phyriad_topology pillar — Phase 1.A");
    std::puts("----------------------------------------------------------------");
    std::printf("  sizeof(CoreInfo)          = %zu bytes\n",
                sizeof(phyriad::CoreInfo));
    std::printf("  sizeof(NumaNode)          = %zu bytes\n",
                sizeof(phyriad::NumaNode));
    std::printf("  sizeof(GpuInfo)           = %zu bytes\n",
                sizeof(phyriad::GpuInfo));
    std::printf("  sizeof(HardwareTopology)  = %zu bytes\n",
                sizeof(phyriad::HardwareTopology));
    std::printf("  sizeof(CacheInfo)         = %zu bytes\n",
                sizeof(phyriad::CacheInfo));
    std::puts("----------------------------------------------------------------");

    test_cache_info();
    test_hardware_topology_probe();
    test_enumerate_cores();
    test_numa_node_count();
    test_numa_cores();
    test_p_cores();
    test_pin_current_thread();
    test_optimal_pair();
    test_detect_system_memory();
    test_cpu_features();
    test_vcache_detector();
    test_hybrid_core_detector();
    test_pcie_affinity();
    test_core_counts();
    test_allocate_cores();
    test_ccd_count();
    test_topology_singleton();
    test_process_affinity();

    std::puts("----------------------------------------------------------------");
    if (g_tests_failed == 0) {
        std::printf("[OK] %d/%d tests passed\n", g_tests_run, g_tests_run);
        return 0;
    } else {
        std::printf("[FAIL] %d/%d tests FAILED\n", g_tests_failed, g_tests_run);
        return 1;
    }
}
// Made with my soul - Swately <3
