# ayama/core/CMakeLists.txt
# ayama_core — AgentRuntime, AdaptiveTick, SelfMonitor, PowerWatch, SingleInstance.
#
# Dependencies: phyriad_hal (WakeEvent FR-16), phyriad_schema, phyriad_topology,
#               phyriad_tuning (WorkingSet FR-19), phyriad_process (self_pid FR-18),
#               ayama_observer, ayama_policy, ayama_action.
#

add_library(ayama_core STATIC
    src/AgentRuntime.cpp
    src/AutoRevertGuard.cpp
)

target_include_directories(ayama_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(ayama_core PUBLIC
    phyriad_hal           # WakeEvent (FR-16), Timestamp
    phyriad_schema
    phyriad_topology
    phyriad_tuning        # set_self_working_set (FR-19), PrivilegeCheck
    phyriad_process       # self_pid / self_name (FR-18)
    ayama_observer
    ayama_policy
    ayama_action
)

# Implementation-only: not exposed through AgentRuntime.hpp's public API.
target_link_libraries(ayama_core PRIVATE
    ayama_bench     # ABRunner, PerceptualMetrics
    ayama_config    # ConfigStore
    ayama_learn     # PerGameMemory
    ayama_ipc       # AyamaAgentPublisher (SHM write side)
)

if(WIN32)
    # kernel32: SetThreadAffinityMask, GetCurrentThread (apply_self_pin).
    # psapi removed: now provided transitively via phyriad_process.
    target_link_libraries(ayama_core PUBLIC kernel32)
endif()

target_compile_features(ayama_core PUBLIC cxx_std_23)

if(MSVC)
    target_compile_options(ayama_core PRIVATE /W4)
else()
    target_compile_options(ayama_core PRIVATE -Wall -Wextra -Wpedantic)
endif()

# ── Tests ─────────────────────────────────────────────────────────────────────
if(AYAMA_BUILD_TESTS)
    add_executable(ayama_core_test tests/agent_runtime_test.cpp)
    target_link_libraries(ayama_core_test PRIVATE ayama_core)
    target_compile_features(ayama_core_test PRIVATE cxx_std_23)
    if(NOT MSVC)
        target_compile_options(ayama_core_test PRIVATE -Wall -Wextra)
    endif()
    if(CMAKE_TESTING_ENABLED OR AYAMA_BUILD_TESTS)
        enable_testing()
        add_test(NAME ayama_core_test COMMAND ayama_core_test)
    endif()

    # ── §7.2 T4 + T6 + T9 — Idle resource budget + degradation-safe sin admin ──
    # 5 s de run en modo sin SHM / sin self-pin; verifica:
    #   T4  → tick_count > 0 + CPU% < 5%
    #   T6  → start() Ok sin admin + no crash
    #   T9  → ΔRSS < 10 MB durante el run + stop() limpio < 2 s
    # NOTA: T2/T3/T5 requieren admin + procesos reales (juego o ayama-bench),
    #       no son automatizables en CI estándar. Documentados en §8.7.
    add_executable(agent_idle_budget_test tests/agent_idle_budget_test.cpp)
    target_link_libraries(agent_idle_budget_test PRIVATE ayama_core)
    target_compile_features(agent_idle_budget_test PRIVATE cxx_std_23)
    if(WIN32)
        target_link_libraries(agent_idle_budget_test PRIVATE psapi)
    endif()
    if(NOT MSVC)
        target_compile_options(agent_idle_budget_test PRIVATE -Wall -Wextra)
    endif()
    if(CMAKE_TESTING_ENABLED OR AYAMA_BUILD_TESTS)
        # Timeout: 5 s sleep + ≤ 2 s stop + márgenes para entornos lentos.
        add_test(NAME agent_idle_budget_test COMMAND agent_idle_budget_test)
        set_tests_properties(agent_idle_budget_test PROPERTIES TIMEOUT 30)
    endif()
endif()
# Made with my soul - Swately <3
