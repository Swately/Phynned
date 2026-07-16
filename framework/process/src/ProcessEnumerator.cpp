// framework/process/src/ProcessEnumerator.cpp
// enumerate_processes() — Windows (EnumProcesses) and Linux (/proc) paths.
//

#include <phyriad/process/ProcessEnumerator.hpp>
#include <algorithm>
#include <cstdio>
#include <cstring>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#  include <psapi.h>
#  include <tlhelp32.h>
#else
#  include <dirent.h>
#  include <fcntl.h>
#  include <unistd.h>
#  include <cerrno>
#  include <cstdlib>
#endif

namespace phyriad::proc {

// ── Thread-local error string ────────────────────────────────────────────────
namespace {
thread_local char t_last_error[128] = {};

void set_error(const char* msg) noexcept {
    std::strncpy(t_last_error, msg, sizeof(t_last_error) - 1u);
    t_last_error[sizeof(t_last_error) - 1u] = '\0';
}
void clear_error() noexcept { t_last_error[0] = '\0'; }
} // anonymous

std::string_view last_enumerate_error() noexcept { return t_last_error; }

// ────────────────────────────────────────────────────────────────────────────
// WINDOWS
// ────────────────────────────────────────────────────────────────────────────
#ifdef _WIN32

uint32_t enumerate_processes(ProcessEntry* out, uint32_t max_count) noexcept {
    if (!out || max_count == 0u) {
        clear_error();
        return 0u;
    }

    // CreateToolhelp32Snapshot gives a coherent snapshot of all processes with
    // their parent PIDs in a single syscall — strictly more information than
    // EnumProcesses, which returns only PIDs. Trade-off vs EnumProcesses:
    //   + parent_pid populated directly (was previously unavailable)
    //   + No QueryFullProcessImageNameA path-stripping needed (szExeFile is
    //     already the basename)
    //   + Includes protected processes (the caller decides whether to skip)
    //   - Slightly slower for large process counts (snapshot copies kernel
    //     state) — measured ~2 ms for 600 processes, well below tick budget.
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) {
        set_error("CreateToolhelp32Snapshot failed");
        return 0u;
    }

    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);

    uint32_t out_count = 0u;
    if (Process32FirstW(snap, &pe)) {
        do {
            if (out_count >= max_count) break;
            if (pe.th32ProcessID == 0u) continue;   // System Idle Process

            ProcessEntry& e = out[out_count];
            e.pid        = static_cast<uint32_t>(pe.th32ProcessID);
            e.parent_pid = static_cast<uint32_t>(pe.th32ParentProcessID);
            e.start_time = 0u;
            e.name[0]    = '\0';

            // Convert WCHAR exe basename → narrow string for ProcessEntry::name.
            // CP_UTF8 is the most portable; clients displaying non-ASCII names
            // get correct rendering when the consumer treats name as UTF-8.
            WideCharToMultiByte(
                CP_UTF8, 0,
                pe.szExeFile, -1,
                e.name, static_cast<int>(sizeof(e.name) - 1u),
                nullptr, nullptr);
            e.name[sizeof(e.name) - 1u] = '\0';

            // Creation time via GetProcessTimes still requires OpenProcess.
            // Failure is non-fatal: the entry remains valid with start_time=0
            // (e.g. for protected processes like System, smss.exe).
            HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,
                                   FALSE, pe.th32ProcessID);
            if (h) {
                FILETIME ft_create{}, ft_exit{}, ft_kernel{}, ft_user{};
                if (GetProcessTimes(h, &ft_create, &ft_exit, &ft_kernel, &ft_user)) {
                    e.start_time =
                        (static_cast<uint64_t>(ft_create.dwHighDateTime) << 32u) |
                         static_cast<uint64_t>(ft_create.dwLowDateTime);
                }
                CloseHandle(h);
            }

            ++out_count;
        } while (Process32NextW(snap, &pe));
    }

    CloseHandle(snap);
    clear_error();
    return out_count;
}

// ────────────────────────────────────────────────────────────────────────────
// LINUX — readdir(/proc) + read(/proc/<pid>/comm)
// ────────────────────────────────────────────────────────────────────────────
#else

