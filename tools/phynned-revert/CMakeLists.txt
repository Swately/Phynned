# phynned/tools/phynned-revert/CMakeLists.txt
# phynned-revert — the external crash-recovery DEAD-MAN for DR1.
#
# Reads the write-ahead RevertJournal and reverts every surviving APPLIED
# placement to its captured prev_mask (reconciled by the pid-recycle guard).
# Covers taskkill-WITHOUT-restart: the operator or a watchdog runs it after the
# agent is killed and never comes back. Idempotent — safe to run anytime.
# See docs/plans/PHYNNED_MASS_ROUTER_RISK_REGISTER.md DR1 + STRATEGIES S2.

add_executable(phynned-revert main.cpp)

target_link_libraries(phynned-revert PRIVATE
    phynned_action     # RevertJournal (the write-ahead journal)
)

target_compile_features(phynned-revert PRIVATE cxx_std_23)

if(MSVC)
    target_compile_options(phynned-revert PRIVATE /W4)
    set_target_properties(phynned-revert PROPERTIES
        LINK_FLAGS "/SUBSYSTEM:CONSOLE")
else()
    target_compile_options(phynned-revert PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(WIN32)
    # kernel32: OpenProcess/GetProcessTimes/QueryFullProcessImageName/FlushFileBuffers.
    target_link_libraries(phynned-revert PRIVATE kernel32)
endif()

if(CMAKE_INSTALL_PREFIX)
    install(TARGETS phynned-revert RUNTIME DESTINATION bin)
endif()
# Made with my soul - Swately <3
