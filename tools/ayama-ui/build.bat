@echo off
:: ============================================================================
:: tools/ayama-ui/build.bat
:: Fallback MinGW-w64 direct compile for ayama-ui.exe.
::
:: Preferred build — CMake from the project root:
::
::   cmake -S . -B build
::   cmake --build build --target ayama-ui
::
:: This script is a fallback for rapid recompilation when a CMake build
:: already exists (GLFW/ImGui pre-compiled by CMake as FetchContent deps).
:: It does NOT run CMake or re-link dependencies — it only recompiles and
:: re-links the ayama-ui sources against the prebuilt artifacts.
::
:: Usage (from project root):
::   tools\ayama-ui\build.bat
::
:: Output: tools\ayama-ui\ayama.exe
:: ============================================================================
setlocal

:: Derive the project root from this script's location (2 directories up).
pushd "%~dp0..\.."
set ROOT=%CD%
popd

set FRAMEWORK=%ROOT%\framework
set AYAMA=%ROOT%
set BUILD=%ROOT%\build

:: ── Verify that a CMake build exists ─────────────────────────────────────────
if not exist "%BUILD%" (
    echo.
    echo [ERROR] build\ not found.
    echo         Run the CMake build first:
    echo           cmake -S "%ROOT%" -B "%BUILD%" -G "MinGW Makefiles"
    echo           cmake --build "%BUILD%"
    exit /b 1
)

set FLAGS=-std=c++23 -O2 -Wall -Wextra -DWIN32_LEAN_AND_MEAN -DNOMINMAX

:: ── Includes ──────────────────────────────────────────────────────────────────
set INC=^
  -I%AYAMA%\tools\ayama-ui ^
  -I%FRAMEWORK%\hal\include ^
  -I%FRAMEWORK%\schema\include ^
  -I%FRAMEWORK%\transport\include ^
  -I%FRAMEWORK%\topology\include ^
  -I%FRAMEWORK%\scheduler\include ^
  -I%FRAMEWORK%\node\include ^
  -I%FRAMEWORK%\graph\include ^
  -I%FRAMEWORK%\runtime\include ^
  -I%FRAMEWORK%\render\include ^
  -I%FRAMEWORK%\ui\include ^
  -I%FRAMEWORK%\_meta\include ^
  -I%AYAMA%\core\include ^
  -I%AYAMA%\observer\include ^
  -I%AYAMA%\policy\include ^
  -I%AYAMA%\action\include ^
  -I%AYAMA%\ipc\include ^
  -I%BUILD%\_deps\imgui-src ^
  -I%BUILD%\_deps\imgui-src\backends ^
  -I%BUILD%\_deps\glfw-src\include

:: ── Sources ────────────────────────────────────────────────────────────────────
set AYAMA_UI_SRCS=^
  %AYAMA%\tools\ayama-ui\main.cpp ^
  %AYAMA%\ipc\src\AyamaClient.cpp ^
  %AYAMA%\action\src\ActionExecutor.cpp ^
  %AYAMA%\observer\src\ProcessObserver.cpp ^
  %AYAMA%\observer\src\ProcessObserver_win32.cpp ^
  %AYAMA%\core\src\AgentRuntime.cpp

:: ImGui sources (from CMake FetchContent build)
set IMGUI_SRCS=^
  %BUILD%\_deps\imgui-src\imgui.cpp ^
  %BUILD%\_deps\imgui-src\imgui_draw.cpp ^
  %BUILD%\_deps\imgui-src\imgui_tables.cpp ^
  %BUILD%\_deps\imgui-src\imgui_widgets.cpp ^
  %BUILD%\_deps\imgui-src\imgui_demo.cpp ^
  %BUILD%\_deps\imgui-src\backends\imgui_impl_glfw.cpp ^
  %BUILD%\_deps\imgui-src\backends\imgui_impl_opengl3.cpp

:: Phyriad framework sources needed by ayama-ui
set FRAMEWORK_SRCS=^
  %FRAMEWORK%\transport\src\SlotCopy.cpp ^
  %FRAMEWORK%\runtime\src\GraphRuntime.cpp

:: ── Libraries ──────────────────────────────────────────────────────────────────
set GLFW_LIB=%BUILD%\_deps\glfw-build\src\libglfw3.a

set LIBS=^
  %GLFW_LIB% ^
  -lopengl32 -lgdi32 ^
  -lkernel32 -luser32 -ladvapi32 ^
  -lpsapi

set OUT=%AYAMA%\tools\ayama-ui\ayama.exe

echo.
echo Compiling ayama-ui (ayama.exe)...
echo.

g++ %FLAGS% %INC% ^
    %AYAMA_UI_SRCS% ^
    %IMGUI_SRCS% ^
    %FRAMEWORK_SRCS% ^
    -o %OUT% ^
    %LIBS%

if %ERRORLEVEL% neq 0 (
    echo.
    echo [ERROR] Compilation failed.
    echo         For a full build use CMake:
    echo           cmake -S "%ROOT%" -B "%BUILD%" -G "MinGW Makefiles" -DPHYRIAD_BUILD_AYAMA=ON
    echo           cmake --build "%BUILD%" --target ayama-ui
    exit /b 1
)

echo.
echo [OK] Compilation successful.
echo      Executable: %OUT%
echo.
echo Usage:
echo   %OUT%
echo.
echo Note: requires ayama-agent.exe running (as Administrator) to display live data.
echo.

endlocal
REM Made with my soul - Swately <3
