// apps/ayama/tools/ayama-ui/BenchRunner.hpp
// BenchRunner — orchestrates single capture or A/B/A/B/A protocol.
//
// State machine that drives:
//   1. PresentMon child process for frame-time capture
//   2. ayama-cli child process for CSV import (PresentMon CSV → ayama-bench)
//   3. Agent IPC pause/resume between baseline/treated runs (5-run protocol)
//   4. ayama-cli bench multi for the final statistical aggregation
//
// Threading: single-thread (UI thread). All polling is non-blocking; call
// `tick()` once per frame.
//
#pragma once

#include "PresentMonSpawner.hpp"

#include <ayama/ipc/AyamaClient.hpp>
#include <ayama/ipc/AyamaProtocol.hpp>

#include <algorithm>
#include <array>
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

/// One captured run.
struct BenchRun {
    bool        is_baseline;          ///< false = treated (agent active)
    uint32_t    duration_sec;
    char        pm_csv_path[260];     ///< output of PresentMon
    char        bench_csv_path[260];  ///< output of ayama-cli presentmon-import
    int32_t     import_exit_code;     ///< 0 on success
    char        last_error[128];
    bool        completed;
};

class BenchRunner {
public:
    enum class State : uint8_t {
        Idle             = 0,
        CapturingRun     = 1,  ///< PresentMon child active
        ImportingRun     = 2,  ///< ayama-cli presentmon-import child active
        BetweenRuns      = 3,  ///< Cooldown between runs (also waits for cmd ack)
        AggregatingStats = 4,  ///< ayama-cli bench multi running
        Done             = 5,
        Failed           = 6,
    };

    enum class Protocol : uint8_t {
        Single5Run0  = 0,  ///< Single capture (baseline-only, no agent control)
        ABABA5Run    = 1,  ///< 5-run baseline/treated/baseline/treated/baseline
    };

    BenchRunner() noexcept = default;
    ~BenchRunner() noexcept { cancel(); }

    BenchRunner(const BenchRunner&)            = delete;
    BenchRunner& operator=(const BenchRunner&) = delete;

    // ── Configuration (called before start_*) ──────────────────────────────

    /// Path to PresentMon.exe. Located via locate_presentmon() typically.
    void set_presentmon_path(const char* path) noexcept {
        std::strncpy(pm_exe_, path, sizeof(pm_exe_) - 1u);
    }

    /// Path to ayama-cli.exe. The UI looks for it next to itself.
    void set_ayama_cli_path(const char* path) noexcept {
        std::strncpy(cli_exe_, path, sizeof(cli_exe_) - 1u);
    }

    /// Directory where captured CSVs are written.
    void set_output_dir(const char* dir) noexcept {
        std::strncpy(out_dir_, dir, sizeof(out_dir_) - 1u);
    }

    /// Pointer to client for sending pause/resume to agent (5-run protocol).
    /// If null, the 5-run protocol cannot be used (single capture still OK).
    void set_client(ipc::AyamaClient* c) noexcept { client_ = c; }

    // ── Start operations ───────────────────────────────────────────────────

    /// Start a single capture (no agent control). Useful for ad-hoc tests.
    /// Returns true on successful spawn.
    [[nodiscard]] bool start_single(const char* process_name,
                                     uint32_t    duration_sec) noexcept;

    /// Start the full A/B/A/B/A 5-run protocol. Requires client_ set so the
    /// runner can pause/resume policies between baseline and treated runs.
    [[nodiscard]] bool start_ababa(const char* process_name,
                                    uint32_t    duration_sec) noexcept;

    /// Cancel any in-flight operation. Best-effort: terminates child processes
    /// and resends "resume policies" to leave the agent in a normal state.
    void cancel() noexcept;

    // ── Per-frame tick ─────────────────────────────────────────────────────

    /// Advance the state machine. Call once per UI frame.
    void tick() noexcept;

    // ── Read-only accessors ────────────────────────────────────────────────
    [[nodiscard]] State    state()         const noexcept { return state_; }
    [[nodiscard]] Protocol protocol()      const noexcept { return protocol_; }
    [[nodiscard]] uint32_t run_index()     const noexcept { return run_index_; }  // 0..n_runs-1
    [[nodiscard]] uint32_t n_runs()        const noexcept { return n_runs_; }
    [[nodiscard]] float    run_progress()  const noexcept { return pm_.progress(); }
    [[nodiscard]] uint32_t run_duration()  const noexcept { return runs_[std::min(run_index_, kMaxRuns - 1u)].duration_sec; }
    [[nodiscard]] const char* process_name() const noexcept { return proc_name_; }
    [[nodiscard]] const char* last_error()   const noexcept { return last_error_; }

