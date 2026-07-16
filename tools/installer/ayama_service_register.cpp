// apps/ayama/tools/installer/ayama_service_register.cpp
// ayama-service-register — Windows service registration helper.
//
// Registers ayama-agent.exe as a Windows Service that starts automatically.
// Must be run as Administrator. This is an alternative to install.bat for
// programmatic installation from a GUI wizard or CI.
//
// Usage:
//   ayama-service-register install   [--path <exe_path>]
//   ayama-service-register uninstall
//   ayama-service-register status
//   ayama-service-register start
//   ayama-service-register stop
//
// The service name is "AyamaAgent". Display name: "Ayama Runtime Optimizer".
//
#include <cstdio>
#include <cstring>
#include <cstdlib>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shlobj.h>
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "shell32.lib")
#endif

static constexpr const char* kServiceName    = "AyamaAgent";
static constexpr const char* kDisplayName    = "Ayama Runtime Optimizer";
static constexpr const char* kDescription    = "Ayama background optimizer. "
    "Non-invasive CPU affinity tuning for games and multi-process workloads. "
    "Uses only documented Windows APIs (SetProcessAffinityMask, ETW counters). "
    "Requires Administrator for full functionality.";

#ifdef _WIN32

// ── Helpers ───────────────────────────────────────────────────────────────
static bool is_admin() noexcept {
    BOOL elevated = FALSE;
    HANDLE token = nullptr;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        TOKEN_ELEVATION elev{};
        DWORD sz = sizeof(elev);
        if (GetTokenInformation(token, TokenElevation, &elev, sz, &sz)) {
            elevated = elev.TokenIsElevated;
        }
        CloseHandle(token);
    }
    return elevated != FALSE;
}

static bool get_default_exe_path(char* out, DWORD max) noexcept {
    // Try same directory as this executable first.
    if (!GetModuleFileNameA(nullptr, out, max)) return false;

    // Walk back to directory.
    char* last_sep = nullptr;
    for (char* p = out; *p; ++p) {
        if (*p == '\\' || *p == '/') last_sep = p;
    }
    if (last_sep) {
        *(last_sep + 1) = '\0';
        strncat_s(out, max, "ayama-agent.exe", _TRUNCATE);
    }
    return true;
}

// ── install ───────────────────────────────────────────────────────────────
static int do_install(const char* exe_path) {
    char default_path[MAX_PATH]{};

    if (exe_path == nullptr || exe_path[0] == '\0') {
        if (!get_default_exe_path(default_path, sizeof(default_path))) {
            std::fprintf(stderr, "Error: cannot determine executable path.\n");
            return 1;
        }
        exe_path = default_path;
    }

    // Verify exe exists.
    if (GetFileAttributesA(exe_path) == INVALID_FILE_ATTRIBUTES) {
        std::fprintf(stderr, "Error: executable not found: %s\n", exe_path);
        return 1;
    }

    SC_HANDLE scm = OpenSCManagerA(nullptr, nullptr, SC_MANAGER_CREATE_SERVICE);
    if (!scm) {
        std::fprintf(stderr, "Error: cannot open Service Control Manager (admin required). "
                              "Error=%lu\n", GetLastError());
        return 1;
    }

    // Build quoted binary path.
    char bin_path[MAX_PATH + 4];
    std::snprintf(bin_path, sizeof(bin_path), "\"%s\"", exe_path);

    SC_HANDLE svc = CreateServiceA(
        scm, kServiceName, kDisplayName,
        SERVICE_ALL_ACCESS,
        SERVICE_WIN32_OWN_PROCESS,
        SERVICE_AUTO_START,          // Auto-start with Windows
        SERVICE_ERROR_NORMAL,
        bin_path,
        nullptr,                     // No load order group
        nullptr,                     // No tag ID
        nullptr,                     // No dependencies
        nullptr,                     // LocalSystem account
        nullptr                      // No password
    );

    if (!svc) {
        DWORD err = GetLastError();
        if (err == ERROR_SERVICE_EXISTS) {
            std::fprintf(stderr, "Service '%s' already exists. "
                                 "Uninstall first.\n", kServiceName);
        } else {
            std::fprintf(stderr, "CreateService failed: error=%lu\n", err);
        }
        CloseServiceHandle(scm);
        return 1;
    }

    // Set description.
    SERVICE_DESCRIPTIONA desc{};
    desc.lpDescription = const_cast<char*>(kDescription);
    ChangeServiceConfig2A(svc, SERVICE_CONFIG_DESCRIPTION, &desc);

    // Set failure actions: restart after 60s on first 2 failures.
    SC_ACTION actions[3]{};
    actions[0].Type  = SC_ACTION_RESTART;   actions[0].Delay = 60000;
    actions[1].Type  = SC_ACTION_RESTART;   actions[1].Delay = 60000;
    actions[2].Type  = SC_ACTION_NONE;      actions[2].Delay = 0;

    SERVICE_FAILURE_ACTIONSA fa{};
    fa.dwResetPeriod = 86400;   // 24h
    fa.lpCommand     = nullptr;
    fa.lpRebootMsg   = nullptr;
    fa.cActions      = 3;
    fa.lpsaActions   = actions;
    ChangeServiceConfig2A(svc, SERVICE_CONFIG_FAILURE_ACTIONS, &fa);

    std::fprintf(stdout,
        "Service '%s' installed successfully.\n"
        "  Binary: %s\n"
        "  Start:  Automatic (at boot)\n\n"
        "To start immediately: ayama-service-register start\n"
        "To start at next boot: reboot or start manually.\n",
        kServiceName, exe_path);

    CloseServiceHandle(svc);
    CloseServiceHandle(scm);
    return 0;
}

