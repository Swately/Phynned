# ayama/tools/ayama-agent/CMakeLists.txt
# ayama-agent — Ayama background optimizer daemon executable.
#

add_executable(ayama-agent main.cpp)

target_link_libraries(ayama-agent PRIVATE
    ayama_core
    ayama_ipc
    ayama_bench     # ABRunner, PerceptualMetrics, Baseline
    ayama_config    # ConfigStore — policies.toml
    ayama_learn     # PerGameMemory — memory.toml
)

target_compile_features(ayama-agent PRIVATE cxx_std_23)

if(MSVC)
    target_compile_options(ayama-agent PRIVATE /W4)
    # Subsystem: console (not windows) so stdout works even when launched manually
    set_target_properties(ayama-agent PROPERTIES
        LINK_FLAGS "/SUBSYSTEM:CONSOLE")
else()
    target_compile_options(ayama-agent PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(WIN32)
    target_link_libraries(ayama-agent PRIVATE kernel32 advapi32)
endif()

# Install target (optional — used by installer)
if(CMAKE_INSTALL_PREFIX)
    install(TARGETS ayama-agent RUNTIME DESTINATION bin)
endif()
# Made with my soul - Swately <3
