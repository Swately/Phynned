// hwinfo-shm.exe — Phynned optional enhanced telemetry probe.
//
// Reads HWiNFO's published sensor shared memory ("Global\HWiNFO_SENS_SM2")
// entirely in USER MODE. HWiNFO's own signed kernel driver reads the MSRs and
// publishes them here; we only map its read-only shared memory and parse it.
// No kernel code of ours. No MSR access of ours.
//
// Layout verified first-hand (2026-07-17) against the Seraksab production
// reader (Hwinfo.SharedMemory.Net, StructLayout Pack=1) which reads real
// HWiNFO, cross-checked against the PromDapterHWiNFO C# provider and namazso's
// reverse-engineered gist. See docs/research/HWINFO_SHM.md for the [V]-tagged
// layout and sources.
//
// Author: Claude (Opus 4.8) for Swately / Phynned. User-mode only, low risk.

#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cctype>

// --- Verified HWiNFO SM2 layout (Pack=1, confirmed against production reader) ---
#pragma pack(push, 1)
struct HdrSM2 {
    uint32_t dwSignature;               // 0x00  ASCII "SiWH" when valid, "DEAD" while (re)initializing
    uint32_t dwVersion;                 // 0x04
    uint32_t dwRevision;                // 0x08
    int64_t  pollTime;                  // 0x0C  __time64_t last poll (unix)
    uint32_t dwOffsetOfSensorSection;   // 0x14
    uint32_t dwSizeOfSensorElement;     // 0x18
    uint32_t dwNumSensorElements;       // 0x1C
    uint32_t dwOffsetOfReadingSection;  // 0x20
    uint32_t dwSizeOfReadingElement;    // 0x24
    uint32_t dwNumReadingElements;      // 0x28
};                                      // total 0x2C = 44 bytes
struct ReadingSM2 {
    uint32_t tReading;                  // 0x000  SENSOR_READING_TYPE
    uint32_t dwSensorIndex;             // 0x004  index into sensor[] section
    uint32_t dwReadingID;               // 0x008
    char     szLabelOrig[128];          // 0x00C
    char     szLabelUser[128];          // 0x08C
    char     szUnit[16];                // 0x10C
    double   Value;                     // 0x11C  (Pack=1: no pad after szUnit)
    double   ValueMin;                  // 0x124
    double   ValueMax;                  // 0x12C
    double   ValueAvg;                  // 0x134
};                                      // total 0x13C = 316 bytes
struct SensorSM2 {
    uint32_t dwSensorID;                // 0x000
    uint32_t dwSensorInst;             // 0x004
    char     szSensorNameOrig[128];    // 0x008
    char     szSensorNameUser[128];    // 0x088
};                                      // total 0x108 = 264 bytes
#pragma pack(pop)

// Compile-time proof the packing matches the verified byte offsets.
static_assert(sizeof(HdrSM2) == 44,  "HdrSM2 must be 44 bytes (Pack=1)");
static_assert(sizeof(ReadingSM2) == 316, "ReadingSM2 must be 316 bytes (Pack=1)");
static_assert(sizeof(SensorSM2) == 264, "SensorSM2 must be 264 bytes (Pack=1)");
static_assert(offsetof(ReadingSM2, Value) == 0x11C, "Value must be at 0x11C");

enum : uint32_t {
    SENSOR_TYPE_NONE = 0, SENSOR_TYPE_TEMP, SENSOR_TYPE_VOLT, SENSOR_TYPE_FAN,
    SENSOR_TYPE_CURRENT, SENSOR_TYPE_POWER, SENSOR_TYPE_CLOCK, SENSOR_TYPE_USAGE,
    SENSOR_TYPE_OTHER
};
static const char* type_name(uint32_t t) {
    switch (t) {
        case SENSOR_TYPE_NONE: return "NONE";  case SENSOR_TYPE_TEMP:    return "TEMP";
        case SENSOR_TYPE_VOLT: return "VOLT";  case SENSOR_TYPE_FAN:     return "FAN";
        case SENSOR_TYPE_CURRENT:return "CURR";case SENSOR_TYPE_POWER:   return "POWER";
        case SENSOR_TYPE_CLOCK:return "CLOCK"; case SENSOR_TYPE_USAGE:   return "USAGE";
        case SENSOR_TYPE_OTHER:return "OTHER"; default:                  return "?";
    }
}