    /// Array of completed runs. Valid through n_runs() entries.
    [[nodiscard]] const BenchRun* runs() const noexcept { return runs_.data(); }

    /// Output of the final `ayama-cli bench multi` run, captured to a file
    /// the UI can read & display in a multi-line text widget.
    [[nodiscard]] const char* aggregate_log_path() const noexcept {
        return agg_log_;
    }

private:
    static constexpr uint32_t kMaxRuns          = 5u;
    static constexpr uint32_t kBetweenRunsCdMs  = 3000u;  ///< 3 s cooldown
    static constexpr uint32_t kAgentAckTimeoutMs = 2000u; ///< wait up to 2 s for ack

    PresentMonSpawner    pm_{};            // current capture
    ipc::AyamaClient*    client_{nullptr}; // for IPC commands (optional)

    State    state_     {State::Idle};
    Protocol protocol_  {Protocol::Single5Run0};
    uint32_t n_runs_    {0u};
    uint32_t run_index_ {0u};
    char     proc_name_[64]{};

    // Cooldown / ack-wait tracking
    uint32_t between_runs_start_tick_ms_{0u};
    uint64_t expected_cmd_ack_seq_{0ull};
    bool     waiting_for_ack_{false};

    // Result storage
    std::array<BenchRun, kMaxRuns> runs_{};

    // Subprocess for import (ayama-cli presentmon-import)
#ifdef _WIN32
    HANDLE   import_proc_{nullptr};
    HANDLE   agg_proc_{nullptr};
#endif

    // Paths
    char pm_exe_[260]{};
    char cli_exe_[260]{};
    char out_dir_[260]{};
    char agg_log_[260]{};
    char last_error_[128]{};

    // ── State helpers ──────────────────────────────────────────────────────
    bool begin_run_(uint32_t idx) noexcept;
    bool spawn_import_(const BenchRun& r) noexcept;
    bool spawn_aggregate_() noexcept;
    void poll_import_() noexcept;
    void poll_aggregate_() noexcept;
    void enter_between_or_finish_() noexcept;
    bool send_policy_command_for_next_run_() noexcept;
    static uint32_t now_tick_ms_() noexcept {
#ifdef _WIN32
        return GetTickCount();
#else
        return 0u;
#endif
    }
};

// ─────────────────────────────────────────────────────────────────────────
// Implementation (header-only — small enough not to need a .cpp)
// ─────────────────────────────────────────────────────────────────────────

inline bool BenchRunner::start_single(const char* process_name,
                                       uint32_t    duration_sec) noexcept
{
    // Allow restart from terminal states (Done/Failed): clean up first, then
    // proceed. Only block if we're mid-flight on another capture.
    if (state_ == State::Done || state_ == State::Failed) {
        cancel();  // resets state_ to Idle, closes handles, cleans paths
    }
    if (state_ != State::Idle) {
        std::snprintf(last_error_, sizeof(last_error_),
                      "runner busy (state=%u)", static_cast<unsigned>(state_));
        return false;
    }
    if (!pm_exe_[0]) {
        std::snprintf(last_error_, sizeof(last_error_),
                      "PresentMon path not set");
        return false;
    }
    protocol_  = Protocol::Single5Run0;
    n_runs_    = 1u;
    run_index_ = 0u;
    std::strncpy(proc_name_, process_name, sizeof(proc_name_) - 1u);

    // Initialize single run as baseline (no agent control).
    BenchRun& r = runs_[0];
    std::memset(&r, 0, sizeof(r));
    r.is_baseline  = true;
    r.duration_sec = duration_sec;
    std::snprintf(r.pm_csv_path,    sizeof(r.pm_csv_path),
                  "%s/run1.pm.csv", out_dir_);
    std::snprintf(r.bench_csv_path, sizeof(r.bench_csv_path),
                  "%s/run1.bench.csv", out_dir_);

    return begin_run_(0u);
}

