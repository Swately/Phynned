// framework/ipc/include/phyriad/ipc/ShmRegion.hpp
// Typed shared-memory region with seqlock-based producer/consumer coordination.
//
// Provides:
//   ShmHeaderConcept<T>       — compile-time contract for Header types
//   ShmRegion<Header, Payload>— RAII typed SHM mapping (Windows + POSIX)
//
// Producer pattern:
//   auto r = ShmRegion<H,P>::create("Local\\MyApp.v1", 0xAB12u);
//   if (r) {
//       auto guard = r->begin_write();  // seq: even → odd
//       r->payload() = new_data;
//   }   // guard dtor: seq: odd → even
//
// Consumer pattern:
//   auto r = ShmRegion<H,P>::open("Local\\MyApp.v1", 0xAB12u);
//   if (r) {
//       P snapshot{};
//       if (r->try_read_consistent(&snapshot)) {
//           // use snapshot
//       }
//   }
//
// ABI versioning:
//   `create()` writes magic + version into the header before making it
//   visible (magic written LAST with a release fence). `open()` verifies
//   both fields and returns SchemaMismatch / VersionMismatch on mismatch.
//
// Layout: [Header][Payload] contiguous in one mapping. Both types must be
//   standard-layout, trivially copyable, and aligned to ≥ 8 bytes.
//
// Threading:
//   Single-writer / multi-reader seqlock. Producer calls begin_write() which
//   bumps seq to odd (in-progress) and the RAII guard bumps it back to even
//   (consistent). Readers spin up to 3 times; after 3 torn reads return false.
//

#pragma once
#include <phyriad/schema/Error.hpp>
#include <atomic>
#include <concepts>
#include <cstdint>
#include <cstdio>      // std::snprintf
#include <cstring>
#include <expected>
#include <type_traits>
#include <phyriad/hal/MemoryOrder.hpp>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#else
#  include <fcntl.h>
#  include <sys/mman.h>
#  include <sys/stat.h>
#  include <unistd.h>
#endif

namespace phyriad::ipc {

// ── ShmHeaderConcept ─────────────────────────────────────────────────────────
// Every Header type must expose these four fields in a form that allows the
// ShmRegion implementation to initialize/read them.
template <typename T>
concept ShmHeaderConcept = requires(T& h) {
    { h.magic     } -> std::convertible_to<uint32_t&>;
    { h.version   } -> std::convertible_to<uint32_t&>;
    { h.seq       } -> std::convertible_to<std::atomic<uint32_t>&>;
    { h.agent_pid } -> std::convertible_to<std::atomic<uint32_t>&>;
};

// ── ShmRegion ────────────────────────────────────────────────────────────────
template <ShmHeaderConcept Header, typename Payload>
class ShmRegion {
public:
    // Compile-time layout contract.
    static_assert(std::is_standard_layout_v<Header>,
        "ShmRegion: Header must be standard-layout");
    static_assert(std::is_standard_layout_v<Payload>,
        "ShmRegion: Payload must be standard-layout");
    static_assert(std::is_trivially_copyable_v<Payload>,
        "ShmRegion: Payload must be trivially copyable");
    static_assert(alignof(Header)  >= 8u,
        "ShmRegion: Header must be at least 8-byte aligned");
    static_assert(alignof(Payload) >= 8u,
        "ShmRegion: Payload must be at least 8-byte aligned");

    // ── Factory functions ─────────────────────────────────────────────────
    /// Create (or re-create) the named SHM region as producer/owner.
    /// Writes magic + version; sets agent_pid to current PID.
    /// Magic is written LAST with a release fence — consumer gates on it.
    [[nodiscard]] static std::expected<ShmRegion, phyriad::Error>
    create(const char* name, uint32_t magic,
           uint32_t version = 1u) noexcept;

    /// Open an existing SHM region as consumer (read-only mapping on POSIX;
    /// read-write handle on Windows to allow try_read_consistent memcpy).
    /// Verifies magic and version — returns SchemaMismatch on mismatch.
    [[nodiscard]] static std::expected<ShmRegion, phyriad::Error>
    open(const char* name, uint32_t magic,
         uint32_t expected_version = 1u) noexcept;

