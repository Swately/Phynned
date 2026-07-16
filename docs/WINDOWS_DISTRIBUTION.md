# Windows Distribution Notes

How Phyriad's Windows binaries are packaged for end users — the design
choices behind static MinGW runtime linking and the Defender false-positive
mitigation. This document targets:

- **Maintainers** auditing the build-time choices
- **Downstream packagers** wanting to repackage Ayama (Chocolatey, Scoop,
  custom installers)
- **Users** wondering "why is there a PowerShell script in the dist?"

For end-user usage instructions, see the project [`README.md`](../README.md).

---

## 1. The MinGW runtime DLL problem

### 1.1 Symptom

A user on a fresh Windows machine downloads the Ayama dist, double-clicks
`ayama-ui.exe`, and gets three consecutive Windows error dialogs:

```
The code execution cannot proceed because libwinpthread-1.dll was not found.
Reinstalling the program may fix this problem.
```
```
The code execution cannot proceed because libstdc++-6.dll was not found.
...
```
```
The code execution cannot proceed because libgcc_s_seh-1.dll was not found.
...
```

The binary then fails to start. This is **not** a packaging bug —
it's the default behavior of MinGW-w64 gcc when linking C++ binaries.

### 1.2 Cause

MinGW gcc dynamically links three runtime libraries by default:

| DLL | Provides | Origin |
|---|---|---|
| `libstdc++-6.dll`     | C++ standard library implementation | gcc's libstdc++ |
| `libgcc_s_seh-1.dll`  | GCC runtime support (SEH stack unwinding, divisions) | gcc's libgcc |
| `libwinpthread-1.dll` | POSIX threads (used by `std::thread`, `std::mutex`, etc.) | MinGW-w64 |

On the developer's machine, these DLLs live in `<mingw>/bin/` and are
already on PATH (the developer installed MinGW). On a user's machine,
they don't exist anywhere. The binary fails before `main()` runs.

### 1.3 Three possible solutions (and why we picked one)

| Option | What | Trade-off |
|---|---|---|
| **A. Ship the DLLs alongside the .exe** | Copy the 3 DLLs from `<mingw>/bin/` to `ayama-dist/` | Works, but clutters the dist; users see 4 files instead of 1; users may copy only the .exe and break it |
| **B. Static linking** | Link libstdc++/libgcc/libwinpthread *into* the .exe | Single self-contained .exe; +2-3 MB per binary; license compatibility required |
| **C. Mandate MSVC** | Switch the dist build to MSVC (Visual Studio runtime is shipped with Windows) | Requires MSVC build of every dependency; rejects gcc-specific extensions; not portable across our two CI toolchains |

**We chose B (static linking)** for the distribution build, for three
reasons:

1. **Single-file dist matches user expectations.** Most modern Windows
   apps ship as one .exe at the root. Adding 3 mystery DLLs raises
   "is this malware?" questions for non-technical users.
2. **The size cost is acceptable.** `ayama-ui.exe` grew from ~5 MB
   (dynamic) to 7.2 MB (static). On modern bandwidth this is invisible.
3. **License compatibility is clean.** All three runtimes explicitly
   allow static linking (see §1.5).

Option A is still a valid fallback if a user has a strong reason to
avoid the size increase. Set `CMAKE_CXX_FLAGS="-O2"` (no `-static`)
manually for that case. We don't expose this as a CMake option because
the cost-benefit is overwhelming for distribution.

### 1.4 How the static link is configured

In the root `CMakeLists.txt`:

```cmake
if(WIN32 AND CMAKE_CXX_COMPILER_ID MATCHES "GNU")
    function(phyriad_static_mingw_runtime _target)
        target_link_options(${_target} PRIVATE
            -static
            -static-libgcc
            -static-libstdc++
        )
    endfunction()
else()
    function(phyriad_static_mingw_runtime _target)
    endfunction()
endif()
```

A duplicate fallback definition lives in `apps/ayama/CMakeLists.txt` so
the Ayama standalone build (`build.bat`) works without the root project.
**Keep the two in sync** if you modify the flags.

The function is called once per end-user binary in
`apps/ayama/CMakeLists.txt`:

```cmake
foreach(_ayama_exe ayama-ui ayama-agent ayama-cli ayama-bench ayama-service-register)
    if(TARGET ${_ayama_exe})
        phyriad_static_mingw_runtime(${_ayama_exe})
    endif()
endforeach()
```

#### Why `-static` and `-static-libgcc -static-libstdc++` together

You'd think `-static` alone is enough, but MinGW gcc treats them as
separate switches with overlapping effects:

