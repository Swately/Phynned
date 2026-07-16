# ayama/ipc/CMakeLists.txt
# ayama_ipc — AyamaProtocol, AyamaClient (shared memory IPC).
#
# Dependencies: phyriad_schema, ayama_observer, ayama_policy, ayama_action.
#

add_library(ayama_ipc STATIC
    src/AyamaClient.cpp
    src/AyamaAgentPublisher.cpp
)

target_include_directories(ayama_ipc PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(ayama_ipc PUBLIC
    phyriad_schema
    ayama_observer
    ayama_policy
    ayama_action
)

if(WIN32)
    target_link_libraries(ayama_ipc PUBLIC kernel32)
endif()

target_compile_features(ayama_ipc PUBLIC cxx_std_23)

if(MSVC)
    target_compile_options(ayama_ipc PRIVATE /W4)
else()
    target_compile_options(ayama_ipc PRIVATE -Wall -Wextra -Wpedantic)
endif()
# Made with my soul - Swately <3
