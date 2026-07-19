<!-- Phynned research dossier — anti-cheat tolerance of EXTERNAL CPU-affinity changes. -->
<!-- Safety-critical. Every load-bearing claim carries [V1]/[V2]/[V3]. "Could not verify" -->
<!-- is a valid answer and is used where the evidence does not support a confident claim. -->
<!-- Author: research session (Opus), 2026-07-17. Verify-before-claim; zero inflation. -->

# SOTA — Anti-Cheat Tolerance of External CPU-Affinity Changes

**The one question:** which anti-cheat (AC) systems / games tolerate an *external* process
opening a `PROCESS_SET_INFORMATION` handle on the protected game and calling
`SetProcessAffinityMask`, which block or punish it — and is a single "try once, detect
failure, back off" probe genuinely low-risk?

**Verification legend.** `[V1]` = primary source read first-hand (vendor/AC docs, Microsoft,
Bitsum's own pages). `[V2]` = primary-partial, gap declared. `[V3]` = secondary
(forum / GitHub-issue / community). `[V3]` is *admissible* here because per-title lived
behavior is often documented only in forums, but it is never silently upgraded to `[V1]`.

**Scope note vs the sibling dossier.** `SOTA_SAFETY_COMPAT.md` §2 (Facet 2) already covers the
*inherited-affinity* crash path (an affinity change to a launcher/parent propagating into the
protected child and breaking an EAC code-integrity check). This dossier is the *direct-handle*
question — opening a handle on the game process itself — and does not re-derive the inheritance path.

---

## 0. Bottom line up front

- **The mechanism hypothesis is AC-DEPENDENT, not universally true.** "Anti-cheat cares about
  memory/injection rights, not affinity/`SET_INFORMATION`" is **confirmed for Vanguard**
  (which blocks memory-reading tools and, empirically, lets affinity through — matching the
  operator's LoL observation) but **refuted for EAC** (which empirically denies affinity with
  a clean `ACCESS_DENIED`). Whether `PROCESS_SET_INFORMATION` survives depends on whether a
  given AC strips by **blacklist** (remove only memory/injection rights → affinity survives)
  or **whitelist** (allow only a small read set → affinity stripped too). Both patterns are
  documented in the wild.
- **Three distinct AC behaviours exist and the design must treat them differently:**
  - **(a) CLEAN BLOCK** — handle stripped → `SetProcessAffinityMask` fails with a catchable
    `ERROR_ACCESS_DENIED`. *Safe to probe: the failure is the signal.* (EAC.)
  - **(b) ALLOW** — affinity change succeeds, no punishment. (VAC; Vanguard for the affinity op
    itself; base BattlEye policy.)
  - **(c) SILENT-THEN-PUNISH** — the handle-open / affinity call is *monitored and reported*
    with **no error to catch**, and a kick/flag/ban may follow server-side. **This is the
    dangerous class a "detect-failure-and-back-off" probe cannot protect against**, because
    there is no failure to detect. (Ricochet; Destiny 2's strict BattlEye implementation.)
- **A single `SET_INFORMATION`-only probe is low-risk on handle-stripping ACs and NOT provably
  low-risk on monitor-and-report ACs.** The correct design is therefore **not** "probe every
  title" but **"exclude competitive kernel-AC titles by allowlist, and where you do probe, open
  with `PROCESS_SET_INFORMATION` only (never `VM_READ`/`VM_WRITE`/`ALL_ACCESS`)."** This mirrors
  what Process Lasso — the closest prior art — actually does (launcher inheritance + registry
  priority, avoiding a direct handle on the protected game), and its documented zero-ban record
  is a claim about *that* pattern, not about raw direct probing.

---

## 1. The mechanism — where and what is stripped

### 1.1 Detection is (mostly) at handle-open time, via `ObRegisterCallbacks`

Kernel ACs register an **`ObRegisterCallbacks`** pre-operation callback for
`OB_OPERATION_HANDLE_CREATE` / `HANDLE_DUPLICATE`. In the pre-op callback the driver **edits
`Parameters->CreateHandleInformation.DesiredAccess` *before the handle is created*** — so the
requesting process receives a handle with the dangerous bits already removed, and any later API
call needing those bits fails with `ERROR_ACCESS_DENIED`. This is Microsoft's supported
process-protection mechanism, originally for AV, now used by "almost every kernel anticheat."
[V1 — mechanism read first-hand on XPN InfoSec (an AVG self-protection driver, same API) and
corroborated technically by the s4dbrd kernel-AC writeup] [V3 — that EAC/BE/Vanguard specifically
use it: guidedhacking / s4dbrd community writeups].

**Consequence for the load-bearing question:** the decision happens **at `OpenProcess` time, not
at the `SetProcessAffinityMask` call**. If the AC strips the affinity right, your *handle-open*
already lacks it and the affinity call returns a clean error. That is the (a) case and it is
detectable.

**Important exception — Vanguard is not purely open-time.** Riot Vanguard *additionally* runs a
resident system thread that **periodically inspects the handle table of every process** and
strips/patches privileged handles after the fact (KSystemInformer-style direct kernel object
manipulation). So for Vanguard, an already-open handle can be neutralised *later*, not only at
open. [V3 — kkent030315 "Van1338" Vanguard bounty journal]. This means Vanguard has a small (c)
tail even when the initial open succeeds.

### 1.2 Which rights get stripped — blacklist vs whitelist (the crux)

`SetProcessAffinityMask` requires **`PROCESS_SET_INFORMATION` (0x0200)**; the read-side helpers
use `PROCESS_QUERY_(LIMITED_)INFORMATION`. `PROCESS_SET_LIMITED_INFORMATION` is `0x2000`. [V1 —
Microsoft Learn, "Process Security and Access Rights"].

Two documented stripping styles produce **opposite** outcomes for affinity:

- **Blacklist (typical kernel-AC pattern):** strip only the memory/injection rights —
  `PROCESS_VM_READ`, `PROCESS_VM_WRITE`, `PROCESS_VM_OPERATION`, `PROCESS_DUP_HANDLE`
  (often `PROCESS_CREATE_THREAD`, `PROCESS_SUSPEND_RESUME`). **`PROCESS_SET_INFORMATION` is
  *not* in this list → the affinity right survives → the affinity call succeeds.** This is the
  behaviour described for generic kernel anti-cheats and named specifically for EAC's *primary*
  defense (VM_READ/VM_WRITE). [V3 — s4dbrd "How Kernel Anti-Cheats Work"; secret.club / general
  EAC RE writeups]. → supports the operator's hypothesis.
- **Whitelist (AV-style, and the observed EAC affinity behaviour):** allow only a small read set
  (`PROCESS_VM_READ, PROCESS_QUERY_(LIMITED_)INFORMATION, READ_CONTROL, SYNCHRONIZE`) and drop
  **everything else, including `PROCESS_SET_INFORMATION`, `PROCESS_VM_WRITE`,
  `PROCESS_SUSPEND_RESUME`.** Under this style the affinity right is gone and the call fails
  clean. [V1 — XPN InfoSec, verbatim whitelist, for an AVG driver using the identical API].

**Verdict on the hypothesis:** *partially true and AC-specific.* The blacklist pattern (affinity
survives) is real and explains Vanguard/LoL allowing affinity; the whitelist pattern (affinity
stripped) is also real and matches EAC's empirically observed `ACCESS_DENIED` on affinity. The
generic RE writeups that list "only VM rights" describe the blacklist style and **should not be
read as proof that every AC leaves `SET_INFORMATION` alone** — EAC's own titles refute that
empirically (§3). **Honest gap:** I did not obtain a first-hand disassembly of EAC's or
Vanguard's exact `DesiredAccess` mask edit; the per-AC affinity outcomes below are anchored on
**empirical per-title behaviour**, which is what actually matters for the (a)/(b)/(c) call.

---

## 2. Per-AC classification (the deliverable)

Kernel vs usermode baseline [V3 — levvvel kernel-AC survey; corroborated per-vendor below]:
kernel = Vanguard, EAC, BattlEye, Ricochet, nProtect GameGuard, XIGNCODE3, Denuvo AC, EA AC;
usermode = **VAC**.

| AC system | Behaviour on external `SET_INFORMATION`+`SetProcessAffinityMask` | Class | Catchable error? | Tag |
|---|---|---|---|---|
| **Easy Anti-Cheat (EAC)** | Direct affinity denied; `ACCESS_DENIED` (Halo MCC, Armored Core VI) or game exits on white screen (Elden Ring). Bitsum's workaround is launcher-inheritance *because* the direct path is blocked. | **(a) CLEAN BLOCK** | **Yes** — `ERROR_ACCESS_DENIED` | [V3]×multiple titles + [V1] Bitsum |
| **Riot Vanguard** | Blocks *memory-reading* tools ("no allow list"); does not enumerate affinity/process tools. Affinity op empirically succeeds (operator: LoL allows). Residual: periodic handle-table sweep can neutralise handles later. | **(b) ALLOW** (op succeeds) with **(c) tail** | Partly — open succeeds, later strip is silent | [V1] Riot FAQ + [V2] operator-observed + [V3] Van1338 |
| **Valve VAC** | Usermode, signature/behaviour scan; checks for foreign processes *hooked into game binaries*; no `ObRegisterCallbacks` handle stripping. External affinity is not a cheat signature. | **(b) ALLOW** | N/A (no strip) | [V3] levvvel + [V1] Bitsum (works on CS2/Dota2) |
| **BattlEye** | Vendor policy: bans **only** for actual cheats; **may *kick*** (not ban) for e.g. macro tools; non-cheat overlays generally supported. Base behaviour is permissive. | **(b) ALLOW** (per vendor) | Kick = observable; ban = not | [V1] BattlEye FAQ |
| **BattlEye — Destiny 2 impl.** | Bungie layers strict policy: "any application that attempts to interact… can result in a ban"; a user was **banned for merely having Process Hacker open ~10–15 min** while D2 ran. Report-based, server-side. | **(c) SILENT-THEN-PUNISH** | **No** | [V3] Bungie forum + [V3] GH issue #1035 |
| **Activision Ricochet** | Kernel driver that **"monitors and reports applications that attempt to interact with protected titles"** — detection is heuristic + server-side, not a clean local block. | **(c) SILENT-THEN-PUNISH** | **No** | [V1] Activision overview |
| **nProtect GameGuard** | Kernel rootkit-style, aggressive. **Could not verify** affinity-specific behaviour first-hand. | **UNVERIFIED** (assume (a)/(c)) | Unknown | [V3] classification only |
| **XIGNCODE3** | Kernel/usermode hybrid, Korean-MMO standard. **Could not verify** affinity-specific behaviour. | **UNVERIFIED** (assume (a)/(c)) | Unknown | [V3] classification only |
| **Denuvo Anti-Cheat** | Kernel-mode. **Could not verify** affinity-specific behaviour; not covered by the sources read. | **UNVERIFIED** (assume (a)/(c)) | Unknown | Could not verify |

**Reading the table for design:** treat **(a)** as safe-to-probe (the error is the answer);
treat **(b)** as tolerant but never "guaranteed forever"; treat **(c)** and every **UNVERIFIED**
row as **do-not-touch by allowlist** — a probe cannot protect you where there is no error to catch.

---

## 3. Per-title evidence

- **League of Legends (Vanguard) — ALLOWS [V2 operator-observed, consistent with V1 policy].**
  Riot's own third-party FAQ says only *memory-reading* external tools stop working and there is
  "no allow list"; process/affinity utilities are not named as blocked. This is consistent with
  the operator's empirical "LoL ALLOWS affinity." Not independently reproduced by me first-hand;
  labelled honestly.
- **VALORANT (Vanguard) — ALLOW for the affinity op, same basis [V1 policy + V2].** VALORANT
  runs Vanguard boot-start and is the stricter title; treat the (c) tail (periodic handle-table
  sweep) as real. Recommend excluding it regardless of the affinity op succeeding.
- **Halo: MCC (EAC) — BLOCKS [V3].** Community reports: "EAC prevents you changing core affinity,
  even in [Process Lasso]"; users hit "Access Denied." **Corroborates the operator's Halo-MCC =
  BLOCKS observation.** Clean (a) failure.
- **Armored Core VI (EAC) — BLOCKS [V3].** "EAC prevents you changing core affinity, even in that
  tool"; the affinity workaround itself returns "Access Denied." Same (a) pattern; the core-0
  parking issue there had to be fixed by *renaming the EXE to disable EAC*, which drops
  multiplayer — evidence the direct path is genuinely closed.
- **Elden Ring (EAC) — BLOCKS direct; inherit via AC exes [V3].** "if I select the current
  affinity, elden ring will close on the white screen." Workaround = set affinity on
  `easyanticheat_eso.exe` / `launch_protected_game.exe` while EAC loads, not on the game. Confirms
  direct-handle affinity is not tolerated; the launcher/AC-process inheritance path is.
- **Apex / Fortnite / Rust (EAC/BE) — no title-specific first-hand affinity report obtained.**
  Inference from the EAC pattern only; **could not verify** per title. Flagged, not asserted.
- **Call of Duty (Ricochet) — UNCONFIRMED ban correlation [V3].** A user reported a Warzone ban
  while running "a CPU limiter"/Process Lasso; **the community and a moderator disputed causation
  and Activision never named the software.** Treat as *unconfirmed correlation, not evidence of an
  affinity ban.* What *is* [V1]: Ricochet "monitors and reports applications that interact with
  protected titles" — i.e. the (c) risk class exists by design even if this one ban is unproven.
- **CS2 / Dota 2 (VAC) — ALLOW [V3 + V1 Bitsum].** VAC is usermode signature/hook detection;
  external affinity is not a signature. Process Lasso is used on these titles without a documented
  affinity ban.
- **Destiny 2 (BattlEye) — DANGEROUS (c) [V3].** GitHub issue #1035: user **banned** after having
  Process Hacker open ~10–15 min alongside D2. Bungie moderator: "any application that attempts to
  interact… can result in a ban… if [the] anticheat detects any interaction from an outside source,
  it will pop a flag and submit a report… which can lead to a ban." **Caveat that keeps this
  honest:** Process Hacker is a *memory* tool (it can read/write process memory), so this is not a
  pure affinity analog — but it proves Destiny 2's AC punishes *presence/interaction of external
  process tools* heuristically, with no local error. That is exactly the class a back-off probe
  cannot defend against.

**Does Process Lasso / an affinity tool "work" per title?** Yes on VAC titles and (via
launcher/AC-exe inheritance) on EAC titles; **the tool's own guidance is to avoid the direct
handle on the protected game** and use inheritance/registry instead — which is precisely why its
zero-ban record cannot be transferred to a design that opens the handle directly.

---

## 4. The probe-risk question — is a single try low-risk?

**Split answer, by class:**

- **On handle-stripping ACs (EAC; whitelist Vanguard):** a single `OpenProcess` +
  `SetProcessAffinityMask` returns a clean `ERROR_ACCESS_DENIED` at handle-open. **No memory is
  touched, no cheat signature is produced, and the failure is the signal.** This is genuinely
  low-risk and is the model case for "try once, detect failure, back off." [Reasoned from §1
  mechanism [V1] + EAC empirical [V3].]
- **On monitor-and-report ACs (Ricochet; Destiny 2's BattlEye):** the *act of opening a handle on
  the protected process* is itself the monitored, loggable, server-reportable event — and it
  **succeeds locally, so there is no error to catch and back off from.** A single probe here is
  **not provably low-risk.** [V1 Ricochet "monitors and reports"; [V3] Destiny-2 presence-based ban].
- **Bitsum / Process Lasso documented stance [V1]:** *"No, using Process Lasso will not cause bans.
  There has never been a single case of such… Process Lasso can't be used for cheating in any way…
  No direct access to process memory is ever made."* Worst case: *"if the rules you set are somehow
  problematic for a game, it will refuse to launch."* This is the strongest reassurance available —
  **but it is a claim about Process Lasso's pattern**, which sets affinity on the *launcher* and
  uses *registry* priority to reach AC-protected processes, deliberately **not** opening a direct
  handle on the protected game. A raw direct probe is outside the behaviour that record covers.
- **Ban vs kick vs nothing — the distinction the operator asked for:**
  - **Nothing / clean error:** EAC (a), VAC (b), Vanguard affinity op (b).
  - **Kick (recoverable, no cheater flag):** BattlEye's documented behaviour for some tools
    [V1]; Vanguard blocking an incompatible tool from running.
  - **Ban (design-fatal):** only *documented* against external process-tool *presence* on Destiny 2
    [V3], and *alleged-but-unconfirmed* on Warzone/Ricochet [V3]. **No source ties a permanent ban
    to a single external affinity call specifically.** That absence is itself a finding: bans in the
    corpus attach to memory tools / persistent patterns / strict title policy, not to one affinity
    poke — but "no documented case" is not "proven safe," especially for kernel ACs that change
    policy without notice.

**Design implication (the safety-critical part):** a single probe is a *sound* strategy **only for
the (a) class**, where the OS/AC hands you a catchable error. For (c) and every UNVERIFIED AC, the
probe model fails silently by construction; the only safe control is an **allowlist that excludes
competitive kernel-AC titles up front**, plus, if a probe is ever issued, requesting **only
`PROCESS_SET_INFORMATION` (0x0200)** — never `PROCESS_VM_READ/WRITE` or `PROCESS_ALL_ACCESS`, since
it is the *memory* rights that read as "cheat" to every AC in §1.

---

## 5. ToS / policy angle (brief)

- **Destiny 2 / Bungie [V3]:** "*any application that attempts to interact, overwrite, or otherwise
  modify the Destiny 2 processes or code can result in a ban.*" → *any* external interaction is a
  policy harm regardless of technical outcome; a design that even opens a handle is out of policy.
- **Riot Vanguard [V1]:** "*no one is exempt… no allow list.*" Memory-reading tools are blocked;
  process tools aren't named, but there is no sanctioned-tool carve-out. A block/kick is a design
  harm for a performance tool even without a ban.
- **BattlEye [V1]:** bans only for actual cheats, but **may kick** for some non-cheat tools — a kick
  is still a bad user outcome for a performance utility.
- **VAC [V3]:** scoped to cheat signatures / foreign hooks into game binaries; external affinity is
  outside its remit.
- **Net:** even where **no ban** exists, a **kick or launch-refusal is a real design harm**. For any
  competitive kernel-AC title the correct product stance is *do not touch the game process at all*
  (allowlist exclusion), not *probe and hope for a clean error.*

---

## 6. Honest gaps (named, not hidden)

1. **No first-hand disassembly** of EAC's or Vanguard's exact `DesiredAccess` mask edit — the
   affinity-specific verdicts rest on empirical per-title behaviour, not on my reading their driver.
2. **nProtect GameGuard, XIGNCODE3, Denuvo AC:** kernel classification only; **no affinity-specific
   evidence obtained** — treated as do-not-touch, not asserted either way.
3. **Ricochet ban causation is unproven** — the one Warzone anecdote is disputed correlation; the
   (c) classification rests on Activision's *own* "monitors and reports" wording, which is about
   design intent, not a confirmed affinity ban.
4. **Reddit was not machine-readable** for this session (blocked to the fetcher), so several
   community reports could not be pulled first-hand; per-title items are anchored on Steam/Bungie/
   GitHub/vendor sources instead.
5. **Kernel ACs change policy silently.** Every (b) ALLOW here is "as observed," not a guarantee;
   an AC update could reclassify affinity handling without notice.

---

## Sources

Primary / vendor [V1]:
- Microsoft Learn — Process Security and Access Rights (`PROCESS_SET_INFORMATION` 0x0200; `PROCESS_SET_LIMITED_INFORMATION` 0x2000): https://learn.microsoft.com/en-us/windows/win32/procthread/process-security-and-access-rights
- XPN InfoSec — Windows Anti-Debug: OpenProcess filtering (ObRegisterCallbacks pre-op strips at open time; whitelist includes/excludes `SET_INFORMATION`): https://blog.xpnsec.com/anti-debug-openprocess/
- Bitsum / Process Lasso FAQ (no-bans stance; EAC launcher-inheritance workaround; "refuse to launch" worst case): https://bitsum.com/process-lasso-faq/
- BattlEye — official FAQ (bans only for cheats; may kick for some tools): https://www.battleye.com/support/faq/
- Riot Games — Vanguard FAQ for Third-Party Applications ("external tools reading memory will no longer work… no allow list"): https://www.riotgames.com/en/DevRel/vanguard-faq
- Activision Support — RICOCHET Anti-Cheat overview (kernel driver "monitors and reports applications that attempt to interact with protected titles"): https://support.activision.com/articles/ricochet-overview

Technical secondary [V3]:
- s4dbrd — How Kernel Anti-Cheats Work (ObRegisterCallbacks strips VM_READ/VM_WRITE/VM_OPERATION/DUP_HANDLE via DesiredAccess edit): https://s4dbrd.github.io/posts/how-kernel-anti-cheats-work/
- guidedhacking — Anticheat Bypass: ObRegisterCallbacks Blocking Handle Creation: https://guidedhacking.com/threads/anticheat-bypass-obregistercallbacks-blocking-handle-creation.16164/
- secret.club — CVEAC-2020: Bypassing EasyAntiCheat integrity checks: https://secret.club/2020/04/08/eac_integrity_check_bypass.html
- kkent030315 — "Van1338" Riot Vanguard bounty journal (periodic handle-table inspection + kernel object mask stripping): https://github.com/kkent030315/Van1338
- levvvel — Every game with kernel-level anti-cheat (kernel vs usermode classification; VAC excluded as user-mode): https://levvvel.com/tech/games-with-kernel-level-anti-cheat-software/

Per-title community reports [V3]:
- Halo MCC — EAC ruining frame rates (affinity blocked): https://steamcommunity.com/app/976730/discussions/0/2666626316170807961/
- Armored Core VI — "EAC prevents you changing core affinity… Access Denied": https://steamcommunity.com/app/1888160/discussions/0/3820795131608163674/
- Elden Ring — change CPU affinity without losing online (direct closes on white screen; use AC exes): https://steamcommunity.com/app/1245620/discussions/0/6725644068305330569/
- Process Lasso "Unlock more Cores" guide (launcher-inheritance for EAC titles): https://steamcommunity.com/sharedfiles/filedetails/?id=3042130454
- Destiny 2 — Process Lasso compatibility (Bungie mod: any outside interaction can flag/ban): https://www.bungie.net/en/Forums/Post/256058990
- Process Hacker / System Informer — "Bungie bans" issue #1035 (banned for merely having PH open ~10–15 min): https://github.com/processhacker/processhacker/issues/1035
- Call of Duty MW2/Warzone — "banned for unauthorized software" (Process Lasso; causation disputed, unconfirmed): https://steamcommunity.com/discussions/forum/0/3392923906950002494/

<!-- Made with my soul - Swately <3 -->