inline bool BenchRunner::start_ababa(const char* process_name,
                                      uint32_t    duration_sec) noexcept
{
    // Same restart-from-terminal-state policy as start_single.
    if (state_ == State::Done || state_ == State::Failed) {
        cancel();
    }
    if (state_ != State::Idle) {
        std::snprintf(last_error_, sizeof(last_error_),
                      "runner busy (state=%u)", static_cast<unsigned>(state_));
        return false;
    }
    if (!client_ || !client_->can_send_commands()) {
        std::snprintf(last_error_, sizeof(last_error_),
                      "client not connected or read-only — 5-run protocol "
                      "requires agent IPC for pause/resume");
        return false;
    }
    if (!pm_exe_[0] || !cli_exe_[0]) {
        std::snprintf(last_error_, sizeof(last_error_),
                      "PresentMon and ayama-cli paths must be set");
        return false;
    }

    protocol_  = Protocol::ABABA5Run;
    n_runs_    = 5u;
    run_index_ = 0u;
    std::strncpy(proc_name_, process_name, sizeof(proc_name_) - 1u);

    // A/B/A/B/A: idx 0,2,4 = baseline (paused); idx 1,3 = treated (active).
    for (uint32_t i = 0u; i < 5u; ++i) {
        BenchRun& r = runs_[i];
        std::memset(&r, 0, sizeof(r));
        r.is_baseline  = (i % 2u == 0u);
        r.duration_sec = duration_sec;
        std::snprintf(r.pm_csv_path,    sizeof(r.pm_csv_path),
                      "%s/run%u.pm.csv", out_dir_, i + 1u);
        std::snprintf(r.bench_csv_path, sizeof(r.bench_csv_path),
                      "%s/run%u.bench.csv", out_dir_, i + 1u);
    }

    // For run 0 (baseline): send pause first so the first capture is clean.
    if (!send_policy_command_for_next_run_()) return false;
    // must transition out of Idle so tick() advances.
    // Without this the runner sat in Idle waiting for tick() to do nothing,
    // and the panel kept showing the configuration block instead of progress.
    state_ = State::BetweenRuns;
    return true;
}

inline void BenchRunner::cancel() noexcept {
    pm_.cancel();
#ifdef _WIN32
    if (import_proc_) {
        TerminateProcess(import_proc_, 1u);
        CloseHandle(import_proc_);
        import_proc_ = nullptr;
    }
    if (agg_proc_) {
        TerminateProcess(agg_proc_, 1u);
        CloseHandle(agg_proc_);
        agg_proc_ = nullptr;
    }
#endif
    // Leave agent in a normal state — resume policies if we paused them.
    if (protocol_ == Protocol::ABABA5Run && client_
        && client_->can_send_commands()) {
        (void)client_->send_command(ipc::kAyamaCmdResumePolicies);
    }
    state_           = State::Idle;
    waiting_for_ack_ = false;
}

inline void BenchRunner::tick() noexcept {
    switch (state_) {
    case State::Idle:
    case State::Done:
    case State::Failed:
        return;

    case State::CapturingRun:
        pm_.poll();
        if (pm_.state() == PresentMonSpawner::State::Done) {
            // Capture finished. Now import the CSV.
            BenchRun& r = runs_[run_index_];
            r.completed = true;
            if (!spawn_import_(r)) {
                std::snprintf(r.last_error, sizeof(r.last_error),
                              "%s", last_error_);
                state_ = State::Failed;
                return;
            }
            state_ = State::ImportingRun;
        } else if (pm_.state() == PresentMonSpawner::State::Failed) {
            BenchRun& r = runs_[run_index_];
            r.completed = false;
            std::snprintf(r.last_error, sizeof(r.last_error),
                          "PresentMon failed: %s", pm_.last_error());
            std::strncpy(last_error_, r.last_error, sizeof(last_error_) - 1u);
            state_ = State::Failed;
        }
        return;

    case State::ImportingRun:
        poll_import_();
        return;

    case State::BetweenRuns:
        // Wait for cooldown OR agent ack (whichever comes last).
        if (waiting_for_ack_ && client_) {
            const uint64_t ack = client_->command_ack();
            if (ack >= expected_cmd_ack_seq_) waiting_for_ack_ = false;
        }
        if (!waiting_for_ack_) {
            const uint32_t elapsed =
                now_tick_ms_() - between_runs_start_tick_ms_;
            if (elapsed >= kBetweenRunsCdMs) {
                if (!begin_run_(run_index_)) {
                    state_ = State::Failed;
                }
            }
        } else {
            // Ack timeout fallback
            const uint32_t elapsed =
                now_tick_ms_() - between_runs_start_tick_ms_;
            if (elapsed >= kAgentAckTimeoutMs) {
                // Proceed anyway — agent will catch up on the command.
                waiting_for_ack_ = false;
            }
        }
        return;

    case State::AggregatingStats:
        poll_aggregate_();
        return;
    }
}

