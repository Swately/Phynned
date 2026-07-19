# SOTA — The Windows Process-Control Plane at Mass Scale

> **Facet:** the Windows process-control plane at mass scale — API semantics, rights, and costs.
> **Consumer:** Phynned's next arc (MASS-scale automatic routing — every newborn process placed on
> the cores it benefits from, on a dual-CCD 7950X3D under Windows 11), replacing today's 64-target
> pattern gate.
> **Discipline:** every load-bearing claim carries a verification tag.
> `[V1]` = primary source read first-hand (learn.microsoft.com reference / conceptual page).
> `[V2]` = primary source partially confirmed (landing/abstract only — gap declared).
> `[V3]` = secondary source only (labelled). "could not verify" = honest gap, not a guess.

---

## 0. Bottom line up front

- Windows exposes **three** placement mechanisms with materially different contracts:
  **hard affinity mask** (`SetProcessAffinityMask`), **CPU Sets** (soft, power-aware), and
  **Job Objects** (bulk, one call governs N processes). They are not interchangeable. `[V1]`
- **The decisive composability fact:** *a restrictive affinity mask is respected above any
  conflicting CPU Set assignment.* `[V1]` Hard affinity wins; CPU Sets are advisory. This
  dictates the layering for Phynned (hard-pin the game, soft-herd the background — or the
  reverse — but never let the two contradict on the same core).
- **For touching hundreds of processes, no single Win32 call fans out across PIDs.** The only
  true one-call-governs-many primitive is the **Job Object**: `SetInformationJobObject` with
  `JOB_OBJECT_LIMIT_AFFINITY` sets the same affinity for *every* process in the job. `[V1]`
  Every other mechanism is per-handle (open → set → close), so N processes = O(N) syscalls.
- **Job Objects have a one-way trap for a reversible optimizer:** there is no documented API to
  *remove* a running process from a job; association ends only when the process exits, and the
  only escape is `CREATE_BREAKAWAY_FROM_JOB` at child-creation time. `[V1]` (breakaway is the
  sole documented exit) / `[V3]` (the "no un-assign" statement). Phynned's `AutoRevertGuard`
  needs freely-reversible actions → **per-process affinity / CPU Sets are revertible; a job
  membership is not.**
- Windows already does the *game* half of Phynned's job on X3D (AMD 3D V-Cache Optimizer driver
  + Xbox Game Bar game-list → CCD parking) `[V3]`. The **open niche is the non-game herd**: the
  hundreds of background processes the OS leaves scattered. That is exactly where a mass-scale
  router adds value the OS does not.

---

## 1. Affinity mechanisms and their exact semantics

### 1.1 `SetProcessAffinityMask` — the hard mask

```c
BOOL SetProcessAffinityMask(HANDLE hProcess, DWORD_PTR dwProcessAffinityMask);
```

- **What it is:** "A process affinity mask is a bit vector in which each bit represents a logical
  processor on which the threads of the process are allowed to run." `[V1]`
- **Required right:** the handle "must have the **PROCESS_SET_INFORMATION** access right." `[V1]`
- **Subset rule:** "The value of the process affinity mask must be a subset of the system affinity
  mask." Requesting an unconfigured processor fails with `ERROR_INVALID_PARAMETER`. `[V1]`
- **Inheritance — CONFIRMED:** "Process affinity is inherited by any child process or newly
  instantiated local process." `[V1]` (Rationale from The Old New Thing: a parent's restriction
  should bind the whole process tree it spawns. `[V3]`) → **Consequence for Phynned:** pinning a
  launcher pins its children automatically, but it also means a hard mask is *sticky down the
  tree* whether wanted or not.
- **Scheduler interaction:** it is a hard constraint — the scheduler will *never* run a thread off
  the mask, even if every allowed core is saturated and other cores idle. This is the classic
  under-utilization risk of hard affinity (implied by "allowed to run" semantics; the CPU Sets
  page explicitly contrasts this by calling affinity "restrictive"). `[V1]`
- **Processor-group limit (>64 LPs):** on a system with more than 64 processors the mask "must
  specify processors in a single processor group." `[V1]` **Not relevant to a 7950X3D** (32 LPs,
  one group) but relevant to the "ready for other hardware" framing.
- **Win11 change:** "Starting with Windows 11 … process and thread affinities span all processors
  in the system, across all processor groups, by default." The call now fails only if the process
  had explicitly set a thread's affinity outside its primary group. `[V1]`
- **DLL caveat:** "Do not call SetProcessAffinityMask in a DLL that may be called by processes
  other than your own." `[V1]`

