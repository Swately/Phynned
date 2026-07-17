# package_release.ps1 — produce signed release zip for a tagged build.
#
# Usage:
#   .\scripts\package_release.ps1 -Version 0.5.0
#   .\scripts\package_release.ps1 -Version 0.5.0 -Strip -LTO
#
# Output:
#   phynned-<version>-windows-x64.zip in repo root (target < 20 MB).
#
# Performance notes:
#   - LTO/IPO enabled when -LTO is passed (slower link, smaller faster binaries).
#   - Strip enabled when -Strip is passed (removes debug symbols).
#   - Both ON recommended for public release zips.

param(
    [Parameter(Mandatory=$true)][string]$Version,
    [switch]$Strip = $false,
    [switch]$LTO = $false,
    [string]$BuildDir = "build-release",
    [switch]$Clean = $false
)

$ErrorActionPreference = "Stop"
$repoRoot = (Get-Item $PSScriptRoot).Parent.FullName
Set-Location $repoRoot

Write-Host "===== package_release.ps1 =====" -ForegroundColor Cyan
Write-Host "Version:  $Version"
Write-Host "BuildDir: $BuildDir"
Write-Host "LTO:      $LTO"
Write-Host "Strip:    $Strip"
Write-Host ""

# ── 1. Configure release build ────────────────────────────────────────────────
if ($Clean -and (Test-Path $BuildDir)) {
    Write-Host "Cleaning $BuildDir..."
    Remove-Item -Recurse -Force $BuildDir
}

# Self-contained project since the 2026-07-16 separation: the source is this
# repo's root and the build produces the <build>/phynned-dist layout itself.
$cmakeArgs = @(
    "-S", ".",
    "-B", $BuildDir,
    "-G", "Ninja",
    "-DCMAKE_BUILD_TYPE=Release"
)
if ($LTO)   { $cmakeArgs += "-DCMAKE_INTERPROCEDURAL_OPTIMIZATION=ON" }
if ($Strip) { $cmakeArgs += "-DCMAKE_EXE_LINKER_FLAGS=-s" }

Write-Host "Configuring CMake..."
cmake @cmakeArgs
if ($LASTEXITCODE -ne 0) { throw "CMake configure failed" }

# ── 2. Build ──────────────────────────────────────────────────────────────────
Write-Host "Building (parallel)..."
$buildStart = Get-Date
cmake --build $BuildDir --config Release --parallel
if ($LASTEXITCODE -ne 0) { throw "Build failed" }
$buildDur = ((Get-Date) - $buildStart).TotalSeconds
Write-Host "Build completed in $([math]::Round($buildDur, 1)) s"

# ── 3. Verify expected binaries ───────────────────────────────────────────────
$bins = @(
    "$BuildDir\phynned-dist\runtime\phynned-agent.exe",
    "$BuildDir\phynned-dist\phynned-ui.exe",
    "$BuildDir\phynned-dist\runtime\phynned-cli.exe"
)
foreach ($b in $bins) {
    if (-not (Test-Path $b)) { throw "Missing binary: $b" }
    $sz = (Get-Item $b).Length / 1MB
    Write-Host ("  {0,-50} {1,8:N2} MB" -f $b, $sz)
}

# ── 4. Stage release content ──────────────────────────────────────────────────
$stagingDir = "release-staging"
if (Test-Path $stagingDir) { Remove-Item -Recurse -Force $stagingDir }
New-Item -ItemType Directory -Path $stagingDir | Out-Null

# Binaries
foreach ($b in $bins) { Copy-Item $b $stagingDir }

# PresentMon — required for bench; CMake should have fetched it to build dir
$pmPath = Get-ChildItem -Path $BuildDir -Filter "PresentMon*.exe" -Recurse `
            -ErrorAction SilentlyContinue | Select-Object -First 1
if ($pmPath) {
    Copy-Item $pmPath.FullName "$stagingDir\PresentMon.exe"
    Write-Host "  Bundled PresentMon: $($pmPath.Name)"
} else {
    Write-Warning "PresentMon not found in build dir; install.ps1 will download at install time"
}

# Public-facing docs (curated subset, NOT internal dev docs)
$publicDocs = @(
    "README.md",
    "LICENSE",
    "docs\FAQ.md",
    "scripts\install.ps1"
)
foreach ($d in $publicDocs) {
    if (Test-Path $d) { Copy-Item $d $stagingDir }
    else { Write-Warning "Public doc missing: $d (skipping)" }
}

# Empirical evidence summary (the key differentiator)
$docDir = "$stagingDir\docs"
New-Item -ItemType Directory -Path $docDir -Force | Out-Null
$evidenceDocs = @(
    "docs\reports\EMPIRICAL_EVIDENCE_SUMMARY.md",
    "docs\EMPIRICAL_TEST_PROTOCOL.md"
)
foreach ($d in $evidenceDocs) {
    if (Test-Path $d) { Copy-Item $d $docDir }
}

# Figures (hero plots)
if (Test-Path "docs\figures") {
    Copy-Item -Recurse "docs\figures" "$docDir\figures"
}

# ── 5. Compress ───────────────────────────────────────────────────────────────
$outZip = "phynned-$Version-windows-x64.zip"
if (Test-Path $outZip) { Remove-Item $outZip }

Write-Host "Compressing to $outZip (Optimal level)..."
Compress-Archive -Path "$stagingDir\*" -DestinationPath $outZip -CompressionLevel Optimal

$zipSize = (Get-Item $outZip).Length / 1MB
Write-Host ""
Write-Host "===== Release zip ready =====" -ForegroundColor Green
Write-Host "  $outZip" -ForegroundColor Green
Write-Host ("  Size: {0:N2} MB" -f $zipSize) -ForegroundColor Green

if ($zipSize -gt 20) {
    Write-Warning "Release zip exceeds 20 MB target. Consider:"
    Write-Warning "  - Re-running with -Strip and -LTO for smaller binaries"
    Write-Warning "  - Excluding unused docs/figures"
}

# ── 6. Cleanup staging (preserve for inspection on failure) ───────────────────
Remove-Item -Recurse -Force $stagingDir
Write-Host ""
Write-Host "Done."
# Made with my soul - Swately <3
