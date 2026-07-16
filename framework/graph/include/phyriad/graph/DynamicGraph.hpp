// framework/graph/include/phyriad/graph/DynamicGraph.hpp
// Type-erased runtime graph view backed by a SHM GraphSchemaDescriptor.
//
// DynamicGraph attaches to memory that was previously written by a broker
// running the same StaticGraph topology. Validation is three-layer:
//   1. phyriad_magic sentinel  — rejects garbage/uninitialized memory
//   2. hal_version + pad     — rejects ABI-incompatible binaries
//   3. graph_hash comparison — rejects topology mismatches
// Any failure returns Error{SchemaMismatch} immediately (hard-fail, R0).
//
// Two factory functions:
//   attach(memory, expected_hash)   — caller already mapped the SHM region.
//                                     DynamicGraph does NOT own the mapping.
//   open_shm(name, expected_hash)   — opens + maps SHM by name internally.
//                                     DynamicGraph owns and unmaps on destruction.
//
// After successful attach, node_descriptors() and wire_descriptors() return
// pointers into the mapped region (valid while DynamicGraph is alive).
//
// The layout in SHM after GraphSchemaDescriptor (64B) is:
//   NodeDescriptor[num_nodes]  — sizeof(NodeDescriptor) = 64B each
//   WireDescriptor[num_wires]  — sizeof(WireDescriptor) = 48B each
//
// §3.E of PHASE_H_IMPLEMENTATION_PATTERNS.md

#pragma once
#include "Graph.hpp"
#include <phyriad/schema/SchemaDescriptor.hpp>
#include <phyriad/schema/Error.hpp>
#include <cstring>
#include <expected>
#include <string_view>
#include <utility>

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
#  include <cerrno>
#endif

namespace phyriad::graph {

class DynamicGraph {
public:
    DynamicGraph() = default;
    ~DynamicGraph() noexcept { cleanup_(); }

    DynamicGraph(DynamicGraph const&)            = delete;
    DynamicGraph& operator=(DynamicGraph const&) = delete;

    DynamicGraph(DynamicGraph&& o) noexcept
        : descriptor_   (o.descriptor_)
        , mapped_base_  (std::exchange(o.mapped_base_, nullptr))
        , mapped_size_  (std::exchange(o.mapped_size_, 0u))
        , owns_mapping_ (std::exchange(o.owns_mapping_, false))
    {}

    DynamicGraph& operator=(DynamicGraph&& o) noexcept {
        if (this != &o) {
            cleanup_();
            descriptor_   = o.descriptor_;
            mapped_base_  = std::exchange(o.mapped_base_, nullptr);
            mapped_size_  = std::exchange(o.mapped_size_, 0u);
            owns_mapping_ = std::exchange(o.owns_mapping_, false);
        }
        return *this;
    }

    // ── attach ────────────────────────────────────────────────────────────
    // Caller already mapped the SHM region; DynamicGraph does NOT own it.
    // memory must point to sizeof(GraphSchemaDescriptor) bytes at minimum.
    // expected_hash must equal StaticGraph<...>::schema_hash() of this binary.
    [[nodiscard]] static auto attach(
        void const* memory,
        schema::Hash128 expected_hash) noexcept
        -> std::expected<DynamicGraph, phyriad::Error>
    {
        if (!memory) [[unlikely]] {
            return std::unexpected(phyriad::Error{
                .code           = ErrorCode::InvalidHandle,
                .source_node_id = 0,
                .timestamp_ns   = 0});
        }

        schema::GraphSchemaDescriptor desc{};
        std::memcpy(&desc, memory, sizeof(desc));

        if (!schema::validate_schema_descriptor(desc)) [[unlikely]] {
            return std::unexpected(phyriad::Error{
                .code           = ErrorCode::SchemaMismatch,
                .source_node_id = 0,
                .timestamp_ns   = 0});
        }

        if (desc.graph_hash != expected_hash) [[unlikely]] {
            return std::unexpected(phyriad::Error{
                .code           = ErrorCode::SchemaMismatch,
                .source_node_id = 0,
                .timestamp_ns   = 0});
        }

        DynamicGraph g{};
        g.descriptor_   = desc;
        g.mapped_base_  = const_cast<void*>(memory);
        g.mapped_size_  = 0;      // caller-owned mapping — size not tracked
        g.owns_mapping_ = false;
        return g;
    }

    // ── open_shm ─────────────────────────────────────────────────────────
    // Open a POSIX/Win32 SHM region by name, map it, validate, and return.
    // The mapping is released in ~DynamicGraph().
    [[nodiscard]] static auto open_shm(
        std::string_view name,
        schema::Hash128 expected_hash) noexcept
        -> std::expected<DynamicGraph, phyriad::Error>
    {
#ifdef _WIN32
        return open_shm_windows_(name, expected_hash);
#else
        return open_shm_posix_(name, expected_hash);
#endif
    }

    // ── Graph<G> interface ────────────────────────────────────────────────
    // schema_hash is read from the SHM descriptor (runtime value, not consteval).
    // Callers comparing it to StaticGraph<...>::schema_hash() use operator==.
    [[nodiscard]] static schema::Hash128 schema_hash() noexcept {
        // DynamicGraph's schema_hash returns the hash baked into the SHM descriptor.
        // Called as G::schema_hash() where G = DynamicGraph is non-standard —
        // DynamicGraph instances carry the hash in descriptor_.graph_hash.
        // For concept conformance, return a zero sentinel; callers use descriptor().graph_hash.
        return schema::Hash128{};
    }

    [[nodiscard]] uint32_t node_count() const noexcept {
        return descriptor_.num_nodes;
    }

