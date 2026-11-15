add_executable(test_path_tracing
  test_path_tracing_emission.cpp
  test_path_tracing_point_light.cpp
  test_path_tracing_area_light.cpp
  test_path_tracing_materials.cpp
  test_path_tracing_diffuse_sampling.cpp
  test_path_tracing_indirect_environment.cpp
  test_path_tracing_mirror_sample.cpp
  test_path_tracing_fresnel_sampling.cpp
  test_path_tracing_total_internal_reflection.cpp
  test_path_tracing_equal_ior.cpp
)
target_link_libraries(test_path_tracing PRIVATE renderer_test_common)
gtest_discover_tests(test_path_tracing DISCOVERY_TIMEOUT 30
  PROPERTIES LABELS PathTracing)
