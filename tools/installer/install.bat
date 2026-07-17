@echo off
:: Phynned Installer — install.bat
:: Installs phynned-agent.exe and optionally registers it as a Windows service.
::
:: Usage:
::   install.bat               — Install to %ProgramFiles%\Phynned, no service
::   install.bat --service     — Install + register as auto-start service
::   install.bat --user        — Install to %LOCALAPPDATA%\Phynned (no admin needed)
::
setlocal EnableDelayedExpansion

:: ── Parse arguments ────────────────────────────────────────────────────────
set INSTALL_MODE=normal
set INSTALL_DIR=%ProgramFiles%\Phynned
for %%A in (%*) do (
    if /I "%%A"=="--service" set INSTALL_MODE=service
    if /I "%%A"=="--user"    (
        set INSTALL_MODE=user
        set INSTALL_DIR=%LOCALAPPDATA%\Phynned
    )
)

:: ── Check admin (skip for --user) ─────────────────────────────────────────
if not "%INSTALL_MODE%"=="user" (
    net session >nul 2>&1
    if %errorlevel% NEQ 0 (
        echo [Phynned Installer] ERROR: Administrator rights required.
        echo Run this script as Administrator, or use --user for per-user install.
        pause
        exit /b 1
    )
)

echo [Phynned Installer] Installing Phynned to: %INSTALL_DIR%

:: ── Source directory: same directory as this script ───────────────────────
set SCRIPT_DIR=%~dp0
:: Default: the project's dist layout (developer install). This script lives at
:: tools\installer\, so the project root is 2 levels up. The pre-separation
:: tiers (..\..\..\..\build\phynned\ / build_phynned\, the retired container root
:: build) were removed 2026-07-16 — they resolved OUTSIDE the project and could
:: have silently installed foreign binaries if a stale container build existed.
set SOURCE_AGENT=%SCRIPT_DIR%..\..\build\phynned-dist\runtime\phynned-agent.exe
set SOURCE_UI=%SCRIPT_DIR%..\..\build\phynned-dist\phynned-ui.exe
set SOURCE_CLI=%SCRIPT_DIR%..\..\build\phynned-dist\runtime\phynned-cli.exe
set SOURCE_BENCH=%SCRIPT_DIR%..\..\build\phynned-dist\runtime\phynned-bench.exe
set SOURCE_SVC=%SCRIPT_DIR%..\..\build\phynned-dist\runtime\phynned-service-register.exe

:: Release-build tier (build-release\, produced by build-release.bat).
if not exist "%SOURCE_AGENT%" (
    set SOURCE_AGENT=%SCRIPT_DIR%..\..\build-release\phynned-dist\runtime\phynned-agent.exe
    set SOURCE_UI=%SCRIPT_DIR%..\..\build-release\phynned-dist\phynned-ui.exe
    set SOURCE_CLI=%SCRIPT_DIR%..\..\build-release\phynned-dist\runtime\phynned-cli.exe
    set SOURCE_BENCH=%SCRIPT_DIR%..\..\build-release\phynned-dist\runtime\phynned-bench.exe
    set SOURCE_SVC=%SCRIPT_DIR%..\..\build-release\phynned-dist\runtime\phynned-service-register.exe
)

:: Last fallback: same directory as this script (deployed installer ZIP).
if not exist "%SOURCE_AGENT%" (
    set SOURCE_AGENT=%SCRIPT_DIR%phynned-agent.exe
    set SOURCE_UI=%SCRIPT_DIR%phynned-ui.exe
    set SOURCE_CLI=%SCRIPT_DIR%phynned-cli.exe
    set SOURCE_BENCH=%SCRIPT_DIR%phynned-bench.exe
    set SOURCE_SVC=%SCRIPT_DIR%phynned-service-register.exe
)

:: ── Create install directory ───────────────────────────────────────────────
if not exist "%INSTALL_DIR%" (
    mkdir "%INSTALL_DIR%"
    if %errorlevel% NEQ 0 (
        echo [Phynned Installer] ERROR: Could not create directory: %INSTALL_DIR%
        exit /b 1
    )
)

:: ── Copy executables ───────────────────────────────────────────────────────
if exist "%SOURCE_AGENT%" (
    copy /Y "%SOURCE_AGENT%" "%INSTALL_DIR%\phynned-agent.exe"
    echo [Phynned Installer] Installed: phynned-agent.exe
) else (
    echo [Phynned Installer] WARNING: phynned-agent.exe not found at %SOURCE_AGENT%
)

if exist "%SOURCE_UI%" (
    copy /Y "%SOURCE_UI%" "%INSTALL_DIR%\phynned-ui.exe"
    echo [Phynned Installer] Installed: phynned-ui.exe
)

