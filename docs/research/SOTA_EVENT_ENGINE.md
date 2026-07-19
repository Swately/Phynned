# SOTA_EVENT_ENGINE — Event-driven new-process detection and the overhead engineering of a mass-scale agent

**Facet:** event-driven new-process detection + the overhead engineering of a mass-scale
agent (route every newborn process at birth, at MASS scale, on Windows 11 / AMD 7950X3D).
**Operator requirement:** *"lo más barato y con máximo rendimiento posible"* — the agent
itself must cost near-nothing.
**Date:** 2026-07-17. **Author:** research session (Opus) for Swately.

## Verification legend

- **[V1]** — primary source read first-hand this session (Microsoft Learn page, WDK header
  semantics, or Phynned source read directly).
- **[V1-int]** — Phynned's own internal claim (code comment / doc) read first-hand. The *read*
  is V1; the *measurement itself* is the project's, **not re-run by me** — treat the number as
  a project assertion, not an independently reproduced benchmark.
- **[V2]** — primary source read, but the specific number is partial / mechanism-derived / a
  vendor self-report (declared each time).
- **[V3]** — secondary source only (blog, GUID list, undocumented-internals writeup); not
  independently confirmable against a primary.
- **[DERIVED]** — arithmetic/estimate I computed from a V1 anchor; the method is shown so the
  operator can check it.

Numbers below that would drive a design decision and that I could NOT verify are marked
**"could not verify — needs a bench on the 7950X3D."**

---

## 0. TL;DR — decisive findings (tagged)

1. **No user-mode mechanism can suspend an arbitrary externally-launched process *before* its
   first thread runs.** Every user-mode notifier (ETW, WMI, job object, WNF) fires *after* the
   process object and its first thread already exist and are schedulable. Suspend-at-birth is
   strictly kernel-driver territory (`PsSetCreateProcessNotifyRoutineEx`), and even there you
   suspend/deny, you do not "pre-configure." **[V1]** The honest user-mode ceiling is: *route a
   few milliseconds to ~1 second after the first thread has already started.*

2. **ETW Microsoft-Windows-Kernel-Process is the right user-mode birth notifier.** ProcessStart
   (Event ID 1) carries `ProcessID`, `CreateTime`, `ParentProcessID`, `SessionID`, `ImageName`
   at spawn — enough to route at birth without any follow-up query. Provider GUID
   `{22FB2CD6-0E7B-422B-A0C7-2FAD1FD0E716}`, keyword `WINEVENT_KEYWORD_PROCESS = 0x10`, admin
   required. **[V1]**

3. **BUG FOUND — Phynned's `kKernelProcess` GUID is wrong.** The code uses
   `{0268a8b6-74fd-4302-9b4a-6ea0fbb19d9e}`; the real Microsoft-Windows-Kernel-Process GUID is
   `{22FB2CD6-0E7B-422B-A0C7-2FAD1FD0E716}`. **[V1]** Process/thread events from that spec are
   almost certainly never being delivered. Verify on-box with `logman query providers
   Microsoft-Windows-Kernel-Process`.

4. **BUG FOUND — Phynned's `kKernelContextSwitch` GUID is actually StackWalkGuid.**
   `{def2fe46-7bd6-4b80-bd94-f57fe20d0ce3}` is the kernel **StackWalk** provider, not a
   scheduler/dispatcher provider. **[V2/V3]** CSwitch cannot come from it.

5. **CSwitch in a private real-time session requires `EVENT_TRACE_SYSTEM_LOGGER_MODE`** plus the
   **System Scheduler Provider** `{599a2a76-4d91-4910-9ac7-7d33f2e97a6c}` with keyword
   `SYSTEM_SCHEDULER_KW_CONTEXT_SWITCH` (= `PERF_CONTEXT_SWITCH` / `EVENT_TRACE_FLAG_CSWITCH`),
   on Windows build 20348+. Phynned's session sets only `EVENT_TRACE_REAL_TIME_MODE`, so its
   context-switch path is doubly broken (wrong GUID + wrong session mode). **[V1]**

6. **WMI `__InstanceCreationEvent … WITHIN` is the worst option for this facet:** polling under
   the hood (default 5 s), misses any process that lives < the interval, and each poll
   re-enumerates all `Win32_Process` instances. Its extrinsic cousin `Win32_ProcessStartTrace`
   is kernel-backed (no polling) but is just a heavier wrapper over the same ETW kernel data —
   prefer ETW directly. **[V1/V2]**

7. **Job objects are NOT a system-wide birth notifier.** `JOB_OBJECT_MSG_NEW_PROCESS` only fires
   for descendants of processes *you* placed in the job; delivery is explicitly best-effort
   ("not guaranteed"); PID may already be dead/recycled on receipt. Useful only if Phynned
   becomes the launcher. **[V1]**

8. **One ETW session can serve process-birth + CSwitch — if it is a system-logger session.** On
   Win11 (20348+) a single `EVENT_TRACE_SYSTEM_LOGGER_MODE` session can enable both the System
   Process Provider `{151f55dc-…}` (birth/death/image-load/thread) and the System Scheduler
   Provider (CSwitch). The manifest Kernel-Process provider does **not** emit CSwitch, so if you
   keep birth on the manifest provider you need a *second* session for CSwitch. **[V1]**