    // ── Rule-of-five ──────────────────────────────────────────────────────
    ShmRegion() noexcept = default;
    ~ShmRegion() noexcept { close(); }

    ShmRegion(ShmRegion&& o) noexcept
        : ptr_(o.ptr_), size_(o.size_), is_owner_(o.is_owner_)
#ifdef _WIN32
          , mapping_(o.mapping_)
#else
          , fd_(o.fd_)
#endif
    {
#ifdef _WIN32
        o.mapping_ = nullptr;
#else
        std::strncpy(name_buf_, o.name_buf_, sizeof(name_buf_) - 1u);
        name_buf_[sizeof(name_buf_) - 1u] = '\0';
        o.fd_      = -1;
        o.name_buf_[0] = '\0';
#endif
        o.ptr_      = nullptr;
        o.size_     = 0u;
        o.is_owner_ = false;
    }

    ShmRegion& operator=(ShmRegion&& o) noexcept {
        if (this != &o) {
            close();
            ptr_      = o.ptr_;
            size_     = o.size_;
            is_owner_ = o.is_owner_;
#ifdef _WIN32
            mapping_   = o.mapping_;
            o.mapping_ = nullptr;
#else
            fd_        = o.fd_;
            std::strncpy(name_buf_, o.name_buf_, sizeof(name_buf_) - 1u);
            name_buf_[sizeof(name_buf_) - 1u] = '\0';
            o.fd_      = -1;
            o.name_buf_[0] = '\0';
#endif
            o.ptr_      = nullptr;
            o.size_     = 0u;
            o.is_owner_ = false;
        }
        return *this;
    }

    ShmRegion(const ShmRegion&)            = delete;
    ShmRegion& operator=(const ShmRegion&) = delete;

    // ── State ─────────────────────────────────────────────────────────────
    void close() noexcept;
    [[nodiscard]] bool is_open()  const noexcept { return ptr_ != nullptr; }
    [[nodiscard]] bool is_owner() const noexcept { return is_owner_; }

    // ── Producer-side (mutable access) ───────────────────────────────────
    [[nodiscard]] Header&  header()  noexcept {
        return *static_cast<Header*>(ptr_);
    }
    [[nodiscard]] Payload& payload() noexcept {
        return *reinterpret_cast<Payload*>(
            static_cast<uint8_t*>(ptr_) + sizeof(Header));
    }

    /// RAII write guard: bumps seq odd on construction, even on destruction.
    /// Must be kept alive for the duration of the write critical section.
    ///
    ///   { auto g = region.begin_write(); region.payload() = new_data; }
    ///
    /// Do NOT use in move-constructed copies — guard holds a raw pointer
    /// to this region's header.
    [[nodiscard]] auto begin_write() noexcept {
        struct WriteGuard {
            Header* hdr;
            explicit WriteGuard(Header* h) noexcept : hdr(h) {
                hdr->seq.fetch_add(1u, std::memory_order_acq_rel); // HAL: acq_rel fetch_add — synchronising counter (even→odd)
            }
            ~WriteGuard() noexcept {
                hdr->seq.fetch_add(1u, std::memory_order_acq_rel); // HAL: acq_rel fetch_add — synchronising counter (odd→even)
            }
            WriteGuard(const WriteGuard&)            = delete;
            WriteGuard& operator=(const WriteGuard&) = delete;
            WriteGuard(WriteGuard&&)                 = delete;
        };
        return WriteGuard{&header()};
    }

    // ── Consumer-side (const access + seqlock read helper) ───────────────
    [[nodiscard]] const Header& header() const noexcept {
        return *static_cast<const Header*>(ptr_);
    }
    [[nodiscard]] const Payload& payload() const noexcept {
        return *reinterpret_cast<const Payload*>(
            static_cast<const uint8_t*>(ptr_) + sizeof(Header));
    }

