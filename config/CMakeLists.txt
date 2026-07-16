# ayama/config/CMakeLists.txt
# ayama_config — ConfigStore (TOML persistence for policies.toml).
#

add_library(ayama_config STATIC
    src/ConfigStore.cpp
)

target_include_directories(ayama_config PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# ConfigStore.cpp includes <ayama/version.hpp> (lives in core/include)
# to stamp the AYAMA_VERSION_STRING into the generated TOML header. We expose
# the include path directly — linking against ayama_core would create a cycle
# because ayama_core PRIVATE-depends on ayama_config (see core/CMakeLists.txt).
target_include_directories(ayama_config PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../core/include
)

target_link_libraries(ayama_config PUBLIC
    phyriad_schema
    ayama_policy    # DefaultPolicyPack.hpp uses AutoPolicySelector (§5.3)
)

target_compile_features(ayama_config PUBLIC cxx_std_23)

if(MSVC)
    target_compile_options(ayama_config PRIVATE /W4)
else()
    target_compile_options(ayama_config PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(WIN32)
    target_compile_definitions(ayama_config PRIVATE
        WIN32_LEAN_AND_MEAN NOMINMAX)
    # SHGetFolderPath is in Shell32 (available without extra linking on MinGW)
    target_link_libraries(ayama_config PRIVATE shell32)
endif()

if(AYAMA_BUILD_TESTS)
    if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/tests/config_test.cpp)
        add_executable(ayama_config_test tests/config_test.cpp)
        target_link_libraries(ayama_config_test PRIVATE ayama_config)
        target_compile_features(ayama_config_test PRIVATE cxx_std_23)
        enable_testing()
        add_test(NAME ayama_config_test COMMAND ayama_config_test)
    endif()
endif()
# Made with my soul - Swately <3
