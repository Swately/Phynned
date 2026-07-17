# phynned/tools/phynned-agent/CMakeLists.txt
# phynned-agent — Phynned background optimizer daemon executable.
#

add_executable(phynned-agent main.cpp)

target_link_libraries(phynned-agent PRIVATE
    phynned_core
    phynned_ipc
    phynned_bench     # ABRunner, PerceptualMetrics, Baseline
    phynned_config    # ConfigStore — policies.toml
    phynned_learn     # PerGameMemory — memory.toml
)

target_compile_features(phynned-agent PRIVATE cxx_std_23)

if(MSVC)
    target_compile_options(phynned-agent PRIVATE /W4)
    # Subsystem: console (not windows) so stdout works even when launched manually
    set_target_properties(phynned-agent PROPERTIES
        LINK_FLAGS "/SUBSYSTEM:CONSOLE")
else()
    target_compile_options(phynned-agent PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(WIN32)
    target_link_libraries(phynned-agent PRIVATE kernel32 advapi32)
endif()

# Install target (optional — used by installer)
if(CMAKE_INSTALL_PREFIX)
    install(TARGETS phynned-agent RUNTIME DESTINATION bin)
endif()
# Made with my soul - Swately <3
