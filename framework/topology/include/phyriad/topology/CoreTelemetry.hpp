// framework/topology/include/phyriad/topology/CoreTelemetry.hpp
// Per-core CLOCK telemetry for Phynned — a reusable, driverless abstraction over
// two backends, auto-selected at construction:
//
//   Backend::HwinfoShm  (preferred, richer) — reads HWiNFO's published sensor
//       shared memory ("Global\HWiNFO_SENS_SM2") in USER MODE. HWiNFO's own
//       signed driver reads the MSRs; we only map and parse its read-only SHM.
//       Gives per-core clock + (optional) effective clock, VID, package power/PPT.
//       Only present if the operator enabled "Shared Memory Support" in HWiNFO.
//
//   Backend::Pdh        (fallback, always available) — Windows PDH counter
//       "\Processor Information(*)\% Processor Performance". Ships with Windows,
//       no external app, no driver. Per-logical-processor effective clock =
//       %perf/100 * base_clock. Lower fidelity than HWiNFO but always there.
//
//   Backend::None       — non-Windows build, or catastrophic PDH failure.
//
// Auto-select: HWiNFO SHM if the mapping exists at construction, else PDH.
// which_backend() reports which one is live — the reader NEVER fabricates: if
// HWiNFO SHM is absent it transparently uses PDH and says so.
//
// Consumed by Phynned's routing A/B (reads per-core clocks before/after placing a
// workload on a CCD) and the UI. Cheap, callable periodically — see the blocking
// note on read_core_clocks() for the PDH sampling window.
//
// Base clock source (for the %perf -> MHz conversion): see base_clock_source().
// The framework topology leaves max_freq_mhz==0 on Windows (WMI too costly) and
// CPUID leaf 0x16 is Intel-only, so we read the RATED base via a clean OS call
// (CallNtPowerInformation MaxMhz), with registry ~MHz then a 4200 MHz constant as
// documented fallbacks.
//
// Public header is windows.h/pdh.h-free (pimpl), like hw::MmcssToken.
//
// Author: Claude (Opus 4.8) for Swately / Phynned. User-mode only, low risk.

#pragma once
#include <cstdint>
#include <memory>
#include <vector>

namespace phyriad {

// ── CoreClock — the must-have per-core reading ───────────────────────────────
struct CoreClock {
    // PDH backend:    OS logical-processor index (0 .. logical_core_count-1).
    // HWiNFO backend: HWiNFO's "Core N" index — a PHYSICAL core index, not a
    //                 logical processor (HWiNFO reports one clock per physical
    //                 core). Documented mismatch; consumers that need logical
    //                 granularity should prefer the PDH backend.
    uint32_t logical_id{0};
    double   mhz{0.0};          // current/effective clock in MHz
    double   pct_of_base{0.0};  // mhz / base_clock_mhz * 100
};

// ── CoreExtra — optional richer per-core fields (HWiNFO SHM backend only) ─────
// Empty from the PDH backend. NaN in a field means "not exposed by this HWiNFO
// config". Best-effort — not verified live on this box (SHM was OFF 2026-07-17).
struct CoreExtra {
    uint32_t logical_id{0};      // HWiNFO "Core N" index (physical core)
    double   effective_mhz{0.0}; // HWiNFO "Core N T0 Effective Clock" (NaN if absent)
    double   vid_volts{0.0};     // HWiNFO per-core VID (NaN if absent)
};

// ── PackageTelemetry — package-level extras (HWiNFO SHM backend only) ─────────
struct PackageTelemetry {
    bool   valid{false};         // false from the PDH backend / if unparsed
    double package_power_w{0.0}; // HWiNFO "CPU Package Power" (NaN if absent)
    double ppt_w{0.0};           // HWiNFO "CPU PPT" (NaN if absent)
};

enum class Backend : uint8_t { None = 0, HwinfoShm = 1, Pdh = 2 };

const char* backend_name(Backend b) noexcept;

// ── CoreTelemetry — the reusable telemetry abstraction ───────────────────────
class CoreTelemetry {
public:
    // Auto-selects the backend at construction. `pdh_sample_ms` is the interval
    // between the two PDH collects a rate counter needs (ignored by HWiNFO).
    explicit CoreTelemetry(uint32_t pdh_sample_ms = 200u) noexcept;
    ~CoreTelemetry();

    CoreTelemetry(CoreTelemetry&&) noexcept;
    CoreTelemetry& operator=(CoreTelemetry&&) noexcept;
    CoreTelemetry(const CoreTelemetry&)            = delete;
    CoreTelemetry& operator=(const CoreTelemetry&) = delete;

    // Which backend is actually live. Never lies about HWiNFO availability.
    [[nodiscard]] Backend which_backend() const noexcept;

    // Rated base clock (MHz) used for the %perf<->MHz conversion, and where it
    // came from ("CallNtPowerInformation", "registry ~MHz", or "hardcoded 4200").
    [[nodiscard]] double       base_clock_mhz()    const noexcept;
    [[nodiscard]] const char*  base_clock_source() const noexcept;

    // The must-have: per-core clocks, sorted by logical_id.
    //   PDH backend: BLOCKS ~pdh_sample_ms (two collects for the rate counter).
    //   HWiNFO backend: cheap — re-reads the live SHM view, no sleep.
    // Returns empty on Backend::None.
    [[nodiscard]] std::vector<CoreClock> read_core_clocks();

    // Optional extras — non-empty ONLY on the HWiNFO SHM backend.
    [[nodiscard]] std::vector<CoreExtra> read_core_extras();
    [[nodiscard]] PackageTelemetry       read_package();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace phyriad
// Made with my soul - Swately <3