- `-static-libgcc` — static link **libgcc** only.
- `-static-libstdc++` — static link **libstdc++** only.
- `-static` — static link **everything that has a static archive**.
  On MinGW that means libgcc, libstdc++, libwinpthread. System DLLs
  (kernel32, user32, opengl32, …) stay dynamic because there are no
  static archives for them.

The three together are belt-and-suspenders: the explicit per-library
flags survive any future change to `-static`'s default behavior.

Earlier iterations tried `-Wl,-Bstatic -lwinpthread -Wl,-Bdynamic` — a
narrower scope intended to keep everything else dynamic. **This did not
work** because MinGW gcc re-adds `-lwinpthread` automatically at the
end of the link command (after the explicit `-Wl,-Bdynamic`), so
winpthread stayed dynamic. `-static` is the only reliable approach.

### 1.5 License compatibility for static linking

All three runtimes are explicitly safe to static-link from non-GPL code:

#### libgcc and libstdc++

GPL v3 + the **[GCC Runtime Library Exception](https://www.gnu.org/licenses/gcc-exception-3.1.html)**.
The exception specifically permits static linking from any program
compiled by an "Eligible Compilation Process" — which gcc itself is.
Quoting the exception:

> You have permission to propagate a work of Target Code formed by
> combining the Runtime Library with Independent Modules, even if such
> propagation would otherwise violate the terms of GPLv3, provided
> that all Target Code was generated by Eligible Compilation Processes.

In plain English: if you compiled it with gcc, you can statically link
gcc's runtime, no copyleft propagation.

#### libwinpthread

MIT license. No restrictions on linking.

### 1.6 Verification

After a build, verify with `objdump` (or `dumpbin` if you have MSVC tools):

```bash
objdump -p build/ayama-dist/ayama-ui.exe | grep "DLL Name:"
```

Expected output — system DLLs only, no `lib*` entries:

```
DLL Name: ADVAPI32.dll
DLL Name: GDI32.dll
DLL Name: KERNEL32.dll
DLL Name: api-ms-win-crt-*.dll
DLL Name: ole32.dll
DLL Name: OPENGL32.dll
DLL Name: pdh.dll
DLL Name: SHELL32.dll
DLL Name: USER32.dll
```

If `libgcc_s_seh-1.dll`, `libstdc++-6.dll`, or `libwinpthread-1.dll`
appears, the static link failed (likely a CMake regression).

---

## 2. The Windows Defender false-positive problem

### 2.1 Symptom

When a user runs `ayama-ui.exe` for the first time, Windows Defender
may quarantine it as *"Trojan:Win32/Wacatac.B!ml"* or similar
machine-learning-driven detection. SmartScreen may also show
*"Windows protected your PC"* on first download.

### 2.2 Cause

Three factors compound:

1. **Unsigned binary.** Ayama v1.0 has no commercial code-signing
   certificate (target for v1.1). Any unsigned new binary has near-zero
   SmartScreen reputation by default.

2. **Process-affinity API surface.** Ayama calls
   `SetProcessAffinityMask`, `SetPriorityClass`, and
   `OpenProcess(PROCESS_SET_INFORMATION, ..., other_pid)` on processes
   owned by other users. These exact API calls are textbook signals
   for cheats, miners, and "FPS booster" grey-market tools — which
   Defender's heuristics flag aggressively.

3. **ETW kernel session.** Ayama opens an ETW kernel-mode tracing
   session (`EVENT_TRACE_FLAG_PROCESS | _IMAGE_LOAD`) to enumerate
   game processes without polling the process table. ETW kernel
   sessions are a feature, not a hack, but Defender's ML models pick
   them up as one more "this binary is monitoring the system" signal.

Ayama is verifiably benign — Win32-only, no kernel driver, no code
injection, no memory reads of other processes' address spaces (see
[`action/`](../action/) for the complete API
inventory). But Defender's ML cannot prove that from the binary alone
without a signature establishing provenance.

### 2.3 Options we considered

| Approach | Implementation cost | User friction | Status |
|---|---|---|---|
| **A. Disable Defender entirely** | One PowerShell command | Catastrophic — exposes the whole system | **Rejected** |
| **B. Per-binary Defender exclusion (script)** | ~150 LOC PowerShell | One admin-elevated double-click on first install | **Shipped** |
| **C. Submit to Microsoft for SmartScreen reputation** | Web form submission per release | Slow ramp-up; reputation builds over downloads | **Recommended in parallel with B** |
| **D. Code-signing certificate (commercial EV/OV)** | $200-500/year + signing infra in CI | Zero — eliminates the warning entirely | **v1.1 milestone** |
| **E. Free open-source signing (SignPath)** | Application + project review (1-2 weeks) | Zero | **Applied; pending approval** |

