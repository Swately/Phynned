// framework/topology/src/CoreTelemetry.cpp
// Implementation of phyriad::CoreTelemetry — see CoreTelemetry.hpp.
//
// The HWiNFO SHM parsing reuses the verified layout + logic from the standalone
// reader tools/hwinfo_shm/main.cpp (Pack=1 structs, stride by dwSizeOf*Element,
// type + case-insensitive substring matching). See docs/research/HWINFO_SHM.md
// for the [V]-tagged byte offsets and sources.
//
// Author: Claude (Opus 4.8) for Swately / Phynned. User-mode only, low risk.

#include <phyriad/topology/CoreTelemetry.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <cctype>

namespace phyriad {

const char* backend_name(Backend b) noexcept {
    switch (b) {
        case Backend::HwinfoShm: return "HwinfoShm";
        case Backend::Pdh:       return "Pdh";
        case Backend::None:      return "None";
    }
    return "None";
}

#if defined(_WIN32)

// Keep platform headers OUT of the public header (pimpl). Everything Win32 is
// confined to this translation unit.
#include <windows.h>
#include <powrprof.h>
#include <pdh.h>
#include <pdhmsg.h>

namespace {

// ── Verified HWiNFO SM2 layout (Pack=1) — mirrors tools/hwinfo_shm/main.cpp ───
#pragma pack(push, 1)
struct HdrSM2 {
    uint32_t dwSignature;               // 0x00
    uint32_t dwVersion;                 // 0x04
    uint32_t dwRevision;                // 0x08
    int64_t  pollTime;                  // 0x0C
    uint32_t dwOffsetOfSensorSection;   // 0x14
    uint32_t dwSizeOfSensorElement;     // 0x18
    uint32_t dwNumSensorElements;       // 0x1C
    uint32_t dwOffsetOfReadingSection;  // 0x20
    uint32_t dwSizeOfReadingElement;    // 0x24
    uint32_t dwNumReadingElements;      // 0x28
};                                      // 44 bytes
struct ReadingSM2 {
    uint32_t tReading;                  // 0x000
    uint32_t dwSensorIndex;             // 0x004
    uint32_t dwReadingID;               // 0x008
    char     szLabelOrig[128];          // 0x00C
    char     szLabelUser[128];          // 0x08C
    char     szUnit[16];                // 0x10C
    double   Value;                     // 0x11C
    double   ValueMin;                  // 0x124
    double   ValueMax;                  // 0x12C
    double   ValueAvg;                  // 0x134
};                                      // 316 bytes
#pragma pack(pop)
static_assert(sizeof(HdrSM2) == 44, "HdrSM2 must be 44 bytes (Pack=1)");
static_assert(sizeof(ReadingSM2) == 316, "ReadingSM2 must be 316 bytes (Pack=1)");
static_assert(offsetof(ReadingSM2, Value) == 0x11C, "Value must be at 0x11C");

enum : uint32_t {
    SENSOR_TYPE_NONE = 0, SENSOR_TYPE_TEMP, SENSOR_TYPE_VOLT, SENSOR_TYPE_FAN,
    SENSOR_TYPE_CURRENT, SENSOR_TYPE_POWER, SENSOR_TYPE_CLOCK, SENSOR_TYPE_USAGE,
    SENSOR_TYPE_OTHER
};

bool icontains(const char* hay, const char* needle) {
    if (!hay || !needle) return false;
    size_t nl = std::strlen(needle);
    if (nl == 0) return true;
    for (size_t i = 0; hay[i]; ++i) {
        size_t j = 0;
        while (j < nl && hay[i + j] &&
               std::tolower((unsigned char)hay[i + j]) == std::tolower((unsigned char)needle[j]))
            ++j;
        if (j == nl) return true;
    }
    return false;
}

// Parse a "Core <N> ..." label. Returns true and sets `idx` if the label starts
// (after optional leading spaces, case-insensitive) with "Core " + digits.
bool parse_core_index(const char* label, uint32_t& idx) {
    if (!label) return false;
    const char* p = label;
    while (*p == ' ') ++p;
    static const char kCore[] = "core";
    for (int k = 0; k < 4; ++k) {
        if (!p[k] || std::tolower((unsigned char)p[k]) != kCore[k]) return false;
    }
    p += 4;
    if (*p != ' ') return false;   // "Core " — not "CoreN"/"Cores"
    while (*p == ' ') ++p;
    if (!std::isdigit((unsigned char)*p)) return false;
    uint32_t n = 0;
    while (std::isdigit((unsigned char)*p)) { n = n * 10u + uint32_t(*p - '0'); ++p; }
    idx = n;
    return true;
}

// ── Base clock (rated) detection — clean, driverless ─────────────────────────
struct PPI {  // PROCESSOR_POWER_INFORMATION (declared locally to avoid header skew)
    ULONG Number, MaxMhz, CurrentMhz, MhzLimit, MaxIdleState, CurrentIdleState;
};

double base_from_ntpower() {
    SYSTEM_INFO si{}; GetSystemInfo(&si);
    ULONG n = si.dwNumberOfProcessors ? si.dwNumberOfProcessors : 1u;
    std::vector<PPI> ppi(n);
    LONG st = CallNtPowerInformation(ProcessorInformation, nullptr, 0,
                                     ppi.data(), (ULONG)(n * sizeof(PPI)));
    if (st != 0) return 0.0;
    ULONG mx = 0;
    for (ULONG i = 0; i < n; ++i) mx = (ppi[i].MaxMhz > mx) ? ppi[i].MaxMhz : mx;
    return (double)mx;
}

double base_from_registry() {
    HKEY k{};
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE,
            "HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0",
            0, KEY_READ, &k) != ERROR_SUCCESS)
        return 0.0;
    DWORD mhz = 0, sz = sizeof(mhz), type = 0;
    LONG r = RegQueryValueExA(k, "~MHz", nullptr, &type, (LPBYTE)&mhz, &sz);
    RegCloseKey(k);
    return (r == ERROR_SUCCESS && type == REG_DWORD) ? (double)mhz : 0.0;
}

} // namespace

