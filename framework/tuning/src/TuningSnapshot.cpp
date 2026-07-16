// framework/tuning/src/TuningSnapshot.cpp
// TuningSnapshot — persistent rollback record implementation.
//
#include <phyriad/tuning/TuningSnapshot.hpp>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <utility>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#  include <powrprof.h>
// MinGW-w64: timeBeginPeriod / timeEndPeriod / TIMECAPS live in <mmsystem.h>.
// MSVC SDK: declared via <timeapi.h>. The __GNUC__ guard handles both cases.
#  ifdef __GNUC__
#    include <mmsystem.h>
#  else
#    include <timeapi.h>
#  endif
// MSVC: link powrprof + winmm via #pragma comment.
// MinGW/Clang: linking is handled by CMake target_link_libraries (powrprof, winmm).
#  ifdef _MSC_VER
#    pragma comment(lib, "powrprof.lib")
#    pragma comment(lib, "winmm.lib")
#  endif
#else
#  include <cerrno>
#  include <fcntl.h>
#  include <unistd.h>
#endif

namespace phyriad::tuning {

// ── default_snapshot_path ────────────────────────────────────────────────────
std::filesystem::path default_snapshot_path() noexcept {
#ifdef _WIN32
    char buf[256]{};
    ::GetEnvironmentVariableA("ProgramData", buf, static_cast<DWORD>(sizeof(buf)));
    return std::filesystem::path{buf} / "Phyriad" / "tuning_snapshot.bin";
#else
    return "/var/lib/phyriad/tuning_snapshot.bin";
#endif
}

// ── Binary file header (64 bytes) ─────────────────────────────────────────────
struct SnapshotFileHeader_ {
    uint32_t magic{0x54554E47u};   // "TUNG"
    uint32_t version{1u};
    uint32_t record_count{0u};
    uint8_t  reserved[52]{};
};
static_assert(sizeof(SnapshotFileHeader_) == 64);

// ── Platform-specific per-record rollback ─────────────────────────────────────

#ifdef _WIN32

static void rollback_record_(TuningRecord const& r) noexcept {
    switch (r.op) {

    case TuningOp::WinPowercfg: {
        // old_value holds the GUID string of the previous power scheme.
        char cmd[320]{};
        std::snprintf(cmd, sizeof(cmd), "powercfg /setactive %s", r.old_value);
        ::WinExec(cmd, SW_HIDE);
        break;
    }

    case TuningOp::WinTimer: {
        // old_value == "N" where N is the period in milliseconds.
        const unsigned period = static_cast<unsigned>(std::atoi(r.old_value));
        if (period > 0) ::timeEndPeriod(period);
        break;
    }

    case TuningOp::WinWorkSet: {
        // old_value == "min:max" (decimal byte sizes).
        std::size_t wmin = 0, wmax = 0;
        std::sscanf(r.old_value, "%zu:%zu", &wmin, &wmax);
        if (wmin > 0 || wmax > 0)
            ::SetProcessWorkingSetSize(::GetCurrentProcess(), wmin, wmax);
        break;
    }

    case TuningOp::WinPrioBoost: {
        // old_value == "1" if boost was enabled, "0" if it was already disabled.
        const BOOL state = (r.old_value[0] == '1') ? TRUE : FALSE;
        // SetProcessPriorityBoost param is bDisablePriorityBoost — invert the recorded state.
        ::SetProcessPriorityBoost(::GetCurrentProcess(), !state);
        break;
    }

    case TuningOp::WinRegistry: {
        // target == "HKLM\\path\\to\\key:ValueName"
        const char* colon = std::strrchr(r.target, ':');
        if (!colon) break;
        const char* value_name = colon + 1;
        std::string key_path(r.target, static_cast<std::size_t>(colon - r.target));
        HKEY root = HKEY_LOCAL_MACHINE;
        const char* sub = key_path.c_str();
        if (std::strncmp(sub, "HKLM\\", 5) == 0) sub += 5;
        HKEY hkey{};
        if (::RegOpenKeyExA(root, sub, 0, KEY_SET_VALUE, &hkey) == ERROR_SUCCESS) {
            const DWORD size = static_cast<DWORD>(std::strlen(r.old_value) + 1u);
            ::RegSetValueExA(hkey, value_name, 0, REG_SZ,
                reinterpret_cast<BYTE const*>(r.old_value), size);
            ::RegCloseKey(hkey);
        }
        break;
    }

    default: break;
    }
}

#else // Linux ─────────────────────────────────────────────────────────────────

static void rollback_record_(TuningRecord const& r) noexcept {
    if (r.op != TuningOp::SysfsWrite) return;
    if (r.target[0] == '\0' || r.old_value[0] == '\0') return;

    const int fd = ::open(r.target, O_WRONLY | O_CLOEXEC);
    if (fd < 0) return;
    const std::size_t len = std::strlen(r.old_value);
    [[maybe_unused]] auto wr = ::write(fd, r.old_value, len);  // best-effort
    ::close(fd);
}

#endif // _WIN32

// ── TuningSnapshot ─────────────────────────────────────────────────────────────

TuningSnapshot::TuningSnapshot(std::filesystem::path path) noexcept
    : path_(std::move(path))
{}

TuningSnapshot::~TuningSnapshot() noexcept {
    if (!hal::ctrl_load_acquire(committed_))
        rollback_all();
}

TuningSnapshot::TuningSnapshot(TuningSnapshot&& o) noexcept
    : records_(std::move(o.records_))
    , path_(std::move(o.path_))
{
    const bool was_committed = hal::ctrl_load_acquire(o.committed_);
    hal::ctrl_store_release(committed_, was_committed);
    // Source is now empty — mark it committed so its destructor is a no-op.
    hal::ctrl_store_release(o.committed_, true);
}

TuningSnapshot& TuningSnapshot::operator=(TuningSnapshot&& o) noexcept {
    if (this != &o) {
        // Roll back our own records if not committed before overwriting.
        if (!hal::ctrl_load_acquire(committed_))
            rollback_all();

        records_ = std::move(o.records_);
        path_    = std::move(o.path_);
        hal::ctrl_store_release(committed_,
            hal::ctrl_load_acquire(o.committed_));
        hal::ctrl_store_release(o.committed_, true);
    }
    return *this;
}

void TuningSnapshot::add_record(TuningOp         op,
                                std::string_view target,
                                std::string_view old_value,
                                std::string_view new_value) noexcept
{
    TuningRecord r{};
    r.op = op;
    const auto tlen = std::min(target.size(),    sizeof(r.target)    - 1u);
    const auto olen = std::min(old_value.size(), sizeof(r.old_value) - 1u);
    const auto nlen = std::min(new_value.size(), sizeof(r.new_value) - 1u);
    std::memcpy(r.target,    target.data(),    tlen);
    std::memcpy(r.old_value, old_value.data(), olen);
    std::memcpy(r.new_value, new_value.data(), nlen);
    records_.push_back(r);
}

std::expected<void, phyriad::Error> TuningSnapshot::save() const noexcept {
    if (path_.empty()) return {};

    const auto tmp  = path_.parent_path() / (path_.filename().string() + ".tmp");
    const auto pstr = path_.string();
    const auto tstr = tmp.string();

    char cpath[1024]{};
    char ctmp[1024]{};
    std::memcpy(cpath, pstr.data(), std::min(pstr.size(), sizeof(cpath) - 1u));
    std::memcpy(ctmp,  tstr.data(), std::min(tstr.size(), sizeof(ctmp)  - 1u));

    std::FILE* f = std::fopen(ctmp, "wb");
    if (!f) [[unlikely]] {
        return std::unexpected(phyriad::Error{
            .code = phyriad::ErrorCode::IoError,
            .source_node_id = 0, .timestamp_ns = 0});
    }

    SnapshotFileHeader_ hdr{};
    hdr.record_count = static_cast<uint32_t>(records_.size());
    if (std::fwrite(&hdr, sizeof(hdr), 1u, f) != 1u) {
        std::fclose(f);
        return std::unexpected(phyriad::Error{
            .code = phyriad::ErrorCode::IoError,
            .source_node_id = 0, .timestamp_ns = 0});
    }

    if (!records_.empty()) {
        if (std::fwrite(records_.data(), sizeof(TuningRecord),
                         records_.size(), f) != records_.size())
        {
            std::fclose(f);
            return std::unexpected(phyriad::Error{
                .code = phyriad::ErrorCode::IoError,
                .source_node_id = 0, .timestamp_ns = 0});
        }
    }

    std::fflush(f);
    std::fclose(f);

#ifdef _WIN32
    if (!::MoveFileExA(ctmp, cpath,
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
    {
        return std::unexpected(phyriad::Error{
            .code = phyriad::ErrorCode::IoError,
            .source_node_id = 0, .timestamp_ns = 0});
    }
#else
    if (::rename(ctmp, cpath) != 0) {
        return std::unexpected(phyriad::Error{
            .code = phyriad::ErrorCode::IoError,
            .source_node_id = 0, .timestamp_ns = 0});
    }
#endif
    return {};
}

std::expected<TuningSnapshot, phyriad::Error>
TuningSnapshot::load(std::filesystem::path path) noexcept
{
    char cpath[1024]{};
    const auto pstr = path.string();
    std::memcpy(cpath, pstr.data(), std::min(pstr.size(), sizeof(cpath) - 1u));

    std::FILE* f = std::fopen(cpath, "rb");
    if (!f) [[unlikely]] {
        return std::unexpected(phyriad::Error{
            .code = phyriad::ErrorCode::IoError,
            .source_node_id = 0, .timestamp_ns = 0});
    }

    SnapshotFileHeader_ hdr{};
    if (std::fread(&hdr, sizeof(hdr), 1u, f) != 1u
        || hdr.magic   != 0x54554E47u
        || hdr.version != 1u)
    {
        std::fclose(f);
        return std::unexpected(phyriad::Error{
            .code = phyriad::ErrorCode::SchemaMismatch,
            .source_node_id = 0, .timestamp_ns = 0});
    }

    TuningSnapshot snap{std::move(path)};
    snap.records_.resize(hdr.record_count);
    if (hdr.record_count > 0u) {
        if (std::fread(snap.records_.data(), sizeof(TuningRecord),
                        hdr.record_count, f) != hdr.record_count)
        {
            std::fclose(f);
            return std::unexpected(phyriad::Error{
                .code = phyriad::ErrorCode::IoError,
                .source_node_id = 0, .timestamp_ns = 0});
        }
    }
    std::fclose(f);

    // Loaded snapshot is NOT committed — destructor will roll back unless
    // caller explicitly calls mark_committed() or rollback_all().
    return snap;
}

void TuningSnapshot::rollback_all() noexcept {
    for (auto it = records_.rbegin(); it != records_.rend(); ++it)
        rollback_record_(*it);
    // Prevent double-rollback if destructor fires after explicit call.
    hal::ctrl_store_release(committed_, true);
}

void TuningSnapshot::mark_committed() noexcept {
    hal::ctrl_store_release(committed_, true);
}

bool TuningSnapshot::is_committed() const noexcept {
    return hal::ctrl_load_acquire(committed_);
}

std::vector<TuningRecord> const& TuningSnapshot::records() const noexcept {
    return records_;
}

std::filesystem::path const& TuningSnapshot::path() const noexcept {
    return path_;
}

} // namespace phyriad::tuning
// Made with my soul - Swately <3
