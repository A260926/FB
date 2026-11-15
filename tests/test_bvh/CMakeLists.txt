add_executable(test_bvh
  test_bvh_intersection.cpp
)
target_link_libraries(test_bvh PRIVATE renderer_test_common)
gtest_discover_tests(test_bvh DISCOVERY_TIMEOUT 30
  PROPERTIES LABELS BVH)
