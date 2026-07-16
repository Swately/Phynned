// apps/ayama/tools/ayama-ui/PresentMonSpawner.hpp
// PresentMonSpawner — Win32 child-process wrapper for PresentMon CLI.
//
// Spawns PresentMon.exe (downloaded at build time, see CMakeLists.txt) with
// process-name + timed + output_file arguments. Polls its lifecycle without
// blocking the UI thread.
//
// Usage (single-shot):
//   PresentMonSpawner s;
//   s.start("F:/.../PresentMon.exe", "javaw.exe", 90u, "F:/.../run1.csv");
//   while (s.state() == State::Running) { ... s.poll(); }
//   if (s.state() == State::Done) { ... import CSV ... }
//
// Threading: single-thread (UI thread). poll() is non-blocking — call from
// the per-frame draw loop.
//
#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace ayama::ui {

// PresentMon binary filename — defined by ayama-ui's CMakeLists target_compile_definitions.
#ifndef AYAMA_PRESENTMON_FILENAME
#  define AYAMA_PRESENTMON_FILENAME "PresentMon.exe"
#endif

/// Locate PresentMon.exe in the same directory as the running ayama-ui.exe.
/// Returns true if the file exists and writes the absolute path into `out`.
/// `out_cap` must be >= 260 (MAX_PATH).
inline bool locate_presentmon(char* out, uint32_t out_cap) noexcept {
#ifdef _WIN32
    if (!out || out_cap < 260u) return false;
    wchar_t wpath[MAX_PATH]{};
    const DWORD n = GetModuleFileNameW(nullptr, wpath, MAX_PATH);
    if (n == 0u || n >= MAX_PATH) return false;
    // Strip filename to get directory
    for (int i = static_cast<int>(n) - 1; i >= 0; --i) {
        if (wpath[i] == L'\\' || wpath[i] == L'/') {
            wpath[i + 1] = L'\0';
            break;
        }
    }
    // Append PresentMon binary name (narrow → wide trivially since ASCII)
    const char* fn = AYAMA_PRESENTMON_FILENAME;
    const size_t cur_len = std::wcslen(wpath);
    if (cur_len + std::strlen(fn) >= MAX_PATH) return false;
    for (size_t i = 0; fn[i]; ++i) wpath[cur_len + i] = static_cast<wchar_t>(fn[i]);
    wpath[cur_len + std::strlen(fn)] = L'\0';

    // Existence check
    const DWORD attr = GetFileAttributesW(wpath);
    if (attr == INVALID_FILE_ATTRIBUTES || (attr & FILE_ATTRIBUTE_DIRECTORY)) {
        return false;
    }
    // Wide → narrow (ASCII path safe; non-ASCII paths fall back to '?')
    const size_t len = std::wcslen(wpath);
    if (len >= out_cap) return false;
    for (size_t i = 0; i <= len; ++i) {
        out[i] = (wpath[i] < 0x80) ? static_cast<char>(wpath[i]) : '?';
    }
    return true;
#else
    (void)out; (void)out_cap;
    return false;
#endif
}

class PresentMonSpawner {
public:
    enum class State : uint8_t {
        Idle      = 0,  // Never started OR cleaned up after Done/Failed.
        Running   = 1,  // PresentMon child process is active.
        Done      = 2,  // Child exited normally (exit code 0).
        Failed    = 3,  // Spawn failed or child exited with non-zero code.
    };

    PresentMonSpawner() noexcept = default;
    ~PresentMonSpawner() noexcept { cleanup(); }

    PresentMonSpawner(const PresentMonSpawner&)            = delete;
    PresentMonSpawner& operator=(const PresentMonSpawner&) = delete;

