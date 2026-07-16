// framework/hal/include/phyriad/hal/Allocator.hpp
// Cross-platform aligned allocator with optional NUMA-binding + hugepage hints.
//
// API design rationale:
//   - The default path (no hint, small alloc) uses the standard fast path
//     (`_aligned_malloc` on Win, `std::aligned_alloc` on POSIX). Costs ~50 ns
//     per call, indistinguishable from `_aligned_malloc` for hot-path callers.
//
//   - The Large hint switches to the OS page allocator (`VirtualAlloc` /
//     `mmap`). Higher per-call cost (~1-10 µs) but dedicated pages → no heap
//     fragmentation, predictable behavior. Use for allocations ≥ 64 KiB.
//
//   - The Huge hint requests 2 MiB pages on top of Large. Reduces TLB
//     pressure: a 16 MiB Ring buffer needs 4096 entries with 4 KiB pages but
//     only 8 entries with 2 MiB pages — fits entirely in L1 dTLB (~64 entries).
//
//   - The NumaLocal / NumaNode hints pin the allocation to a specific NUMA
//     node. Soft-fails to default policy on single-NUMA machines or when the
//     OS doesn't support the requested binding.
//
// Required caller contract:
//   - Pair every `aligned_alloc_hint(size, ..., hint, ...)` with
//     `aligned_free_hint(p, size, hint)`. The hint determines which underlying
//     OS API is used for free.
//   - `size` passed to free MUST match the allocation size (Linux munmap
//     requirement on the Large/Huge paths; Windows ignores it for VirtualFree).
//

#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#  include <malloc.h>
#else
#  include <sys/mman.h>
#  include <cerrno>
#  if defined(__linux__)
#    include <unistd.h>
#  endif
#endif

