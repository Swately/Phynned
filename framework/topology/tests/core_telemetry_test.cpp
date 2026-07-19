// framework/topology/tests/core_telemetry_test.cpp
// Eyeball test for phyriad::CoreTelemetry: constructs it, prints which backend
// was auto-selected + the base-clock source, and dumps per-core MHz so the
// operator/agent can sanity-check against HWiNFO or ON_BOX_FACTS §6.4.
//
// This is a "does it read sane numbers" harness, not a pass/fail unit test — the
// values are hardware/live-state dependent. It returns non-zero only if NO core
// clocks could be read at all (a real failure of both backends).
//
// Author: Claude (Opus 4.8) for Swately / Phynned.

#include <phyriad/topology/CoreTelemetry.hpp>
#include <cmath>
#include <cstdio>

int main() {
    using namespace phyriad;

    CoreTelemetry tel;   // default 200 ms PDH sample window
    Backend b = tel.which_backend();

    std::printf("CoreTelemetry\n");
    std::printf("  backend      : %s\n", backend_name(b));
    std::printf("  base clock   : %.0f MHz  (source: %s)\n",
                tel.base_clock_mhz(), tel.base_clock_source());

    if (b == Backend::Pdh)
        std::printf("  note         : HWiNFO SHM absent -> transparently using PDH "
                    "(\\Processor Information(*)\\%% Processor Performance).\n");
    else if (b == Backend::HwinfoShm)
        std::printf("  note         : HWiNFO Shared Memory is LIVE -> using it.\n");
    else
        std::printf("  note         : no backend available (non-Windows or PDH failure).\n");

    auto clocks = tel.read_core_clocks();
    std::printf("\n  %-8s  %10s  %10s\n", "core", "MHz", "%ofBase");
    std::printf("  --------  ----------  ----------\n");
    for (const auto& c : clocks)
        std::printf("  %-8u  %10.1f  %9.1f%%\n", c.logical_id, c.mhz, c.pct_of_base);
    std::printf("\n  %zu per-core clock(s) read.\n", clocks.size());

    // Optional HWiNFO-only extras.
    auto extras = tel.read_core_extras();
    if (!extras.empty()) {
        std::printf("\n  extras (HWiNFO):\n");
        for (const auto& e : extras)
            std::printf("    core %-3u  eff=%.1f MHz  vid=%.4f V\n",
                        e.logical_id,
                        std::isnan(e.effective_mhz) ? 0.0 : e.effective_mhz,
                        std::isnan(e.vid_volts) ? 0.0 : e.vid_volts);
    }
    auto pkg = tel.read_package();
    if (pkg.valid)
        std::printf("\n  package: power=%.1f W  PPT=%.1f W\n",
                    std::isnan(pkg.package_power_w) ? 0.0 : pkg.package_power_w,
                    std::isnan(pkg.ppt_w) ? 0.0 : pkg.ppt_w);

    return clocks.empty() ? 1 : 0;
}
// Made with my soul - Swately <3
