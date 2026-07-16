# ayama/policy/CMakeLists.txt
# ayama_policy — PolicyEngine, Rule, Condition, PolicyDecision.
#
# Dependencies: phyriad_schema, phyriad_topology, ayama_observer, ayama_learn.
#

add_library(ayama_policy STATIC
    src/PolicyEngine.cpp
)

target_include_directories(ayama_policy PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(ayama_policy PUBLIC
    phyriad_schema
    phyriad_topology
    phyriad_stigmergy   # §P-0.5.3 — AutoPolicySelector extends stigmergy::Classifier
    ayama_observer
    ayama_learn         # AutoPolicySelector.hpp includes PerGameMemory.hpp (§9.2)
)

target_compile_features(ayama_policy PUBLIC cxx_std_23)

if(MSVC)
    target_compile_options(ayama_policy PRIVATE /W4)
else()
    target_compile_options(ayama_policy PRIVATE -Wall -Wextra -Wpedantic)
endif()

# ── Tests ─────────────────────────────────────────────────────────────────────
if(AYAMA_BUILD_TESTS)
    # T-policy: PolicyEngine rule evaluation
    add_executable(ayama_policy_test tests/policy_engine_test.cpp)
    target_link_libraries(ayama_policy_test PRIVATE ayama_policy)
    target_compile_features(ayama_policy_test PRIVATE cxx_std_23)
    if(NOT MSVC)
        target_compile_options(ayama_policy_test PRIVATE -Wall -Wextra)
    endif()
    enable_testing()
    add_test(NAME ayama_policy_test COMMAND ayama_policy_test)

    # T7: AutoPolicySelector classification + per-game memory shortcut (§9.2)
    add_executable(ayama_auto_policy_test tests/auto_policy_selector_test.cpp)
    target_link_libraries(ayama_auto_policy_test PRIVATE
        ayama_policy  # publicly links ayama_learn → PerGameMemory is available
    )
    target_compile_features(ayama_auto_policy_test PRIVATE cxx_std_23)
    if(NOT MSVC)
        target_compile_options(ayama_auto_policy_test PRIVATE -Wall -Wextra)
    endif()
    add_test(NAME ayama_auto_policy_test COMMAND ayama_auto_policy_test)
endif()
# Made with my soul - Swately <3
