cmake_minimum_required(VERSION 3.16)
project(Software-Rasterizer VERSION 1.0.0 LANGUAGES CXX)
include(CTest)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_BINARY_DIR)
  message(FATAL_ERROR "Use an out-of-source build: cmake -S . -B build")
endif()

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(tbb_default ON)

if(APPLE)
  set(tbb_default OFF)
endif()

option(SOFT_RASTERIZER_ENABLE_TBB "Parallelize tiles and pixel rows using oneTBB" ${tbb_default})
option(SOFT_RASTERIZER_ENABLE_GUI "Build optional OpenCV window support" OFF)
option(SOFT_RASTERIZER_NATIVE_AVX2 "Require native AVX2 (x86 only); portable SIMDe is default" OFF)
option(SOFT_RASTERIZER_SANITIZERS "Enable address and undefined behavior sanitizers" OFF)
option(SOFT_RASTERIZER_BUILD_BENCHMARKS "Build the draw-only raster benchmark" OFF)

# Apple builds use the portable CPU backend, preserving the existing platform policy.
if(APPLE)
  set(SOFT_RASTERIZER_ENABLE_TBB OFF CACHE BOOL "Parallelize using oneTBB" FORCE)
endif()
add_library(tinyobjloader STATIC external/tinyobjloader/tiny_obj_loader.cc)
target_include_directories(tinyobjloader SYSTEM PUBLIC external/tinyobjloader)

add_library(soft_rasterizer STATIC
  src/BVHAcceleration.cpp src/Camera.cpp src/Cube.cpp
  src/Material.cpp src/Mesh.cpp src/Object.cpp src/ObjLoader.cpp
  src/PathTracing.cpp src/Rasterizer.cpp src/Render.cpp src/Scene.cpp
  src/Sphere.cpp src/TextureLoader.cpp src/Triangle.cpp)

target_include_directories(soft_rasterizer PUBLIC include)
target_include_directories(soft_rasterizer SYSTEM PUBLIC external/glm external/simde)
target_include_directories(soft_rasterizer SYSTEM PRIVATE external/tinyobjloader/examples/viewer)

target_link_libraries(soft_rasterizer PRIVATE tinyobjloader)
target_compile_definitions(soft_rasterizer PUBLIC SOFT_RASTERIZER_USE_TBB=$<BOOL:${SOFT_RASTERIZER_ENABLE_TBB}>)

if(MSVC)
  target_compile_options(soft_rasterizer PRIVATE /W4 /utf-8)
  target_compile_definitions(soft_rasterizer PUBLIC NOMINMAX)
else()
  target_compile_options(soft_rasterizer PRIVATE -Wall -Wextra -Wpedantic -ffp-contract=off)
endif()

if(SOFT_RASTERIZER_ENABLE_TBB)
  find_package(TBB QUIET)
  if(NOT TBB_FOUND)
    set(TBB_TEST OFF CACHE BOOL "Build oneTBB tests" FORCE)
    add_subdirectory(external/oneTBB EXCLUDE_FROM_ALL)
  endif()
  target_link_libraries(soft_rasterizer PUBLIC TBB::tbb)
endif()

if(SOFT_RASTERIZER_ENABLE_GUI)
  find_package(OpenCV REQUIRED COMPONENTS core highgui)
  target_compile_definitions(soft_rasterizer PRIVATE SOFT_RASTERIZER_OPENCV=1)
  target_include_directories(soft_rasterizer SYSTEM PRIVATE ${OpenCV_INCLUDE_DIRS})
  target_link_libraries(soft_rasterizer PRIVATE ${OpenCV_LIBS})
endif()

if(SOFT_RASTERIZER_NATIVE_AVX2)
  if(NOT CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64|amd64|i.86")
    message(FATAL_ERROR "Native AVX2 requires an x86 target")
  endif()
  if(MSVC)
    target_compile_options(soft_rasterizer PRIVATE /arch:AVX2)
  else()
    target_compile_options(soft_rasterizer PRIVATE -mavx2)
  endif()
endif()

if(SOFT_RASTERIZER_SANITIZERS)
  if(MSVC)
    message(FATAL_ERROR "This sanitizer configuration requires Clang or GCC")
  endif()
  foreach(target soft_rasterizer tinyobjloader)
    target_compile_options(${target} PUBLIC -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(${target} PUBLIC -fsanitize=address,undefined)
  endforeach()
endif()

add_executable(Software-Rasterizer src/main.cpp)
target_link_libraries(Software-Rasterizer PRIVATE soft_rasterizer)

if(BUILD_TESTING)
  add_subdirectory(tests)
endif()

if(SOFT_RASTERIZER_BUILD_BENCHMARKS)
  add_executable(raster_benchmark benchmarks/raster_benchmark.cpp)
  target_link_libraries(raster_benchmark PRIVATE soft_rasterizer)
  target_compile_definitions(raster_benchmark PRIVATE
    BENCHMARK_ASSET_DIR="${CMAKE_CURRENT_SOURCE_DIR}/examples/models"
    BENCHMARK_BUILD_TYPE="${CMAKE_BUILD_TYPE}"
    BENCHMARK_COMPILER="${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
endif()
