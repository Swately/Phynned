# add-defender-exclusion.ps1
# Whitelists Ayama with Windows Defender to prevent false-positive blocks.
#
# Why is this needed?
#   Ayama binaries are unsigned (no commercial code-signing certificate yet).
#   Windows Defender / SmartScreen often flag unsigned executables that
#   manipulate process affinity + spawn child processes as "potentially
#   unwanted" — even though Ayama only calls Win32 APIs (no kernel driver,
#   no code injection, no memory reads of other processes).
#
# What this script does:
#   1. Resolves the directory containing this script (parent of /scripts/).
#   2. Adds that directory as a Microsoft Defender exclusion path.
#   3. Adds each Ayama executable as a Defender process exclusion.
#   4. Adds %LOCALAPPDATA%\Ayama as an exclusion (config + audit logs).
#
# What this script does NOT do:
#   - Disable Defender (it remains active for everything else).
#   - Whitelist arbitrary paths (only the Ayama install location).
#   - Touch Defender's network protection / cloud-delivered protection.
#
# Requirements:
#   - Run as Administrator (Defender configuration requires elevation).
#   - PowerShell 5.1+ (ships with Windows 10 1809+).
#
# Reverting:
#   Pass -Remove to undo all exclusions added by this script:
#     .\add-defender-exclusion.ps1 -Remove

[CmdletBinding()]
param(
    [switch] $Remove
)

$ErrorActionPreference = 'Stop'

# ── Elevation check ────────────────────────────────────────────────────────
$currentUser = [Security.Principal.WindowsIdentity]::GetCurrent()
$principal   = New-Object Security.Principal.WindowsPrincipal($currentUser)
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    Write-Host "ERROR: This script must be run as Administrator." -ForegroundColor Red
    Write-Host "Right-click the script and select 'Run as administrator', or" -ForegroundColor Yellow
    Write-Host "open an elevated PowerShell prompt and re-run." -ForegroundColor Yellow
    exit 1
}

# ── Defender availability check ────────────────────────────────────────────
if (-not (Get-Command Add-MpPreference -ErrorAction SilentlyContinue)) {
    Write-Host "ERROR: Windows Defender cmdlets not available." -ForegroundColor Red
    Write-Host "(Are you on Windows Server Core or a stripped Windows edition?)" -ForegroundColor Yellow
    exit 1
}

# ── Resolve Ayama install directory ────────────────────────────────────────
# This script lives in <install>/scripts/. Walk up one level to find the
# directory containing ayama-ui.exe.
$scriptDir   = Split-Path -Parent $MyInvocation.MyCommand.Path
$ayamaRoot   = Split-Path -Parent $scriptDir
$ayamaConfig = Join-Path $env:LOCALAPPDATA 'Ayama'

if (-not (Test-Path (Join-Path $ayamaRoot 'ayama-ui.exe'))) {
    Write-Host "ERROR: ayama-ui.exe not found in $ayamaRoot" -ForegroundColor Red
    Write-Host "This script expects the layout: <install>/scripts/add-defender-exclusion.ps1" -ForegroundColor Yellow
    Write-Host "                                <install>/ayama-ui.exe" -ForegroundColor Yellow
    exit 1
}

# ── Executables to whitelist ───────────────────────────────────────────────
$exeList = @(
    (Join-Path $ayamaRoot 'ayama-ui.exe'),
    (Join-Path $ayamaRoot 'runtime\ayama-agent.exe'),
    (Join-Path $ayamaRoot 'runtime\ayama-cli.exe'),
    (Join-Path $ayamaRoot 'runtime\ayama-bench.exe'),
    (Join-Path $ayamaRoot 'runtime\ayama-service-register.exe')
)

# ── Apply or remove ────────────────────────────────────────────────────────
if ($Remove) {
    Write-Host "Removing Ayama exclusions from Windows Defender..." -ForegroundColor Cyan
    try { Remove-MpPreference -ExclusionPath $ayamaRoot   -ErrorAction SilentlyContinue } catch {}
    try { Remove-MpPreference -ExclusionPath $ayamaConfig -ErrorAction SilentlyContinue } catch {}
    foreach ($exe in $exeList) {
        try { Remove-MpPreference -ExclusionProcess $exe -ErrorAction SilentlyContinue } catch {}
    }
    Write-Host "Done. Ayama no longer excluded from Defender scans." -ForegroundColor Green
    exit 0
}

Write-Host "Adding Ayama exclusions to Windows Defender..." -ForegroundColor Cyan
Write-Host "  Install path:  $ayamaRoot"  -ForegroundColor Gray
Write-Host "  Config path:   $ayamaConfig" -ForegroundColor Gray
Write-Host ""

# 1. Path exclusions (install dir + config dir)
Add-MpPreference -ExclusionPath $ayamaRoot
Write-Host "  [OK] Path excluded: $ayamaRoot" -ForegroundColor Green

if (-not (Test-Path $ayamaConfig)) {
    New-Item -ItemType Directory -Path $ayamaConfig -Force | Out-Null
}
Add-MpPreference -ExclusionPath $ayamaConfig
Write-Host "  [OK] Path excluded: $ayamaConfig" -ForegroundColor Green

# 2. Process exclusions (each .exe by name)
foreach ($exe in $exeList) {
    if (Test-Path $exe) {
        Add-MpPreference -ExclusionProcess $exe
        Write-Host "  [OK] Process excluded: $(Split-Path -Leaf $exe)" -ForegroundColor Green
    } else {
        Write-Host "  [skip] Not present: $exe" -ForegroundColor DarkGray
    }
}

Write-Host ""
Write-Host "Done. Ayama is now excluded from real-time Defender scanning." -ForegroundColor Green
Write-Host "To undo: .\add-defender-exclusion.ps1 -Remove" -ForegroundColor Gray
# Made with my soul - Swately <3
