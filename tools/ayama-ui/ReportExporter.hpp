// apps/ayama/tools/ayama-ui/ReportExporter.hpp
// ReportExporter — generates a .md empirical report from BenchRunner results.
//
// Template mirrors the existing reports in docs/ayama/reports/. The exporter
// fills in objective data (process name, hardware, per-run numbers, raw
// aggregate log) but leaves interpretive sections (analysis, conclusion)
// for the user to fill in afterwards — the agent cannot generate honest
// scientific interpretation, only structured data.
//
// Threading: single-thread (UI thread). All I/O blocking — call from a
// "Export" button click handler, not from per-frame draw.
//
#pragma once

#include "BenchRunner.hpp"

#include <phyriad/topology/HardwareTopology.hpp>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <sstream>
#include <string>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace ayama::ui {

/// Build an auto-suggested output path for the report.
///
/// Format: `<dir>/<game_short>_<YYYY-MM-DD>_<cpu_tag>.md`
/// Example: `<repo-root>/docs/ayama/reports/Fallout4_2026-05-17_7950x3d.md`
///
/// `process_name` is the full exe (e.g. "Fallout4.exe"); `.exe` is stripped.
/// `cpu_tag` is a short lowercase identifier (e.g. "7950x3d", "12700k", "generic").
/// `out_path` must point to ≥ 260 chars.
inline void suggest_report_path(const char* process_name,
                                 char*       out_path,
                                 uint32_t    out_cap) noexcept
{
    if (!out_path || out_cap < 260u || !process_name) return;

    // Locate the ayama-ui.exe directory, climb to the project root if possible.
    // The dist layout puts the exe at: <root>/build/ayama-dist/ayama-ui.exe
    // and the reports live at:         <root>/docs/reports/
    // So from ayama-ui.exe: ../../docs/reports/  (installed copies fall
    // through to the %TEMP%\ayama-reports fallback below).
    char base_dir[260]{};
#ifdef _WIN32
    {
        wchar_t wself[MAX_PATH]{};
        const DWORD n = GetModuleFileNameW(nullptr, wself, MAX_PATH);
        if (n > 0u && n < MAX_PATH) {
            // Strip filename
            for (int i = static_cast<int>(n) - 1; i >= 0; --i) {
                if (wself[i] == L'\\' || wself[i] == L'/') {
                    wself[i + 1] = L'\0';
                    break;
                }
            }
            // Narrow ASCII
            uint32_t bi = 0u;
            for (uint32_t i = 0u; wself[i] != L'\0' && bi < 259u; ++i) {
                base_dir[bi++] = (wself[i] < 0x80)
                    ? static_cast<char>(wself[i]) : '?';
            }
            base_dir[bi] = '\0';
            // Append relative path. The dist layout has reports 2 levels up.
            std::strncat(base_dir, "..\\..\\docs\\reports",
                         sizeof(base_dir) - std::strlen(base_dir) - 1);
        }
    }
#endif
    if (base_dir[0] == '\0') {
        // Fallback: write into the user's TEMP directory under
        // ayama-reports/. Previously this was hardcoded to a dev
        // machine's drive — broken for any downstream adopter. The
        // %TEMP% path is guaranteed writable, always exists on Windows,
        // and the reports persist until the user clears their temp dir
        // or moves them somewhere permanent.
#ifdef _WIN32
        char tmp[260]{};
        const DWORD tn = GetTempPathA(sizeof(tmp), tmp);
        if (tn > 0u && tn < sizeof(tmp)) {
            std::snprintf(base_dir, sizeof(base_dir),
                          "%sayama-reports", tmp);
            CreateDirectoryA(base_dir, nullptr);  // OK if exists
        } else {
            std::strncpy(base_dir, ".\\ayama-reports", sizeof(base_dir) - 1);
        }
#else
        std::strncpy(base_dir, "./ayama-reports", sizeof(base_dir) - 1);
#endif
        base_dir[sizeof(base_dir) - 1] = '\0';
    }

    // Strip .exe from process name → short game name
    char game[64]{};
    {
        uint32_t i = 0u;
        while (i < 63u && process_name[i] != '\0') {
            // Stop at .exe / .EXE
            if ((process_name[i] == '.' || process_name[i] == '-') &&
                (process_name[i + 1] == 'e' || process_name[i + 1] == 'E') &&
                (process_name[i + 2] == 'x' || process_name[i + 2] == 'X') &&
                (process_name[i + 3] == 'e' || process_name[i + 3] == 'E')) {
                break;
            }
            game[i] = process_name[i];
            ++i;
        }
        game[i] = '\0';
    }
    if (game[0] == '\0') std::strncpy(game, "report", 63);

    // Current date YYYY-MM-DD
    char date_str[16]{};
    {
        const std::time_t now = std::time(nullptr);
        std::tm tm{};
#ifdef _WIN32
        localtime_s(&tm, &now);
#else
        localtime_r(&now, &tm);
#endif
        std::snprintf(date_str, sizeof(date_str), "%04d-%02d-%02d",
                      tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
    }

    // CPU tag — derive from phyriad::hw::topology(). For now we hardcode the
    // common cases and fall back to "cpu" generic.
    char cpu_tag[24] = "cpu";
    {
        const auto& topo = phyriad::hw::topology();
        const uint32_t lcc = topo.logical_core_count();
        bool has_vcache = false;
        for (const auto& c : topo.cores) {
            if (c.has_v_cache) { has_vcache = true; break; }
        }
        const uint32_t ccd_count = topo.ccd_count();

        if (has_vcache && lcc == 32u && ccd_count == 2u) {
            std::strncpy(cpu_tag, "7950x3d", sizeof(cpu_tag) - 1);
        } else if (has_vcache && lcc == 24u && ccd_count == 2u) {
            std::strncpy(cpu_tag, "7900x3d", sizeof(cpu_tag) - 1);
        } else if (has_vcache && lcc == 16u && ccd_count == 1u) {
            std::strncpy(cpu_tag, "7800x3d", sizeof(cpu_tag) - 1);
        } else if (has_vcache) {
            std::strncpy(cpu_tag, "amd-x3d", sizeof(cpu_tag) - 1);
        }
    }

    std::snprintf(out_path, out_cap, "%s\\%s_%s_%s.md",
                  base_dir, game, date_str, cpu_tag);
}

/// Build an short CPU description for the Hardware section of the report.
inline void cpu_description(char* out, uint32_t out_cap) noexcept
{
    if (!out || out_cap < 64u) return;
    const auto& topo = phyriad::hw::topology();
    const uint32_t lcc = topo.logical_core_count();
    const uint32_t pcc = topo.physical_core_count();
    bool has_vcache = false;
    for (const auto& c : topo.cores) {
        if (c.has_v_cache) { has_vcache = true; break; }
    }
    const uint32_t ccd_count = topo.ccd_count();

    std::snprintf(out, out_cap,
        "AMD Ryzen (%uC/%uT, %u CCD%s%s)",
        pcc, lcc, ccd_count, ccd_count == 1u ? "" : "s",
        has_vcache ? ", V-Cache" : "");
}

/// Read a small text file into a string (for embedding aggregate log).
/// Returns empty string on failure.
inline std::string read_text_file(const char* path) noexcept
{
    if (!path || !path[0]) return {};
    std::ifstream f(path);
    if (!f.good()) return {};
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

/// Generate a complete empirical report .md file from a BenchRunner result.
///
/// Returns true on success. On failure logs to stderr and returns false.
///
/// The report is **template-style**: it fills in objective data (per-run
/// numbers, hardware, raw aggregate log) but leaves the "Summary",
/// "Interpretation", and "Conclusion" sections for the user to write. This
/// is intentional — the tool can produce data, but scientific
/// interpretation belongs to the human running the test.
inline bool generate_md_report(const BenchRun*  runs,
                                uint32_t         n_runs,
                                const char*      process_name,
                                BenchRunner::Protocol protocol,
                                const char*      aggregate_log_path,
                                const char*      output_path) noexcept
{
    if (!runs || n_runs == 0u || !process_name || !output_path) {
        return false;
    }

    std::ofstream out(output_path);
    if (!out.good()) {
        std::fprintf(stderr,
            "[ReportExporter] failed to open output path: %s\n", output_path);
        return false;
    }

    // ── Game short name (strip .exe) ──────────────────────────────────────
    char game_short[64]{};
    std::strncpy(game_short, process_name, sizeof(game_short) - 1u);
    {
        char* dot = std::strrchr(game_short, '.');
        if (dot && (std::strcmp(dot, ".exe") == 0 ||
                    std::strcmp(dot, ".EXE") == 0)) {
            *dot = '\0';
        }
    }

    // ── Current date YYYY-MM-DD ───────────────────────────────────────────
    char date_str[16]{};
    {
        const std::time_t now = std::time(nullptr);
        std::tm tm{};
#ifdef _WIN32
        localtime_s(&tm, &now);
#else
        localtime_r(&now, &tm);
#endif
        std::snprintf(date_str, sizeof(date_str), "%04d-%02d-%02d",
                      tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
    }

    // ── Hardware description ──────────────────────────────────────────────
    char cpu_desc[64]{};
    cpu_description(cpu_desc, sizeof(cpu_desc));

    // ── Header ────────────────────────────────────────────────────────────
    out << "# Scenario: " << game_short << " on " << cpu_desc
        << " — 5-run A/B/A/B/A\n\n";
    out << "**Status: VALID — _**[FILL IN VERDICT FROM AGGREGATE BELOW]**_**\n";
    out << "**Date**: " << date_str << "\n";
    out << "**Protocol**: "
        << (protocol == BenchRunner::Protocol::ABABA5Run ? "A/B/A/B/A (5 runs)"
                                                          : "Single capture")
        << "\n\n";

    // ── Summary placeholder ───────────────────────────────────────────────
    out << "## Summary\n\n";
    out << "_[FILL IN: 2-3 paragraphs describing the result. Reference the\n";
    out << "headline numbers below. Include the FPS delta and key metric\n";
    out << "improvements. Be honest — if NULL, say NULL.]_\n\n";

    // ── Hardware ──────────────────────────────────────────────────────────
    out << "## Hardware\n\n";
    out << "| Component | Spec |\n";
    out << "|---|---|\n";
    out << "| CPU | " << cpu_desc << " |\n";
    out << "| RAM | _[fill]_ |\n";
    out << "| GPU | _[fill]_ |\n";
    out << "| OS | Windows 11 |\n\n";

    // ── Software ──────────────────────────────────────────────────────────
    out << "## Software\n\n";
    out << "- Ayama: build " << date_str << "\n";
    out << "- " << game_short << ": _[fill: version, mods, renderer]_\n";
    out << "- PresentMon: 2.4.1\n";
    out << "- ayama-ui bench runner\n\n";

    // ── Methodology ───────────────────────────────────────────────────────
    out << "## Methodology\n\n";
    out << "Standard 5-run A/B/A/B/A protocol via UI bench runner. Each run\n";
    out << "is 30 s wall-clock enforced by PresentMon's `--timed` flag.\n";
    out << "Pause/resume between baseline and treated runs via Ayama's IPC\n";
    out << "command slot.\n\n";

    // ── Raw per-run data ──────────────────────────────────────────────────
    out << "## Raw per-run data\n\n";
    out << "```\n";
    for (uint32_t i = 0u; i < n_runs; ++i) {
        const auto& r = runs[i];
        out << "Run " << (i + 1u) << " ("
            << (r.is_baseline ? "Baseline" : "Treated ") << "): "
            << r.bench_csv_path << "\n";
    }
    out << "```\n\n";

    // ── Aggregate log ─────────────────────────────────────────────────────
    out << "## Aggregate verdict (from `ayama-cli bench multi`)\n\n";
    out << "```\n";
    const std::string agg = read_text_file(aggregate_log_path);
    if (!agg.empty()) {
        out << agg;
    } else {
        out << "[Aggregate log not available — re-run with single-capture\n";
        out << " protocol does not produce a multi-run aggregate.]\n";
    }
    out << "```\n\n";

    // ── Interpretation placeholder ────────────────────────────────────────
    out << "## Interpretation\n\n";
    out << "_[FILL IN: explain why the result is what it is. Engine\n";
    out << "characteristics, CPU saturation level, GPU bound vs CPU bound,\n";
    out << "VSync constraints, etc. See examples in fallout4, hogwarts-legacy,\n";
    out << "halo2-mcc reports.]_\n\n";

    // ── Files preserved ───────────────────────────────────────────────────
    out << "## Files preserved\n\n";
    for (uint32_t i = 0u; i < n_runs; ++i) {
        out << "- " << runs[i].pm_csv_path << " (PresentMon raw)\n";
        out << "- " << runs[i].bench_csv_path << " (ayama-bench format)\n";
    }
    if (aggregate_log_path && aggregate_log_path[0]) {
        out << "- " << aggregate_log_path << " (aggregate output)\n";
    }
    out << "\n";

    // ── Footer ────────────────────────────────────────────────────────────
    out << "---\n\n";
    out << "**Generated by**: `ayama-ui` Report Exporter.\n";
    out << "**Template**: structured data filled automatically; interpretive\n";
    out << "sections (Summary, Interpretation, Conclusion) require manual\n";
    out << "completion. See `EMPIRICAL_TEST_PROTOCOL.md §7` for methodology\n";
    out << "pitfalls.\n";

    out.close();
    if (!out.good()) {
        std::fprintf(stderr,
            "[ReportExporter] write failed for %s\n", output_path);
        return false;
    }
    std::fprintf(stdout,
        "[ReportExporter] Report written to: %s\n", output_path);
    return true;
}

} // namespace ayama::ui
// Made with my soul - Swately <3