9. **Pairing a low-volume provider (process-birth) with a high-volume one (CSwitch) in one
   session collapses birth latency.** Real-time buffers deliver on *buffer-full OR FlushTimer*
   (min 1 s). Alone, a birth event can wait up to the flush timer (~1 s). Sharing buffers with
   CSwitch traffic fills+flushes them in milliseconds. **[V2]** This is a real, cheap latency
   lever — but it costs the full CSwitch event rate.

10. **CSwitch is the dominant ETW cost by far.** ETW itself handles ~100k events/s comfortably,
    but CSwitch on a busy desktop is tens-of-thousands to >100k events/s and can hit millions
    over tens of seconds. **[V2]** Use 64–128 KB buffers for it (Microsoft's own guidance for a
    few-MB/s stream). **[V1]** For "route at birth," **you do not need CSwitch at all** —
    process-birth is a rare event stream.

11. **Full-table `NtQuerySystemInformation(SystemProcessInformation)` stays sub-millisecond even
    at 800 processes.** Phynned measures ~80–150 µs at ~200 processes **[V1-int]**; the cost is
    ~linear in (processes + total threads), so ~800 processes ≈ **~0.3–0.6 ms [DERIVED]** plus a
    trivial O(N log N) sort. This is cheap enough that a periodic reconcile pass is affordable
    even at MASS scale — **could not verify the 400/800 points; needs a bench on the 7950X3D.**

12. **The current per-PID delta state is fat because of per-thread tracking.** `PidState` ≈
    **1096 B/PID [DERIVED, matches the "~1.1 KB" claim]**, of which **1024 B is
    `prev_threads[64]×16 B`** (hot-thread identification). A process-level-only state is
    **~48–56 B/PID [DERIVED]**. Dropping per-thread tracking for non-hot processes is a ~95%
    memory cut — the single biggest scale lever.

13. **Two hard structural caps block MASS scale today.** `kMaxTargets = 64` **[V1]** and the
    `pid_hash` slot index is `uint8_t` (`slot_plus_1`) → a **~254-PID ceiling** baked into the
    hash **[V1]**. Both must widen (`uint16_t` slot, larger arrays) before "every newborn
    process" is literally every process.

14. **Event-driven reconciliation lets the full NtQSI pass run rarely.** With births arriving via
    ETW, the periodic full-table snapshot is only needed for (a) initial population and (b)
    slow-drift correction (missed events, PID recycle). A reconcile every **2–5 s** is ample;
    the 100 ms Active tick does *not* need a full scan. **[DERIVED from mechanism]**

15. **Batched control ops: apply to the dirty set only, open/close per op.** `OpenProcess` +
    `SetPriorityClass`/`SetProcessAffinityMask` + `CloseHandle` is ~single-digit µs per process
    **[V2]**. At birth you touch exactly one process, so caching handles buys almost nothing on
    the hot path and adds handle-table pressure + zombie-EPROCESS pinning. **Cache handles only
    where PID-recycle safety is the goal**, not for speed.

16. **Handle caching at 500 PIDs is feasible but has a hidden cost.** 500 cached process handles
    is trivial for the handle table (tables scale to thousands) **[V2]**, but each open handle
    *prevents the kernel from tearing down the EPROCESS after exit* (zombie) and pins the PID
    against recycle. That PID-pinning is the *only* real reason to cache — otherwise re-open.

17. **The agent-footprint bar is Process Lasso's core engine: ~1–3 MB RAM, "almost no CPU"
    idle** (ProcessGovernor.exe, GUI closed) — vendor self-report **[V2]**. Phynned's own budget
    (Idle CPU<1% / RSS<20 MB, Active CPU<5% / RSS<50 MB, measured sustained ~2.9%) **[V1-int]**
    is ~10× looser on RAM. A pure event engine (no Vulkan/UI deps) should be able to beat 20 MB.

18. **WNF (Windows Notification Facility) is a real but undocumented birth signal.** A well-known
    state name is toggled on process create; subscribable via
    `RtlSubscribeWnfStateChangeNotification`, no admin, very low cost. **[V3]** Undocumented,
    version-fragile, callback runs on a system thread — acceptable as an *optimization/secondary*
    signal, never as the load-bearing one.

19. **PssCaptureSnapshot is a red herring for this facet.** It snapshots an *already-running*
    process for diagnostics/dumping; it is not a creation notifier. **[V1]** Do not pursue it for
    birth detection.

---

## 1. Q1 — Process-birth notification WITHOUT polling, from user mode

### 1.1 Ranked options

