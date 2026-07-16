// apps/ayama/tools/ayama-agent/main.cpp
// ayama-agent — Ayama background optimizer daemon.
//
// Entry point for the background agent process. Responsibilities:
//   1. Enforce single-instance (named mutex).
//   2. Set process working set limits (anti-parasitic).
//   3. Register signal/console handlers for clean shutdown.
//   4. Instantiate and run AgentRuntime.
//
// Usage:
//   ayama-agent.exe [--verbose] [--no-selfpin] [--require-admin]
//
// Exit codes:
//   0 — clean shutdown
//   1 — already running
//   2 — admin required but not available
//   3 — init failure
//

#include <ayama/core/AgentRuntime.hpp>
#include <ayama/core/Diag.hpp>          // crash-diagnostic phase markers
#include <ayama/core/SingleInstance.hpp>
#include <phyriad/tuning/WorkingSet.hpp>

#include <atomic>
#include <cstdio>
#include <cstring>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

// ── Globals for signal handler ─────────────────────────────────────────────
static ayama::core::AgentRuntime* g_runtime = nullptr;

// ── Unhandled exception filter (Windows) ──────────────────────────────────
// When the agent crashes (access violation, divide-by-zero, stack overflow,
// etc.) Windows would normally
// pop up Werfault and the process exits with no useful trace. Wiring an
// UnhandledExceptionFilter lets us print:
//   - the exception code (decoded)
//   - the faulting address
//   - the LAST phase reached in AgentRuntime::run() (PhaseClassify, etc.)
//   - the last apply pid/rule/tid (so we know which decision triggered it)
//
// fflush(stderr) is paranoid — stderr is unbuffered by default but stdout
// (where most logs go) is set _IONBF in main(). After printing, return
// EXCEPTION_EXECUTE_HANDLER so the process terminates cleanly (no crash
// dialog).
#ifdef _WIN32
static const char* exception_code_name(DWORD code) noexcept {
    switch (code) {
        case EXCEPTION_ACCESS_VIOLATION:        return "ACCESS_VIOLATION";
        case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:   return "ARRAY_BOUNDS_EXCEEDED";
        case EXCEPTION_DATATYPE_MISALIGNMENT:   return "DATATYPE_MISALIGNMENT";
        case EXCEPTION_FLT_DIVIDE_BY_ZERO:      return "FLT_DIVIDE_BY_ZERO";
        case EXCEPTION_ILLEGAL_INSTRUCTION:     return "ILLEGAL_INSTRUCTION";
        case EXCEPTION_IN_PAGE_ERROR:           return "IN_PAGE_ERROR";
        case EXCEPTION_INT_DIVIDE_BY_ZERO:      return "INT_DIVIDE_BY_ZERO";
        case EXCEPTION_INT_OVERFLOW:            return "INT_OVERFLOW";
        case EXCEPTION_PRIV_INSTRUCTION:        return "PRIV_INSTRUCTION";
        case EXCEPTION_STACK_OVERFLOW:          return "STACK_OVERFLOW";
        case EXCEPTION_NONCONTINUABLE_EXCEPTION:return "NONCONTINUABLE";
        case 0xE06D7363u:                       return "CXX_EXCEPTION"; // MSVC C++ EH
        default:                                return "?";
    }
}

static LONG WINAPI ayama_unhandled_filter(EXCEPTION_POINTERS* eps) noexcept {
    if (!eps || !eps->ExceptionRecord) {
        std::fprintf(stderr, "[Ayama][CRASH] (no exception record)\n");
        std::fflush(stderr);
        return EXCEPTION_EXECUTE_HANDLER;
    }
    const auto* er = eps->ExceptionRecord;
    const DWORD  code = er->ExceptionCode;
    const void*  addr = er->ExceptionAddress;
    const uint32_t phase = ayama::core::diag::g_last_phase
                            .load(std::memory_order_relaxed);  // HAL: relaxed — secondary atomic in compound op
    const uint32_t tick  = ayama::core::diag::g_last_tick
                            .load(std::memory_order_relaxed);  // HAL: relaxed — secondary atomic in compound op
    const uint32_t ap_pid  = ayama::core::diag::g_last_apply_pid
                                .load(std::memory_order_relaxed);  // HAL: relaxed — secondary atomic in compound op
    const uint32_t ap_rule = ayama::core::diag::g_last_apply_rule
                                .load(std::memory_order_relaxed);  // HAL: relaxed — secondary atomic in compound op
    const uint32_t ap_tid  = ayama::core::diag::g_last_apply_tid
                                .load(std::memory_order_relaxed);  // HAL: relaxed — secondary atomic in compound op

    std::fprintf(stderr,
        "\n[Ayama][CRASH] code=0x%08lx (%s) addr=%p\n"
        "[Ayama][CRASH] tick=%u last_phase=%u (%s)\n"
        "[Ayama][CRASH] last_apply: pid=%u rule=%u tid=%u\n",
        static_cast<unsigned long>(code),
        exception_code_name(code),
        addr,
        tick, phase, ayama::core::diag::phase_name(phase),
        ap_pid, ap_rule, ap_tid);

    // Access violation: extra info — read vs write + faulting address.
    if (code == EXCEPTION_ACCESS_VIOLATION && er->NumberParameters >= 2u) {
        const auto rw_kind = er->ExceptionInformation[0];
        const auto bad_va  = er->ExceptionInformation[1];
        const char* kind =
            (rw_kind == 0) ? "READ" :
            (rw_kind == 1) ? "WRITE" :
            (rw_kind == 8) ? "DEP/NX" : "?";
        std::fprintf(stderr,
            "[Ayama][CRASH] AV detail: %s at 0x%llx\n",
            kind, static_cast<unsigned long long>(bad_va));
    }
    std::fflush(stderr);
    std::fflush(stdout);
    return EXCEPTION_EXECUTE_HANDLER;  // terminate process
}
#endif

