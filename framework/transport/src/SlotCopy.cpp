// framework/transport/src/SlotCopy.cpp
// SIMD slot copy implementations and pick_slot_copy() dispatch.
//
// Compiled once into libphyriad_transport.a.
// CPUID detection is done in pick_slot_copy() via static booleans (one-time cost).
//
// Compiler flags applied per-TU (set in CMakeLists.txt):
//   GCC/Clang: -mavx2 -mavx512f (if supported)
//   MSVC:      /arch:AVX2 /arch:AVX512
//

#include <phyriad/transport/SlotCopy.hpp>
#include <phyriad/hal/Simd.hpp>   // the single SIMD detector (OSXSAVE/XGETBV-gated)

// ── CPUID detection helpers (x86/x64 only) ────────────────────────────────────
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
#  define PHYRIAD_SLOTCOPY_X86 1
#else
#  define PHYRIAD_SLOTCOPY_X86 0
#endif

namespace phyriad::transport {

// ── CPUID feature detection — cached once per process ─────────────────────────

namespace {

// Delegate to the single hal detector — it carries the OSXSAVE/XGETBV OS-enablement
// gate (a VEX-encoded op #UDs without XCR0 YMM state, regardless of the CPUID feature
// bit). Avoids a second, gate-less CPUID detector drifting from hal::SimdCaps. The
// detection itself is scalar (CPUID/XGETBV), so it is safe under this TU's ISA flags.
// (See CPU_SUBSTRATE_PRIOR_ART.md Appendix A.2.)
bool detect_avx2()    noexcept { return phyriad::hal::simd_caps().avx2; }
bool detect_avx512f() noexcept { return phyriad::hal::simd_caps().avx512f; }

// Singletons — evaluated once (static local + atomic from the OS view).
bool has_avx2() noexcept {
    static bool v = detect_avx2();
    return v;
}

bool has_avx512f() noexcept {
    static bool v = detect_avx512f();
    return v;
}

} // anonymous namespace

// ── slot_copy_scalar ──────────────────────────────────────────────────────────
void slot_copy_scalar(void* dst, const void* src, uint32_t n) noexcept {
    std::memcpy(dst, src, n);
}

// ── slot_copy_avx2 ────────────────────────────────────────────────────────────
void slot_copy_avx2(void* dst, const void* src, uint32_t n) noexcept {
#if PHYRIAD_TRANSPORT_HAS_SIMD && (defined(__AVX2__) || defined(_MSC_VER))
    uint8_t*       d = static_cast<uint8_t*>(dst);
    const uint8_t* s = static_cast<const uint8_t*>(src);

    // 4×256-bit unrolled = 128 B/iter.
    while (n >= 128u) {
        __m256i v0 = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(s));
        __m256i v1 = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(s + 32));
        __m256i v2 = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(s + 64));
        __m256i v3 = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(s + 96));
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(d),       v0);
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(d + 32),  v1);
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(d + 64),  v2);
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(d + 96),  v3);
        s += 128u; d += 128u; n -= 128u;
    }
    // Single 256-bit blocks.
    while (n >= 32u) {
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(d),
            _mm256_loadu_si256(reinterpret_cast<const __m256i*>(s)));
        s += 32u; d += 32u; n -= 32u;
    }
    // Remainder < 32 B.
    if (n) std::memcpy(d, s, n);

    // Mandatory vzeroupper — prevents AVX→SSE transition penalty.
    _mm256_zeroupper();
#else
    std::memcpy(dst, src, n);
#endif
}

// ── slot_copy_avx512 ──────────────────────────────────────────────────────────
void slot_copy_avx512(void* dst, const void* src, uint32_t n) noexcept {
#if PHYRIAD_TRANSPORT_HAS_SIMD && defined(__AVX512F__)
    uint8_t*       d = static_cast<uint8_t*>(dst);
    const uint8_t* s = static_cast<const uint8_t*>(src);

    // 2×512-bit unrolled = 128 B/iter.
    while (n >= 128u) {
        __m512i v0 = _mm512_loadu_si512(s);
        __m512i v1 = _mm512_loadu_si512(s + 64);
        _mm512_storeu_si512(d,       v0);
        _mm512_storeu_si512(d + 64u, v1);
        s += 128u; d += 128u; n -= 128u;
    }
    if (n >= 64u) {
        _mm512_storeu_si512(d, _mm512_loadu_si512(s));
        s += 64u; d += 64u; n -= 64u;
    }
    // Residue: fall to AVX2 if available.
    if (n >= 32u) {
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(d),
            _mm256_loadu_si256(reinterpret_cast<const __m256i*>(s)));
        s += 32u; d += 32u; n -= 32u;
        _mm256_zeroupper();
    }
    if (n) std::memcpy(d, s, n);
