// apps/ayama/tools/ayama-ui/AgentLauncher.hpp
// AgentLauncher — detect-and-spawn helper so users only need to launch
// ayama-ui.exe; the background agent process appears automatically.
//
// Behaviour matrix:
//
//   Agent already running          → AgentLauncher::start() does nothing,
//                                    spawned() returns false. UI just
//                                    connects to it like before.
//
//   Agent NOT running, spawn OK    → CreateProcess of ayama-agent.exe
//                                    (next to the UI binary), no console
//                                    window, attached to a Job Object with
//                                    JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE.
//                                    When the UI process exits (whether
//                                    cleanly or via crash), Windows tears
//                                    down the job and the spawned agent
//                                    with it. spawned() returns true.
//
//   Agent NOT running, spawn FAIL  → start() returns false. The UI still
//                                    runs but will show "Agent: disconnected"
//                                    until the user manually starts the
//                                    agent. (Most common cause: agent.exe
//                                    not found next to ui.exe; second most
//                                    common: admin-elevation denied.)
//
// Linux note: only the spawn step is implemented; there is no Job-Object
// equivalent. We rely on the user's session manager / systemd-user / Ctrl+C
// to reap the agent. Stop semantics differ but are not blocking for the
// Windows-first publication.
//
#pragma once

#include <cstdio>
#include <string>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#else
#  include <sys/types.h>
#  include <sys/stat.h>
#  include <unistd.h>
#  include <spawn.h>
extern char** environ;
#endif

namespace ayama_ui {

class AgentLauncher {
public:
    AgentLauncher() noexcept = default;
    ~AgentLauncher() noexcept { stop(); }

    AgentLauncher(const AgentLauncher&)            = delete;
    AgentLauncher& operator=(const AgentLauncher&) = delete;

    // ── Detect whether the agent is already running ───────────────────────
    // Uses the same named mutex the agent's SingleInstance guard creates.
    // OpenMutex returns a handle if it exists, regardless of who owns it.
    [[nodiscard]] static bool agent_running() noexcept {
#ifdef _WIN32
        HANDLE h = OpenMutexW(SYNCHRONIZE, FALSE, L"Local\\AyamaAgentMutex.v1");
        if (h) {
            CloseHandle(h);
            return true;
        }
        return false;
#else
        // POSIX: agent uses flock on /tmp/ayama-agent.lock. We attempt a
        // non-blocking lock to check; if it succeeds we release immediately.
        int fd = ::open("/tmp/ayama-agent.lock", O_CREAT | O_RDWR, 0666);
        if (fd < 0) return false;
        const bool locked = (flock(fd, LOCK_EX | LOCK_NB) != 0);
        if (!locked) flock(fd, LOCK_UN);
        ::close(fd);
        return locked;  // someone else holds it → agent running
#endif
    }

    // ── Spawn the agent if not running. Returns true on success. ─────────
    // The UI executable's own path is resolved via GetModuleFileNameA on
    // Windows or /proc/self/exe on Linux — we then look for ayama-agent
    // [.exe] next to it. This is more reliable than argv[0] which can be
    // a bare name when launched from PATH.
    [[nodiscard]] bool start() noexcept {
        if (agent_running()) {
            // Someone else owns the agent; don't disturb them.
            spawned_ = false;
            return true;
        }

#ifdef _WIN32
        char ui_path[MAX_PATH] = {};
        const DWORD n = GetModuleFileNameA(nullptr, ui_path, MAX_PATH);
        if (n == 0u || n >= MAX_PATH) {
            std::fprintf(stderr,
                "[ayama-ui] GetModuleFileNameA failed (GLE=%lu)\n",
                GetLastError());
            return false;
        }
        const std::string agent_path = find_agent_exe_(ui_path, "ayama-agent.exe");
        if (agent_path.empty()) {
            std::fprintf(stderr,
                "[ayama-ui] agent launcher: could not locate ayama-agent.exe "
                "(searched: next to UI, ../ayama-agent/, parent dir/bin/)\n");
            return false;
        }

        // Create a Job Object with KILL_ON_JOB_CLOSE so the agent dies when
        // the UI exits (even via crash). This is the cleanest cross-Windows
        // mechanism — no IPC handshake needed, no orphaned background
        // processes if the UI segfaults.
        job_ = CreateJobObjectW(nullptr, nullptr);
        if (!job_) {
            std::fprintf(stderr,
                "[ayama-ui] CreateJobObjectW failed (GLE=%lu)\n",
                GetLastError());
            return false;
        }
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags =
            JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!SetInformationJobObject(
                job_, JobObjectExtendedLimitInformation,
                &limits, sizeof(limits)))
        {
            std::fprintf(stderr,
                "[ayama-ui] SetInformationJobObject failed (GLE=%lu)\n",
                GetLastError());
            CloseHandle(job_); job_ = nullptr;
            return false;
        }

        // Build the command line. CreateProcess wants a mutable buffer.
        std::string cmdline = std::string("\"") + agent_path + "\"";