// ── Signal / console event handler ────────────────────────────────────────
#ifdef _WIN32
static BOOL WINAPI console_ctrl_handler(DWORD ctrl_type) noexcept {
    if (ctrl_type == CTRL_C_EVENT || ctrl_type == CTRL_BREAK_EVENT ||
        ctrl_type == CTRL_CLOSE_EVENT || ctrl_type == CTRL_SHUTDOWN_EVENT) {
        if (g_runtime) {
            std::fprintf(stdout, "\n[Ayama] Shutdown signal received — stopping...\n");
            g_runtime->stop();
        }
        return TRUE;
    }
    return FALSE;
}
#else
#include <signal.h>
static void signal_handler(int /*sig*/) noexcept {
    if (g_runtime) g_runtime->stop();
}
#endif

// ── Working set limits (anti-parasitic) ───────────────────────────────────
// phyriad::tuning::set_self_working_set (FR-19) — cross-platform, RAII-safe.
// Non-fatal: restricted environments (CI, AppContainer) deny the hint; agent
// degrades gracefully. AgentRuntime::start() also calls this; the duplication
// ensures the limit is applied as early as possible in the process lifetime.
static void set_memory_limits() noexcept {
    constexpr uint64_t kMinBytes = 16ull * 1024u * 1024u;   // 16 MB
    constexpr uint64_t kMaxBytes = 50ull * 1024u * 1024u;   // 50 MB
    (void)phyriad::tuning::set_self_working_set(kMinBytes, kMaxBytes);
}

// ── CLI argument parsing ───────────────────────────────────────────────────
struct CliArgs {
    bool verbose       {false};
    bool no_selfpin    {false};
    bool require_admin {false};
    // --start-active opts out of the safe-default paused state
    // (see AgentRuntime::start() for rationale).
    bool start_active  {false};
};

static CliArgs parse_args(int argc, char* argv[]) noexcept {
    CliArgs a{};
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--verbose")       == 0) a.verbose       = true;
        if (std::strcmp(argv[i], "--no-selfpin")    == 0) a.no_selfpin    = true;
        if (std::strcmp(argv[i], "--require-admin") == 0) a.require_admin = true;
        if (std::strcmp(argv[i], "--start-active")  == 0) a.start_active  = true;
    }
    return a;
}

// ── main ──────────────────────────────────────────────────────────────────
int main(int argc, char* argv[]) {
    // Disable stdout buffering — without this, when stdout is redirected to
    // a file/pipe (e.g. PowerShell `Tee-Object`), C stdio uses block buffering
    // (~4 KB chunks). Diagnostic logs from the classifier and auto-discovery
    // accumulate in the buffer and never reach the file until the agent exits
    // OR the buffer fills, which can take minutes. Setting _IONBF forces every
    // fprintf(stdout, ...) to flush immediately — same behavior as when stdout
    // is a console terminal. stderr is already unbuffered by default.
    std::setvbuf(stdout, nullptr, _IONBF, 0);

#ifdef _WIN32
    // Install unhandled exception filter BEFORE any heavy initialisation so
    // crashes during start() are also reported (e.g. ETW init access faults).
    SetUnhandledExceptionFilter(ayama_unhandled_filter);
#endif

    const CliArgs cli = parse_args(argc, argv);

    std::fprintf(stdout,
        "Ayama Agent v0.1.0\n"
        "Built on Phyriad Framework 1.0.0\n"
        "-----------------------------------------------\n");

    // ── Single instance guard ─────────────────────────────────────────────
    ayama::core::SingleInstance inst;
    if (!inst.acquire()) {
        std::fprintf(stderr,
            "[Ayama] Another instance is already running. Exiting.\n");
        return 1;
    }

    // ── Memory limits ─────────────────────────────────────────────────────
    set_memory_limits();

    // ── Signal handlers ───────────────────────────────────────────────────
#ifdef _WIN32
    SetConsoleCtrlHandler(console_ctrl_handler, TRUE);
#else
    signal(SIGINT,  signal_handler);
    signal(SIGTERM, signal_handler);
#endif

    // ── Configure runtime ─────────────────────────────────────────────────
    ayama::core::AgentConfig cfg{};
    cfg.require_admin         = cli.require_admin;
    cfg.self_pin_to_slow_cores = !cli.no_selfpin;
    cfg.enable_shm_publish    = true;
    cfg.start_active          = cli.start_active;

    // ── Create and start runtime ──────────────────────────────────────────
    ayama::core::AgentRuntime runtime(cfg);
    g_runtime = &runtime;

    auto start_result = runtime.start();
    if (!start_result) {
        std::fprintf(stderr,
            "[Ayama] Failed to start: error code %d\n",
            static_cast<int>(start_result.error().code));
        g_runtime = nullptr;
        return (start_result.error().code == phyriad::ErrorCode::PermissionDenied) ? 2 : 3;
    }

    std::fprintf(stdout,
        "[Ayama] Running. Press Ctrl+C to stop.\n"
        "         Admin: %s | ETW: %s | Self-pin: %s\n",
        runtime.is_admin()   ? "yes" : "no",
        runtime.etw_active() ? "yes" : "no",
        cli.no_selfpin       ? "disabled" : "enabled");

    // ── Blocking main loop ────────────────────────────────────────────────
    runtime.run();

    std::fprintf(stdout,
        "[Ayama] Clean shutdown. Ticks: %u\n",
        runtime.tick_count());

    g_runtime = nullptr;
    return 0;
}
// Made with my soul - Swately <3
