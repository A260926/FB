add_executable(test_camera
  test_camera_projection.cpp
)
target_link_libraries(test_camera PRIVATE renderer_test_common)
gtest_discover_tests(test_camera DISCOVERY_TIMEOUT 30
  PROPERTIES LABELS Camera)
