// framework/ipc/tests/shm_region_test.cpp
// Test suite for phyriad::ipc::ShmRegion<Header, Payload>.
//
// Tests:
//   1. Static layout assertions pass (sizeof, alignof, trivially_copyable).
//   2. create() + open() roundtrip: payload written by producer readable by consumer.
//   3. try_read_consistent() returns correct data after a write.
//   4. magic mismatch in open() → SchemaMismatch error.
//   5. version mismatch in open() → SchemaMismatch error.
//   6. begin_write() RAII guard bumps seq correctly (even→odd→even).
//   7. close() / move: moved-from region is not open.
//
// Note: these tests use an in-process create+open pair (same process, two
// ShmRegion objects mapping the same segment). This exercises all code paths
// including the seqlock without requiring a fork/subprocess.
//

#include <phyriad/ipc/ShmRegion.hpp>
#include <phyriad/schema/Error.hpp>

#include <atomic>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <type_traits>
#include <phyriad/hal/MemoryOrder.hpp>

// ── Micro-test framework ─────────────────────────────────────────────────────
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

// ── Test types ───────────────────────────────────────────────────────────────
// A minimal header satisfying ShmHeaderConcept.
struct alignas(8) TestHeader {
    uint32_t              magic     {0u};
    uint32_t              version   {0u};
    std::atomic<uint32_t> seq       {0u};
    std::atomic<uint32_t> agent_pid {0u};
    uint32_t              pad[2]    {};   // pad to 24 bytes
};
static_assert(sizeof(TestHeader)  == 24u, "TestHeader size");
static_assert(alignof(TestHeader) ==  8u, "TestHeader align");
static_assert(std::is_standard_layout_v<TestHeader>,  "standard_layout");

struct alignas(8) TestPayload {
    uint64_t counter  {0u};
    uint32_t value_a  {0u};
    uint32_t value_b  {0u};
    char     label[16]{};
};
static_assert(sizeof(TestPayload)  == 32u, "TestPayload size");
static_assert(alignof(TestPayload) ==  8u, "TestPayload align");
static_assert(std::is_trivially_copyable_v<TestPayload>, "trivially_copyable");
static_assert(std::is_standard_layout_v<TestPayload>,    "standard_layout");

using Region = phyriad::ipc::ShmRegion<TestHeader, TestPayload>;

// Unique names per test run to avoid collisions with previous crashed runs.
// On POSIX this becomes /TestShmRoundtrip, etc.
static constexpr const char* kNameRoundtrip = "TestShmRoundtrip_gma";
static constexpr const char* kNameMagic     = "TestShmMagic_gma";
static constexpr const char* kNameVersion   = "TestShmVersion_gma";
static constexpr const char* kNameSeqlock   = "TestShmSeqlock_gma";
static constexpr const char* kNameMove      = "TestShmMove_gma";
static constexpr uint32_t    kMagic         = 0x474D4100u; // "GMA\0"
static constexpr uint32_t    kVersion       = 1u;

