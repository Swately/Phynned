# HWiNFO Shared Memory — optional enhanced telemetry for Phynned

**Date:** 2026-07-17 · **Author:** Claude (Opus 4.8) for Swately · **Risk:** user-mode only, low.

Goal: read HWiNFO's per-core clock (current + max), package power / PPT, VID, and CPU
temps from its published **shared memory**, the same interface RTSS/RivaTuner and OSD
tools consume — **with no kernel code of ours**. HWiNFO's own signed driver reads the MSRs
and publishes the numbers into a read-only memory-mapped file; Phynned only maps and parses
that file. No MSR access of ours, no driver of ours.

Verification tags: **[V1]** first-hand on this box / primary source read directly ·
**[V2]** read first-hand from production reader source + cross-confirmed by ≥2 independent
implementations · **[V3]** secondary / widely cited but not confirmed first-hand.

---

## 1. The interface

- **Named file mapping (local):** `Global\HWiNFO_SENS_SM2` **[V2]** — the "Shared Memory
  Support" feature. Confirmed first-hand in the source of the Seraksab production library
  (`SharedMemoryReader.cs`, const `HWiNfoSensorsMapFileNameLocal`), cross-checked against the
  PromDapterHWiNFO C# provider and namazso's reverse-engineered gist.
- **Writer mutex (optional sync):** `Global\HWiNFO_SM2_MUTEX` **[V2]** — hold it while reading
  to avoid a torn read against HWiNFO's writer. Same source.
- **Remote variant:** `Global\HWiNFO_SENS_SM2_REMOTE_<index>` **[V2]** — not needed here.
- **Signature:** ASCII `"SiWH"` when valid, `"DEAD"` while HWiNFO is (re)initializing the
  block **[V3]**. Could NOT confirm the exact byte value first-hand (official forum SDK
  thread returned HTTP 403 / Cloudflare; the primary `HWiNFO_SENSORS_SM2.h` header was not
  fetchable). The production reader does **not** validate the signature at all — so our reader
  **prints** the four signature bytes and validates the block **structurally** (sane
  offsets/sizes/counts within the mapping) rather than asserting a magic value.

### Header — `HWiNFO_SENSORS_SHARED_MEM2` (Pack=1, 44 bytes) **[V2]**

Verified against `SharedMemoryElements.cs` (`[StructLayout(LayoutKind.Sequential, Pack = 1,
CharSet = CharSet.Ansi)]`) read first-hand, cross-checked with namazso's byte offsets.

| off  | field                       | type        |
|------|-----------------------------|-------------|
| 0x00 | `dwSignature`               | uint32      |
| 0x04 | `dwVersion`                 | uint32      |
| 0x08 | `dwRevision`                | uint32      |
| 0x0C | `poll_time`                 | int64 (`__time64_t`) |
| 0x14 | `dwOffsetOfSensorSection`   | uint32      |
| 0x18 | `dwSizeOfSensorElement`     | uint32      |
| 0x1C | `dwNumSensorElements`       | uint32      |
| 0x20 | `dwOffsetOfReadingSection`  | uint32      |
| 0x24 | `dwSizeOfReadingElement`    | uint32      |
| 0x28 | `dwNumReadingElements`      | uint32      |

### Reading element — `HWiNFO_SENSORS_READING_ELEMENT` (Pack=1, 316 bytes) **[V2]**

| off   | field           | type       | note |
|-------|-----------------|------------|------|
| 0x000 | `tReading`      | uint32 enum| SENSOR_READING_TYPE |
| 0x004 | `dwSensorIndex` | uint32     | index into the sensor[] section |
| 0x008 | `dwReadingID`   | uint32     | unique within its sensor |
| 0x00C | `szLabelOrig`   | char[128]  | e.g. `"Core 0 Clock"` |
| 0x08C | `szLabelUser`   | char[128]  | user-renamed label (may be empty) |
| 0x10C | `szUnit`        | char[16]   | e.g. `"MHz"`, `"W"`, `"V"`, `"°C"` |
| 0x11C | `Value`         | double     | **current** |
| 0x124 | `ValueMin`      | double     |      |
| 0x12C | `ValueMax`      | double     | **max** |
| 0x134 | `ValueAvg`      | double     |      |

> **Load-bearing packing note.** `szUnit` ends at 0x11C, which is **not** 8-byte aligned.
> Under *natural* alignment a compiler would pad `Value` to 0x120 (element = 320 bytes) →
> every double would be read from the wrong offset = garbage. The real block is **Pack=1**
> (confirmed first-hand from the production library's `StructLayout(Pack = 1)` and namazso's
> observed 0x11C offset), so `Value` is at **0x11C** and the element is **316 bytes**. Our
> reader forces this with `#pragma pack(1)` and *proves it at compile time* via
> `static_assert(offsetof(ReadingSM2, Value) == 0x11C)` and `sizeof == 316`.

### Sensor element — `HWiNFO_SENSORS_SENSOR_ELEMENT` (Pack=1, 264 bytes) **[V2]**

`dwSensorID` (u32, 0x00), `dwSensorInst` (u32, 0x04), `szSensorNameOrig[128]` (0x08),
`szSensorNameUser[128]` (0x88). A reading's `dwSensorIndex` indexes this array, giving the
owning sensor name, e.g. `"CPU [#0]: AMD Ryzen 9 7950X3D"`.

### `SENSOR_READING_TYPE` enum **[V2]**

`NONE=0, TEMP=1, VOLT=2, FAN=3, CURRENT=4, POWER=5, CLOCK=6, USAGE=7, OTHER=8`
(confirmed identical in Seraksab + PromDapter + namazso).

