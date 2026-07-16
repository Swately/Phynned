// framework/hal/include/phyriad/hal/HAL.hpp
// Header paraguas de la Hardware Abstraction Layer de Phyriad.
//
// Incluir este header da acceso a toda la HAL desde un solo punto.
// Los headers individuales también son incluibles directamente si sólo
// se necesita una subsección (ej. Timestamp.hpp en tests de timing).
//
// Dependencias (en orden de inclusión):
//   Arch.hpp       — macros de arquitectura/compilador (raíz, sin dependencias)
//   Cacheline.hpp  — kCachelineSize, kDestructivePad, kMessageSlotSize
//   MemoryOrder.hpp — seq_store_release, seq_load_acquire, full_fence, spin_hint
//   Timestamp.hpp  — rdtsc(), tsc_freq_hz(), calibrate_tsc_freq(), tsc_to_ns()
//   Prefetch.hpp   — prefetch<H>(), prefetch_range(), kRingPrefetchSlots
//   Simd.hpp       — SimdCaps, detect_simd_caps(), slot_copy_fixed64(), ...
//
// Uso:
//   #include <phyriad/hal/HAL.hpp>
//   // Da acceso a todo el namespace phyriad::hal.
//

#pragma once
#include "Arch.hpp"// IWYU pragma: export
#include "Cacheline.hpp"// IWYU pragma: export
#include "MemoryOrder.hpp"// IWYU pragma: export
#include "Timestamp.hpp"// IWYU pragma: export
#include "Prefetch.hpp"// IWYU pragma: export
#include "Simd.hpp"// IWYU pragma: export

// ── Resumen de la API pública ─────────────────────────────────────────────────
//
// namespace phyriad::hal {
//
//   // Arch (macros, no namespace)
//   PHYRIAD_ARCH_X86_64, PHYRIAD_ARCH_AARCH64, PHYRIAD_ARCH_APPLE
//   PHYRIAD_ALWAYS_INLINE, PHYRIAD_FLATTEN
//
//   // Cacheline
//   constexpr kCachelineSize    — 64 B (x86/ARM) / 128 B (Apple Silicon)
//   constexpr kDestructivePad   — 128 B (x86), 256 B (Apple Silicon)
//   constexpr kMessageSlotSize  — 64 B
//
//   // MemoryOrder
//   seq_store_release(atomic, val)    — publicar seq/índice en ring (writer)
//   seq_load_acquire(atomic)          — consumir seq/índice del ring (reader)
//   ctrl_store_release(atomic, val)   — publicar status/handshake de control
//   ctrl_load_acquire(atomic)         — leer status/handshake de control
//   stat_store_relaxed(atomic, val)   — contador estadístico (sin sincronización)
//   stat_load_relaxed(atomic)         — leer contador estadístico
//   stat_fetch_add_relaxed(atomic, d) — incrementar contador estadístico
//   full_fence()                      — barrera total (MFENCE/DMB ISH) — path frío
//   spin_hint()                       — PAUSE/YIELD en spin loop
//   compiler_fence()                  — barrera de compilador (sin instrucción HW)
//
//   // Timestamp
//   tsc_t                    — uint64_t alias para cycle counter
//   rdtsc()                  — leer cycle counter (~7-20 cycles en x86)
//   tsc_freq_hz()            — frecuencia via CPUID/cntfrq_el0
//   calibrate_tsc_freq()     — calibración empírica (~10 ms, una sola vez)
//   tsc_to_ns(delta)         — cycles → nanosegundos
//
//   // Prefetch
//   PrefetchHint             — Read, Write, NonTemporal
//   prefetch<H>(ptr)         — software prefetch de línea de cache
//   prefetch_range<H>(base, count, stride)
//   kRingPrefetchSlots       — 16 (distancia recomendada para ring consumer)
//
//   // Simd
//   SimdCaps                 — {avx2, avx512f, neon, sve, has_crc}
//   detect_simd_caps()       — detección completa de capacidades
//   simd_caps()              — singleton const& de SimdCaps
//   slot_copy_fixed64(d,s)   — copia inline de 64 B (compile-time SIMD dispatch)
//   payload_copy_fixed32(d,s)— copia inline de 32 B
//
// } // namespace phyriad::hal
// Made with my soul - Swately <3