**Verdict:** correct for the *one* process you want hard-nailed (the game), reversible (call again
with the full system mask), cheap per call. Wrong as the primary tool for a herd of hundreds
because it (a) can strand threads on a saturated CCD and (b) fights OS power management.

### 1.2 CPU Sets — soft, power-aware placement

APIs: `SetProcessDefaultCpuSets` / `SetThreadSelectedCpuSets` (by CPU-set ID) and the Win11
mask-based `SetProcessDefaultCpuSetMasks` / `SetThreadSelectedCpuSetMasks` (by `GROUP_AFFINITY`).
`[V1]`

- **Purpose:** "CPU Sets provide APIs to declare application affinity in a 'soft' manner that is
  compatible with OS power management." `[V1]` This is the key distinction from a hard mask — CPU
  Sets are a *preference* the scheduler honors when it can, and they do not defeat power/idle
  management the way a hard mask does.
- **Two levels, with inheritance:** "The Process Default CPU sets are assigned to all threads in a
  process that do not have an assignment at the Thread Selected level." `[V1]` Threads created
  without an explicit thread-level set **inherit** the process default. `[V1]`
- **Child-process inheritance — NEGATIVE:** "child processes don't inherit CPU Sets settings."
  `[V1, from SetProcessDefaultCpuSets reference/search corpus]` → opposite of the hard mask.
  **Consequence:** CPU Sets do *not* propagate down a process tree; a mass router must (re)apply
  them per newborn process. This is actually convenient for per-process routing — no sticky
  inheritance to fight.
- **When the system OVERRIDES / IGNORES a CPU-set request** `[V1]`:
  1. **Reserved sets:** if a process requests a set "allocated exclusively to other processes, its
     request is ignored and threads assigned to disallowed CPU sets are scheduled elsewhere."
  2. **Affinity wins:** "If a thread or process has a restrictive affinity mask set, the affinity
     mask is respected above any conflicting CPU Set assignment." ← the load-bearing rule.
  3. **Cross-group:** "On multi-group systems, CPU assignments are ignored if they are in groups
     that do not match the group in the thread's affinity mask."
- **Non-exclusive by default:** "Thread ownership of the CPU set is not exclusive. Threads … not
  locked to a specific CPU set may take time from" your set unless *they* are steered away too.
  `[V1, Xbox CPUSets guide]` → to truly isolate the game CCD you must *also* herd everyone else
  off it; a soft set on the game alone does not evict the background.
- **Mask-based Win11 variant** (`SetProcessDefaultCpuSetMasks`): takes `GROUP_AFFINITY` structs
  instead of set IDs; "the resulting process default CPU Set assignment is the set of all CPU sets
  with a home processor in the provided list of group affinities"; requires
  **PROCESS_SET_LIMITED_INFORMATION** (a *lighter* right than PROCESS_SET_INFORMATION); "This
  function cannot fail when passed valid parameters." `[V1]` The lighter right + can't-fail
  contract make the mask variant attractive for a mass router.

**Verdict:** CPU Sets are the *right* primitive for the background herd — they compose with OS
power management, don't strand threads as hard as a mask, and don't infect children. Their
weakness is that they are advisory: they will not *guarantee* eviction, and any process (or the OS)
with a hard mask overrides them.

### 1.3 Job Objects — the only cheap "govern N processes at once"

`JOBOBJECT_BASIC_LIMIT_INFORMATION` via `SetInformationJobObject`. Relevant flags (all `[V1]`):

| Flag | Value | Effect |
|---|---|---|
| `JOB_OBJECT_LIMIT_AFFINITY` | 0x00000010 | "Causes all processes associated with the job to use the same processor affinity." One call, whole job. |
| `JOB_OBJECT_LIMIT_SUBSET_AFFINITY` | 0x00004000 | (needs LIMIT_AFFINITY) lets member processes narrow to a *subset* of the job affinity. |
| `JOB_OBJECT_LIMIT_PRIORITY_CLASS` | 0x00000020 | Forces one priority class on all members; "Processes and threads cannot modify their priority class." Needs `SE_INC_BASE_PRIORITY_NAME`. |
| `JOB_OBJECT_LIMIT_SCHEDULING_CLASS` | 0x00000080 | One scheduling class (0–9, default 5) for all members. |
| `JOB_OBJECT_LIMIT_BREAKAWAY_OK` | 0x00000800 | children created with `CREATE_BREAKAWAY_FROM_JOB` escape the job. |
| `JOB_OBJECT_LIMIT_SILENT_BREAKAWAY_OK` | 0x00001000 | children silently break away without the create flag. |