| Mechanism | Latency to first notify | Privilege | Per-event cost | Fields at spawn | System-wide? |
|---|---|---|---|---|---|
| **ETW Microsoft-Windows-Kernel-Process** (manifest) | buffer-fill → up to FlushTimer (min 1 s) alone; ms if session is busy | **Admin** | very low (kernel buffers; consumer thread parse) | PID, CreateTime, **ParentPID**, SessionID, **ImageName** — enough to route | **Yes** |
| **ETW System Process Provider** (system-logger) | same as above | **Admin** + `EVENT_TRACE_SYSTEM_LOGGER_MODE` | very low | same class of data + image-load/thread | **Yes** |
| **WNF state-change subscription** | ~immediate (kernel publish → callback) | **None** | very low | state name toggles; **must then query** PID list yourself | Yes (but signal only) |
| **WMI `Win32_ProcessStartTrace`** (extrinsic) | near real-time (kernel-backed) | Admin-ish (`SeSystemProfile`) | moderate (WMI/COM marshaling per event) | ProcessName, ProcessID, ParentProcessID, SessionID | Yes |
| **WMI `__InstanceCreationEvent … WITHIN n`** (intrinsic) | **= polling interval, default 5 s** | Standard user | **high** (re-enumerates all `Win32_Process` each poll) | full `Win32_Process` instance | Yes |
| **Job object `JOB_OBJECT_MSG_NEW_PROCESS`** | ~immediate | Standard user | very low (IOCP completion) | PID only (may be dead/recycled) | **No — job descendants only** |
| **PssCaptureSnapshot** | N/A | — | — | not a creation notifier | N/A |
| **Kernel driver `PsSetCreateProcessNotifyRoutineEx`** | **synchronous, in the creating thread, before the new thread runs meaningfully** | **Kernel (signed driver)** | very low | PID, ParentPID, ImageFileName, CommandLine (Ex), can **deny/suspend** | Yes |

### 1.2 ETW Kernel-Process — the recommended path

- ProcessStart is **Event ID 1**; template (v0): `ProcessID (UInt32)`, `CreateTime (FILETIME)`,
  `ParentProcessID (UInt32)`, `SessionID (UInt32)`, `ImageName (UnicodeString)`. v1/v2 add
  `Flags`, `ImageChecksum`, `TimeDateStamp`, package info. ProcessStop = Event ID 2. **[V1]**
