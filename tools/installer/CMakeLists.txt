# phynned/tools/installer/CMakeLists.txt
# phynned-service-register — Windows service registration helper.
#
add_executable(phynned-service-register phynned_service_register.cpp)

target_compile_features(phynned-service-register PRIVATE cxx_std_23)

if(MSVC)
    target_compile_options(phynned-service-register PRIVATE /W4)
    set_target_properties(phynned-service-register PROPERTIES
        LINK_FLAGS "/SUBSYSTEM:CONSOLE")
    # advapi32 for Service Control Manager APIs
    target_link_libraries(phynned-service-register PRIVATE advapi32 shell32 kernel32)
else()
    target_compile_options(phynned-service-register PRIVATE -Wall -Wextra)
    if(WIN32)
        target_link_libraries(phynned-service-register PRIVATE advapi32 shell32 kernel32)
    endif()
endif()

if(CMAKE_INSTALL_PREFIX)
    install(TARGETS phynned-service-register RUNTIME DESTINATION bin)
    install(FILES install.bat uninstall.bat DESTINATION bin)
endif()
# Made with my soul - Swately <3
