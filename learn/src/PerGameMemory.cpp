// apps/ayama/learn/src/PerGameMemory.cpp
// PerGameMemory — memory.toml load/save + per-game lookup.
//
// Hardware ID format: "<vendor>-<class>-<ncores>c"
//   Examples:
//     "amd-x3d-16c"     — AMD Ryzen with 3D V-Cache, 16 physical cores
//     "intel-hybrid-24c" — Intel Alder/Raptor Lake with P+E cores, 24 physical
//     "amd-generic-8c"   — AMD without X3D, 8 cores
//     "intel-generic-6c" — Intel without E-cores, 6 cores
//     "arm-12c"          — ARM64
//
#include <ayama/learn/PerGameMemory.hpp>
#include <phyriad/topology/HardwareTopology.hpp>

#include <cstdio>
#include <cstring>
#include <cctype>
#include <cstdlib>
#include <ctime>
#include <algorithm>

// ── Platform helpers ──────────────────────────────────────────────────────────
#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#  include <shlobj.h>
#  include <sys/stat.h>
#  include <direct.h>
#else
#  include <sys/stat.h>
#  include <pwd.h>
#  include <unistd.h>
#endif

namespace ayama::learn {

// ── Local helpers (anonymous namespace) ───────────────────────────────────────
namespace {

[[nodiscard]] static const char* ltrim(const char* p) noexcept
{
    while (*p && (unsigned char)*p <= ' ') ++p;
    return p;
}

static void safe_strncpy(char* dst, const char* src, uint32_t max_len) noexcept
{
    if (max_len == 0u) return;
    std::strncpy(dst, src, max_len - 1u);
    dst[max_len - 1u] = '\0';
}

[[nodiscard]] static bool extract_string(const char* rhs,
                                          char* out,
                                          uint32_t out_max) noexcept
{
    const char* p = ltrim(rhs);
    if (*p != '"') return false;
    ++p;
    const char* end = std::strchr(p, '"');
    if (!end) return false;
    const uint32_t len      = static_cast<uint32_t>(end - p);
    const uint32_t copy_len = (len < out_max - 1u) ? len : out_max - 1u;
    std::memcpy(out, p, copy_len);
    out[copy_len] = '\0';
    return true;
}

[[nodiscard]] static bool extract_bool(const char* rhs) noexcept
{
    return std::strncmp(ltrim(rhs), "true", 4) == 0;
}

[[nodiscard]] static float extract_float(const char* rhs) noexcept
{
    char* end = nullptr;
    return std::strtof(ltrim(rhs), &end);
}

[[nodiscard]] static uint64_t extract_uint(const char* rhs) noexcept
{
    const char* p = ltrim(rhs);
    if (!std::isdigit((unsigned char)*p)) return 0ull;
    char* end = nullptr;
    return std::strtoull(p, &end, 10);
}

[[nodiscard]] static bool get_config_dir(char* out, uint32_t max_len) noexcept
{
#ifdef _WIN32
    char appdata[MAX_PATH]{};
    if (::GetEnvironmentVariableA("LOCALAPPDATA", appdata, MAX_PATH) == 0) {
        if (FAILED(::SHGetFolderPathA(nullptr, CSIDL_LOCAL_APPDATA,
                                       nullptr, 0, appdata))) {
            return false;
        }
    }
    const int n = std::snprintf(out, max_len, "%s\\Ayama\\", appdata);
    return (n > 0 && static_cast<uint32_t>(n) < max_len);
#else
    const char* home = ::getenv("HOME");
    if (!home) {
        const struct passwd* pw = ::getpwuid(::getuid());
        if (!pw) return false;
        home = pw->pw_dir;
    }
    const int n = std::snprintf(out, max_len, "%s/.config/ayama/", home);
    return (n > 0 && static_cast<uint32_t>(n) < max_len);
#endif
}

static void ensure_dir(const char* path) noexcept
{
#ifdef _WIN32
    const DWORD attr = ::GetFileAttributesA(path);
    if (attr == INVALID_FILE_ATTRIBUTES)
        ::CreateDirectoryA(path, nullptr);
#else
    struct stat st{};
    if (::stat(path, &st) != 0)
        ::mkdir(path, 0755);
#endif
}

} // namespace

// ── Forward declaration for load_bad_list ─────────────────────────────────────
// load_bad_list is defined after load() but called from within it.
// Forward declaration avoids a re-ordering of the definitions.
static void load_bad_list(const char* resolved, BadEntry* bad,
                          uint32_t* n_bad, uint32_t max_bad) noexcept;

// ── generate_hardware_id ──────────────────────────────────────────────────────
void PerGameMemory::generate_hardware_id() noexcept
{
    // Determine CPU class from topology singleton.
    const auto vcache = phyriad::hw::v_cache_cores();
    const auto ecores = phyriad::hw::e_cores();
    const auto all    = phyriad::hw::enumerate_cores();

    // Count unique physical cores (non-SMT siblings).
    uint32_t phys_count = 0u;
    for (const auto& c : all) {
        if (!c.is_ht_sibling) ++phys_count;
    }
    if (phys_count == 0u) {
        // Fallback: use logical core count
        phys_count = static_cast<uint32_t>(all.size());
    }

    // Classify
    if (!vcache.empty()) {
        // AMD 3D V-Cache CPU
        std::snprintf(hardware_id_, sizeof(hardware_id_),
                      "amd-x3d-%uc", phys_count);
    } else if (!ecores.empty()) {
        // Intel hybrid (Alder Lake, Raptor Lake, Meteor Lake, …)
        std::snprintf(hardware_id_, sizeof(hardware_id_),
                      "intel-hybrid-%uc", phys_count);
    } else if (!all.empty() && all[0].ccx_id > 0u) {
        // Multi-CCX AMD without X3D (e.g., Threadripper, Zen 2 multi-CCX)
        std::snprintf(hardware_id_, sizeof(hardware_id_),
                      "amd-multi-ccx-%uc", phys_count);
    } else {
        // Determine rough vendor from CPU brand string (CPUID EAX=0 — "GenuineIntel" or "AuthenticAMD")
#if defined(_MSC_VER)
        int cpuid_data[4]{};
        __cpuidex(cpuid_data, 0, 0);
#elif defined(__GNUC__) || defined(__clang__)
        int cpuid_data[4]{};
#  if defined(__i386__) || defined(__x86_64__)
        __asm__ volatile(
            "cpuid"
            : "=a"(cpuid_data[0]), "=b"(cpuid_data[1]),
              "=c"(cpuid_data[2]), "=d"(cpuid_data[3])
            : "a"(0), "c"(0)
        );
#  endif
#else
        int cpuid_data[4]{};
#endif
        char vendor[13]{};
        std::memcpy(vendor + 0, &cpuid_data[1], 4); // EBX
        std::memcpy(vendor + 4, &cpuid_data[3], 4); // EDX
        std::memcpy(vendor + 8, &cpuid_data[2], 4); // ECX
        vendor[12] = '\0';

        if (std::strstr(vendor, "AMD") != nullptr) {
            std::snprintf(hardware_id_, sizeof(hardware_id_),
                          "amd-generic-%uc", phys_count);
        } else if (std::strstr(vendor, "Intel") != nullptr) {
            std::snprintf(hardware_id_, sizeof(hardware_id_),
                          "intel-generic-%uc", phys_count);
        } else {
            // ARM or unknown fallback
            std::snprintf(hardware_id_, sizeof(hardware_id_),
                          "cpu-%uc", phys_count);
        }
    }
}

// ── resolve_path ──────────────────────────────────────────────────────────────
bool PerGameMemory::resolve_path(char* out, uint32_t max_len,
                                  const char* override_path) const noexcept
{
    if (override_path) {
        safe_strncpy(out, override_path, max_len);
        return true;
    }
    char dir[512]{};
    if (!get_config_dir(dir, sizeof(dir))) return false;
    const int n = std::snprintf(out, max_len, "%smemory.toml", dir);
    return (n > 0 && static_cast<uint32_t>(n) < max_len);
}

// ── exe_match ────────────────────────────────────────────────────────────────
bool PerGameMemory::exe_match(const char* a, const char* b) const noexcept
{
#ifdef _WIN32
    // Case-insensitive on Windows (filesystem is case-insensitive by default)
    return ::_stricmp(a, b) == 0;
#else
    return std::strcmp(a, b) == 0;
#endif
}

// ── find ─────────────────────────────────────────────────────────────────────
const LearnedEntry* PerGameMemory::find(const char* exe) const noexcept
{
    if (!exe) return nullptr;
    for (uint32_t i = 0u; i < n_entries_; ++i) {
        if (exe_match(entries_[i].exe, exe) &&
            std::strcmp(entries_[i].hardware_id, hardware_id_) == 0) {
            return &entries_[i];
        }
    }
    return nullptr;
}

LearnedEntry* PerGameMemory::find(const char* exe) noexcept
{
    return const_cast<LearnedEntry*>(
        static_cast<const PerGameMemory*>(this)->find(exe));
}

// ── upsert ───────────────────────────────────────────────────────────────────
void PerGameMemory::upsert(const LearnedEntry& e) noexcept
{
    // Update existing
    for (uint32_t i = 0u; i < n_entries_; ++i) {
        if (exe_match(entries_[i].exe, e.exe) &&
            std::strcmp(entries_[i].hardware_id, e.hardware_id) == 0) {
            entries_[i] = e;
            return;
        }
    }
    // Insert new
    if (n_entries_ < kMaxEntries) {
        entries_[n_entries_++] = e;
    }
    // Silently discard if full.
}

// ── remove ───────────────────────────────────────────────────────────────────
void PerGameMemory::remove(const char* exe) noexcept
{
    if (!exe) return;
    for (uint32_t i = 0u; i < n_entries_; ++i) {
        if (exe_match(entries_[i].exe, exe) &&
            std::strcmp(entries_[i].hardware_id, hardware_id_) == 0) {
            // Swap with last
            entries_[i] = entries_[--n_entries_];
            return;
        }
    }
}

// ── load ─────────────────────────────────────────────────────────────────────
std::expected<void, phyriad::Error> PerGameMemory::load(const char* path) noexcept
{
    char resolved[512]{};
    if (!resolve_path(resolved, sizeof(resolved), path)) {
        return {};  // Non-fatal: use empty table.
    }

    std::FILE* f = std::fopen(resolved, "r");
    if (!f) return {};  // First-run: no file yet.

    char     line[256]{};
    bool     in_learned = false;
    LearnedEntry cur{};

    auto commit = [&]() noexcept {
        if (!in_learned || cur.exe[0] == '\0') return;
        upsert(cur);
        cur = LearnedEntry{};
        in_learned = false;
    };

    while (std::fgets(line, sizeof(line), f)) {
        // Strip newline
        const uint32_t llen = static_cast<uint32_t>(std::strlen(line));
        if (llen > 0u && (line[llen-1u] == '\n' || line[llen-1u] == '\r'))
            line[llen-1u] = '\0';

        const char* p = ltrim(line);
        if (*p == '\0' || *p == '#') continue;

        if (std::strncmp(p, "[[learned]]", 11) == 0) {
            commit();
            in_learned = true;
            continue;
        }

        // Root-level key=value
        const char* eq = std::strchr(p, '=');
        if (!eq) continue;

        // Extract key
        char key[64]{};
        {
            const char* ke = eq;
            while (ke > p && (unsigned char)*(ke-1) <= ' ') --ke;
            const uint32_t kl = static_cast<uint32_t>(ke - p);
            std::memcpy(key, p, std::min(kl, uint32_t{sizeof(key)-1u}));
            key[std::min(kl, uint32_t{sizeof(key)-1u})] = '\0';
        }
        const char* rhs = eq + 1u;

        if (!in_learned) {
            if (std::strcmp(key, "version") == 0) {
                version_ = static_cast<uint32_t>(extract_uint(rhs));
            } else if (std::strcmp(key, "hardware_id") == 0) {
                // Don't overwrite our detected hardware_id —
                // just read it for informational purposes.
                (void)rhs;
            }
        } else {
            if (std::strcmp(key, "exe") == 0) {
                extract_string(rhs, cur.exe, sizeof(cur.exe));
            } else if (std::strcmp(key, "best_action") == 0) {
                extract_string(rhs, cur.best_action, sizeof(cur.best_action));
            } else if (std::strcmp(key, "improvement_pct") == 0) {
                cur.improvement_pct = extract_float(rhs);
            } else if (std::strcmp(key, "sample_count") == 0) {
                cur.sample_count = static_cast<uint32_t>(extract_uint(rhs));
            } else if (std::strcmp(key, "last_validated") == 0) {
                extract_string(rhs, cur.last_validated, sizeof(cur.last_validated));
            } else if (std::strcmp(key, "user_locked") == 0) {
                cur.user_locked = extract_bool(rhs);
            } else if (std::strcmp(key, "best_core_mask") == 0) {
                const char* vp = ltrim(rhs);
                char* end = nullptr;
                if (vp[0] == '0' && (vp[1] == 'x' || vp[1] == 'X')) {
                    cur.best_core_mask = std::strtoull(vp, &end, 16);
                } else {
                    cur.best_core_mask = std::strtoull(vp, &end, 10);
                }
            } else if (std::strcmp(key, "hardware_id") == 0) {
                // Per-entry hardware_id (in case memory has entries for
                // different hardware from a shared file).
                extract_string(rhs, cur.hardware_id, sizeof(cur.hardware_id));
            }
        }
    }
    commit();
    std::fclose(f);

    // Also load bad-list entries from the same file.
    load_bad_list(resolved, bad_, &n_bad_, kMaxBadEntries);

    return {};
}

// ── load — parse [[bad]] sections too ─────────────────────────────────────────
// (load() already reads [[learned]]; we add [[bad]] parsing here by
//  re-opening the file and looking for [[bad]] headers)
// NOTE: load() above handles [[learned]]. This companion scan reads [[bad]].
static void load_bad_list(const char* resolved, BadEntry* bad, uint32_t* n_bad,
                          uint32_t max_bad) noexcept
{
    std::FILE* f = std::fopen(resolved, "r");
    if (!f) return;

    char line[256]{};
    bool in_bad = false;
    BadEntry cur{};

    auto commit = [&]() noexcept {
        if (!in_bad || cur.exe[0] == '\0') return;
        if (*n_bad < max_bad) {
            bad[(*n_bad)++] = cur;
        }
        cur = BadEntry{};
        in_bad = false;
    };

    while (std::fgets(line, sizeof(line), f)) {
        const uint32_t llen = static_cast<uint32_t>(std::strlen(line));
        if (llen > 0u && (line[llen-1u] == '\n' || line[llen-1u] == '\r'))
            line[llen-1u] = '\0';

        const char* p = ltrim(line);
        if (*p == '\0' || *p == '#') continue;

        if (std::strncmp(p, "[[bad]]", 7) == 0) {
            commit();
            in_bad = true;
            continue;
        }
        if (std::strncmp(p, "[[learned]]", 11) == 0) {
            commit();
            in_bad = false;
            continue;
        }

        if (!in_bad) continue;

        const char* eq = std::strchr(p, '=');
        if (!eq) continue;

        char key[64]{};
        {
            const char* ke = eq;
            while (ke > p && (unsigned char)*(ke-1) <= ' ') --ke;
            const uint32_t kl = static_cast<uint32_t>(ke - p);
            std::memcpy(key, p, std::min(kl, uint32_t{sizeof(key)-1u}));
        }
        const char* rhs = eq + 1u;

        if (std::strcmp(key, "exe") == 0) {
            extract_string(rhs, cur.exe, sizeof(cur.exe));
        } else if (std::strcmp(key, "reason") == 0) {
            extract_string(rhs, cur.reason, sizeof(cur.reason));
        } else if (std::strcmp(key, "last_attempted") == 0) {
            extract_string(rhs, cur.last_attempted, sizeof(cur.last_attempted));
        }
    }
    commit();
    std::fclose(f);
}

// ── save (atomic: .tmp → .bak → real) ────────────────────────────────────────
std::expected<void, phyriad::Error> PerGameMemory::save(const char* path) noexcept
{
    char resolved[512]{};
    if (!resolve_path(resolved, sizeof(resolved), path)) {
        return std::unexpected(phyriad::Error{phyriad::ErrorCode::IoError, 0u, 0u});
    }

    // Ensure directory exists.
    {
        char dir[512]{};
        if (get_config_dir(dir, sizeof(dir))) {
            char dir_no_sep[512]{};
            safe_strncpy(dir_no_sep, dir, sizeof(dir_no_sep));
            const uint32_t dl = static_cast<uint32_t>(std::strlen(dir_no_sep));
            if (dl > 0u && (dir_no_sep[dl-1u] == '/' || dir_no_sep[dl-1u] == '\\'))
                dir_no_sep[dl-1u] = '\0';
            ensure_dir(dir_no_sep);
        }
    }

    // Build temp and backup paths.
    char tmp_path[544]{};
    char bak_path[544]{};
    std::snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", resolved);
    std::snprintf(bak_path, sizeof(bak_path), "%s.bak", resolved);

    // ── Step 1: Write to .tmp ─────────────────────────────────────────────
    std::FILE* f = std::fopen(tmp_path, "w");
    if (!f) {
        return std::unexpected(phyriad::Error{phyriad::ErrorCode::IoError, 0u, 0u});
    }

    std::fprintf(f,
        "# Ayama per-game learned optimisations\n"
        "# Auto-generated by ayama-agent — edit while agent is stopped.\n"
        "# user_locked = true prevents re-evaluation for that entry.\n"
        "\n"
        "version     = %u\n"
        "hardware_id = \"%s\"\n"
        "\n",
        version_, hardware_id_);

    for (uint32_t i = 0u; i < n_entries_; ++i) {
        const LearnedEntry& e = entries_[i];
        if (e.exe[0] == '\0') continue;

        std::fprintf(f, "[[learned]]\n");
        std::fprintf(f, "exe             = \"%s\"\n", e.exe);
        std::fprintf(f, "hardware_id     = \"%s\"\n", e.hardware_id);
        std::fprintf(f, "best_action     = \"%s\"\n", e.best_action);
        if (e.best_core_mask != 0ull) {
            std::fprintf(f, "best_core_mask  = 0x%llx\n",
                         static_cast<unsigned long long>(e.best_core_mask));
        } else {
            std::fprintf(f, "best_core_mask  = 0\n");
        }
        std::fprintf(f, "improvement_pct = %.2f\n", static_cast<double>(e.improvement_pct));
        std::fprintf(f, "sample_count    = %u\n", e.sample_count);
        std::fprintf(f, "last_validated  = \"%s\"\n", e.last_validated);
        std::fprintf(f, "user_locked     = %s\n\n", e.user_locked ? "true" : "false");
    }

    // Write bad-list.
    if (n_bad_ > 0u) {
        std::fprintf(f,
            "# Bad-list: Ayama detected regression for these exes.\n"
            "# Remove an entry to allow Ayama to retry.\n\n");
        for (uint32_t i = 0u; i < n_bad_; ++i) {
            const BadEntry& b = bad_[i];
            if (b.exe[0] == '\0') continue;
            std::fprintf(f, "[[bad]]\n");
            std::fprintf(f, "exe            = \"%s\"\n", b.exe);
            std::fprintf(f, "reason         = \"%s\"\n",
                         b.reason[0] ? b.reason : "regression_detected");
            if (b.last_attempted[0]) {
                std::fprintf(f, "last_attempted = \"%s\"\n", b.last_attempted);
            }
            std::fprintf(f, "\n");
        }
    }

    std::fflush(f);
    std::fclose(f);

    // ── Step 2: Atomically rename old → .bak, tmp → real ─────────────────
#ifdef _WIN32
    // On Windows, MoveFileExW is the atomic rename primitive.
    // Remove old .bak first (MoveFileExW REPLACE_EXISTING won't remove .bak first
    // if destination already exists on older Windows).
    (void)DeleteFileA(bak_path);
    (void)MoveFileExA(resolved, bak_path, 0u);  // old → .bak (may fail on first run)
    if (!MoveFileExA(tmp_path, resolved, MOVEFILE_REPLACE_EXISTING)) {
        // Rename failed — try to restore.
        (void)MoveFileExA(bak_path, resolved, 0u);
        return std::unexpected(phyriad::Error{phyriad::ErrorCode::IoError, 0u, 0u});
    }
#else
    // POSIX: rename() is atomic within same filesystem.
    (void)::rename(resolved, bak_path);
    if (::rename(tmp_path, resolved) != 0) {
        (void)::rename(bak_path, resolved);  // restore
        return std::unexpected(phyriad::Error{phyriad::ErrorCode::IoError, 0u, 0u});
    }
#endif

    return {};
}

// ── Bad-list methods ──────────────────────────────────────────────────────────
void PerGameMemory::mark_bad(const char* exe, const char* reason) noexcept
{
    if (!exe || exe[0] == '\0') return;

    // Update existing entry if present.
    for (uint32_t i = 0u; i < n_bad_; ++i) {
        if (exe_match(bad_[i].exe, exe)) {
            if (reason) {
                safe_strncpy(bad_[i].reason, reason, sizeof(bad_[i].reason));
            }
            return;
        }
    }

    // Insert new entry.
    if (n_bad_ >= kMaxBadEntries) return;  // table full
    BadEntry& b = bad_[n_bad_++];
    safe_strncpy(b.exe,    exe,    sizeof(b.exe));
    safe_strncpy(b.reason, reason ? reason : "regression_detected",
                 sizeof(b.reason));
    b.last_attempted[0] = '\0';

    std::fprintf(stdout,
        "[Ayama][Learn] Marked %s as bad: %s\n",
        exe, b.reason);
}

bool PerGameMemory::is_bad(const char* exe) const noexcept
{
    if (!exe || exe[0] == '\0') return false;
    for (uint32_t i = 0u; i < n_bad_; ++i) {
        if (exe_match(bad_[i].exe, exe)) return true;
    }
    return false;
}

void PerGameMemory::clear_bad(const char* exe) noexcept
{
    if (!exe) return;
    for (uint32_t i = 0u; i < n_bad_; ++i) {
        if (exe_match(bad_[i].exe, exe)) {
            bad_[i] = bad_[--n_bad_];
            return;
        }
    }
}

void PerGameMemory::clear_all_bad() noexcept
{
    n_bad_ = 0u;
}

// ── Re-validation strategy (§8.3) ────────────────────────────────────────────

/// Parse "YYYY-MM-DD" prefix from an ISO-8601 string into (year, month, day).
/// Returns false if the timestamp is empty or malformed.
static bool parse_iso_date(const char* ts,
                            int* out_year,
                            int* out_month,
                            int* out_day) noexcept
{
    if (!ts || ts[0] == '\0') return false;
    // Expect at least "YYYY-MM-DD" (10 chars)
    if (ts[4] != '-' || ts[7] != '-') return false;
    char* end = nullptr;
    const long y = std::strtol(ts,      &end, 10); if (!end || *end != '-') return false;
    const long m = std::strtol(ts + 5u, &end, 10); if (!end || *end != '-') return false;
    const long d = std::strtol(ts + 8u, &end, 10);
    if (y < 2020 || y > 2100) return false;
    if (m < 1    || m > 12)   return false;
    if (d < 1    || d > 31)   return false;
    *out_year  = static_cast<int>(y);
    *out_month = static_cast<int>(m);
    *out_day   = static_cast<int>(d);
    return true;
}

/// Compute a simple "day number" from a Gregorian date (not astronomical).
/// Accurate enough for a 30-day age comparison (off by ≤ 2 days near leap years).
static int gregorian_day(int y, int m, int d) noexcept
{
    // Knuth/Dershowitz simplified ordinal
    const int a = (14 - m) / 12;
    const int yy = y + 4800 - a;
    const int mm = m + 12 * a - 3;
    return d + (153 * mm + 2) / 5 + 365 * yy + yy / 4 - yy / 100 + yy / 400 - 32045;
}

/*static*/ bool PerGameMemory::needs_revalidation(
    const LearnedEntry& e,
    uint32_t            max_age_days) noexcept
{
    // user_locked entries never expire — user explicitly opted out.
    if (e.user_locked) return false;

    // Entries with no validated data always need a run.
    if (e.sample_count == 0u || e.best_action[0] == '\0') return true;

    // Parse last_validated timestamp.
    int vy = 0, vm = 0, vd = 0;
    if (!parse_iso_date(e.last_validated, &vy, &vm, &vd)) {
        return true;  // unparseable → conservative: re-validate
    }

    // Get today's date.
#ifdef _WIN32
    SYSTEMTIME st{};
    GetLocalTime(&st);
    const int today_ord = gregorian_day(st.wYear, st.wMonth, st.wDay);
#else
    const std::time_t now = std::time(nullptr);
    const std::tm* lt = std::localtime(&now);
    const int today_ord = lt
        ? gregorian_day(lt->tm_year + 1900, lt->tm_mon + 1, lt->tm_mday)
        : gregorian_day(2025, 1, 1);
#endif

    const int validated_ord = gregorian_day(vy, vm, vd);
    const int age_days      = today_ord - validated_ord;
    return age_days > static_cast<int>(max_age_days);
}

uint32_t PerGameMemory::expire_stale_entries(uint32_t max_age_days) noexcept
{
    uint32_t expired = 0u;
    for (uint32_t i = 0u; i < n_entries_; ++i) {
        LearnedEntry& e = entries_[i];
        if (e.exe[0] == '\0') continue;
        if (!needs_revalidation(e, max_age_days)) continue;

        // Invalidate the cached result — preserve (exe, hardware_id) key.
        e.best_action[0]  = '\0';
        e.best_core_mask  = 0ull;
        e.improvement_pct = 0.0f;
        e.sample_count    = 0u;
        e.last_validated[0] = '\0';
        ++expired;

        std::fprintf(stdout,
            "[Ayama][Learn] Expired stale entry: %s (>%u days)\n",
            e.exe, max_age_days);
    }
    return expired;
}

} // namespace ayama::learn
// Made with my soul - Swately <3
