# testbench.ps1 -- standalone build+test for Phynned (self-contained project).
# Owned by this project since the 2026-07-16 separation (it no longer tracks the
# container's scripts/gen_testbench.py — edit freely).
# The build lives IN THIS FOLDER (build/) -- change $BuildRoot to relocate it.
# It exports compile_commands.json so clangd/IntelliSense resolves this unit's includes.
# Usage:  .\testbench.ps1              # full unit: configure + build + all tests
#         .\testbench.ps1 -Filter learn # only tests matching the regex (ctest -R)
param(
    [string]$BuildRoot = $PSScriptRoot,
    [string]$Filter    = ""
)
$unit  = Split-Path -Leaf $PSScriptRoot
$build = Join-Path $BuildRoot "build"
cmake -S $PSScriptRoot -B $build -DPHYNNED_BUILD_TESTS=ON -DPHYRIAD_BUILD_TESTS=ON -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
if (-not $? -or $LASTEXITCODE -ne 0) { Write-Host "[$unit] CONFIG_FAIL"; exit 1 }
cmake --build $build
if (-not $? -or $LASTEXITCODE -ne 0) { Write-Host "[$unit] BUILD_FAIL"; exit 2 }
# ctest exits 0 on an EMPTY test set -- that false green hid a failing test for
# ~9 weeks (caught 2026-07-16). Assert the set is non-empty before trusting it.
$listing = ctest --test-dir $build -N 2>$null | Out-String
if ($listing -notmatch 'Total Tests:\s*(\d+)' -or [int]$Matches[1] -lt 1) {
    Write-Host "[$unit] NO_TESTS_DISCOVERED (false green refused)"; exit 4
}
$total = [int]$Matches[1]
if ($Filter) { ctest --test-dir $build -R $Filter --output-on-failure }
else         { ctest --test-dir $build --output-on-failure }
if (-not $? -or $LASTEXITCODE -ne 0) { Write-Host "[$unit] TESTS_FAIL"; exit 3 }
Write-Host "[$unit] OK ($total tests discovered)"
# Made with my soul - Swately <3