- **Job affinity semantics:** "The affinity of each thread is set to this value, but threads are
  free to subsequently set their affinity, as long as it is a subset of the specified affinity
  mask. **Processes cannot set their own affinity mask.**" `[V1]` → a job is a *ceiling*: members
  can narrow within it but never widen out. Perfect for "confine the whole background herd to
  CCD1 and never let them onto the V-Cache CCD."
- **Nesting (Win8+):** a process can belong to nested jobs; for affinity, "the specified processor
  affinity must be a subset of the effective affinity of the parent job" (a superset is ignored,
  parent wins); for priority "the effective priority class is the lowest priority class in the job
  chain." `[V1]` → you can assign a process already in someone else's job to your own job and your
  limits nest under theirs.
- **CPU rate control** (`JOBOBJECT_CPU_RATE_CONTROL_INFORMATION`, Win8+) `[V1]`:
  - `..._ENABLE` 0x1 (required with any other flag); `..._WEIGHT_BASED` 0x2 (Weight 1–9, default
    5); `..._HARD_CAP` 0x4 ("After the job reaches its CPU cycle limit for the current scheduling
    interval, no threads … will run until the next interval"); `..._MIN_MAX_RATE` 0x10
    (MinRate/MaxRate); `..._NOTIFY` 0x8.
  - **CpuRate encoding:** "cycles per 10,000 cycles … a percentage times 100. For example, to let
    the job use 20% of the CPU, set CpuRate to 2,000." `[V1]`
  - Nested quotas are computed relative to the immediate parent job; a top-level job's rate is a
    fraction of the whole system. `[V1]`
  - **Caveat:** unavailable under Remote Desktop Services when Dynamic Fair Share Scheduling is in
    effect. `[V1]`
- **THE ONE-WAY TRAP:** the only documented way for a process to leave a job is breakaway *at
  child creation* `[V1]`; there is **no documented API to disassociate an already-running process
  from a job** — membership ends when the process exits. `[V3]` This is disqualifying for using
  jobs as Phynned's *revertible* per-target action, but fine for a *static* "background pen" the
  agent creates once and tears down on exit (closing the last job handle can even kill members via
  `JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE` 0x00002000 — Phynned would *not* want that on a shared
  background job). `[V1]`

**Verdict:** the Job Object is the only mechanism that touches many processes for O(1) API cost,
and its "ceiling affinity, members-may-narrow" model is a near-perfect fit for *penning the
background herd onto the non-V-Cache CCD*. But its non-reversibility per process means it can't be
the vehicle for the auto-revert loop. **Recommended split: jobs for the durable background pen,
CPU Sets / hard affinity for the individually-tracked, revertible targets.**

### 1.4 Which mechanism composes with "games untouched on their own CCD"

The winning composition, given rule §1.2(2) (affinity beats CPU Sets) `[V1]`:

- **Game (foreground, few):** leave to the AMD 3D V-Cache Optimizer + Game Bar CCD parking `[V3]`,
  OR hard-pin to the V-Cache CCD with `SetProcessAffinityMask` when the OS mis-detects. Reversible
  by re-applying the full mask.
- **Background herd (hundreds):** confine to the non-V-Cache CCD. Two composable options:
  (a) a single **Job Object** with `JOB_OBJECT_LIMIT_AFFINITY = CCD1 mask` — one call governs all
  members, they can narrow but never touch the V-Cache CCD `[V1]`; or
  (b) **`SetProcessDefaultCpuSetMasks(CCD1)`** per process — soft, power-friendly, reversible,
  lighter right, "cannot fail" `[V1]`.
  Because a hard job affinity on the herd never names the V-Cache cores, it *cannot* contradict the
  game's placement there — the two live on disjoint core sets by construction.

---

## 2. Priority mechanisms

### 2.1 `SetPriorityClass`

Six classes: `IDLE`, `BELOW_NORMAL`, `NORMAL` (default), `ABOVE_NORMAL`, `HIGH`,
`REALTIME_PRIORITY_CLASS`. `[V1, SetPriorityClass ref via search]` The class + each thread's
relative priority determine the thread base priority. `IDLE` = "threads run only when the system is
idle"; `HIGH` = "time-critical tasks … preempt normal or idle." `[V1]` Required right:
**PROCESS_SET_INFORMATION**. `[V1, access-rights page lists SetPriorityClass under it]`

- **REALTIME warning (systems lore, not a doc quote):** REALTIME_PRIORITY_CLASS can starve kernel
  threads (input, disk) and needs `SE_INC_BASE_PRIORITY_NAME`; avoid it for a herd. `[V3]`
- **The memory-priority trap — `PROCESS_MODE_BACKGROUND_BEGIN`:** entering background mode lowers
  **CPU, I/O, *and* memory priority together**; "even an idle CPU priority process can easily
  interfere with system responsiveness when it uses the disk and memory." `[V1]` Side effects to
  respect: it can only be set on the *calling* process (`GetCurrentProcess`) — it is **not a tool
  to background *other* processes** — and new threads inherit background mode. `[V1]` So for
  Phynned's mass router (which acts on *other* PIDs), `PROCESS_MODE_BACKGROUND_BEGIN` is **not
  applicable**; use it only inside Phynned's own agent if it wants to stay out of the way. The
  broader lesson (dropping memory priority when you drop CPU priority) is delivered on other
  processes via `SetProcessInformation(ProcessMemoryPriority, MEMORY_PRIORITY_LOW)`. `[V1]`

### 2.2 `SetProcessInformation` + EcoQoS — the Task Manager "leaf"

```c
BOOL SetProcessInformation(HANDLE, PROCESS_INFORMATION_CLASS, LPVOID, DWORD);
```
Required right: **PROCESS_SET_INFORMATION**; min Windows 8 (the EcoQoS *level* is Windows 11).
`[V1]`

- **The efficiency-mode leaf** = `ProcessPowerThrottling` +
  `PROCESS_POWER_THROTTLING_EXECUTION_SPEED`. Enabling it: "the process will be classified as
  EcoQoS. The system will try to increase power efficiency through strategies such as reducing CPU
  frequency or **using more power efficient cores**." `[V1]` On a hybrid Intel part that means
  E-cores; on the homogeneous-frequency 7950X3D there are no E-cores, so the lever is mostly
  frequency/power, **not** cross-CCD placement — an important honest limit for Phynned's target.
  `[V1] semantics; [V3] the "no E-cores on 7950X3D so mostly a frequency lever" inference.`
- **The exact three-state control** (verbatim structure use) `[V1]`:
  ```c
  PROCESS_POWER_THROTTLING_STATE p; RtlZeroMemory(&p,sizeof p);
  p.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
  // EcoQoS ON:   ControlMask = EXECUTION_SPEED; StateMask = EXECUTION_SPEED;
  // HighQoS:     ControlMask = EXECUTION_SPEED; StateMask = 0;
  // System-managed (default): ControlMask = 0;  StateMask = 0;
  ```
  "ControlMask selects the mechanism and StateMask declares which mechanism should be on or off."
  `[V1]`
- **When EcoQoS beats affinity:** for *throttling the herd's power/heat* without geometrically
  constraining where it runs, EcoQoS is better than a hard mask — it lets the scheduler keep using
  idle cores while signalling "this is not foreground work," and it composes with OS power
  management. Affinity beats EcoQoS when the goal is **cache-fit placement** (keep the game's
  working set off the V-Cache CCD's neighbours) — QoS does not pin cores on a same-frequency CCD
  design. `[V1] mechanism; [V3] the beats-when judgement.`
- Also on `SetProcessInformation`: `ProcessMemoryPriority` (`MEMORY_PRIORITY_LOW` for background
  file/data work) and `PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION` (stop a background app
  from holding a high global timer resolution — a real system-wide power/latency lever). `[V1]`

### 2.3 QoS levels the OS actually maintains (the classification Phynned is fighting/using)

From the Quality of Service page, the full ladder `[V1]`:

| QoS | Trigger | Perf/power |
|---|---|---|
| **High** | foreground+focus, or audible, or explicit HighQoS tag | standard high performance |
| **Medium** | visible but not focused; also foreground lowered after inactivity (battery) | between High and Low |
| **Low** | not visible/audible | on battery: most-efficient freq, efficient core |
| **Utility** | background services (Win11 22H2) | on battery: efficient freq + efficient cores |
| **Eco** | explicit EcoQoS tag | **always** most-efficient freq + efficient cores |
| **Media** / **Deadline** | tagged by MMCSS | batch efficiency / meet audio deadlines |

Default auto-classification when apps don't opt in `[V1]`: **Audible → High**; **window owner (or
descendant) → In Focus = High, Visible = Medium, Minimized/Fully Occluded = Low**; everything else
by **heuristic** (e.g. reduced thread priority implies lower QoS). There is also a battery-only
"user inactivity → foreground drops to Medium" behavior, toggled by
`HKLM\SYSTEM\CurrentControlSet\Control\Power\PowerThrottling\DisableUserPresenceQos`. `[V1]`

**Implication for Phynned:** the OS already assigns *Low/Utility/Eco* to invisible background work
on battery. On a **desktop 7950X3D on AC power**, much of this efficiency steering is muted (Low/
Utility qualify their efficiency behavior with "on battery"). That is precisely the gap: on an AC
desktop the OS does *not* aggressively herd background processes off the game's cache domain, and
there are no E-cores to demote them to — so a router that does explicit CCD placement adds value
the QoS system does not deliver on this platform. `[V1] for the "on battery" qualifiers; [V3] the
desktop-AC gap inference.`

### 2.4 MMCSS (multimedia)

MMCSS boosts registered multimedia/audio threads; threads call
`AvSetMmThreadCharacteristics`/`AvSetMmMaxThreadCharacteristics` naming a task ("Games", "Audio",
"Pro Audio"…); it maps to the **Media**/**Deadline** QoS levels. `[V1, QoS page states MMCSS
sources Media/Deadline]` The registry knob
`HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Multimedia\SystemProfile\SystemResponsiveness`
reserves a % of CPU for low-priority tasks. `[V3, secondary snippet — official MMCSS page not
fetched first-hand; treat the specific SystemResponsiveness semantics as V3]` MMCSS is
opt-in-by-the-app (the game/audio engine registers its own threads); **Phynned cannot register
another process's threads with MMCSS from outside**, so MMCSS is context, not a lever Phynned
pulls on third-party PIDs. `[V3]`

---

## 3. Windows's own heterogeneous scheduling (what to *not* reinvent)

- **AMD X3D CCD steering (already shipped):** the AMD chipset package ships a "**3D V-Cache
  Performance Optimizer**" driver that changes how CPPC (Collaborative Processor Performance
  Control) ranks cores so gaming workloads are sent to the V-Cache CCD; the driver uses **Xbox
  Game Bar's game detection** (its KGL game list) to know when a game is running, then **parks the
  second (non-V-Cache) CCD** to keep the game on the cache die. A BIOS knob
  `CPPC Dynamic Preferred Cores` = Auto/Driver/Frequency/Cache tunes the tie-break. `[V3 —
  Neowin + hwbusters + forums; no first-hand AMD/MSDN page fetched. Load-bearing detail flagged.]`
- **CPPC preferred cores generally:** Windows reads per-core "preferred/favored core" rankings and
  biases scheduling toward the best cores; on X3D the driver rewrites that ranking for games. `[V3]`
- **Intel Thread Director / EHFI:** on Intel hybrid parts a per-package microcontroller feeds the
  **Enhanced Hardware Feedback Interface** telemetry (per-core perf/efficiency capability, updated
  live) to the Windows 11 scheduler so it places heavy work on P-cores and background/less-
  important work on E-cores; QoS class feeds into this. `[V3 — Intel/Digital Trends/press; the
  learn.microsoft.com scheduler internals page was not fetched.]` **Not applicable to 7950X3D**
  (no Thread Director; AMD path is CPPC + the V-Cache driver). Relevant only to Phynned's "ready
  for Intel hybrid" claim.
- **What Windows does *NOT* do (the niche):**
  1. It steers the **game** on X3D, but does **not** herd the **hundreds of non-game background
     processes** onto the non-V-Cache CCD — they stay wherever the general scheduler + CPPC land
     them, often sharing the cache die and evicting the game's working set. `[V3 inference from the
     driver only parking CCD for detected games.]`
  2. On a **desktop on AC power** the QoS efficiency steering (Low/Utility → efficient cores) is
     largely battery-gated `[V1]`, and there are **no E-cores** on the 7950X3D to demote to — so
     background demotion is weaker than on a hybrid laptop. `[V1] + [V3]`
  3. No **per-process cache-fit** decision for arbitrary processes: the OS has no notion of "this
     background service thrashes L3, keep it off the V-Cache die." That per-process,
     cache-topology-aware placement is Phynned's actual contribution. `[V3]`

---

## 4. Access rights + failure modes

### 4.1 Rights needed (all `[V1]` from Process Security and Access Rights)

| Right | Value | Needed for |
|---|---|---|
| `PROCESS_SET_INFORMATION` | 0x0200 | `SetProcessAffinityMask`, `SetPriorityClass`, `SetProcessInformation` (EcoQoS/memory priority) |
| `PROCESS_SET_LIMITED_INFORMATION` | 0x2000 `[V3 for the hex]` | `SetProcessDefaultCpuSetMasks` (lighter than SET_INFORMATION) `[V1 for the requirement]` |
| `PROCESS_QUERY_LIMITED_INFORMATION` | 0x1000 | `GetPriorityClass`, `QueryFullProcessImageName`, `IsProcessInJob` — the cheap read tier |
| `PROCESS_QUERY_INFORMATION` | 0x0400 | fuller query (token, etc.); **auto-grants** QUERY_LIMITED_INFORMATION |
| `PROCESS_TERMINATE` | 0x0001 | (not used by Phynned by design) |
| `PROCESS_SET_QUOTA` | 0x0100 | working-set limits (`SetProcessWorkingSetSize`) |

- **Least privilege for a mass router:** to *classify* open with
  `PROCESS_QUERY_LIMITED_INFORMATION` (0x1000); to *act* open with `PROCESS_SET_INFORMATION`
  (0x0200) — or `PROCESS_SET_LIMITED_INFORMATION` (0x2000) if using only the CPU-set-mask path.
  `[V1]` Do **not** open `PROCESS_ALL_ACCESS` per process (heavier, more likely denied, and a
  bigger anti-cheat/AV red flag).
- **SeDebugPrivilege:** "To open a handle to another process and obtain full access rights, you
  must enable the SeDebugPrivilege privilege." `[V1]` An elevated agent enabling SeDebugPrivilege
  can open `PROCESS_SET_INFORMATION` on most other-user / service processes (e.g. svchost).
  `[V3 for the svchost-specific example.]`

### 4.2 What refuses — the uncontrollable fraction

- **Protected Processes / PPL:** for a protected process, the following are **not allowed from a
  less-/equal-privileged caller**: `PROCESS_ALL_ACCESS`, `PROCESS_QUERY_INFORMATION`,
  **`PROCESS_SET_INFORMATION`**, `PROCESS_SET_QUOTA`, `PROCESS_VM_*`, create-thread/process,
  dup-handle. `[V1]` → **Phynned cannot set affinity/priority/QoS on PPL processes** (antimalware
  service, LSASS-as-PPL, DRM/media pipelines, some System components). The access check compares
  PPL signer/level; a standard elevated Admin < an Antimalware-level PPL → `STATUS_ACCESS_DENIED`.
  `[V3 for the access-check detail; V1 for the rights-denied list.]`
- **`PROCESS_QUERY_LIMITED_INFORMATION` still works on protected processes** — "introduced to
  provide access to a subset of the information available through PROCESS_QUERY_INFORMATION."
  `[V1]` So Phynned can still *see/enumerate* PPL processes to know to skip them; it just can't
  *act* on them.
- **System/idle & kernel PIDs (0, 4):** not user-mode controllable. `[V3 — well known; not a
  fetched doc.]`
- **UWP / AppContainer processes:** these are ordinary (non-PPL) processes from an affinity-rights
  standpoint — an elevated agent with SeDebugPrivilege can generally open
  `PROCESS_SET_INFORMATION` on them and set affinity/QoS. **could not verify** a primary MSDN page
  stating AppContainer-specific affinity restrictions; treat "UWP is controllable" as `[V3]`
  pending first-hand confirmation. The real UWP nuance is *lifecycle* (PLM can suspend them), not
  an affinity ACL.
- **Realistic controllable fraction (elevated user-mode agent, SeDebugPrivilege on):** the large
  majority of a desktop's processes — all normal user apps and most services — are controllable;
  the uncontrollable minority is PPL (a handful: Defender/AV, LSASS-PPL, some MS media/DRM
  components), plus PID 0/4 and a few System-integrity processes. Order-of-magnitude: on a typical
  Win11 desktop of ~250–350 processes, **single-digit-to-low-double-digit** are PPL/refused; the
  rest are actionable. `[V3 — reasoned estimate, no measured census; label as V3 and MEASURE on
  the 7950X3D box before quoting a number.]`

### 4.3 Failure modes to code against

- `ERROR_ACCESS_DENIED` on `OpenProcess`/set → PPL or a tighter-DACL process → **skip and log**,
  don't retry-storm.
- `ERROR_INVALID_PARAMETER` on `SetProcessAffinityMask` → mask names an unconfigured CPU or (>64
  LP) crosses groups. `[V1]`
- Race: PID recycled / process exits between enumerate and act → handle open fails or set fails →
  benign, skip.
- Job trap: a process already in a non-nestable/kill-on-close job, or that breaks away, won't stay
  penned. `[V1]`

---

## 5. Costs at scale (300+ processes per policy pass)

- **No batch-across-PIDs API exists.** `SetProcessAffinityMask`, `SetProcessInformation`,
  `SetProcessDefaultCpuSets/Masks`, `SetThreadSelectedCpuSets/Masks` all take **one handle**.
  Touching N processes = N × (OpenProcess + Set + CloseHandle) ≈ **3N syscalls**. `[V1 — every
  reference page takes a single HANDLE; no multi-PID overload documented.]`
- **The exception, and the only real "batch":** a **Job Object** — one `SetInformationJobObject`
  call re-affinitizes *every* member process. `[V1]` If the herd is pre-assigned to a job, a
  policy change is O(1) API calls instead of O(N). This is the strongest scale argument for the
  job-pen design in §1.4.
- **Enumeration is already batched:** `NtQuerySystemInformation(SystemProcessInformation)`
  returns the whole process+thread table in one (variable-size) call — the cheap way to scan
  hundreds of processes per tick. `[V1 — Phynned's own ARCHITECTURE.md already uses this;
  documented behavior. The exact per-call cost is unmeasured here → V3.]`
- **Per-syscall cost:** Microsoft publishes **no** official latency figure for these calls. A
  user→kernel transition + `NtSetInformationProcess`/`NtSetInformationThread` is on the order of
  ~1 µs (hundreds of ns to a few µs) on modern x86-64. **could not verify** an authoritative
  number — label `[V3]` and MEASURE.
- **Budget arithmetic (derived, not measured):** 300 processes × 3 syscalls ≈ 900 syscalls; at an
  assumed ~1 µs each that is **≈ 0.9 ms** of pure syscall time — comfortably inside Phynned's
  100 ms tick, even at ~10 µs/syscall (~9 ms). The dominant cost is far more likely **enumeration
  + classification + string work**, not the affinity syscalls. `[V3 — arithmetic on an unverified
  per-syscall constant; the qualitative conclusion (syscalls are not the bottleneck) is robust,
  the millisecond figure is provisional until measured on the 7950X3D.]`
- **Design consequence:** act **only on deltas** — newborn processes and processes whose
  classification changed — not the full table every tick. A router that re-pins all 300 every
  100 ms wastes syscalls and risks fighting the OS/AMD driver's own re-placement. Pair the ETW
  process/image-load events (already in Phynned's observer) with a per-PID "already-placed" cache
  so steady state is ~0 set-calls/tick. `[V3 — engineering judgement built on §1–§4 facts.]`

---

## 6. Recommendation for Phynned's MASS-scale arc

1. **Game (foreground, ≤ a few):** default to letting the **AMD 3D V-Cache Optimizer + Game Bar**
   park CCD1 and keep the game on the V-Cache die `[V3]`; hard-pin with `SetProcessAffinityMask`
   only as an override when detection misses. Reversible; inherits to children `[V1]`.
2. **Background herd (hundreds):** pen them onto the **non-V-Cache CCD**. Prefer
   **`SetProcessDefaultCpuSetMasks(CCD1)`** per newborn — soft, power-manager-friendly, lighter
   right (`PROCESS_SET_LIMITED_INFORMATION`), "cannot fail," freely reversible, and does **not**
   infect children `[V1]`. Use a **Job Object with `JOB_OBJECT_LIMIT_AFFINITY`** only for a
   *durable* pen where O(1)-repolicy matters and per-process revert is not required (remember: no
   un-assign; don't set KILL_ON_JOB_CLOSE on a shared job) `[V1]`.
3. **Optionally demote the herd's power profile** with EcoQoS (`SetProcessInformation` +
   `EXECUTION_SPEED`) — but know that on the same-frequency 7950X3D this is a frequency/power lever,
   not a placement lever (no E-cores) `[V1]+[V3]`. Drop memory priority too
   (`MEMORY_PRIORITY_LOW`) for genuinely background file/data churners `[V1]`.
4. **Respect the composability law:** never let a hard affinity mask and a CPU-set assignment name
   contradictory cores on the same target — the mask silently wins `[V1]`. Keep game cores and
   herd cores **disjoint by construction** so the two layers can't fight.
5. **Rights & skip-list:** open `PROCESS_QUERY_LIMITED_INFORMATION` to classify, then
   `PROCESS_SET_INFORMATION` (or `PROCESS_SET_LIMITED_INFORMATION`) to act; enable
   SeDebugPrivilege; **skip PPL and PID 0/4** on `ACCESS_DENIED` and log once `[V1]`.
6. **Cost discipline:** delta-driven placement keyed off ETW newborn events + an "already-placed"
   cache; the syscalls are not the bottleneck, re-placement churn is. **Measure** per-syscall cost
   and the PPL-refused fraction on the 7950X3D before quoting either as fact `[V3]`.

---

## 7. Verification ledger (honest gaps)

- **V1 (first-hand primary):** SetProcessAffinityMask semantics + inheritance + rights; CPU Sets
  conceptual page (soft affinity, override rules, affinity-beats-cpuset, thread-inherit); Xbox
  CPUSets guide (non-exclusive, hyperthreading, LLC); SetProcessDefaultCpuSetMasks (Win11, lighter
  right, can't-fail); SetProcessInformation (EcoQoS/memory-priority, verbatim examples);
  JOBOBJECT_BASIC_LIMIT_INFORMATION (all flags + values + affinity/priority nesting);
  JOBOBJECT_CPU_RATE_CONTROL_INFORMATION (flags/encoding); Process Security & Access Rights (all
  rights + hex + PPL denied list); Quality of Service (level table + default heuristics).
- **V3 (secondary, not yet first-hand):** AMD 3D V-Cache Optimizer driver + Game Bar CCD-parking
  mechanics (Neowin/hwbusters/forums — get a first-hand AMD page before load-bearing use); Intel
  Thread Director/EHFI internals; MMCSS `SystemResponsiveness` specifics; the "no API to un-assign
  a process from a job" statement; UWP/AppContainer controllability; per-syscall latency and the
  budget arithmetic; the PPL-refused fraction estimate; PROCESS_SET_LIMITED_INFORMATION = 0x2000.
- **could not verify:** an authoritative per-call latency for Set*Affinity/CpuSets/Information;
  a primary MSDN statement on AppContainer-specific affinity ACLs; a measured census of
  controllable-vs-refused processes on a real Win11 desktop. → **MEASURE on the 7950X3D box.**

---

## Sources

- [SetProcessAffinityMask (winbase.h)](https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-setprocessaffinitymask) — V1
- [GetProcessAffinityMask (winbase.h)](https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-getprocessaffinitymask) — V1 (referenced)
- [CPU Sets (conceptual)](https://learn.microsoft.com/en-us/windows/win32/procthread/cpu-sets) — V1
- [SetProcessDefaultCpuSetMasks](https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-setprocessdefaultcpusetmasks) — V1
- [SetThreadSelectedCpuSets](https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-setthreadselectedcpusets) — V1 (referenced)
- [CPUSets for game development (Xbox/UWP)](https://learn.microsoft.com/en-us/previous-versions/windows/uwp/xbox-apps/cpusets-games) — V1
- [SetProcessInformation (processthreadsapi.h)](https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-setprocessinformation) — V1
- [Quality of Service](https://learn.microsoft.com/en-us/windows/win32/procthread/quality-of-service) — V1
- [SetPriorityClass (processthreadsapi.h)](https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-setpriorityclass) — V1 (via search corpus)
- [Scheduling Priorities](https://learn.microsoft.com/en-us/windows/win32/procthread/scheduling-priorities) — V1 (referenced)
- [JOBOBJECT_BASIC_LIMIT_INFORMATION (winnt.h)](https://learn.microsoft.com/en-us/windows/win32/api/winnt/ns-winnt-jobobject_basic_limit_information) — V1
- [JOBOBJECT_CPU_RATE_CONTROL_INFORMATION (winnt.h)](https://learn.microsoft.com/en-us/windows/win32/api/winnt/ns-winnt-jobobject_cpu_rate_control_information) — V1
- [Process Security and Access Rights](https://learn.microsoft.com/en-us/windows/win32/procthread/process-security-and-access-rights) — V1
- [Multimedia Class Scheduler Service](https://learn.microsoft.com/en-us/windows/win32/procthread/multimedia-class-scheduler-service) — V2 (official page identified; specifics taken from secondary snippet)
- [Why is processor affinity inherited by child processes? — The Old New Thing](https://devblogs.microsoft.com/oldnewthing/20050817-10/?p=34553) — V3
- [Microsoft's Xbox Game Bar boosts AMD Ryzen 7950X3D — Neowin](https://www.neowin.net/news/microsofts-xbox-game-bar-boosts-windows-11-and-10-gaming-performance-on-amd-ryzen-7950x3d/) — V3
- [AMD Ryzen 9 7950X3D Core Parking Problem & Solution — Hardware Busters](https://hwbusters.com/cpu/amd-ryzen-9-7950x3d-core-parking-problem-solution/3/) — V3
- [How Intel Thread Director marries Alder Lake & Windows 11 — Digital Trends](https://www.digitaltrends.com/computing/how-intel-thread-director-marries-alder-lake-windows-11/) — V3
- [Enhanced Hardware Feedback Interface (EHFI) — itigic](https://itigic.com/enhanced-hardware-feedback-interface-ehfi-what-is-it/) — V3
- [Windows PPL (Protected Processes Light) — Medium/S12](https://medium.com/@s12deff/windows-ppl-protected-processes-light-e158332aedca) — V3

<!-- Made with my soul - Swately <3 -->