        STARTUPINFOA si{}; si.cb = sizeof(si);
        PROCESS_INFORMATION pi{};
        const DWORD flags = CREATE_NO_WINDOW | CREATE_SUSPENDED;
        if (!CreateProcessA(
                /*app*/        agent_path.c_str(),
                /*cmd*/        cmdline.data(),
                /*procAttr*/   nullptr,
                /*threadAttr*/ nullptr,
                /*inherit*/    FALSE,
                /*flags*/      flags,
                /*env*/        nullptr,
                /*curDir*/     nullptr,
                &si, &pi))
        {
            const DWORD gle = GetLastError();
            std::fprintf(stderr,
                "[ayama-ui] CreateProcessA failed for '%s' (GLE=%lu)\n",
                agent_path.c_str(), gle);
            CloseHandle(job_); job_ = nullptr;
            return false;
        }

        // Assign to the job BEFORE resuming so we never have a window where
        // the process is alive but not under the job's death-grip.
        if (!AssignProcessToJobObject(job_, pi.hProcess)) {
            std::fprintf(stderr,
                "[ayama-ui] AssignProcessToJobObject failed (GLE=%lu) — "
                "terminating spawned agent.\n", GetLastError());
            TerminateProcess(pi.hProcess, 1);
            CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
            CloseHandle(job_); job_ = nullptr;
            return false;
        }

        ResumeThread(pi.hThread);
        CloseHandle(pi.hThread);
        agent_process_ = pi.hProcess;
        spawned_       = true;
        return true;
#else
        // POSIX: posix_spawn ayama-agent located via the same multi-candidate
        // search the Windows side uses.
        char ui_path[4096] = {};
        const ssize_t n = readlink("/proc/self/exe", ui_path, sizeof(ui_path) - 1);
        if (n <= 0) return false;
        ui_path[n] = '\0';
        const std::string agent_path = find_agent_exe_(ui_path, "ayama-agent");
        if (agent_path.empty()) return false;
        char* const argv[] = { const_cast<char*>(agent_path.c_str()), nullptr };
        pid_t pid = 0;
        if (posix_spawn(&pid, agent_path.c_str(),
                        nullptr, nullptr, argv, environ) != 0)
        {
            return false;
        }
        agent_pid_ = pid;
        spawned_   = true;
        return true;
#endif
    }

    // ── Stop the spawned agent (if we spawned one) ────────────────────────
    void stop() noexcept {
#ifdef _WIN32
        // Closing the job handle triggers KILL_ON_JOB_CLOSE → all processes
        // attached to the job (i.e. the agent) are terminated immediately.
        if (job_) {
            CloseHandle(job_);
            job_ = nullptr;
        }
        if (agent_process_) {
            // The job kill already terminated it; just close our handle.
            CloseHandle(agent_process_);
            agent_process_ = nullptr;
        }
#else
        if (spawned_ && agent_pid_ > 0) {
            kill(agent_pid_, SIGTERM);
            agent_pid_ = 0;
        }
#endif
        spawned_ = false;
    }

    [[nodiscard]] bool spawned() const noexcept { return spawned_; }

private:
    bool spawned_{false};

#ifdef _WIN32
    HANDLE job_           {nullptr};
    HANDLE agent_process_ {nullptr};
#else
    pid_t agent_pid_      {0};
#endif

    // ── Locate the agent executable ──────────────────────────────────────
    //
    // Search order (first existing file wins, no hardcoding):
    //   1. <ui-dir>/runtime/<name>
    //        Canonical distribution layout: ayama-ui.exe
    //        lives at the top of the bundle, everything else lives in a
    //        `runtime/` subfolder so the first thing the user sees when
    //        they open the install dir is just the entry-point .exe.
    //   2. <ui-dir>/<name>
    //        Flat layout — kept so a downstream packager
    //        who flattens everything back to one dir still works.
    //   3. <ui-dir>/../ayama-agent/<name>
    //        Legacy split-build-tree layout, where each tool had its
    //        own build subdirectory.
    //   4. <ui-dir>/../bin/<name>
    //        Hypothetical future layout where ayama-ui sits next to its
    //        resources (e.g. /opt/ayama/ui/) and binaries live under
    //        /opt/ayama/bin/.
    //
    // Returns the first candidate that exists, or empty string if none do.
    [[nodiscard]] static std::string find_agent_exe_(
        const char* ui_path, const char* agent_name) noexcept
    {
        if (!ui_path || !*ui_path || !agent_name) return {};
        std::string ui_str = ui_path;
        const size_t pos = ui_str.find_last_of("/\\");
        const std::string ui_dir = (pos == std::string::npos)
            ? std::string{}
            : ui_str.substr(0, pos + 1);
        const std::string sep = "/";

        std::string candidates[] = {
            ui_dir + "runtime" + sep + agent_name,                  // (1) new canonical
            ui_dir + agent_name,                                    // (2) flat layout
            ui_dir + ".." + sep + "ayama-agent" + sep + agent_name, // (3) dev tree
            ui_dir + ".." + sep + "bin"         + sep + agent_name, // (4) future
        };
        for (const auto& path : candidates) {
            if (path_exists_(path)) return path;
        }
        return {};
    }

    [[nodiscard]] static bool path_exists_(const std::string& path) noexcept {
#ifdef _WIN32
        const DWORD attr = GetFileAttributesA(path.c_str());
        return (attr != INVALID_FILE_ATTRIBUTES) &&
               !(attr & FILE_ATTRIBUTE_DIRECTORY);
#else
        struct stat st{};
        return (::stat(path.c_str(), &st) == 0) && S_ISREG(st.st_mode);
#endif
    }
};

} // namespace ayama_ui
// Made with my soul - Swately <3
