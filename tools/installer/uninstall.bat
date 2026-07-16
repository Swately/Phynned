@echo off
:: Ayama Uninstaller — uninstall.bat
:: Removes ayama-agent.exe, service registration, and optionally config.
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
    echo [Ayama Uninstaller] Administrator rights required.
    pause
    exit /b 1
)

set INSTALL_DIR=%ProgramFiles%\Ayama
set CONFIG_DIR=%LOCALAPPDATA%\Ayama

echo [Ayama Uninstaller] Removing Ayama...

:: ── Stop and remove service ────────────────────────────────────────────────
:: Prefiere ayama-service-register.exe si está disponible (cleanup completo).
if exist "%INSTALL_DIR%\ayama-service-register.exe" (
    "%INSTALL_DIR%\ayama-service-register.exe" uninstall >nul 2>&1
) else (
    sc query AyamaAgent >nul 2>&1
    if %errorlevel% EQU 0 (
        echo [Ayama Uninstaller] Stopping service AyamaAgent...
        sc stop AyamaAgent >nul 2>&1
        timeout /t 2 /nobreak >nul
        sc delete AyamaAgent >nul 2>&1
        echo [Ayama Uninstaller] Service removed.
    )
)

:: ── Terminate any running processes ────────────────────────────────────────
taskkill /F /IM ayama-agent.exe >nul 2>&1
taskkill /F /IM ayama-ui.exe    >nul 2>&1
taskkill /F /IM ayama-cli.exe   >nul 2>&1
taskkill /F /IM ayama-bench.exe >nul 2>&1

:: ── Remove desktop shortcut ────────────────────────────────────────────────
if exist "%USERPROFILE%\Desktop\Ayama.lnk" (
    del /F /Q "%USERPROFILE%\Desktop\Ayama.lnk"
    echo [Ayama Uninstaller] Desktop shortcut removed.
)

:: ── Remove install directory ───────────────────────────────────────────────
if exist "%INSTALL_DIR%" (
    rmdir /S /Q "%INSTALL_DIR%"
    echo [Ayama Uninstaller] Removed: %INSTALL_DIR%
)

:: ── Remove config (only with --purge) ─────────────────────────────────────
if "%PURGE%"=="1" (
    if exist "%CONFIG_DIR%" (
        rmdir /S /Q "%CONFIG_DIR%"
        echo [Ayama Uninstaller] Purged config: %CONFIG_DIR%
    )
) else (
    echo [Ayama Uninstaller] Config preserved at: %CONFIG_DIR%
    echo [Ayama Uninstaller] Use --purge to also remove memory.toml and policies.toml
)

:: ── Remove from PATH ───────────────────────────────────────────────────────
:: (PATH cleanup is best done via PowerShell to handle the registry properly)
powershell -Command ^
    "$path = [Environment]::GetEnvironmentVariable('PATH', 'Machine'); ^
     $parts = $path -split ';' ^| Where-Object { $_ -notlike '*\Ayama*' }; ^
     [Environment]::SetEnvironmentVariable('PATH', ($parts -join ';'), 'Machine')" ^
    >nul 2>&1

echo.
echo [Ayama Uninstaller] Uninstall complete.
if "%PURGE%"=="0" (
    echo   Per-game memory and config preserved at:
    echo   %CONFIG_DIR%
)
echo.
pause
endlocal
REM Made with my soul - Swately <3
