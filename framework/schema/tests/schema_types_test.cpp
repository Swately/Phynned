// framework/schema/tests/schema_types_test.cpp
// Test suite para phyriad_schema — todos los bloques del pillar schema.
//
// Tests:
//   1.  Hash128 — trivially copyable, sizeof==16, zero-value default
//   2.  schema_hash<SampleTick>() — non-zero, deterministic (same call == same value)
//   3.  schema_hash distinguishes types — SampleTick != uint64_t
//   4.  PodMessage<SampleTick> — concept satisfied
//   5.  PodMessage<void*> — NOT satisfied (pointer, trivially copyable but fails alignof? no — actually ptr is fine. Use a non-standard-layout type)
//   6.  kMaxPodSize == 4096
//   7.  OpaqueSpan satisfies OpaqueMessage concept
//   8.  GraphSchemaDescriptor — sizeof==64, alignof==64, trivially copyable
//   9.  make_schema_descriptor() — fills magic, hal_version, cache_line_size
//   10. validate_schema_descriptor() — true for valid, false for tampered
//   11. tier_of<SampleTick> == 0
//   12. WireDescriptor — sizeof==48, trivially copyable
//   13. XXH3State empty digest is non-zero (compile-time already, runtime smoke)
//   14. NodeDescriptor — sizeof==64, trivially copyable
//

#include <phyriad/schema/Schema.hpp>
#include <phyriad/hal/Cacheline.hpp>

#include <cassert>
#include <cstdio>
#include <cstring>
#include <type_traits>

namespace schema = phyriad::schema;
namespace hal    = phyriad::hal;

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
// Compile-time static assertions (execute at translation-unit load)
// ─────────────────────────────────────────────────────────────────────────────

// § Hash128 layout
static_assert(sizeof(schema::Hash128) == 16,
    "Hash128 must be 16 bytes");
static_assert(std::is_trivially_copyable_v<schema::Hash128>,
    "Hash128 must be trivially copyable");
static_assert(std::is_standard_layout_v<schema::Hash128>,
    "Hash128 must be standard layout");

// § schema_hash is non-zero for SampleTick (compile-time)
static_assert(schema::schema_hash<schema::SampleTick>() != schema::Hash128{},
    "schema_hash<SampleTick>() must be non-zero");

// § schema_hash distinguishes types (compile-time)
static_assert(schema::schema_hash<schema::SampleTick>() !=
              schema::schema_hash<uint64_t>(),
    "schema_hash must differ between types");

// § PodMessage concept — satisfied
static_assert(schema::PodMessage<schema::SampleTick>,
    "SampleTick must satisfy PodMessage");
static_assert(schema::PodMessage<uint32_t>,
    "uint32_t must satisfy PodMessage");
static_assert(schema::PodMessage<double>,
    "double must satisfy PodMessage");

// § PodMessage concept — NOT satisfied
struct NonPodBase { virtual ~NonPodBase() = default; };  // polymorphic
static_assert(!schema::PodMessage<NonPodBase>,
    "Polymorphic type must NOT satisfy PodMessage");

// § kMaxPodSize
static_assert(schema::kMaxPodSize == 4096u);

// § OpaqueSpan satisfies OpaqueMessage (compile-time)
static_assert(schema::OpaqueMessage<schema::OpaqueSpan>);

// § GraphSchemaDescriptor layout (compile-time)
static_assert(sizeof(schema::GraphSchemaDescriptor) == 64,
    "GraphSchemaDescriptor must be exactly 64 bytes");
static_assert(alignof(schema::GraphSchemaDescriptor) == 64,
    "GraphSchemaDescriptor must be alignas(64)");
static_assert(std::is_trivially_copyable_v<schema::GraphSchemaDescriptor>);

// § WireDescriptor layout (compile-time)
static_assert(sizeof(schema::WireDescriptor) == 48);
static_assert(std::is_trivially_copyable_v<schema::WireDescriptor>);

// § NodeDescriptor layout (compile-time)
static_assert(sizeof(schema::NodeDescriptor) == 64);
static_assert(std::is_trivially_copyable_v<schema::NodeDescriptor>);

// § tier_of (compile-time)
static_assert(schema::tier_of<schema::SampleTick> == 0,
    "SampleTick must be Tier 0");

