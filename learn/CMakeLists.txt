# ayama/learn/CMakeLists.txt
# ayama_learn — PerGameMemory (per-game learned optimisations, memory.toml).
#
add_library(ayama_learn STATIC
    src/PerGameMemory.cpp
)

target_include_directories(ayama_learn PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(ayama_learn PUBLIC
    phyriad_schema
    phyriad_topology   # for hw::v_cache_cores(), hw::e_cores(), hw::enumerate_cores()
)

target_compile_features(ayama_learn PUBLIC cxx_std_23)

if(MSVC)
    target_compile_options(ayama_learn PRIVATE /W4)
else()
    target_compile_options(ayama_learn PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(WIN32)
    target_compile_definitions(ayama_learn PRIVATE
        WIN32_LEAN_AND_MEAN NOMINMAX)
    target_link_libraries(ayama_learn PRIVATE shell32)
endif()

if(AYAMA_BUILD_TESTS)
    # T8: PerGameMemory TOML roundtrip, bad-list, revalidation, expiry (§9.3)
    add_executable(ayama_learn_test tests/learn_test.cpp)
    target_link_libraries(ayama_learn_test PRIVATE ayama_learn)
    target_compile_features(ayama_learn_test PRIVATE cxx_std_23)
    if(NOT MSVC)
        target_compile_options(ayama_learn_test PRIVATE -Wall -Wextra)
    endif()
    enable_testing()
    add_test(NAME ayama_learn_test COMMAND ayama_learn_test)
endif()
# Made with my soul - Swately <3
