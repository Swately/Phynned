// framework/stigmergy/tests/stigmergy_field_test.cpp
// Field<T> unit tests — Phase P-0.5.1 verification.
//
// Sections:
//   §1  default ctor produces default-T on first read
//   §2  publish() then read() round-trip
//   §3  multiple publishes — last-value wins
//   §4  initial-value constructor publishes at construction
//   §5  read() is wait-free in steady state (no writers)
//   §6  concurrent: 1 writer + 1 reader sees monotonic values
//   §7  concurrent: 1 writer + N readers all converge on final value
//
// Stigmergy as first-class pillar.
#include <phyriad/stigmergy/Field.hpp>

#include <atomic>
#include <cstdio>
#include <cstdint>
#include <thread>
#include <vector>
#include <phyriad/hal/MemoryOrder.hpp>

using namespace phyriad::stigmergy;
using phyriad::schema::SampleTick;

// Under sanitizers (TSan/ASan), shrink the §6/§7 concurrent stress loops.
// Their 3-10x instrumentation overhead makes the full-size loops (100k
// publishes × 8 spinning readers) time out, while a few thousand iterations
// are more than enough for TSan to detect any data race. Full size in normal
// builds.
#if defined(__SANITIZE_THREAD__) || defined(__SANITIZE_ADDRESS__)
inline constexpr uint32_t kSanDiv = 50u;
#else
inline constexpr uint32_t kSanDiv = 1u;
#endif

static int g_pass{0};
static int g_fail{0};

#define SECTION(msg) std::printf("  § %s\n", (msg))
#define EXPECT(cond)                                                          \
    do {                                                                      \
        if (cond) { ++g_pass; }                                               \
        else {                                                                \
            ++g_fail;                                                         \
            std::printf("  [FAIL] %s:%d: %s\n", __FILE__, __LINE__, #cond);   \
        }                                                                     \
    } while (false)

// §1 ──────────────────────────────────────────────────────────────────────────
static void test_default_ctor_reads_default_t() {
    SECTION("Test 1: default-constructed Field<T> reads default T");
    Field<SampleTick> field;
    auto v = field.read();
    EXPECT(v.price    == 0u);
    EXPECT(v.volume   == 0u);
    EXPECT(v.side     == 0u);
    EXPECT(v.sequence == 0u);
}

// §2 ──────────────────────────────────────────────────────────────────────────
static void test_publish_read_roundtrip() {
    SECTION("Test 2: publish() then read() round-trip");
    Field<SampleTick> field;
    SampleTick t{};
    t.price    = 12345u;
    t.volume   = 100u;
    t.side     = 1u;
    t.sequence = 42u;
    field.publish(t);

    auto r = field.read();
    EXPECT(r.price    == 12345u);
    EXPECT(r.volume   == 100u);
    EXPECT(r.side     == 1u);
    EXPECT(r.sequence == 42u);
}

// §3 ──────────────────────────────────────────────────────────────────────────
static void test_multiple_publishes_last_wins() {
    SECTION("Test 3: multiple publishes — last value wins");
    Field<SampleTick> field;
    for (uint32_t i = 0; i < 1000u; ++i) {
        SampleTick t{};
        t.sequence = i;
        field.publish(t);
    }
    auto r = field.read();
    EXPECT(r.sequence == 999u);
}

// §4 ──────────────────────────────────────────────────────────────────────────
static void test_initial_value_constructor() {
    SECTION("Test 4: initial-value constructor publishes immediately");
    SampleTick initial{};
    initial.price    = 7u;
    initial.sequence = 13u;

    Field<SampleTick> field{initial};
    auto r = field.read();
    EXPECT(r.price    == 7u);
    EXPECT(r.sequence == 13u);
}

// §5 ──────────────────────────────────────────────────────────────────────────
static void test_wait_free_no_writers() {
    SECTION("Test 5: read() returns without blocking when no writer active");
    Field<SampleTick> field;
    SampleTick t{};
    t.sequence = 7u;
    field.publish(t);

    // Many sequential reads — none should hang. We just check we don't
    // deadlock and the value stays consistent.
    for (int i = 0; i < 10000; ++i) {
        auto r = field.read();
        EXPECT(r.sequence == 7u);
        if (r.sequence != 7u) return; // bail to avoid flooding output
    }
}

// §6 ──────────────────────────────────────────────────────────────────────────
static void test_concurrent_writer_reader_monotonic() {
    SECTION("Test 6: 1 writer + 1 reader — reader sees monotonic sequences");
    Field<SampleTick> field;
    std::atomic<bool> stop{false};
    std::atomic<uint64_t> non_monotonic{0};
    uint64_t last_seq_observed = 0;

    std::thread writer{[&]() noexcept {
        for (uint32_t i = 1; i <= 50000u / kSanDiv && !stop.load(); ++i) {
            SampleTick t{};
            t.sequence = i;
            field.publish(t);
        }
    }};

    // Reader runs concurrently and verifies the sequence number it
    // observes is never lower than what it saw before (Field<T> values
    // are point-in-time snapshots; later reads can only return equal-or-
    // greater sequence numbers from this single-writer scenario).
    for (int i = 0; i < 100000 / static_cast<int>(kSanDiv); ++i) {
        auto r = field.read();
        if (r.sequence < last_seq_observed) {
            phyriad::hal::stat_fetch_add_relaxed(non_monotonic, 1);
        }
        last_seq_observed = r.sequence;
    }
    phyriad::hal::stat_store_relaxed(stop, true);
    writer.join();

    EXPECT(non_monotonic.load() == 0u);
}

// §7 ──────────────────────────────────────────────────────────────────────────
static void test_concurrent_writer_n_readers_converge() {
    SECTION("Test 7: 1 writer + N readers all converge to final value");
    Field<SampleTick> field;
    constexpr int kReaders = 8;
    constexpr uint32_t kFinalSeq = 100000u / kSanDiv;

    std::thread writer{[&]() noexcept {
        for (uint32_t i = 1; i <= kFinalSeq; ++i) {
            SampleTick t{};
            t.sequence = i;
            field.publish(t);
        }
    }};

    std::vector<std::thread> readers;
    std::atomic<int> converged{0};
    for (int r = 0; r < kReaders; ++r) {
        readers.emplace_back([&]() noexcept {
            // Spin until we observe the final value.
            while (true) {
                auto v = field.read();
                if (v.sequence == kFinalSeq) {
                    converged.fetch_add(1);
                    return;
                }
            }
        });
    }

    writer.join();
    for (auto& t : readers) t.join();
    EXPECT(converged.load() == kReaders);
}

int main() {
    std::printf("[stigmergy_field_test] phyriad_stigmergy — Phase P-0.5.1\n");
    std::printf("----------------------------------------------------------------\n");

    test_default_ctor_reads_default_t();
    test_publish_read_roundtrip();
    test_multiple_publishes_last_wins();
    test_initial_value_constructor();
    test_wait_free_no_writers();
    test_concurrent_writer_reader_monotonic();
    test_concurrent_writer_n_readers_converge();

    std::printf("----------------------------------------------------------------\n");
    const int total = g_pass + g_fail;
    if (g_fail == 0)
        std::printf("[OK] %d/%d checks passed\n", g_pass, total);
    else
        std::printf("[FAIL] %d/%d checks FAILED\n", g_fail, total);
    return g_fail ? 1 : 0;
}
// Made with my soul - Swately <3
