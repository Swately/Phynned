// framework/transport/tests/ring_channel_test.cpp
// RingChannel<T,Cap,MaxReaders> tests — Phase 4 restored-feature verification.
//
// Sections:
//   §1  Default ctor — producer_cursor==-1, live_readers==0
//   §2  subscribe returns valid handle; unsubscribe clears bit
//   §3  subscribe past MaxReaders returns invalid handle
//   §4  publish/consume — message round-trip
//   §5  Subscribers see only messages published after subscribe
//   §6  Multiple readers each see the same message stream
//   §7  Ring full → publish returns false
//   §8  pending() reflects backlog
//   §9  MPMC: concurrent producers + consumer see no message loss
//

#include <phyriad/transport/RingChannel.hpp>
#include <atomic>
#include <cstdio>
#include <cstdint>
#include <thread>
#include <vector>
#include <unordered_set>

using namespace phyriad::transport;

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

// ── §1 Default ctor state ────────────────────────────────────────────────────
static void test_default_state() {
    SECTION("Test 1: default ctor — cursor==-1, no readers");
    RingChannel<int, 8> ch;
    EXPECT(ch.producer_cursor()  == -1);
    EXPECT(ch.live_readers_mask() == 0ULL);
    EXPECT(ch.pending()           == 0);   // (cursor==-1 ⇒ 0)
}

// ── §2 subscribe / unsubscribe ───────────────────────────────────────────────
static void test_subscribe_unsubscribe() {
    SECTION("Test 2: subscribe yields valid handle; unsubscribe clears its bit");
    RingChannel<int, 8> ch;
    auto h = ch.subscribe();
    EXPECT(h.valid());
    EXPECT(ch.live_readers_mask() != 0ULL);

    ch.unsubscribe(h);
    EXPECT(ch.live_readers_mask() == 0ULL);
}

// ── §3 saturation ────────────────────────────────────────────────────────────
static void test_max_readers_saturation() {
    SECTION("Test 3: subscribe past MaxReaders returns invalid handle");
    constexpr std::size_t kMaxR = 4;
    RingChannel<int, 8, kMaxR> ch;

    std::vector<RingChannel<int, 8, kMaxR>::ReaderHandle> handles;
    for (std::size_t i = 0; i < kMaxR; ++i) {
        auto h = ch.subscribe();
        EXPECT(h.valid());
        handles.push_back(h);
    }
    auto extra = ch.subscribe();
    EXPECT(!extra.valid());

    for (auto h : handles) ch.unsubscribe(h);
}

// ── §4 publish + consume round-trip ──────────────────────────────────────────
static void test_publish_consume() {
    SECTION("Test 4: publish then consume yields the same value");
    RingChannel<int, 8> ch;
    auto h = ch.subscribe();
    EXPECT(h.valid());

    EXPECT(ch.publish(42));
    EXPECT(ch.publish(43));

    auto a = ch.consume(h);
    auto b = ch.consume(h);
    auto c = ch.consume(h);

    EXPECT(a.has_value() && *a == 42);
    EXPECT(b.has_value() && *b == 43);
    EXPECT(!c.has_value());   // no more messages
}

// ── §5 late subscriber skips backlog ─────────────────────────────────────────
static void test_late_subscriber_skips_backlog() {
    SECTION("Test 5: a subscriber sees only messages published after subscribe()");
    RingChannel<int, 8> ch;

    // Publish before any subscriber — message is lost / not delivered.
    (void)ch.publish(-1);

    auto h = ch.subscribe();
    EXPECT(h.valid());
    EXPECT(ch.publish(10));
    EXPECT(ch.publish(20));

    auto a = ch.consume(h);
    auto b = ch.consume(h);
    EXPECT(a.has_value() && *a == 10);
    EXPECT(b.has_value() && *b == 20);
}

// ── §6 multi-reader fan-out ──────────────────────────────────────────────────
static void test_multi_reader_fanout() {
    SECTION("Test 6: multiple readers each see the full message stream");
    RingChannel<int, 8> ch;
    auto h1 = ch.subscribe();
    auto h2 = ch.subscribe();
    EXPECT(h1.valid() && h2.valid());
    EXPECT(h1.id != h2.id);

    EXPECT(ch.publish(7));
    EXPECT(ch.publish(8));

    auto a1 = ch.consume(h1);
    auto b1 = ch.consume(h1);
    auto a2 = ch.consume(h2);
    auto b2 = ch.consume(h2);

    EXPECT(a1.has_value() && *a1 == 7);
    EXPECT(b1.has_value() && *b1 == 8);
    EXPECT(a2.has_value() && *a2 == 7);
    EXPECT(b2.has_value() && *b2 == 8);
}

