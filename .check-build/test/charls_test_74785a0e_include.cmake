if(EXISTS "/Volumes/UserFBWorkspace/questions/FSFE895A4B656CU6060/attempts/20261127T160801599810Z/repo/.check-build/test/charls-test")
  if(NOT EXISTS "/Volumes/UserFBWorkspace/questions/FSFE895A4B656CU6060/attempts/20261127T160801599810Z/repo/.check-build/test/charls_test_74785a0e_tests.cmake" OR
     NOT "/Volumes/UserFBWorkspace/questions/FSFE895A4B656CU6060/attempts/20261127T160801599810Z/repo/.check-build/test/charls_test_74785a0e_tests.cmake" IS_NEWER_THAN "/Volumes/UserFBWorkspace/questions/FSFE895A4B656CU6060/attempts/20261127T160801599810Z/repo/.check-build/test/charls-test" OR
     NOT "/Volumes/UserFBWorkspace/questions/FSFE895A4B656CU6060/attempts/20261127T160801599810Z/repo/.check-build/test/charls_test_74785a0e_tests.cmake" IS_NEWER_THAN "${CMAKE_CURRENT_LIST_FILE}")
    include("/Volumes/UserFBWorkspace/questions/FSFE895A4B656CU6060/attempts/20261127T160801599810Z/repo/.check-build/test/charls_test_74785a0e_discovery.cmake")
  endif()
endif()
if(EXISTS "/Volumes/UserFBWorkspace/questions/FSFE895A4B656CU6060/attempts/20261127T160801599810Z/repo/.check-build/test/charls_test_74785a0e_tests.cmake")
  include("/Volumes/UserFBWorkspace/questions/FSFE895A4B656CU6060/attempts/20261127T160801599810Z/repo/.check-build/test/charls_test_74785a0e_tests.cmake")
else()
  add_test(charls-test_NOT_BUILT charls-test_NOT_BUILT)
endif()
