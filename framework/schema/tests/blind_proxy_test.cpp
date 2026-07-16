// framework/schema/tests/blind_proxy_test.cpp
// Blind-proxy schema test — arc 70.
//
// Validates:
//   1. CapabilityRequest — POD asserts pass; size == 192; field layout smoke
//   2. PassthroughResponse — POD asserts pass; size == 64; field layout smoke
//   3. CapabilityBit enum values are stable (regression guard — do not reorder)
//   4. PassthroughError enum values are stable
//   5. Trivial construction smoke (zero-init, field write/read round-trip)
//
// See: framework/schema/include/phyriad/schema/CapabilityRequest.hpp
//      framework/schema/include/phyriad/schema/PassthroughResponse.hpp
//      IM_REAL.md §6.3

#include <phyriad/schema/CapabilityRequest.hpp>
#include <phyriad/schema/PassthroughResponse.hpp>

#include <cassert>
#include <cstdio>
#include <cstring>
#include <type_traits>

namespace schema = phyriad::schema;

// ─────────────────────────────────────────────────────────────────────────────
// Compile-time assertions (execute at translation-unit load)
// ─────────────────────────────────────────────────────────────────────────────

// § CapabilityRequest layout
static_assert(sizeof(schema::CapabilityRequest) == 192,
    "CapabilityRequest must be exactly 192 bytes");
static_assert(std::is_trivially_copyable_v<schema::CapabilityRequest>,
    "CapabilityRequest must be trivially copyable");
static_assert(std::is_standard_layout_v<schema::CapabilityRequest>,
    "CapabilityRequest must be standard layout");
static_assert(schema::PodMessage<schema::CapabilityRequest>,
    "CapabilityRequest must satisfy PodMessage concept");

// § PassthroughResponse layout
static_assert(sizeof(schema::PassthroughResponse) == 64,
    "PassthroughResponse must be exactly 64 bytes");
static_assert(std::is_trivially_copyable_v<schema::PassthroughResponse>,
    "PassthroughResponse must be trivially copyable");
static_assert(std::is_standard_layout_v<schema::PassthroughResponse>,
    "PassthroughResponse must be standard layout");
static_assert(schema::PodMessage<schema::PassthroughResponse>,
    "PassthroughResponse must satisfy PodMessage concept");

// § CapabilityBit enum values — regression guard (do NOT reorder)
static_assert(static_cast<std::uint32_t>(schema::CapabilityBit::FileRead)      == (1u << 0));
static_assert(static_cast<std::uint32_t>(schema::CapabilityBit::FileWrite)     == (1u << 1));
static_assert(static_cast<std::uint32_t>(schema::CapabilityBit::NetFetch)      == (1u << 2));
static_assert(static_cast<std::uint32_t>(schema::CapabilityBit::SqlQuery)      == (1u << 3));
static_assert(static_cast<std::uint32_t>(schema::CapabilityBit::ProcessSpawn)  == (1u << 4));
static_assert(static_cast<std::uint32_t>(schema::CapabilityBit::OperatorInput) == (1u << 5));

// § PassthroughError enum values — regression guard
static_assert(static_cast<std::uint16_t>(schema::PassthroughError::OK)                == 0);
static_assert(static_cast<std::uint16_t>(schema::PassthroughError::NotFound)          == 1);
static_assert(static_cast<std::uint16_t>(schema::PassthroughError::AccessDenied)      == 2);
static_assert(static_cast<std::uint16_t>(schema::PassthroughError::Timeout)           == 3);
static_assert(static_cast<std::uint16_t>(schema::PassthroughError::InvalidCapability) == 4);
static_assert(static_cast<std::uint16_t>(schema::PassthroughError::PayloadTooLarge)   == 5);
static_assert(static_cast<std::uint16_t>(schema::PassthroughError::InferenceDetected) == 6);

// ─────────────────────────────────────────────────────────────────────────────
// Micro-test framework (consistent with schema_types_test.cpp)
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

#define SECTION(name) std::puts("  " name)

// ─────────────────────────────────────────────────────────────────────────────
// §1 — CapabilityRequest: zero-init + field write/read round-trip
// ─────────────────────────────────────────────────────────────────────────────
static void test_capability_request_smoke() {
    SECTION("Test 1: CapabilityRequest construction + field round-trip");

    schema::CapabilityRequest req{};
    EXPECT(req.request_id          == 0u);
    EXPECT(req.requestor_holon_id  == 0u);
    EXPECT(req.capability_bit      == 0u);
    EXPECT(req.timestamp_ns        == 0u);
    EXPECT(req.timeout_ms          == 0u);
    EXPECT(req._pad                == 0u);

    // Write fields
    req.request_id         = 0xDEADBEEF12345678ull;
    req.requestor_holon_id = 42u;
    req.capability_bit     = static_cast<std::uint32_t>(schema::CapabilityBit::NetFetch);
    req.timestamp_ns       = 1'000'000'000ull;
    req.timeout_ms         = 5000u;

    // Populate payload_descriptor with a fake URL
    const char url[] = "https://example.com/resource";
    std::memcpy(req.payload_descriptor, url, sizeof(url));

    // Read back
    EXPECT(req.request_id         == 0xDEADBEEF12345678ull);
    EXPECT(req.requestor_holon_id == 42u);
    EXPECT(req.capability_bit     ==
           static_cast<std::uint32_t>(schema::CapabilityBit::NetFetch));
    EXPECT(req.timeout_ms         == 5000u);
    EXPECT(std::memcmp(req.payload_descriptor, url, sizeof(url)) == 0);

    // Verify payload_hash_expected is still zeroed (untouched)
    const std::uint8_t zeros[32]{};
    EXPECT(std::memcmp(req.payload_hash_expected, zeros, 32) == 0);

    std::printf("    sizeof(CapabilityRequest) = %zu bytes\n",
                sizeof(schema::CapabilityRequest));
}

