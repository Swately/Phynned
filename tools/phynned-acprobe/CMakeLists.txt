# phynned/tools/phynned-acprobe/CMakeLists.txt
# phynned-acprobe — standalone M0-safety proof harness for the CR1 AcProbe.
#
# Runs the least-privilege anti-cheat probe against a real/synthetic target and
# dumps every handle open (or the absence of one for a refused title). This is
# the vehicle that proves the CR1 mitigation is safe before ActionExecutor
# integration. See docs/plans/PHYNNED_MASS_ROUTER_RISK_REGISTER.md CR1.

add_executable(phynned-acprobe main.cpp)

target_link_libraries(phynned-acprobe PRIVATE
    phynned_action     # AcProbe + AuditLog (also pulls observer + learn)
    phynned_observer   # AcDriverOracle (zero-handle AC gate)
    phynned_learn      # PerGameMemory (the ALLOWED/BLOCKED label store)
)

target_compile_features(phynned-acprobe PRIVATE cxx_std_23)

if(MSVC)
    target_compile_options(phynned-acprobe PRIVATE /W4)
    set_target_properties(phynned-acprobe PROPERTIES
        LINK_FLAGS "/SUBSYSTEM:CONSOLE")
else()
    target_compile_options(phynned-acprobe PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(WIN32)
    # psapi/advapi32: driver+service enum (via AcDriverOracle). kernel32: OpenProcess.
    target_link_libraries(phynned-acprobe PRIVATE psapi advapi32 kernel32)
endif()

if(CMAKE_INSTALL_PREFIX)
    install(TARGETS phynned-acprobe RUNTIME DESTINATION bin)
endif()
# Made with my soul - Swately <3