// ── Impl ─────────────────────────────────────────────────────────────────────
struct CoreTelemetry::Impl {
    Backend     backend  = Backend::None;
    double      base_mhz = 4200.0;
    const char* base_src = "hardcoded 4200 (TODO: no OS source)";
    uint32_t    pdh_ms   = 200u;

    // HWiNFO state
    HANDLE hMap   = nullptr;
    HANDLE hMutex = nullptr;
    const unsigned char* base = nullptr;
    size_t mapSize = 0;

    // PDH state
    PDH_HQUERY   query   = nullptr;
    PDH_HCOUNTER counter = nullptr;

    void detect_base_clock() {
        double b = base_from_ntpower();
        if (b >= 1000.0 && b <= 12000.0) { base_mhz = b; base_src = "CallNtPowerInformation MaxMhz"; return; }
        b = base_from_registry();
        if (b >= 1000.0 && b <= 12000.0) { base_mhz = b; base_src = "registry ~MHz"; return; }
        // fallthrough keeps the hardcoded 4200 + its TODO source string.
    }

    bool try_open_hwinfo() {
        hMap = OpenFileMappingA(FILE_MAP_READ, FALSE, "Global\\HWiNFO_SENS_SM2");
        if (!hMap) hMap = OpenFileMappingA(FILE_MAP_READ, FALSE, "HWiNFO_SENS_SM2");
        if (!hMap) return false;
        base = (const unsigned char*)MapViewOfFile(hMap, FILE_MAP_READ, 0, 0, 0);
        if (!base) { CloseHandle(hMap); hMap = nullptr; return false; }
        MEMORY_BASIC_INFORMATION mbi{};
        if (VirtualQuery(base, &mbi, sizeof(mbi))) mapSize = mbi.RegionSize;
        hMutex = OpenMutexA(SYNCHRONIZE, FALSE, "Global\\HWiNFO_SM2_MUTEX");
        return true;
    }

    bool open_pdh() {
        if (PdhOpenQueryA(nullptr, 0, &query) != ERROR_SUCCESS) return false;
        // English counter name -> locale-independent.
        if (PdhAddEnglishCounterA(query,
                "\\Processor Information(*)\\% Processor Performance",
                0, &counter) != ERROR_SUCCESS) {
            PdhCloseQuery(query); query = nullptr; return false;
        }
        PdhCollectQueryData(query);  // prime (harmless; read_core_clocks re-pairs)
        return true;
    }

    // Read + snapshot the HWiNFO header under the writer mutex; returns false if
    // the mapping looks unsound (refuse to parse rather than emit garbage).
    bool hwinfo_header(HdrSM2& h) {
        if (!base) return false;
        bool held = false;
        if (hMutex && WaitForSingleObject(hMutex, 1000) == WAIT_OBJECT_0) held = true;
        std::memcpy(&h, base, sizeof(h));
        if (held) ReleaseMutex(hMutex);
        if (h.dwSizeOfReadingElement < sizeof(ReadingSM2)) return false;
        if (h.dwNumReadingElements   >= 100000u)           return false;
        if (mapSize) {
            uint64_t rEnd = (uint64_t)h.dwOffsetOfReadingSection +
                            (uint64_t)h.dwNumReadingElements * h.dwSizeOfReadingElement;
            if (rEnd > mapSize) return false;
        }
        return true;
    }

