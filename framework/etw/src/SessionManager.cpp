// framework/etw/src/SessionManager.cpp
// ETW SessionManager — Windows-only implementation.
//
// On non-Windows this file compiles to an empty translation unit (all
// functions are header-only no-op stubs in the header).
//

#include <phyriad/etw/SessionManager.hpp>

#ifdef _WIN32

#include <cstdio>
#include <cstring>
#include <memory>
#include <phyriad/hal/MemoryOrder.hpp>

// ─────────────────────────────────────────────────────────────────────────────
// Helpers: allocate EVENT_TRACE_PROPERTIES buffer
// ─────────────────────────────────────────────────────────────────────────────
namespace {

// The EVENT_TRACE_PROPERTIES struct must be followed immediately in memory by
// the session name string (LoggerName) and then an optional log-file name.
// Allocate a flat buffer large enough for the struct + the name + null.
struct PropsBuffer {
    static constexpr size_t kNameExtra = 256u; // max session name + null

    alignas(8) uint8_t buf[sizeof(EVENT_TRACE_PROPERTIES) + kNameExtra];

    EVENT_TRACE_PROPERTIES* props() noexcept {
        return reinterpret_cast<EVENT_TRACE_PROPERTIES*>(buf);
    }
    const EVENT_TRACE_PROPERTIES* props() const noexcept {
        return reinterpret_cast<const EVENT_TRACE_PROPERTIES*>(buf);
    }
};

void init_props(PropsBuffer& pb, const char* session_name,
                uint32_t buffer_size_kb, uint32_t max_buffers) noexcept {
    std::memset(pb.buf, 0, sizeof(pb.buf));
    EVENT_TRACE_PROPERTIES* p = pb.props();
    p->Wnode.BufferSize          = static_cast<ULONG>(sizeof(pb.buf));
    p->Wnode.Flags               = WNODE_FLAG_TRACED_GUID;
    p->Wnode.ClientContext        = 1; // QPC clock
    p->BufferSize                = buffer_size_kb;
    p->MaximumBuffers            = max_buffers;
    p->LogFileMode               = EVENT_TRACE_REAL_TIME_MODE;
    p->FlushTimer                = 1; // flush every second
    // LoggerName offset: immediately after the struct.
    p->LoggerNameOffset          = sizeof(EVENT_TRACE_PROPERTIES);
    // Copy name into the buffer after the struct.
    std::strncpy(reinterpret_cast<char*>(pb.buf + sizeof(EVENT_TRACE_PROPERTIES)),
                 session_name, PropsBuffer::kNameExtra - 1u);
}

/// Stop a session by name (best-effort, ignores errors).
void try_stop_session(const char* session_name) noexcept {
    PropsBuffer pb;
    init_props(pb, session_name, 64u, 32u);
    ControlTraceA(0, session_name, pb.props(), EVENT_TRACE_CONTROL_STOP);
}

} // anonymous namespace