// ─────────────────────────────────────────────────────────────────────────────
// §1 — Hash128 default value and equality
// ─────────────────────────────────────────────────────────────────────────────
static void test_hash128_basics() {
    SECTION("Test 1: Hash128 basics");

    schema::Hash128 h{};
    EXPECT(h.low  == 0u);
    EXPECT(h.high == 0u);
    EXPECT(h.is_zero());

    schema::Hash128 a{42u, 99u};
    schema::Hash128 b{42u, 99u};
    EXPECT(a == b);
    EXPECT(!(a != b));

    schema::Hash128 c{42u, 100u};
    EXPECT(a != c);
}

// ─────────────────────────────────────────────────────────────────────────────
// §2 — schema_hash<T>() runtime smoke (deterministic)
// ─────────────────────────────────────────────────────────────────────────────
static void test_schema_hash_deterministic() {
    SECTION("Test 2: schema_hash<T>() is deterministic");

    constexpr auto h1 = schema::schema_hash<schema::SampleTick>();
    constexpr auto h2 = schema::schema_hash<schema::SampleTick>();
    EXPECT(h1 == h2);
    EXPECT(!h1.is_zero());

    constexpr auto h_u64 = schema::schema_hash<uint64_t>();
    EXPECT(h_u64 != h1);

    std::printf("    schema_hash<SampleTick>: low=0x%016llx high=0x%016llx\n",
                static_cast<unsigned long long>(h1.low),
                static_cast<unsigned long long>(h1.high));
}

// ─────────────────────────────────────────────────────────────────────────────
// §3 — OpaqueSpan satisfies OpaqueMessage at runtime
// ─────────────────────────────────────────────────────────────────────────────
static void test_opaque_span() {
    SECTION("Test 3: OpaqueSpan satisfies OpaqueMessage");

    const uint8_t buf[8] = {1,2,3,4,5,6,7,8};
    schema::OpaqueSpan span{ buf, sizeof(buf) };

    EXPECT(span.data() == static_cast<const void*>(buf));
    EXPECT(span.size() == 8u);
    EXPECT(schema::OpaqueSpan::type_id == 0xFFFFFFFFu);
}

// ─────────────────────────────────────────────────────────────────────────────
// §4 — GraphSchemaDescriptor: make + validate
// ─────────────────────────────────────────────────────────────────────────────
static void test_schema_descriptor() {
    SECTION("Test 4: make_schema_descriptor() and validate_schema_descriptor()");

    const schema::Hash128 gh{ 0xDEADBEEFCAFEBABEull, 0x0102030405060708ull };
    const auto d = schema::make_schema_descriptor(gh, /*nodes=*/4u, /*wires=*/7u);

    EXPECT(d.graph_hash      == gh);
    EXPECT(d.hal_version     == schema::kPhyriadHalVersion);
    EXPECT(d.cache_line_size == static_cast<uint32_t>(hal::kDestructivePad));
    EXPECT(d.num_nodes       == 4u);
    EXPECT(d.num_wires       == 7u);
    EXPECT(d.phyriad_magic     == schema::kPhyriadMagic);
    EXPECT(d.phyriad_magic     == 0x50485952u);

    EXPECT(schema::validate_schema_descriptor(d));

    // Tamper with magic → should fail
    auto d2 = d;
    d2.phyriad_magic = 0xBAD0F00Du;
    EXPECT(!schema::validate_schema_descriptor(d2));

    // Tamper with hal_version → should fail
    auto d3 = d;
    d3.hal_version = 0u;
    EXPECT(!schema::validate_schema_descriptor(d3));

    // Tamper with cache_line_size → should fail
    auto d4 = d;
    d4.cache_line_size = 0u;
    EXPECT(!schema::validate_schema_descriptor(d4));

    std::printf("    hal::kDestructivePad = %zu, kPhyriadHalVersion = %llu\n",
                hal::kDestructivePad,
                static_cast<unsigned long long>(schema::kPhyriadHalVersion));
}

