@echo off
:: Ayama Installer — install.bat
:: Installs ayama-agent.exe and optionally registers it as a Windows service.
::
:: Usage:
::   install.bat               — Install to %ProgramFiles%\Ayama, no service
::   install.bat --service     — Install + register as auto-start service
::   install.bat --user        — Install to %LOCALAPPDATA%\Ayama (no admin needed)
::
setlocal EnableDelayedExpansion

:: ── Parse arguments ────────────────────────────────────────────────────────
set INSTALL_MODE=normal
set INSTALL_DIR=%ProgramFiles%\Ayama
for %%A in (%*) do (
    if /I "%%A"=="--service" set INSTALL_MODE=service
    if /I "%%A"=="--user"    (
        set INSTALL_MODE=user
        set INSTALL_DIR=%LOCALAPPDATA%\Ayama
    )
)

:: ── Check admin (skip for --user) ─────────────────────────────────────────
if not "%INSTALL_MODE%"=="user" (
    net session >nul 2>&1
    if %errorlevel% NEQ 0 (
        echo [Ayama Installer] ERROR: Administrator rights required.
        echo Run this script as Administrator, or use --user for per-user install.
        pause
        exit /b 1
    )
)

echo [Ayama Installer] Installing Ayama to: %INSTALL_DIR%

:: ── Source directory: same directory as this script ───────────────────────
set SCRIPT_DIR=%~dp0
:: Default: the project's dist layout (developer install). This script lives at
:: tools\installer\, so the project root is 2 levels up. The pre-separation
:: tiers (..\..\..\..\build\ayama\ / build_ayama\, the retired container root
:: build) were removed 2026-07-16 — they resolved OUTSIDE the project and could
:: have silently installed foreign binaries if a stale container build existed.
set SOURCE_AGENT=%SCRIPT_DIR%..\..\build\ayama-dist\runtime\ayama-agent.exe
set SOURCE_UI=%SCRIPT_DIR%..\..\build\ayama-dist\ayama-ui.exe
set SOURCE_CLI=%SCRIPT_DIR%..\..\build\ayama-dist\runtime\ayama-cli.exe
set SOURCE_BENCH=%SCRIPT_DIR%..\..\build\ayama-dist\runtime\ayama-bench.exe
set SOURCE_SVC=%SCRIPT_DIR%..\..\build\ayama-dist\runtime\ayama-service-register.exe

:: Release-build tier (build-release\, produced by build-release.bat).
if not exist "%SOURCE_AGENT%" (
    set SOURCE_AGENT=%SCRIPT_DIR%..\..\build-release\ayama-dist\runtime\ayama-agent.exe
    set SOURCE_UI=%SCRIPT_DIR%..\..\build-release\ayama-dist\ayama-ui.exe
    set SOURCE_CLI=%SCRIPT_DIR%..\..\build-release\ayama-dist\runtime\ayama-cli.exe
    set SOURCE_BENCH=%SCRIPT_DIR%..\..\build-release\ayama-dist\runtime\ayama-bench.exe
    set SOURCE_SVC=%SCRIPT_DIR%..\..\build-release\ayama-dist\runtime\ayama-service-register.exe
)

:: Last fallback: same directory as this script (deployed installer ZIP).
if not exist "%SOURCE_AGENT%" (
    set SOURCE_AGENT=%SCRIPT_DIR%ayama-agent.exe
    set SOURCE_UI=%SCRIPT_DIR%ayama-ui.exe
    set SOURCE_CLI=%SCRIPT_DIR%ayama-cli.exe
    set SOURCE_BENCH=%SCRIPT_DIR%ayama-bench.exe
    set SOURCE_SVC=%SCRIPT_DIR%ayama-service-register.exe
)

:: ── Create install directory ───────────────────────────────────────────────
if not exist "%INSTALL_DIR%" (
    mkdir "%INSTALL_DIR%"
    if %errorlevel% NEQ 0 (
        echo [Ayama Installer] ERROR: Could not create directory: %INSTALL_DIR%
        exit /b 1
    )
)

:: ── Copy executables ───────────────────────────────────────────────────────
if exist "%SOURCE_AGENT%" (
    copy /Y "%SOURCE_AGENT%" "%INSTALL_DIR%\ayama-agent.exe"
    echo [Ayama Installer] Installed: ayama-agent.exe
) else (
    echo [Ayama Installer] WARNING: ayama-agent.exe not found at %SOURCE_AGENT%
)

if exist "%SOURCE_UI%" (
    copy /Y "%SOURCE_UI%" "%INSTALL_DIR%\ayama-ui.exe"
    echo [Ayama Installer] Installed: ayama-ui.exe
)

