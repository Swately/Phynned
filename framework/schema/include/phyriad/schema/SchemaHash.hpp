// framework/schema/include/phyriad/schema/SchemaHash.hpp
// Compile-time XXH3_128 type fingerprinting for cross-process schema validation.
// schema_hash<T>() produces a unique 128-bit identifier baked into every binary
// that uses type T — mismatched versions fail at attach time, not silently.
//
// ANTI-PATTERNS:
//   ❌ typeid(T).hash_code()  — not portable across processes/binaries
//   ❌ runtime_hash<T>()      — unnecessary overhead; schema never changes post-compile
//   ❌ Omit HAL_version       — layout mismatch if HAL constants change
#pragma once
#include "xxh3_inline.hpp"
#include <phyriad/hal/Cacheline.hpp>   // hal::kDestructivePad
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace phyriad::schema {

// ── HAL version sentinel ──────────────────────────────────────────
// Bump this whenever the SHM/wire layout changes in an incompatible way.
// Incorporated into every schema_hash<T>() → stale attachers fail at startup.
inline constexpr uint64_t kPhyriadHalVersion = 7u; // v_b_7

// ── Hash128 ───────────────────────────────────────────────────────
struct Hash128 {
    uint64_t low{0};   // = XXH3_128bits().low64
    uint64_t high{0};  // = XXH3_128bits().high64
    [[nodiscard]] constexpr bool operator==(const Hash128&) const noexcept = default;
    [[nodiscard]] constexpr bool operator!=(const Hash128&) const noexcept = default;
    [[nodiscard]] constexpr bool is_zero() const noexcept { return low == 0 && high == 0; }
};
static_assert(sizeof(Hash128) == 16);
static_assert(std::is_trivially_copyable_v<Hash128>);

// ── XXH3State ─────────────────────────────────────────────────────
// Streaming accumulator for consteval use.
// Buffer capacity: 512 bytes — more than enough for sizeof+alignof+__PRETTY_FUNCTION__+version.
// Input beyond capacity is silently clamped (schema inputs are bounded).
class XXH3State {
public:
    static constexpr size_t kBufCap = 512u;

    consteval XXH3State() noexcept = default;

    consteval void update(std::string_view sv) noexcept {
        for (char c : sv) {
            if (pos_ < kBufCap) buf_[pos_++] = static_cast<std::byte>(c);
        }
    }

    consteval void update(uint64_t v) noexcept {
        if (pos_ + 8 <= kBufCap) {
            // Store as little-endian bytes
            for (int i = 0; i < 8; ++i)
                buf_[pos_++] = static_cast<std::byte>((v >> (i * 8)) & 0xFFu);
        }
    }

    // Mix a previously-computed Hash128 into the state (used in StaticGraph).
    consteval void update(Hash128 h) noexcept {
        update(h.low);
        update(h.high);
    }

    [[nodiscard]] consteval Hash128 digest_128() const noexcept {
        const auto r = xxh3::xxh3_128(buf_, pos_);
        return { r.low64, r.high64 };
    }

private:
    std::array<std::byte, kBufCap> buf_{};
    size_t pos_{0};
};

// ── schema_hash<T> ────────────────────────────────────────────────
// Computes a deterministic 128-bit fingerprint of type T's layout.
//
// Components hashed (order matters for hash uniqueness):
//   1. sizeof(T)                          — catches size changes
//   2. alignof(T)                         — catches alignment changes
//   3. __PRETTY_FUNCTION__                — catches renames and type substitution
//   4. kPhyriadHalVersion                   — catches HAL/layout version bumps
//   5. hal::kDestructivePad               — catches cacheline constant changes
//
// Usage:
//   static constexpr auto h = schema_hash<MyTick>();
//   static_assert(h != Hash128{});          // compile-time non-zero check
template <typename T>
[[nodiscard]] consteval Hash128 schema_hash() noexcept {
    XXH3State s{};
    s.update(sizeof(T));
    s.update(alignof(T));
#if defined(_MSC_VER)
    s.update(std::string_view{__FUNCSIG__});           // unique per T (MSVC)
#else
    s.update(std::string_view{__PRETTY_FUNCTION__});   // unique per T (GCC/Clang)
#endif
    s.update(kPhyriadHalVersion);
    s.update(static_cast<uint64_t>(hal::kDestructivePad));
    return s.digest_128();
}

// ── Compile-time non-zero probe ───────────────────────────────────
// The empty-state hash is non-zero (sec64 XOR mixing ensures this).
// Note: our consteval implementation uses a simplified XXH3 path that
// produces deterministic but spec-incompatible values for the empty case.
static_assert(!XXH3State{}.digest_128().is_zero(),
    "XXH3 empty hash must be non-zero");

} // namespace phyriad::schema
// Made with my soul - Swately <3