// ── §7 ring full → publish false ─────────────────────────────────────────────
static void test_publish_full() {
    SECTION("Test 7: subscribed-but-not-yet-consumed blocks producer at capacity "
            "(Phase O4.1 fix)");
    RingChannel<int, 4> ch;          // Cap = 4
    auto h = ch.subscribe();
    EXPECT(h.valid());

    // Fix verified: with a live subscriber that hasn't consumed
    // yet, `min_reader_cursor_()` returns -1 (sentinel) instead of INT64_MIN
    // (no readers). The cap-gate now CORRECTLY enforces capacity from the
    // first publish — the legacy bug allowed 5+ publishes (free-fill) here.
    for (int i = 0; i < 4; ++i) EXPECT(ch.publish(i));
    // 5th publish must fail — slot 0 would overwrite the unread message at
    // cursor 0 (sentinel + 1).
    EXPECT(!ch.publish(99));

    // After consuming one message, one more publish fits (slot 0 is now free).
    auto first = ch.consume(h);
    EXPECT(first.has_value() && *first == 0);
    EXPECT(ch.publish(100));
    // Next publish would again overrun.
    EXPECT(!ch.publish(200));
}

// ── §10 broadcaster pattern — no subscribers → producer free-fills ───────────
// The new INT64_MIN sentinel distinguishes "no readers live"
// from "readers live but at sentinel cursor". The former still allows the
// producer to free-fill (broadcaster pattern) — confirm via this test.
static void test_no_readers_free_fill() {
    SECTION("Test 10: no subscribers → publish succeeds past capacity (free-fill broadcaster)");
    RingChannel<int, 4> ch;          // Cap = 4
    // No subscribers — producer can free-fill indefinitely.
    for (int i = 0; i < 16; ++i) EXPECT(ch.publish(i));
    EXPECT(ch.producer_cursor() == 15);
}

// ── §8 pending tracks backlog ────────────────────────────────────────────────
static void test_pending_count() {
    SECTION("Test 8: pending() returns the slow-reader backlog");
    RingChannel<int, 16> ch;
    auto h = ch.subscribe();
    EXPECT(h.valid());

    for (int i = 0; i < 5; ++i) EXPECT(ch.publish(i));
    EXPECT(ch.pending() == 5);

    (void)ch.consume(h);
    (void)ch.consume(h);
    EXPECT(ch.pending() == 3);
}

// ── §9 MPMC concurrent producers + single consumer ───────────────────────────
static void test_mpmc_no_loss() {
    SECTION("Test 9: 4 producers × 250 msgs — consumer receives all 1000 distinct ids");
    RingChannel<uint32_t, 1024> ch;
    auto h = ch.subscribe();
    EXPECT(h.valid());

    constexpr uint32_t kProducers = 4;
    constexpr uint32_t kPer       = 250;

    std::atomic<bool> all_done{false};
    std::unordered_set<uint32_t> seen;
    seen.reserve(kProducers * kPer);

    std::thread consumer([&]() {
        while (seen.size() < kProducers * kPer || !all_done.load()) {
            if (auto v = ch.consume(h)) seen.insert(*v);
            if (all_done.load() && !ch.pending()) break;
        }
        // Drain leftovers.
        while (auto v = ch.consume(h)) seen.insert(*v);
    });

    std::vector<std::thread> producers;
    producers.reserve(kProducers);
    for (uint32_t p = 0; p < kProducers; ++p) {
        producers.emplace_back([&, p]() {
            for (uint32_t i = 0; i < kPer; ++i) {
                const uint32_t id = p * kPer + i;
                ch.publish_block(id);   // blocks if full
            }
        });
    }
    for (auto& t : producers) t.join();
    all_done.store(true);
    consumer.join();

    EXPECT(seen.size() == kProducers * kPer);
}

// ── main ──────────────────────────────────────────────────────────────────────
int main() {
    std::printf("[ring_channel_test] phyriad_transport — Phase 4\n");
    std::printf("----------------------------------------------------------------\n");

    test_default_state();
    test_subscribe_unsubscribe();
    test_max_readers_saturation();
    test_publish_consume();
    test_late_subscriber_skips_backlog();
    test_multi_reader_fanout();
    test_publish_full();
    test_pending_count();
    test_mpmc_no_loss();
    test_no_readers_free_fill();   // Verifies INT64_MIN sentinel

    std::printf("----------------------------------------------------------------\n");
    const int total = g_pass + g_fail;
    if (g_fail == 0)
        std::printf("[OK] %d/%d checks passed\n", g_pass, total);
    else
        std::printf("[FAIL] %d/%d checks FAILED\n", g_fail, total);
    return g_fail ? 1 : 0;
}
// Made with my soul - Swately <3
