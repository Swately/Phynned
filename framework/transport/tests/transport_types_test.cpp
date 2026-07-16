// framework/transport/tests/transport_types_test.cpp
// Test suite for the phyriad_transport pillar.
//
// Tests:
//   1.  Transport concept — Latest<SampleTick> satisfies Transport
//   2.  Latest<T> write/read — single-threaded consistency
//   3.  Latest<T> concurrent — 1 writer, 2 readers (seqlock correctness)
//   4.  Ring<T,Cap> subscribe → send → receive roundtrip
//   5.  Ring<T,Cap> overflow — RingFull when capacity exceeded
//   6.  Ring<T,Cap> multi-reader — 2 readers see all messages
//   7.  RingHandle — invalid handle returns InvalidHandle error
//   8.  SlotCopyFn smoke — pick_slot_copy returns non-null for all modes
//   9.  slot_copy_scalar — bitwise identical copy
//   10. LatencyClass enum values
//

#include <phyriad/transport/TransportAll.hpp>
#include <phyriad/schema/Schema.hpp>

#include <atomic>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <thread>
#include <type_traits>
#include <phyriad/hal/MemoryOrder.hpp>

namespace tr     = phyriad::transport;
namespace schema = phyriad::schema;

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
// Compile-time checks
// ─────────────────────────────────────────────────────────────────────────────

// Latest<T> satisfies Transport
static_assert(tr::Transport<tr::Latest<schema::SampleTick>, schema::SampleTick>,
    "Latest<SampleTick> must satisfy Transport");

// LatencyClass enum values
static_assert(static_cast<uint8_t>(tr::LatencyClass::LocalCache)   == 0);
static_assert(static_cast<uint8_t>(tr::LatencyClass::ProcessLocal) == 1);
static_assert(static_cast<uint8_t>(tr::LatencyClass::CrossProcess) == 2);

// RingHandle default is invalid
static_assert(!tr::RingHandle{}.valid());

// ─────────────────────────────────────────────────────────────────────────────
// §1 — Transport concept: Latest<SampleTick>
// ─────────────────────────────────────────────────────────────────────────────
static void test_transport_concept() {
    SECTION("Test 1: Transport concept — Latest<SampleTick>");

    EXPECT((tr::Transport<tr::Latest<schema::SampleTick>, schema::SampleTick>));
    EXPECT(tr::Latest<schema::SampleTick>::latency_class() == tr::LatencyClass::LocalCache);
    EXPECT(!tr::Latest<schema::SampleTick>::type_hash.is_zero());
}

// ─────────────────────────────────────────────────────────────────────────────
// §2 — Latest<T> write/read single-threaded
// ─────────────────────────────────────────────────────────────────────────────
static void test_latest_single_threaded() {
    SECTION("Test 2: Latest<T> write/read single-threaded");

    tr::Latest<schema::SampleTick> ch;

    schema::SampleTick tick_a{100u, 50u, 1u, 0u};
    ch.write(tick_a);
    const auto r1 = ch.read();
    EXPECT(r1.price    == 100u);
    EXPECT(r1.volume   == 50u);
    EXPECT(r1.side     == 1u);

    schema::SampleTick tick_b{999u, 1u, 0u, 42u};
    ch.write(tick_b);
    const auto r2 = ch.read();
    EXPECT(r2.price    == 999u);
    EXPECT(r2.sequence == 42u);

    // send()/receive() wrappers
    auto s = ch.send(tick_a);
    EXPECT(s.has_value());
    auto rv = ch.receive();
    EXPECT(rv.has_value());
    EXPECT(rv->price == 100u);
}