    template <class Fn>
    void hwinfo_for_each_reading(const HdrSM2& h, Fn&& fn) {
        const unsigned char* readBase = base + h.dwOffsetOfReadingSection;
        for (uint32_t i = 0; i < h.dwNumReadingElements; ++i) {
            ReadingSM2 r{};
            std::memcpy(&r, readBase + (size_t)i * h.dwSizeOfReadingElement, sizeof(r));
            r.szLabelOrig[sizeof(r.szLabelOrig) - 1] = 0;
            r.szLabelUser[sizeof(r.szLabelUser) - 1] = 0;
            r.szUnit[sizeof(r.szUnit) - 1] = 0;
            fn(r);
        }
    }

    std::vector<CoreClock> hwinfo_clocks() {
        std::vector<CoreClock> out;
        HdrSM2 h{};
        if (!hwinfo_header(h)) return out;
        // Per core index: prefer the plain "Core N Clock" over the
        // "Core N T0 Effective Clock" for the primary mhz field.
        hwinfo_for_each_reading(h, [&](const ReadingSM2& r) {
            if (r.tReading != SENSOR_TYPE_CLOCK) return;
            const char* L = r.szLabelUser[0] ? r.szLabelUser : r.szLabelOrig;
            uint32_t idx = 0;
            if (!parse_core_index(L, idx)) return;
            if (!icontains(L, "Clock")) return;
            const bool eff = icontains(L, "Effective");
            auto it = std::find_if(out.begin(), out.end(),
                                   [&](const CoreClock& c){ return c.logical_id == idx; });
            if (it == out.end()) {
                out.push_back(CoreClock{ idx, r.Value,
                                         base_mhz > 0 ? r.Value / base_mhz * 100.0 : 0.0 });
            } else if (!eff) {
                // A non-effective reading overrides a previously stored effective one.
                it->mhz = r.Value;
                it->pct_of_base = base_mhz > 0 ? r.Value / base_mhz * 100.0 : 0.0;
            }
        });
        std::sort(out.begin(), out.end(),
                  [](const CoreClock& a, const CoreClock& b){ return a.logical_id < b.logical_id; });
        return out;
    }

    std::vector<CoreExtra> hwinfo_extras() {
        std::vector<CoreExtra> out;
        HdrSM2 h{};
        if (!hwinfo_header(h)) return out;
        const double NaN = std::nan("");
        auto slot = [&](uint32_t idx) -> CoreExtra& {
            auto it = std::find_if(out.begin(), out.end(),
                                   [&](const CoreExtra& c){ return c.logical_id == idx; });
            if (it != out.end()) return *it;
            out.push_back(CoreExtra{ idx, NaN, NaN });
            return out.back();
        };
        hwinfo_for_each_reading(h, [&](const ReadingSM2& r) {
            const char* L = r.szLabelUser[0] ? r.szLabelUser : r.szLabelOrig;
            uint32_t idx = 0;
            if (!parse_core_index(L, idx)) return;
            if (r.tReading == SENSOR_TYPE_CLOCK && icontains(L, "Effective"))
                slot(idx).effective_mhz = r.Value;
            else if (r.tReading == SENSOR_TYPE_VOLT && icontains(L, "VID"))
                slot(idx).vid_volts = r.Value;
        });
        std::sort(out.begin(), out.end(),
                  [](const CoreExtra& a, const CoreExtra& b){ return a.logical_id < b.logical_id; });
        return out;
    }

    PackageTelemetry hwinfo_package() {
        PackageTelemetry pkg;
        HdrSM2 h{};
        if (!hwinfo_header(h)) return pkg;
        const double NaN = std::nan("");
        pkg.package_power_w = NaN; pkg.ppt_w = NaN;
        hwinfo_for_each_reading(h, [&](const ReadingSM2& r) {
            if (r.tReading != SENSOR_TYPE_POWER) return;
            const char* L = r.szLabelUser[0] ? r.szLabelUser : r.szLabelOrig;
            if (icontains(L, "PPT"))            { pkg.ppt_w = r.Value; pkg.valid = true; }
            else if (icontains(L, "Package"))   { pkg.package_power_w = r.Value; pkg.valid = true; }
        });
        return pkg;
    }

