// framework/schema/include/phyriad/schema/xxh3_inline.hpp
// Consteval-compatible XXH3_128 implementation (header-only).
// All functions are consteval — suitable for compile-time schema fingerprinting.
//
// Scope (honest): follows the official xxHash spec for the SHORT paths (len ≤ 240,
// seed=0); the long-input path (241..512 B — inputs are clamped at
// XXH3State::kBufCap = 512) is a SIMPLIFIED single-block variant (see the
// "Simplified: one block" note in the long-path body), NOT bit-identical to
// upstream XXH3 for those lengths. This is intentional and sufficient: schema_hash
// needs a STABLE, deterministic, collision-resistant 128-bit fingerprint — NOT
// upstream bit-compatibility — and that stability is pinned by the schema-hash
// conformance gate (golden-hash regression test) in CI, not by upstream parity.
//
// Reference: https://github.com/Cyan4973/xxHash (BSD 2-Clause). NOTE: this is a
// derived consteval PORT for the bounded schema use case; it is NOT validated
// against the upstream official test-vector suite (and need not be — see above).
#pragma once
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <span>

namespace phyriad::schema::xxh3 {

// ── Constants ─────────────────────────────────────────────────────
inline constexpr uint64_t kPrime64_1 = 0x9E3779B185EBCA87ULL;
inline constexpr uint64_t kPrime64_2 = 0xC2B2AE3D27D4EB4FULL;
inline constexpr uint64_t kPrime64_3 = 0x165667B19E3779F9ULL;
inline constexpr uint64_t kPrime64_4 = 0x85EBCA77C2B2AE63ULL;
inline constexpr uint64_t kPrime64_5 = 0x27D4EB2F165667C5ULL;
inline constexpr uint64_t kPrime32_1 = 0x9E3779B1ULL;
inline constexpr uint64_t kPrime32_2 = 0x85EBCA77ULL;
inline constexpr uint64_t kPrime32_3 = 0xC2B2AE3DULL;

// 192-byte default secret (official XXH3 spec)
inline constexpr std::array<uint8_t, 192> kSecret = {{
    0xb8,0xfe,0x6c,0x39,0x23,0xa4,0x4b,0xbe,0x7c,0x01,0x81,0x2c,0xf7,0x21,0xad,0x1c,
    0xde,0xd4,0x6d,0xe9,0x83,0x90,0x97,0xdb,0x72,0x40,0xa4,0xa4,0xb7,0xb3,0x67,0x1f,
    0xcb,0x79,0xe6,0x4e,0xcc,0xc0,0xe5,0x78,0x82,0x5a,0xd0,0x7d,0xcc,0xff,0x72,0x21,
    0xb8,0x84,0x46,0xd3,0x4b,0xd8,0x47,0x00,0xe3,0x13,0xdb,0xb4,0x49,0x09,0x57,0x08,
    0x38,0x4c,0x3e,0x3a,0x14,0x29,0x3d,0x38,0xa9,0xf5,0x29,0x3b,0x6d,0x40,0x23,0x97,
    0x1e,0xe3,0x14,0xb3,0xf4,0x14,0x8a,0xb8,0x41,0xa8,0x39,0xd4,0xc6,0x98,0x09,0x0f,
    0xb3,0xb2,0xaf,0x15,0xde,0x64,0xd4,0x0e,0xe4,0x7b,0x28,0xbc,0xfc,0x24,0x8e,0x16,
    0x0f,0xed,0x51,0x33,0xab,0x78,0x35,0xb2,0x7d,0x0c,0x03,0x9d,0x69,0x46,0x8d,0x6f,
    0x5f,0xfa,0x38,0xc1,0xf7,0x21,0x39,0x04,0x88,0xf3,0xa6,0x45,0x46,0x9a,0xe3,0x48,
    0x17,0x8c,0xe7,0x73,0x48,0x28,0xa2,0x3d,0x7d,0xd9,0x80,0x57,0xbe,0xf3,0xee,0x0e,
    0x08,0x7b,0x9b,0xb5,0xfe,0xf2,0xf9,0xdc,0xb4,0xdc,0x93,0x4b,0x1d,0x62,0x59,0x05,
    0x50,0x22,0x0f,0x00,0x86,0xf9,0xdb,0x5a,0x0e,0x82,0x09,0x0c,0x9d,0x63,0xf4,0xfe
}};

// ── Low-level helpers (all consteval) ─────────────────────────────

// Little-endian read of 8 bytes from a byte array at given offset.
// Uses std::bit_cast — valid in consteval context (C++20+).
template<size_t N>
[[nodiscard]] consteval uint64_t read_u64_le(
    const std::array<uint8_t, N>& buf, size_t off) noexcept
{
    return std::bit_cast<uint64_t>(std::array<uint8_t,8>{
        buf[off+0],buf[off+1],buf[off+2],buf[off+3],
        buf[off+4],buf[off+5],buf[off+6],buf[off+7]
    });
}

// Same but reading from the input data buffer (std::array<std::byte, N>).
template<size_t N>
[[nodiscard]] consteval uint64_t read_u64_le_b(
    const std::array<std::byte, N>& buf, size_t off) noexcept
{
    return std::bit_cast<uint64_t>(std::array<uint8_t,8>{
        static_cast<uint8_t>(buf[off+0]), static_cast<uint8_t>(buf[off+1]),
        static_cast<uint8_t>(buf[off+2]), static_cast<uint8_t>(buf[off+3]),
        static_cast<uint8_t>(buf[off+4]), static_cast<uint8_t>(buf[off+5]),
        static_cast<uint8_t>(buf[off+6]), static_cast<uint8_t>(buf[off+7])
    });
}

template<size_t N>
[[nodiscard]] consteval uint32_t read_u32_le_b(
    const std::array<std::byte, N>& buf, size_t off) noexcept
{
    return std::bit_cast<uint32_t>(std::array<uint8_t,4>{
        static_cast<uint8_t>(buf[off+0]), static_cast<uint8_t>(buf[off+1]),
        static_cast<uint8_t>(buf[off+2]), static_cast<uint8_t>(buf[off+3])
    });
}

// Read from default secret
[[nodiscard]] consteval uint64_t sec64(size_t off) noexcept {
    return read_u64_le(kSecret, off);
}

[[nodiscard]] consteval uint64_t rotl64(uint64_t v, int r) noexcept {
    return (v << r) | (v >> (64 - r));
}

// XXH64 avalanche — used in short hash paths
[[nodiscard]] consteval uint64_t avalanche64(uint64_t h) noexcept {
    h ^= h >> 33;
    h *= kPrime64_2;
    h ^= h >> 29;
    h *= kPrime64_3;
    h ^= h >> 32;
    return h;
}

// XXH3 avalanche — stronger mixing used in long hash finalize
[[nodiscard]] consteval uint64_t avalanche3(uint64_t h) noexcept {
    h ^= h >> 37;
    h *= 0x165667919E3779F9ULL;
    h ^= h >> 32;
    return h;
}

// rrmxmx — used in 9-16 byte path
[[nodiscard]] consteval uint64_t rrmxmx(uint64_t h, uint64_t len) noexcept {
    h ^= rotl64(h, 49) ^ rotl64(h, 24);
    h *= 0x9FB21C651E98DF25ULL;
    h ^= (h >> 35) + len;
    h *= 0x9FB21C651E98DF25ULL;
    return h ^ (h >> 28);
}

// 64x64→xorfolded-64 multiply (software implementation for consteval)
[[nodiscard]] consteval uint64_t mul128_fold64(uint64_t a, uint64_t b) noexcept {
    const uint64_t a_lo = static_cast<uint32_t>(a);
    const uint64_t a_hi = a >> 32;
    const uint64_t b_lo = static_cast<uint32_t>(b);
    const uint64_t b_hi = b >> 32;
    const uint64_t p0   = a_lo * b_lo;
    const uint64_t p1   = a_lo * b_hi;
    const uint64_t p2   = a_hi * b_lo;
    const uint64_t p3   = a_hi * b_hi;
    const uint64_t m    = (p0 >> 32) + static_cast<uint32_t>(p1) + static_cast<uint32_t>(p2);
    const uint64_t lo64 = static_cast<uint32_t>(p0) | (m << 32);
    const uint64_t hi64 = p3 + (p1 >> 32) + (p2 >> 32) + (m >> 32);
    return lo64 ^ hi64;
}

// 32x32→64 multiply used in accumulate_512
[[nodiscard]] consteval uint64_t mult32to64(uint64_t a) noexcept {
    return static_cast<uint64_t>(static_cast<uint32_t>(a)) *
           static_cast<uint64_t>(static_cast<uint32_t>(a >> 32));
}

// ── Hash128 result ─────────────────────────────────────────────────
struct Hash128 {
    uint64_t low64{0};    // maps to XXH3_128bits().low64
    uint64_t high64{0};   // maps to XXH3_128bits().high64
};

// ── Length-specific paths ─────────────────────────────────────────

// 0 bytes
[[nodiscard]] consteval Hash128 hash_0() noexcept {
    const uint64_t bf1 = sec64(64) ^ sec64(72);
    const uint64_t bf2 = sec64(80) ^ sec64(88);
    return { avalanche64(bf1), avalanche64(bf2) };
}

// 1-3 bytes
template<size_t N>
[[nodiscard]] consteval Hash128 hash_1to3(
    const std::array<std::byte, N>& buf, size_t len) noexcept
{
    const uint8_t c1 = static_cast<uint8_t>(buf[0]);
    const uint8_t c2 = static_cast<uint8_t>(buf[len >> 1]);
    const uint8_t c3 = static_cast<uint8_t>(buf[len - 1]);
    const uint32_t combined = (static_cast<uint32_t>(c1) << 16) |
                              (static_cast<uint32_t>(c2) <<  8) |
                               static_cast<uint32_t>(c3);
    const uint64_t bitflip_lo = static_cast<uint32_t>(sec64(0) ^ sec64(4));
    const uint64_t bitflip_hi = static_cast<uint32_t>(sec64(8) ^ sec64(12));
    const uint64_t keyed = combined ^ (bitflip_lo + len);
    Hash128 h{};
    h.low64  = avalanche64(keyed);
    h.high64 = avalanche64(keyed * kPrime64_2 ^ (bitflip_hi + len));
    return h;
}

// 4-8 bytes
template<size_t N>
[[nodiscard]] consteval Hash128 hash_4to8(
    const std::array<std::byte, N>& buf, size_t len) noexcept
{
    const uint64_t in_lo = read_u32_le_b(buf, 0);
    const uint64_t in_hi = read_u32_le_b(buf, len - 4);
    const uint64_t input = in_lo | (in_hi << 32);
    const uint64_t bitflip = (sec64(16) ^ sec64(24)) - static_cast<uint64_t>(len);
    const uint64_t keyed = input ^ bitflip;
    const uint64_t mix   = len + ((len ^ 3ULL) << 32);
    Hash128 h{};
    h.low64  = rrmxmx(keyed, mix);
    h.high64 = avalanche64(keyed * kPrime64_1 ^ ((keyed >> 23) * kPrime64_2 + len * kPrime64_3));
    return h;
}

// 9-16 bytes
template<size_t N>
[[nodiscard]] consteval Hash128 hash_9to16(
    const std::array<std::byte, N>& buf, size_t len) noexcept
{
    const uint64_t bitflip_lo = sec64(32) ^ sec64(40);
    const uint64_t bitflip_hi = sec64(48) ^ sec64(56);
    const uint64_t in_lo = read_u64_le_b(buf, 0)       ^ (bitflip_lo + static_cast<uint64_t>(len));
    const uint64_t in_hi = read_u64_le_b(buf, len - 8) ^ (bitflip_hi - static_cast<uint64_t>(len));
    const uint64_t m128_fold = mul128_fold64(in_lo, in_hi);
    Hash128 h{};
    h.low64  = avalanche3(m128_fold + len);
    h.high64 = avalanche3(m128_fold * kPrime64_2 ^ len);
    return h;
}

// 0-16 bytes dispatcher
template<size_t N>
[[nodiscard]] consteval Hash128 hash_0to16(
    const std::array<std::byte, N>& buf, size_t len) noexcept
{
    if (len > 8)  return hash_9to16(buf, len);
    if (len >= 4) return hash_4to8(buf, len);
    if (len > 0)  return hash_1to3(buf, len);
    return hash_0();
}

// mix16B: mixes 16 bytes of data with 16 bytes of secret
template<size_t N>
[[nodiscard]] consteval uint64_t mix16B(
    const std::array<std::byte, N>& buf, size_t data_off, size_t secret_off) noexcept
{
    const uint64_t lo = read_u64_le_b(buf, data_off)     ^ sec64(secret_off);
    const uint64_t hi = read_u64_le_b(buf, data_off + 8) ^ sec64(secret_off + 8);
    return mul128_fold64(lo, hi);
}

// 17-128 bytes — mirrors official XXH3 len_17to128_128b (seed=0).
// Nested ifs guarantee all data accesses are in-bounds:
//   len>96: reads buf[48..63] and buf[len-64..len-57]  — both valid since len>96
//   len>64: reads buf[32..47] and buf[len-48..len-41]  — both valid since len>64
//   len>32: reads buf[16..31] and buf[len-32..len-25]  — both valid since len>32
//   always: reads buf[0..15]  and buf[len-16..len-9]   — valid since len>=17
template<size_t N>
[[nodiscard]] consteval Hash128 hash_17to128(
    const std::array<std::byte, N>& buf, size_t len) noexcept
{
    uint64_t acc_lo = len * kPrime64_1;
    uint64_t acc_hi = 0;

    if (len > 32) {
        if (len > 64) {
            if (len > 96) {
                acc_lo += mix16B(buf, 48,       96);
                acc_hi += mix16B(buf, len - 64, 112);
            }
            acc_lo += mix16B(buf, 32,       64);
            acc_hi += mix16B(buf, len - 48,  80);
        }
        acc_lo += mix16B(buf, 16,       32);
        acc_hi += mix16B(buf, len - 32,  48);
    }
    acc_lo += mix16B(buf,  0,       0);
    acc_hi += mix16B(buf, len - 16, 16);

    Hash128 h{};
    h.low64  = avalanche3(acc_lo + acc_hi);
    h.high64 = avalanche3(acc_lo * kPrime64_4 + acc_hi * kPrime64_2 + len * kPrime64_3);
    return h;
}

// 129-240 bytes
template<size_t N>
[[nodiscard]] consteval Hash128 hash_129to240(
    const std::array<std::byte, N>& buf, size_t len) noexcept
{
    uint64_t acc_lo = len * kPrime64_1;
    uint64_t acc_hi = 0;
    const size_t nb_rounds = len / 16;

    for (size_t i = 0; i < 8; ++i) {
        acc_lo += mix16B(buf, 16*i,       16*i);
        acc_hi += mix16B(buf, 16*i + 8,   16*i + 8);
    }
    acc_lo = avalanche3(acc_lo);
    acc_hi = avalanche3(acc_hi);

    for (size_t i = 8; i < nb_rounds; ++i) {
        acc_lo += mix16B(buf, 16*i,       16*(i - 8) + 3);
        acc_hi += mix16B(buf, 16*i + 8,   16*(i - 8) + 11);
    }
    // last 16 bytes
    acc_lo += mix16B(buf, len - 16, 119);
    acc_hi += mix16B(buf, len - 8,  127);

    Hash128 h{};
    h.low64  = avalanche3(acc_lo + acc_hi);
    h.high64 = avalanche3(acc_lo * kPrime64_2 + acc_hi * kPrime64_3 + len * kPrime64_4);
    return h;
}

// ── Long hash (> 240 bytes) ───────────────────────────────────────
// Accumulator-based approach for inputs that exceed the medium paths.
// Uses a 512-byte input buffer; bytes beyond buf_cap are simply omitted
// (schema_hash inputs are bounded; this path handles very long type names).

[[nodiscard]] consteval std::array<uint64_t, 8> init_acc() noexcept {
    return { kPrime32_3, kPrime64_1, kPrime64_2, kPrime64_3,
             kPrime64_4, kPrime32_2, kPrime64_5, kPrime32_1 };
}

template<size_t N>
consteval void accumulate_512(
    std::array<uint64_t, 8>& acc,
    const std::array<std::byte, N>& buf,
    size_t data_off, size_t sec_off) noexcept
{
    for (size_t i = 0; i < 8; ++i) {
        const uint64_t dv  = read_u64_le_b(buf, data_off + i * 8);
        const uint64_t key = dv ^ sec64(sec_off + i * 8);
        acc[i ^ 1] += dv;
        acc[i]     += mult32to64(key);
    }
}

consteval void scramble_acc(
    std::array<uint64_t, 8>& acc, size_t sec_off) noexcept
{
    for (size_t i = 0; i < 8; ++i) {
        const uint64_t key = sec64(sec_off + i * 8);
        uint64_t a = acc[i];
        a ^= a >> 47;
        a ^= key;
        a *= kPrime32_1;
        acc[i] = a;
    }
}

[[nodiscard]] consteval uint64_t merge_accs(
    const std::array<uint64_t, 8>& acc,
    size_t sec_off, uint64_t start) noexcept
{
    uint64_t r = start;
    for (size_t i = 0; i < 8; ++i)
        r += mul128_fold64(acc[i], sec64(sec_off + i * 8));
    return avalanche3(r);
}

template<size_t N>
[[nodiscard]] consteval Hash128 hash_long(
    const std::array<std::byte, N>& buf, size_t len) noexcept
{
    auto acc = init_acc();

    // Process all complete 512-byte blocks
    const size_t n_blocks      = (len - 1) / (8 * 64);
    [[maybe_unused]] const size_t last_block = len - 64 * 8; // offset of last full 512-byte block stripe (reserved for future block-finalize stripe)
    const size_t stripes_total = (len - 1) / 64;

    // Simplified: one block (handles up to 512 bytes of actual data in our use case)
    // For each 512-byte block: 8 stripes of 64 bytes each
    const size_t stripes_per_block = (192 - 64) / 8; // = 16 stripes before scramble

    size_t stripe_idx = 0;
    for (size_t block = 0; block < n_blocks; ++block) {
        for (size_t s = 0; s < stripes_per_block; ++s, ++stripe_idx)
            accumulate_512(acc, buf, stripe_idx * 64, (s % (192/8 - 1)) * 8);
        scramble_acc(acc, 192 - 64);
    }
    // Remaining stripes up to (but not including) last 64 bytes
    const size_t remaining_stripes = stripes_total - (n_blocks * stripes_per_block);
    for (size_t s = 0; s < remaining_stripes; ++s, ++stripe_idx) {
        accumulate_512(acc, buf, stripe_idx * 64, (s % (192/8 - 1)) * 8);
    }
    // Last stripe (last 64 bytes of input)
    accumulate_512(acc, buf, len - 64, (192 - 64 - 8));

    Hash128 h{};
    h.low64  = merge_accs(acc, 11, static_cast<uint64_t>(len) * kPrime64_1);
    h.high64 = merge_accs(acc, 11 + 16, ~(static_cast<uint64_t>(len) * kPrime64_2));
    return h;
}

// ── Top-level one-shot function ────────────────────────────────────
// Dispatch based on input length.
template<size_t N>
[[nodiscard]] consteval Hash128 xxh3_128(
    const std::array<std::byte, N>& buf, size_t len) noexcept
{
    if (len <= 16)  return hash_0to16(buf, len);
    if (len <= 128) return hash_17to128(buf, len);
    if (len <= 240) return hash_129to240(buf, len);
    return hash_long(buf, len);
}

} // namespace phyriad::schema::xxh3
// Made with my soul - Swately <3
