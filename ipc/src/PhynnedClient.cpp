// ipc/src/PhynnedClient.cpp
// PhynnedClient — shared memory connect/disconnect.
//

#include <phynned/ipc/PhynnedClient.hpp>
#include <cstdio>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <phyriad/hal/MemoryOrder.hpp>
#endif

namespace phynned::ipc {

std::expected<void, phyriad::Error>
PhynnedClient::connect(const char* shm_name) noexcept {
    if (layout_) return {};  // already connected

#ifdef _WIN32
    // Convert to wide
    wchar_t wname[128]{};
    for (int i = 0; shm_name[i] && i < 127; ++i)
        wname[i] = static_cast<wchar_t>(shm_name[i]);

    // need FILE_MAP_WRITE for the command slot
    // (UI→agent IPC). Read-only data (targets, metrics, action_log) is still
    // only READ via the same mapping; the agent enforces seqlock protection
    // for the read-mostly region. Clients are trusted (single-instance
    // typical use case).
    file_handle_ = OpenFileMappingW(FILE_MAP_READ | FILE_MAP_WRITE,
                                     FALSE, wname);
    if (!file_handle_) {
        // Fallback: try read-only if write access denied (e.g. mismatched
        // versions or restricted access).
        file_handle_ = OpenFileMappingW(FILE_MAP_READ, FALSE, wname);
        if (!file_handle_) {
            return std::unexpected(phyriad::Error{phyriad::ErrorCode::Unavailable});
        }
        ro_only_ = true;
    }

    const DWORD access = ro_only_ ? FILE_MAP_READ
                                  : (FILE_MAP_READ | FILE_MAP_WRITE);
    void* view = MapViewOfFile(file_handle_, access, 0, 0, kPhynnedShmSize);
    if (!view) {
        CloseHandle(file_handle_);
        file_handle_ = nullptr;
        return std::unexpected(phyriad::Error{phyriad::ErrorCode::SystemError});
    }
    map_handle_ = view;
    layout_ = static_cast<PhynnedShmLayout*>(view);
#else
    // map RW so the command slot can be written.
    shm_fd_ = shm_open(shm_name, O_RDWR, 0600);
    if (shm_fd_ < 0) {
        // Fallback to read-only
        shm_fd_ = shm_open(shm_name, O_RDONLY, 0600);
        if (shm_fd_ < 0) {
            return std::unexpected(phyriad::Error{phyriad::ErrorCode::Unavailable});
        }
        ro_only_ = true;
    }
    const int prot = ro_only_ ? PROT_READ : (PROT_READ | PROT_WRITE);
    void* view = mmap(nullptr, kPhynnedShmSize, prot, MAP_SHARED, shm_fd_, 0);
    if (view == MAP_FAILED) {
        close(shm_fd_);
        shm_fd_ = -1;
        return std::unexpected(phyriad::Error{phyriad::ErrorCode::SystemError});
    }
    layout_ = static_cast<PhynnedShmLayout*>(view);
#endif

    // Validate magic
    if (layout_->header.magic != kPhynnedShmMagic) {
        disconnect();
        return std::unexpected(phyriad::Error{phyriad::ErrorCode::InvalidArgument});
    }

    std::fprintf(stdout,
        "[PhynnedClient] Connected to agent (pid=%u)\n",
        phyriad::hal::seq_load_acquire(layout_->header.agent_pid));
    return {};
}

void PhynnedClient::disconnect() noexcept {
#ifdef _WIN32
    if (map_handle_) {
        UnmapViewOfFile(map_handle_);
        map_handle_ = nullptr;
    }
    if (file_handle_) {
        CloseHandle(file_handle_);
        file_handle_ = nullptr;
    }
#else
    if (layout_) {
        munmap(layout_, kPhynnedShmSize);
    }
    if (shm_fd_ >= 0) {
        close(shm_fd_);
        shm_fd_ = -1;
    }
#endif
    layout_ = nullptr;
}

} // namespace phynned::ipc
// Made with my soul - Swately <3
