// framework/transport/include/phyriad/transport/SlotCopy.hpp
// SIMD slot copy backend — runtime-dispatch via function pointer.
//
// Design: one CPUID probe at init → selects the best SlotCopyFn for this CPU.
// The function pointer is stored once per ring channel and called in the hot path.
// Zero virtual dispatch overhead. Zero template proliferation in the user API.
//
// Implementations (all defined in SlotCopy.cpp):
//   slot_copy_scalar        — std::memcpy fallback (universal)
//   slot_copy_avx2          — 256-bit unaligned loads/stores (ymm0-ymm3)
//   slot_copy_avx512        — 512-bit stores (zmm0-zmm1)
//   slot_copy_non_temporal  — MOVNTDQ — bypass L1/L2 for large slots
//
// Selection policy (pick_slot_copy AUTO — revised after bench_slot_copy.cpp):
//   slot_size <   32 B → scalar   (SIMD overhead > gain for tiny data)
//   slot_size <  128 B → AVX-512 if available, else AVX2, else scalar
//   slot_size >= 128 B → AVX2 if available, else scalar
//   NT is NEVER auto-selected: it bypasses L1/L2, so a consumer that re-reads the
//   slot (Ring/RingChannel always do) eats a full LLC/RAM round-trip (~100× slower
//   in-bench). Opt in via SlotCopyMode::NonTemporal only for write-only-then-
//   discarded destinations. (See the AUTO heuristic in SlotCopy.cpp.)
//
// pick_slot_copy() is called ONCE at ring construction. The returned function
// pointer is stored and called on every publish/consume.
//
// Compatibility: SlotCopyFn is ABI-stable — the pointer can be stored across
// library reload as long as the function bodies are in the same binary.
//

#pragma once
#include <phyriad/hal/Arch.hpp>   // PHYRIAD_ARCH_X86_64, PHYRIAD_HAS_SIMD_HEADERS
#include <cstdint>
#include <cstring>

// ── SIMD intrinsics (only when available) ────────────────────────────────────
#if defined(__AVX512F__) || defined(__AVX2__) || \
    defined(_M_AMD64) || defined(_M_X64) || defined(__x86_64__)
#  if defined(_MSC_VER) && !defined(__clang__)
#    include <intrin.h>
#  else
#    include <immintrin.h>
#  endif
#  define PHYRIAD_TRANSPORT_HAS_SIMD 1
#else
#  define PHYRIAD_TRANSPORT_HAS_SIMD 0
#endif

namespace phyriad::transport {

// ── SlotCopyFn — the function pointer type ───────────────────────────────────
// dst: pointer to the start of slot user-data region.
// src: pointer to the user's message.
// n:   number of bytes to copy (== sizeof(T), constant per ring instance).
using SlotCopyFn = void(*)(void* dst, const void* src, uint32_t n) noexcept;

// ── SlotCopyMode — copy strategy selector ────────────────────────────────────
// Used as argument to pick_slot_copy(). Choose AUTO for production; explicit
// modes are useful for benchmarking and manual override.
enum class SlotCopyMode : uint8_t {
    Auto         = 0,  // CPUID-based automatic selection (recommended)
    Scalar       = 1,  // always std::memcpy
    Avx2         = 2,  // always AVX2 256-bit (falls back to scalar if unsupported)
    Avx512       = 3,  // always AVX-512F 512-bit (falls back via Avx2 → Scalar)
    NonTemporal  = 4,  // always NT stores (bypass L1/L2; falls back to scalar)
};

// ── SIMD copy functions ───────────────────────────────────────────────────────
// Declared here (defined in SlotCopy.cpp) — visible for benchmarking.

// Scalar fallback — std::memcpy. Always available.
void slot_copy_scalar(void* dst, const void* src, uint32_t n) noexcept;

// AVX2 256-bit stores (4×32 B/iter unrolled). Requires AVX2-capable CPU.
// Falls back to scalar at runtime if called on non-AVX2 hardware.
void slot_copy_avx2(void* dst, const void* src, uint32_t n) noexcept;

// AVX-512F 512-bit stores (2×64 B/iter unrolled). Requires AVX-512F CPU.
// Falls back to AVX2 or scalar at runtime if unsupported.
void slot_copy_avx512(void* dst, const void* src, uint32_t n) noexcept;

// Non-temporal stores — MOVNTDQ / VMOVNTDQ.
// Bypasses L1/L2 (write-combining → LLC/DRAM). Requires 32B-aligned dst.
// Best for slots >= 256 B where the destination is not read soon.
// Followed by SFENCE to guarantee visibility before slot publication.
void slot_copy_non_temporal(void* dst, const void* src, uint32_t n) noexcept;

// ── pick_slot_copy ────────────────────────────────────────────────────────────
// Selects the best SlotCopyFn for the given mode and slot size.
// Performs one-time CPUID detection (cached in a static bool).
// Call ONCE at ring construction time and store the returned pointer.
//
// Example:
//   SlotCopyFn copy_fn = pick_slot_copy(SlotCopyMode::Auto, sizeof(MyTick));
//   // in hot path:
//   copy_fn(slot_ptr, &msg, sizeof(MyTick));
[[nodiscard]] SlotCopyFn pick_slot_copy(SlotCopyMode mode, uint32_t slot_size) noexcept;

} // namespace phyriad::transport
// Made with my soul - Swately <3
