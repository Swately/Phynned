# CMake generated Testfile for 
# Source directory: G:/Phyriad/projects/Phynned/framework/topology
# Build directory: G:/Phyriad/projects/Phynned/build-relsym/_phyriad_build/topology
# 
# This file includes the relevant testing commands required for 
# testing this directory and lists subdirectories to be tested as well.
add_test([=[topology_test]=] "G:/Phyriad/projects/Phynned/build-relsym/_phyriad_build/topology/topology_test.exe" "LABELS" "topology")
set_tests_properties([=[topology_test]=] PROPERTIES  _BACKTRACE_TRIPLES "G:/Phyriad/projects/Phynned/framework/topology/CMakeLists.txt;98;add_test;G:/Phyriad/projects/Phynned/framework/topology/CMakeLists.txt;0;")
add_test([=[thread_affinity_test]=] "G:/Phyriad/projects/Phynned/build-relsym/_phyriad_build/topology/thread_affinity_test.exe" "LABELS" "topology")
set_tests_properties([=[thread_affinity_test]=] PROPERTIES  _BACKTRACE_TRIPLES "G:/Phyriad/projects/Phynned/framework/topology/CMakeLists.txt;114;add_test;G:/Phyriad/projects/Phynned/framework/topology/CMakeLists.txt;0;")
add_test([=[core_telemetry_test]=] "G:/Phyriad/projects/Phynned/build-relsym/_phyriad_build/topology/core_telemetry_test.exe" "LABELS" "topology")
set_tests_properties([=[core_telemetry_test]=] PROPERTIES  _BACKTRACE_TRIPLES "G:/Phyriad/projects/Phynned/framework/topology/CMakeLists.txt;132;add_test;G:/Phyriad/projects/Phynned/framework/topology/CMakeLists.txt;0;")