uint32_t enumerate_processes(ProcessEntry* out, uint32_t max_count) noexcept {
    if (!out || max_count == 0u) {
        clear_error();
        return 0u;
    }

    DIR* d = opendir("/proc");
    if (!d) {
        set_error("opendir(/proc) failed");
        return 0u;
    }

    uint32_t out_count = 0u;
    struct dirent* dent = nullptr;

    while ((dent = readdir(d)) != nullptr && out_count < max_count) {
        // Only entries whose name is purely numeric are processes.
        char* endp = nullptr;
        const unsigned long pid_ul =
            std::strtoul(dent->d_name, &endp, 10);
        if (*endp != '\0') continue;  // not a PID directory
        if (pid_ul == 0u)  continue;  // skip /proc/0 (shouldn't exist normally)

        ProcessEntry& e = out[out_count];
        e.pid        = static_cast<uint32_t>(pid_ul);
        e.parent_pid = 0u;
        e.start_time = 0u;
        e.name[0]    = '\0';

        // /proc/<pid>/comm — short name (up to 15 chars + newline).
        char comm_path[32];
        std::snprintf(comm_path, sizeof(comm_path),
                      "/proc/%s/comm", dent->d_name);
        const int fd = ::open(comm_path, O_RDONLY | O_CLOEXEC);
        if (fd >= 0) {
            const ssize_t n =
                ::read(fd, e.name, sizeof(e.name) - 1u);
            ::close(fd);
            if (n > 0) {
                // Trim trailing newline that /proc/<pid>/comm appends.
                ssize_t len = n;
                if (len > 0 && e.name[len - 1] == '\n') --len;
                e.name[len] = '\0';
            } else {
                e.name[0] = '\0';
            }
        }
        // If the process exited between readdir and open, the file may be gone.
        // In that case name stays empty — caller can skip pid with empty name.

        // /proc/<pid>/stat — PPID is field 4 and start_time is field 22 (per
        // proc(5)). The comm field (field 2) can legally contain spaces and
        // parentheses, so we must locate the LAST ')' before tokenising:
        //
        //   "<pid> (<comm>) <state> <ppid> <pgrp> ... <start_time> ..."
        //   ^                       ^                ^
        //   buf                     here we tokenise this field is #22 (clock ticks)
        char stat_path[32];
        std::snprintf(stat_path, sizeof(stat_path),
                      "/proc/%s/stat", dent->d_name);
        const int stat_fd = ::open(stat_path, O_RDONLY | O_CLOEXEC);
        if (stat_fd >= 0) {
            char buf[512];
            const ssize_t sn = ::read(stat_fd, buf, sizeof(buf) - 1u);
            ::close(stat_fd);
            if (sn > 0) {
                buf[sn] = '\0';
                // Locate the LAST ')' which closes the comm field.
                char* p = nullptr;
                for (ssize_t i = sn - 1; i >= 0; --i) {
                    if (buf[i] == ')') { p = buf + i + 1; break; }
                }
                if (p) {
                    // Skip ' state ' (single non-space token + spaces).
                    while (*p == ' ' || *p == '\t') ++p;
                    while (*p && *p != ' ' && *p != '\t') ++p;
                    // Field 4: ppid (strtoul skips leading whitespace).
                    e.parent_pid = static_cast<uint32_t>(
                        std::strtoul(p, &p, 10));
                    // Skip fields 5..21 (17 fields) to reach start_time (#22).
                    for (int skip = 0; skip < 17 && *p; ++skip) {
                        (void)std::strtoul(p, &p, 10);
                    }
                    // Field 22: start_time in clock ticks since boot. We store
                    // the raw ticks; callers translate via sysconf(_SC_CLK_TCK)
                    // if they need wall-clock seconds.
                    e.start_time = static_cast<uint64_t>(
                        std::strtoull(p, nullptr, 10));
                }
            }
        }

        ++out_count;
    }

    closedir(d);
    clear_error();
    return out_count;
}

#endif // _WIN32

} // namespace phyriad::proc
// Made with my soul - Swately <3
