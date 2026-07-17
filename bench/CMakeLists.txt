# phynned/bench/CMakeLists.txt
# phynned_bench — Baseline, ABRunner, PerceptualMetrics.
#
# Dependencies: phyriad_schema, phyriad_hal, phynned_observer, phynned_action.
#
add_library(phynned_bench STATIC
    src/Baseline.cpp
    src/ABRunner.cpp
    src/PerceptualMetrics.cpp
)

target_include_directories(phynned_bench PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(phynned_bench PUBLIC
    phyriad_schema
    phyriad_hal
    phynned_observer
    phynned_action
)

target_compile_features(phynned_bench PUBLIC cxx_std_23)

if(MSVC)
    target_compile_options(phynned_bench PRIVATE /W4)
else()
    target_compile_options(phynned_bench PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(PHYNNED_BUILD_TESTS)
    if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/tests/baseline_test.cpp)
        add_executable(phynned_bench_test tests/baseline_test.cpp)
        target_link_libraries(phynned_bench_test PRIVATE phynned_bench)
        target_compile_features(phynned_bench_test PRIVATE cxx_std_23)
        enable_testing()
        add_test(NAME phynned_bench_test COMMAND phynned_bench_test)
    endif()

    if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/tests/abrunner_test.cpp)
        add_executable(phynned_abrunner_test tests/abrunner_test.cpp)
        target_link_libraries(phynned_abrunner_test PRIVATE phynned_bench)
        target_compile_features(phynned_abrunner_test PRIVATE cxx_std_23)
        enable_testing()
        add_test(NAME phynned_abrunner_test COMMAND phynned_abrunner_test)
    endif()

    if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/tests/perceptual_test.cpp)
        add_executable(phynned_perceptual_test tests/perceptual_test.cpp)
        target_link_libraries(phynned_perceptual_test PRIVATE phynned_bench)
        target_compile_features(phynned_perceptual_test PRIVATE cxx_std_23)
        enable_testing()
        add_test(NAME phynned_perceptual_test COMMAND phynned_perceptual_test)
    endif()
endif()
# Made with my soul - Swately <3