// ─────────────────────────────────────────────────────────────────────────────
// §3 — Latest<T> concurrent: 1 writer, 2 readers
// ─────────────────────────────────────────────────────────────────────────────
static void test_latest_concurrent() {
    SECTION("Test 3: Latest<T> concurrent seqlock (1 writer, 2 readers)");

    tr::Latest<schema::SampleTick> ch;
    std::atomic<bool> done{false};
    std::atomic<uint32_t> bad_reads{0};

    constexpr uint32_t kWrites = 100000u;

    // Writer: publishes incremental sequence numbers.
    auto writer = [&]() {
        for (uint32_t i = 0; i < kWrites; ++i) {
            ch.write({static_cast<uint64_t>(i), static_cast<uint64_t>(i),
                      i & 1u, i});
        }
        phyriad::hal::seq_store_release(done, true);
    };

    // Reader: verifies consistency — price must equal volume (set by writer).
    auto reader = [&]() {
        uint32_t local_bad = 0u;
        while (!phyriad::hal::seq_load_acquire(done)) {
            const auto v = ch.read();
            if (v.price != v.volume) ++local_bad;  // torn read would violate this
        }
        // Final read after writer is done.
        const auto v = ch.read();
        if (v.price != v.volume) ++local_bad;
        phyriad::hal::stat_fetch_add_relaxed(bad_reads, local_bad);
    };

    std::thread t_writer(writer);
    std::thread t_reader1(reader);
    std::thread t_reader2(reader);

    t_writer.join();
    t_reader1.join();
    t_reader2.join();

    EXPECT(bad_reads.load() == 0u);
    std::printf("    Concurrent: %u writes, %u torn reads (expected 0)\n",
                kWrites, bad_reads.load());
}

// ─────────────────────────────────────────────────────────────────────────────
// §4 — Ring<T,Cap> subscribe → send → receive roundtrip
// ─────────────────────────────────────────────────────────────────────────────
static void test_ring_roundtrip() {
    SECTION("Test 4: Ring<T,8> subscribe → send → receive roundtrip");

    tr::Ring<schema::SampleTick, 8> ring;

    auto h = ring.subscribe();
    EXPECT(h.valid());

    schema::SampleTick msg{42u, 7u, 1u, 99u};
    auto s = ring.send(msg);
    EXPECT(s.has_value());

    auto r = ring.receive(h);
    EXPECT(r.has_value());
    EXPECT(r->price    == 42u);
    EXPECT(r->volume   == 7u);
    EXPECT(r->side     == 1u);
    EXPECT(r->sequence == 99u);

    // No more messages → RingEmpty
    auto r2 = ring.receive(h);
    EXPECT(!r2.has_value());
    EXPECT(r2.error().code == phyriad::ErrorCode::RingEmpty);

    ring.unsubscribe(h);
}

// ─────────────────────────────────────────────────────────────────────────────
// §5 — Ring<T,4> overflow — RingFull when capacity exceeded
// ─────────────────────────────────────────────────────────────────────────────
static void test_ring_full() {
    SECTION("Test 5: Ring<T,4> overflow → RingFull");

    tr::Ring<schema::SampleTick, 4> ring;

    // Subscribe one reader so backpressure kicks in.
    auto h = ring.subscribe();

    schema::SampleTick msg{0u, 0u, 0u, 0u};
    uint32_t sent = 0u;

    // Try to send more than capacity.
    for (int i = 0; i < 8; ++i) {
        auto r = ring.send(msg);
        if (r.has_value()) ++sent;
        else {
            EXPECT(r.error().code == phyriad::ErrorCode::RingFull);
        }
    }

    // Should have sent at most 4 (ring capacity).
    EXPECT(sent <= 4u);
    EXPECT(sent > 0u);

    ring.unsubscribe(h);
    std::printf("    Ring<4>: sent %u/8 before RingFull\n", sent);
}

// ─────────────────────────────────────────────────────────────────────────────
// §6 — Ring<T,16> multi-reader: 2 readers both receive all messages
// ─────────────────────────────────────────────────────────────────────────────
static void test_ring_multi_reader() {
    SECTION("Test 6: Ring<T,16> multi-reader (2 readers see all messages)");

    constexpr uint32_t kN = 16u;
    tr::Ring<schema::SampleTick, kN> ring;

    auto h1 = ring.subscribe();
    auto h2 = ring.subscribe();
    EXPECT(h1.valid());
    EXPECT(h2.valid());
    EXPECT(h1.reader_id != h2.reader_id);

    // Send kN messages.
    for (uint32_t i = 0; i < kN; ++i) {
        auto r = ring.send({static_cast<uint64_t>(i), 0u, 0u, i});
        EXPECT(r.has_value());
    }

    // Both readers drain all messages.
    uint32_t count1 = 0u, count2 = 0u;
    for (uint32_t i = 0; i < kN; ++i) {
        auto r1 = ring.receive(h1);
        auto r2 = ring.receive(h2);
        EXPECT(r1.has_value());
        EXPECT(r2.has_value());
        if (r1) {
            EXPECT(r1->price    == static_cast<uint64_t>(i));
            EXPECT(r1->sequence == i);
            ++count1;
        }
        if (r2) {
            EXPECT(r2->sequence == i);
            ++count2;
        }
    }
    EXPECT(count1 == kN);
    EXPECT(count2 == kN);

    ring.unsubscribe(h1);
    ring.unsubscribe(h2);
}

