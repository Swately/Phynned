# phynned/core/CMakeLists.txt
# phynned_core — AgentRuntime, AdaptiveTick, SelfMonitor, PowerWatch, SingleInstance.
#
# Dependencies: phyriad_hal (WakeEvent FR-16), phyriad_schema, phyriad_topology,
#               phyriad_tuning (WorkingSet FR-19), phyriad_process (self_pid FR-18),
#               phynned_observer, phynned_policy, phynned_action.
#

add_library(phynned_core STATIC
    src/AgentRuntime.cpp
    src/AutoRevertGuard.cpp
)

target_include_directories(phynned_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(phynned_core PUBLIC
    phyriad_hal           # WakeEvent (FR-16), Timestamp
    phyriad_schema
    phyriad_topology
    phyriad_tuning        # set_self_working_set (FR-19), PrivilegeCheck
    phyriad_process       # self_pid / self_name (FR-18)
    phynned_observer
    phynned_policy
    phynned_action
)

# Implementation-only: not exposed through AgentRuntime.hpp's public API.
target_link_libraries(phynned_core PRIVATE
    phynned_bench     # ABRunner, PerceptualMetrics
    phynned_config    # ConfigStore
    phynned_learn     # PerGameMemory
    phynned_ipc       # PhynnedAgentPublisher (SHM write side)
)

if(WIN32)
    # kernel32: SetThreadAffinityMask, GetCurrentThread (apply_self_pin).
    # psapi removed: now provided transitively via phyriad_process.
    target_link_libraries(phynned_core PUBLIC kernel32)
endif()

target_compile_features(phynned_core PUBLIC cxx_std_23)

if(MSVC)
    target_compile_options(phynned_core PRIVATE /W4)
else()
    target_compile_options(phynned_core PRIVATE -Wall -Wextra -Wpedantic)
endif()

# ── Tests ─────────────────────────────────────────────────────────────────────
if(PHYNNED_BUILD_TESTS)
    add_executable(phynned_core_test tests/agent_runtime_test.cpp)
    target_link_libraries(phynned_core_test PRIVATE phynned_core)
    target_compile_features(phynned_core_test PRIVATE cxx_std_23)
    if(NOT MSVC)
        target_compile_options(phynned_core_test PRIVATE -Wall -Wextra)
    endif()
    if(CMAKE_TESTING_ENABLED OR PHYNNED_BUILD_TESTS)
        enable_testing()
        add_test(NAME phynned_core_test COMMAND phynned_core_test)
    endif()

    # ── §7.2 T4 + T6 + T9 — Idle resource budget + degradation-safe sin admin ──
    # 5 s de run en modo sin SHM / sin self-pin; verifica:
    #   T4  → tick_count > 0 + CPU% < 5%
    #   T6  → start() Ok sin admin + no crash
    #   T9  → ΔRSS < 10 MB durante el run + stop() limpio < 2 s
    # NOTA: T2/T3/T5 requieren admin + procesos reales (juego o phynned-bench),
    #       no son automatizables en CI estándar. Documentados en §8.7.
    add_executable(agent_idle_budget_test tests/agent_idle_budget_test.cpp)
    target_link_libraries(agent_idle_budget_test PRIVATE phynned_core)
    target_compile_features(agent_idle_budget_test PRIVATE cxx_std_23)
    if(WIN32)
        target_link_libraries(agent_idle_budget_test PRIVATE psapi)
    endif()
    if(NOT MSVC)
        target_compile_options(agent_idle_budget_test PRIVATE -Wall -Wextra)
    endif()
    if(CMAKE_TESTING_ENABLED OR PHYNNED_BUILD_TESTS)
        # Timeout: 5 s sleep + ≤ 2 s stop + márgenes para entornos lentos.
        add_test(NAME agent_idle_budget_test COMMAND agent_idle_budget_test)
        set_tests_properties(agent_idle_budget_test PROPERTIES TIMEOUT 30)
    endif()

    # ── Use-modes §5 tests 4 & 5 — R1 (AC gate) + R4 (flap guard) ──────────────
    # Pure evaluate_user_pin decision + the real AcProbe class-c refusal. No admin.
    add_executable(use_modes_test tests/use_modes_test.cpp)
    # phynned_config linked directly: the test exercises find_user_rule_for
    # (UserRuleLookup.hpp), which consumes ConfigStore — config is PRIVATE to
    # phynned_core, so its include dir does not propagate.
    target_link_libraries(use_modes_test PRIVATE phynned_core phynned_config)
    target_compile_features(use_modes_test PRIVATE cxx_std_23)
    if(NOT MSVC)
        target_compile_options(use_modes_test PRIVATE -Wall -Wextra)
    endif()
    if(CMAKE_TESTING_ENABLED OR PHYNNED_BUILD_TESTS)
        enable_testing()
        add_test(NAME use_modes_test COMMAND use_modes_test)
    endif()
endif()
# Made with my soul - Swately <3