// ─────────────────────────────────────────────────────────────────────────────
// Test 1 — static layout checks (compile-time; verified by static_assert above)
// ─────────────────────────────────────────────────────────────────────────────
static void test_static_layout() {
    SECTION("Test 1: static layout assertions");
    // All asserts above are compile-time. If we reach here they passed.
    EXPECT(sizeof(TestHeader)  == 24u);
    EXPECT(sizeof(TestPayload) == 32u);
    std::printf("    sizeof(TestHeader)=%zu  sizeof(TestPayload)=%zu\n",
                sizeof(TestHeader), sizeof(TestPayload));
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 2 — create + open roundtrip
// ─────────────────────────────────────────────────────────────────────────────
static void test_roundtrip() {
    SECTION("Test 2: create() + open() roundtrip — payload visible to consumer");

    auto prod_r = Region::create(kNameRoundtrip, kMagic, kVersion);
    EXPECT(prod_r.has_value());
    if (!prod_r.has_value()) return;

    Region& prod = *prod_r;
    EXPECT(prod.is_open());
    EXPECT(prod.is_owner());

    // Write a payload.
    {
        auto g = prod.begin_write();
        prod.payload().counter = 42u;
        prod.payload().value_a = 7u;
        prod.payload().value_b = 13u;
        std::strncpy(prod.payload().label, "hello", 15);
    }

    // Open as consumer (same process, same SHM segment).
    auto cons_r = Region::open(kNameRoundtrip, kMagic, kVersion);
    EXPECT(cons_r.has_value());
    if (!cons_r.has_value()) {
        std::fprintf(stderr, "    open() failed — error code %u\n",
                     static_cast<unsigned>(cons_r.error().code));
        return;
    }

    Region& cons = *cons_r;
    EXPECT(cons.is_open());
    EXPECT(!cons.is_owner());

    // Read consistent snapshot.
    TestPayload snap{};
    const bool ok = cons.try_read_consistent(&snap);
    EXPECT(ok);
    if (ok) {
        EXPECT(snap.counter == 42u);
        EXPECT(snap.value_a == 7u);
        EXPECT(snap.value_b == 13u);
        EXPECT(std::strcmp(snap.label, "hello") == 0);
        std::printf("    snapshot: counter=%llu  a=%u  b=%u  label=\"%s\"\n",
                    static_cast<unsigned long long>(snap.counter),
                    snap.value_a, snap.value_b, snap.label);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 3 — try_read_consistent after multiple writes
// ─────────────────────────────────────────────────────────────────────────────
static void test_seqlock_consistent_read() {
    SECTION("Test 3: try_read_consistent returns latest consistent data");

    auto prod_r = Region::create(kNameSeqlock, kMagic, kVersion);
    EXPECT(prod_r.has_value());
    if (!prod_r.has_value()) return;
    Region& prod = *prod_r;

    // Write 1
    { auto g = prod.begin_write(); prod.payload().counter = 1u; }
    // Write 2
    { auto g = prod.begin_write(); prod.payload().counter = 2u; }
    // Write 3
    { auto g = prod.begin_write(); prod.payload().counter = 3u; }

    auto cons_r = Region::open(kNameSeqlock, kMagic, kVersion);
    EXPECT(cons_r.has_value());
    if (!cons_r.has_value()) return;
    Region& cons = *cons_r;

    TestPayload snap{};
    EXPECT(cons.try_read_consistent(&snap));
    EXPECT(snap.counter == 3u);

    // seq must be even (consistent) after writes.
    const uint32_t seq_val =
        phyriad::hal::seq_load_acquire(cons.header().seq);
    EXPECT((seq_val & 1u) == 0u);
    std::printf("    seq=%u (even=consistent)  counter=%llu\n",
                seq_val,
                static_cast<unsigned long long>(snap.counter));
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 4 — magic mismatch
// ─────────────────────────────────────────────────────────────────────────────
static void test_magic_mismatch() {
    SECTION("Test 4: open() with wrong magic → SchemaMismatch");

    // Create with kMagic, try to open with kMagic+1.
    auto prod_r = Region::create(kNameMagic, kMagic, kVersion);
    EXPECT(prod_r.has_value());
    if (!prod_r.has_value()) return;

    auto bad_r = Region::open(kNameMagic, kMagic + 1u, kVersion);
    EXPECT(!bad_r.has_value());
    if (!bad_r.has_value()) {
        EXPECT(bad_r.error().code == phyriad::ErrorCode::SchemaMismatch);
        std::printf("    got expected SchemaMismatch (code=%u)\n",
                    static_cast<unsigned>(bad_r.error().code));
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 5 — version mismatch
// ─────────────────────────────────────────────────────────────────────────────
static void test_version_mismatch() {
    SECTION("Test 5: open() with wrong version → SchemaMismatch");

    auto prod_r = Region::create(kNameVersion, kMagic, kVersion);
    EXPECT(prod_r.has_value());
    if (!prod_r.has_value()) return;

    auto bad_r = Region::open(kNameVersion, kMagic, kVersion + 1u);
    EXPECT(!bad_r.has_value());
    if (!bad_r.has_value()) {
        EXPECT(bad_r.error().code == phyriad::ErrorCode::SchemaMismatch);
        std::printf("    got expected SchemaMismatch (code=%u)\n",
                    static_cast<unsigned>(bad_r.error().code));
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 6 — begin_write() seq bookkeeping
// ─────────────────────────────────────────────────────────────────────────────
static void test_write_guard_seq() {
    SECTION("Test 6: begin_write() increments seq even→odd→even");

    // Re-use the roundtrip segment if still open; create a fresh one.
    auto prod_r = Region::create("TestShmGuard_gma", kMagic, kVersion);
    EXPECT(prod_r.has_value());
    if (!prod_r.has_value()) return;
    Region& prod = *prod_r;

    const uint32_t seq_before =
        phyriad::hal::seq_load_acquire(prod.header().seq);
    EXPECT((seq_before & 1u) == 0u); // starts even

    {
        auto g = prod.begin_write();
        const uint32_t seq_during =
            phyriad::hal::seq_load_acquire(prod.header().seq);
        EXPECT((seq_during & 1u) == 1u); // odd during write
    }

    const uint32_t seq_after =
        phyriad::hal::seq_load_acquire(prod.header().seq);
    EXPECT((seq_after & 1u) == 0u); // back to even
    EXPECT(seq_after == seq_before + 2u);

    std::printf("    seq: %u → odd → %u\n", seq_before, seq_after);
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 7 — move semantics
// ─────────────────────────────────────────────────────────────────────────────
static void test_move() {
    SECTION("Test 7: move constructor — moved-from region is not open");

    auto r1 = Region::create(kNameMove, kMagic, kVersion);
    EXPECT(r1.has_value());
    if (!r1.has_value()) return;
    EXPECT(r1->is_open());

    Region r2 = std::move(*r1);
    EXPECT(r2.is_open());
    EXPECT(!r1->is_open()); // moved-from must be closed

    std::puts("    move: r1 closed, r2 open — OK");
}

// ─────────────────────────────────────────────────────────────────────────────
// main
// ─────────────────────────────────────────────────────────────────────────────
int main() {
    std::puts("[shm_region_test] phyriad_ipc pillar — Phase 5.A");
    std::puts("----------------------------------------------------------------");
    std::printf("  sizeof(ShmRegion<TestHeader,TestPayload>) = %zu bytes\n",
                sizeof(Region));
    std::puts("----------------------------------------------------------------");

    test_static_layout();
    test_roundtrip();
    test_seqlock_consistent_read();
    test_magic_mismatch();
    test_version_mismatch();
    test_write_guard_seq();
    test_move();

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
