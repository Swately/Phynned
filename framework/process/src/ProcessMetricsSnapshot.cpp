// framework/process/src/ProcessMetricsSnapshot.cpp
// ProcessMetricsSnapshot — Windows + Linux implementation.
//
// Windows path: NtQuerySystemInformation(SystemProcessInformation).
//   - One syscall to retrieve ALL processes' CPU/memory/IO metrics.
//   - ntdll.dll loaded dynamically — no import lib required.
//   - Buffer grows on STATUS_INFO_LENGTH_MISMATCH (typically at most once).
//   - Metrics array sorted by PID for O(log N) find().
//
// Linux path: /proc/<pid>/stat parsing.
//   - Iterates /proc to discover live PIDs, reads stat + status per-PID.
//   - Slower than Windows bulk API but correct and CAP-free.
//


#include <phyriad/process/ProcessMetricsSnapshot.hpp>

#include <algorithm>
#include <cstring>
#include <cstdio>
#include <cstdlib>

// ── Platform includes ─────────────────────────────────────────────────────────
#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
// Note: We do NOT include winternl.h to avoid conflicts with its partial
// SYSTEM_PROCESS_INFORMATION definition. All NT types are declared locally.

// ── NT-private type + constants ───────────────────────────────────────────
// NTSTATUS is a LONG on all Windows platforms; define it ourselves to avoid
// depending on ntstatus.h (not always pulled in by WIN32_LEAN_AND_MEAN builds).
#  ifndef NTSTATUS
typedef LONG NTSTATUS;
#  endif

static constexpr NTSTATUS kStatusSuccess            = static_cast<NTSTATUS>(0x00000000L);
static constexpr NTSTATUS kStatusInfoLengthMismatch = static_cast<NTSTATUS>(0xC0000004L);
static constexpr ULONG    kSystemProcessInformation = 5u;

// Full SYSTEM_PROCESS_INFORMATION layout.
// The winternl.h partial definition only exposes a subset.
struct GMA_SYSTEM_PROCESS_INFORMATION {
    ULONG          NextEntryOffset;
    ULONG          NumberOfThreads;
    LARGE_INTEGER  WorkingSetPrivateSize;
    ULONG          HardFaultCount;
    ULONG          NumberOfThreadsHighWatermark;
    ULONGLONG      CycleTime;
    LARGE_INTEGER  CreateTime;
    LARGE_INTEGER  UserTime;
    LARGE_INTEGER  KernelTime;
    // UNICODE_STRING inline to avoid dependency on winternl.h / ntdef.h.
    struct { USHORT Length; USHORT MaximumLength; PWSTR Buffer; } ImageName;
    LONG           BasePriority;
    HANDLE         UniqueProcessId;
    HANDLE         InheritedFromUniqueProcessId;
    ULONG          HandleCount;
    ULONG          SessionId;
    ULONG_PTR      UniqueProcessKey;
    SIZE_T         PeakVirtualSize;
    SIZE_T         VirtualSize;
    ULONG          PageFaultCount;
    SIZE_T         PeakWorkingSetSize;
    SIZE_T         WorkingSetSize;
    SIZE_T         QuotaPeakPagedPoolUsage;
    SIZE_T         QuotaPagedPoolUsage;
    SIZE_T         QuotaPeakNonPagedPoolUsage;
    SIZE_T         QuotaNonPagedPoolUsage;
    SIZE_T         PagefileUsage;      // private bytes (committed)
    SIZE_T         PeakPagefileUsage;
    SIZE_T         PrivatePageCount;
    LARGE_INTEGER  ReadOperationCount;
    LARGE_INTEGER  WriteOperationCount;
    LARGE_INTEGER  OtherOperationCount;
    LARGE_INTEGER  ReadTransferCount;
    LARGE_INTEGER  WriteTransferCount;
    LARGE_INTEGER  OtherTransferCount;
    // Followed by variable-length SYSTEM_THREAD_INFORMATION[NumberOfThreads]
};

