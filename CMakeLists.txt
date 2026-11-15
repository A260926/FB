cmake_minimum_required(VERSION 3.16)
project(softgpu CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

option(SOFTGPU_ASAN "Build with AddressSanitizer + UBSan" OFF)

find_package(Threads REQUIRED)

add_library(softgpu STATIC
  src/image.cpp
  src/mesh.cpp
  src/scenes.cpp
  src/texture.cpp)
target_include_directories(softgpu PUBLIC include)
target_compile_options(softgpu PUBLIC -Wall -Wextra -Wpedantic)
target_link_libraries(softgpu PUBLIC Threads::Threads)

if(SOFTGPU_ASAN)
  target_compile_options(softgpu PUBLIC -fsanitize=address,undefined -fno-omit-frame-pointer)
  target_link_options(softgpu PUBLIC -fsanitize=address,undefined)
endif()

add_executable(softgpu_render src/main_render.cpp)
add_executable(softgpu_bench src/main_bench.cpp)
foreach(t softgpu_render softgpu_bench)
  target_link_libraries(${t} PRIVATE softgpu)
endforeach()

enable_testing()
add_executable(softgpu_tests
  tests/test_main.cpp
  tests/test_math.cpp
  tests/test_raster.cpp
  tests/test_clip.cpp
  tests/test_interp.cpp
  tests/test_depth.cpp
  tests/test_texture.cpp
  tests/test_tiled.cpp)
target_link_libraries(softgpu_tests PRIVATE softgpu)
add_test(NAME softgpu_tests COMMAND softgpu_tests)