    std::vector<CoreClock> pdh_clocks() {
        std::vector<CoreClock> out;
        if (!query || !counter) return out;
        // Two collects ~pdh_ms apart: % Processor Performance is a rate counter.
        if (PdhCollectQueryData(query) != ERROR_SUCCESS) return out;
        Sleep(pdh_ms);
        if (PdhCollectQueryData(query) != ERROR_SUCCESS) return out;

        DWORD bufSize = 0, itemCount = 0;
        PDH_STATUS s = PdhGetFormattedCounterArrayA(counter, PDH_FMT_DOUBLE,
                                                    &bufSize, &itemCount, nullptr);
        if (s != (PDH_STATUS)PDH_MORE_DATA || bufSize == 0) return out;
        std::vector<uint8_t> buf(bufSize);
        auto* items = (PDH_FMT_COUNTERVALUE_ITEM_A*)buf.data();
        if (PdhGetFormattedCounterArrayA(counter, PDH_FMT_DOUBLE,
                                         &bufSize, &itemCount, items) != ERROR_SUCCESS)
            return out;
        for (DWORD i = 0; i < itemCount; ++i) {
            const char* name = items[i].szName;   // "group,cpu", e.g. "0,17"
            if (!name) continue;
            const char* comma = std::strchr(name, ',');
            const char* cpuStr = comma ? comma + 1 : name;
            if (!std::isdigit((unsigned char)*cpuStr)) continue;   // skip "_Total"
            uint32_t lid = (uint32_t)std::strtoul(cpuStr, nullptr, 10);
            double pct = items[i].FmtValue.doubleValue;
            if (items[i].FmtValue.CStatus != PDH_CSTATUS_VALID_DATA &&
                items[i].FmtValue.CStatus != 0)
                continue;
            out.push_back(CoreClock{ lid, pct / 100.0 * base_mhz, pct });
        }
        std::sort(out.begin(), out.end(),
                  [](const CoreClock& a, const CoreClock& b){ return a.logical_id < b.logical_id; });
        return out;
    }

    ~Impl() {
        if (base)   UnmapViewOfFile(base);
        if (hMutex) CloseHandle(hMutex);
        if (hMap)   CloseHandle(hMap);
        if (query)  PdhCloseQuery(query);
    }
};

CoreTelemetry::CoreTelemetry(uint32_t pdh_sample_ms) noexcept
    : impl_(std::make_unique<Impl>()) {
    impl_->pdh_ms = pdh_sample_ms ? pdh_sample_ms : 1u;
    impl_->detect_base_clock();
    if (impl_->try_open_hwinfo()) {
        impl_->backend = Backend::HwinfoShm;
    } else if (impl_->open_pdh()) {
        impl_->backend = Backend::Pdh;
    } else {
        impl_->backend = Backend::None;
    }
}

CoreTelemetry::~CoreTelemetry() = default;
CoreTelemetry::CoreTelemetry(CoreTelemetry&&) noexcept = default;
CoreTelemetry& CoreTelemetry::operator=(CoreTelemetry&&) noexcept = default;

Backend     CoreTelemetry::which_backend()    const noexcept { return impl_->backend; }
double      CoreTelemetry::base_clock_mhz()    const noexcept { return impl_->base_mhz; }
const char* CoreTelemetry::base_clock_source() const noexcept { return impl_->base_src; }

std::vector<CoreClock> CoreTelemetry::read_core_clocks() {
    switch (impl_->backend) {
        case Backend::HwinfoShm: return impl_->hwinfo_clocks();
        case Backend::Pdh:       return impl_->pdh_clocks();
        case Backend::None:      return {};
    }
    return {};
}

std::vector<CoreExtra> CoreTelemetry::read_core_extras() {
    return impl_->backend == Backend::HwinfoShm ? impl_->hwinfo_extras()
                                                : std::vector<CoreExtra>{};
}

PackageTelemetry CoreTelemetry::read_package() {
    return impl_->backend == Backend::HwinfoShm ? impl_->hwinfo_package()
                                                : PackageTelemetry{};
}

#else // ── non-Windows stub — Phynned is Windows-only; keep it compile-clean ──

struct CoreTelemetry::Impl { Backend backend = Backend::None; };
CoreTelemetry::CoreTelemetry(uint32_t) noexcept : impl_(std::make_unique<Impl>()) {}
CoreTelemetry::~CoreTelemetry() = default;
CoreTelemetry::CoreTelemetry(CoreTelemetry&&) noexcept = default;
CoreTelemetry& CoreTelemetry::operator=(CoreTelemetry&&) noexcept = default;
Backend     CoreTelemetry::which_backend()    const noexcept { return Backend::None; }
double      CoreTelemetry::base_clock_mhz()    const noexcept { return 0.0; }
const char* CoreTelemetry::base_clock_source() const noexcept { return "n/a (non-Windows)"; }
std::vector<CoreClock> CoreTelemetry::read_core_clocks() { return {}; }
std::vector<CoreExtra> CoreTelemetry::read_core_extras() { return {}; }
PackageTelemetry       CoreTelemetry::read_package()     { return {}; }

#endif

} // namespace phyriad
// Made with my soul - Swately <3
