# phynned/tools/phynned-cli/CMakeLists.txt
# phynned-cli — command-line management tool for phynned-agent.
#
# Commands: status, targets, actions, bench diff, memory list/clear.
#
# §Phynned Master Plan §8.2

add_executable(phynned-cli main.cpp)

target_link_libraries(phynned-cli PRIVATE
    phynned_ipc
    phynned_learn
    phynned_observer   # TargetProcess, TargetMetrics types (via phynned_ipc)
    phynned_action     # ActionLog types (via phynned_ipc)
    phynned_policy     # PolicyDecision types (via phynned_ipc)
)

target_compile_features(phynned-cli PRIVATE cxx_std_23)

if(MSVC)
    target_compile_options(phynned-cli PRIVATE /W4)
    set_target_properties(phynned-cli PROPERTIES
        LINK_FLAGS "/SUBSYSTEM:CONSOLE")
else()
    target_compile_options(phynned-cli PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(WIN32)
    target_link_libraries(phynned-cli PRIVATE kernel32)
endif()

if(CMAKE_INSTALL_PREFIX)
    install(TARGETS phynned-cli RUNTIME DESTINATION bin)
endif()
# Made with my soul - Swately <3
