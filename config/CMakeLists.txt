# phynned/config/CMakeLists.txt
# phynned_config — ConfigStore (TOML persistence for policies.toml).
#

add_library(phynned_config STATIC
    src/ConfigStore.cpp
)

target_include_directories(phynned_config PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# ConfigStore.cpp includes <phynned/version.hpp> (lives in core/include)
# to stamp the PHYNNED_VERSION_STRING into the generated TOML header. We expose
# the include path directly — linking against phynned_core would create a cycle
# because phynned_core PRIVATE-depends on phynned_config (see core/CMakeLists.txt).
target_include_directories(phynned_config PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../core/include
)

target_link_libraries(phynned_config PUBLIC
    phyriad_schema
    phynned_policy    # DefaultPolicyPack.hpp uses AutoPolicySelector (§5.3)
)

target_compile_features(phynned_config PUBLIC cxx_std_23)

if(MSVC)
    target_compile_options(phynned_config PRIVATE /W4)
else()
    target_compile_options(phynned_config PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(WIN32)
    target_compile_definitions(phynned_config PRIVATE
        WIN32_LEAN_AND_MEAN NOMINMAX)
    # SHGetFolderPath is in Shell32 (available without extra linking on MinGW)
    target_link_libraries(phynned_config PRIVATE shell32)
endif()

if(PHYNNED_BUILD_TESTS)
    if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/tests/config_test.cpp)
        add_executable(phynned_config_test tests/config_test.cpp)
        target_link_libraries(phynned_config_test PRIVATE phynned_config)
        target_compile_features(phynned_config_test PRIVATE cxx_std_23)
        enable_testing()
        add_test(NAME phynned_config_test COMMAND phynned_config_test)
    endif()
endif()
# Made with my soul - Swately <3
