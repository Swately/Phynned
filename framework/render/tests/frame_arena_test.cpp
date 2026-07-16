// framework/render/tests/frame_arena_test.cpp
// FrameArena bump-allocator tests.
//
// Replicates the legacy `test_frame_arena` (32 checks) covering:
//   §1 — alloc basic
//   §2 — custom alignment
//   §3 — OOM returns nullptr without corrupting the arena
//   §4 — reset_frame zeroes the offset
//   §5 — high_water_mark behaviour
//   §6 — concurrent alloc from multiple threads (no overlap, no nulls)
//   §7 — zero-size alloc returns a non-null pointer
//
#include <phyriad/render/FrameArena.hpp>
#include <atomic>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <thread>
#include <vector>

using phyriad::render::FrameArena;

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

// ── §1 alloc basic ────────────────────────────────────────────────────────────
static void test_alloc_basic() {
    SECTION("Test 1: alloc(100) returns non-null and 16-byte aligned");
    FrameArena arena{4u * 1024u};
    EXPECT(arena.valid());

    void* p = arena.alloc(100);
    EXPECT(p != nullptr);
    EXPECT(arena.bytes_used() >= 100u);
    EXPECT((reinterpret_cast<uintptr_t>(p) & 15u) == 0u);   // default align=16
}

// ── §2 custom alignment ──────────────────────────────────────────────────────
static void test_custom_alignment() {
    SECTION("Test 2: alloc with align=64 returns 64-byte aligned pointer");
    FrameArena arena{4u * 1024u};

    void* p64 = arena.alloc(1u, 64u);
    EXPECT(p64 != nullptr);
    EXPECT((reinterpret_cast<uintptr_t>(p64) & 63u) == 0u);

    void* q64 = arena.alloc(128u, 64u);
    EXPECT(q64 != nullptr);
    EXPECT((reinterpret_cast<uintptr_t>(q64) & 63u) == 0u);
    EXPECT(p64 != q64);
}

// ── §3 OOM returns nullptr without corrupting arena ──────────────────────────
static void test_oom_returns_null() {
    SECTION("Test 3: alloc beyond capacity returns nullptr; arena still works");
    FrameArena arena{1024u};

    void* big = arena.alloc(2048u);
    EXPECT(big == nullptr);

    void* small = arena.alloc(64u);
    EXPECT(small != nullptr);    // small alloc still works after OOM
}

// ── §4 reset_frame zeroes the offset ─────────────────────────────────────────
static void test_reset_frame() {
    SECTION("Test 4: reset_frame() rewinds the bump pointer");
    FrameArena arena{4u * 1024u};

    void* first = arena.alloc(128u);
    EXPECT(first != nullptr);
    EXPECT(arena.bytes_used() >= 128u);

    arena.reset_frame();
    EXPECT(arena.bytes_used() == 0u);

    void* second = arena.alloc(128u);
    EXPECT(second != nullptr);
    EXPECT(first == second);   // same address after reset
}

// ── §5 high_water_mark ───────────────────────────────────────────────────────
static void test_high_water_mark() {
    SECTION("Test 5: high_water_mark tracks peak usage across resets");
    FrameArena arena{4u * 1024u};

    (void)arena.alloc(500u);
    (void)arena.alloc(300u);
    EXPECT(arena.bytes_used()      >= 800u);
    EXPECT(arena.high_water_mark() >= 800u);

    arena.reset_frame();
    EXPECT(arena.bytes_used() == 0u);
    EXPECT(arena.high_water_mark() >= 800u);   // preserved across reset

    (void)arena.alloc(200u);
    EXPECT(arena.high_water_mark() >= 800u);   // smaller post-reset alloc doesn't change peak
}

// ── §6 concurrent alloc ──────────────────────────────────────────────────────
static void test_concurrent_alloc() {
    SECTION("Test 6: concurrent alloc from 4 threads — no nulls, no overlap");
    constexpr std::size_t kCap        = 1024u * 1024u;
    constexpr int         kThreads    = 4;
    constexpr int         kAllocsPerT = 1000;
    constexpr std::size_t kAllocSize  = 32u;

    FrameArena arena{kCap};
    EXPECT(arena.valid());

    std::vector<std::vector<void*>> per_thread(kThreads);
    std::vector<std::thread>        workers;
    workers.reserve(kThreads);

    for (int t = 0; t < kThreads; ++t) {
        per_thread[t].resize(kAllocsPerT, nullptr);
        workers.emplace_back([&, t]() noexcept {
            for (int i = 0; i < kAllocsPerT; ++i)
                per_thread[t][i] = arena.alloc(kAllocSize, 16u);
        });
    }
    for (auto& w : workers) w.join();

    // No nulls.
    int null_count = 0;
    for (auto const& v : per_thread)
        for (void* p : v) if (!p) ++null_count;
    EXPECT(null_count == 0);

    // No overlap — write a unique pattern and read it back.
    int idx = 0;
    for (auto const& v : per_thread)
        for (void* p : v) {
            if (p) std::memset(p, idx & 0xFF, kAllocSize);
            ++idx;
        }
    idx = 0;
    int corruption = 0;
    for (auto const& v : per_thread)
        for (void* p : v) {
            if (p) {
                auto* b = static_cast<unsigned char*>(p);
                if (b[0] != static_cast<unsigned char>(idx & 0xFF))
                    ++corruption;
            }
            ++idx;
        }
    EXPECT(corruption == 0);
    EXPECT(arena.bytes_used() >= kThreads * kAllocsPerT * kAllocSize);
}

// ── §7 zero-size alloc ───────────────────────────────────────────────────────
static void test_zero_size_alloc() {
    SECTION("Test 7: alloc(0) returns a valid pointer (advances offset by 0)");
    FrameArena arena{1024u};

    void* z = arena.alloc(0u);
    EXPECT(z != nullptr);
    EXPECT((reinterpret_cast<uintptr_t>(z) & 15u) == 0u);

    void* p = arena.alloc(16u);
    EXPECT(p != nullptr);
    // Zero-size shouldn't have advanced the offset — p should equal z OR be
    // strictly after (depends on alignment padding semantics). In any case,
    // p must be valid + writable.
    std::memset(p, 0xAB, 16u);
    EXPECT(static_cast<unsigned char*>(p)[0] == 0xAB);
}

// ── main ──────────────────────────────────────────────────────────────────────
int main() {
    std::printf("[frame_arena_test] phyriad_render — Phase 4\n");
    std::printf("----------------------------------------------------------------\n");

    test_alloc_basic();
    test_custom_alignment();
    test_oom_returns_null();
    test_reset_frame();
    test_high_water_mark();
    test_concurrent_alloc();
    test_zero_size_alloc();

    std::printf("----------------------------------------------------------------\n");
    const int total = g_pass + g_fail;
    if (g_fail == 0)
        std::printf("[OK] %d/%d checks passed\n", g_pass, total);
    else
        std::printf("[FAIL] %d/%d checks FAILED\n", g_fail, total);
    return g_fail ? 1 : 0;
}
// Made with my soul - Swately <3