// SYSTEM_THREAD_INFORMATION layout (x64 / aligned to 8). Sourced from
// undocumented but stable Windows kernel struct.  GFR-Ayama-3.
struct GMA_SYSTEM_THREAD_INFORMATION {
    LARGE_INTEGER KernelTime;       //  8B @0
    LARGE_INTEGER UserTime;         //  8B @8
    LARGE_INTEGER CreateTime;       //  8B @16
    ULONG         WaitTime;         //  4B @24
    // PVOID is 8B on x64, 4B on x86. We assume x64 throughout Phyriad.
    PVOID         StartAddress;     //  8B @32 (with 4B padding before)
    struct {
        HANDLE UniqueProcess;       //  8B @40
        HANDLE UniqueThread;        //  8B @48
    } ClientId;
    LONG          Priority;         //  4B @56 (KPRIORITY, signed)
    LONG          BasePriority;     //  4B @60
    ULONG         ContextSwitches;  //  4B @64
    ULONG         ThreadState;      //  4B @68
    ULONG         WaitReason;       //  4B @72
    // 4B padding to align to 8 → struct ends at 80B on x64
};
static_assert(sizeof(GMA_SYSTEM_THREAD_INFORMATION) == 80u,
    "SYSTEM_THREAD_INFORMATION must be 80B on x64");

using NtQuerySystemInformation_t = NTSTATUS (NTAPI*)(
    ULONG  SystemInformationClass,
    PVOID  SystemInformation,
    ULONG  SystemInformationLength,
    PULONG ReturnLength);

#else
// ── Linux includes ────────────────────────────────────────────────────────────
#  include <dirent.h>
#  include <sys/types.h>
#  include <unistd.h>
#  include <cerrno>
#endif

namespace phyriad::proc {

// ── Impl struct ───────────────────────────────────────────────────────────────
struct ProcessMetricsSnapshot::Impl {
    // OS query buffer (grows on SIZE_MISMATCH, stable after 1-2 captures).
    std::unique_ptr<uint8_t[]> buf;
    uint32_t                   buf_cap{0u};

    // Sorted-by-PID array built in capture(). capacity() pre-reserved once.
    std::vector<ProcessMetrics> metrics;

