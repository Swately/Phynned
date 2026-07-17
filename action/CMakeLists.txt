# phynned/action/CMakeLists.txt
# phynned_action — ActionExecutor, ActionLog.
#
# Dependencies: phyriad_schema, phyriad_hal, phyriad_topology (FR-3 set_process_affinity
#               + FR-9 set_process_priority), phyriad_ipc (Ring.hpp), phynned_policy.
# Note: phyriad_ipc is INTERFACE (header-only), required by ActionLog.hpp
#       which uses phyriad::ipc::Ring<ActionLogEntry, 128> (FR-12 migration).
# Note: phyriad_daemon was removed — replaced by phyriad::hw::set_process_affinity
# and phyriad::hw::set_process_priority (both in phyriad_topology).

add_library(phynned_action STATIC
    src/ActionExecutor.cpp
)

target_include_directories(phynned_action PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(phynned_action PUBLIC
    phyriad_schema
    phyriad_hal
    phyriad_topology  # FR-3 set_process_affinity, FR-9 set_process_priority
    phyriad_ipc       # ActionLog.hpp: phyriad::ipc::Ring<ActionLogEntry, 128>
    phynned_policy
)

if(WIN32)
    target_link_libraries(phynned_action PUBLIC kernel32)
endif()

target_compile_features(phynned_action PUBLIC cxx_std_23)

if(MSVC)
    target_compile_options(phynned_action PRIVATE /W4)
else()
    target_compile_options(phynned_action PRIVATE -Wall -Wextra -Wpedantic)
endif()

# ── Tests ─────────────────────────────────────────────────────────────────────
if(PHYNNED_BUILD_TESTS)
    add_executable(phynned_action_test tests/action_executor_test.cpp)
    target_link_libraries(phynned_action_test PRIVATE phynned_action)
    target_compile_features(phynned_action_test PRIVATE cxx_std_23)
    if(NOT MSVC)
        target_compile_options(phynned_action_test PRIVATE -Wall -Wextra)
    endif()
    enable_testing()
    add_test(NAME phynned_action_test COMMAND phynned_action_test)
endif()
# Made with my soul - Swately <3
