// framework/hal/include/phyriad/hal/Prefetch.hpp
// Software prefetch portables para x86 y ARM.
//
// En el ring de SPSC de 448 M msg/s, el productor publica mensajes a ~2.2 ns/msg.
// Con latencia L1-hit de ~4 ciclos (~1 ns), el prefetch debe adelantarse ~16-32
// slots para mantener el pipeline lleno sin stalls de cache.
//
// Hints disponibles:
//   PrefetchHint::Read        — prefetch a L1 para lectura (T0 / PLDL1KEEP).
//   PrefetchHint::Write       — prefetch para escritura (PREFETCHW / PSTL1KEEP).
//   PrefetchHint::NonTemporal — sin polución de L1/L2 (NTA / PLDL2KEEP).
//
// Uso:
//   // Prefetch 4 eventos adelante en snapshot loop:
//   hal::prefetch<hal::PrefetchHint::Read>(&events_[(start + i + 4u) & kMask]);
//
//   // Prefetch de N slots del ring:
//   hal::prefetch_range<hal::PrefetchHint::Read>(base, 8, 64);
//

#pragma once
#include "Arch.hpp"
#include <cstddef>

namespace phyriad::hal {

enum class PrefetchHint : int {
    Read        = 0,  // _MM_HINT_T0 (x86) / prfm pldl1keep (ARM)
    Write       = 1,  // write-intent  (x86) / prfm pstl1keep (ARM)
    NonTemporal = 2,  // _MM_HINT_NTA (x86) / prfm pldl2keep (ARM)
};

// ─────────────────────────────────────────────────────────────────────────────
// prefetch<H>(p) — software prefetch de la línea de cache en p.
// ─────────────────────────────────────────────────────────────────────────────
// Siempre inline y sin costo en ausencia de HW support (instrucción ignorada).
template <PrefetchHint H = PrefetchHint::Read>
PHYRIAD_ALWAYS_INLINE void prefetch(const void* p) noexcept {
#if PHYRIAD_ARCH_X86_64
    if constexpr (H == PrefetchHint::Read) {
        _mm_prefetch(static_cast<const char*>(p), _MM_HINT_T0);
    } else if constexpr (H == PrefetchHint::Write) {
#  if PHYRIAD_COMPILER_GCC || PHYRIAD_COMPILER_CLANG
        __builtin_prefetch(p, /*write=*/1, /*locality=*/3);
#  else
        _mm_prefetch(static_cast<const char*>(p), _MM_HINT_T0);
#  endif
    } else {  // NonTemporal
        _mm_prefetch(static_cast<const char*>(p), _MM_HINT_NTA);
    }

#elif PHYRIAD_ARCH_AARCH64
    if constexpr (H == PrefetchHint::Read) {
        __asm__ volatile("prfm pldl1keep, %0" :: "Q"(*static_cast<const char*>(p)));
    } else if constexpr (H == PrefetchHint::Write) {
        __asm__ volatile("prfm pstl1keep, %0" :: "Q"(*static_cast<const char*>(p)));
    } else {  // NonTemporal
        __asm__ volatile("prfm pldl2keep, %0" :: "Q"(*static_cast<const char*>(p)));
    }

#else
    (void)p;
#if defined(__GNUC__) || defined(__clang__)
    __builtin_prefetch(p, (H == PrefetchHint::Write) ? 1 : 0, 3);
#endif
#endif
}

// ─────────────────────────────────────────────────────────────────────────────
// prefetch_range<H>(base, count, stride) — prefetch de N elementos con stride.
// ─────────────────────────────────────────────────────────────────────────────
// Ejemplo: prefetch_range<Read>(base, 8, 64) → prefetcha 8 líneas cada 64 B.
template <PrefetchHint H = PrefetchHint::Read>
PHYRIAD_ALWAYS_INLINE void prefetch_range(const void* base,
                                         std::size_t count,
                                         std::size_t stride) noexcept {
    const char* p = static_cast<const char*>(base);
    for (std::size_t i = 0; i < count; ++i, p += stride) {
        prefetch<H>(p);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Distancia de prefetch recomendada para el ring (en número de slots).
// ─────────────────────────────────────────────────────────────────────────────
// ~16 slots × 64 B/slot = 1 KB → L1 hit garantizado en 7950X3D con V-Cache.
inline constexpr std::size_t kRingPrefetchSlots = 16;

} // namespace phyriad::hal
// Made with my soul - Swately <3