// ── uninstall ─────────────────────────────────────────────────────────────
static int do_uninstall() {
    SC_HANDLE scm = OpenSCManagerA(nullptr, nullptr, SC_MANAGER_ALL_ACCESS);
    if (!scm) {
        std::fprintf(stderr, "Error: cannot open SCM. Error=%lu\n", GetLastError());
        return 1;
    }

    SC_HANDLE svc = OpenServiceA(scm, kServiceName,
                                 SERVICE_STOP | SERVICE_QUERY_STATUS | DELETE);
    if (!svc) {
        std::fprintf(stderr, "Service '%s' not found.\n", kServiceName);
        CloseServiceHandle(scm);
        return 1;
    }

    // Stop if running.
    SERVICE_STATUS ss{};
    ControlService(svc, SERVICE_CONTROL_STOP, &ss);

    // Delete.
    if (DeleteService(svc)) {
        std::fprintf(stdout, "Service '%s' uninstalled.\n", kServiceName);
    } else {
        std::fprintf(stderr, "DeleteService failed: error=%lu\n", GetLastError());
    }

    CloseServiceHandle(svc);
    CloseServiceHandle(scm);
    return 0;
}

// ── status ────────────────────────────────────────────────────────────────
static int do_status() {
    SC_HANDLE scm = OpenSCManagerA(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scm) {
        std::fprintf(stderr, "Error: cannot open SCM.\n");
        return 1;
    }

    SC_HANDLE svc = OpenServiceA(scm, kServiceName,
                                 SERVICE_QUERY_STATUS | SERVICE_QUERY_CONFIG);
    if (!svc) {
        std::fprintf(stdout, "Service '%s': NOT INSTALLED\n", kServiceName);
        CloseServiceHandle(scm);
        return 1;
    }

    SERVICE_STATUS_PROCESS ssp{};
    DWORD bytes_needed = 0u;
    QueryServiceStatusEx(svc, SC_STATUS_PROCESS_INFO,
                         reinterpret_cast<LPBYTE>(&ssp),
                         sizeof(ssp), &bytes_needed);

    const char* state_str =
        ssp.dwCurrentState == SERVICE_RUNNING  ? "RUNNING"  :
        ssp.dwCurrentState == SERVICE_STOPPED  ? "STOPPED"  :
        ssp.dwCurrentState == SERVICE_PAUSED   ? "PAUSED"   :
        ssp.dwCurrentState == SERVICE_START_PENDING ? "STARTING" :
        ssp.dwCurrentState == SERVICE_STOP_PENDING  ? "STOPPING" : "UNKNOWN";

    std::fprintf(stdout,
        "Service '%s':\n"
        "  Display: %s\n"
        "  State:   %s\n"
        "  PID:     %lu\n",
        kServiceName, kDisplayName,
        state_str,
        static_cast<unsigned long>(ssp.dwProcessId));

    CloseServiceHandle(svc);
    CloseServiceHandle(scm);
    return 0;
}