#elif PHYRIAD_TRANSPORT_HAS_SIMD && defined(__AVX2__)
    slot_copy_avx2(dst, src, n);
#else
    std::memcpy(dst, src, n);
#endif
}

// ── slot_copy_non_temporal ────────────────────────────────────────────────────
void slot_copy_non_temporal(void* dst, const void* src, uint32_t n) noexcept {
#if PHYRIAD_TRANSPORT_HAS_SIMD && defined(__AVX2__)
    uint8_t*       d = static_cast<uint8_t*>(dst);
    const uint8_t* s = static_cast<const uint8_t*>(src);

    // NT stores require 32B-aligned destination.
    // Ring slots are aligned to kDestructivePad (128 B ≥ 32 B) — always satisfied.
    if (reinterpret_cast<uintptr_t>(d) & 31u) {
        slot_copy_avx2(dst, src, n);  // Unaligned fallback — temporal.
        return;
    }

    // 4×NT-256-bit unrolled = 128 B/iter, bypassing L1/L2.
    while (n >= 128u) {
        __m256i v0 = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(s));
        __m256i v1 = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(s + 32));
        __m256i v2 = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(s + 64));
        __m256i v3 = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(s + 96));
        _mm256_stream_si256(reinterpret_cast<__m256i*>(d),       v0);
        _mm256_stream_si256(reinterpret_cast<__m256i*>(d + 32),  v1);
        _mm256_stream_si256(reinterpret_cast<__m256i*>(d + 64),  v2);
        _mm256_stream_si256(reinterpret_cast<__m256i*>(d + 96),  v3);
        s += 128u; d += 128u; n -= 128u;
    }
    while (n >= 32u) {
        _mm256_stream_si256(reinterpret_cast<__m256i*>(d),
            _mm256_loadu_si256(reinterpret_cast<const __m256i*>(s)));
        s += 32u; d += 32u; n -= 32u;
    }
    if (n) std::memcpy(d, s, n);

    // SFENCE: serialize NT stores — ensures visibility before slot publication.
    _mm_sfence();
    _mm256_zeroupper();
#else
    std::memcpy(dst, src, n);
#endif
}

// ── pick_slot_copy ────────────────────────────────────────────────────────────
SlotCopyFn pick_slot_copy(SlotCopyMode mode, uint32_t slot_size) noexcept {
    switch (mode) {
        case SlotCopyMode::Scalar:
            return slot_copy_scalar;

        case SlotCopyMode::Avx2:
            return has_avx2() ? slot_copy_avx2 : slot_copy_scalar;

        case SlotCopyMode::Avx512:
            if (has_avx512f()) return slot_copy_avx512;
            if (has_avx2())   return slot_copy_avx2;
            return slot_copy_scalar;

        case SlotCopyMode::NonTemporal:
            return has_avx2() ? slot_copy_non_temporal : slot_copy_scalar;

        case SlotCopyMode::Auto:
        default:
            // Revised AUTO dispatch after empirical bench
            // (bench_slot_copy.cpp on AMD Zen 4 V-Cache + Intel measured paths).
            //
            // Findings:
            //   1. AVX-512 ≈ AVX2 throughput on Zen 4 (256-bit datapath
            //      internally, AVX-512 is double-pumped — no gain).
            //   2. NT-store path is 100× slower than AVX2/AVX-512 for ALL
            //      sizes ≥ 64 B in transport-style use (where the consumer
            //      reads the slot immediately after publish). NT bypasses L1
            //      so the consumer pays a full LLC/RAM round-trip, while
            //      AVX-512 leaves the slot warm in L1 for instant consume.
            //   3. The old "≥256 B → NT" heuristic was a regression for
            //      transport: Ring/RingChannel always re-read the slot.
            //
            // New heuristic: never auto-select NT. NT is opt-in via
            // SlotCopyMode::NonTemporal when the caller KNOWS the destination
            // is write-only-then-discarded (e.g. async logging to RAM).
            //   <  32 B   → scalar (SIMD setup > payload)
            //   32-127 B  → AVX-512 if available, else AVX2 (best for one-line copies)
            //   ≥ 128 B   → AVX2 (Zen 4 datapath caps at 256-bit anyway;
            //                     AVX-512 ties at best, AVX2 has lower frequency
            //                     impact on older Intel SKUs)
            if (slot_size < 32u) {
                return slot_copy_scalar;
            }
            if (slot_size < 128u) {
                if (has_avx512f()) return slot_copy_avx512;
                if (has_avx2())    return slot_copy_avx2;
                return slot_copy_scalar;
            }
            // ≥ 128 B
            if (has_avx2())   return slot_copy_avx2;
            return slot_copy_scalar;
    }
}

} // namespace phyriad::transport
// Made with my soul - Swately <3
