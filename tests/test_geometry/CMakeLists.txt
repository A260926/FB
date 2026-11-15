add_executable(test_geometry
  test_geometry_transform.cpp
)
target_link_libraries(test_geometry PRIVATE renderer_test_common)
gtest_discover_tests(test_geometry DISCOVERY_TIMEOUT 30
  PROPERTIES LABELS Geometry)
