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
    src/AcProbe.cpp        # CR1 — least-privilege anti-cheat probe (M0-safety)
    src/RevertJournal.cpp  # DR1 — crash-durable write-ahead revert journal
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
    phynned_observer  # CR1 AcProbe: observer::AcDriverOracle (zero-handle gate)
    phynned_learn     # CR1 AcProbe: learn::PerGameMemory (the ALLOWED/BLOCKED label)
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

    # DR1 — write-ahead revert journal (durability + recover + pid-recycle guard).
    add_executable(revert_journal_test tests/revert_journal_test.cpp)
    target_link_libraries(revert_journal_test PRIVATE phynned_action)
    target_compile_features(revert_journal_test PRIVATE cxx_std_23)
    if(NOT MSVC)
        target_compile_options(revert_journal_test PRIVATE -Wall -Wextra)
    endif()
    add_test(NAME revert_journal_test COMMAND revert_journal_test)

    # Use-modes §5 test 3 (R3) — selective revert by rule_id: a mixed active set
    # (corral rule 20 + game rule 1) reverts ONLY the corral action.
    add_executable(selective_revert_test tests/selective_revert_test.cpp)
    target_link_libraries(selective_revert_test PRIVATE phynned_action)
    target_compile_features(selective_revert_test PRIVATE cxx_std_23)
    if(NOT MSVC)
        target_compile_options(selective_revert_test PRIVATE -Wall -Wextra)
    endif()
    add_test(NAME selective_revert_test COMMAND selective_revert_test)
    set_tests_properties(selective_revert_test PROPERTIES TIMEOUT 30)

    # DR1 (S3) — END-TO-END: the journal wired into ActionExecutor restores a
    # placed process to its CAPTURED prev after a simulated taskkill of the agent
    # (leaked executor → recover() on a fresh construction). Windows-only: uses
    # CreateProcess + Set/GetProcessAffinityMask on a spawned child target.
    if(WIN32)
        add_executable(journal_taskkill_test tests/journal_taskkill_test.cpp)
        target_link_libraries(journal_taskkill_test PRIVATE phynned_action)
        target_compile_features(journal_taskkill_test PRIVATE cxx_std_23)
        if(NOT MSVC)
            target_compile_options(journal_taskkill_test PRIVATE -Wall -Wextra)
        endif()
        add_test(NAME journal_taskkill_test COMMAND journal_taskkill_test)
        set_tests_properties(journal_taskkill_test PROPERTIES TIMEOUT 30)
    endif()
endif()
# Made with my soul - Swately <3
