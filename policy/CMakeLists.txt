# phynned/policy/CMakeLists.txt
# phynned_policy — PolicyEngine, Rule, Condition, PolicyDecision.
#
# Dependencies: phyriad_schema, phyriad_topology, phynned_observer, phynned_learn.
#

add_library(phynned_policy STATIC
    src/PolicyEngine.cpp
)

target_include_directories(phynned_policy PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(phynned_policy PUBLIC
    phyriad_schema
    phyriad_topology
    phyriad_stigmergy   # §P-0.5.3 — AutoPolicySelector extends stigmergy::Classifier
    phynned_observer
    phynned_learn         # AutoPolicySelector.hpp includes PerGameMemory.hpp (§9.2)
)

target_compile_features(phynned_policy PUBLIC cxx_std_23)

if(MSVC)
    target_compile_options(phynned_policy PRIVATE /W4)
else()
    target_compile_options(phynned_policy PRIVATE -Wall -Wextra -Wpedantic)
endif()

# ── Tests ─────────────────────────────────────────────────────────────────────
if(PHYNNED_BUILD_TESTS)
    # T-policy: PolicyEngine rule evaluation
    add_executable(phynned_policy_test tests/policy_engine_test.cpp)
    target_link_libraries(phynned_policy_test PRIVATE phynned_policy)
    target_compile_features(phynned_policy_test PRIVATE cxx_std_23)
    if(NOT MSVC)
        target_compile_options(phynned_policy_test PRIVATE -Wall -Wextra)
    endif()
    enable_testing()
    add_test(NAME phynned_policy_test COMMAND phynned_policy_test)

    # T7: AutoPolicySelector classification + per-game memory shortcut (§9.2)
    add_executable(phynned_auto_policy_test tests/auto_policy_selector_test.cpp)
    target_link_libraries(phynned_auto_policy_test PRIVATE
        phynned_policy  # publicly links phynned_learn → PerGameMemory is available
    )
    target_compile_features(phynned_auto_policy_test PRIVATE cxx_std_23)
    if(NOT MSVC)
        target_compile_options(phynned_auto_policy_test PRIVATE -Wall -Wextra)
    endif()
    add_test(NAME phynned_auto_policy_test COMMAND phynned_auto_policy_test)
endif()
# Made with my soul - Swately <3
