// framework/etw/include/phyriad/etw/SessionManager.hpp
// ETW (Event Tracing for Windows) session lifecycle manager.
//
// Windows implementation:
//   Wraps StartTrace / EnableTraceEx2 / OpenTrace / ProcessTrace / CloseTrace
//   and ControlTrace into a clean RAII object with a background consumer thread.
//
// Non-Windows:
//   Provides header-only no-op stubs so cross-platform code can include this
//   header without #ifdef guards. All start() calls return Unavailable.
//
// Thread model:
//   start()          — initialises the ETW kernel session (caller thread).
//   start_consumer() — spawns a background thread that calls ProcessTrace().
//                      ProcessTrace() blocks until stop() sends ControlTrace STOP.
//   stop()           — sends ControlTrace STOP, waits for consumer thread to exit.
//
// Event callback contract:
//   EventCallback must be lightweight — copy data and return.
//   Heavy processing goes in the caller's tick loop.
//   Called from the consumer thread; must be noexcept.
//
// Privilege:
//   Creating a new real-time ETW session requires admin on Windows 10+.
//   start() returns PermissionDenied if StartTrace fails with ACCESS_DENIED.
//
// Usage example (Windows):
//   phyriad::etw::SessionManager session;
//   auto r = session.start("MySession",
//       {{phyriad::etw::providers::kKernelProcess, TRACE_LEVEL_INFORMATION,
//         0x10, 0}});
//   if (!r) { /* handle error */ }
//   session.start_consumer(my_callback, &my_ctx);
//   // ... agent tick loop ...
//   session.stop();
//

#pragma once
#include <phyriad/schema/Error.hpp>
#include <phyriad/hal/MemoryOrder.hpp>
#include <atomic>
#include <cstdint>
#include <expected>
#include <span>

#ifdef _WIN32

// ── Windows-only definitions ──────────────────────────────────────────────────
// Include evntrace.h via windows.h (must come before evntrace.h on MSVC).
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef INITGUID
#    define INITGUID
#  endif
#  include <windows.h>
#  include <evntrace.h>
#  include <evntcons.h>

namespace phyriad::etw {

// ── ProviderSpec ─────────────────────────────────────────────────────────────
/// Specifies a single ETW provider to enable on a session.
struct ProviderSpec {
    GUID      guid;       ///< Provider GUID
    UCHAR     level;      ///< TRACE_LEVEL_INFORMATION, _WARNING, _ERROR, etc.
    ULONGLONG match_any;  ///< Keyword mask: events with ANY of these keywords
    ULONGLONG match_all;  ///< Additional ALL-keyword filter (usually 0)
};

// ── EventCallback ─────────────────────────────────────────────────────────────
/// Called from the consumer thread for each ETW event.
/// MUST be lightweight — copy data and return immediately.
using EventCallback = void (*)(const EVENT_RECORD& rec, void* user_ctx) noexcept;

// ── SessionManager ───────────────────────────────────────────────────────────
/// One ETW real-time session with a background consumer thread.
/// Non-copyable, non-movable (owns OS handles and a thread).
class SessionManager {
public:
    SessionManager() noexcept = default;
    ~SessionManager() noexcept { stop(); }

    SessionManager(const SessionManager&)            = delete;
    SessionManager& operator=(const SessionManager&) = delete;
    SessionManager(SessionManager&&)                 = delete;
    SessionManager& operator=(SessionManager&&)      = delete;

    // ── Lifecycle ─────────────────────────────────────────────────────────

    /// Start a real-time ETW session with the given name and providers.
    ///
    /// If a session with the same name already exists, it is stopped first
    /// (avoids ERROR_ALREADY_EXISTS on restart without agent shutdown).
    ///
    /// `buffer_size_kb`  — per-buffer size in KB (default 64 KB).
    /// `max_buffers`     — maximum number of event buffers (default 32).
    ///
    /// Returns PermissionDenied if StartTrace fails due to lack of admin.
    [[nodiscard]] std::expected<void, phyriad::Error>
    start(const char*                   session_name,
          std::span<const ProviderSpec> providers,
          uint32_t                      buffer_size_kb = 64u,
          uint32_t                      max_buffers    = 32u) noexcept;

    /// Stop the session (sends ControlTrace STOP). Safe to call if not started.
    /// Blocks until the consumer thread (if any) has exited.
    void stop() noexcept;

    /// Returns true after a successful start() and before stop().
    [[nodiscard]] bool is_running() const noexcept {
        return session_handle_ != 0;
    }

    // ── Consumer thread ───────────────────────────────────────────────────

