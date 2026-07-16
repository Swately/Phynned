// apps/ayama/ipc/src/AyamaAgentPublisher.cpp
// AyamaAgentPublisher — shared memory creation and per-tick publish.
//

#include <ayama/ipc/AyamaAgentPublisher.hpp>

#include <cstdio>
#include <cstring>
#include <phyriad/hal/MemoryOrder.hpp>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#else
#  include <sys/mman.h>
#  include <sys/stat.h>
#  include <fcntl.h>
#  include <unistd.h>
#endif

namespace ayama::ipc {

// ── open() ───────────────────────────────────────────────────────────────────
std::expected<void, phyriad::Error>
AyamaAgentPublisher::open(const char* shm_name, uint32_t agent_pid) noexcept {
    if (layout_) return {};   // already open — idempotent

#ifdef _WIN32
    // Build wide-character name for CreateFileMappingW.
    wchar_t wname[128]{};
    for (int i = 0; shm_name[i] && i < 127; ++i)
        wname[i] = static_cast<wchar_t>(shm_name[i]);

    file_handle_ = CreateFileMappingW(
        INVALID_HANDLE_VALUE,   // page-file-backed
        nullptr,                // default security
        PAGE_READWRITE,
        0u,
        static_cast<DWORD>(kAyamaShmSize),
        wname);

    if (!file_handle_) {
        std::fprintf(stderr,
            "[AyamaPublisher] CreateFileMappingW('%s') failed: error=%lu\n",
            shm_name, GetLastError());
        return std::unexpected(phyriad::Error{phyriad::ErrorCode::SystemError});
    }

    void* view = MapViewOfFile(file_handle_, FILE_MAP_ALL_ACCESS,
                               0u, 0u, kAyamaShmSize);
    if (!view) {
        std::fprintf(stderr,
            "[AyamaPublisher] MapViewOfFile failed: error=%lu\n",
            GetLastError());
        CloseHandle(file_handle_);
        file_handle_ = nullptr;
        return std::unexpected(phyriad::Error{phyriad::ErrorCode::SystemError});
    }

    map_handle_ = view;
    layout_     = static_cast<AyamaShmLayout*>(view);

#else  // POSIX
    // Sanitise name: POSIX requires a leading '/', no backslashes.
    shm_name_[0] = '/';
    int out_i = 1;
    for (int i = 0; shm_name[i] && out_i < 62; ++i) {
        shm_name_[out_i++] = (shm_name[i] == '\\') ? '_' : shm_name[i];
    }
    shm_name_[out_i] = '\0';

    shm_unlink(shm_name_);  // remove stale mapping from a previous crash

    shm_fd_ = shm_open(shm_name_, O_CREAT | O_RDWR | O_EXCL, 0600);
    if (shm_fd_ < 0) {
        std::fprintf(stderr, "[AyamaPublisher] shm_open('%s') failed\n", shm_name_);
        return std::unexpected(phyriad::Error{phyriad::ErrorCode::SystemError});
    }

    if (ftruncate(shm_fd_, static_cast<off_t>(kAyamaShmSize)) < 0) {
        ::close(shm_fd_);
        shm_fd_ = -1;
        shm_unlink(shm_name_);
        return std::unexpected(phyriad::Error{phyriad::ErrorCode::SystemError});
    }

    void* view = mmap(nullptr, kAyamaShmSize,
                      PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd_, 0);
    if (view == MAP_FAILED) {
        ::close(shm_fd_);
        shm_fd_      = -1;
        shm_name_[0] = '\0';
        shm_unlink(shm_name_);
        return std::unexpected(phyriad::Error{phyriad::ErrorCode::SystemError});
    }

    layout_ = static_cast<AyamaShmLayout*>(view);
#endif

    // ── Initialise the mapped region ──────────────────────────────────────
    // Zero header + state; leave targets/metrics/decisions/log as zeroed memory
    // (FileMapping / POSIX shm are zero-initialised by the OS).
    std::memset(layout_, 0, sizeof(AyamaShmHeader) + sizeof(AyamaStateHeader));

    layout_->header.version = kAyamaShmVersion;
    phyriad::hal::seq_store_release(layout_->header.agent_pid, agent_pid);
    phyriad::hal::seq_store_release(layout_->header.seq, 0u);

    // Write magic last — clients poll on magic to detect readiness.
    // Use a compiler barrier so the store is not reordered above the setup.
    std::atomic_thread_fence(std::memory_order_release);  // HAL: explicit fence — paired with seq_load_acquire / seq_store_release
    layout_->header.magic = kAyamaShmMagic;

    action_read_cursor_ = 0ull;

    std::fprintf(stdout,
        "[AyamaPublisher] SHM created: '%s'  size=%.1f KB  pid=%u\n",
        shm_name,
        static_cast<double>(kAyamaShmSize) / 1024.0,
        agent_pid);
    return {};
}

// ── publish() ────────────────────────────────────────────────────────────────
void AyamaAgentPublisher::publish(
    uint8_t                         privilege_level,
    uint8_t                         etw_active_flag,
    const observer::TargetProcess*  targets,
    uint32_t                        n_targets,
    const observer::TargetMetrics*  metrics,
    const policy::PolicyDecision*   decisions,
    uint32_t                        n_decisions,
    uint32_t                        n_active_actions,
    uint32_t                        total_migrations,
    float                           aggregate_pressure,
    uint64_t                        now_tsc,
    const action::ActionLogRing&    exec_log,
    uint32_t                        bad_count,
    uint8_t                         deep_idle,
    uint8_t                         watchdog_ok,
    uint32_t                        ccd_defense_count,
    float                           ccd_defense_cpu_pct
) noexcept {
    if (!layout_) return;

    // ── Drain new ActionLogEntries from executor → SHM ring ───────────────
    // Use write_cursor()/peek_at() — independent of AuditLog's cursor.
    // Done outside the seqlock because the SHM ring has its own atomics.
    const uint64_t exec_head = exec_log.write_cursor();
    while (action_read_cursor_ < exec_head) {
        // Guard against wrap-around overwrite by the executor.
        if (exec_head - action_read_cursor_ > action::kActionLogCap) {
            action_read_cursor_ = exec_head - action::kActionLogCap;
        }
        action::ActionLogEntry entry{};
        if (exec_log.peek_at(action_read_cursor_, entry)) {
            // SHM ring: try_push; if full the oldest entry is simply skipped.
            (void)layout_->action_log.try_push(entry);
        }
        ++action_read_cursor_;
    }

    // ── Seqlock write begin — seq becomes odd ─────────────────────────────
    shm_write_begin(&layout_->header);

    // ── Copy TargetProcess[] ──────────────────────────────────────────────
    if (n_targets > 0u && targets) {
        std::memcpy(layout_->targets, targets,
                    n_targets * sizeof(observer::TargetProcess));
    }

    // ── Copy TargetMetrics[] ──────────────────────────────────────────────
    if (n_targets > 0u && metrics) {
        std::memcpy(layout_->metrics, metrics,
                    n_targets * sizeof(observer::TargetMetrics));
    }

    // ── Copy PolicyDecision[] ─────────────────────────────────────────────
    if (n_decisions > 0u && decisions) {
        std::memcpy(layout_->decisions, decisions,
                    n_decisions * sizeof(policy::PolicyDecision));
    }

    // ── Write AyamaStateHeader in-place ───────────────────────────────────
    // (state is inside the seqlock window → consistent with targets/decisions)
    layout_->state.n_targets         = n_targets;
    layout_->state.n_decisions       = n_decisions;
    layout_->state.n_active_actions  = n_active_actions;
    layout_->state.agent_connected   = 1u;
    layout_->state.privilege_level   = privilege_level;
    layout_->state.etw_active        = etw_active_flag;
    // bench_phase written separately via set_bench_phase(); do not clear here.
    layout_->state.total_migrations   = total_migrations;
    layout_->state.aggregate_pressure = aggregate_pressure;
    layout_->state.last_publish_tsc   = now_tsc;
    layout_->state.bad_count          = bad_count;
    layout_->state.deep_idle          = deep_idle;
    layout_->state.watchdog_ok        = watchdog_ok;
    // CCD Load Defense telemetry
    layout_->state.ccd_defense_count   = ccd_defense_count;
    layout_->state.ccd_defense_cpu_pct = ccd_defense_cpu_pct;

    // ── Seqlock write end — seq becomes even ─────────────────────────────
    shm_write_end(&layout_->header);
}

// ── set_bench_phase() ─────────────────────────────────────────────────────────
void AyamaAgentPublisher::set_bench_phase(uint8_t phase) noexcept {
    if (!layout_) return;
    // Single-byte write is atomic on x86/ARM — no seqlock needed.
    layout_->state.bench_phase = phase;
}

// ── set_hw_classification() ──────────────────────────────────────────────────
void AyamaAgentPublisher::set_hw_classification(uint8_t cpu_class,
                                                uint8_t ccd_count,
                                                uint8_t p_core_count,
                                                uint8_t e_core_count) noexcept {
    if (!layout_) return;
    // 4 single-byte stores; each atomic on x86/ARM. Readers tolerate a brief
    // window where 1-3 of the 4 fields lag (they're all initialised to 0 in
    // open(), then transition once to the detected values at startup).
    layout_->state.cpu_class    = cpu_class;
    layout_->state.ccd_count    = ccd_count;
    layout_->state.p_core_count = p_core_count;
    layout_->state.e_core_count = e_core_count;
}

// ── set_policies_paused() ─────────────────────────────────────────────────────
void AyamaAgentPublisher::set_policies_paused(uint8_t paused) noexcept {
    if (!layout_) return;
    // Single-byte write is atomic on x86/ARM — no seqlock needed.
    // The UI's AyamaLogicNode reads this without holding any lock.
    layout_->state.policies_paused = paused;
}

// ── set_self_resources() ──────────────────────────────────────────────────────
void AyamaAgentPublisher::set_self_resources(float cpu_pct, float rss_mb) noexcept {
    if (!layout_) return;
    // Two 4-byte writes; each atomic on x86. The reader tolerates a one-
    // sample window where one of the two fields lags — they're displayed
    // independently and both lag by at most one SelfMonitor sample (~500 ms).
    layout_->state.self_cpu_pct = cpu_pct;
    layout_->state.self_rss_mb  = rss_mb;
}

// ── mark_disconnected() ───────────────────────────────────────────────────────
void AyamaAgentPublisher::mark_disconnected() noexcept {
    if (!layout_) return;
    shm_write_begin(&layout_->header);
    layout_->state.agent_connected = 0u;
    shm_write_end(&layout_->header);
}

// ── close() ──────────────────────────────────────────────────────────────────
void AyamaAgentPublisher::close() noexcept {
    if (!layout_) return;

    mark_disconnected();

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
    munmap(layout_, kAyamaShmSize);
    if (shm_fd_ >= 0) {
        ::close(shm_fd_);
        shm_fd_ = -1;
    }
    if (shm_name_[0] != '\0') {
        shm_unlink(shm_name_);
        shm_name_[0] = '\0';
    }
#endif

    layout_ = nullptr;
    std::fprintf(stdout, "[AyamaPublisher] SHM closed.\n");
}

} // namespace ayama::ipc
// Made with my soul - Swately <3
