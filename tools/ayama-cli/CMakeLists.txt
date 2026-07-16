# ayama/tools/ayama-cli/CMakeLists.txt
# ayama-cli — command-line management tool for ayama-agent.
#
# Commands: status, targets, actions, bench diff, memory list/clear.
#
# §Ayama Master Plan §8.2

add_executable(ayama-cli main.cpp)

target_link_libraries(ayama-cli PRIVATE
    ayama_ipc
    ayama_learn
    ayama_observer   # TargetProcess, TargetMetrics types (via ayama_ipc)
    ayama_action     # ActionLog types (via ayama_ipc)
    ayama_policy     # PolicyDecision types (via ayama_ipc)
)

target_compile_features(ayama-cli PRIVATE cxx_std_23)

if(MSVC)
    target_compile_options(ayama-cli PRIVATE /W4)
    set_target_properties(ayama-cli PROPERTIES
        LINK_FLAGS "/SUBSYSTEM:CONSOLE")
else()
    target_compile_options(ayama-cli PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(WIN32)
    target_link_libraries(ayama-cli PRIVATE kernel32)
endif()

if(CMAKE_INSTALL_PREFIX)
    install(TARGETS ayama-cli RUNTIME DESTINATION bin)
endif()
# Made with my soul - Swately <3