if exist "%SOURCE_CLI%" (
    copy /Y "%SOURCE_CLI%" "%INSTALL_DIR%\phynned-cli.exe"
    echo [Phynned Installer] Installed: phynned-cli.exe
)

if exist "%SOURCE_BENCH%" (
    copy /Y "%SOURCE_BENCH%" "%INSTALL_DIR%\phynned-bench.exe"
    echo [Phynned Installer] Installed: phynned-bench.exe
)

if exist "%SOURCE_SVC%" (
    copy /Y "%SOURCE_SVC%" "%INSTALL_DIR%\phynned-service-register.exe"
    echo [Phynned Installer] Installed: phynned-service-register.exe
)

:: ── Add to PATH (user-level) ───────────────────────────────────────────────
setx PATH "%PATH%;%INSTALL_DIR%" /M >nul 2>&1 || setx PATH "%PATH%;%INSTALL_DIR%" >nul 2>&1
echo [Phynned Installer] Added to PATH: %INSTALL_DIR%

:: ── Create config directory ────────────────────────────────────────────────
set CONFIG_DIR=%LOCALAPPDATA%\Phynned
if not exist "%CONFIG_DIR%" (
    mkdir "%CONFIG_DIR%"
    echo [Phynned Installer] Created config dir: %CONFIG_DIR%
)

:: ── Register Windows service (--service mode only) ────────────────────────
:: Prefiere phynned-service-register.exe (lógica completa: failure actions,
:: description, AutoStart). Fallback a sc.exe si el binario no está.
if "%INSTALL_MODE%"=="service" (
    echo [Phynned Installer] Registering Windows service "PhynnedAgent"...

    if exist "%INSTALL_DIR%\phynned-service-register.exe" (
        :: Stop+delete any pre-existing service.
        "%INSTALL_DIR%\phynned-service-register.exe" uninstall >nul 2>&1
        "%INSTALL_DIR%\phynned-service-register.exe" install --path "%INSTALL_DIR%\phynned-agent.exe"
        if %errorlevel% EQU 0 (
            "%INSTALL_DIR%\phynned-service-register.exe" start >nul 2>&1
            echo [Phynned Installer] Service registered and started: PhynnedAgent
        ) else (
            echo [Phynned Installer] WARNING: Service registration failed.
        )
    ) else (
        :: Fallback path con sc.exe (sin failure actions ni description).
        sc query PhynnedAgent >nul 2>&1
        if %errorlevel% EQU 0 (
            sc stop PhynnedAgent >nul 2>&1
            sc delete PhynnedAgent >nul 2>&1
        )
        sc create PhynnedAgent ^
            binPath= "\"%INSTALL_DIR%\phynned-agent.exe\"" ^
            DisplayName= "Phynned Runtime Optimizer" ^
            start= auto ^
            type= own
        if %errorlevel% EQU 0 (
            sc start PhynnedAgent
            echo [Phynned Installer] Service registered (via sc.exe fallback).
        ) else (
            echo [Phynned Installer] WARNING: Service registration failed.
            echo [Phynned Installer] Run phynned-agent.exe manually as Administrator.
        )
    )
    echo [Phynned Installer] View logs: Event Viewer ^> Windows Logs ^> Application
)

:: ── Create Desktop shortcut for phynned-ui.exe ──────────────────────────────
if exist "%INSTALL_DIR%\phynned-ui.exe" (
    set SHORTCUT=%USERPROFILE%\Desktop\Phynned.lnk
    powershell -Command ^
        "$ws = New-Object -ComObject WScript.Shell; ^
         $s = $ws.CreateShortcut('%SHORTCUT%'); ^
         $s.TargetPath = '%INSTALL_DIR%\phynned-ui.exe'; ^
         $s.WorkingDirectory = '%INSTALL_DIR%'; ^
         $s.Description = 'Phynned Runtime Optimizer'; ^
         $s.Save()" >nul 2>&1
    if exist "%SHORTCUT%" echo [Phynned Installer] Desktop shortcut created: Phynned.lnk
)

echo.
echo [Phynned Installer] Installation complete!
echo   Executable : %INSTALL_DIR%\phynned-agent.exe
echo   UI         : %INSTALL_DIR%\phynned-ui.exe
echo   Config dir : %CONFIG_DIR%
if "%INSTALL_MODE%"=="service" (
    echo   Service    : PhynnedAgent (auto-start)
) else (
    echo   To auto-start, re-run with: install.bat --service
    echo   Or add to Task Scheduler: %INSTALL_DIR%\phynned-agent.exe
)
echo.
pause
endlocal
REM Made with my soul - Swately <3
