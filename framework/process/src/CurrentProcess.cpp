// framework/process/src/CurrentProcess.cpp
// CurrentProcess — implementation of self_pid(), self_ppid(), self_name().
//
// All values are function-local statics: computed once per process lifetime,
// returned cheaply on all subsequent calls.
//


#include <phyriad/process/CurrentProcess.hpp>

#include <cstdio>
#include <cstring>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#  include <psapi.h>    // GetModuleBaseNameA
#else
#  include <unistd.h>
#  include <sys/types.h>
#  include <cstdlib>

// /proc/self/status parser for Linux ppid.
static uint32_t read_ppid_linux() noexcept {
    std::FILE* f = std::fopen("/proc/self/status", "r");
    if (!f) return 0u;
    char line[128]{};
    uint32_t ppid = 0u;
    while (std::fgets(line, sizeof(line), f)) {
        if (std::strncmp(line, "PPid:", 5) == 0) {
            ppid = static_cast<uint32_t>(std::strtoul(line + 5, nullptr, 10));
            break;
        }
    }
    std::fclose(f);
    return ppid;
}
#endif

namespace phyriad::proc {

// ── self_pid ──────────────────────────────────────────────────────────────────
uint32_t self_pid() noexcept {
    static const uint32_t pid = []() noexcept -> uint32_t {
#ifdef _WIN32
        return static_cast<uint32_t>(GetCurrentProcessId());
#else
        return static_cast<uint32_t>(::getpid());
#endif
    }();
    return pid;
}

// ── self_ppid ─────────────────────────────────────────────────────────────────
uint32_t self_ppid() noexcept {
    static const uint32_t ppid = []() noexcept -> uint32_t {
#ifdef _WIN32
        // Windows: use NtQueryInformationProcess to get ParentProcessId.
        // Fall back to 0 if unavailable (sandboxed, older SKU).
        using NtQueryInfoProc_t = LONG (NTAPI*)(HANDLE, DWORD, PVOID, ULONG, PULONG);

        HMODULE ntdll = GetModuleHandleA("ntdll.dll");
        if (!ntdll) return 0u;

        auto fn = reinterpret_cast<NtQueryInfoProc_t>(
            GetProcAddress(ntdll, "NtQueryInformationProcess"));
        if (!fn) return 0u;

        // ProcessBasicInformation = 0; struct has ParentProcessId at offset 24 (x64)
        struct BasicInfo {
            PVOID   Reserved1;
            PVOID   PebBaseAddress;
            PVOID   Reserved2[2];
            ULONG_PTR UniqueProcessId;
            ULONG_PTR InheritedFromUniqueProcessId;
        } info{};
        LONG s = fn(GetCurrentProcess(), 0, &info, sizeof(info), nullptr);
        if (s != 0) return 0u;
        return static_cast<uint32_t>(info.InheritedFromUniqueProcessId);

#else
        return read_ppid_linux();
#endif
    }();
    return ppid;
}

// ── self_name ─────────────────────────────────────────────────────────────────
const char* self_name() noexcept {
    static const char* name = []() noexcept -> const char* {
        static char buf[256]{};

#ifdef _WIN32
        // GetModuleBaseNameA returns the exe name without the path.
        const DWORD n = GetModuleBaseNameA(
            GetCurrentProcess(), nullptr, buf, static_cast<DWORD>(sizeof(buf) - 1u));
        if (n == 0u) {
            // Fallback: parse GetModuleFileNameA manually.
            char full[512]{};
            GetModuleFileNameA(nullptr, full, sizeof(full) - 1u);
            const char* slash = std::strrchr(full, '\\');
            const char* src   = slash ? slash + 1 : full;
            std::strncpy(buf, src, sizeof(buf) - 1u);
            buf[sizeof(buf) - 1u] = '\0';
        }
#else
        // Linux: /proc/self/exe → realpath → basename
        char path[512]{};
        const ssize_t len = readlink("/proc/self/exe", path, sizeof(path) - 1u);
        if (len > 0) {
            path[len] = '\0';
            const char* slash = std::strrchr(path, '/');
            const char* src   = slash ? slash + 1 : path;
            std::strncpy(buf, src, sizeof(buf) - 1u);
            buf[sizeof(buf) - 1u] = '\0';
        } else {
            // macOS / other POSIX: getprogname() if available
#  if defined(__APPLE__)
            extern const char* getprogname();
            const char* pn = getprogname();
            if (pn) {
                std::strncpy(buf, pn, sizeof(buf) - 1u);
                buf[sizeof(buf) - 1u] = '\0';
            }
#  endif
        }
#endif
        return buf;
    }();
    return name;
}

} // namespace phyriad::proc
// Made with my soul - Swately <3
