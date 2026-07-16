# ayama/action/CMakeLists.txt
# ayama_action — ActionExecutor, ActionLog.
#
# Dependencies: phyriad_schema, phyriad_hal, phyriad_topology (FR-3 set_process_affinity
#               + FR-9 set_process_priority), phyriad_ipc (Ring.hpp), ayama_policy.
# Note: phyriad_ipc is INTERFACE (header-only), required by ActionLog.hpp
#       which uses phyriad::ipc::Ring<ActionLogEntry, 128> (FR-12 migration).
# Note: phyriad_daemon was removed — replaced by phyriad::hw::set_process_affinity
# and phyriad::hw::set_process_priority (both in phyriad_topology).

add_library(ayama_action STATIC
    src/ActionExecutor.cpp
)

target_include_directories(ayama_action PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(ayama_action PUBLIC
    phyriad_schema
    phyriad_hal
    phyriad_topology  # FR-3 set_process_affinity, FR-9 set_process_priority
    phyriad_ipc       # ActionLog.hpp: phyriad::ipc::Ring<ActionLogEntry, 128>
    ayama_policy
)

if(WIN32)
    target_link_libraries(ayama_action PUBLIC kernel32)
endif()

target_compile_features(ayama_action PUBLIC cxx_std_23)

if(MSVC)
    target_compile_options(ayama_action PRIVATE /W4)
else()
    target_compile_options(ayama_action PRIVATE -Wall -Wextra -Wpedantic)
endif()

# ── Tests ─────────────────────────────────────────────────────────────────────
if(AYAMA_BUILD_TESTS)
    add_executable(ayama_action_test tests/action_executor_test.cpp)
    target_link_libraries(ayama_action_test PRIVATE ayama_action)
    target_compile_features(ayama_action_test PRIVATE cxx_std_23)
    if(NOT MSVC)
        target_compile_options(ayama_action_test PRIVATE -Wall -Wextra)
    endif()
    enable_testing()
    add_test(NAME ayama_action_test COMMAND ayama_action_test)
endif()
# Made with my soul - Swately <3