    // GFR-Ayama-3: per-PID index for thread-array lookups inside `buf`.
    // Sorted in lockstep with `metrics` (same PID order). Each entry records
    // the byte offset (within `buf`) where this process's SYSTEM_THREAD_
    // INFORMATION array begins, plus its element count. extract_threads()
    // does a binary search by PID against this vector, then memcpys.
    struct ThreadIndex {
        uint32_t pid;
        uint32_t thread_count;
        uint32_t buf_offset;    // bytes into buf where threads array starts
        uint32_t _pad;          // alignment
    };
    static_assert(sizeof(ThreadIndex) == 16u, "ThreadIndex must be 16B");
    std::vector<ThreadIndex> thread_index;

#ifdef _WIN32
    NtQuerySystemInformation_t NtQSI{nullptr};
#endif
};

// ── Constructor / destructor helpers ─────────────────────────────────────────
ProcessMetricsSnapshot::~ProcessMetricsSnapshot() noexcept = default;

ProcessMetricsSnapshot::ProcessMetricsSnapshot(ProcessMetricsSnapshot&&) noexcept
    = default;

ProcessMetricsSnapshot&
ProcessMetricsSnapshot::operator=(ProcessMetricsSnapshot&&) noexcept = default;

// ── create() ─────────────────────────────────────────────────────────────────
std::expected<ProcessMetricsSnapshot, phyriad::Error>
ProcessMetricsSnapshot::create(uint32_t initial_capacity_bytes) noexcept
{
    ProcessMetricsSnapshot snap;
    snap.impl_ = std::make_unique<Impl>();
    if (!snap.impl_) {
        return std::unexpected(phyriad::Error{phyriad::ErrorCode::OutOfMemory, 0u, 0u});
    }

    // Pre-allocate query buffer.
    const uint32_t cap = (initial_capacity_bytes < 4096u) ? 65536u
                                                           : initial_capacity_bytes;
    snap.impl_->buf = std::make_unique<uint8_t[]>(cap);
    if (!snap.impl_->buf) {
        return std::unexpected(phyriad::Error{phyriad::ErrorCode::OutOfMemory, 0u, 0u});
    }
    snap.impl_->buf_cap = cap;

    // Pre-reserve metrics vector (600 typical processes on Win11 developer machine).
    try { snap.impl_->metrics.reserve(600u); }
    catch (...) {
        return std::unexpected(phyriad::Error{phyriad::ErrorCode::OutOfMemory, 0u, 0u});
    }

#ifdef _WIN32
    // Load NtQuerySystemInformation dynamically (avoids linking ntdll.lib).
    HMODULE ntdll = GetModuleHandleA("ntdll.dll");
    if (!ntdll) {
        return std::unexpected(phyriad::Error{phyriad::ErrorCode::SystemError, 0u, 0u});
    }
    // Double-cast through void* to avoid -Wcast-function-type on MinGW.
    void* fn_raw = reinterpret_cast<void*>(
        GetProcAddress(ntdll, "NtQuerySystemInformation"));
    std::memcpy(&snap.impl_->NtQSI, &fn_raw, sizeof(fn_raw));
    if (!snap.impl_->NtQSI) {
        return std::unexpected(phyriad::Error{phyriad::ErrorCode::SystemError, 0u, 0u});
    }
#endif

    return snap;
}

// ── capture() — Windows ───────────────────────────────────────────────────────
#ifdef _WIN32

std::expected<void, phyriad::Error> ProcessMetricsSnapshot::capture() noexcept
{
    auto& imp = *impl_;

    // Query loop: retry on SIZE_MISMATCH (typically at most once cold).
    ULONG returned = 0u;
    NTSTATUS s = kStatusInfoLengthMismatch;

    for (uint32_t attempt = 0u; attempt < 4u; ++attempt) {
        s = imp.NtQSI(kSystemProcessInformation,
                      imp.buf.get(),
                      static_cast<ULONG>(imp.buf_cap),
                      &returned);
        if (s == kStatusInfoLengthMismatch) {
            // Grow buffer: max(2× current, returned + 64KB).
            const uint32_t new_cap = std::max(
                imp.buf_cap * 2u,
                static_cast<uint32_t>(returned) + 65536u);
            imp.buf = std::make_unique<uint8_t[]>(new_cap);
            if (!imp.buf) {
                return std::unexpected(
                    phyriad::Error{phyriad::ErrorCode::OutOfMemory, 0u, 0u});
            }
            imp.buf_cap = new_cap;
            continue;
        }
        break;
    }

    if (s != kStatusSuccess) {
        return std::unexpected(phyriad::Error{phyriad::ErrorCode::SystemError, 0u, 0u});
    }

    // Parse the linked list of GMA_SYSTEM_PROCESS_INFORMATION entries.
    imp.metrics.clear();        // clear() preserves capacity — no realloc
    imp.thread_index.clear();   // same

    const uint8_t* p = imp.buf.get();
    const uint8_t* const end = imp.buf.get() + returned;

    for (;;) {
        if (p >= end) break;
        const auto* spi = reinterpret_cast<const GMA_SYSTEM_PROCESS_INFORMATION*>(p);

        ProcessMetrics m{};
        m.pid = static_cast<uint32_t>(
            reinterpret_cast<uintptr_t>(spi->UniqueProcessId));
        m.kernel_time_100ns = static_cast<uint64_t>(spi->KernelTime.QuadPart);
        m.user_time_100ns   = static_cast<uint64_t>(spi->UserTime.QuadPart);
        m.working_set_bytes = static_cast<uint64_t>(spi->WorkingSetSize);
        m.private_bytes     = static_cast<uint64_t>(spi->PagefileUsage);
        m.read_bytes        = static_cast<uint64_t>(spi->ReadTransferCount.QuadPart);
        m.write_bytes       = static_cast<uint64_t>(spi->WriteTransferCount.QuadPart);
        m.thread_count      = spi->NumberOfThreads;
        m.handle_count      = spi->HandleCount;

        imp.metrics.push_back(m);

        // GFR-Ayama-3: record where this process's SYSTEM_THREAD_INFORMATION
        // array starts within the buffer. Threads array immediately follows
        // the SPI struct in memory.
        Impl::ThreadIndex ti{};
        ti.pid          = m.pid;
        ti.thread_count = spi->NumberOfThreads;
        ti.buf_offset   = static_cast<uint32_t>(
            (p - imp.buf.get()) +
            sizeof(GMA_SYSTEM_PROCESS_INFORMATION));
        imp.thread_index.push_back(ti);

        if (spi->NextEntryOffset == 0u) break;
        p += spi->NextEntryOffset;
    }

    // Sort by PID ascending for O(log N) find(). thread_index uses the same
    // PID key, so sorting it independently produces a parallel ordering.
    std::sort(imp.metrics.begin(), imp.metrics.end(),
              [](const ProcessMetrics& a, const ProcessMetrics& b) noexcept {
                  return a.pid < b.pid;
              });
    std::sort(imp.thread_index.begin(), imp.thread_index.end(),
              [](const Impl::ThreadIndex& a,
                 const Impl::ThreadIndex& b) noexcept {
                  return a.pid < b.pid;
              });

    return {};
}

// ── capture() — Linux ─────────────────────────────────────────────────────────
#else

namespace {

// Parse key fields from /proc/<pid>/stat (single-line format).
// stat line format: pid (comm) state ppid pgroup session ... utime stime ...
// We skip to utime (field 14) and stime (field 15), 0-indexed from 1.
static bool parse_stat(uint32_t pid, ProcessMetrics& out) noexcept {
    char path[64]{};
    std::snprintf(path, sizeof(path), "/proc/%u/stat", pid);
    std::FILE* f = std::fopen(path, "r");
    if (!f) return false;

    char line[512]{};
    const bool got = std::fgets(line, sizeof(line), f) != nullptr;
    std::fclose(f);
    if (!got) return false;

    // Find the closing ')' of the comm field to avoid comm names with spaces.
    const char* p = std::strrchr(line, ')');
    if (!p) return false;
    p += 2; // skip ') '

    // Fields after comm: state(1) ppid(2) pgrp(3) session(4) tty(5) tpgid(6)
    //   flags(7) minflt(8) cminflt(9) majflt(10) cmajflt(11)
    //   utime(12) stime(13) cutime(14) cstime(15) ...
    // We need fields 12 and 13 (0-based after state), i.e. skip 11 tokens.
    unsigned long long utime = 0, stime = 0;
    unsigned long long dummy = 0;
    // Parse: state(str) ppid pgrp session tty tpgid flags minflt cminflt majflt cmajflt utime stime
    char state = 0;
    int matched = std::sscanf(p,
        "%c "                // state
        "%llu %llu %llu %llu %llu %llu " // ppid pgrp session tty tpgid flags
        "%llu %llu %llu %llu "           // minflt cminflt majflt cmajflt
        "%llu %llu",                      // utime stime
        &state,
        &dummy, &dummy, &dummy, &dummy, &dummy, &dummy,
        &dummy, &dummy, &dummy, &dummy,
        &utime, &stime);
    if (matched < 13) return false;

    // Convert from clock ticks to 100ns units (HZ = sysconf(_SC_CLK_TCK)).
    static const long hz = sysconf(_SC_CLK_TCK);
    const uint64_t tick_ns100 = (hz > 0) ? (10000000ull / static_cast<uint64_t>(hz))
                                          : 100000ull;  // fallback: HZ=100

    out.kernel_time_100ns = stime * tick_ns100;
    out.user_time_100ns   = utime * tick_ns100;
    return true;
}

// Parse cumulative I/O byte counters from /proc/<pid>/io. The file is only
// readable when the caller has CAP_SYS_PTRACE or owns the process — silently
// returns false otherwise (counters remain 0). Format (proc(5)):
//
//   rchar: <bytes-read-via-syscalls>
//   wchar: <bytes-written-via-syscalls>
//   read_bytes:  <actual-bytes-fetched-from-storage>
//   write_bytes: <actual-bytes-sent-to-storage>
//   ...
//
// We use `read_bytes` / `write_bytes` (physical I/O) over `rchar` / `wchar`
// (syscall byte count) because the former is what drives disk pressure.
static bool parse_io(uint32_t pid, ProcessMetrics& out) noexcept {
    char path[64]{};
    std::snprintf(path, sizeof(path), "/proc/%u/io", pid);
    std::FILE* f = std::fopen(path, "r");
    if (!f) return false;   // EACCES is normal — only self / CAP_SYS_PTRACE.

    char line[128]{};
    while (std::fgets(line, sizeof(line), f)) {
        if (std::strncmp(line, "read_bytes:", 11) == 0) {
            out.read_bytes = static_cast<uint64_t>(
                std::strtoull(line + 11, nullptr, 10));
        } else if (std::strncmp(line, "write_bytes:", 12) == 0) {
            out.write_bytes = static_cast<uint64_t>(
                std::strtoull(line + 12, nullptr, 10));
        }
    }
    std::fclose(f);
    return true;
}

// Parse a single thread's /proc/<pid>/task/<tid>/stat into a ThreadEntry.
// Same format as /proc/<pid>/stat (proc(5)). Robust against comm fields with
// embedded ')' by locating the LAST ')' in the buffer before tokenising.
//
// Returns true on success; leaves `out` partially populated on parse failure
// (caller still sees tid + pid, may see zero CPU times).
static bool parse_thread_stat(uint32_t pid, uint32_t tid,
                               ThreadEntry& out) noexcept {
    out = ThreadEntry{};
    out.tid = tid;
    out.pid = pid;

    char path[64]{};
    std::snprintf(path, sizeof(path),
                  "/proc/%u/task/%u/stat", pid, tid);
    std::FILE* f = std::fopen(path, "r");
    if (!f) return false;

    char buf[512]{};
    const size_t n = std::fread(buf, 1u, sizeof(buf) - 1u, f);
    std::fclose(f);
    if (n == 0u) return false;
    buf[n] = '\0';

    // Locate the LAST ')' (comm field can legally contain ')').
    char* p = nullptr;
    for (ssize_t i = static_cast<ssize_t>(n) - 1; i >= 0; --i) {
        if (buf[i] == ')') { p = buf + i + 1; break; }
    }
    if (!p) return false;

    // Field 3 = state (single char 'R'/'S'/'D'/'Z'/'T'/'I'/'X').
    while (*p == ' ' || *p == '\t') ++p;
    if (!*p) return false;
    out.state = static_cast<uint32_t>(static_cast<unsigned char>(*p));
    while (*p && *p != ' ' && *p != '\t') ++p;

    // Convert clock ticks → 100-ns units (matches the Windows convention).
    static const long hz = sysconf(_SC_CLK_TCK);
    const uint64_t tick_ns100 =
        (hz > 0) ? (10000000ull / static_cast<uint64_t>(hz)) : 100000ull;

    // Skip 10 unsigned fields (ppid pgrp session tty tpgid flags
    //                          minflt cminflt majflt cmajflt).
    for (int i = 0; i < 10 && *p; ++i) {
        (void)std::strtoull(p, &p, 10);
    }

    // Field 14 = utime, field 15 = stime (clock ticks).
    const uint64_t utime = std::strtoull(p, &p, 10);
    const uint64_t stime = std::strtoull(p, &p, 10);
    out.user_time_100ns   = utime * tick_ns100;
    out.kernel_time_100ns = stime * tick_ns100;

    // Skip cutime + cstime (signed, fields 16-17).
    (void)std::strtoll(p, &p, 10);
    (void)std::strtoll(p, &p, 10);

    // Field 18 = priority (signed). Linux: 0..99 RT, 100..139 = nice -20..19.
    out.priority = static_cast<int32_t>(std::strtol(p, &p, 10));

    // Skip nice (19), num_threads (20), itrealvalue (21).
    (void)std::strtol(p, &p, 10);
    (void)std::strtol(p, &p, 10);
    (void)std::strtoll(p, &p, 10);

    // Field 22 = starttime in clock ticks since boot.
    out.create_time_100ns =
        std::strtoull(p, nullptr, 10) * tick_ns100;

    // wait_reason is a Windows kernel concept — leave zero on Linux.
    out.wait_reason = 0u;
    return true;
}

// Parse VmRSS and threads from /proc/<pid>/status.
static bool parse_status(uint32_t pid, ProcessMetrics& out) noexcept {
    char path[64]{};
    std::snprintf(path, sizeof(path), "/proc/%u/status", pid);
    std::FILE* f = std::fopen(path, "r");
    if (!f) return false;

    char line[128]{};
    while (std::fgets(line, sizeof(line), f)) {
        if (std::strncmp(line, "VmRSS:", 6) == 0) {
            out.working_set_bytes =
                static_cast<uint64_t>(std::strtoul(line + 6, nullptr, 10)) * 1024ull;
        } else if (std::strncmp(line, "VmData:", 7) == 0) {
            out.private_bytes =
                static_cast<uint64_t>(std::strtoul(line + 7, nullptr, 10)) * 1024ull;
        } else if (std::strncmp(line, "Threads:", 8) == 0) {
            out.thread_count =
                static_cast<uint32_t>(std::strtoul(line + 8, nullptr, 10));
        }
    }
    std::fclose(f);
    return true;
}

} // namespace

std::expected<void, phyriad::Error> ProcessMetricsSnapshot::capture() noexcept
{
    auto& imp = *impl_;
    imp.metrics.clear();
    imp.thread_index.clear();

    DIR* proc_dir = opendir("/proc");
    if (!proc_dir) {
        return std::unexpected(phyriad::Error{phyriad::ErrorCode::SystemError, 0u, 0u});
    }

    struct dirent* ent = nullptr;
    while ((ent = readdir(proc_dir)) != nullptr) {
        // Only numeric entries are process directories.
        const char* name = ent->d_name;
        if (name[0] < '1' || name[0] > '9') continue;
        bool all_digits = true;
        for (const char* c = name; *c; ++c) {
            if (*c < '0' || *c > '9') { all_digits = false; break; }
        }
        if (!all_digits) continue;

        const uint32_t pid = static_cast<uint32_t>(std::strtoul(name, nullptr, 10));

        ProcessMetrics m{};
        m.pid = pid;
        parse_stat(pid, m);
        parse_status(pid, m);
        // I/O byte counters: best-effort. Access requires CAP_SYS_PTRACE or
        // owning the process — failure leaves m.read_bytes / write_bytes = 0
        // which is the documented "unavailable" sentinel.
        (void)parse_io(pid, m);

        try { imp.metrics.push_back(m); }
        catch (...) { closedir(proc_dir);
                      return std::unexpected(phyriad::Error{
                          phyriad::ErrorCode::OutOfMemory, 0u, 0u}); }
    }
    closedir(proc_dir);

    std::sort(imp.metrics.begin(), imp.metrics.end(),
              [](const ProcessMetrics& a, const ProcessMetrics& b) noexcept {
                  return a.pid < b.pid;
              });

    // ── Linux thread enumeration ─────────────────────────────────────────
    // Walk /proc/<pid>/task/* for each process and write ThreadEntry structs
    // into imp.buf, recording each PID's range in imp.thread_index. Layout
    // matches the Windows path (binary-search by PID → range in buf), so
    // extract_threads() and thread_count_for() work uniformly across
    // platforms.
    //
    // Memory: ThreadEntry is 48 bytes, so the default 64 KB buf holds
    // ~1365 threads. We grow geometrically if a process is unusually large
    // (e.g. chrome with hundreds of threads).
    uint32_t buf_off = 0u;
    auto grow_buf_for = [&](uint32_t need_bytes) noexcept -> bool {
        while (buf_off + need_bytes > imp.buf_cap) {
            const uint32_t new_cap = (imp.buf_cap < 1024u)
                ? 4096u : imp.buf_cap * 2u;
            auto new_buf = std::make_unique<uint8_t[]>(new_cap);
            if (!new_buf) return false;
            std::memcpy(new_buf.get(), imp.buf.get(), buf_off);
            imp.buf     = std::move(new_buf);
            imp.buf_cap = new_cap;
        }
        return true;
    };

    for (const auto& proc_m : imp.metrics) {
        char task_dir[48]{};
        std::snprintf(task_dir, sizeof(task_dir),
                      "/proc/%u/task", proc_m.pid);
        DIR* td = opendir(task_dir);
        if (!td) continue;   // process exited between metrics scan & here

        const uint32_t range_start = buf_off;
        uint32_t       n_threads   = 0u;

        struct dirent* tent;
        while ((tent = readdir(td)) != nullptr) {
            const char* tname = tent->d_name;
            if (tname[0] < '1' || tname[0] > '9') continue;
            bool ok = true;
            for (const char* c = tname; *c; ++c) {
                if (*c < '0' || *c > '9') { ok = false; break; }
            }
            if (!ok) continue;
            const uint32_t tid =
                static_cast<uint32_t>(std::strtoul(tname, nullptr, 10));

            if (!grow_buf_for(sizeof(ThreadEntry))) {
                closedir(td);
                return std::unexpected(phyriad::Error{
                    phyriad::ErrorCode::OutOfMemory, 0u, 0u});
            }

            auto* dst = reinterpret_cast<ThreadEntry*>(
                imp.buf.get() + buf_off);
            (void)parse_thread_stat(proc_m.pid, tid, *dst);
            buf_off += static_cast<uint32_t>(sizeof(ThreadEntry));
            ++n_threads;
        }
        closedir(td);

        if (n_threads > 0u) {
            Impl::ThreadIndex idx{};
            idx.pid          = proc_m.pid;
            idx.thread_count = n_threads;
            idx.buf_offset   = range_start;
            idx._pad         = 0u;
            try { imp.thread_index.push_back(idx); }
            catch (...) {
                return std::unexpected(phyriad::Error{
                    phyriad::ErrorCode::OutOfMemory, 0u, 0u});
            }
        }
    }

    return {};
}

#endif // !_WIN32

// ── find() ───────────────────────────────────────────────────────────────────
const ProcessMetrics* ProcessMetricsSnapshot::find(uint32_t pid) const noexcept
{
    const auto& v = impl_->metrics;
    if (v.empty()) return nullptr;

    // Binary search by PID.
    uint32_t lo = 0u;
    uint32_t hi = static_cast<uint32_t>(v.size());
    while (lo < hi) {
        const uint32_t mid = lo + (hi - lo) / 2u;
        if (v[mid].pid == pid) return &v[mid];
        if (v[mid].pid < pid) lo = mid + 1u;
        else                  hi = mid;
    }
    return nullptr;
}

// ── extract() ────────────────────────────────────────────────────────────────
uint32_t ProcessMetricsSnapshot::extract(const uint32_t* pids, uint32_t n,
                                          ProcessMetrics* out) const noexcept
{
    if (!pids || !out || n == 0u) return 0u;

    uint32_t found = 0u;
    for (uint32_t i = 0u; i < n; ++i) {
        const ProcessMetrics* m = find(pids[i]);
        if (m) {
            out[i] = *m;
            ++found;
        } else {
            // Not found: zero out, but set PID so caller can match by index.
            std::memset(&out[i], 0, sizeof(ProcessMetrics));
            out[i].pid = pids[i];
        }
    }
    return found;
}

// ── size helpers ──────────────────────────────────────────────────────────────
uint32_t ProcessMetricsSnapshot::process_count() const noexcept {
    return impl_ ? static_cast<uint32_t>(impl_->metrics.size()) : 0u;
}

const ProcessMetrics* ProcessMetricsSnapshot::data() const noexcept {
    return (impl_ && !impl_->metrics.empty()) ? impl_->metrics.data() : nullptr;
}

// ── GFR-Ayama-3: thread enumeration ──────────────────────────────────────────
//
// Binary search by PID against thread_index. Inlined at each call site
// (Impl is private — non-member function can't reach the nested type).

uint32_t ProcessMetricsSnapshot::thread_count_for(uint32_t pid) const noexcept
{
    if (!impl_) return 0u;
    const auto& v = impl_->thread_index;
    if (v.empty()) return 0u;
    uint32_t lo = 0u, hi = static_cast<uint32_t>(v.size());
    while (lo < hi) {
        const uint32_t mid = lo + (hi - lo) / 2u;
        if (v[mid].pid == pid) return v[mid].thread_count;
        if (v[mid].pid < pid) lo = mid + 1u;
        else                  hi = mid;
    }
    return 0u;
}

uint32_t ProcessMetricsSnapshot::extract_threads(uint32_t pid,
                                                  ThreadEntry* out,
                                                  uint32_t max_count) const noexcept
{
    if (!impl_ || !out || max_count == 0u) return 0u;

    const auto& v = impl_->thread_index;
    if (v.empty()) return 0u;

    // Inline binary search by PID.
    const Impl::ThreadIndex* ti = nullptr;
    {
        uint32_t lo = 0u, hi = static_cast<uint32_t>(v.size());
        while (lo < hi) {
            const uint32_t mid = lo + (hi - lo) / 2u;
            if (v[mid].pid == pid) { ti = &v[mid]; break; }
            if (v[mid].pid < pid) lo = mid + 1u;
            else                  hi = mid;
        }
    }
    if (!ti || ti->thread_count == 0u) return 0u;

#ifdef _WIN32
    // Threads array starts at `buf + buf_offset`. Each entry is
    // GMA_SYSTEM_THREAD_INFORMATION (80B on x64).
    const uint8_t* base = impl_->buf.get() + ti->buf_offset;
    const uint32_t count = (ti->thread_count < max_count)
                           ? ti->thread_count : max_count;

    for (uint32_t i = 0u; i < count; ++i) {
        const auto* sti = reinterpret_cast<const GMA_SYSTEM_THREAD_INFORMATION*>(
            base + i * sizeof(GMA_SYSTEM_THREAD_INFORMATION));

        ThreadEntry& e = out[i];
        e.tid = static_cast<uint32_t>(
            reinterpret_cast<uintptr_t>(sti->ClientId.UniqueThread));
        e.pid = pid;
        e.kernel_time_100ns = static_cast<uint64_t>(sti->KernelTime.QuadPart);
        e.user_time_100ns   = static_cast<uint64_t>(sti->UserTime.QuadPart);
        e.create_time_100ns = static_cast<uint64_t>(sti->CreateTime.QuadPart);
        e.priority          = static_cast<int32_t>(sti->Priority);
        e.state             = static_cast<uint32_t>(sti->ThreadState);
        e.wait_reason       = static_cast<uint64_t>(sti->WaitReason);
    }
    return count;
#else
    // Linux: capture() wrote ThreadEntry structs (48 B each, fixed layout)
    // into imp.buf at ti->buf_offset. extract_threads() is just a bounded
    // memcpy — no per-thread parsing on the hot path.
    const uint8_t* base = impl_->buf.get() + ti->buf_offset;
    const uint32_t count = (ti->thread_count < max_count)
                           ? ti->thread_count : max_count;
    std::memcpy(out, base, static_cast<size_t>(count) * sizeof(ThreadEntry));
    return count;
#endif
}

} // namespace phyriad::proc
// Made with my soul - Swately <3