// ── start ─────────────────────────────────────────────────────────────────
static int do_start() {
    SC_HANDLE scm = OpenSCManagerA(nullptr, nullptr, SC_MANAGER_CONNECT);
    SC_HANDLE svc = scm ? OpenServiceA(scm, kServiceName, SERVICE_START) : nullptr;

    int ret = 0;
    if (svc) {
        if (StartServiceA(svc, 0, nullptr)) {
            std::fprintf(stdout, "Service '%s' started.\n", kServiceName);
        } else {
            std::fprintf(stderr, "StartService failed: error=%lu\n", GetLastError());
            ret = 1;
        }
        CloseServiceHandle(svc);
    } else {
        std::fprintf(stderr, "Cannot open service '%s'.\n", kServiceName);
        ret = 1;
    }
    if (scm) CloseServiceHandle(scm);
    return ret;
}

// ── stop ──────────────────────────────────────────────────────────────────
static int do_stop() {
    SC_HANDLE scm = OpenSCManagerA(nullptr, nullptr, SC_MANAGER_CONNECT);
    SC_HANDLE svc = scm ? OpenServiceA(scm, kServiceName, SERVICE_STOP) : nullptr;

    int ret = 0;
    if (svc) {
        SERVICE_STATUS ss{};
        if (ControlService(svc, SERVICE_CONTROL_STOP, &ss)) {
            std::fprintf(stdout, "Service '%s' stop requested.\n", kServiceName);
        } else {
            std::fprintf(stderr, "ControlService STOP failed: error=%lu\n", GetLastError());
            ret = 1;
        }
        CloseServiceHandle(svc);
    } else {
        std::fprintf(stderr, "Cannot open service '%s'.\n", kServiceName);
        ret = 1;
    }
    if (scm) CloseServiceHandle(scm);
    return ret;
}

#else  // !_WIN32

static int do_install(const char*) {
    std::fprintf(stderr, "Service registration is Windows-only.\n"
                         "On Linux, create a systemd unit for ayama-agent.\n");
    return 1;
}
static int do_uninstall() { return do_install(nullptr); }
static int do_status()    { return do_install(nullptr); }
static int do_start()     { return do_install(nullptr); }
static int do_stop()      { return do_install(nullptr); }

#endif  // _WIN32

// ── main ─────────────────────────────────────────────────────────────────────
static void print_usage() {
    std::fprintf(stdout,
        "ayama-service-register v0.1\n"
        "Register ayama-agent.exe as a Windows Service.\n\n"
        "Usage:\n"
        "  ayama-service-register install   [--path <agent.exe>]\n"
        "  ayama-service-register uninstall\n"
        "  ayama-service-register status\n"
        "  ayama-service-register start\n"
        "  ayama-service-register stop\n\n"
        "Administrator rights required for install/uninstall/start/stop.\n");
}

int main(int argc, char** argv)
{
    if (argc < 2) { print_usage(); return 0; }

    const char* cmd = argv[1];

#ifdef _WIN32
    if (!is_admin()) {
        std::fprintf(stderr,
            "Error: Administrator privileges required.\n"
            "Right-click and select 'Run as Administrator'.\n");
        return 1;
    }
#endif

    if (std::strcmp(cmd, "install") == 0) {
        const char* path = nullptr;
        for (int i = 2; i < argc - 1; ++i) {
            if (std::strcmp(argv[i], "--path") == 0) {
                path = argv[i + 1];
                break;
            }
        }
        return do_install(path);
    }

    if (std::strcmp(cmd, "uninstall") == 0) return do_uninstall();
    if (std::strcmp(cmd, "status")    == 0) return do_status();
    if (std::strcmp(cmd, "start")     == 0) return do_start();
    if (std::strcmp(cmd, "stop")      == 0) return do_stop();

    std::fprintf(stderr, "Unknown command: %s\n\n", cmd);
    print_usage();
    return 1;
}
// Made with my soul - Swately <3
