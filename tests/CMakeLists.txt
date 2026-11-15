find_package(Threads REQUIRED)
find_package(GTest CONFIG REQUIRED)
include(GoogleTest)

# Shared test-only helpers, renderer linkage and Google Test's main function.
add_library(renderer_test_common INTERFACE)
target_include_directories(renderer_test_common INTERFACE
  ${CMAKE_CURRENT_SOURCE_DIR}/common)
target_link_libraries(renderer_test_common INTERFACE
  soft_rasterizer Threads::Threads GTest::gtest_main)
target_compile_definitions(renderer_test_common INTERFACE
  TEST_ASSET_DIR="${PROJECT_SOURCE_DIR}/examples/models")

add_subdirectory(test_geometry)
add_subdirectory(test_sphere)
add_subdirectory(test_bvh)
add_subdirectory(test_scene)
add_subdirectory(test_asset_loading)
add_subdirectory(test_rasterizer)
add_subdirectory(test_sampler)
add_subdirectory(test_path_tracing)
add_subdirectory(test_validation)
add_subdirectory(test_camera)

# Keep a convenient build target for all independent test executables.
add_custom_target(renderer_tests)
add_dependencies(renderer_tests
  test_geometry
  test_sphere
  test_bvh
  test_scene
  test_asset_loading
  test_rasterizer
  test_sampler
  test_path_tracing
  test_validation
  test_camera
)
