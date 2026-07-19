# Phynned — SOTA / Prior-Art Teardown

**Facet:** every tool/system that already does mass process routing, pinning, or
priority management on asymmetric CPUs — what exactly each does, at what scale,
with what config surface, what measured gains exist, and what measurable niche
is left for a Phynned mass-router.

**Scope of Phynned's target arc (for reference):** *mass automatic core-routing of
every process by measured consumption* on an asymmetric CPU (7950X3D dual-CCD,
one cache CCD + one frequency CCD), Windows 11. This teardown is written against
that goal, not against Phynned's current single-target game-pinning build.

---

## Verification legend

- **[V1]** — primary source read first-hand (official doc / source / changelog fetched this session).
- **[V2]** — primary partial: fetched a summary of the primary, or the primary is now unreachable; gap declared inline.
- **[V3]** — secondary only (forum/press/wiki), not corroborated by a primary.

Verify-before-claim caveat: where a public vendor doc deliberately omits an
internal threshold (ProBalance CPU%, AMD's exact detection heuristic), this
dossier says **"not publicly documented"** rather than guessing.

---

## 0. Headline: the niche is more crowded than the operator's README assumes

The single most important finding: **the exact thing Phynned currently ships —
detect a game, pin it to the X3D cache CCD on a 7950X3D — is already done by at
least four free tools and by AMD+Microsoft's own bundled stack.** The *game-pinning*
half of the vision is a solved, commoditised problem in 2026. Full plain-spoken
verdict in §8. The teardown below establishes that claim tool by tool.

---

## 1. Process Lasso (Bitsum) — the incumbent

**What it is:** the reference Windows real-time CPU automation tool since ~2004.
Free edition (core features usable indefinitely, with a nag / 30-day commercial
grace); Pro is **$29.95/yr or $99.95 lifetime**, 40%-off codes routinely floated.
Closed source. [V2 — bitsum.com/get-lasso-pro/, howfree/]

> Correction for Phynned's own README: it lists Process Lasso at "$30 one-time".
> Reality is $29.95/**year** or $99.95 lifetime, and **ProBalance itself is in the
> free edition** — the paid tier gates automation/watchdog/persistent-rules polish,
> not the core algorithm. The comparison table should be fixed. [V2]

### 1.1 ProBalance algorithm [V1 — bitsum.com/how-probalance-works/]
- **Mechanism:** a *priority-restraint* algorithm, not an affinity router. It watches
  for CPU-bound processes that threaten foreground responsiveness under high load and
  **temporarily lowers the offending process's priority class to Below Normal**,
  restoring it "as soon as system conditions change." [V1]
- **Default exclusions (this is the load-bearing detail):** it **avoids the foreground
  process by default** — "you will need to click away from a problematic application
  to have it engage" — and **"Ignore processes of non-normal priority" is on by
  default**, so anything that sets its own priority class is skipped. [V1]
- **Thresholds:** the public doc states it engages during "high load" and is
  "conservative … does not slow down processes unless there is significant CPU
  contention," but **the exact CPU% and duration triggers are not publicly
  documented.** Do not cite a number. [V1 for the omission]
- **Scope:** it *monitors* effectively all normal-priority background processes but
  only *acts* on offenders — it does not proactively route the whole process table.

### 1.2 Affinities / CPU Sets / X3D handling
- **Persistent per-process affinity:** right-click → CPU Affinity → Always → pick
  cores; remembered across app restart and reboot. **Per-application, manual.** [V1 — bitsum.com/cpu-affinities/]
- **CPU Sets vs hard affinity:** Process Lasso supports both; CPU Sets are a *soft*
  hint the scheduler may deviate from, hard affinity is a hard restriction. (The
  affinities page itself does not spell out the distinction; a known 7950X3D forum
  thread reports that on X3D "CPU Sets behave like Affinities" i.e. act hard in
  practice. [V3 — community.bitsum.com topic 10757])
- **X3D-specific (v14.2, the current relevant feature):** the CPU-Affinity / CPU-Sets
  dialogs now **label CCDs as "frequency" vs "cache" cores**, add buttons to select a
  CCD (≤4 CCDs) or a dropdown (>4), and mark cache-centric cores in the context menu.
  **Crucially, the update describes only *manual* selection — it does NOT claim any
  automatic routing of processes to the cache CCD.** [V1 — bitsum.com/product-update/process-lasso-14-2-improved-core-selection/]
- **Performance Mode / Instance Balancer:** "Performance Mode" = switch to the Bitsum
  Highest Performance power plan (disables power-saving); Instance Balancer distributes
  load across multiple instances of one process. Neither is topology-aware routing. [V2]

**Net:** Process Lasso is a **priority-restraint + persistent-rule** engine. Its X3D
support is a *manual affinity picker with nice labels*. **It does not measure
per-process cache benefit and does not auto-route the process flood to a CCD.** That
is the single biggest gap Phynned can honestly point at — see §8.

---

## 2. AMD's own stack — the 3D V-Cache Performance Optimizer + Xbox Game Bar

This is the default, pre-installed router on every 7950X3D. Understanding it is
mandatory because *it already ships the routing behaviour for games.* [V2 throughout —
assembled from AMD/Neowin/HardForum/VRChat-wiki summaries; AMD's own whitepaper was
not fetched first-hand, gap declared]

### 2.1 Mechanism
- Two services: **`amd3dvcacheSvc.exe`** (LocalSystem) + **`amd3dvcacheUser.exe`**. [V2]
- On a dual-CCD X3D part, one CCD has the stacked cache, the other clocks higher. The
  optimizer biases scheduling to **prefer the cache CCD for games** and uses each CCD's
  **CPPC "preferred core" ranking** to pick the best cores within a CCD. [V2]
- **Game detection is delegated to Xbox Game Bar.** When Game Bar decides a foreground
  app is a game, it flags "game mode" and the workload is steered onto the cache CCD;
  in normal desktop use the **frequency CCD** is used and the cache CCD may be **parked
  (asleep)** — visible in Task Manager. Users can force recognition with Game Bar's
  **"Remember this is a game"** toggle. [V2]
- The parking half relies on Windows **core parking** of the non-preferred CCD.

### 2.2 Blind spots / failure modes (the openings)
- **Detection is heuristic and game-only.** Anything Game Bar doesn't classify as a
  game — a compiler, a simulator, an emulator, a CPU-bound productivity app that would
  *also* benefit from V-Cache — is **not routed.** No measured-benefit path exists;
  it's a binary is-this-a-game gate. [V2]
- **Service reliability:** the optimizer service **intermittently fails to auto-start**,
  silently disabling routing until manually restarted (widely reported on 9950X3D). [V2 —
  hwbusters, pcforum.amd.com]
- **Wrong-CCD parking race:** third-party tools that lean on the same CPPC/parking
  machinery have reproduced parking the **cache CCD by mistake on ~1 in 5 boots**
  (CPUSetSetter issue #68 on 9950X3D) — a boot-order/CPPC-ranking nondeterminism the
  thread never fully root-causes. AMD's own path is more reliable but rides the same
  fragile primitives. [V1 — github.com/SimonvBez/CPUSetSetter/issues/68]
- **Microsoft-side fragility:** a 2025 report flagged Microsoft quietly changing core-
  parking behaviour and breaking X3D optimisation for many users (Neowin) — the whole
  scheme is a multi-party contract (AMD driver + Game Bar + Windows scheduler + core
  parking) with no single owner. [V3 — neowin.net; primary MS statement not located]

**Net:** AMD already does the game-routing Phynned does — *for games it recognises,
when its service is running.* Its blind spot is **non-game CPU-heavy processes** and
**a lack of any per-process measurement.**

---

## 3. Windows built-ins

### 3.1 Game Mode [V2 — assembled from press/forum; MS DirectX docs not fetched first-hand]
- Raises the **foreground game's CPU + GPU scheduler priority**, **lowers non-essential
  background priority**, and **defers Windows Update installs** while active. No
  overclock, no driver change. Effect is mostly on **1% lows / frame pacing**, not
  average FPS. [V2]
- **Conflict warning that matters for Phynned's niche:** CPUSetSetter's own docs tell
  X3D users to **disable Windows Game Mode** when using an external CCD router, because
  Game Mode's optimisations can conflict and cause **performance loss or crashes.** Any
  Phynned mass-router must detect/coexist-with or advise-disabling Game Mode. [V1 via §5.1]

### 3.2 EcoQoS / Efficiency Mode [V2 — windowsforum/beebom/MS-Q&A summaries]
- Efficiency Mode = **lowered scheduling priority + an EcoQoS hint** so the CPU favours
  energy efficiency and starves the process of turbo/thermal headroom. [V2]
- **No global toggle** — it is per-process via Task Manager, though **Windows itself
  auto-applies it to some background work** (e.g. suspended UWP, background Edge tabs).
  Third-party **Energy Star X** auto-applies EcoQoS to background processes en masse
  (open source). MS lab claims 14–76% responsiveness gains (lab, not field). [V2]
- Relevance: EcoQoS is the **OS-blessed way to demote the background flood** — the
  eviction half of Phynned's vision. A mass-router should prefer emitting EcoQoS hints
  over crude priority changes for background demotion, because it's the sanctioned API.

### 3.3 Heterogeneous / core-parking power knobs (powercfg hidden settings) [V2 — Microsoft Learn page titles confirmed via search; individual pages not fetched first-hand]
Windows exposes a family of **hidden power-plan knobs** that govern hybrid/heterogeneous
scheduling and core parking — the *policy layer* underneath everything above:
- **`HETEROPOLICY`** — heterogeneous thread scheduling policy, values **0–4**, active on
  systems with ≥2 processor power-efficiency classes.
- **`ShortThreadRuntimeThreshold`**, **`ShortSchedulingPolicy`**,
  **`ShortThreadArchClassUpperThreshold`**, **`LongThreadArchClassLowerThreshold`** —
  define short-vs-long thread classification and which core-classes each may run on.
- **`CPMINCORES` / `CPMAXCORES`** — core-parking min/max active cores.
- Bitsum's free **ParkControl** exposes the parking knobs with a GUI. [V2]

Relevance: these are **the lever Phynned would be fighting or cooperating with.** A
serious mass-router either (a) drives affinity/CPU-Sets *above* this layer, or (b)
tunes these knobs directly. AMD's optimizer uses the parking layer; CPUSetSetter
deliberately uses CPU-Sets to sit *above* it (see §5).

---

## 4. CorePrio & Ryzen Master — adjacent, not competitors

### 4.1 CorePrio + NUMA Dissociater (Bitsum, free, open-source) [V2 — bitsum.com/portfolio/coreprio + github/jeremycollake]
- Built for the **Threadripper 2990WX/2970WX** memory-starved-die problem, not X3D.
- **Dynamic Local Mode:** dynamically migrates the most active threads onto prioritised
  cores — configurable prioritised-affinity, **thread count, refresh rate, include/
  exclude list.** This is *conceptually the closest thing to "route active threads to
  good cores by activity"* in the Bitsum stack — but it's the DLM heuristic (most-active
  threads), **not measured cache benefit**, and it's aimed at 2018 HCC NUMA parts. [V2]
- **NUMA Dissociater:** works around a Windows kernel NUMA bug; near-doubled 2990WX
  perf in press tests. Irrelevant to 7950X3D topology. [V3 — bit-tech/hexus]
- Design-mine value: DLM's **configurable refresh-rate + thread-activity migration loop**
  is a direct antecedent of Phynned's per-tick router. Worth studying its knobs.

### 4.2 Ryzen Master [V2 — amd.com/docs.amd.com Curve Optimizer guide]
- **Per-core / per-CCD Curve Optimizer, PBO, voltage/frequency** only. It monitors
  per-CCD clocks but has **no affinity, no pinning, no scheduler interaction, no game
  detection.** It is a **V/F tuning tool, not a router** — orthogonal to Phynned. Listing
  it as prior art is a category error to avoid. [V2]

---

## 5. The real competition — free open-source X3D CCD routers (2025–2026)

**This is the crowded shelf Phynned actually lands on.** Three tools implement the
detect-game-and-route-to-cache-CCD loop for free and open-source. This is the honest
core of the teardown.

### 5.1 CPUSetSetter / "Core Setter" (SimonvBez) — MIT, free [V1 — github.com/SimonvBez/CPUSetSetter README + issue #68]
- Uses **Windows CPU Sets** (soft hint) rather than hard affinity — a deliberate design
  choice: **CPU Sets need fewer privileges and work with anti-cheat** where hard affinity
  can trip kernel AC. This is a **direct, meaningful advantage over Phynned's
  `SetProcessAffinityMask` approach** for competitive games. [V1]
- **Auto-detects topology** (multi-CCD, X3D presence, Intel P/E) and auto-creates default
  masks: **CCD0, CCD1, Cache, Cache-no-SMT, Freq, Freq-no-SMT, All-no-SMT.** [V2 — search summary of README]
- **Process routing is *manual / static rule-based*** — the user picks which processes
  get which mask (persistent rules + hotkeys). Per the README fetched first-hand: **no
  A/B measurement, no per-process consumption-based auto-assignment, no measured cache
  benefit.** It is a fast, clean *manual* CPU-Set picker. [V1]
- Ships the X3D **"disable Game Mode when using this"** warning (see §3.1). [V2]

### 5.2 x3d-ccd-optimizer (LordBlacksun) — closest thing to Phynned [V2 — repo now returns HTTP 404, deleted/privatised since indexing; description + docs read only via search cache, primary UNREACHABLE, gap declared]
Its own description + a cached research doc reference describe **almost exactly Phynned's
current build**:
- **Automatic game detection via ETW** + **Steam / Epic / GOG library scanning** (Phynned
  also uses ETW for detection — same primitive).
- **Per-game CCD preference set via AMD's own registry interface** (rides the AMD
  optimizer instead of fighting it), with **fallback affinity pinning** via standard
  Win32 APIs and **restore-on-exit.**
- **Protected process list** to avoid touching system infra; **real-time per-core CCD
  activity dashboard.**
- It explicitly documents **"AMD's scheduling stack … CPPC rankings, the amd3dvcache
  driver, Xbox Game Bar, GameMode power profiles, and core parking"** — i.e. it maps the
  same terrain Phynned must.
- **What it does NOT appear to do:** measured per-process consumption routing, A/B self-
  measurement, or a published evidence protocol. (Cannot fully verify — repo is gone.) [V2]

### 5.3 AMD-X3D-Vcache-tray (Coldblackice) [V3 — GitHub description only]
- Lightweight **tray utility to toggle the dynamic CCD preference** of X3D parts. Thin
  wrapper over the AMD mechanism; no routing intelligence. [V3]

**Net for §5:** the "asymmetric-aware game pinner" category is **populated by free,
open-source, MIT tools**, at least one of which (CPUSetSetter) is *technically ahead of
Phynned on anti-cheat compatibility* by using CPU Sets. Phynned's game-pinning feature
is **not novel** and its comparison table (which only benchmarks itself against Process
Lasso and Game Mode) **omits its actual nearest competitors.** Fix the table. [V1/V2]

---

## 6. Linux ecosystem — design-idea mines (not competitors, different OS)

### 6.1 ananicy / ananicy-cpp — the mass-rule daemon [V2 — github READMEs via search; rule schema quoted from primary README text]
- **The single most design-relevant prior art for Phynned's "mass, rule-based, every
  process" arc.** A daemon that watches the process table and applies rules matched by
  **executable name (and optionally cmdline args)**.
- **Rule schema (JSON-ish, one process per line, `*.rules` in `/etc/ananicy.d/`):**
  `{ "name":"gcc", "type":"Heavy_CPU", "nice":19, "ioclass":"best-effort", "ionice":7,
  "cgroup":"cpu90" }`. Fields: `nice` (-20..19), `ioclass`/`ionice`, `sched`
  (fifo/rr/normal/batch/idle), `cgroup`, `oom_score_adj` (-999..999), and a **`type`
  system** (named bundles of settings a rule inherits and can override). [V2]
- **ananicy-cpp** = C++ rewrite for much lower CPU/RAM overhead — directly relevant since
  Phynned's agent must scan the whole process table cheaply per tick.
- **What it does NOT do:** no CPU-topology / CCD / cache awareness, no per-process
  *measured* benefit — it's **static crowd-sourced rules**, not measurement. Phynned's
  measurement angle is genuinely absent here. But **steal the rule format and the mass-
  matching engine wholesale** — it's the proven design for "touch every process by rule."

### 6.2 Feral GameMode (gamemoded) [V1 — github gamemode.ini + README]
- Daemon+lib; a game requests a temporary optimisation bundle. Options: **CPU governor,
  I/O priority, process niceness, SCHED_ISO/softrealtime, screensaver inhibit, GPU perf
  mode, and — most relevant — CPU core pinning/parking.**
- **Config (from `gamemode.ini`, read first-hand):** `pin_cores` **default `yes`**,
  `park_cores` **default `no`**; either accepts an explicit core list
  (`park_cores=1,8-15`). **Autodetection is explicitly limited to "Ryzen 7900x3d,
  7950x3d and Intel CPUs with E- and P-cores."** `renice` 0–20 (negated), `ioprio`
  default BE/0, `softrealtime` off (SCHED_ISO, "not supported by upstream kernels"). [V1]
- **Directly relevant:** GameMode already does **X3D-aware CPU pinning/parking on Linux
  with the 7950x3d hard-coded**, triggered per-game. The Linux world already solved the
  exact game-pinning problem too — with a clean opt-in `gamemoderun` trigger model
  Phynned could learn from.

### 6.3 Cache-aware scheduling — the 2024–2026 kernel series (merged, Linux "7.2") [V1 — lwn.net/Articles/1041668/]
The **most sophisticated prior art on the *measurement* axis**, and it's now in the
mainline kernel. Designed by Tim Chen, Chen Yu, Peter Zijlstra (Intel).
- **Mechanism:** track **per-LLC occupancy per process** every epoch; assign each process
  a **preferred LLC**; aggregate a process's threads onto that LLC to improve locality —
  while guarding against overloading one LLC.
- **Tunables (all read first-hand):** `llc_aggr_tolerance` (0=off; scales the RSS/thread-
  count cutoff above which a process is too big to aggregate), **`llc_overload_pct`
  default 50%** (migrate to preferred LLC only if its avg core util < this),
  **`llc_imb_pct` default 20%** (else keep within 20% of balance to avoid overload),
  **`llc_epoch_period` 10ms** (occupancy sampling), **`llc_epoch_affinity_timeout` 50ms**
  (lose preference if idle past this). [V1]
- **Measured (first-hand):** hackbench 1-group Sapphire Rapids **37.60→26.43 (+29.69%)**;
  schbench p99 wakeup latency **12→8µs (+33%)**; ChaCha20 on AMD **Genoa** at
  tolerance=100 **50,868ms→28,349ms (~44%)**; Stream/Netperf **<1%**; a netperf-360-pairs
  **−8.24% regression** that the authors argue is within run-to-run noise (CoV rose). On
  **AMD Milan** the gains were minimal/mixed (−1.6%…+8.3%). [V1]
- **Direct lesson for Phynned:** this is the **rigorous, published version of "route by
  measured cache behaviour"** — occupancy epochs, an aggregation-tolerance cutoff, an
  overload guard, and an honest regression column. Any Phynned "measured per-process
  V-Cache benefit" claim must reckon with the fact that **the kernel is doing online
  per-LLC occupancy placement already**, and that the *measured* gains are **workload-
  specific and sometimes near-zero or negative** — a sober calibration for Phynned's own
  (currently provisional, +98%-headline) numbers.

### 6.4 cgroups cpuset + autogroup [V3 — general kernel knowledge, not re-verified this session]
- **cpuset controller** pins a cgroup's tasks to a CPU/mem-node set — the Linux primitive
  ananicy/systemd-slices use to *mass-pin by group*. **autogroup** auto-buckets tasks by
  session for fair desktop scheduling. Both are *group-granularity* mass mechanisms
  Windows lacks a clean equivalent of (Windows' nearest analogues are Job Objects + CPU
  Sets). Design-mine only. Marked [V3] — not re-fetched.

---

## 7. Academic — contention-aware & online cache-benefit placement

### 7.1 Zhuravlev / Blagodurov / Fedorova line [V2 — ACM Queue primary returned HTTP 403; details from ACM/ResearchGate summaries + the Queue abstract; numeric gap declared]
- **"Addressing shared resource contention in multicore processors via scheduling,"
  ASPLOS'10** + the **2012 ACM Computing Surveys** survey are the canonical reference.
- **Intensity metric `Z`:** how aggressively a thread uses cache, **= LLC accesses per
  one million instructions** — a proxy for how much cache pressure it inflicts on
  co-runners. [V2]
- **Distributed Intensity (DI/DIO):** measure each thread's miss-rate **online**, then
  **spread the high-intensity threads across separate memory domains** so they don't
  fight over one LLC / memory controller.
- **Measured:** DIO reported **up to ~11% workload-average improvement over the default
  Linux scheduler** on an 8-core Opteron (4 cores/memory-domain). The paper also
  emphasises **reduced worst-case degradation / variance** — but I could not fetch the
  exact worst-case-reduction percentage first-hand (ACM 403). Declared gap. [V2]

### 7.2 Relevance and the honest state of the art
- The academic answer to "route by *measured* per-process cache behaviour" has existed
  since **2010** and is now **in-kernel** (§6.3). LFOC+ (arXiv 2402.07693) and a long
  survey line extend it. **Phynned's "measured per-process V-Cache benefit" idea is not
  a new research direction — it is a 15-year-old, well-mined one.** What is *not* done in
  this literature: a **consumer Windows tool** that does it for the **X3D cache-vs-freq
  CCD split specifically**, with a **published, reproducible A/B evidence protocol for
  games.** That gap is real but narrow. See §8.

---

## 8. Niche verdict — plainly

### 8a. What is ALREADY covered (say it without softening)
1. **Detect a game → pin it to the X3D cache CCD on 7950X3D.** Done by **AMD's own
   optimizer + Xbox Game Bar** (bundled, free), **CPUSetSetter** (MIT), **x3d-ccd-
   optimizer** (open-source), **AMD-X3D-Vcache-tray**, and **Process Lasso** (manual).
   Phynned's flagship feature is **commoditised**. [V1/V2]
2. **Anti-cheat-safe CCD steering.** CPUSetSetter's **CPU-Sets-over-affinity** design is
   *ahead* of Phynned's hard-affinity approach on this exact axis. Phynned's own README
   admits AC incompatibility; a free competitor already fixed it. [V1]
3. **Priority-restraint of the background flood under load.** Process Lasso ProBalance
   (free) + Windows Game Mode + EcoQoS auto-application already demote background work.
   Phynned's "evict background to the other CCD" is a variation, not a new capability. [V1/V2]
4. **Activity-based dynamic thread migration to good cores.** CorePrio's Dynamic Local
   Mode did this in 2018 (configurable refresh/thread-count). [V2]
5. **Online, per-process, measured cache-aware placement.** The **Linux kernel does this
   in mainline** (cache-aware scheduling, 2026) and academia did it in 2010 (DI/DIO). The
   *measurement idea itself* is not novel. [V1/V2]
6. **X3D-aware per-game CPU pinning/parking on Linux.** Feral GameMode, 7950x3d hard-coded. [V1]

### 8b. What NOTHING above actually does (the defensible niche)
Ranked by how defensible each is:

1. **[STRONGEST] A published, reproducible A/B(/A/B/A) *evidence protocol* per game, on
   Windows, for the X3D cache-vs-frequency decision.** Every competitor asserts benefit;
   **none publishes a measurement methodology + per-title reports.** AMD/Game Bar is a
   black box; CPUSetSetter/x3d-ccd-optimizer route by static rule with **no self-
   measurement**; the kernel series publishes benchmarks but for servers, not games, not
   X3D-CCD, not per-title. **This is Phynned's realest differentiator** — provided the
   dataset is actually validated (the current one is self-flagged provisional; the whole
   claim collapses if the numbers don't survive re-validation).

2. **[MODERATE] Measured *per-process V-Cache benefit* as the routing signal, for the
   whole process table, on Windows.** Process Lasso routes by CPU-usage/rules;
   CPUSetSetter/x3d-ccd by static masks; AMD by is-this-a-game. **None routes by an
   online estimate of "does THIS process actually gain from the cache CCD."** The kernel
   does something like it — but not on Windows, not X3D-CCD-specific, not exposed to a
   user tool. *Caveat:* the kernel evidence shows these gains are **workload-specific and
   frequently near-zero** — Phynned must be ready for many processes to measure as
   "no benefit," which is itself a valid, honest result.

3. **[MODEST] Auto-revert on measured regression + per-game memory.** Phynned's README
   claims this and it does appear rare among the competitors (Process Lasso: no; Game
   Mode: no; CPUSetSetter: no A/B). This is a **genuine, if small, edge** — *if* the
   regression detector is real and validated.

4. **[WEAK / ALREADY-CONTESTED] "Mass automatic routing of every process."** The vision's
   headline. **ananicy proves the *mechanism* (mass rule-based, whole-table) is a solved
   design**, and Process Lasso already touches the whole normal-priority set for
   ProBalance. Phynned's twist is only defensible if "by *measured consumption/benefit*"
   is the operative phrase — the mass-ness alone is not novel.

### 8c. Two hard warnings for the operator
- **Fix the comparison table.** It benchmarks Phynned only against Process Lasso and
  Game Mode and omits **CPUSetSetter and x3d-ccd-optimizer** — the actual nearest
  competitors, both free and open-source, one of them technically ahead on anti-cheat.
  Leaving them out reads as either ignorance or spin; both damage the honesty posture
  the project's own charter demands.
- **The niche is the *evidence protocol*, not the routing.** Everything routes. Almost
  nothing *measures and publishes*. Phynned should reposition around the A/B protocol +
  measured-benefit signal + auto-revert, **explicitly conceding that game-pinning is
  commoditised** — and it can only make that claim once the provisional dataset is
  re-validated. Until then the one true differentiator is unproven.

---

## Sources

**Process Lasso / Bitsum**
- https://bitsum.com/how-probalance-works/ [V1]
- https://bitsum.com/cpu-affinities/ [V1]
- https://bitsum.com/product-update/process-lasso-14-2-improved-core-selection/ [V1]
- https://bitsum.com/processlasso-docs/ , https://bitsum.com/automation/ [V2]
- https://bitsum.com/get-lasso-pro/ , https://bitsum.com/howfree/ [V2]
- https://community.bitsum.com/forum/index.php?topic=10757.0 (CPU Sets behave like affinities on 7950X3D) [V3]
- https://bitsum.com/portfolio/coreprio/ , https://github.com/jeremycollake/coreprio [V2]
- https://bitsum.com/parkcontrol/ [V2]

**AMD stack / Windows**
- https://www.neowin.net/news/report-microsoft-quietly-disables-vital-windows-feature-crippling-many-amd-ryzen-cpus/ [V3]
- https://hwbusters.com/cpu/amd-ryzen-9-7950x3d-core-parking-problem-solution/3/ [V2]
- https://wiki.vrchat.com/wiki/Guides:AMD_X3D_Series_Processors [V3]
- https://hardforum.com/threads/ryzen-7000x3d-series-a-brief-technical-chat-with-amd.2027211/ [V3]
- https://learn.microsoft.com/en-us/windows-hardware/customize/power-settings/configuration-for-hetero-power-scheduling-schedulingpolicy (+ ShortThreadRuntimeThreshold, ShortSchedulingPolicy, Short/LongThreadArchClass, options-for-core-parking-cpmaxcores) [V2]
- https://learn.microsoft.com/en-us/answers/questions/4011547/automatically-activates-efficiency-mode [V2]
- https://www.amd.com/system/files/documents/ryzen-master-quick-reference-guide.pdf , https://docs.amd.com/r/en-US/68886-ryzen-master-user-guide/Curve-Optimizer [V2]

**Free X3D CCD routers (nearest competitors)**
- https://github.com/SimonvBez/CPUSetSetter [V1]
- https://github.com/SimonvBez/CPUSetSetter/issues/68 [V1]
- https://github.com/SimonvBez/CPUSetSetter/blob/main/docs/setup/AMD.md [V2]
- https://github.com/LordBlacksun/x3d-ccd-optimizer (repo now HTTP 404 — read via search cache only) [V2]
- https://github.com/Coldblackice/AMD-X3D-Vcache-tray [V3]

**Linux design mines**
- https://github.com/EvoXCX/ananicy-cpp , https://github.com/nefelim4ag/Ananicy [V2]
- https://github.com/FeralInteractive/gamemode + example/gamemode.ini + README.md [V1]
- https://lwn.net/Articles/1041668/ (cache-aware scheduling, tunables + benchmarks) [V1]
- https://lwn.net/Articles/1018334/ , https://www.phoronix.com/news/Linux-Cache-Aware-Sched-Nears [V2]
- https://patchew.org/linux/cover.1764801860.git.tim.c.chen@linux.intel.com/ [V2]

**Academic**
- https://queue.acm.org/detail.cfm?id=1709862 (Managing Contention…; primary returned HTTP 403, summary only) [V2]
- https://dl.acm.org/doi/10.1145/1735971.1736036 (ASPLOS'10) [V2]
- https://www.blagodurov.net/files/a8-blagodurov.pdf (ACM Computing Surveys 2012; PDF binary, not parsed) [V2]
- https://arxiv.org/pdf/2402.07693 (LFOC+, cache-clustering) [V3]

<!-- Made with my soul - Swately <3 -->
