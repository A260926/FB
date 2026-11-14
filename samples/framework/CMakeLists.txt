# Framework library
add_library(sample_framework STATIC
  application.h
  application.cc
  imgui.h
  image.h
  image.cc
  mesh.h
  mesh.cc
  motion_utils.h
  motion_utils.cc
  profile.h
  profile.cc
  renderer.h
  utils.h
  utils.cc
  internal/camera.h
  internal/camera.cc
  internal/icosphere.h
  internal/immediate.h
  internal/immediate.cc
  internal/imgui_impl.h
  internal/imgui_impl.cc
  internal/renderer_impl.h
  internal/renderer_impl.cc
  internal/shader.h
  internal/shader.cc
  internal/shooter.h
  internal/shooter.cc)

# Samples requires OpenGL package.
if(NOT EMSCRIPTEN)
  find_package(OpenGL REQUIRED)
  set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
  set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
  set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
  set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)
  set(GLFW_BUILD_WAYLAND OFF CACHE BOOL "" FORCE)
  add_subdirectory(${PROJECT_SOURCE_DIR}/extern/glfw glfw3)
  target_link_libraries(sample_framework glfw OpenGL::GL)
  target_compile_definitions(sample_framework PRIVATE GL_SILENCE_DEPRECATION)

  # Keep extern/glfw include path to access bundled glext.h extension headers.
  target_include_directories(sample_framework PRIVATE ${PROJECT_SOURCE_DIR}/extern/glfw/include)
else()
  target_compile_options(sample_framework PUBLIC "--use-port=contrib.glfw3")
  target_link_options(sample_framework PUBLIC "--use-port=contrib.glfw3")
  target_link_options(sample_framework PUBLIC -sFULL_ES3=1 -sASSERTIONS=1)
endif()

target_link_libraries(sample_framework
  ozz_geometry
  ozz_animation_offline
  ozz_options)
  
target_include_directories(sample_framework PUBLIC
  $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/samples>
  $<INSTALL_INTERFACE:$<INSTALL_PREFIX>/samples>)

if(TARGET BUILD_DATA_SAMPLE)
  add_dependencies(sample_framework BUILD_DATA_SAMPLE)
endif()

set_target_properties(sample_framework PROPERTIES FOLDER "samples")

add_subdirectory(tools)