---

## 2. How to open + parse (the robust recipe)

1. `OpenFileMappingA(FILE_MAP_READ, FALSE, "Global\\HWiNFO_SENS_SM2")`. If it fails with
   `ERROR_FILE_NOT_FOUND (2)`, the feature is off → bail with the enable message.
2. `MapViewOfFile(hMap, FILE_MAP_READ, 0, 0, 0)`; `VirtualQuery` the base to get the region
   size for bounds checks.
3. *(optional, recommended)* `OpenMutexA(SYNCHRONIZE, ..., "Global\\HWiNFO_SM2_MUTEX")` and
   `WaitForSingleObject(…, 1000)` around the read to avoid a torn snapshot; release after.
4. `memcpy` the 44-byte header. **Stride the sections by the header's `dwSizeOf*Element`,
   never by `sizeof(struct)`** — this is what the production reader does, so a future HWiNFO
   revision that *appends* fields still parses. Element `i` lives at
   `readingBase + i * dwSizeOfReadingElement`.
5. For each reading: NUL-terminate the label/unit buffers defensively, then filter.

### Identifying per-core clocks (and the rest)

On a 7950X3D, HWiNFO exposes per-core rows under the CPU sensor:
- **Per-core clock:** `tReading == CLOCK` and label contains `"Core"` (e.g. `"Core 0 Clock"`,
  `"Core 0 T0 Effective Clock"`), unit `MHz`. `Value` = current, `ValueMax` = session max.
- **Per-core VID:** `tReading == VOLT` and label contains `"VID"`/`"Core"`, unit `V`.
- **Package power / PPT:** `tReading == POWER` and label contains `"CPU"`/`"Package"`/`"PPT"`,
  unit `W`.
- **Core temps:** `tReading == TEMP` and label contains `"Core"`/`"CPU"`, unit `°C`.

Exact label strings vary by CPU/HWiNFO version, so the reader matches by **type + case-
insensitive substring**, not by hard-coded `dwReadingID`. (If Phynned later wants a stable
binding it can cache `dwSensorID`+`dwReadingID` after a first match — those are stable for a
given HWiNFO config **[V3]**.)

---

## 3. Enable requirement + free-version limit

- HWiNFO → **Settings** → main/General tab → check **"Shared Memory Support"**. The **Sensors
  window must be open** (or minimized to tray) — closing sensors tears the mapping down. **[V2]**
- **Free (non-Pro) version:** "Shared Memory Support" **auto-disables after 12 hours** of
  runtime and must be re-enabled; HWiNFO Pro keeps it persistent. **[V3]** (widely documented
  on the HWiNFO forum; not confirmed first-hand — the forum thread was not fetchable, 403.)

---

## 4. State on THIS box (2026-07-17) **[V1]**

- `HWiNFO64.EXE` **is running** (PID 13092, verified via `tasklist`).
- The mapping `Global\HWiNFO_SENS_SM2` is **NOT present** — `OpenFileMappingA` returns
  `ERROR_FILE_NOT_FOUND (2)` (also absent without the `Global\` prefix).
- **Diagnosis:** HWiNFO is up but **"Shared Memory Support" is OFF**. `hwinfo-shm.exe`
  printed the clean not-available message and exited **2**. No readings were fabricated.
- To make it live: enable the toggle above, keep the Sensors window open, re-run `hwinfo-shm.exe`.

---

## 5. Integration verdict for Phynned

**Treat HWiNFO SHM as OPTIONAL enhanced telemetry, never a dependency.**

- **When present** (HWiNFO running + Shared Memory Support on): Phynned gets rich, per-core,
  MSR-grade numbers — effective clock per core, PPT/package watts, per-core VID, temps —
  **without shipping or loading any driver of ours**. Excellent for the affinity/V-Cache
  measurement questions ("which process actually benefits from the V-Cache CCD", per the
  queued AAP re-architecture) where per-core effective clock is exactly the signal wanted.
- **The catch:** it is gated on a third-party app the user must run, with the correct toggle,
  and (on free HWiNFO) a 12-hour self-disable. So it cannot be Phynned's baseline telemetry.
- **No-dependency fallback = PDH `% Processor Performance`** (`\Processor Information(*)\%
  Processor Performance`, and its `Processor Frequency` companion). Ships with Windows, needs
  no external app and no driver, gives per-logical-processor relative clock. Lower fidelity
  than HWiNFO's effective-clock, but always available. **[V3]** (standard PDH counter set.)

**Recommendation:** Phynned's telemetry layer probes for the SHM at startup; if live, it
surfaces the enhanced HWiNFO readings; if absent, it silently falls back to PDH and logs a
one-line "HWiNFO SHM not detected — using PDH" note. Never block on HWiNFO.

---

## Sources

- Seraksab `Hwinfo.SharedMemory.Net` — `SharedMemoryElements.cs` / `SharedMemoryReader.cs`
  (read first-hand; `StructLayout(Pack = 1)`, map + mutex names). **[V2]**
- PromDapterHWiNFO C# provider (field order, enum). **[V2]**
- namazso reverse-engineered HWiNFO SM gist (byte offsets, signature). **[V2/V3]**
- HWiNFO forum "Shared Memory Support" (enable steps, 12h limit) — **not fetched first-hand
  (HTTP 403)**; via search summary. **[V3]**
- This box: `tasklist` + `hwinfo-shm.exe` run. **[V1]**

Made with my soul - Swately <3
