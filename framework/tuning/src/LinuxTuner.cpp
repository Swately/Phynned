// framework/tuning/src/LinuxTuner.cpp
// LinuxTuner snapshot-aware implementation (THP / governor / C-states / IRQ).
//
// All writes are recorded in TuningSnapshot so rollback restores original values.
//
#ifndef _WIN32   // entire file is Linux-only

#include <phyriad/tuning/LinuxTuner.hpp>
#include <phyriad/tuning/PrivilegeCheck.hpp>
#include <phyriad/tuning/TuningProvider.hpp>
#include <phyriad/tuning/TuningSnapshot.hpp>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <filesystem>
#include <string>
#include <unistd.h>
#include <vector>

namespace phyriad::tuning {

// ── Low-level sysfs helpers ──────────────────────────────────────────────────

static bool read_sysfs_(char const* path, char* buf, std::size_t max_len) noexcept {
    const int fd = ::open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
    const ssize_t n = ::read(fd, buf, max_len - 1u);
    ::close(fd);
    if (n <= 0) return false;
    buf[n] = '\0';
    if (buf[n - 1] == '\n') buf[n - 1] = '\0';
    return true;
}

static bool write_sysfs_(char const* path, char const* value) noexcept {
    const int fd = ::open(path, O_WRONLY | O_CLOEXEC);
    if (fd < 0) return false;
    const std::size_t len  = std::strlen(value);
    const ssize_t     sent = ::write(fd, value, len);
    ::close(fd);
    return sent == static_cast<ssize_t>(len);
}

// Parse "/sys/devices/system/cpu/online" range format: "0-3,8-11".
static std::vector<uint32_t> read_online_cpus_() noexcept {
    std::vector<uint32_t> cpus;
    char buf[256]{};
    if (!read_sysfs_("/sys/devices/system/cpu/online", buf, sizeof(buf)))
        return cpus;

    char* p = buf;
    while (*p) {
        uint32_t start = static_cast<uint32_t>(std::strtoul(p, &p, 10));
        if (*p == '-') {
            ++p;
            uint32_t end = static_cast<uint32_t>(std::strtoul(p, &p, 10));
            for (uint32_t i = start; i <= end; ++i) cpus.push_back(i);
        } else {
            cpus.push_back(start);
        }
        if (*p == ',') ++p;
        else break;
    }
    return cpus;
}

// Returns all numeric IRQ entries under /proc/irq/.
static std::vector<uint32_t> read_irq_numbers_() noexcept {
    std::vector<uint32_t> irqs;
    DIR* d = ::opendir("/proc/irq");
    if (!d) return irqs;
    struct dirent* e;
    while ((e = ::readdir(d)) != nullptr) {
        if (e->d_name[0] < '0' || e->d_name[0] > '9') continue;
        char* end{};
        const uint32_t n = static_cast<uint32_t>(std::strtoul(e->d_name, &end, 10));
        if (*end == '\0') irqs.push_back(n);
    }
    ::closedir(d);
    return irqs;
}

// ── Per-feature apply helpers ────────────────────────────────────────────────

namespace {

std::expected<void, phyriad::Error>
apply_thp_(TuningSnapshot& snap, bool dry_run) noexcept {
    static constexpr char kPath[] =
        "/sys/kernel/mm/transparent_hugepage/defrag";
    static constexpr char kNew[]  = "defer+madvise";

    char old_val[64]{};
    if (!read_sysfs_(kPath, old_val, sizeof(old_val))) return {};  // kernel may lack THP — skip
    if (std::strcmp(old_val, kNew) == 0) return {};

    if (dry_run) {
        std::fprintf(stderr, "[dry-run] %s: '%s' -> '%s'\n",
                     kPath, old_val, kNew);
        return {};
    }
    if (!write_sysfs_(kPath, kNew)) {
        return std::unexpected(phyriad::Error{
            .code = phyriad::ErrorCode::IoError,
            .source_node_id = 0, .timestamp_ns = 0});
    }
    snap.add_record(TuningOp::SysfsWrite, kPath, old_val, kNew);
    return {};
}

std::expected<void, phyriad::Error>
apply_governor_(TuningSnapshot& snap, bool dry_run) noexcept {
    static constexpr char kNew[] = "performance";
    const auto cpus = read_online_cpus_();

    for (uint32_t cpu : cpus) {
        char path[128]{};
        std::snprintf(path, sizeof(path),
            "/sys/devices/system/cpu/cpu%u/cpufreq/scaling_governor", cpu);
        if (!std::filesystem::exists(path)) continue;

        char old_val[64]{};
        if (!read_sysfs_(path, old_val, sizeof(old_val))) continue;
        if (std::strcmp(old_val, kNew) == 0) continue;

        if (dry_run) {
            std::fprintf(stderr, "[dry-run] %s: '%s' -> '%s'\n",
                         path, old_val, kNew);
            continue;
        }
        if (!write_sysfs_(path, kNew)) continue;  // non-fatal: some CPUs lack cpufreq
        snap.add_record(TuningOp::SysfsWrite, path, old_val, kNew);
    }
    return {};
}

std::expected<void, phyriad::Error>
disable_cstates_(TuningSnapshot& snap, bool dry_run) noexcept {
    static constexpr char kNew[] = "1";   // disable state
    const auto cpus = read_online_cpus_();

    for (uint32_t cpu : cpus) {
        // state0 = C0 (active, cannot disable); state1+ = C1, C1E, C2, ...
        for (uint32_t state = 1u; state < 16u; ++state) {
            char path[160]{};
            std::snprintf(path, sizeof(path),
                "/sys/devices/system/cpu/cpu%u/cpuidle/state%u/disable",
                cpu, state);
            if (!std::filesystem::exists(path)) break;

            char old_val[8]{};
            if (!read_sysfs_(path, old_val, sizeof(old_val))) continue;
            if (std::strcmp(old_val, kNew) == 0) continue;

            if (dry_run) {
                std::fprintf(stderr,
                    "[dry-run] %s: '%s' -> '%s'\n", path, old_val, kNew);
                continue;
            }
            if (!write_sysfs_(path, kNew)) continue;
            snap.add_record(TuningOp::SysfsWrite, path, old_val, kNew);
        }
    }
    return {};
}

std::expected<void, phyriad::Error>
reroute_irqs_(TuningSnapshot& snap, bool dry_run, uint32_t cpu_mask) noexcept {
    const auto irqs = read_irq_numbers_();
    char new_mask[16]{};
    std::snprintf(new_mask, sizeof(new_mask), "%x", cpu_mask);

    for (uint32_t irq : irqs) {
        char path[64]{};
        std::snprintf(path, sizeof(path), "/proc/irq/%u/smp_affinity", irq);

        char old_val[64]{};
        if (!read_sysfs_(path, old_val, sizeof(old_val))) continue;
        if (std::strcmp(old_val, new_mask) == 0) continue;

        if (dry_run) {
            std::fprintf(stderr, "[dry-run] %s: '%s' -> '%s'\n",
                         path, old_val, new_mask);
            continue;
        }
        if (!write_sysfs_(path, new_mask)) continue;
        snap.add_record(TuningOp::SysfsWrite, path, old_val, new_mask);
    }
    return {};
}

} // anonymous namespace

// ── LinuxTuner ITuningProvider implementation ────────────────────────────────

std::expected<void, phyriad::Error>
LinuxTuner::apply_full(TuningSnapshot& snap,
                       TuningConfig const& cfg,
                       bool dry_run) noexcept
{
    // Privilege check: all sysfs writes require root/CAP_SYS_ADMIN. Without
    // privilege, individual operations will fail silently (returned best-effort).
    const auto info = PrivilegeCheck::probe();
    (void)info;  // hint only — actual gating is per-op via fd open() success

    if (cfg.apply_hugepages) {
        if (auto r = apply_thp_(snap, dry_run); !r) return r;
    }
    if (cfg.apply_governor) {
        if (auto r = apply_governor_(snap, dry_run); !r) return r;
    }
    if (cfg.disable_cstates) {
        if (auto r = disable_cstates_(snap, dry_run); !r) return r;
    }
    if (cfg.reroute_irqs) {
        if (auto r = reroute_irqs_(snap, dry_run, cfg.irq_target_cpu_mask); !r)
            return r;
    }
    return {};
}

bool LinuxTuner::verify_snapshot(TuningSnapshot const& snap) const noexcept {
    for (auto const& r : snap.records()) {
        if (r.op != TuningOp::SysfsWrite) continue;
        char cur[64]{};
        if (!read_sysfs_(r.target, cur, sizeof(cur))) return false;
        if (std::strcmp(cur, r.new_value) != 0) return false;
    }
    return true;
}

void LinuxTuner::reapply_snapshot(TuningSnapshot const& snap) noexcept {
    for (auto const& r : snap.records()) {
        if (r.op != TuningOp::SysfsWrite) continue;
        write_sysfs_(r.target, r.new_value);
    }
}

} // namespace phyriad::tuning

#endif // !_WIN32
// Made with my soul - Swately <3