// ─────────────────────────────────────────────────────────────────────────────
// §7 — RingHandle invalid → InvalidHandle error
// ─────────────────────────────────────────────────────────────────────────────
static void test_ring_invalid_handle() {
    SECTION("Test 7: RingHandle invalid → InvalidHandle error");

    tr::Ring<schema::SampleTick, 8> ring;
    tr::RingHandle bad_handle{};  // reader_id == UINT32_MAX

    EXPECT(!bad_handle.valid());

    auto r = ring.receive(bad_handle);
    EXPECT(!r.has_value());
    EXPECT(r.error().code == phyriad::ErrorCode::InvalidHandle);
}

// ─────────────────────────────────────────────────────────────────────────────
// §8 — pick_slot_copy smoke — non-null for all modes
// ─────────────────────────────────────────────────────────────────────────────
static void test_pick_slot_copy_smoke() {
    SECTION("Test 8: pick_slot_copy returns non-null for all modes");

    constexpr uint32_t kSlotSize = sizeof(schema::SampleTick);

    const auto fn_auto = tr::pick_slot_copy(tr::SlotCopyMode::Auto,        kSlotSize);
    const auto fn_sc   = tr::pick_slot_copy(tr::SlotCopyMode::Scalar,      kSlotSize);
    const auto fn_avx2 = tr::pick_slot_copy(tr::SlotCopyMode::Avx2,        kSlotSize);
    const auto fn_avx5 = tr::pick_slot_copy(tr::SlotCopyMode::Avx512,      kSlotSize);
    const auto fn_nt   = tr::pick_slot_copy(tr::SlotCopyMode::NonTemporal, kSlotSize);

    EXPECT(fn_auto != nullptr);
    EXPECT(fn_sc   != nullptr);
    EXPECT(fn_avx2 != nullptr);
    EXPECT(fn_avx5 != nullptr);
    EXPECT(fn_nt   != nullptr);
}

// ─────────────────────────────────────────────────────────────────────────────
// §9 — slot_copy_scalar correctness
// ─────────────────────────────────────────────────────────────────────────────
static void test_slot_copy_scalar_correctness() {
    SECTION("Test 9: slot_copy_scalar — bitwise identical copy");

    schema::SampleTick src{0xDEADBEEFCAFEBABEull, 0x0102030405060708ull, 0xABCDu, 0x1234u};
    schema::SampleTick dst{};

    tr::slot_copy_scalar(&dst, &src, static_cast<uint32_t>(sizeof(src)));

    EXPECT(std::memcmp(&src, &dst, sizeof(src)) == 0);
    EXPECT(dst.price    == src.price);
    EXPECT(dst.volume   == src.volume);
    EXPECT(dst.side     == src.side);
    EXPECT(dst.sequence == src.sequence);
}