inline bool BenchRunner::begin_run_(uint32_t idx) noexcept {
    run_index_ = idx;
    BenchRun& r = runs_[idx];
    if (!pm_.start(pm_exe_, proc_name_, r.duration_sec, r.pm_csv_path)) {
        std::snprintf(last_error_, sizeof(last_error_),
                      "PresentMon spawn failed: %s", pm_.last_error());
        return false;
    }
    state_ = State::CapturingRun;
    return true;
}

inline bool BenchRunner::spawn_import_(const BenchRun& r) noexcept {
#ifdef _WIN32
    char cmdline[768];
    std::snprintf(cmdline, sizeof(cmdline),
        "\"%s\" presentmon-import \"%s\" --process %s --output \"%s\"",
        cli_exe_, r.pm_csv_path, proc_name_, r.bench_csv_path);
    wchar_t wcmd[768]{};
    for (int i = 0; cmdline[i] && i < 767; ++i)
        wcmd[i] = static_cast<wchar_t>(cmdline[i]);

    STARTUPINFOW si{};
    si.cb          = sizeof(si);
    si.dwFlags     = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, wcmd, nullptr, nullptr, FALSE,
                         CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        std::snprintf(last_error_, sizeof(last_error_),
                      "CreateProcess(ayama-cli import) failed");
        return false;
    }
    CloseHandle(pi.hThread);
    import_proc_ = pi.hProcess;
    return true;
#else
    (void)r;
    last_error_[0] = 'L'; last_error_[1] = '\0';
    return false;
#endif
}

inline void BenchRunner::poll_import_() noexcept {
#ifdef _WIN32
    if (!import_proc_) { state_ = State::Failed; return; }
    const DWORD w = WaitForSingleObject(import_proc_, 0u);
    if (w == WAIT_OBJECT_0) {
        DWORD code = 0u;
        GetExitCodeProcess(import_proc_, &code);
        CloseHandle(import_proc_);
        import_proc_ = nullptr;
        BenchRun& r = runs_[run_index_];
        r.import_exit_code = static_cast<int32_t>(code);
        if (code != 0u) {
            std::snprintf(r.last_error, sizeof(r.last_error),
                          "ayama-cli presentmon-import exited %lu",
                          static_cast<unsigned long>(code));
            std::strncpy(last_error_, r.last_error,
                         sizeof(last_error_) - 1u);
            state_ = State::Failed;
            return;
        }
        enter_between_or_finish_();
    } else if (w != WAIT_TIMEOUT) {
        CloseHandle(import_proc_);
        import_proc_ = nullptr;
        std::snprintf(last_error_, sizeof(last_error_),
                      "WaitForSingleObject(import) failed");
        state_ = State::Failed;
    }
#endif
}

inline void BenchRunner::enter_between_or_finish_() noexcept {
    const uint32_t next = run_index_ + 1u;
    if (next >= n_runs_) {
        // All runs done. For ABABA, leave agent in resumed state.
        if (protocol_ == Protocol::ABABA5Run && client_
            && client_->can_send_commands()) {
            (void)client_->send_command(ipc::kAyamaCmdResumePolicies);
        }
        // For Single5Run0, no aggregation step (only 1 run).
        if (protocol_ == Protocol::Single5Run0) {
            state_ = State::Done;
            return;
        }
        // ABABA: spawn the aggregate stats run.
        if (!spawn_aggregate_()) {
            state_ = State::Failed;
            return;
        }
        state_ = State::AggregatingStats;
        return;
    }

    // Prepare next run
    run_index_ = next;
    between_runs_start_tick_ms_ = now_tick_ms_();
    // For ABABA: send the appropriate pause/resume command for the next run.
    if (protocol_ == Protocol::ABABA5Run) {
        if (!send_policy_command_for_next_run_()) {
            state_ = State::Failed;
            return;
        }
    }
    state_ = State::BetweenRuns;
}

