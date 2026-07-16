# ayama/tools/installer/CMakeLists.txt
# ayama-service-register — Windows service registration helper.
#
add_executable(ayama-service-register ayama_service_register.cpp)

target_compile_features(ayama-service-register PRIVATE cxx_std_23)

if(MSVC)
    target_compile_options(ayama-service-register PRIVATE /W4)
    set_target_properties(ayama-service-register PROPERTIES
        LINK_FLAGS "/SUBSYSTEM:CONSOLE")
    # advapi32 for Service Control Manager APIs
    target_link_libraries(ayama-service-register PRIVATE advapi32 shell32 kernel32)
else()
    target_compile_options(ayama-service-register PRIVATE -Wall -Wextra)
    if(WIN32)
        target_link_libraries(ayama-service-register PRIVATE advapi32 shell32 kernel32)
    endif()
endif()

if(CMAKE_INSTALL_PREFIX)
    install(TARGETS ayama-service-register RUNTIME DESTINATION bin)
    install(FILES install.bat uninstall.bat DESTINATION bin)
endif()
# Made with my soul - Swately <3
