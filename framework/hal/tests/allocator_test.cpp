// framework/hal/tests/allocator_test.cpp
// Property tests for phyriad::hal::aligned_alloc_hint / aligned_free_hint.
//
// Sections:
//   §1  Default path: aligned + writable + correct free
//   §2  Large path: aligned + page-sized + writable
//   §3  Huge path: tolerates lack of privilege (graceful fallback)
//   §4  Various alignments (16/64/128/4096)
//   §5  Zero-size / zero-align return nullptr
//   §6  Caller-provided NUMA node (soft-fails on single-NUMA systems)
//

#include <phyriad/hal/Allocator.hpp>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>

namespace hal = phyriad::hal;

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

static void test_default_path() {
    SECTION("Test 1: Default path — aligned + writable + free");
    for (std::size_t size : {64u, 256u, 4096u, 32u * 1024u}) {
        for (std::size_t align : {16u, 64u, 128u}) {
            void* p = hal::aligned_alloc_hint(size, align, hal::AllocHint::Default);
            EXPECT(p != nullptr);
            if (!p) continue;
            EXPECT((reinterpret_cast<uintptr_t>(p) & (align - 1)) == 0u);
            std::memset(p, 0xAB, size);
            EXPECT(static_cast<unsigned char*>(p)[0] == 0xAB);
            EXPECT(static_cast<unsigned char*>(p)[size - 1] == 0xAB);
            hal::aligned_free_hint(p, size, hal::AllocHint::Default);
        }
    }
}

static void test_large_path() {
    SECTION("Test 2: Large path — OS page allocator");
    constexpr std::size_t kSize = 256u * 1024u;
    void* p = hal::aligned_alloc_hint(kSize, 64u, hal::AllocHint::Large);
    EXPECT(p != nullptr);
    if (p) {
        EXPECT((reinterpret_cast<uintptr_t>(p) & (hal::k4KiB - 1)) == 0u);
        std::memset(p, 0xCD, kSize);
        EXPECT(static_cast<unsigned char*>(p)[0] == 0xCD);
        EXPECT(static_cast<unsigned char*>(p)[kSize - 1] == 0xCD);
        hal::aligned_free_hint(p, kSize, hal::AllocHint::Large);
    }
}

static void test_huge_path_graceful() {
    SECTION("Test 3: Huge path — succeeds OR falls back gracefully");
    constexpr std::size_t kSize = 4u * 1024u * 1024u;   // 4 MiB
    // Even without privilege, the function must return non-null (falls back to
    // ordinary 4 KiB pages or normal allocator).
    void* p = hal::aligned_alloc_hint(
        kSize, 64u, hal::AllocHint::Huge | hal::AllocHint::Large);
    EXPECT(p != nullptr);
    if (p) {
        std::memset(p, 0xEF, kSize);
        EXPECT(static_cast<unsigned char*>(p)[0] == 0xEF);
        EXPECT(static_cast<unsigned char*>(p)[kSize - 1] == 0xEF);
        hal::aligned_free_hint(p, kSize, hal::AllocHint::Huge | hal::AllocHint::Large);
    }
    std::printf("        hugepages_available() = %s\n",
                hal::hugepages_available() ? "yes" : "no (graceful fallback expected)");
}

static void test_various_alignments() {
    SECTION("Test 4: alignments 16/64/128/256/4096 — all aligned correctly");
    for (std::size_t align : {16u, 64u, 128u, 256u, 4096u}) {
        void* p = hal::aligned_alloc_hint(8192u, align, hal::AllocHint::Default);
        EXPECT(p != nullptr);
        if (p) {
            EXPECT((reinterpret_cast<uintptr_t>(p) & (align - 1)) == 0u);
            hal::aligned_free_hint(p, 8192u, hal::AllocHint::Default);
        }
    }
}

static void test_invalid_args() {
    SECTION("Test 5: zero-size / zero-align return nullptr");
    EXPECT(hal::aligned_alloc_hint(0u, 16u, hal::AllocHint::Default) == nullptr);
    EXPECT(hal::aligned_alloc_hint(64u, 0u, hal::AllocHint::Default) == nullptr);
    // aligned_free_hint on nullptr is a no-op (must not crash).
    hal::aligned_free_hint(nullptr, 0u, hal::AllocHint::Default);
    EXPECT(true);
}

static void test_numa_local() {
    SECTION("Test 6: NumaLocal — accepts any single-NUMA system");
    constexpr std::size_t kSize = 64u * 1024u;
    void* p = hal::aligned_alloc_hint(
        kSize, 64u, hal::AllocHint::Large | hal::AllocHint::NumaLocal);
    EXPECT(p != nullptr);
    if (p) {
        std::memset(p, 0x42, kSize);
        EXPECT(static_cast<unsigned char*>(p)[kSize / 2] == 0x42);
        hal::aligned_free_hint(p, kSize,
            hal::AllocHint::Large | hal::AllocHint::NumaLocal);
    }
}

int main() {
    std::printf("[allocator_test] phyriad_hal — Phase O2.1\n");
    std::printf("----------------------------------------------------------------\n");

    test_default_path();
    test_large_path();
    test_huge_path_graceful();
    test_various_alignments();
    test_invalid_args();
    test_numa_local();

    std::printf("----------------------------------------------------------------\n");
    const int total = g_pass + g_fail;
    if (g_fail == 0)
        std::printf("[OK] %d/%d checks passed\n", g_pass, total);
    else
        std::printf("[FAIL] %d/%d checks FAILED\n", g_fail, total);
    return g_fail ? 1 : 0;
}
// Made with my soul - Swately <3
