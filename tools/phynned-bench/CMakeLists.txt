# phynned/tools/phynned-bench/CMakeLists.txt
# phynned-bench — standalone CPU-bound synthetic benchmark.
#
# Simulates a game-like workload (matrix work + worker bursts).
# Used to validate Phynned optimizations without requiring real games.
# Output: CSV file with frame timing metrics for comparison.
#
# Usage: phynned-bench --duration 60000 --output baseline.csv
#
# §Phynned Master Plan §5.4

add_executable(phynned-bench main.cpp)

target_compile_features(phynned-bench PRIVATE cxx_std_23)

# phyriad_hal is INTERFACE-only (no .a/.lib) — just provides the
# include path for <phyriad/hal/MemoryOrder.hpp>. Required so the
# atomic ops in workload.hpp can use the hal::* wrappers and satisfy
# the project-wide HAL discipline (scripts/lint_hal.sh).
target_link_libraries(phynned-bench PRIVATE phyriad_hal)

if(MSVC)
    target_compile_options(phynned-bench PRIVATE /W4 /O2)
    set_target_properties(phynned-bench PROPERTIES
        LINK_FLAGS "/SUBSYSTEM:CONSOLE")
else()
    target_compile_options(phynned-bench PRIVATE -Wall -Wextra -O2)
endif()

# No Phynned library deps — phynned-bench is intentionally standalone
# so it can run without the agent present.

if(CMAKE_INSTALL_PREFIX)
    install(TARGETS phynned-bench RUNTIME DESTINATION bin)
endif()
# Made with my soul - Swately <3
