@echo off
:: ============================================================================
:: tools/phynned-ui/build.bat
:: Fallback MinGW-w64 direct compile for phynned-ui.exe.
::
:: Preferred build — CMake from the project root:
::
::   cmake -S . -B build
::   cmake --build build --target phynned-ui
::
:: This script is a fallback for rapid recompilation when a CMake build
:: already exists (GLFW/ImGui pre-compiled by CMake as FetchContent deps).
:: It does NOT run CMake or re-link dependencies — it only recompiles and
:: re-links the phynned-ui sources against the prebuilt artifacts.
::
:: Usage (from project root):
::   tools\phynned-ui\build.bat
::
:: Output: tools\phynned-ui\phynned.exe
:: ============================================================================
setlocal

:: Derive the project root from this script's location (2 directories up).
pushd "%~dp0..\.."
set ROOT=%CD%
popd

set FRAMEWORK=%ROOT%\framework
set PHYNNED=%ROOT%
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
  -I%PHYNNED%\tools\phynned-ui ^
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
  -I%PHYNNED%\core\include ^
  -I%PHYNNED%\observer\include ^
  -I%PHYNNED%\policy\include ^
  -I%PHYNNED%\action\include ^
  -I%PHYNNED%\ipc\include ^
  -I%BUILD%\_deps\imgui-src ^
  -I%BUILD%\_deps\imgui-src\backends ^
  -I%BUILD%\_deps\glfw-src\include

:: ── Sources ────────────────────────────────────────────────────────────────────
set PHYNNED_UI_SRCS=^
  %PHYNNED%\tools\phynned-ui\main.cpp ^
  %PHYNNED%\ipc\src\PhynnedClient.cpp ^
  %PHYNNED%\action\src\ActionExecutor.cpp ^
  %PHYNNED%\observer\src\ProcessObserver.cpp ^
  %PHYNNED%\observer\src\ProcessObserver_win32.cpp ^
  %PHYNNED%\core\src\AgentRuntime.cpp

:: ImGui sources (from CMake FetchContent build)
set IMGUI_SRCS=^
  %BUILD%\_deps\imgui-src\imgui.cpp ^
  %BUILD%\_deps\imgui-src\imgui_draw.cpp ^
  %BUILD%\_deps\imgui-src\imgui_tables.cpp ^
  %BUILD%\_deps\imgui-src\imgui_widgets.cpp ^
  %BUILD%\_deps\imgui-src\imgui_demo.cpp ^
  %BUILD%\_deps\imgui-src\backends\imgui_impl_glfw.cpp ^
  %BUILD%\_deps\imgui-src\backends\imgui_impl_opengl3.cpp

:: Phyriad framework sources needed by phynned-ui
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

set OUT=%PHYNNED%\tools\phynned-ui\phynned.exe

echo.
echo Compiling phynned-ui (phynned.exe)...
echo.

g++ %FLAGS% %INC% ^
    %PHYNNED_UI_SRCS% ^
    %IMGUI_SRCS% ^
    %FRAMEWORK_SRCS% ^
    -o %OUT% ^
    %LIBS%

if %ERRORLEVEL% neq 0 (
    echo.
    echo [ERROR] Compilation failed.
    echo         For a full build use CMake:
    echo           cmake -S "%ROOT%" -B "%BUILD%" -G "MinGW Makefiles" -DPHYRIAD_BUILD_PHYNNED=ON
    echo           cmake --build "%BUILD%" --target phynned-ui
    exit /b 1
)

echo.
echo [OK] Compilation successful.
echo      Executable: %OUT%
echo.
echo Usage:
echo   %OUT%
echo.
echo Note: requires phynned-agent.exe running (as Administrator) to display live data.
echo.

endlocal
REM Made with my soul - Swately <3