// ─────────────────────────────────────────────────────────────────────────────
// §2 — PassthroughResponse: zero-init + field write/read round-trip
// ─────────────────────────────────────────────────────────────────────────────
static void test_passthrough_response_smoke() {
    SECTION("Test 2: PassthroughResponse construction + field round-trip");

    schema::PassthroughResponse resp{};
    EXPECT(resp.request_id         == 0u);
    EXPECT(resp.payload_size_bytes == 0u);
    EXPECT(resp.error_code         == 0u);
    EXPECT(resp._pad               == 0u);
    EXPECT(resp.responder_holon_id == 0u);
    EXPECT(resp.timestamp_ns       == 0u);

    // Write fields
    resp.request_id         = 0xDEADBEEF12345678ull;
    resp.payload_size_bytes = 4096u;
    resp.error_code         = static_cast<std::uint16_t>(schema::PassthroughError::OK);
    resp.responder_holon_id = 99u;
    resp.timestamp_ns       = 2'000'000'000ull;

    // Populate payload_hash with a fake SHA256
    for (int i = 0; i < 32; ++i) {
        resp.payload_hash[i] = static_cast<std::uint8_t>(i);
    }

    // Read back
    EXPECT(resp.request_id         == 0xDEADBEEF12345678ull);
    EXPECT(resp.payload_size_bytes == 4096u);
    EXPECT(resp.error_code         ==
           static_cast<std::uint16_t>(schema::PassthroughError::OK));
    EXPECT(resp.responder_holon_id == 99u);
    EXPECT(resp.payload_hash[0]    == 0u);
    EXPECT(resp.payload_hash[31]   == 31u);

    std::printf("    sizeof(PassthroughResponse) = %zu bytes\n",
                sizeof(schema::PassthroughResponse));
}

// ─────────────────────────────────────────────────────────────────────────────
// §3 — memcpy round-trip (zero-copy transport simulation)
// ─────────────────────────────────────────────────────────────────────────────
static void test_memcpy_roundtrip() {
    SECTION("Test 3: memcpy round-trip (zero-copy transport simulation)");

    schema::CapabilityRequest src{};
    src.request_id     = 0xCAFEBABE00112233ull;
    src.capability_bit = static_cast<std::uint32_t>(schema::CapabilityBit::FileRead);
    src.timeout_ms     = 1000u;

    // Simulate SHM ring-slot copy
    alignas(64) std::uint8_t slot_buf[sizeof(schema::CapabilityRequest)];
    std::memcpy(slot_buf, &src, sizeof(src));

    schema::CapabilityRequest dst{};
    std::memcpy(&dst, slot_buf, sizeof(dst));

    EXPECT(dst.request_id     == 0xCAFEBABE00112233ull);
    EXPECT(dst.capability_bit == static_cast<std::uint32_t>(schema::CapabilityBit::FileRead));
    EXPECT(dst.timeout_ms     == 1000u);

    // Same for PassthroughResponse
    schema::PassthroughResponse sresp{};
    sresp.request_id = 0xCAFEBABE00112233ull;
    sresp.error_code = static_cast<std::uint16_t>(schema::PassthroughError::InferenceDetected);

    alignas(64) std::uint8_t resp_buf[sizeof(schema::PassthroughResponse)];
    std::memcpy(resp_buf, &sresp, sizeof(sresp));

    schema::PassthroughResponse dresp{};
    std::memcpy(&dresp, resp_buf, sizeof(dresp));

    EXPECT(dresp.request_id == 0xCAFEBABE00112233ull);
    EXPECT(dresp.error_code ==
           static_cast<std::uint16_t>(schema::PassthroughError::InferenceDetected));
}

// ─────────────────────────────────────────────────────────────────────────────
// main
// ─────────────────────────────────────────────────────────────────────────────
int main() {
    std::puts("[blind_proxy_test] phyriad_schema blind-proxy PODs (arc 70)");
    std::puts("----------------------------------------------------------------");
    std::printf("  sizeof(CapabilityRequest)    = %zu bytes (limit 256)\n",
                sizeof(schema::CapabilityRequest));
    std::printf("  sizeof(PassthroughResponse)  = %zu bytes (limit 128)\n",
                sizeof(schema::PassthroughResponse));
    std::puts("----------------------------------------------------------------");

    test_capability_request_smoke();
    test_passthrough_response_smoke();
    test_memcpy_roundtrip();

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