    /// Attempt a consistent seqlock read into `*out`.
    /// Retries up to 3 times. Returns true on success, false if all 3
    /// attempts were torn (caller should use previous good snapshot).
    [[nodiscard]] bool try_read_consistent(Payload* out) const noexcept {
        if (!out || !ptr_) return false;
        const Header&  hdr = header();
        const Payload& src = payload();
        for (int attempt = 0; attempt < 3; ++attempt) {
            const uint32_t s0 =
                hal::seq_load_acquire(hdr.seq);
            if (s0 & 1u) continue; // writer in progress — spin
            std::memcpy(out, &src, sizeof(Payload));
            std::atomic_thread_fence(std::memory_order_acquire);  // HAL: explicit fence — paired with seq_load_acquire / seq_store_release
            const uint32_t s1 =
                hal::seq_load_acquire(hdr.seq);
            if (s0 == s1) return true; // consistent
        }
        return false; // 3 consecutive torn reads
    }

private:
    void*    ptr_      {nullptr};
    size_t   size_     {0u};
    bool     is_owner_ {false};

#ifdef _WIN32
    HANDLE   mapping_  {nullptr};
#else
    int      fd_       {-1};
    char     name_buf_[64]{};
#endif
};

// ── Implementation: create() ─────────────────────────────────────────────────

template <ShmHeaderConcept H, typename P>
std::expected<ShmRegion<H, P>, phyriad::Error>
ShmRegion<H, P>::create(const char* name, uint32_t magic,
                        uint32_t version) noexcept
{
    if (!name) return std::unexpected(phyriad::Error{phyriad::ErrorCode::InvalidArgument});
    constexpr size_t kSize = sizeof(H) + sizeof(P);

    ShmRegion r;
    r.size_     = kSize;
    r.is_owner_ = true;

#ifdef _WIN32
    wchar_t wname[256]{};
    if (MultiByteToWideChar(CP_UTF8, 0, name, -1, wname, 256) == 0)
        return std::unexpected(phyriad::Error{phyriad::ErrorCode::InvalidArgument});

    HANDLE map = CreateFileMappingW(
        INVALID_HANDLE_VALUE, nullptr,
        PAGE_READWRITE, 0u, static_cast<DWORD>(kSize), wname);
    if (!map)
        return std::unexpected(phyriad::Error{phyriad::ErrorCode::ShmOpenFailed});

    void* p = MapViewOfFile(map, FILE_MAP_ALL_ACCESS, 0u, 0u, kSize);
    if (!p) { CloseHandle(map);
              return std::unexpected(phyriad::Error{phyriad::ErrorCode::ShmOpenFailed}); }

    std::memset(p, 0, kSize);
    r.ptr_      = p;
    r.mapping_  = map;

#else // POSIX ────────────────────────────────────────────────────────────────
    char canonical[64]{};
    if (name[0] == '/') {
        std::strncpy(canonical, name, sizeof(canonical) - 1u);
    } else {
        std::snprintf(canonical, sizeof(canonical), "/%s", name);
    }
    canonical[sizeof(canonical) - 1u] = '\0';

    const int fd = shm_open(canonical, O_CREAT | O_RDWR | O_TRUNC, 0600);
    if (fd < 0)
        return std::unexpected(phyriad::Error{phyriad::ErrorCode::ShmOpenFailed});

    if (ftruncate(fd, static_cast<off_t>(kSize)) != 0) {
        ::close(fd); shm_unlink(canonical);
        return std::unexpected(phyriad::Error{phyriad::ErrorCode::ShmOpenFailed});
    }

    void* p = mmap(nullptr, kSize, PROT_READ | PROT_WRITE,
                   MAP_SHARED, fd, 0);
    if (p == MAP_FAILED) {
        ::close(fd); shm_unlink(canonical);
        return std::unexpected(phyriad::Error{phyriad::ErrorCode::ShmOpenFailed});
    }

    std::memset(p, 0, kSize);
    r.ptr_ = p;
    r.fd_  = fd;
    std::strncpy(r.name_buf_, canonical, sizeof(r.name_buf_) - 1u);
    r.name_buf_[sizeof(r.name_buf_) - 1u] = '\0';
#endif

    // Initialise header fields — magic LAST with release fence so consumers
    // waiting for magic != 0 see a fully initialised header.
    H* hdr     = static_cast<H*>(r.ptr_);
    hdr->version = version;
#ifdef _WIN32
    hal::seq_store_release(hdr->agent_pid, static_cast<uint32_t>(GetCurrentProcessId()));
#else
    hal::seq_store_release(hdr->agent_pid, static_cast<uint32_t>(getpid()));
#endif
    std::atomic_thread_fence(std::memory_order_release);  // HAL: explicit fence — paired with seq_load_acquire / seq_store_release
    hdr->magic = magic; // visible to consumers AFTER version + agent_pid

    return r;
}

// ── Implementation: open() ───────────────────────────────────────────────────

template <ShmHeaderConcept H, typename P>
std::expected<ShmRegion<H, P>, phyriad::Error>
ShmRegion<H, P>::open(const char* name, uint32_t magic,
                      uint32_t expected_version) noexcept
{
    if (!name) return std::unexpected(phyriad::Error{phyriad::ErrorCode::InvalidArgument});
    constexpr size_t kSize = sizeof(H) + sizeof(P);

    ShmRegion r;
    r.size_     = kSize;
    r.is_owner_ = false;

#ifdef _WIN32
    wchar_t wname[256]{};
    if (MultiByteToWideChar(CP_UTF8, 0, name, -1, wname, 256) == 0)
        return std::unexpected(phyriad::Error{phyriad::ErrorCode::InvalidArgument});

    HANDLE map = OpenFileMappingW(FILE_MAP_READ | FILE_MAP_WRITE,
                                  FALSE, wname);
    if (!map)
        return std::unexpected(phyriad::Error{phyriad::ErrorCode::ShmOpenFailed});

    void* p = MapViewOfFile(map, FILE_MAP_READ | FILE_MAP_WRITE,
                            0u, 0u, kSize);
    if (!p) { CloseHandle(map);
              return std::unexpected(phyriad::Error{phyriad::ErrorCode::ShmOpenFailed}); }

    r.ptr_     = p;
    r.mapping_ = map;

#else // POSIX ────────────────────────────────────────────────────────────────
    char canonical[64]{};
    if (name[0] == '/') {
        std::strncpy(canonical, name, sizeof(canonical) - 1u);
    } else {
        std::snprintf(canonical, sizeof(canonical), "/%s", name);
    }
    canonical[sizeof(canonical) - 1u] = '\0';

    const int fd = shm_open(canonical, O_RDWR, 0);
    if (fd < 0)
        return std::unexpected(phyriad::Error{phyriad::ErrorCode::ShmOpenFailed});

    void* p = mmap(nullptr, kSize, PROT_READ | PROT_WRITE,
                   MAP_SHARED, fd, 0);
    if (p == MAP_FAILED) {
        ::close(fd);
        return std::unexpected(phyriad::Error{phyriad::ErrorCode::ShmOpenFailed});
    }

    r.ptr_ = p;
    r.fd_  = fd;
    std::strncpy(r.name_buf_, canonical, sizeof(r.name_buf_) - 1u);
    r.name_buf_[sizeof(r.name_buf_) - 1u] = '\0';
#endif

    // Validate magic and version.
    const H* hdr = static_cast<const H*>(r.ptr_);
    std::atomic_thread_fence(std::memory_order_acquire);  // HAL: explicit fence — paired with seq_load_acquire / seq_store_release

    if (hdr->magic != magic) {
        r.close();
        return std::unexpected(phyriad::Error{phyriad::ErrorCode::SchemaMismatch});
    }
    if (hdr->version != expected_version) {
        r.close();
        return std::unexpected(phyriad::Error{phyriad::ErrorCode::SchemaMismatch});
    }

    return r;
}

// ── Implementation: close() ─────────────────────────────────────────────────

template <ShmHeaderConcept H, typename P>
void ShmRegion<H, P>::close() noexcept {
    if (!ptr_) return;

#ifdef _WIN32
    UnmapViewOfFile(ptr_);
    if (mapping_) { CloseHandle(mapping_); mapping_ = nullptr; }
#else
    munmap(ptr_, size_);
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
    // Owner unlinks the POSIX SHM object on close.
    if (is_owner_ && name_buf_[0] != '\0') {
        shm_unlink(name_buf_);
        name_buf_[0] = '\0';
    }
#endif
    ptr_      = nullptr;
    size_     = 0u;
    is_owner_ = false;
}

} // namespace phyriad::ipc
// Made with my soul - Swately <3
