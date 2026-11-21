# CMake generated Testfile for 
# Source directory: /Volumes/UserFBWorkspace/questions/FSC4E0CBFBA75AU14179/attempts/20261121T181124651953Z/repo
# Build directory: /Volumes/UserFBWorkspace/questions/FSC4E0CBFBA75AU14179/attempts/20261121T181124651953Z/repo/.task-build/engine
# 
# This file includes the relevant testing commands required for 
# testing this directory and lists subdirectories to be tested as well.
add_test(pbrt_unit_test "pbrt_test")
set_tests_properties(pbrt_unit_test PROPERTIES  _BACKTRACE_TRIPLES "/Volumes/UserFBWorkspace/questions/FSC4E0CBFBA75AU14179/attempts/20261121T181124651953Z/repo/CMakeLists.txt;564;ADD_TEST;/Volumes/UserFBWorkspace/questions/FSC4E0CBFBA75AU14179/attempts/20261121T181124651953Z/repo/CMakeLists.txt;0;")
subdirs("src/ext/zlib")
subdirs("src/ext/openexr")
subdirs("src/ext/glog")
subdirs("src/ext/ptex")
