add_executable(test_sampler
  test_sampler_threading.cpp
)
target_link_libraries(test_sampler PRIVATE renderer_test_common)
gtest_discover_tests(test_sampler DISCOVERY_TIMEOUT 30
  PROPERTIES LABELS Sampler)
