# CMake generated Testfile for 
# Source directory: /Users/solo/pro/UserFB/workspace/FS9EAD0D64B3DBU9861/attempts/20261117T161610210424Z/repo
# Build directory: /Users/solo/pro/UserFB/workspace/FS9EAD0D64B3DBU9861/attempts/20261117T161610210424Z/repo/.task-build/fresh
# 
# This file includes the relevant testing commands required for 
# testing this directory and lists subdirectories to be tested as well.
add_test("shaders" "/Users/solo/pro/UserFB/workspace/FS9EAD0D64B3DBU9861/attempts/20261117T161610210424Z/repo/.task-build/fresh/glsl_optimizer_tests" "/Users/solo/pro/UserFB/workspace/FS9EAD0D64B3DBU9861/attempts/20261117T161610210424Z/repo/tests")
set_tests_properties("shaders" PROPERTIES  WORKING_DIRECTORY "/Users/solo/pro/UserFB/workspace/FS9EAD0D64B3DBU9861/attempts/20261117T161610210424Z/repo/.task-build/fresh" _BACKTRACE_TRIPLES "/Users/solo/pro/UserFB/workspace/FS9EAD0D64B3DBU9861/attempts/20261117T161610210424Z/repo/CMakeLists.txt;239;add_test;/Users/solo/pro/UserFB/workspace/FS9EAD0D64B3DBU9861/attempts/20261117T161610210424Z/repo/CMakeLists.txt;0;")
add_test("test_runner" "/opt/homebrew/bin/cmake" "-DTEST_RUNNER=/Users/solo/pro/UserFB/workspace/FS9EAD0D64B3DBU9861/attempts/20261117T161610210424Z/repo/.task-build/fresh/glsl_optimizer_tests" "-DTEST_SOURCE_DIR=/Users/solo/pro/UserFB/workspace/FS9EAD0D64B3DBU9861/attempts/20261117T161610210424Z/repo/tests" "-P" "/Users/solo/pro/UserFB/workspace/FS9EAD0D64B3DBU9861/attempts/20261117T161610210424Z/repo/tests/test_runner.cmake")
set_tests_properties("test_runner" PROPERTIES  _BACKTRACE_TRIPLES "/Users/solo/pro/UserFB/workspace/FS9EAD0D64B3DBU9861/attempts/20261117T161610210424Z/repo/CMakeLists.txt;241;add_test;/Users/solo/pro/UserFB/workspace/FS9EAD0D64B3DBU9861/attempts/20261117T161610210424Z/repo/CMakeLists.txt;0;")
