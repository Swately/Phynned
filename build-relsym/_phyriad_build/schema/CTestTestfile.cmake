# CMake generated Testfile for 
# Source directory: G:/Phyriad/projects/Phynned/framework/schema
# Build directory: G:/Phyriad/projects/Phynned/build-relsym/_phyriad_build/schema
# 
# This file includes the relevant testing commands required for 
# testing this directory and lists subdirectories to be tested as well.
add_test([=[schema_types_test]=] "G:/Phyriad/projects/Phynned/build-relsym/_phyriad_build/schema/schema_types_test.exe" "LABELS" "schema")
set_tests_properties([=[schema_types_test]=] PROPERTIES  _BACKTRACE_TRIPLES "G:/Phyriad/projects/Phynned/framework/schema/CMakeLists.txt;51;add_test;G:/Phyriad/projects/Phynned/framework/schema/CMakeLists.txt;0;")
add_test([=[blind_proxy_test]=] "G:/Phyriad/projects/Phynned/build-relsym/_phyriad_build/schema/blind_proxy_test.exe" "LABELS" "schema")
set_tests_properties([=[blind_proxy_test]=] PROPERTIES  _BACKTRACE_TRIPLES "G:/Phyriad/projects/Phynned/framework/schema/CMakeLists.txt;64;add_test;G:/Phyriad/projects/Phynned/framework/schema/CMakeLists.txt;0;")
add_test([=[schema_hash_conformance_test]=] "G:/Phyriad/projects/Phynned/build-relsym/_phyriad_build/schema/schema_hash_conformance_test.exe" "LABELS" "schema")
set_tests_properties([=[schema_hash_conformance_test]=] PROPERTIES  _BACKTRACE_TRIPLES "G:/Phyriad/projects/Phynned/framework/schema/CMakeLists.txt;81;add_test;G:/Phyriad/projects/Phynned/framework/schema/CMakeLists.txt;0;")