// Non-POD type for Tier 2 tier_of test:
// has data()/size()/type_id but is NOT trivially copyable (non-trivial destructor).
struct TestOpaqueNonPod {
    const void* ptr{nullptr};
    size_t      len{0};
    static constexpr uint32_t type_id = 42u;
    [[nodiscard]] const void* data() const noexcept { return ptr; }
    [[nodiscard]] size_t      size() const noexcept { return len; }
    // non-trivial destructor → breaks PodMessage, keeps OpaqueMessage
    ~TestOpaqueNonPod() {}
};
static_assert(schema::OpaqueMessage<TestOpaqueNonPod>);
static_assert(!schema::PodMessage<TestOpaqueNonPod>,
    "TestOpaqueNonPod must not satisfy PodMessage (non-trivial dtor)");

// OpaqueSpan is trivially copyable → it satisfies PodMessage first (tier 0).
static_assert(schema::PodMessage<schema::OpaqueSpan>,
    "OpaqueSpan is a trivially copyable POD — satisfies Tier 0");

// ─────────────────────────────────────────────────────────────────────────────
// §5 — tier_of classification
// ─────────────────────────────────────────────────────────────────────────────
static void test_tier_classification() {
    SECTION("Test 5: tier_of<T> classification");

    // Tier 0 — POD
    EXPECT(schema::tier_of<schema::SampleTick>  == 0);
    EXPECT(schema::tier_of<uint32_t>            == 0);
    // OpaqueSpan is trivially copyable → classified as Tier 0 (first match wins)
    EXPECT(schema::tier_of<schema::OpaqueSpan>  == 0);

    // Tier 2 — non-POD type that satisfies OpaqueMessage but not PodMessage
    EXPECT(schema::tier_of<TestOpaqueNonPod>    == 2);
}

// ─────────────────────────────────────────────────────────────────────────────
// §6 — XXH3State runtime smoke (empty → non-zero)
// ─────────────────────────────────────────────────────────────────────────────
static void test_xxh3state_empty_nonzero() {
    SECTION("Test 6: XXH3State{}.digest_128() is non-zero (runtime)");

    // The static_assert above already verified this at compile-time.
    // This is a runtime confirmation that the same consteval path compiles
    // to correct runtime code (identical symbols, different eval context).
    constexpr auto h = schema::XXH3State{}.digest_128();
    EXPECT(!h.is_zero());
    std::printf("    XXH3 empty hash: low=0x%016llx high=0x%016llx\n",
                static_cast<unsigned long long>(h.low),
                static_cast<unsigned long long>(h.high));
}

// ─────────────────────────────────────────────────────────────────────────────
// §7 — WireDescriptor binary layout
// ─────────────────────────────────────────────────────────────────────────────
static void test_wire_descriptor_layout() {
    SECTION("Test 7: WireDescriptor layout");

    EXPECT(sizeof(schema::WireDescriptor) == 48u);
    EXPECT(std::is_trivially_copyable_v<schema::WireDescriptor>);
    EXPECT(std::is_standard_layout_v<schema::WireDescriptor>);
}

// ─────────────────────────────────────────────────────────────────────────────
// §8 — NodeDescriptor binary layout
// ─────────────────────────────────────────────────────────────────────────────
static void test_node_descriptor_layout() {
    SECTION("Test 8: NodeDescriptor layout");

    EXPECT(sizeof(schema::NodeDescriptor) == 64u);
    EXPECT(std::is_trivially_copyable_v<schema::NodeDescriptor>);
}

// ─────────────────────────────────────────────────────────────────────────────
// main
// ─────────────────────────────────────────────────────────────────────────────
int main() {
    std::puts("[schema_types_test] phyriad_schema pillar");
    std::puts("----------------------------------------------------------------");
    std::printf("  sizeof(Hash128)                 = %zu bytes\n",
                sizeof(schema::Hash128));
    std::printf("  sizeof(GraphSchemaDescriptor)   = %zu bytes\n",
                sizeof(schema::GraphSchemaDescriptor));
    std::printf("  sizeof(WireDescriptor)          = %zu bytes\n",
                sizeof(schema::WireDescriptor));
    std::printf("  sizeof(NodeDescriptor)          = %zu bytes\n",
                sizeof(schema::NodeDescriptor));
    std::printf("  hal::kDestructivePad            = %zu bytes\n",
                hal::kDestructivePad);
    std::puts("----------------------------------------------------------------");

    test_hash128_basics();
    test_schema_hash_deterministic();
    test_opaque_span();
    test_schema_descriptor();
    test_tier_classification();
    test_xxh3state_empty_nonzero();
    test_wire_descriptor_layout();
    test_node_descriptor_layout();

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