    /// Start a capture. Returns true on successful spawn (child process
    /// created); false if PresentMon binary is missing or CreateProcess
    /// failed. `output_csv_path` will be passed to `--output_file`.
    /// `duration_sec` is passed to `--timed`. `process_name` is passed to
    /// `--process_name` (just the exe short name, e.g. "javaw.exe").
    [[nodiscard]] bool start(const char* presentmon_exe_path,
                             const char* process_name,
                             uint32_t    duration_sec,
                             const char* output_csv_path) noexcept
    {
#ifdef _WIN32
        cleanup();

        // Build command line. PresentMon CLI parses argv; we pass
        // double-quoted paths for safety.
        //
        // --stop_existing_session: a previous PresentMon run that
        // was killed (UI crash, agent crash mid-bench, Ctrl+C, etc.) leaves
        // the named ETW session "PresentMon" alive in the kernel. The next
        // launch aborts with exit code 6 and stderr "a trace session named
        // 'PresentMon' is already running". Passing this flag tells
        // PresentMon to forcibly stop any lingering session before starting
        // its own, making the bench runner resilient to prior crashes.
        char cmdline[1024];
        const int n = std::snprintf(cmdline, sizeof(cmdline),
            "\"%s\" --process_name %s --timed %u --output_file \"%s\" "
            "--no_console_stats --terminate_after_timed "
            "--stop_existing_session",
            presentmon_exe_path,
            process_name,
            duration_sec,
            output_csv_path);
        if (n < 0 || n >= static_cast<int>(sizeof(cmdline))) {
            std::strncpy(last_error_, "command line too long",
                         sizeof(last_error_) - 1u);
            state_      = State::Failed;
            return false;
        }
        // CreateProcessW wants a writable LPWSTR — copy + widen.
        wchar_t wcmd[1024]{};
        for (int i = 0; i < n && i < 1023; ++i)
            wcmd[i] = static_cast<wchar_t>(cmdline[i]);

        // Redirect PresentMon's stdout+stderr to a log file next to the CSV.
        // Critical for diagnosing failures (e.g. "no frames captured", "needs
        // admin", etc.). Same path as output_csv but with .log extension.
        std::snprintf(log_path_, sizeof(log_path_), "%s", output_csv_path);
        // Replace .csv with .log (or append .log if no extension)
        {
            char* dot = std::strrchr(log_path_, '.');
            if (dot && dot > log_path_) {
                std::strncpy(dot, ".log", static_cast<size_t>(
                    log_path_ + sizeof(log_path_) - dot - 1));
            } else {
                const size_t l = std::strlen(log_path_);
                if (l + 4u < sizeof(log_path_)) {
                    std::strcpy(log_path_ + l, ".log");
                }
            }
        }
        wchar_t wlog[260]{};
        for (int i = 0; log_path_[i] && i < 259; ++i)
            wlog[i] = static_cast<wchar_t>(log_path_[i]);

        SECURITY_ATTRIBUTES sa{};
        sa.nLength        = sizeof(sa);
        sa.bInheritHandle = TRUE;
        HANDLE hLog = CreateFileW(wlog,
            GENERIC_WRITE, FILE_SHARE_READ, &sa,
            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        // hLog may be INVALID_HANDLE_VALUE — proceed without redirection in
        // that case rather than failing the whole spawn.

        STARTUPINFOW si{};
        si.cb          = sizeof(si);
        si.dwFlags     = STARTF_USESHOWWINDOW;
        si.wShowWindow = SW_HIDE;
        if (hLog != INVALID_HANDLE_VALUE) {
            si.dwFlags    |= STARTF_USESTDHANDLES;
            si.hStdOutput  = hLog;
            si.hStdError   = hLog;
            si.hStdInput   = GetStdHandle(STD_INPUT_HANDLE);
        }
        PROCESS_INFORMATION pi{};

        const BOOL ok = CreateProcessW(
            nullptr,
            wcmd,
            nullptr,
            nullptr,
            (hLog != INVALID_HANDLE_VALUE) ? TRUE : FALSE,
            CREATE_NO_WINDOW,
            nullptr,
            nullptr,
            &si,
            &pi);
        if (hLog != INVALID_HANDLE_VALUE) CloseHandle(hLog);
        if (!ok) {
            std::strncpy(last_error_,
                         "CreateProcess failed (PresentMon spawn)",
                         sizeof(last_error_) - 1u);
            state_      = State::Failed;
            return false;
        }
        // We don't need the primary thread handle; close it now.
        CloseHandle(pi.hThread);
        proc_handle_   = pi.hProcess;
        proc_id_       = pi.dwProcessId;
        duration_sec_  = duration_sec;
        start_tick_ms_ = GetTickCount();
        std::strncpy(out_path_, output_csv_path, sizeof(out_path_) - 1u);
        std::strncpy(proc_name_, process_name, sizeof(proc_name_) - 1u);
        state_         = State::Running;
        last_error_[0] = '\0';
        return true;
#else
        (void)presentmon_exe_path;
        (void)process_name;
        (void)duration_sec;
        (void)output_csv_path;
        std::strncpy(last_error_,
                     "PresentMon spawn not implemented on Linux",
                     sizeof(last_error_) - 1u);
        state_      = State::Failed;
        return false;
#endif
    }

    /// Non-blocking poll. Call once per UI frame (or every ~100 ms). Updates
    /// `state()` and `progress()`. Safe to call when state is not Running.
    void poll() noexcept {
#ifdef _WIN32
        if (state_ != State::Running) return;
        if (!proc_handle_) {
            state_ = State::Failed;
            std::strncpy(last_error_, "no process handle",
                         sizeof(last_error_) - 1u);
            return;
        }
        const DWORD wait = WaitForSingleObject(proc_handle_, 0u);
        if (wait == WAIT_OBJECT_0) {
            DWORD code = 0u;
            GetExitCodeProcess(proc_handle_, &code);
            CloseHandle(proc_handle_);
            proc_handle_ = nullptr;
            exit_code_   = static_cast<int32_t>(code);
            state_ = (code == 0u) ? State::Done : State::Failed;
            if (state_ == State::Failed) {
                std::snprintf(last_error_, sizeof(last_error_),
                              "PresentMon exited with code %d", exit_code_);
            }
        } else if (wait != WAIT_TIMEOUT) {
            // Unexpected error
            CloseHandle(proc_handle_);
            proc_handle_ = nullptr;
            state_       = State::Failed;
            std::strncpy(last_error_, "WaitForSingleObject failed",
                         sizeof(last_error_) - 1u);
        }
        // else: still running — fall through and let caller poll again
#endif
    }

    /// Cancel an in-flight capture by terminating the child process.
    /// No-op if not Running. The output CSV may be partial/corrupt.
    void cancel() noexcept {
#ifdef _WIN32
        if (state_ == State::Running && proc_handle_) {
            TerminateProcess(proc_handle_, 1u);
            CloseHandle(proc_handle_);
            proc_handle_ = nullptr;
            state_       = State::Failed;
            std::strncpy(last_error_, "cancelled by user",
                         sizeof(last_error_) - 1u);
        }
#endif
    }

    /// Close any open handles and reset state to Idle. Safe to call from
    /// destructor or before re-starting.
    void cleanup() noexcept {
#ifdef _WIN32
        if (proc_handle_) {
            TerminateProcess(proc_handle_, 1u);
            CloseHandle(proc_handle_);
            proc_handle_ = nullptr;
        }
#endif
        state_         = State::Idle;
        proc_id_       = 0u;
        exit_code_     = 0;
        duration_sec_  = 0u;
        start_tick_ms_ = 0u;
        out_path_[0]   = '\0';
        log_path_[0]   = '\0';
        proc_name_[0]  = '\0';
        last_error_[0] = '\0';
    }

    // ── Read-only accessors ────────────────────────────────────────────────
    [[nodiscard]] State        state()         const noexcept { return state_; }
    [[nodiscard]] int32_t      exit_code()     const noexcept { return exit_code_; }
    [[nodiscard]] uint32_t     duration_sec()  const noexcept { return duration_sec_; }
    [[nodiscard]] const char*  output_path()   const noexcept { return out_path_; }
    [[nodiscard]] const char*  process_name()  const noexcept { return proc_name_; }
    [[nodiscard]] const char*  last_error()    const noexcept { return last_error_; }
    /// Path to the PresentMon stdout+stderr log (for diagnosing failures).
    [[nodiscard]] const char*  log_path()      const noexcept { return log_path_; }

    /// Elapsed time since start (seconds). 0 when Idle.
    [[nodiscard]] float elapsed_sec() const noexcept {
#ifdef _WIN32
        if (state_ == State::Idle) return 0.f;
        const uint32_t now = GetTickCount();
        return static_cast<float>(now - start_tick_ms_) / 1000.f;
#else
        return 0.f;
#endif
    }

    /// Progress fraction 0..1. Useful for ProgressBar UI. Clamped.
    [[nodiscard]] float progress() const noexcept {
        if (duration_sec_ == 0u) return 0.f;
        const float p = elapsed_sec() / static_cast<float>(duration_sec_);
        return p < 0.f ? 0.f : (p > 1.f ? 1.f : p);
    }

private:
#ifdef _WIN32
    HANDLE   proc_handle_  {nullptr};
#endif
    uint32_t proc_id_      {0u};
    int32_t  exit_code_    {0};
    uint32_t duration_sec_ {0u};
    uint32_t start_tick_ms_{0u};
    State    state_        {State::Idle};
    char     out_path_[260]{};
    char     log_path_[260]{};
    char     proc_name_[64]{};
    char     last_error_[128]{};
};

} // namespace ayama::ui
// Made with my soul - Swately <3
