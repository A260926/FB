cmake_minimum_required(VERSION 3.5)
project(tinynurbs LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} "${CMAKE_CURRENT_SOURCE_DIR}/cmake/modules/")

set(BUILD_TESTS ON CACHE BOOL "Build unit tests")
set(GLM_ROOT_DIR "" CACHE STRING "Root directory of GLM (>=0.9.9)")
set(TINYNURBS_USE_OWN_GLM ON CACHE BOOL "Use own GLM library from submodule")
message(STATUS "Variable from cache: ${GLM_ROOT_DIR}")

if(NOT TINYNURBS_USE_OWN_GLM)
    find_package(GLM 0.9.9 REQUIRED MODULE)
endif()

set(HEADER_FILES
    include/tinynurbs/tinynurbs.h
    include/tinynurbs/core/basis.h
    include/tinynurbs/core/check.h
    include/tinynurbs/core/curve.h
    include/tinynurbs/core/evaluate.h
    include/tinynurbs/core/modify.h
    include/tinynurbs/core/patch_editor.h
    include/tinynurbs/core/patch_mesh.h
    include/tinynurbs/core/surface.h
    include/tinynurbs/io/obj.h
    include/tinynurbs/util/util.h
    include/tinynurbs/util/array2.h
)
source_group("Header Files" FILES ${HEADER_FILES})
source_group("CMake Files" FILES CMakeLists.txt)

# PatchEditor is not header-only; it is compiled into a small library.
add_library(tinynurbs_patch_editor STATIC src/patch_editor.cpp src/patch_mesh.cpp)
target_link_libraries(tinynurbs_patch_editor PUBLIC tinynurbs::tinynurbs)
target_include_directories(tinynurbs_patch_editor PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_compile_features(tinynurbs_patch_editor PUBLIC cxx_std_17)

add_library(tinynurbs INTERFACE)
add_library(tinynurbs::tinynurbs ALIAS tinynurbs )
if(NOT TINYNURBS_USE_OWN_GLM)
    target_link_libraries(tinynurbs INTERFACE glm)
else()
    set(GLM_INCLUDE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/dependencies/glm)
    target_include_directories(tinynurbs INTERFACE $<BUILD_INTERFACE:${GLM_INCLUDE_DIRS}>)
endif()
target_include_directories(tinynurbs INTERFACE $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)

add_custom_target(tinynurbs_dummy SOURCES ${HEADER_FILES} CMakeLists.txt)

if(BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

add_executable(patch_editor_example examples/patch_editor.cpp)
target_link_libraries(patch_editor_example PRIVATE tinynurbs_patch_editor)