if exist "%SOURCE_CLI%" (
    copy /Y "%SOURCE_CLI%" "%INSTALL_DIR%\ayama-cli.exe"
    echo [Ayama Installer] Installed: ayama-cli.exe
)

if exist "%SOURCE_BENCH%" (
    copy /Y "%SOURCE_BENCH%" "%INSTALL_DIR%\ayama-bench.exe"
    echo [Ayama Installer] Installed: ayama-bench.exe
)

if exist "%SOURCE_SVC%" (
    copy /Y "%SOURCE_SVC%" "%INSTALL_DIR%\ayama-service-register.exe"
    echo [Ayama Installer] Installed: ayama-service-register.exe
)

:: ── Add to PATH (user-level) ───────────────────────────────────────────────
setx PATH "%PATH%;%INSTALL_DIR%" /M >nul 2>&1 || setx PATH "%PATH%;%INSTALL_DIR%" >nul 2>&1
echo [Ayama Installer] Added to PATH: %INSTALL_DIR%

:: ── Create config directory ────────────────────────────────────────────────
set CONFIG_DIR=%LOCALAPPDATA%\Ayama
if not exist "%CONFIG_DIR%" (
    mkdir "%CONFIG_DIR%"
    echo [Ayama Installer] Created config dir: %CONFIG_DIR%
)

:: ── Register Windows service (--service mode only) ────────────────────────
:: Prefiere ayama-service-register.exe (lógica completa: failure actions,
:: description, AutoStart). Fallback a sc.exe si el binario no está.
if "%INSTALL_MODE%"=="service" (
    echo [Ayama Installer] Registering Windows service "AyamaAgent"...

    if exist "%INSTALL_DIR%\ayama-service-register.exe" (
        :: Stop+delete any pre-existing service.
        "%INSTALL_DIR%\ayama-service-register.exe" uninstall >nul 2>&1
        "%INSTALL_DIR%\ayama-service-register.exe" install --path "%INSTALL_DIR%\ayama-agent.exe"
        if %errorlevel% EQU 0 (
            "%INSTALL_DIR%\ayama-service-register.exe" start >nul 2>&1
            echo [Ayama Installer] Service registered and started: AyamaAgent
        ) else (
            echo [Ayama Installer] WARNING: Service registration failed.
        )
    ) else (
        :: Fallback path con sc.exe (sin failure actions ni description).
        sc query AyamaAgent >nul 2>&1
        if %errorlevel% EQU 0 (
            sc stop AyamaAgent >nul 2>&1
            sc delete AyamaAgent >nul 2>&1
        )
        sc create AyamaAgent ^
            binPath= "\"%INSTALL_DIR%\ayama-agent.exe\"" ^
            DisplayName= "Ayama Runtime Optimizer" ^
            start= auto ^
            type= own
        if %errorlevel% EQU 0 (
            sc start AyamaAgent
            echo [Ayama Installer] Service registered (via sc.exe fallback).
        ) else (
            echo [Ayama Installer] WARNING: Service registration failed.
            echo [Ayama Installer] Run ayama-agent.exe manually as Administrator.
        )
    )
    echo [Ayama Installer] View logs: Event Viewer ^> Windows Logs ^> Application
)

:: ── Create Desktop shortcut for ayama-ui.exe ──────────────────────────────
if exist "%INSTALL_DIR%\ayama-ui.exe" (
    set SHORTCUT=%USERPROFILE%\Desktop\Ayama.lnk
    powershell -Command ^
        "$ws = New-Object -ComObject WScript.Shell; ^
         $s = $ws.CreateShortcut('%SHORTCUT%'); ^
         $s.TargetPath = '%INSTALL_DIR%\ayama-ui.exe'; ^
         $s.WorkingDirectory = '%INSTALL_DIR%'; ^
         $s.Description = 'Ayama Runtime Optimizer'; ^
         $s.Save()" >nul 2>&1
    if exist "%SHORTCUT%" echo [Ayama Installer] Desktop shortcut created: Ayama.lnk
)

echo.
echo [Ayama Installer] Installation complete!
echo   Executable : %INSTALL_DIR%\ayama-agent.exe
echo   UI         : %INSTALL_DIR%\ayama-ui.exe
echo   Config dir : %CONFIG_DIR%
if "%INSTALL_MODE%"=="service" (
    echo   Service    : AyamaAgent (auto-start)
) else (
    echo   To auto-start, re-run with: install.bat --service
    echo   Or add to Task Scheduler: %INSTALL_DIR%\ayama-agent.exe
)
echo.
pause
endlocal
REM Made with my soul - Swately <3