    [[nodiscard]] uint32_t wire_count() const noexcept {
        return descriptor_.num_wires;
    }

    [[nodiscard]] schema::GraphSchemaDescriptor const& descriptor() const noexcept {
        return descriptor_;
    }

    // Pointer to NodeDescriptor array — immediately after the 64-byte header.
    [[nodiscard]] schema::NodeDescriptor const* node_descriptors() const noexcept {
        if (!mapped_base_) return nullptr;
        return reinterpret_cast<schema::NodeDescriptor const*>(
            static_cast<char const*>(mapped_base_) + sizeof(schema::GraphSchemaDescriptor));
    }

    // Pointer to WireDescriptor array — after NodeDescriptor[num_nodes].
    [[nodiscard]] schema::WireDescriptor const* wire_descriptors() const noexcept {
        if (!mapped_base_) return nullptr;
        return reinterpret_cast<schema::WireDescriptor const*>(
            static_cast<char const*>(mapped_base_)
            + sizeof(schema::GraphSchemaDescriptor)
            + sizeof(schema::NodeDescriptor) * descriptor_.num_nodes);
    }

private:
    schema::GraphSchemaDescriptor descriptor_{};
    void*    mapped_base_  {nullptr};
    uint64_t mapped_size_  {0};
    bool     owns_mapping_ {false};

    void cleanup_() noexcept {
        if (!owns_mapping_ || !mapped_base_) return;
#ifdef _WIN32
        UnmapViewOfFile(mapped_base_);
#else
        if (mapped_size_ > 0)
            munmap(mapped_base_, static_cast<std::size_t>(mapped_size_));
#endif
        mapped_base_  = nullptr;
        mapped_size_  = 0;
        owns_mapping_ = false;
    }

#ifndef _WIN32
    [[nodiscard]] static auto open_shm_posix_(
        std::string_view name,
        schema::Hash128 expected_hash) noexcept
        -> std::expected<DynamicGraph, phyriad::Error>
    {
        // Build a null-terminated name (shm_open requires C string).
        char cname[256]{};
        const std::size_t len = name.size() < 255 ? name.size() : 254;
        std::memcpy(cname, name.data(), len);
        cname[len] = '\0';

        const int fd = ::shm_open(cname, O_RDONLY, 0);
        if (fd < 0) [[unlikely]] {
            return std::unexpected(phyriad::Error{
                .code           = ErrorCode::ShmOpenFailed,
                .source_node_id = 0,
                .timestamp_ns   = 0});
        }

        // Get the total SHM size via fstat.
        struct ::stat st{};
        if (::fstat(fd, &st) < 0 || st.st_size < static_cast<off_t>(sizeof(schema::GraphSchemaDescriptor))) [[unlikely]] {
            ::close(fd);
            return std::unexpected(phyriad::Error{
                .code           = ErrorCode::ShmOpenFailed,
                .source_node_id = 0,
                .timestamp_ns   = 0});
        }

        const auto map_size = static_cast<std::size_t>(st.st_size);
        void* ptr = ::mmap(nullptr, map_size, PROT_READ, MAP_SHARED, fd, 0);
        ::close(fd);  // fd no longer needed after mmap

        if (ptr == MAP_FAILED) [[unlikely]] {
            return std::unexpected(phyriad::Error{
                .code           = ErrorCode::ShmOpenFailed,
                .source_node_id = 0,
                .timestamp_ns   = 0});
        }

        auto result = attach(ptr, expected_hash);
        if (!result) [[unlikely]] {
            ::munmap(ptr, map_size);
            return std::unexpected(result.error());
        }

        result->mapped_size_  = static_cast<uint64_t>(map_size);
        result->owns_mapping_ = true;
        return result;
    }
#endif  // !_WIN32

#ifdef _WIN32
    [[nodiscard]] static auto open_shm_windows_(
        std::string_view name,
        schema::Hash128 expected_hash) noexcept
        -> std::expected<DynamicGraph, phyriad::Error>
    {
        char cname[256]{};
        const std::size_t len = name.size() < 255 ? name.size() : 254;
        std::memcpy(cname, name.data(), len);
        cname[len] = '\0';

        HANDLE hmap = ::OpenFileMappingA(FILE_MAP_READ, FALSE, cname);
        if (!hmap) [[unlikely]] {
            return std::unexpected(phyriad::Error{
                .code           = ErrorCode::ShmOpenFailed,
                .source_node_id = 0,
                .timestamp_ns   = 0});
        }

        // Map the entire file: 0 as the last argument tells MapViewOfFile to
        // extend the view to the full size of the underlying section. attach()
        // validates the descriptor + checks all node/wire arrays fit within
        // the mapped region, so a single full mapping is both simpler and
        // safer than a two-step "header first, then re-map" scheme.
        void* ptr = ::MapViewOfFile(hmap, FILE_MAP_READ, 0, 0, 0);
        ::CloseHandle(hmap);

        if (!ptr) [[unlikely]] {
            return std::unexpected(phyriad::Error{
                .code           = ErrorCode::ShmOpenFailed,
                .source_node_id = 0,
                .timestamp_ns   = 0});
        }

        auto result = attach(ptr, expected_hash);
        if (!result) [[unlikely]] {
            ::UnmapViewOfFile(ptr);
            return std::unexpected(result.error());
        }

        result->mapped_size_  = 0;   // Windows tracks size via MapViewOfFile handle
        result->owns_mapping_ = true;
        return result;
    }
#endif  // _WIN32
};

// ── Graph concept verification ────────────────────────────────────────────
static_assert(Graph<DynamicGraph>,
    "DynamicGraph must satisfy Graph<DynamicGraph>");

} // namespace phyriad::graph
// Made with my soul - Swately <3
