// framework/hal/include/phyriad/hal/WakeEvent.hpp
// Cross-platform auto-reset wakeable event for thread sleep/wake patterns.
//
// Provides:
//   phyriad::hal::WakeEvent — create() → wait(ms) / signal() / reset()
//
// Semantics:
//   Auto-reset: a single wait() consumes one signal(). Multiple signals
//   before wait() still consume only one wait().
//
// Platforms:
//   Windows: CreateEventW (manual-reset=FALSE → auto-reset), WaitForSingleObject.
//   Linux:   eventfd(EFD_CLOEXEC | EFD_NONBLOCK) + poll().
//   Other:   Fallback using std::mutex + std::condition_variable.
//
// Threading:
//   Any thread may call signal().
//   Only ONE thread should call wait() at a time (SPSC pattern).
//   reset() is rarely needed; call from the waiting thread only.
//
// Usage pattern (AgentRuntime main loop):
//   auto evt = *phyriad::hal::WakeEvent::create();
//   // ...
//   evt.wait(sleep_ms);   // blocks up to sleep_ms or until signal()
//   // ...
//   evt.signal();         // from stop() or watchdog
//

#pragma once

#include <cstdint>
#include <optional>

// ── Platform-specific forward declarations ────────────────────────────────────
#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#elif defined(__linux__)
#  include <sys/eventfd.h>
#  include <poll.h>
#  include <unistd.h>
#else
#  include <mutex>
#  include <condition_variable>
#endif

namespace phyriad::hal {

class WakeEvent {
public:
    ~WakeEvent() noexcept;

    // Non-copyable, movable.
    WakeEvent(const WakeEvent&)            = delete;
    WakeEvent& operator=(const WakeEvent&) = delete;
    WakeEvent(WakeEvent&&) noexcept;
    WakeEvent& operator=(WakeEvent&&) noexcept;

    /// Create an auto-reset wake event.
    /// Returns std::nullopt on OS failure (extremely rare; treat as fatal).
    [[nodiscard]] static std::optional<WakeEvent> create() noexcept;

    /// Block until signaled or `timeout_ms` elapses.
    ///
    /// Returns true  → woken via signal().
    /// Returns false → timeout elapsed without signal.
    ///
    /// `timeout_ms == ~0u` (UINT32_MAX) waits indefinitely.
    [[nodiscard]] bool wait(uint32_t timeout_ms) noexcept;

    /// Signal the event. If a thread is blocked in wait(), it wakes immediately.
    /// If no thread is waiting, the next wait() returns immediately (auto-reset).
    /// Idempotent: multiple signals before wait() still consume only one wait().
    void signal() noexcept;

    /// Reset to non-signaled state without consuming a wait().
    /// Rarely needed. Call only from the consumer thread.
    void reset() noexcept;

    [[nodiscard]] bool is_valid() const noexcept;

private:
    WakeEvent() noexcept = default;

#if defined(_WIN32)
    HANDLE handle_{nullptr};

#elif defined(__linux__)
    int fd_{-1};

#else
    // Portable fallback
    struct FallbackImpl {
        std::mutex              mtx;
        std::condition_variable cv;
        bool                    signaled{false};
    };
    FallbackImpl* impl_{nullptr};
#endif
};

// ── Inline implementation ─────────────────────────────────────────────────────

inline WakeEvent::~WakeEvent() noexcept {
#if defined(_WIN32)
    if (handle_) {
        CloseHandle(handle_);
        handle_ = nullptr;
    }
#elif defined(__linux__)
    if (fd_ >= 0) {
        close(fd_);
        fd_ = -1;
    }
#else
    delete impl_;
    impl_ = nullptr;
#endif
}

inline WakeEvent::WakeEvent(WakeEvent&& other) noexcept {
#if defined(_WIN32)
    handle_       = other.handle_;
    other.handle_ = nullptr;
#elif defined(__linux__)
    fd_       = other.fd_;
    other.fd_ = -1;
#else
    impl_       = other.impl_;
    other.impl_ = nullptr;
#endif
}

inline WakeEvent& WakeEvent::operator=(WakeEvent&& other) noexcept {
    if (this != &other) {
        this->~WakeEvent();
#if defined(_WIN32)
        handle_       = other.handle_;
        other.handle_ = nullptr;
#elif defined(__linux__)
        fd_       = other.fd_;
        other.fd_ = -1;
#else
        impl_       = other.impl_;
        other.impl_ = nullptr;
#endif
    }
    return *this;
}

inline std::optional<WakeEvent> WakeEvent::create() noexcept {
    WakeEvent ev;
#if defined(_WIN32)
    // FALSE = auto-reset, FALSE = initially non-signaled.
    ev.handle_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!ev.handle_) return std::nullopt;
#elif defined(__linux__)
    ev.fd_ = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (ev.fd_ < 0) return std::nullopt;
#else
    ev.impl_ = new (std::nothrow) FallbackImpl{};
    if (!ev.impl_) return std::nullopt;
#endif
    return ev;
}

inline bool WakeEvent::wait(uint32_t timeout_ms) noexcept {
#if defined(_WIN32)
    if (!handle_) return false;
    const DWORD ms = (timeout_ms == ~0u) ? INFINITE : static_cast<DWORD>(timeout_ms);
    return WaitForSingleObject(handle_, ms) == WAIT_OBJECT_0;

#elif defined(__linux__)
    if (fd_ < 0) return false;
    struct pollfd pfd{fd_, POLLIN, 0};
    const int timeout_i = (timeout_ms == ~0u) ? -1 : static_cast<int>(timeout_ms);
    const int r = poll(&pfd, 1, timeout_i);
    if (r > 0 && (pfd.revents & POLLIN)) {
        uint64_t v = 0;
        (void)read(fd_, &v, sizeof(v));   // consume → auto-reset
        return true;
    }
    return false;

#else
    if (!impl_) return false;
    std::unique_lock<std::mutex> lk(impl_->mtx);
    if (timeout_ms == ~0u) {
        impl_->cv.wait(lk, [this] { return impl_->signaled; });
    } else {
        impl_->cv.wait_for(lk,
            std::chrono::milliseconds(timeout_ms),
            [this] { return impl_->signaled; });
    }
    const bool was_signaled = impl_->signaled;
    impl_->signaled = false;  // auto-reset
    return was_signaled;
#endif
}

inline void WakeEvent::signal() noexcept {
#if defined(_WIN32)
    if (handle_) SetEvent(handle_);
#elif defined(__linux__)
    if (fd_ >= 0) {
        const uint64_t v = 1u;
        (void)write(fd_, &v, sizeof(v));
    }
#else
    if (!impl_) return;
    {
        std::lock_guard<std::mutex> lk(impl_->mtx);
        impl_->signaled = true;
    }
    impl_->cv.notify_one();
#endif
}

inline void WakeEvent::reset() noexcept {
#if defined(_WIN32)
    if (handle_) ResetEvent(handle_);
#elif defined(__linux__)
    if (fd_ >= 0) {
        uint64_t v = 0;
        // Drain any pending value (non-blocking).
        while (read(fd_, &v, sizeof(v)) > 0) {}
    }
#else
    if (!impl_) return;
    std::lock_guard<std::mutex> lk(impl_->mtx);
    impl_->signaled = false;
#endif
}

inline bool WakeEvent::is_valid() const noexcept {
#if defined(_WIN32)
    return handle_ != nullptr;
#elif defined(__linux__)
    return fd_ >= 0;
#else
    return impl_ != nullptr;
#endif
}

} // namespace phyriad::hal
// Made with my soul - Swately <3
