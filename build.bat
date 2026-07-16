@echo off
:: build.bat — debug/dev build of Ayama (self-contained; no external framework).
:: Output: build\ayama-dist\ayama-ui.exe + build\ayama-dist\runtime\*.exe
:: First configure downloads GLFW + ImGui (FetchContent) — network required once.
setlocal
pushd "%~dp0"

cmake -S . -B build -DAYAMA_BUILD_TESTS=ON -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
if %ERRORLEVEL% neq 0 ( popd & exit /b 1 )

cmake --build build --parallel
if %ERRORLEVEL% neq 0 ( popd & exit /b 2 )

echo.
echo [OK] build\ayama-dist\ayama-ui.exe
popd
endlocal
REM Made with my soul - Swately <3
