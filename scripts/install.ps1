# install.ps1 — idempotent installer for end users.
#
# Usage (from extracted release zip):
#   Right-click → Run with PowerShell (as Administrator)
#   OR from PowerShell:
#     .\install.ps1                # default install
#     .\install.ps1 -Force         # reinstall over existing
#     .\install.ps1 -InstallDir C:\Tools\Phynned  # custom location
#
# Requirements:
#   - Windows 10 build 19041+ or Windows 11
#   - Administrator privileges (for cross-process affinity + ETW)
#   - PowerShell 5.1+ (built into Windows)

[CmdletBinding()]
param(
    [switch]$Force,
    [string]$InstallDir = "$env:LOCALAPPDATA\Phynned",
    [switch]$NoPath,
    [switch]$NoStartMenu,
    [string]$PresentMonUrl = "https://github.com/GameTechDev/PresentMon/releases/download/v2.4.1/PresentMon-2.4.1-x64.exe"
)

$ErrorActionPreference = "Stop"
$installerStart = Get-Date

# ── 1. Admin check (required for ETW + affinity ops) ──────────────────────────
$isAdmin = ([Security.Principal.WindowsPrincipal] `
    [Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole(
    [Security.Principal.WindowsBuiltInRole]::Administrator)

if (-not $isAdmin) {
    Write-Host ""
    Write-Host "Phynned requires Administrator privileges to:" -ForegroundColor Yellow
    Write-Host "  - Open ETW sessions for context-switch tracking"
    Write-Host "  - Set cross-process CPU affinity"
    Write-Host ""
    Write-Host "Please run this script as Administrator:" -ForegroundColor Yellow
    Write-Host "  Right-click install.ps1 -> Run with PowerShell (as Administrator)"
    Write-Host ""
    exit 2
}

Write-Host "===== Phynned installer =====" -ForegroundColor Cyan
Write-Host "Install dir: $InstallDir"
Write-Host ""

# ── 2. Detect existing install ────────────────────────────────────────────────
if ((Test-Path $InstallDir) -and (Get-ChildItem $InstallDir -ErrorAction SilentlyContinue) -and (-not $Force)) {
    Write-Host "Existing installation found at $InstallDir" -ForegroundColor Yellow
    Write-Host "Use -Force to reinstall over it."
    Write-Host "Exiting without changes."
    exit 0
}

# Stop any running agent/ui before overwriting binaries
foreach ($procName in @("phynned-agent", "phynned-ui", "phynned-cli")) {
    $running = Get-Process -Name $procName -ErrorAction SilentlyContinue
    if ($running) {
        Write-Host "Stopping running $procName (PID $($running.Id))..."
        $running | Stop-Process -Force
        Start-Sleep -Milliseconds 500
    }
}

# ── 3. Prepare install directory ──────────────────────────────────────────────
if (-not (Test-Path $InstallDir)) {
    New-Item -ItemType Directory -Path $InstallDir -Force | Out-Null
}

# ── 4. Copy binaries from script's source directory ───────────────────────────
$scriptDir = $PSScriptRoot
if (-not $scriptDir) { $scriptDir = (Get-Location).Path }

Write-Host "Copying binaries..." -ForegroundColor Cyan
$exes = Get-ChildItem -Path $scriptDir -Filter "*.exe" -File
if ($exes.Count -eq 0) {
    Write-Error "No .exe files found in $scriptDir. Make sure you extracted the release zip and are running install.ps1 from inside it."
    exit 3
}

foreach ($exe in $exes) {
    Copy-Item -Path $exe.FullName -Destination $InstallDir -Force
    Write-Host "  $($exe.Name)"
}

# Public docs (subset)
foreach ($doc in @("README.md", "LICENSE", "FAQ.md")) {
    $srcDoc = Join-Path $scriptDir $doc
    if (Test-Path $srcDoc) {
        Copy-Item $srcDoc $InstallDir -Force
    }
}

# Bundled docs folder
$srcDocsDir = Join-Path $scriptDir "docs"
if (Test-Path $srcDocsDir) {
    $dstDocsDir = Join-Path $InstallDir "docs"
    if (Test-Path $dstDocsDir) { Remove-Item -Recurse -Force $dstDocsDir }
    Copy-Item -Recurse $srcDocsDir $dstDocsDir
}

# ── 5. PresentMon (download if not bundled) ───────────────────────────────────
$pmPath = Join-Path $InstallDir "PresentMon.exe"
if (-not (Test-Path $pmPath)) {
    Write-Host "Downloading PresentMon from $PresentMonUrl ..." -ForegroundColor Cyan
    try {
        Invoke-WebRequest -Uri $PresentMonUrl -OutFile $pmPath -UseBasicParsing -TimeoutSec 60
        Write-Host "  PresentMon installed: $pmPath"
    }
    catch {
        Write-Warning "Failed to download PresentMon: $($_.Exception.Message)"
        Write-Warning "Benchmark feature will not work until PresentMon is placed at $pmPath manually."
    }
}

# ── 6. PATH entry (User-scope, no reboot required for new processes) ──────────
if (-not $NoPath) {
    $userPath = [Environment]::GetEnvironmentVariable("Path", "User")
    if ($userPath -notlike "*$InstallDir*") {
        $newPath = if ($userPath) { "$userPath;$InstallDir" } else { $InstallDir }
        [Environment]::SetEnvironmentVariable("Path", $newPath, "User")
        Write-Host "Added $InstallDir to user PATH." -ForegroundColor Green
    }
    else {
        Write-Host "PATH already contains $InstallDir."
    }
}

# ── 7. Start menu shortcut ────────────────────────────────────────────────────
if (-not $NoStartMenu) {
    $startMenuDir = "$env:APPDATA\Microsoft\Windows\Start Menu\Programs"
    $lnkPath = "$startMenuDir\Phynned.lnk"

    $uiExe = Join-Path $InstallDir "phynned-ui.exe"
    if (Test-Path $uiExe) {
        $wshell = New-Object -ComObject WScript.Shell
        $shortcut = $wshell.CreateShortcut($lnkPath)
        $shortcut.TargetPath = $uiExe
        $shortcut.WorkingDirectory = $InstallDir
        $shortcut.IconLocation = $uiExe
        $shortcut.Description = "Phynned — built on Phyriad Framework"
        $shortcut.Save()
        Write-Host "Start menu shortcut created."
    }
}

# ── 8. Verify install ─────────────────────────────────────────────────────────
$installed = @()
foreach ($name in @("phynned-agent.exe", "phynned-ui.exe", "phynned-cli.exe")) {
    $p = Join-Path $InstallDir $name
    if (Test-Path $p) { $installed += $name }
}

Write-Host ""
Write-Host "===== Install complete =====" -ForegroundColor Green
Write-Host "Location:    $InstallDir"
Write-Host "Components:  $($installed -join ', ')"
$dur = ((Get-Date) - $installerStart).TotalSeconds
Write-Host ("Total time:  {0:N1} s" -f $dur)
Write-Host ""
Write-Host "To launch:" -ForegroundColor Cyan
Write-Host "  - Start menu -> Phynned, OR"
Write-Host "  - Open a new terminal and type:  phynned-ui"
Write-Host ""
Write-Host "To uninstall:" -ForegroundColor Cyan
Write-Host "  Remove-Item -Recurse '$InstallDir'"
Write-Host ""
# Made with my soul - Swately <3
