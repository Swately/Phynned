# phynned/learn/CMakeLists.txt
# phynned_learn — PerGameMemory (per-game learned optimisations, memory.toml).
#
add_library(phynned_learn STATIC
    src/PerGameMemory.cpp
    src/RouteAdvisor.cpp          # MR-1 shadow router (advice only; never places)
)

target_include_directories(phynned_learn PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    # MR-1: RouteAdvisor uses the observer POD headers (TargetMetrics / TargetKind)
    # only — no observer .cpp symbols — so we add the include path WITHOUT linking
    # phynned_observer. This keeps learn decoupled (no heavy psapi/pdh/etw transitive
    # deps, no link cycle) while the RouteAdvice value type crosses the module edge.
    ${CMAKE_CURRENT_SOURCE_DIR}/../observer/include
)

target_link_libraries(phynned_learn PUBLIC
    phyriad_schema
    phyriad_topology   # for hw::v_cache_cores(), hw::e_cores(), hw::enumerate_cores()
)

target_compile_features(phynned_learn PUBLIC cxx_std_23)

if(MSVC)
    target_compile_options(phynned_learn PRIVATE /W4)
else()
    target_compile_options(phynned_learn PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(WIN32)
    target_compile_definitions(phynned_learn PRIVATE
        WIN32_LEAN_AND_MEAN NOMINMAX)
    target_link_libraries(phynned_learn PRIVATE shell32)
endif()

if(PHYNNED_BUILD_TESTS)
    # T8: PerGameMemory TOML roundtrip, bad-list, revalidation, expiry (§9.3)
    add_executable(phynned_learn_test tests/learn_test.cpp)
    target_link_libraries(phynned_learn_test PRIVATE phynned_learn)
    target_compile_features(phynned_learn_test PRIVATE cxx_std_23)
    if(NOT MSVC)
        target_compile_options(phynned_learn_test PRIVATE -Wall -Wextra)
    endif()
    enable_testing()
    add_test(NAME phynned_learn_test COMMAND phynned_learn_test)
endif()
# Made with my soul - Swately <3
