// action/src/RevertJournal.cpp
// RevertJournal — implementation of the crash-durable write-ahead revert journal.
//
// On-disk model: a flat file of fixed-size 112-byte DiskRecord slots. A slot is
// appended (PENDING) by record_pending; its 4-byte status field is later flipped
// in place by mark_applied / mark_reverted. Fixed-size slots make the in-place
// flip a single aligned seek+write, and a magic guard lets recover() stop cleanly
// at a torn tail record left by a crash. Durability = fflush (CRT -> OS) followed
// by FlushFileBuffers (OS cache -> platter) on every state-changing write.
//
#include <phynned/action/RevertJournal.hpp>

#include <cctype>
#include <cstddef>
#include <cstring>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#  include <io.h>       // _fileno, _get_osfhandle
#  include <share.h>    // _SH_DENYNO (shared open so a dead-man can read live)
#else
#  include <cstdlib>    // getenv
#  include <ctime>
#  include <unistd.h>   // fsync, fileno
#  include <sys/stat.h> // mkdir
#endif

namespace phynned::action {

// ── On-disk record (internal) ────────────────────────────────────────────────
namespace {

constexpr uint32_t kMagic = 0x314A5250u; // "PRJ1" — guards against garbage/tears

#pragma pack(push, 8)
struct DiskRecord {
    uint32_t magic;         //  0 : kMagic
    uint32_t status;        //  4 : RevertStatus
    uint32_t pid;           //  8 : target pid
    uint32_t _pad0;         // 12 : reserved (keeps 8B fields aligned)
    uint64_t creation_time; // 16 : FILETIME 100ns ticks since 1601 (recycle guard)
    uint64_t prev_mask;     // 24 : captured prior affinity — the revert target
    uint64_t new_mask;      // 32 : the mask Phynned applied
    uint64_t wall_time_ms;  // 40 : ms since Unix epoch when written
    char     exe_name[64];  // 48 : basename, null-terminated
};
#pragma pack(pop)
static_assert(sizeof(DiskRecord) == 112u, "DiskRecord must be 112B");
static_assert(offsetof(DiskRecord, status) == 4u, "status offset for in-place flip");

// Bounded, always-null-terminating copy (no strncpy truncation warning).
void copy_str(char* dst, std::size_t dst_sz, const char* src) noexcept {
    if (dst_sz == 0u) return;
    std::size_t n = 0u;
    if (src != nullptr)
        for (; n + 1u < dst_sz && src[n] != '\0'; ++n) dst[n] = src[n];
    dst[n] = '\0';
}

// Case-insensitive basename compare (both already basenames).
bool exe_eq(const char* a, const char* b) noexcept {
    if (a == nullptr || b == nullptr) return false;
    for (;; ++a, ++b) {
        const int ca = std::tolower(static_cast<unsigned char>(*a));
        const int cb = std::tolower(static_cast<unsigned char>(*b));
        if (ca != cb) return false;
        if (ca == 0) return true;
    }
}

#ifdef _WIN32
void ensure_dir(const std::string& dir) noexcept {
    // Create each ancestor in turn (CreateDirectory only makes one level).
    std::string acc;
    for (std::size_t i = 0u; i <= dir.size(); ++i) {
        const char c = (i < dir.size()) ? dir[i] : '\0';
        if (c == '\\' || c == '/' || c == '\0') {
            if (!acc.empty() && !(acc.size() == 2u && acc[1] == ':')) {
                CreateDirectoryA(acc.c_str(), nullptr); // ignore "already exists"
            }
            if (c != '\0') acc.push_back('\\');
        } else {
            acc.push_back(c);
        }
    }
}
#else
void ensure_dir(const std::string& dir) noexcept {
    std::string acc;
    for (std::size_t i = 0u; i <= dir.size(); ++i) {
        const char c = (i < dir.size()) ? dir[i] : '\0';
        if (c == '/' || c == '\0') {
            if (!acc.empty()) ::mkdir(acc.c_str(), 0755);
            if (c != '\0') acc.push_back('/');
        } else {
            acc.push_back(c);
        }
    }
}
#endif

// Directory portion of a path (everything before the last separator).
std::string parent_dir(const std::string& p) noexcept {
    const std::size_t s = p.find_last_of("\\/");
    return (s == std::string::npos) ? std::string() : p.substr(0u, s);
}

} // namespace

// ── to_string ─────────────────────────────────────────────────────────────────
const char* to_string(RevertStatus s) noexcept {
    switch (s) {
        case RevertStatus::Free:     return "FREE";
        case RevertStatus::Pending:  return "PENDING";
        case RevertStatus::Applied:  return "APPLIED";
        case RevertStatus::Reverted: return "REVERTED";
    }
    return "?";
}

// ── Lifecycle ─────────────────────────────────────────────────────────────────
RevertJournal::~RevertJournal() noexcept { close(); }

std::string RevertJournal::default_path() {
    std::string dir;
#ifdef _WIN32
    char buf[MAX_PATH]{};
    const DWORD n = GetEnvironmentVariableA("LOCALAPPDATA", buf, sizeof(buf));
    dir = (n > 0u && n < sizeof(buf)) ? std::string(buf) : std::string(".");
    dir += "\\Phynned\\revert_journal";
    ensure_dir(dir);
    return dir + "\\journal.bin";
#else
    const char* home = std::getenv("HOME");
    dir = (home != nullptr) ? std::string(home) : std::string(".");
    dir += "/.local/share/phynned/revert_journal";
    ensure_dir(dir);
    return dir + "/journal.bin";
#endif
}

bool RevertJournal::open(const char* path) noexcept {
    if (file_ != nullptr) return true; // already open
    if (path == nullptr || path[0] == '\0') return false;
    path_ = path;

    auto try_open = [&](const char* mode) -> bool {
#ifdef _WIN32
        // _SH_DENYNO: allow other readers (the external dead-man / a debug peek)
        // to open the journal while the agent holds it — the write-ahead record
        // must be inspectable the instant it is flushed.
        file_ = _fsopen(path, mode, _SH_DENYNO);
        return file_ != nullptr;
#else
        file_ = std::fopen(path, mode);
        return file_ != nullptr;
#endif
    };

    // "r+b" = open existing read/write without truncation; "w+b" = create.
    if (!try_open("r+b")) {
        if (!try_open("w+b")) {
            // Parent dir may not exist yet — create it and retry once.
            const std::string pd = parent_dir(path_);
            if (!pd.empty()) ensure_dir(pd);
            if (!try_open("w+b")) {
                file_ = nullptr;
                std::fprintf(stderr,
                    "[RevertJournal] cannot open/create '%s'\n", path);
                return false;
            }
        }
    }
    rebuild_index();
    return true;
}

void RevertJournal::close() noexcept {
    if (file_ != nullptr) {
        std::fflush(file_);
        std::fclose(file_);
        file_ = nullptr;
    }
    index_.clear();
}

// ── Durability ────────────────────────────────────────────────────────────────
void RevertJournal::flush_durable() noexcept {
    if (file_ == nullptr) return;
    std::fflush(file_); // CRT buffer -> OS
#ifdef _WIN32
    const int fd = _fileno(file_);
    if (fd >= 0) {
        const HANDLE h = reinterpret_cast<HANDLE>(_get_osfhandle(fd));
        if (h != INVALID_HANDLE_VALUE && h != nullptr) {
            FlushFileBuffers(h); // OS cache -> platter (the write-ahead guarantee)
        }
    }
#else
    const int fd = ::fileno(file_);
    if (fd >= 0) ::fsync(fd);
#endif
}

// ── Low-level record I/O ──────────────────────────────────────────────────────
long RevertJournal::append_record(const void* rec) noexcept {
    if (file_ == nullptr) return -1;
    std::fflush(file_);
    if (std::fseek(file_, 0, SEEK_END) != 0) return -1;
    const long off = std::ftell(file_);
    if (off < 0) return -1;
    if (std::fwrite(rec, sizeof(DiskRecord), 1u, file_) != 1u) return -1;
    return off;
}

void RevertJournal::rebuild_index() noexcept {
    index_.clear();
    if (file_ == nullptr) return;
    std::fflush(file_);
    if (std::fseek(file_, 0, SEEK_SET) != 0) return;
    DiskRecord dr{};
    long off = 0;
    while (std::fread(&dr, sizeof(dr), 1u, file_) == 1u) {
        if (dr.magic != kMagic) break; // torn tail — stop cleanly
        // Keep the latest offset per key (later slots supersede earlier ones).
        bool found = false;
        for (auto& e : index_) {
            if (e.pid == dr.pid && e.creation_time == dr.creation_time) {
                e.offset = off;
                found = true;
                break;
            }
        }
        if (!found) index_.push_back(IndexEntry{dr.pid, dr.creation_time, off});
        off += static_cast<long>(sizeof(DiskRecord));
    }
}

long RevertJournal::find_offset(const RevertKey& key) noexcept {
    for (const auto& e : index_) {
        if (e.pid == key.pid && e.creation_time == key.creation_time)
            return e.offset;
    }
    // Not indexed (e.g. a fresh instance that skipped rebuild) — scan the file
    // and take the LAST matching slot.
    if (file_ == nullptr) return -1;
    std::fflush(file_);
    if (std::fseek(file_, 0, SEEK_SET) != 0) return -1;
    DiskRecord dr{};
    long off = 0, hit = -1;
    while (std::fread(&dr, sizeof(dr), 1u, file_) == 1u) {
        if (dr.magic != kMagic) break;
        if (dr.pid == key.pid && dr.creation_time == key.creation_time) hit = off;
        off += static_cast<long>(sizeof(DiskRecord));
    }
    return hit;
}

// ── Write-ahead API ───────────────────────────────────────────────────────────
void RevertJournal::record_pending(uint32_t    pid,
                                   const char* exe_name,
                                   uint64_t    creation_time,
                                   uint64_t    prev_mask,
                                   uint64_t    new_mask) noexcept {
    if (file_ == nullptr) return;
    DiskRecord dr{};
    dr.magic         = kMagic;
    dr.status        = static_cast<uint32_t>(RevertStatus::Pending);
    dr.pid           = pid;
    dr.creation_time = creation_time;
    dr.prev_mask     = prev_mask;
    dr.new_mask      = new_mask;
    dr.wall_time_ms  = now_wall_ms();
    copy_str(dr.exe_name, sizeof(dr.exe_name), exe_name);

    const long off = append_record(&dr);
    // THE write-ahead barrier: the PENDING record is on the platter before we
    // return, so a crash before the caller's Set* cannot orphan it.
    flush_durable();
    if (off < 0) return;

    // Record the latest offset for this key (replace any prior slot).
    for (auto& e : index_) {
        if (e.pid == pid && e.creation_time == creation_time) {
            e.offset = off;
            return;
        }
    }
    index_.push_back(IndexEntry{pid, creation_time, off});
}

void RevertJournal::set_status(const RevertKey& key, RevertStatus to) noexcept {
    if (file_ == nullptr) return;
    const long off = find_offset(key);
    if (off < 0) return; // no such record — nothing to flip
    std::fflush(file_);
    if (std::fseek(file_, off + static_cast<long>(offsetof(DiskRecord, status)),
                   SEEK_SET) != 0)
        return;
    const uint32_t v = static_cast<uint32_t>(to);
    if (std::fwrite(&v, sizeof(v), 1u, file_) != 1u) return;
    flush_durable(); // the flip is durable before we proceed
}

void RevertJournal::mark_applied(const RevertKey& key) noexcept {
    set_status(key, RevertStatus::Applied);
}

void RevertJournal::mark_reverted(const RevertKey& key) noexcept {
    set_status(key, RevertStatus::Reverted);
}

// ── Recovery ──────────────────────────────────────────────────────────────────
std::vector<RevertRecord> RevertJournal::recover() noexcept {
    std::vector<RevertRecord> out;
    if (file_ == nullptr) return out;
    std::fflush(file_);
    if (std::fseek(file_, 0, SEEK_SET) != 0) return out;

    DiskRecord dr{};
    while (std::fread(&dr, sizeof(dr), 1u, file_) == 1u) {
        if (dr.magic != kMagic) break; // torn tail — stop cleanly
        const RevertStatus st = static_cast<RevertStatus>(dr.status);
        if (st != RevertStatus::Pending && st != RevertStatus::Applied) continue;

        RevertRecord r{};
        r.pid           = dr.pid;
        r.creation_time = dr.creation_time;
        r.prev_mask     = dr.prev_mask;
        r.new_mask      = dr.new_mask;
        r.wall_time_ms  = dr.wall_time_ms;
        r.status        = st;
        copy_str(r.exe_name, sizeof(r.exe_name), dr.exe_name);

        // Dedup by key, keeping the latest on-disk state (later slot wins).
        bool replaced = false;
        for (auto& e : out) {
            if (e.pid == r.pid && e.creation_time == r.creation_time) {
                e = r;
                replaced = true;
                break;
            }
        }
        if (!replaced) out.push_back(r);
    }
    return out;
}

// ── pid-recycle guard + process queries ───────────────────────────────────────
uint64_t RevertJournal::query_creation_time(uint32_t pid) noexcept {
#ifdef _WIN32
    if (pid == 0u) return 0u;
    const HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (h == nullptr) return 0u;
    FILETIME ft_create{}, ft_exit{}, ft_kernel{}, ft_user{};
    uint64_t ct = 0u;
    if (GetProcessTimes(h, &ft_create, &ft_exit, &ft_kernel, &ft_user)) {
        ct = (static_cast<uint64_t>(ft_create.dwHighDateTime) << 32u) |
              static_cast<uint64_t>(ft_create.dwLowDateTime);
    }
    CloseHandle(h);
    return ct;
#else
    (void)pid;
    return 0u;
#endif
}

bool RevertJournal::query_exe_basename(uint32_t pid,
                                       char* out, std::size_t n) noexcept {
    if (out == nullptr || n == 0u) return false;
    out[0] = '\0';
#ifdef _WIN32
    if (pid == 0u) return false;
    const HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (h == nullptr) return false;
    char full[MAX_PATH]{};
    DWORD len = static_cast<DWORD>(sizeof(full));
    bool ok = false;
    if (QueryFullProcessImageNameA(h, 0, full, &len) && len > 0u) {
        // strip to basename
        const char* base = full;
        for (const char* p = full; *p; ++p)
            if (*p == '\\' || *p == '/') base = p + 1;
        copy_str(out, n, base);
        ok = (out[0] != '\0');
    }
    CloseHandle(h);
    return ok;
#else
    (void)pid; (void)n;
    return false;
#endif
}

bool RevertJournal::process_still_matches(const RevertRecord& r) noexcept {
    // 1) creation-time is the authoritative recycle discriminator.
    const uint64_t ct = query_creation_time(r.pid);
    if (ct == 0u) return false;              // gone / unreadable -> stale, drop
    if (ct != r.creation_time) return false; // pid recycled -> a DIFFERENT process

    // 2) exe basename as a second check (only when the record carries one).
    if (r.exe_name[0] != '\0') {
        char live[64]{};
        if (query_exe_basename(r.pid, live, sizeof(live))) {
            if (!exe_eq(live, r.exe_name)) return false;
        }
        // If the name can't be read but the creation-time matched, trust the
        // (stronger) creation-time match rather than dropping a real placement.
    }
    return true;
}

uint64_t RevertJournal::now_wall_ms() noexcept {
#ifdef _WIN32
    FILETIME ft{};
    GetSystemTimeAsFileTime(&ft);
    uint64_t t = (static_cast<uint64_t>(ft.dwHighDateTime) << 32u) | ft.dwLowDateTime;
    t /= 10000u;             // 100ns -> ms
    t -= 11644473600000ull;  // 1601 epoch -> 1970 epoch
    return t;
#else
    struct timespec ts{};
    clock_gettime(CLOCK_REALTIME, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000u +
           static_cast<uint64_t>(ts.tv_nsec) / 1'000'000u;
#endif
}

} // namespace phynned::action
// Made with my soul - Swately <3
