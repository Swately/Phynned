# CMake generated Testfile for 
# Source directory: G:/Phyriad/projects/Phynned/framework/ipc
# Build directory: G:/Phyriad/projects/Phynned/build-relsym/_phyriad_build/ipc
# 
# This file includes the relevant testing commands required for 
# testing this directory and lists subdirectories to be tested as well.
add_test([=[shm_region_test]=] "G:/Phyriad/projects/Phynned/build-relsym/_phyriad_build/ipc/shm_region_test.exe" "LABELS" "ipc")
set_tests_properties([=[shm_region_test]=] PROPERTIES  _BACKTRACE_TRIPLES "G:/Phyriad/projects/Phynned/framework/ipc/CMakeLists.txt;75;add_test;G:/Phyriad/projects/Phynned/framework/ipc/CMakeLists.txt;0;")
add_test([=[ring_test]=] "G:/Phyriad/projects/Phynned/build-relsym/_phyriad_build/ipc/ring_test.exe" "LABELS" "ipc")
set_tests_properties([=[ring_test]=] PROPERTIES  _BACKTRACE_TRIPLES "G:/Phyriad/projects/Phynned/framework/ipc/CMakeLists.txt;93;add_test;G:/Phyriad/projects/Phynned/framework/ipc/CMakeLists.txt;0;")
