add_executable(test_rasterizer
  test_rasterizer_perspective.cpp
  test_rasterizer_scalar_simd_fill.cpp
  test_rasterizer_wireframe_coverage.cpp
  test_rasterizer_scalar_simd_wireframe.cpp
  test_rasterizer_near_plane.cpp
  test_rasterizer_behind_camera.cpp
  test_rasterizer_backface_culling.cpp
  test_rasterizer_two_sided.cpp
  test_rasterizer_spanning_triangle.cpp
  test_rasterizer_spanning_wireframe.cpp
  test_rasterizer_camera_plane.cpp
  test_rasterizer_camera_plane_crossing.cpp
)
target_link_libraries(test_rasterizer PRIVATE renderer_test_common)
gtest_discover_tests(test_rasterizer DISCOVERY_TIMEOUT 30
  PROPERTIES LABELS Rasterizer)
