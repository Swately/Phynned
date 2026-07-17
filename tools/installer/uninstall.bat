@echo off
:: Phynned Uninstaller — uninstall.bat
:: Removes phynned-agent.exe, service registration, and optionally config.
::
:: Usage:
::   uninstall.bat              — Remove files + service; keep config
::   uninstall.bat --purge      — Remove everything including config/memory
::
setlocal EnableDelayedExpansion

set PURGE=0
for %%A in (%*) do (
    if /I "%%A"=="--purge" set PURGE=1
)

:: ── Check admin ────────────────────────────────────────────────────────────
net session >nul 2>&1
if %errorlevel% NEQ 0 (
    echo [Phynned Uninstaller] Administrator rights required.
    pause
    exit /b 1
)

set INSTALL_DIR=%ProgramFiles%\Phynned
set CONFIG_DIR=%LOCALAPPDATA%\Phynned

echo [Phynned Uninstaller] Removing Phynned...

:: ── Stop and remove service ────────────────────────────────────────────────
:: Prefiere phynned-service-register.exe si está disponible (cleanup completo).
if exist "%INSTALL_DIR%\phynned-service-register.exe" (
    "%INSTALL_DIR%\phynned-service-register.exe" uninstall >nul 2>&1
) else (
    sc query PhynnedAgent >nul 2>&1
    if %errorlevel% EQU 0 (
        echo [Phynned Uninstaller] Stopping service PhynnedAgent...
        sc stop PhynnedAgent >nul 2>&1
        timeout /t 2 /nobreak >nul
        sc delete PhynnedAgent >nul 2>&1
        echo [Phynned Uninstaller] Service removed.
    )
)

:: ── Terminate any running processes ────────────────────────────────────────
taskkill /F /IM phynned-agent.exe >nul 2>&1
taskkill /F /IM phynned-ui.exe    >nul 2>&1
taskkill /F /IM phynned-cli.exe   >nul 2>&1
taskkill /F /IM phynned-bench.exe >nul 2>&1

:: ── Remove desktop shortcut ────────────────────────────────────────────────
if exist "%USERPROFILE%\Desktop\Phynned.lnk" (
    del /F /Q "%USERPROFILE%\Desktop\Phynned.lnk"
    echo [Phynned Uninstaller] Desktop shortcut removed.
)

:: ── Remove install directory ───────────────────────────────────────────────
if exist "%INSTALL_DIR%" (
    rmdir /S /Q "%INSTALL_DIR%"
    echo [Phynned Uninstaller] Removed: %INSTALL_DIR%
)

:: ── Remove config (only with --purge) ─────────────────────────────────────
if "%PURGE%"=="1" (
    if exist "%CONFIG_DIR%" (
        rmdir /S /Q "%CONFIG_DIR%"
        echo [Phynned Uninstaller] Purged config: %CONFIG_DIR%
    )
) else (
    echo [Phynned Uninstaller] Config preserved at: %CONFIG_DIR%
    echo [Phynned Uninstaller] Use --purge to also remove memory.toml and policies.toml
)

:: ── Remove from PATH ───────────────────────────────────────────────────────
:: (PATH cleanup is best done via PowerShell to handle the registry properly)
powershell -Command ^
    "$path = [Environment]::GetEnvironmentVariable('PATH', 'Machine'); ^
     $parts = $path -split ';' ^| Where-Object { $_ -notlike '*\Phynned*' }; ^
     [Environment]::SetEnvironmentVariable('PATH', ($parts -join ';'), 'Machine')" ^
    >nul 2>&1

echo.
echo [Phynned Uninstaller] Uninstall complete.
if "%PURGE%"=="0" (
    echo   Per-game memory and config preserved at:
    echo   %CONFIG_DIR%
)
echo.
pause
endlocal
REM Made with my soul - Swately <3
