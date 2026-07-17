# phynned/ipc/CMakeLists.txt
# phynned_ipc — PhynnedProtocol, PhynnedClient (shared memory IPC).
#
# Dependencies: phyriad_schema, phynned_observer, phynned_policy, phynned_action.
#

add_library(phynned_ipc STATIC
    src/PhynnedClient.cpp
    src/PhynnedAgentPublisher.cpp
)

target_include_directories(phynned_ipc PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(phynned_ipc PUBLIC
    phyriad_schema
    phynned_observer
    phynned_policy
    phynned_action
)

if(WIN32)
    target_link_libraries(phynned_ipc PUBLIC kernel32)
endif()

target_compile_features(phynned_ipc PUBLIC cxx_std_23)

if(MSVC)
    target_compile_options(phynned_ipc PRIVATE /W4)
else()
    target_compile_options(phynned_ipc PRIVATE -Wall -Wextra -Wpedantic)
endif()
# Made with my soul - Swately <3