// ─────────────────────────────────────────────────────────────────────────────
// §10 — Ring<T,Cap> producer_cursor() reports correct value
// ─────────────────────────────────────────────────────────────────────────────
static void test_ring_producer_cursor() {
    SECTION("Test 10: Ring producer_cursor() tracking");

    tr::Ring<schema::SampleTick, 8> ring;
    EXPECT(ring.producer_cursor() == -1);  // initial: -1 (nothing published)

    auto h = ring.subscribe();
    schema::SampleTick msg{};
    [[maybe_unused]] auto s1 = ring.send(msg);
    EXPECT(ring.producer_cursor() == 0);  // first message at seq 0

    [[maybe_unused]] auto s2 = ring.send(msg);
    EXPECT(ring.producer_cursor() == 1);  // second message at seq 1

    ring.unsubscribe(h);
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 11: Ring<T,Cap> batched API — send_batch + receive_batch equivalence
// (Phase O1.2)
// ─────────────────────────────────────────────────────────────────────────────
static void test_ring_batched_equivalence() {
    std::puts("Test 11: Ring<T,Cap> batched API — equivalence + capacity behavior");

    using R = tr::Ring<schema::SampleTick, 1024>;
    auto ring  = std::make_unique<R>();
    auto h     = ring->subscribe();

    // Build a deterministic batch.
    constexpr std::size_t kBatch = 100;
    std::array<schema::SampleTick, kBatch> sent{};
    for (std::size_t i = 0; i < kBatch; ++i) {
        sent[i].price    = static_cast<uint32_t>(i * 7u);
        sent[i].volume   = static_cast<uint32_t>(i);
        sent[i].sequence = static_cast<uint32_t>(i + 1u);
    }

    // §11.a — send_batch publishes the entire batch atomically.
    const uint32_t pushed = ring->send_batch(std::span<schema::SampleTick const>{sent});
    EXPECT(pushed == kBatch);
    EXPECT(ring->producer_cursor() == static_cast<int64_t>(kBatch) - 1);

    // §11.b — receive_batch drains the batch in order with one acquire.
    std::array<schema::SampleTick, kBatch> got{};
    const uint32_t drained = ring->receive_batch(
        h, std::span<schema::SampleTick>{got});
    EXPECT(drained == kBatch);
    for (std::size_t i = 0; i < kBatch; ++i) {
        EXPECT(got[i].sequence == sent[i].sequence);
        EXPECT(got[i].price    == sent[i].price);
    }

    // §11.c — receive_batch on empty ring returns 0.
    const uint32_t empty = ring->receive_batch(
        h, std::span<schema::SampleTick>{got});
    EXPECT(empty == 0u);

    // §11.d — send_batch partial when ring near full.
    {
        auto small = std::make_unique<tr::Ring<schema::SampleTick, 64>>();
        auto sh = small->subscribe();
        std::array<schema::SampleTick, 100> overflow{};
        const uint32_t got2 = small->send_batch(
            std::span<schema::SampleTick const>{overflow});
        // Should publish only up to Cap (64), not all 100.
        EXPECT(got2 <= 64u);
        EXPECT(got2 > 0u);
        small->unsubscribe(sh);
    }

    // §11.e — send_batch + interleaved single send keeps sequence numbering
    // consistent (a single send is just send_batch of size 1).
    {
        auto r2 = std::make_unique<R>();
        auto h2 = r2->subscribe();
        schema::SampleTick a{}; a.sequence = 101u;
        [[maybe_unused]] auto sa = r2->send(a);

        std::array<schema::SampleTick, 3> b{};
        b[0].sequence = 102u; b[1].sequence = 103u; b[2].sequence = 104u;
        [[maybe_unused]] uint32_t pb = r2->send_batch(
            std::span<schema::SampleTick const>{b});
        EXPECT(pb == 3u);

        std::array<schema::SampleTick, 8> drained_msgs{};
        const uint32_t d = r2->receive_batch(
            h2, std::span<schema::SampleTick>{drained_msgs});
        EXPECT(d == 4u);
        EXPECT(drained_msgs[0].sequence == 101u);
        EXPECT(drained_msgs[1].sequence == 102u);
        EXPECT(drained_msgs[2].sequence == 103u);
        EXPECT(drained_msgs[3].sequence == 104u);
        r2->unsubscribe(h2);
    }

    ring->unsubscribe(h);
}

// ─────────────────────────────────────────────────────────────────────────────
// main
// ─────────────────────────────────────────────────────────────────────────────
int main() {
    std::puts("[transport_types_test] phyriad_transport pillar");
    std::puts("----------------------------------------------------------------");
    std::printf("  sizeof(Ring<SampleTick,8>)   = %zu bytes\n",
                sizeof(tr::Ring<schema::SampleTick, 8>));
    std::printf("  sizeof(Latest<SampleTick>)   = %zu bytes\n",
                sizeof(tr::Latest<schema::SampleTick>));
    std::printf("  sizeof(RingHandle)           = %zu bytes\n",
                sizeof(tr::RingHandle));
    std::printf("  sizeof(SampleTick)           = %zu bytes\n",
                sizeof(schema::SampleTick));
    std::puts("----------------------------------------------------------------");

    test_transport_concept();
    test_latest_single_threaded();
    test_latest_concurrent();
    test_ring_roundtrip();
    test_ring_full();
    test_ring_multi_reader();
    test_ring_invalid_handle();
    test_pick_slot_copy_smoke();
    test_slot_copy_scalar_correctness();
    test_ring_producer_cursor();
    test_ring_batched_equivalence();

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