- **All the fields needed to route at birth are in the event** — PID (who), ImageName (classify
  which policy), ParentPID (inherit parent's policy / build the tree). No follow-up
  `OpenProcess`/query is required just to decide the route. **[V1]**
- Provider GUID `{22FB2CD6-0E7B-422B-A0C7-2FAD1FD0E716}`, enable keyword `0x10`
  (`WINEVENT_KEYWORD_PROCESS`). Attaching a real-time session to it needs admin. **[V1]**
- This maps cleanly onto Phynned's existing `phyriad::etw::SessionManager`
  (`StartTraceA` + `EnableTraceEx2` + background `ProcessTrace` consumer) — **swap the GUID and
  it is the birth engine**, no new infrastructure. **[V1, internal read]**

### 1.3 The "route at birth" latency ceiling — stated honestly

**User-mode ceiling:** By the time *any* user-mode consumer sees a ProcessStart, the kernel has
already created the process object **and its first thread, which is already schedulable/running**.
ETW ProcessStart is emitted around process creation but the notification is buffered and
delivered asynchronously to the real-time consumer. So the realistic user-mode outcome is:

> the target has already executed some code by the time you apply affinity/priority; you are
> *re-homing a running process*, not pre-placing a suspended one.

- **You cannot suspend it from user mode at that instant.** `SetProcessAffinityMask` /
  `SetPriorityClass` require `OpenProcess(PROCESS_SET_INFORMATION)` and take effect while the
  process runs; affinity re-homing causes the scheduler to migrate its threads at the next
  quantum. **[V1]**
- **CREATE_SUSPENDED only helps for processes YOU launch.** `START /AFFINITY` works precisely
  because cmd creates the child suspended, sets affinity, then `ResumeThread` — the rule applies
  from the very first instruction. **[V1]** Phynned observes *externally*-launched processes, so
  this path is unavailable unless Phynned inserts itself as a launcher/shim (a different
  product).
- **Kernel driver is the only true suspend-at-birth.** `PsSetCreateProcessNotifyRoutineEx(2)`
  fires *just after the first thread is created*, at `PASSIVE_LEVEL`, in the context of the
  creating thread, inside a critical region — synchronously, before the new thread does
  meaningful work. A driver can deny creation or (via undocumented `PsSuspendProcess`) suspend.
  **[V1]** Cost: a signed kernel driver (or test-signing) — a large jump in blast radius,
  distribution friction, and anti-cheat/PatchGuard sensitivity. Out of scope for "lo más
  barato," but it is the honest answer to "before the first thread runs."

**Practical conclusion for the arc:** aim for **"route within a few ms of birth,"** not "before
birth." That is achievable in user mode with ETW Kernel-Process + an immediate
`OpenProcess`+affinity/priority apply on the consumer thread. The residual (the first few ms of
un-routed execution) is an irreducible user-mode tax — spend a kernel driver only if that tax is
proven to matter.

### 1.4 WMI — why it loses

- `SELECT * FROM __InstanceCreationEvent WITHIN 5 WHERE TargetInstance ISA 'Win32_Process'` is
  **polling**: WMI re-enumerates `Win32_Process` every `WITHIN` seconds and diffs. Default 5 s;
  **any process that starts and exits inside the interval is invisible.** **[V1]** This alone
  disqualifies it for "every newborn process."
- `Win32_ProcessStartTrace` is extrinsic and kernel-backed (no polling, near real-time) but it
  is a COM/WMI wrapper over the same kernel process-trace data ETW gives you directly, with
  per-event marshaling overhead. **[V2]** If you already run an ETW session, WMI adds cost for
  no new information.

### 1.5 Job objects — scope trap

`JOB_OBJECT_MSG_NEW_PROCESS` is delivered to an IOCP via `JOBOBJECT_ASSOCIATE_COMPLETION_PORT`,
but **only for processes that are members of that job** (descendants of what you assigned).
Delivery is explicitly *not guaranteed*; the returned PID may already be dead or recycled unless
you hold an open handle. **[V1]** Only relevant if Phynned launches the game and wants to catch
its child helpers — not for observing the whole system.

### 1.6 WNF — cheap undocumented secondary signal

WNF is a registrationless pub/sub kernel facility (Win8+). A process subscribes to a 64-bit
StateName via `RtlSubscribeWnfStateChangeNotification`; the callback runs on a fresh system
thread on each publish. A known StateName is signaled on process creation. **[V3, Ionescu/
Quarkslab]** No admin, negligible cost. But: undocumented, StateName values drift across builds,
and the signal only says "something changed" — you still query the process list to learn *which*
PID. **Use as an optimization to shorten the reconcile interval, never as the source of truth.**

---

## 2. Q2 — ETW session engineering at scale

### 2.1 Cost of a Kernel-Process + CSwitch session

- ETW is kernel-implemented and "can easily handle 100,000 events/second." **[V2]** The
  bottleneck is never process-birth (rare); it is **CSwitch**, which on a busy desktop runs
  **tens of thousands to >100,000 events/s** and can exceed **8 million events in 30 s** in a
  profiling trace. **[V2]**
- Consequence: **process-birth and CSwitch are different cost classes.** For the "route at
  birth" arc you need only the birth stream, which is cheap. Add CSwitch **only** if migration
  accounting is a live feature — and if so, budget for it as the dominant cost.

### 2.2 Buffer sizing

- Microsoft guidance: a session with a high event rate (a few MB/s) should use **64–128 KB
  buffers**; hundreds of MB/s → 256 KB–1 MB. **[V1]** Phynned's current CSwitch/Process session
  uses **64 KB × 16 buffers [V1-int]** — the low end; adequate for a filtered/light stream but
  under-buffered for full-rate CSwitch on a 32-thread 7950X3D (expect dropped buffers, as xperf
  itself drops with 64 KB defaults). **[V2]**
- **Real-time delivery latency = buffer-full OR FlushTimer, min 1 s.** `FlushTimer = 0` → 1 s
  default; Microsoft suggests starting most real-time traces at **5–10 s** and tuning down,
  because a higher timer *reduces* CPU overhead. **[V1]** Phynned sets `FlushTimer = 1` **[V1-int]
  ** — the minimum, i.e. it has already traded CPU for latency.

### 2.3 One session or several?

- **Manifest Microsoft-Windows-Kernel-Process does not emit CSwitch.** CSwitch is a
  scheduler/system-trace event. So "one session for everything" only works via a **system-logger
  session** (`EVENT_TRACE_SYSTEM_LOGGER_MODE`, Win build 20348+, i.e. Win11) that enables both:
  - **System Process Provider** `{151f55dc-467d-471f-83b5-5f889d46ff66}`, keyword
    `SYSTEM_PROCESS_KW_GENERAL` (birth/death) [+ `_KW_THREAD`, `_KW_LOADER` if wanted). **[V1]**
  - **System Scheduler Provider** `{599a2a76-4d91-4910-9ac7-7d33f2e97a6c}`, keyword
    `SYSTEM_SCHEDULER_KW_CONTEXT_SWITCH`. **[V1]**
- **Recommended split for this project:**
  - **Session A (always-on, cheap):** manifest Microsoft-Windows-Kernel-Process, keyword `0x10`,
    small buffers, `FlushTimer` low — the birth engine. Latency floor ~1 s when idle; fine for
    routing, and it can be dropped to ms by (b) below if needed.
  - **Session B (on-demand, expensive):** a system-logger session for CSwitch, started only when
    a target actually needs migration accounting, with 64–128 KB buffers. Stop it when no target
    needs it (the `SessionManager` hysteresis pattern already models start/stop lifecycle
    **[V1-int]**).
  This keeps steady-state cost at "birth stream only" and pays for CSwitch only on demand —
  directly serving "lo más barato."
- **If you want one session:** use a single system-logger session with System Process + System
  Scheduler, and gate the Scheduler keyword on/off with `EnableTraceEx2` (system providers are
  runtime-controllable like normal providers on 20348+). **[V1]** One session, CSwitch toggled —
  arguably the cleanest on Win11-only.

### 2.4 The latency/cost coupling (design lever)

Because low-volume buffers only flush on the 1 s timer, **a lone birth session is ~1 s-latent**.
Two ways to get ms-latency birth:
1. Share buffers with a high-volume provider (CSwitch) → buffers fill fast → flush fast. Costs
   the CSwitch rate. **[V2]**
2. Keep birth alone but accept ~1 s worst-case, or drop FlushTimer to its 1 s floor (already
   there). You cannot go below 1 s on the timer. **[V1]**

For "route at birth," ~1 s worst-case is likely *worse* than the value of routing (a 1 s-old
process has already done its startup burst). **If sub-second routing matters, option (1) or a
kernel driver is the only path.** State this trade to the operator before building.

---

## 3. Q3 — Cheap steady-state accounting for ALL processes

### 3.1 Full-table NtQSI cost and scaling

- `NtQuerySystemInformation(SystemProcessInformation)` returns **all** processes' CPU/mem/IO in
  **one syscall**; Phynned already uses it (`ProcessMetricsSnapshot`) with a growable ~256 KB
  buffer. **[V1-int]**
- Internal anchor: **capture ≈ 80–150 µs; per-PID `extract` ≈ 600 ns; ~10× faster than N×
  OpenProcess+GetProcessTimes (~300 µs for 32 targets).** **[V1-int]** (Read first-hand in
  `MetricsCollector.hpp`; the numbers are the project's own, not re-measured by me.)
- **Scaling model [DERIVED]:** the buffer is ~`processes × (SPI≈256 B + threads×
  SYSTEM_THREAD_INFORMATION 80 B)`. At ~15 threads/proc that is ~1.4 KB/proc → ~280 KB @200,
  ~560 KB @400, ~1.1 MB @800 (one buffer-growth past the 256 KB default; the code handles
  `STATUS_INFO_LENGTH_MISMATCH`). Kernel walk+copy is ~linear in (processes + total threads), so
  **~80–150 µs @200 → ~0.3–0.6 ms @800**, plus O(N log N) sort of a few hundred entries (tens of
  µs). **Net: sub-millisecond even at 800 processes.** *Could not verify the 400/800 points —
  needs a bench on the 7950X3D.*
- Beware: the internal comment "10 ms" some sources cite for NtQSI is **debugger-inflated**;
  Geoff Chappell notes debugger measurements "easily produce times of the order of 10 ms."
  **[V3]** Ignore it for a release build.
- Cheaper variant: **`SystemBasicProcessInformation`** (a newer class) is documented as faster,
  smaller, and skips the thread-timing sync (fewer processor wakes) — worth evaluating if you
  drop per-thread data. **[V2]**

### 3.2 vs per-PID GetProcessTimes batching

- Per-PID `OpenProcess + GetProcessTimes + CloseHandle` is the *most lightweight* per-call timing
  path (order-of-magnitude cheaper than QPC-loop methods) **[V2]**, but it is **N syscalls**. At
  200+ PIDs the single NtQSI call wins decisively (the ~10× the project already measured
  **[V1-int]**). **Keep NtQSI for the full table; use per-PID GetProcessTimes only for the tiny
  hot set (e.g. the foreground target) between full passes.**

### 3.3 Memory for per-PID delta state

- **Current `PidState` layout [V1, read]:** `pid(4)` + pad(4) + `prev_kernel/user/wall`(3×8=24) +
  `migration_count`/`prev_migration_count`(8) + `cached_cpu_pct`/`cached_thread_count`(8) +
  **`prev_threads[64]×16 = 1024`** + `n_prev_threads`(4) + `hot_tid_candidate`(4) +
  `hot_tid_consecutive`(1)+pad(3) + `cached_hot_tid`(4) + `valid`(1) → **≈ 1096 B/PID
  [DERIVED]**, i.e. the project's "~1.1 KB" is right, and **~93% of it is the per-thread hot-TID
  array.**
- **Slim process-level state [DERIVED]:** drop `prev_threads[]` and hot-TID machinery → `pid` +
  3× u64 + a few u32 ≈ **48–56 B/PID**. At **1024 PIDs: ~1.12 MB (fat) → ~48–56 KB (slim)** — a
  **~95% cut.** The arithmetic checks out against the project's "~48 B/PID slim" figure.
- **Design rule for MASS scale:** track per-thread hot-TID **only for the small set of
  actively-routed / foreground processes** (dozens), and keep everyone else on the slim
  process-level record. Memory then scales as `few×1 KB + many×48 B`, not `all×1 KB`.

### 3.4 How rarely can the full pass run?

With births arriving via ETW, the full NtQSI pass is needed only for: initial population, and
drift correction (missed events, PID recycle, exited-without-event). **A reconcile every 2–5 s is
ample** — the 100 ms Active tick should *drain the ETW ring and update the hot set*, not scan all
processes. **[DERIVED from mechanism]** This is the core of "event-driven reconciliation vs
periodic snapshot": births are event-driven; the snapshot degrades to a slow janitor.

### 3.5 The two structural caps to lift first

- `kMaxTargets = 64` **[V1]** and `kMaxPidStates = kMaxTargets` — the whole per-PID array is 64
  deep.
- `pid_hash_` stores `slot_plus_1` as **`uint8_t`** → **max 254 distinct PIDs** in the O(1) map.
  **[V1]** "Every newborn process at MASS scale" means widening this to `uint16_t` (and the arrays
  to 1024+), which is cheap in bytes given §3.3's slim state but is a hard prerequisite — today
  the map physically cannot hold 1024 PIDs.

---

## 4. Q4 — Batching + amortization of control ops

### 4.1 Per-op cost and the caching question

- `OpenProcess(PROCESS_SET_INFORMATION)` + `SetProcessAffinityMask`/`SetPriorityClass` +
  `CloseHandle` is a few single-digit µs total per process **[V2, mechanism]**. For **event-driven
  routing you touch exactly one process per birth**, so there is nothing to batch on the hot
  path — open, set, close.
- **Handle caching buys speed only if you re-touch the same PID often.** For birth-time routing
  (touch once) it does not. Its real value is **PID-recycle safety**: an open handle guarantees
  the kernel won't reuse that PID for a different process. **[V1, from the Job-object recycle
  caveat.]**
- **Cost of caching at 500 handles [V2]:** the handle table absorbs it easily (tables hold
  thousands), but every cached handle **pins the EPROCESS after the process exits** (a zombie
  that can't be fully reclaimed until you `CloseHandle`) and pins the PID. So a 500-handle cache
  quietly holds kernel memory for dead processes unless you actively prune on ProcessStop.
- **Recommendation:** default to **open/set/close per birth** (stateless, no zombies). Cache a
  handle **only** for a process you will re-touch on a schedule (e.g. periodic affinity
  re-assert against games that reset their own affinity), and **close it on the ETW ProcessStop
  event** to avoid zombie accumulation.

### 4.2 Rate-limiting + dirty-set-only application

- **Dirty-set only:** maintain a set of PIDs whose *desired* policy differs from *applied*
  policy; each pass applies only the diff. At steady state the dirty set is empty → zero syscalls.
  Births enter the dirty set; a successful apply clears them. This is the cheapest possible
  control loop and fits the existing `AutoRevertGuard`/policy structure. **[V1-int, internal
  read]**
- **Rate-limit re-asserts:** some games rewrite their own affinity/priority; if you re-assert,
  cap it (e.g. ≤1 re-assert/s/PID) so a fighting game process can't make Phynned spin. Drive it
  off the 500 ms cached-metrics cadence, not the 100 ms tick.
- **Coalesce a birth storm:** a launcher can spawn dozens of children in a burst (Chrome, Edge,
  MSVC). Drain the whole ETW ring first, dedup by PID+ImageName, then apply once per unique
  target — don't apply inside the per-event callback.

---

## 5. Q5 — The agent's own footprint discipline

### 5.1 The bar: self-budgeting daemons

- **Process Lasso** cleanly separates a **core engine (`ProcessGovernor.exe`)** from the GUI; with
  the GUI closed the engine runs at **~1–3 MB RAM and "almost no CPU."** Vendor self-report
  **[V2]** (bitsum.com) — treat as the *marketing floor*, not an audited number, but it is the
  recognized bar for this product class.
- Design lesson: **the resident daemon must not carry the UI's dependencies.** Process Lasso's
  1–3 MB is achievable precisely because the engine is a separate slim process. Phynned's
  `phynned-agent` should be that slim process; `phynned-ui` (ImGui/Vulkan) must be a *separate,
  optional* process that attaches over the existing SHM IPC. **[V1-int, internal read: the
  agent/UI/IPC split already exists.]**

### 5.2 Phynned's own budget vs the bar

- `SelfMonitor` enforces: **Idle CPU<1% / RSS<20 MB; Active CPU<5% / RSS<50 MB; Bench<10%/100 MB**
  — revised from an "aspirational" 1% Active after measuring **sustained ~2.9% during Fallout 4 +
  agent.** **[V1, read first-hand.]** It edge-triggers logging and (per its doc) is meant to
  reduce work when over budget.
- **Gap to the bar:** Process Lasso's 1–3 MB vs Phynned's 20–50 MB RSS budget is ~10×. Most of
  that is almost certainly framework/dependency weight, not per-PID state (§3.3 shows even 1024
  slim PIDs is ~50 KB). **For the mass-scale event engine, target < 20 MB RSS for the resident
  agent** and treat the 50 MB Active budget as a ceiling, not a goal.
- **CPU:** the event engine's steady state should be **near-zero** — it sleeps on the ETW consumer
  thread (blocking `ProcessTrace`) and wakes only on births. The 100 ms Active tick is the main
  idle cost; consider letting the tick idle longer when only birth-routing (no CSwitch/metrics)
  is active, since routing is event-driven, not poll-driven. **[DERIVED]**

### 5.3 Self-budget as a first-class feature

- Keep `SelfMonitor` in the loop and make **budget breach reduce work** (lengthen tick, drop
  per-thread tracking, stop the CSwitch session) rather than just log. The operator's "cost
  near-nothing" requirement means the agent must be able to *demote itself* under load — the
  hysteresis pattern from `SessionManager` is the right model to reuse. **[V1-int]**

---

## 6. Recommended architecture for "route every newborn at birth, at MASS scale"

1. **Birth engine:** one ETW real-time session on **Microsoft-Windows-Kernel-Process
   `{22FB2CD6-…}` keyword `0x10`** (FIX the GUID first). Consumer thread parses ProcessStart
   (PID/ImageName/ParentPID), classifies via the existing pattern/policy tables, and applies
   affinity/priority with `OpenProcess`+set+close on the spot. Small buffers; accept ~1 s
   worst-case latency unless sub-second is proven necessary. **[V1]**
2. **Metrics/CSwitch:** a *separate, on-demand* system-logger session (`EVENT_TRACE_SYSTEM_LOGGER_
   MODE` + System Scheduler `{599a2a76-…}` `SYSTEM_SCHEDULER_KW_CONTEXT_SWITCH`), started only
   when a target needs migration accounting, 64–128 KB buffers, stopped otherwise. **[V1]**
3. **Reconcile janitor:** full `NtQuerySystemInformation` pass every 2–5 s to catch missed
   births/exits and correct drift — sub-ms even at 800 procs. **[V1-int + DERIVED]**
4. **State:** slim ~48 B process-level record for every PID; fat ~1 KB per-thread record only for
   the small actively-routed/foreground set. Widen `pid_hash` slot to `uint16_t` and
   `kMaxTargets`/arrays to 1024+. **[V1 + DERIVED]**
5. **Control:** dirty-set-only, open/close per op, rate-limited re-asserts, birth-storm
   coalescing; cache a handle only to pin against PID recycle and close it on ProcessStop.
   **[V2]**
6. **Footprint:** resident agent < 20 MB RSS, near-zero idle CPU; UI stays a separate process;
   `SelfMonitor` demotes work on breach. Bar = Process Lasso core engine ~1–3 MB. **[V1-int/V2]**

---

## 7. Code-correctness flags surfaced during research (verify on-box)

- **F-1 [V1]** `observer/.../MetricsCollector.cpp` / `framework/etw/.../SessionManager.hpp`:
  `kKernelProcess` GUID is `{0268a8b6-74fd-4302-9b4a-6ea0fbb19d9e}`; correct
  Microsoft-Windows-Kernel-Process is **`{22FB2CD6-0E7B-422B-A0C7-2FAD1FD0E716}`**. Process/thread
  birth events are very likely never delivered. Confirm: `logman query providers` or an
  `EnableTraceEx2` return-code check (the current code ignores per-provider enable failure).
- **F-2 [V2/V3]** `kKernelContextSwitch` GUID `{def2fe46-7bd6-4b80-bd94-f57fe20d0ce3}` is
  **StackWalkGuid**, not a dispatcher/scheduler provider. CSwitch cannot originate from it.
- **F-3 [V1]** The CSwitch session uses only `EVENT_TRACE_REAL_TIME_MODE`; CSwitch via a private
  session requires `EVENT_TRACE_SYSTEM_LOGGER_MODE` + System Scheduler provider on build 20348+.
  Net: `migrations_per_sec` is likely always ~0 in the current build. The `etw_cswitch_total/
  pushed/skipped` diagnostic counters will show it empirically — read them.
- **F-4 [V1]** `SessionManager::start` ignores `EnableTraceEx2`'s return code ("non-fatal — log
  but continue" — but there is no log). A wrong GUID / missing mode fails silently. Add a
  per-provider status check so F-1…F-3 can't hide again.
- **F-5 [V1]** `pid_hash` `slot_plus_1` is `uint8_t` (≤254 PIDs) — a hard cap incompatible with
  "every newborn process at MASS scale."

*(These are research by-products, not a full code review; each needs an on-box confirmation.)*

---

## Sources

Process-birth / ETW Kernel-Process:
- Microsoft-Windows-Kernel-Process manifest (GUID, ProcessStart fields, keyword 0x10):
  https://github.com/repnz/etw-providers-docs/blob/master/Manifests-Win10-17134/Microsoft-Windows-Kernel-Process.xml
- Velociraptor Windows.ETW.KernelProcess (fields, admin requirement):
  https://docs.velociraptor.app/artifact_references/pages/windows.etw.kernelprocess/
- ETW 101 (real-time sessions, admin): https://www.ired.team/miscellaneous-reversing-forensics/windows-kernel-internals/etw-event-tracing-for-windows-101

WMI:
- `__InstanceCreationEvent` (WITHIN polling semantics): https://learn.microsoft.com/en-us/windows/win32/wmisdk/--instancecreationevent
- Responding to WMI events (default 5 s polling, transient-miss): https://learn.microsoft.com/en-us/previous-versions/technet-magazine/ff898417(v=msdn.10)

Job objects:
- JOBOBJECT_ASSOCIATE_COMPLETION_PORT (messages, not-guaranteed delivery, PID recycle):
  https://learn.microsoft.com/en-us/windows/win32/api/winnt/ns-winnt-jobobject_associate_completion_port
- Raymond Chen, detecting child-process launches via a job (job-scope only, race):
  https://devblogs.microsoft.com/oldnewthing/20250523-00/?p=111216
- Job Objects overview: https://learn.microsoft.com/en-us/windows/win32/procthread/job-objects

Kernel driver / suspend-at-birth:
- PsSetCreateProcessNotifyRoutineEx (callback after first thread, PASSIVE_LEVEL, in creator
  context): https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/ntddk/nf-ntddk-pssetcreateprocessnotifyroutineex
- PsSetCreateProcessNotifyRoutineEx2: https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/ntddk/nf-ntddk-pssetcreateprocessnotifyroutineex2
- StartSuspended KMDF driver (suspend a process on start): https://github.com/IcarusResearch/StartSuspended
- Subscribing to process/thread/image notifications from a driver: https://www.ired.team/miscellaneous-reversing-forensics/windows-kernel-internals/subscribing-to-process-creation-thread-creation-and-image-load-notifications-from-a-kernel-driver

ETW session engineering:
- System Providers (System Process `{151f55dc-…}`, System Scheduler `{599a2a76-…}`,
  SYSTEM_SCHEDULER_KW_CONTEXT_SWITCH, system-logger mode, build 20348):
  https://learn.microsoft.com/en-us/windows/win32/etw/system-providers
- Configuring a SystemTraceProvider session (EVENT_TRACE_SYSTEM_LOGGER_MODE, non-SystemTrace
  GUID): https://learn.microsoft.com/en-us/windows/win32/etw/configuring-and-starting-a-systemtraceprovider-session
- EVENT_TRACE_PROPERTIES (buffer sizing, FlushTimer): https://learn.microsoft.com/en-us/windows/win32/api/evntrace/ns-evntrace-event_trace_properties
- EVENT_TRACE_PROPERTIES_V2 (FlushTimer min 1 s / default): https://learn.microsoft.com/en-us/windows/win32/api/evntrace/ns-evntrace-event_trace_properties_v2
- WPT Sessions (buffer size guidance 64–128 KB / 256 KB–1 MB, FlushTimer 5–10 s start):
  https://learn.microsoft.com/en-us/windows-hardware/test/wpt/sessions
- ETW ~100k events/s, CSwitch expense: https://spin.atomicobject.com/2015/05/29/etw-performance-analysis/
- CSwitch high volume (8M/30s, buffer drops with 64 KB defaults): https://performancebydesign.blogspot.com/2011/11/using-xperf-to-analyze-cswitch-events.html
- CSwitch class (event schema): https://learn.microsoft.com/en-us/windows/win32/etw/cswitch
- StackWalkGuid / GUID list (def2fe46-…): https://gist.github.com/guitarrapc/35a94b908bad677a7310

NtQuerySystemInformation:
- NtQuerySystemInformation (winternl): https://learn.microsoft.com/en-us/windows/win32/api/winternl/nf-winternl-ntquerysysteminformation
- SYSTEM_PROCESS_INFORMATION (layout, SystemBasicProcessInformation is cheaper):
  https://ntdoc.m417z.com/system_process_information
- Geoff Chappell, ZwQuerySystemInformation (debugger 10 ms caveat): https://www.geoffchappell.com/studies/windows/km/ntoskrnl/api/ex/sysinfo/query.htm

Control ops:
- SetProcessAffinityMask (PROCESS_SET_INFORMATION, subset rule): https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-setprocessaffinitymask
- START /AFFINITY = CREATE_SUSPENDED + set + resume (pre-first-instruction affinity):
  https://www.tenforums.com/software-apps/145990-starting-application-affinity.html
- GetProcessTimes (lightweight per-call timing): https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-getprocesstimes

WNF / snapshot (secondary):
- Playing with WNF (subscribe, callback, undocumented): https://blog.quarkslab.com/playing-with-the-windows-notification-facility-wnf.html
- Ionescu, The Windows Notification Facility (BlackHat 2018): http://publications.alex-ionescu.com/BlackHat/BlackHat%202018%20-%20The%20Windows%20Notification%20Facility,%20the%20most%20undocumented%20kernel%20attack%20surface%20yet.pdf
- PssCaptureSnapshot (diagnostics snapshot, not a notifier): https://learn.microsoft.com/en-us/windows/win32/api/processsnapshot/nf-processsnapshot-psscapturesnapshot

Agent footprint:
- Process Lasso resource utilization (core engine 1–3 MB, almost no CPU): https://bitsum.com/pl_resource_usage.php
- Process Lasso FAQ (GUI-closed core engine ProcessGovernor.exe): https://bitsum.com/process-lasso-faq/

Internal (Phynned source, read first-hand this session — [V1-int]):
- `framework/etw/include/phyriad/etw/SessionManager.hpp`, `framework/etw/src/SessionManager.cpp`
- `observer/include/phynned/observer/MetricsCollector.hpp`, `observer/src/MetricsCollector_win32.cpp`
- `framework/process/src/ProcessMetricsSnapshot.cpp`, `framework/process/src/ProcessEnumerator.cpp`
- `observer/src/ProcessObserver.cpp`, `observer/include/phynned/observer/TargetProcess.hpp`
- `core/include/phynned/core/SelfMonitor.hpp`, `core/include/phynned/core/AdaptiveTick.hpp`
- `ipc/include/phynned/ipc/PhynnedProtocol.hpp`

<!-- Made with my soul - Swately <3 -->
