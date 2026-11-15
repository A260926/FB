add_executable(test_scene
  test_scene_resources.cpp
)
target_link_libraries(test_scene PRIVATE renderer_test_common)
gtest_discover_tests(test_scene DISCOVERY_TIMEOUT 30
  PROPERTIES LABELS Scene)
