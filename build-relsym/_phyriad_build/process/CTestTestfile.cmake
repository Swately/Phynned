# CMake generated Testfile for 
# Source directory: G:/Phyriad/projects/Phynned/framework/process
# Build directory: G:/Phyriad/projects/Phynned/build-relsym/_phyriad_build/process
# 
# This file includes the relevant testing commands required for 
# testing this directory and lists subdirectories to be tested as well.
add_test([=[process_test]=] "G:/Phyriad/projects/Phynned/build-relsym/_phyriad_build/process/process_test.exe" "LABELS" "process")
set_tests_properties([=[process_test]=] PROPERTIES  _BACKTRACE_TRIPLES "G:/Phyriad/projects/Phynned/framework/process/CMakeLists.txt;88;add_test;G:/Phyriad/projects/Phynned/framework/process/CMakeLists.txt;0;")
add_test([=[current_process_test]=] "G:/Phyriad/projects/Phynned/build-relsym/_phyriad_build/process/current_process_test.exe" "LABELS" "process")
set_tests_properties([=[current_process_test]=] PROPERTIES  _BACKTRACE_TRIPLES "G:/Phyriad/projects/Phynned/framework/process/CMakeLists.txt;105;add_test;G:/Phyriad/projects/Phynned/framework/process/CMakeLists.txt;0;")
add_test([=[process_metrics_snapshot_test]=] "G:/Phyriad/projects/Phynned/build-relsym/_phyriad_build/process/process_metrics_snapshot_test.exe" "LABELS" "process")
set_tests_properties([=[process_metrics_snapshot_test]=] PROPERTIES  _BACKTRACE_TRIPLES "G:/Phyriad/projects/Phynned/framework/process/CMakeLists.txt;122;add_test;G:/Phyriad/projects/Phynned/framework/process/CMakeLists.txt;0;")
add_test([=[enumerate_threads_test]=] "G:/Phyriad/projects/Phynned/build-relsym/_phyriad_build/process/enumerate_threads_test.exe" "LABELS" "process")
set_tests_properties([=[enumerate_threads_test]=] PROPERTIES  _BACKTRACE_TRIPLES "G:/Phyriad/projects/Phynned/framework/process/CMakeLists.txt;137;add_test;G:/Phyriad/projects/Phynned/framework/process/CMakeLists.txt;0;")