We chose **B** as the v1.0 mitigation because:

- It works **today**, no waiting on Microsoft or paying $500.
- It's **scoped** — only Ayama's specific paths and processes are
  whitelisted; everything else stays under Defender.
- It's **reversible** — `add-defender-exclusion.ps1 -Remove` undoes
  the entire change cleanly.
- It's **transparent** — the PowerShell source is 150 lines, plain
  text, auditable by anyone before running.

D and E are the **real** long-term fix. B is the bridge until then.

### 2.4 What the script does

```
apps/ayama/scripts/add-defender-exclusion.ps1
```

Logical flow:

1. **Elevation check.** Refuses to run without admin rights. Defender
   config requires it; we don't try to escalate silently.

2. **Path detection.** Resolves the install root from
   `$MyInvocation.MyCommand.Path` — the script's own location. This
   means it works regardless of where the user installs Ayama (Program
   Files, Documents, USB stick, anywhere). **No hardcoded paths.**

3. **Layout sanity check.** Verifies `<install>/ayama-ui.exe` exists.
   If not, refuses to run so the user doesn't accidentally whitelist
   the wrong directory.

4. **Path exclusions.** Adds two directories to
   `Add-MpPreference -ExclusionPath`:
   - `<install>` — the Ayama install root
   - `%LOCALAPPDATA%\Ayama` — runtime config + audit logs

5. **Process exclusions.** Adds each Ayama executable to
   `Add-MpPreference -ExclusionProcess`:
   - `ayama-ui.exe`, `ayama-agent.exe`, `ayama-cli.exe`,
     `ayama-bench.exe`, `ayama-service-register.exe`

6. **Result reporting.** Per-line status with explicit *[OK]* /
   *[skip]* indicators so the user knows exactly what happened.

The script uses **only Microsoft's official `Add-MpPreference` cmdlet**
— no registry tampering, no policy hacks, no service stops. This is
the same API the Windows Security UI uses internally.

### 2.5 Why a script, not the installer

We don't ship an MSI installer for Ayama v1.0 (the dist is a
zip-extract layout). When v1.1 introduces an installer, the Defender
exclusion will be **opt-in** during install — the script remains for
users who already extracted the zip directly. Both paths converge on
the same `Add-MpPreference` calls.

### 2.6 What the script does NOT do

- Does **not** disable Defender.
- Does **not** disable SmartScreen, cloud-delivered protection, or
  network protection.
- Does **not** modify Windows Firewall.
- Does **not** add exclusions outside `<install>` and
  `%LOCALAPPDATA%\Ayama`.
- Does **not** survive an Ayama uninstall — paths it excludes simply
  point to non-existent locations after uninstall; harmless, but a
  paranoid user can run `-Remove` first.
- Does **not** persist across Windows OS reset / fresh-install.

### 2.7 The signing roadmap

The Defender exclusion is a band-aid. Real production-grade
distribution requires **code-signing**:

| Step | Status | Cost |
|---|---|---|
| Apply to SignPath open-source program | In progress | $0 |
| Set up GitHub Actions release-signing workflow | Designed, not implemented | $0 (uses SignPath bot) |
| Acquire commercial EV cert as fallback | Deferred to v1.1 | $200-500/year |
| Submit signed binaries to Microsoft SmartScreen | Will happen automatically once signed | $0 |

When signing lands in v1.1, the Defender script remains shipped for
historical compatibility but its README section gets a *"not needed if
you have v1.1+"* note.

---

## 3. Implementation reference

The build-time pieces:

- Root `CMakeLists.txt` — defines `phyriad_static_mingw_runtime()`
  (alongside `phyriad_test_dll_env()`, the test-time MinGW runtime
  helper used in the framework's own tests).
- `apps/ayama/CMakeLists.txt` — duplicate fallback definition (for
  standalone Ayama builds) + `foreach` loop that applies it to every
  end-user binary + `configure_file` that copies
  `scripts/add-defender-exclusion.ps1` into `ayama-dist/scripts/`.
- `apps/ayama/scripts/add-defender-exclusion.ps1` — the script itself.

To audit changes: `git log -- apps/ayama/scripts/ apps/ayama/CMakeLists.txt`.

## 4. Related documents

- The project [`README.md`](../README.md) — end-user-facing
  documentation; mentions the script in the Defender section but does
  not explain the design rationale.
- If you submit a PR that touches the static link flags or the
  exclusion script, please link to a build that produces a binary with
  `objdump -p` output showing zero `lib*` MinGW DLL dependencies.
