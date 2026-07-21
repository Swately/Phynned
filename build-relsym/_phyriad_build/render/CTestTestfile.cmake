# CMake generated Testfile for 
# Source directory: G:/Phyriad/projects/Phynned/framework/render
# Build directory: G:/Phyriad/projects/Phynned/build-relsym/_phyriad_build/render
# 
# This file includes the relevant testing commands required for 
# testing this directory and lists subdirectories to be tested as well.
add_test([=[render_test]=] "G:/Phyriad/projects/Phynned/build-relsym/_phyriad_build/render/render_test.exe")
set_tests_properties([=[render_test]=] PROPERTIES  _BACKTRACE_TRIPLES "G:/Phyriad/projects/Phynned/framework/render/CMakeLists.txt;106;add_test;G:/Phyriad/projects/Phynned/framework/render/CMakeLists.txt;0;")
subdirs("opengl3")
subdirs("composite")
