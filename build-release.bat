@echo off
:: build-release.bat — distributable release build of Phynned (LTO + stripped).
:: Output: build-release\phynned-dist\phynned-ui.exe + runtime\*.exe
:: For the full release zip (staging + docs + install.ps1) use
:: scripts\package_release.ps1 -Version X.Y.Z -Strip -LTO instead.
setlocal
pushd "%~dp0"

cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release -DPHYNNED_BUILD_TESTS=OFF ^
      -DCMAKE_INTERPROCEDURAL_OPTIMIZATION=ON -DCMAKE_EXE_LINKER_FLAGS=-s
if %ERRORLEVEL% neq 0 ( popd & exit /b 1 )

cmake --build build-release --parallel
if %ERRORLEVEL% neq 0 ( popd & exit /b 2 )

echo.
echo [OK] build-release\phynned-dist\phynned-ui.exe
popd
endlocal
REM Made with my soul - Swately <3
