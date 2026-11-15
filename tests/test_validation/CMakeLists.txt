add_executable(test_validation
  test_validation_input.cpp
)
target_link_libraries(test_validation PRIVATE renderer_test_common)
gtest_discover_tests(test_validation DISCOVERY_TIMEOUT 30
  PROPERTIES LABELS Validation)