inline bool BenchRunner::send_policy_command_for_next_run_() noexcept {
    if (!client_ || !client_->can_send_commands()) {
        std::snprintf(last_error_, sizeof(last_error_),
                      "client not available for IPC command");
        return false;
    }
    const BenchRun& next = runs_[run_index_];
    const uint32_t cmd = next.is_baseline
        ? ipc::kAyamaCmdPausePolicies
        : ipc::kAyamaCmdResumePolicies;
    expected_cmd_ack_seq_       = client_->send_command(cmd);
    waiting_for_ack_            = (expected_cmd_ack_seq_ != 0ull);
    between_runs_start_tick_ms_ = now_tick_ms_();
    return true;
}

inline bool BenchRunner::spawn_aggregate_() noexcept {
#ifdef _WIN32
    // Build: ayama-cli bench multi --baseline runX.bench.csv ... --treated runY.bench.csv ...
    // and redirect stdout to agg_log_ for the UI to read & display.
    std::snprintf(agg_log_, sizeof(agg_log_),
                  "%s/bench_multi.log", out_dir_);

    char cmdline[1536];
    int  n = std::snprintf(cmdline, sizeof(cmdline),
                "\"%s\" bench multi", cli_exe_);
    for (uint32_t i = 0u; i < n_runs_; ++i) {
        const char* flag = runs_[i].is_baseline ? "--baseline" : "--treated";
        n += std::snprintf(cmdline + n, sizeof(cmdline) - n,
                           " %s \"%s\"", flag, runs_[i].bench_csv_path);
        if (n < 0 || n >= static_cast<int>(sizeof(cmdline))) {
            std::snprintf(last_error_, sizeof(last_error_),
                          "bench multi cmdline too long");
            return false;
        }
    }

    // Redirect stdout/stderr to agg_log_ file.
    wchar_t wlog[260]{};
    for (int i = 0; agg_log_[i] && i < 259; ++i)
        wlog[i] = static_cast<wchar_t>(agg_log_[i]);

    SECURITY_ATTRIBUTES sa{};
    sa.nLength        = sizeof(sa);
    sa.bInheritHandle = TRUE;
    HANDLE hLog = CreateFileW(wlog,
        GENERIC_WRITE, FILE_SHARE_READ, &sa,
        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hLog == INVALID_HANDLE_VALUE) {
        std::snprintf(last_error_, sizeof(last_error_),
                      "open bench_multi log failed");
        return false;
    }

    wchar_t wcmd[1536]{};
    for (int i = 0; cmdline[i] && i < 1535; ++i)
        wcmd[i] = static_cast<wchar_t>(cmdline[i]);

    STARTUPINFOW si{};
    si.cb         = sizeof(si);
    si.dwFlags    = STARTF_USESHOWWINDOW | STARTF_USESTDHANDLES;
    si.wShowWindow= SW_HIDE;
    si.hStdOutput = hLog;
    si.hStdError  = hLog;
    si.hStdInput  = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION pi{};
    const BOOL ok = CreateProcessW(nullptr, wcmd, nullptr, nullptr,
                                    TRUE, CREATE_NO_WINDOW, nullptr,
                                    nullptr, &si, &pi);
    CloseHandle(hLog);
    if (!ok) {
        std::snprintf(last_error_, sizeof(last_error_),
                      "CreateProcess(bench multi) failed");
        return false;
    }
    CloseHandle(pi.hThread);
    agg_proc_ = pi.hProcess;
    return true;
#else
    return false;
#endif
}

inline void BenchRunner::poll_aggregate_() noexcept {
#ifdef _WIN32
    if (!agg_proc_) { state_ = State::Failed; return; }
    const DWORD w = WaitForSingleObject(agg_proc_, 0u);
    if (w == WAIT_OBJECT_0) {
        CloseHandle(agg_proc_);
        agg_proc_ = nullptr;
        state_    = State::Done;
    } else if (w != WAIT_TIMEOUT) {
        CloseHandle(agg_proc_);
        agg_proc_ = nullptr;
        std::snprintf(last_error_, sizeof(last_error_),
                      "WaitForSingleObject(aggregate) failed");
        state_ = State::Failed;
    }
#endif
}

} // namespace ayama::ui
// Made with my soul - Swately <3
