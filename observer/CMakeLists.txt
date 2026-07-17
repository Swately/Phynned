# phynned/observer/CMakeLists.txt
# phynned_observer — ProcessObserver, MetricsCollector, ProcessClassifier,
#                  ForegroundWatcher, ClassificationCache, TargetProcess, TargetMetrics.
#
# Dependencies: phyriad_schema, phyriad_hal, phyriad_topology (for hw:: APIs),
#               phyriad_process (FR-4 enumerate_processes + FR-11 ProcessMetricsSnapshot),
#               phyriad_etw (FR-6 SessionManager).
# Note: phyriad_process provides BOTH the bulk NtQSI snapshot used by
# MetricsCollector AND the cross-platform enumerate_processes used by
# ProcessObserver::refresh().
# phyriad_daemon removed — its CrossProcessAffinity API is replaced by
# phyriad::hw::set_process_affinity / set_process_priority in phyriad_topology.

set(_obs_sources
    src/ProcessObserver.cpp
    src/ProcessObserver_win32.cpp
    src/ProcessObserver_linux.cpp
    src/MetricsCollector_win32.cpp
    src/MetricsCollector_linux.cpp
    src/ProcessClassifier.cpp
)

add_library(phynned_observer STATIC ${_obs_sources})

target_include_directories(phynned_observer PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(phynned_observer PUBLIC
    phyriad_schema
    phyriad_hal
    phyriad_topology  # set_process_affinity / set_process_priority via hw::
    phyriad_process   # enumerate_processes (FR-4), ProcessMetricsSnapshot (FR-11)
    phyriad_etw       # SessionManager (FR-6)
    phyriad_stigmergy # §P-0.5.3 — ProcessClassifier extends stigmergy::Classifier
)

if(WIN32)
    target_link_libraries(phynned_observer PUBLIC psapi pdh kernel32)
endif()

target_compile_features(phynned_observer PUBLIC cxx_std_23)

if(MSVC)
    target_compile_options(phynned_observer PRIVATE /W4 /wd4100)
else()
    target_compile_options(phynned_observer PRIVATE -Wall -Wextra -Wpedantic)
endif()

# ── Tests ─────────────────────────────────────────────────────────────────────
if(PHYNNED_BUILD_TESTS)
    add_executable(phynned_observer_test tests/process_observer_test.cpp)
    target_link_libraries(phynned_observer_test PRIVATE phynned_observer phynned_policy)
    target_compile_features(phynned_observer_test PRIVATE cxx_std_23)
    if(NOT MSVC)
        target_compile_options(phynned_observer_test PRIVATE -Wall -Wextra)
    endif()
    enable_testing()
    add_test(NAME phynned_observer_test COMMAND phynned_observer_test)
endif()
# Made with my soul - Swately <3
