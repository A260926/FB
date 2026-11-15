add_executable(test_sphere
  test_sphere_sampling.cpp
)
target_link_libraries(test_sphere PRIVATE renderer_test_common)
gtest_discover_tests(test_sphere DISCOVERY_TIMEOUT 30
  PROPERTIES LABELS Sphere)
