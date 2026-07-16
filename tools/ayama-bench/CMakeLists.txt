# ayama/tools/ayama-bench/CMakeLists.txt
# ayama-bench — standalone CPU-bound synthetic benchmark.
#
# Simulates a game-like workload (matrix work + worker bursts).
# Used to validate Ayama optimizations without requiring real games.
# Output: CSV file with frame timing metrics for comparison.
#
# Usage: ayama-bench --duration 60000 --output baseline.csv
#
# §Ayama Master Plan §5.4

add_executable(ayama-bench main.cpp)

target_compile_features(ayama-bench PRIVATE cxx_std_23)

# phyriad_hal is INTERFACE-only (no .a/.lib) — just provides the
# include path for <phyriad/hal/MemoryOrder.hpp>. Required so the
# atomic ops in workload.hpp can use the hal::* wrappers and satisfy
# the project-wide HAL discipline (scripts/lint_hal.sh).
target_link_libraries(ayama-bench PRIVATE phyriad_hal)

if(MSVC)
    target_compile_options(ayama-bench PRIVATE /W4 /O2)
    set_target_properties(ayama-bench PROPERTIES
        LINK_FLAGS "/SUBSYSTEM:CONSOLE")
else()
    target_compile_options(ayama-bench PRIVATE -Wall -Wextra -O2)
endif()

# No Ayama library deps — ayama-bench is intentionally standalone
# so it can run without the agent present.

if(CMAKE_INSTALL_PREFIX)
    install(TARGETS ayama-bench RUNTIME DESTINATION bin)
endif()
# Made with my soul - Swately <3
