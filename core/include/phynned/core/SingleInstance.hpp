// core/include/phynned/core/SingleInstance.hpp
// SingleInstance — named-mutex guard ensuring only one phynned-agent runs.
//
// On Windows: CreateMutexW with a named mutex. If another instance holds it,
//             acquire() returns false and the caller should exit.
// On Linux:   flock on a file in /tmp.
//
// Threading: call acquire() once from main thread before any other init.
// Resource:  1 handle (HANDLE or fd).
// Privilege: None.
//
#pragma once
#include <cstdio>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <sys/file.h>
#include <fcntl.h>
#include <unistd.h>
#endif

namespace phynned::core {

class SingleInstance {
public:
    SingleInstance() noexcept = default;

    ~SingleInstance() noexcept {
#ifdef _WIN32
        if (mutex_ != nullptr) {
            ReleaseMutex(mutex_);
            CloseHandle(mutex_);
        }
#else
        if (fd_ >= 0) {
            flock(fd_, LOCK_UN);
            close(fd_);
        }
#endif
    }

    SingleInstance(const SingleInstance&)            = delete;
    SingleInstance& operator=(const SingleInstance&) = delete;

    /// Try to become the sole running instance.
    /// Returns true on success, false if another instance is already running.
    [[nodiscard]] bool acquire() noexcept {
#ifdef _WIN32
        mutex_ = CreateMutexW(nullptr, FALSE, L"Local\\PhynnedAgentMutex.v1");
        if (mutex_ == nullptr) return false;
        if (GetLastError() == ERROR_ALREADY_EXISTS) {
            CloseHandle(mutex_);
            mutex_ = nullptr;
            return false;
        }
        return true;
#else
        fd_ = open("/tmp/phynned-agent.lock", O_CREAT | O_RDWR, 0666);
        if (fd_ < 0) return false;
        if (flock(fd_, LOCK_EX | LOCK_NB) != 0) {
            close(fd_);
            fd_ = -1;
            return false;
        }
        return true;
#endif
    }

    [[nodiscard]] bool held() const noexcept {
#ifdef _WIN32
        return mutex_ != nullptr;
#else
        return fd_ >= 0;
#endif
    }

private:
#ifdef _WIN32
    HANDLE mutex_{nullptr};
#else
    int fd_{-1};
#endif
};

} // namespace phynned::core
// Made with my soul - Swately <3