    /// Start the background consumer thread that calls `cb(record, user_ctx)`
    /// for each event. Must be called after start().
    ///
    /// The thread runs ProcessTrace() which blocks until stop() is called.
    /// Calling start_consumer() again before stop_consumer() is an error.
    [[nodiscard]] std::expected<void, phyriad::Error>
    start_consumer(EventCallback cb, void* user_ctx) noexcept;

    /// Stop the consumer thread without closing the ETW session.
    /// After this call events continue to buffer in kernel-mode buffers.
    void stop_consumer() noexcept;

    // ── Diagnostics ───────────────────────────────────────────────────────

    /// Total events delivered to the EventCallback since start_consumer().
    [[nodiscard]] uint64_t events_processed() const noexcept {
        return hal::stat_load_relaxed(events_processed_);
    }

private:
    TRACEHANDLE            session_handle_  {0};
    TRACEHANDLE            consumer_handle_ {0};  // 0 = not open
    HANDLE                 consumer_thread_ {nullptr};
    std::atomic<bool>      stop_flag_       {false};
    std::atomic<uint64_t>  events_processed_{0u};
    EventCallback          cb_              {nullptr};
    void*                  user_ctx_        {nullptr};
    char                   session_name_    [128]{};

    // ETW callback thunk — static so it has C-linkage compatibility.
    static void NTAPI event_record_thunk(EVENT_RECORD* rec) noexcept;

    // Consumer thread entry point.
    static DWORD WINAPI consumer_thread_entry(LPVOID arg) noexcept;
};

// ── Common provider GUIDs ─────────────────────────────────────────────────────
/// Centralised GUID database. Use these in ProviderSpec rather than copy-pasting
/// magic bytes in application code.
namespace providers {

/// Microsoft-Windows-Kernel-Process (process/thread/image load events).
inline constexpr GUID kKernelProcess{
    0x0268a8b6u, 0x74fdu, 0x4302u,
    {0x9bu, 0x4au, 0x6eu, 0xa0u, 0xfbu, 0xb1u, 0x9du, 0x9eu}};

/// Microsoft-Windows-Kernel-Thread (thread create/delete/rundown events).
inline constexpr GUID kKernelThread{
    0x3d6fa8d1u, 0xfe05u, 0x11d0u,
    {0x9du, 0xdau, 0x00u, 0xc0u, 0x4fu, 0xd7u, 0xbau, 0x7cu}};

/// Microsoft-Windows-Kernel-Dispatcher (context-switch events, level ≥ 4).
/// Requires Windows 10 RS3+ or Server 2019+.
inline constexpr GUID kKernelContextSwitch{
    0xdef2fe46u, 0x7bd6u, 0x4b80u,
    {0xbdu, 0x94u, 0xf5u, 0x7fu, 0xe2u, 0x0du, 0x0cu, 0xe3u}};

/// Microsoft-Windows-Kernel-Memory (working set / page fault events).
inline constexpr GUID kKernelMemory{
    0x1f008f13u, 0x8a01u, 0x4d04u,
    {0x83u, 0x46u, 0x12u, 0x22u, 0x1au, 0x8au, 0x10u, 0x7bu}};

} // namespace providers

} // namespace phyriad::etw

#else // ── Non-Windows: header-only no-op stubs ────────────────────────────────

#include <cstddef>

namespace phyriad::etw {

struct ProviderSpec {}; // empty on non-Windows

using EventCallback = void (*)(const void*, void*) noexcept;

/// No-op stub. All methods return Unavailable on non-Windows builds.
class SessionManager {
public:
    SessionManager() noexcept = default;
    ~SessionManager() noexcept = default;

    SessionManager(const SessionManager&)            = delete;
    SessionManager& operator=(const SessionManager&) = delete;

    template <typename... Args>
    [[nodiscard]] std::expected<void, phyriad::Error>
    start(const char*, Args&&...) noexcept {
        return std::unexpected(
            phyriad::Error{phyriad::ErrorCode::Unavailable});
    }
    void stop() noexcept {}

    [[nodiscard]] bool is_running() const noexcept { return false; }

    [[nodiscard]] std::expected<void, phyriad::Error>
    start_consumer(EventCallback, void*) noexcept {
        return std::unexpected(
            phyriad::Error{phyriad::ErrorCode::Unavailable});
    }
    void stop_consumer() noexcept {}

    [[nodiscard]] uint64_t events_processed() const noexcept { return 0u; }
};

namespace providers {
// No provider GUIDs on non-Windows (GUID type is not defined).
} // namespace providers

} // namespace phyriad::etw

#endif // _WIN32
// Made with my soul - Swately <3
