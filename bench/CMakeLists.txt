# ayama/bench/CMakeLists.txt
# ayama_bench — Baseline, ABRunner, PerceptualMetrics.
#
# Dependencies: phyriad_schema, phyriad_hal, ayama_observer, ayama_action.
#
add_library(ayama_bench STATIC
    src/Baseline.cpp
    src/ABRunner.cpp
    src/PerceptualMetrics.cpp
)

target_include_directories(ayama_bench PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(ayama_bench PUBLIC
    phyriad_schema
    phyriad_hal
    ayama_observer
    ayama_action
)

target_compile_features(ayama_bench PUBLIC cxx_std_23)

if(MSVC)
    target_compile_options(ayama_bench PRIVATE /W4)
else()
    target_compile_options(ayama_bench PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(AYAMA_BUILD_TESTS)
    if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/tests/baseline_test.cpp)
        add_executable(ayama_bench_test tests/baseline_test.cpp)
        target_link_libraries(ayama_bench_test PRIVATE ayama_bench)
        target_compile_features(ayama_bench_test PRIVATE cxx_std_23)
        enable_testing()
        add_test(NAME ayama_bench_test COMMAND ayama_bench_test)
    endif()

    if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/tests/abrunner_test.cpp)
        add_executable(ayama_abrunner_test tests/abrunner_test.cpp)
        target_link_libraries(ayama_abrunner_test PRIVATE ayama_bench)
        target_compile_features(ayama_abrunner_test PRIVATE cxx_std_23)
        enable_testing()
        add_test(NAME ayama_abrunner_test COMMAND ayama_abrunner_test)
    endif()

    if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/tests/perceptual_test.cpp)
        add_executable(ayama_perceptual_test tests/perceptual_test.cpp)
        target_link_libraries(ayama_perceptual_test PRIVATE ayama_bench)
        target_compile_features(ayama_perceptual_test PRIVATE cxx_std_23)
        enable_testing()
        add_test(NAME ayama_perceptual_test COMMAND ayama_perceptual_test)
    endif()
endif()
# Made with my soul - Swately <3