static bool icontains(const char* hay, const char* needle) {
    if (!hay || !needle) return false;
    size_t nl = strlen(needle);
    if (nl == 0) return true;
    for (size_t i = 0; hay[i]; ++i) {
        size_t j = 0;
        while (j < nl && hay[i + j] &&
               tolower((unsigned char)hay[i + j]) == tolower((unsigned char)needle[j]))
            ++j;
        if (j == nl) return true;
    }
    return false;
}

// Is this reading CPU-relevant per the Phynned filter?
static bool is_cpu_relevant(const ReadingSM2& r) {
    const char* L = r.szLabelUser[0] ? r.szLabelUser : r.szLabelOrig;
    switch (r.tReading) {
        case SENSOR_TYPE_CLOCK:
            return icontains(L, "Core") || icontains(L, "CPU");            // per-core / CPU clock
        case SENSOR_TYPE_VOLT:
            return icontains(L, "VID") || icontains(L, "Core") || icontains(L, "CPU");
        case SENSOR_TYPE_POWER:
            return icontains(L, "CPU") || icontains(L, "Package") ||
                   icontains(L, "PPT") || icontains(L, "Core");            // package power / PPT
        case SENSOR_TYPE_TEMP:
            return icontains(L, "Core") || icontains(L, "CPU");            // Core temps
        default:
            return false;
    }
}

