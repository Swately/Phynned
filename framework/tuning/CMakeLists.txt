# catalog/cpp/tuning/CMakeLists.txt
# phyriad_tuning — OS-level performance tuning pillar
#
# abstract interface + LinuxTuner full sysfs impl (THP/governor/c-states/IRQ)
# + WindowsTuner full impl (power scheme/timer/working set/priority boost).
cmake_minimum_required(VERSION 3.20)
project(phyriad_tuning LANGUAGES CXX)

# ── Standalone dependency resolution ─────────────────────────────────────────
if(NOT TARGET phyriad_schema)
    add_subdirectory(
        ${CMAKE_CURRENT_SOURCE_DIR}/../schema
        ${CMAKE_CURRENT_BINARY_DIR}/phyriad_schema
    )
endif()
# phyriad_hal needed for TuningSnapshot HAL discipline (hal::ctrl_* + kDestructivePad).
if(NOT TARGET phyriad_hal)
    add_subdirectory(
        ${CMAKE_CURRENT_SOURCE_DIR}/../hal
        ${CMAKE_CURRENT_BINARY_DIR}/phyriad_hal
    )
endif()

# ── Library ───────────────────────────────────────────────────────────────────
# Platform-specific sources: LinuxTuner.cpp is Linux-only (entire file guarded
# by #ifndef _WIN32), WindowsTuner.cpp is Windows-only (#ifdef _WIN32). Both
# are listed unconditionally — the build is safe because the entire TU compiles
# to an empty object on the non-matching platform.
add_library(phyriad_tuning STATIC
    src/PrivilegeCheck.cpp
    src/WorkingSet.cpp
    src/TuningSnapshot.cpp
    src/LinuxTuner.cpp
    src/WindowsTuner.cpp
    src/PlatformTunerFactory.cpp
)

target_include_directories(phyriad_tuning PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_features(phyriad_tuning PUBLIC cxx_std_23)

if(MSVC)
    target_compile_options(phyriad_tuning PRIVATE /W4 /WX /permissive-)
else()
    target_compile_options(phyriad_tuning PRIVATE -Wall -Wextra -Werror -std=c++23)
endif()

target_link_libraries(phyriad_tuning PUBLIC phyriad_schema phyriad_hal)

# Platform link dependencies
if(WIN32)
    # winmm for timeBeginPeriod/timeEndPeriod, powrprof for PowerGetActiveScheme,
    # psapi for working-set queries.
    target_link_libraries(phyriad_tuning PUBLIC kernel32 winmm powrprof psapi)
endif()

if(UNIX AND NOT APPLE)
    find_package(Threads REQUIRED)
    target_link_libraries(phyriad_tuning PUBLIC Threads::Threads)
endif()

# ── Tests ─────────────────────────────────────────────────────────────────────
if(BUILD_TESTING OR PHYRIAD_BUILD_TESTS)
    include(CTest)
    enable_testing()
    # Standalone fallback: the root CMakeLists defines this helper for root builds.
    if(NOT COMMAND phyriad_test_dll_env)
        if(WIN32 AND CMAKE_CXX_COMPILER_ID MATCHES "GNU")
            get_filename_component(_phyriad_mingw_bin "${CMAKE_CXX_COMPILER}" DIRECTORY)
            function(phyriad_test_dll_env _target)
                set_tests_properties(${_target} PROPERTIES
                    ENVIRONMENT "PATH=${_phyriad_mingw_bin};$ENV{PATH}")
            endfunction()
        else()
            function(phyriad_test_dll_env _target)
            endfunction()
        endif()
    endif()
    add_executable(tuning_test
        tests/tuning_test.cpp
    )

    target_link_libraries(tuning_test PRIVATE phyriad_tuning)

    target_compile_features(tuning_test PRIVATE cxx_std_23)

    if(MSVC)
        target_compile_options(tuning_test PRIVATE /W4 /WX /permissive-)
    else()
        target_compile_options(tuning_test PRIVATE -Wall -Wextra -Werror -std=c++23)
    endif()

    if(UNIX AND NOT APPLE)
        target_link_libraries(tuning_test PRIVATE pthread)
    endif()

    include(CTest)
    add_test(NAME tuning_test COMMAND tuning_test)
    phyriad_test_dll_env(tuning_test)

    # ── FR-19: WorkingSet test ────────────────────────────────────────────────
    add_executable(working_set_test
        tests/working_set_test.cpp
    )
    target_link_libraries(working_set_test PRIVATE phyriad_tuning)
    target_compile_features(working_set_test PRIVATE cxx_std_23)
    if(MSVC)
        target_compile_options(working_set_test PRIVATE /W4 /WX /permissive-)
    else()
        target_compile_options(working_set_test PRIVATE -Wall -Wextra -Werror -std=c++23)
    endif()
    add_test(NAME working_set_test COMMAND working_set_test)
    phyriad_test_dll_env(working_set_test)

    # ── Phase 4: TuningSnapshot restored-feature test ──────────────────────
    add_executable(tuning_snapshot_test tests/tuning_snapshot_test.cpp)
    target_link_libraries(tuning_snapshot_test PRIVATE phyriad_tuning)
    target_compile_features(tuning_snapshot_test PRIVATE cxx_std_23)
    if(MSVC)
        target_compile_options(tuning_snapshot_test PRIVATE /W4 /WX /permissive-)
    else()
        target_compile_options(tuning_snapshot_test PRIVATE -Wall -Wextra -Werror -std=c++23)
    endif()
    add_test(NAME tuning_snapshot_test COMMAND tuning_snapshot_test)
    phyriad_test_dll_env(tuning_snapshot_test)
endif()
# Made with my soul - Swately <3