// ─────────────────────────────────────────────────────────────────────────────
namespace phyriad::etw {

// ── SessionManager::start() ──────────────────────────────────────────────────
std::expected<void, phyriad::Error>
SessionManager::start(const char*                   session_name,
                      std::span<const ProviderSpec> providers,
                      uint32_t                      buffer_size_kb,
                      uint32_t                      max_buffers) noexcept
{
    if (!session_name) return std::unexpected(phyriad::Error{phyriad::ErrorCode::InvalidArgument});

    // Stop any pre-existing session with this name (idempotent restart).
    try_stop_session(session_name);

    // Save name for stop() / start_consumer().
    std::strncpy(session_name_, session_name, sizeof(session_name_) - 1u);
    session_name_[sizeof(session_name_) - 1u] = '\0';

    PropsBuffer pb;
    init_props(pb, session_name_, buffer_size_kb, max_buffers);

    TRACEHANDLE handle = 0;
    const ULONG rc = StartTraceA(&handle, session_name_, pb.props());
    if (rc != ERROR_SUCCESS) {
        return std::unexpected(phyriad::Error{
            rc == ERROR_ACCESS_DENIED ? phyriad::ErrorCode::PermissionDenied
                                      : phyriad::ErrorCode::IoError});
    }

    session_handle_ = handle;

    // Enable each requested provider.
    for (const auto& spec : providers) {
        GUID guid_copy = spec.guid;
        EnableTraceEx2(
            session_handle_,
            &guid_copy,
            EVENT_CONTROL_CODE_ENABLE_PROVIDER,
            spec.level,
            spec.match_any,
            spec.match_all,
            0,         // timeout: 0 = asynchronous
            nullptr);  // ENABLE_TRACE_PARAMETERS: default
        // Individual provider enable failure is non-fatal — log but continue.
        // (Some providers require specific Windows versions or privileges.)
    }

    return {};
}

// ── SessionManager::stop() ──────────────────────────────────────────────────
void SessionManager::stop() noexcept {
    // Stop consumer thread first so ProcessTrace() can return.
    stop_consumer();

    if (session_handle_ != 0) {
        PropsBuffer pb;
        init_props(pb, session_name_, 64u, 32u);
        ControlTraceA(session_handle_, session_name_,
                      pb.props(), EVENT_TRACE_CONTROL_STOP);
        session_handle_ = 0;
    }
}

// ── Static thunk — called by ETW on the consumer thread ─────────────────────
// The thunk uses the user-data pointer stored in the EVENT_TRACE_LOGFILEW to
// reach the SessionManager instance. We store `this` in the LogFileW.Context.
void NTAPI SessionManager::event_record_thunk(EVENT_RECORD* rec) noexcept {
    if (!rec) return;
    auto* self = static_cast<SessionManager*>(rec->UserContext);
    if (!self || !self->cb_) return;
    self->cb_(*rec, self->user_ctx_);
    hal::stat_fetch_add_relaxed(self->events_processed_, 1u);
}

// ── Consumer thread entry point ─────────────────────────────────────────────
DWORD WINAPI SessionManager::consumer_thread_entry(LPVOID arg) noexcept {
    auto* self = static_cast<SessionManager*>(arg);
    if (!self) return 1u;

    // Build the real-time logfile descriptor.
    EVENT_TRACE_LOGFILEW logfile{};
    // Convert session name to wide for the W variant.
    wchar_t wname[128]{};
    MultiByteToWideChar(CP_UTF8, 0, self->session_name_, -1, wname, 128);
    logfile.LoggerName         = wname;
    logfile.ProcessTraceMode   = PROCESS_TRACE_MODE_REAL_TIME |
                                  PROCESS_TRACE_MODE_EVENT_RECORD;
    logfile.EventRecordCallback = SessionManager::event_record_thunk;
    logfile.Context             = self; // passed to EventRecord::UserContext

    TRACEHANDLE trace = OpenTraceW(&logfile);
    // INVALID_PROCESSTRACE_HANDLE = (TRACEHANDLE)(ULONG_PTR)INVALID_HANDLE_VALUE
    if (trace == (TRACEHANDLE)(ULONG_PTR)INVALID_HANDLE_VALUE) return 2u;

    self->consumer_handle_ = trace;

    // ProcessTrace blocks until the session is stopped via ControlTrace.
    ProcessTrace(&trace, 1u, nullptr, nullptr);

    CloseTrace(trace);
    self->consumer_handle_ = 0;
    return 0u;
}

// ── SessionManager::start_consumer() ────────────────────────────────────────
std::expected<void, phyriad::Error>
SessionManager::start_consumer(EventCallback cb, void* user_ctx) noexcept {
    if (session_handle_ == 0)
        return std::unexpected(phyriad::Error{phyriad::ErrorCode::Unavailable});
    if (consumer_thread_)
        return std::unexpected(phyriad::Error{phyriad::ErrorCode::AlreadyConnected});

    cb_       = cb;
    user_ctx_ = user_ctx;
    hal::stat_store_relaxed(events_processed_, 0u);
    hal::stat_store_relaxed(stop_flag_, false);

    consumer_thread_ = CreateThread(
        nullptr, 0,
        SessionManager::consumer_thread_entry,
        this, 0, nullptr);

    if (!consumer_thread_) {
        cb_       = nullptr;
        user_ctx_ = nullptr;
        return std::unexpected(phyriad::Error{phyriad::ErrorCode::IoError});
    }

    return {};
}

// ── SessionManager::stop_consumer() ─────────────────────────────────────────
void SessionManager::stop_consumer() noexcept {
    if (!consumer_thread_) return;

    // Signal ProcessTrace to return by stopping the real-time trace.
    // We use a secondary ControlTrace call that flushes remaining buffers
    // and returns TRACE_ERROR_END_OF_FILE to ProcessTrace.
    if (consumer_handle_ != 0) {
        PropsBuffer pb;
        init_props(pb, session_name_, 64u, 32u);
        ControlTraceA(session_handle_, session_name_,
                      pb.props(), EVENT_TRACE_CONTROL_FLUSH);
    }
    // A brief ControlTrace STOP on the consumer handle unblocks ProcessTrace.
    // In practice ProcessTrace will return when the session is stopped.
    // The consumer thread detects this and exits.

    WaitForSingleObject(consumer_thread_, 5000u); // wait up to 5 s
    CloseHandle(consumer_thread_);
    consumer_thread_ = nullptr;
    cb_              = nullptr;
    user_ctx_        = nullptr;
}

} // namespace phyriad::etw

#endif // _WIN32
// Made with my soul - Swately <3