int main() {
    const char* NAME = "Global\\HWiNFO_SENS_SM2";
    HANDLE hMap = OpenFileMappingA(FILE_MAP_READ, FALSE, NAME);
    if (!hMap) {
        // Fall back to the un-prefixed name (rare, non-elevated single-session case).
        hMap = OpenFileMappingA(FILE_MAP_READ, FALSE, "HWiNFO_SENS_SM2");
    }
    if (!hMap) {
        fprintf(stderr,
            "HWiNFO SHM not available - enable Shared Memory Support in HWiNFO settings\n"
            "  (HWiNFO -> Settings -> tab 'General/Main Settings' -> check 'Shared Memory Support';\n"
            "   the Sensors window must be open, and on the free version the toggle auto-disables\n"
            "   after 12 hours of runtime and must be re-enabled.)\n");
        return 2;
    }

    // Map the whole object read-only.
    const unsigned char* base =
        (const unsigned char*)MapViewOfFile(hMap, FILE_MAP_READ, 0, 0, 0);
    if (!base) {
        fprintf(stderr, "MapViewOfFile failed (err=%lu)\n", GetLastError());
        CloseHandle(hMap);
        return 3;
    }

    // Determine the mapped region size for bounds checking.
    MEMORY_BASIC_INFORMATION mbi{};
    size_t mapSize = 0;
    if (VirtualQuery(base, &mbi, sizeof(mbi)))
        mapSize = mbi.RegionSize;

    // Optional: synchronize with HWiNFO's writer via its mutex to avoid torn reads.
    HANDLE hMutex = OpenMutexA(SYNCHRONIZE, FALSE, "Global\\HWiNFO_SM2_MUTEX");
    bool held = false;
    if (hMutex && WaitForSingleObject(hMutex, 1000) == WAIT_OBJECT_0)
        held = true;

    // Copy the header out of the live view.
    HdrSM2 h{};
    memcpy(&h, base, sizeof(h));

    char sig[5] = {0};
    memcpy(sig, &h.dwSignature, 4);
    for (int i = 0; i < 4; ++i) if (!isprint((unsigned char)sig[i])) sig[i] = '.';

    printf("HWiNFO Shared Memory (Global\\HWiNFO_SENS_SM2)\n");
    printf("  signature=\"%s\"  version=%u  revision=%u\n", sig, h.dwVersion, h.dwRevision);
    printf("  sensors:  off=0x%X size=%u count=%u\n",
           h.dwOffsetOfSensorSection, h.dwSizeOfSensorElement, h.dwNumSensorElements);
    printf("  readings: off=0x%X size=%u count=%u\n\n",
           h.dwOffsetOfReadingSection, h.dwSizeOfReadingElement, h.dwNumReadingElements);

    if (memcmp(sig, "DEAD", 4) == 0)
        printf("  (note: signature is DEAD - HWiNFO is (re)initializing the block; values may be stale)\n\n");

    // Sanity-check the section descriptors before we trust any offset.
    bool ok = h.dwSizeOfReadingElement >= sizeof(ReadingSM2) &&
              h.dwSizeOfSensorElement  >= sizeof(SensorSM2)  &&
              h.dwNumReadingElements   < 100000 &&
              h.dwNumSensorElements    < 100000;
    if (ok && mapSize) {
        uint64_t rEnd = (uint64_t)h.dwOffsetOfReadingSection +
                        (uint64_t)h.dwNumReadingElements * h.dwSizeOfReadingElement;
        uint64_t sEnd = (uint64_t)h.dwOffsetOfSensorSection +
                        (uint64_t)h.dwNumSensorElements * h.dwSizeOfSensorElement;
        if (rEnd > mapSize || sEnd > mapSize) ok = false;
    }
    if (!ok) {
        fprintf(stderr, "Header failed sanity check - refusing to parse (possible layout mismatch).\n");
        if (held) ReleaseMutex(hMutex);
        if (hMutex) CloseHandle(hMutex);
        UnmapViewOfFile(base);
        CloseHandle(hMap);
        return 4;
    }

    const unsigned char* sensBase = base + h.dwOffsetOfSensorSection;
    const unsigned char* readBase = base + h.dwOffsetOfReadingSection;

    printf("%-5s  %-40s  %14s  %14s  %-8s  %s\n",
           "TYPE", "LABEL", "VALUE", "MAX", "UNIT", "SENSOR");
    printf("--------------------------------------------------------------------------------------------------------\n");

    int shown = 0;
    for (uint32_t i = 0; i < h.dwNumReadingElements; ++i) {
        ReadingSM2 r{};
        memcpy(&r, readBase + (size_t)i * h.dwSizeOfReadingElement, sizeof(r));
        r.szLabelOrig[sizeof(r.szLabelOrig) - 1] = 0;
        r.szLabelUser[sizeof(r.szLabelUser) - 1] = 0;
        r.szUnit[sizeof(r.szUnit) - 1] = 0;

        if (!is_cpu_relevant(r)) continue;

        const char* L = r.szLabelUser[0] ? r.szLabelUser : r.szLabelOrig;
        char sensName[129] = "?";
        if (r.dwSensorIndex < h.dwNumSensorElements) {
            SensorSM2 s{};
            memcpy(&s, sensBase + (size_t)r.dwSensorIndex * h.dwSizeOfSensorElement, sizeof(s));
            s.szSensorNameUser[sizeof(s.szSensorNameUser) - 1] = 0;
            s.szSensorNameOrig[sizeof(s.szSensorNameOrig) - 1] = 0;
            const char* sn = s.szSensorNameUser[0] ? s.szSensorNameUser : s.szSensorNameOrig;
            snprintf(sensName, sizeof(sensName), "%s", sn);
        }

        printf("%-5s  %-40.40s  %14.2f  %14.2f  %-8.8s  %.40s\n",
               type_name(r.tReading), L, r.Value, r.ValueMax, r.szUnit, sensName);
        ++shown;
    }

    if (shown == 0)
        printf("(no CPU-relevant CLOCK/POWER/VOLT/TEMP readings matched - is a CPU sensor active?)\n");
    else
        printf("\n%d CPU-relevant reading(s) shown of %u total.\n", shown, h.dwNumReadingElements);

    if (held) ReleaseMutex(hMutex);
    if (hMutex) CloseHandle(hMutex);
    UnmapViewOfFile(base);
    CloseHandle(hMap);
    return 0;
}

// Made with my soul - Swately <3