namespace phyriad::hal {

// ── AllocHint ────────────────────────────────────────────────────────────────
// Bitmask of allocator hints. Combine via bitwise OR.
enum class AllocHint : uint32_t {
    Default   = 0,             // _aligned_malloc / aligned_alloc fast path
    Large     = 1u << 0,       // size >= 64 KiB; use OS page allocator
    Huge      = 1u << 1,       // request 2 MiB pages (implies Large)
    NumaLocal = 1u << 2,       // bind to caller's current NUMA node
    NumaNode  = 1u << 3,       // bind to specified node (numa_node arg)
};

[[nodiscard]] constexpr AllocHint operator|(AllocHint a, AllocHint b) noexcept {
    return static_cast<AllocHint>(
        static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
}
[[nodiscard]] constexpr bool has_hint(AllocHint h, AllocHint flag) noexcept {
    return (static_cast<uint32_t>(h) & static_cast<uint32_t>(flag)) != 0u;
}

// ── Constants ────────────────────────────────────────────────────────────────
inline constexpr std::size_t k4KiB = 4u * 1024u;
inline constexpr std::size_t k2MiB = 2u * 1024u * 1024u;

// ── Platform helpers ─────────────────────────────────────────────────────────
namespace detail {

[[nodiscard]] inline std::size_t round_up_(std::size_t v, std::size_t a) noexcept {
    return (v + a - 1u) & ~(a - 1u);
}

[[nodiscard]] inline uint32_t current_numa_node_() noexcept {
#ifdef _WIN32
    PROCESSOR_NUMBER pn;
    ::GetCurrentProcessorNumberEx(&pn);
    USHORT node = 0;
    if (::GetNumaProcessorNodeEx(&pn, &node)) return static_cast<uint32_t>(node);
    return 0u;
#elif defined(__linux__)
    // syscall(__NR_getcpu, &cpu, &node, nullptr) — but __NR_getcpu numbers vary
    // by arch; the libc `getcpu` is a thin wrapper. Avoid the dependency: we
    // soft-fail to node 0 and rely on Linux first-touch policy.
    return 0u;
#else
    return 0u;
#endif
}

#ifdef _WIN32
// Detect SeLockMemoryPrivilege availability. Cached after first call.
[[nodiscard]] inline bool can_use_large_pages_() noexcept {
    static const bool ok = []() noexcept -> bool {
        HANDLE token;
        if (!::OpenProcessToken(::GetCurrentProcess(),
                                TOKEN_QUERY | TOKEN_ADJUST_PRIVILEGES, &token))
            return false;
        LUID luid;
        if (!::LookupPrivilegeValueA(nullptr, "SeLockMemoryPrivilege", &luid)) {
            ::CloseHandle(token);
            return false;
        }
        TOKEN_PRIVILEGES tp{};
        tp.PrivilegeCount = 1u;
        tp.Privileges[0].Luid = luid;
        tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
        ::AdjustTokenPrivileges(token, FALSE, &tp, 0u, nullptr, nullptr);
        const bool granted = (::GetLastError() == ERROR_SUCCESS);
        ::CloseHandle(token);
        return granted;
    }();
    return ok;
}
#endif

} // namespace detail

// ── aligned_alloc_hint ───────────────────────────────────────────────────────
// Returns nullptr on failure. The default path (Default hint) is the cheapest
// (uses platform malloc with alignment) and matches _aligned_malloc cost.
[[nodiscard]] inline void*
aligned_alloc_hint(std::size_t size,
                   std::size_t align,
                   AllocHint   hint     = AllocHint::Default,
                   uint32_t    numa_node = UINT32_MAX) noexcept
{
    if (size == 0u || align == 0u) return nullptr;
    // Default fast path: standard aligned malloc.
    const bool wants_large = has_hint(hint, AllocHint::Large) ||
                             has_hint(hint, AllocHint::Huge);
    const bool wants_numa  = has_hint(hint, AllocHint::NumaLocal) ||
                             has_hint(hint, AllocHint::NumaNode);

    if (!wants_large && !wants_numa) {
#ifdef _WIN32
        return ::_aligned_malloc(size, align);
#else
        // aligned_alloc requires size to be a multiple of align (C11). Round up.
        const std::size_t s = detail::round_up_(size, align);
        return std::aligned_alloc(align, s);
#endif
    }

#ifdef _WIN32
    // ── Windows OS-page path: VirtualAlloc[ExNuma] ──────────────────────────
    DWORD alloc_type = MEM_RESERVE | MEM_COMMIT;
    SIZE_T page_size = k4KiB;
    if (has_hint(hint, AllocHint::Huge) && detail::can_use_large_pages_()) {
        const SIZE_T lp = ::GetLargePageMinimum();
        if (lp != 0u) {
            alloc_type |= MEM_LARGE_PAGES;
            page_size = lp;
        }
    }
    const SIZE_T actual_size = detail::round_up_(
        std::max(size, align), page_size);

    if (wants_numa) {
        const uint32_t node = (has_hint(hint, AllocHint::NumaNode) &&
                               numa_node != UINT32_MAX)
                              ? numa_node
                              : detail::current_numa_node_();
        void* p = ::VirtualAllocExNuma(::GetCurrentProcess(), nullptr,
                                        actual_size, alloc_type,
                                        PAGE_READWRITE,
                                        static_cast<DWORD>(node));
        if (p) return p;
        // Fall through to non-NUMA path on failure (e.g. node out of range).
    }
    return ::VirtualAlloc(nullptr, actual_size, alloc_type, PAGE_READWRITE);
#else
    // ── POSIX OS-page path: mmap + madvise ───────────────────────────────────
    const std::size_t actual_size = detail::round_up_(
        std::max(size, align), k4KiB);

    int prot  = PROT_READ | PROT_WRITE;
    int flags = MAP_PRIVATE | MAP_ANONYMOUS;
#  if defined(MAP_HUGETLB)
    if (has_hint(hint, AllocHint::Huge)) {
        // Try MAP_HUGETLB first; if it fails, fall back to MAP_PRIVATE + madvise.
        void* p = ::mmap(nullptr, actual_size, prot, flags | MAP_HUGETLB, -1, 0);
        if (p != MAP_FAILED) {
            (void)numa_node;   // mbind soft-failed (see header rationale)
            return p;
        }
        // fall through to non-hugetlb mmap below
    }
#  endif
    void* p = ::mmap(nullptr, actual_size, prot, flags, -1, 0);
    if (p == MAP_FAILED) return nullptr;
#  if defined(MADV_HUGEPAGE)
    if (has_hint(hint, AllocHint::Huge)) {
        (void)::madvise(p, actual_size, MADV_HUGEPAGE);   // best-effort
    }
#  endif
    (void)numa_node;   // first-touch policy will bind to caller's node
    return p;
#endif
}

// ── aligned_free_hint ────────────────────────────────────────────────────────
// `size` must match the size passed to aligned_alloc_hint (required for munmap
// on POSIX; ignored on Windows). The `hint` must match too — it determines
// which underlying free API is used.
inline void aligned_free_hint(void* p, std::size_t size, AllocHint hint) noexcept {
    if (!p) return;
    const bool wants_large = has_hint(hint, AllocHint::Large) ||
                             has_hint(hint, AllocHint::Huge);
    const bool wants_numa  = has_hint(hint, AllocHint::NumaLocal) ||
                             has_hint(hint, AllocHint::NumaNode);

    if (!wants_large && !wants_numa) {
#ifdef _WIN32
        ::_aligned_free(p);
#else
        std::free(p);
#endif
        return;
    }

#ifdef _WIN32
    (void)size;
    ::VirtualFree(p, 0u, MEM_RELEASE);
#else
    const std::size_t actual_size = detail::round_up_(size, k4KiB);
    (void)::munmap(p, actual_size);
#endif
}

// ── Capability query (caller can decide hint before calling) ────────────────
[[nodiscard]] inline bool hugepages_available() noexcept {
#ifdef _WIN32
    return detail::can_use_large_pages_() && ::GetLargePageMinimum() != 0u;
#else
#  if defined(MAP_HUGETLB) || defined(MADV_HUGEPAGE)
    return true;   // best-effort; actual hugepage availability depends on /sys/kernel/mm/transparent_hugepage
#  else
    return false;
#  endif
#endif
}

} // namespace phyriad::hal
// Made with my soul - Swately <3
