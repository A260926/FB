# Install script for directory: /Users/solo/pro/UserFB/workspace/FS9DAA9DCA24EDU9854/attempts/20261114T073142057223Z/repo/src/base

# Set the install prefix
if(NOT DEFINED CMAKE_INSTALL_PREFIX)
  set(CMAKE_INSTALL_PREFIX "/usr/local")
endif()
string(REGEX REPLACE "/$" "" CMAKE_INSTALL_PREFIX "${CMAKE_INSTALL_PREFIX}")

# Set the install configuration name.
if(NOT DEFINED CMAKE_INSTALL_CONFIG_NAME)
  if(BUILD_TYPE)
    string(REGEX REPLACE "^[^A-Za-z0-9_]+" ""
           CMAKE_INSTALL_CONFIG_NAME "${BUILD_TYPE}")
  else()
    set(CMAKE_INSTALL_CONFIG_NAME "Release")
  endif()
  message(STATUS "Install configuration: \"${CMAKE_INSTALL_CONFIG_NAME}\"")
endif()

# Set the component getting installed.
if(NOT CMAKE_INSTALL_COMPONENT)
  if(COMPONENT)
    message(STATUS "Install component: \"${COMPONENT}\"")
    set(CMAKE_INSTALL_COMPONENT "${COMPONENT}")
  else()
    set(CMAKE_INSTALL_COMPONENT)
  endif()
endif()

# Is this installation the result of a crosscompile?
if(NOT DEFINED CMAKE_CROSSCOMPILING)
  set(CMAKE_CROSSCOMPILING "FALSE")
endif()

# Set path to fallback-tool for dependency-resolution.
if(NOT DEFINED CMAKE_OBJDUMP)
  set(CMAKE_OBJDUMP "/usr/bin/objdump")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/lib" TYPE STATIC_LIBRARY FILES "/Users/solo/pro/UserFB/workspace/FS9DAA9DCA24EDU9854/attempts/20261114T073142057223Z/repo/.task-build-target/ozz/src/base/libozz_base_r.a")
  if(EXISTS "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/lib/libozz_base_r.a" AND
     NOT IS_SYMLINK "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/lib/libozz_base_r.a")
    execute_process(COMMAND "/usr/bin/ranlib" "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/lib/libozz_base_r.a")
  endif()
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/include/ozz/base" TYPE FILE FILES
    "/Users/solo/pro/UserFB/workspace/FS9DAA9DCA24EDU9854/attempts/20261114T073142057223Z/repo/include/ozz/base/export.h"
    "/Users/solo/pro/UserFB/workspace/FS9DAA9DCA24EDU9854/attempts/20261114T073142057223Z/repo/include/ozz/base/endianness.h"
    "/Users/solo/pro/UserFB/workspace/FS9DAA9DCA24EDU9854/attempts/20261114T073142057223Z/repo/include/ozz/base/gtest_helper.h"
    "/Users/solo/pro/UserFB/workspace/FS9DAA9DCA24EDU9854/attempts/20261114T073142057223Z/repo/include/ozz/base/platform.h"
    "/Users/solo/pro/UserFB/workspace/FS9DAA9DCA24EDU9854/attempts/20261114T073142057223Z/repo/include/ozz/base/span.h"
    "/Users/solo/pro/UserFB/workspace/FS9DAA9DCA24EDU9854/attempts/20261114T073142057223Z/repo/include/ozz/base/log.h"
    )
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/include/ozz/base/containers" TYPE FILE FILES
    "/Users/solo/pro/UserFB/workspace/FS9DAA9DCA24EDU9854/attempts/20261114T073142057223Z/repo/include/ozz/base/containers/intrusive_list.h"
    "/Users/solo/pro/UserFB/workspace/FS9DAA9DCA24EDU9854/attempts/20261114T073142057223Z/repo/include/ozz/base/containers/deque.h"
    "/Users/solo/pro/UserFB/workspace/FS9DAA9DCA24EDU9854/attempts/20261114T073142057223Z/repo/include/ozz/base/containers/list.h"
    "/Users/solo/pro/UserFB/workspace/FS9DAA9DCA24EDU9854/attempts/20261114T073142057223Z/repo/include/ozz/base/containers/map.h"
    "/Users/solo/pro/UserFB/workspace/FS9DAA9DCA24EDU9854/attempts/20261114T073142057223Z/repo/include/ozz/base/containers/queue.h"
    "/Users/solo/pro/UserFB/workspace/FS9DAA9DCA24EDU9854/attempts/20261114T073142057223Z/repo/include/ozz/base/containers/set.h"
    "/Users/solo/pro/UserFB/workspace/FS9DAA9DCA24EDU9854/attempts/20261114T073142057223Z/repo/include/ozz/base/containers/stack.h"
    "/Users/solo/pro/UserFB/workspace/FS9DAA9DCA24EDU9854/attempts/20261114T073142057223Z/repo/include/ozz/base/containers/string.h"
    "/Users/solo/pro/UserFB/workspace/FS9DAA9DCA24EDU9854/attempts/20261114T073142057223Z/repo/include/ozz/base/containers/string_archive.h"
    "/Users/solo/pro/UserFB/workspace/FS9DAA9DCA24EDU9854/attempts/20261114T073142057223Z/repo/include/ozz/base/containers/unordered_map.h"
    "/Users/solo/pro/UserFB/workspace/FS9DAA9DCA24EDU9854/attempts/20261114T073142057223Z/repo/include/ozz/base/containers/unordered_set.h"
    "/Users/solo/pro/UserFB/workspace/FS9DAA9DCA24EDU9854/attempts/20261114T073142057223Z/repo/include/ozz/base/containers/vector.h"
    "/Users/solo/pro/UserFB/workspace/FS9DAA9DCA24EDU9854/attempts/20261114T073142057223Z/repo/include/ozz/base/containers/vector_archive.h"
    "/Users/solo/pro/UserFB/workspace/FS9DAA9DCA24EDU9854/attempts/20261114T073142057223Z/repo/include/ozz/base/containers/std_allocator.h"
    )
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/include/ozz/base/encode" TYPE FILE FILES "/Users/solo/pro/UserFB/workspace/FS9DAA9DCA24EDU9854/attempts/20261114T073142057223Z/repo/include/ozz/base/encode/group_varint.h")
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/include/ozz/base/io" TYPE FILE FILES
    "/Users/solo/pro/UserFB/workspace/FS9DAA9DCA24EDU9854/attempts/20261114T073142057223Z/repo/include/ozz/base/io/archive.h"
    "/Users/solo/pro/UserFB/workspace/FS9DAA9DCA24EDU9854/attempts/20261114T073142057223Z/repo/include/ozz/base/io/archive_traits.h"
    "/Users/solo/pro/UserFB/workspace/FS9DAA9DCA24EDU9854/attempts/20261114T073142057223Z/repo/include/ozz/base/io/stream.h"
    )
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/include/ozz/base/maths" TYPE FILE FILES
    "/Users/solo/pro/UserFB/workspace/FS9DAA9DCA24EDU9854/attempts/20261114T073142057223Z/repo/include/ozz/base/maths/box.h"
    "/Users/solo/pro/UserFB/workspace/FS9DAA9DCA24EDU9854/attempts/20261114T073142057223Z/repo/include/ozz/base/maths/gtest_math_helper.h"
    "/Users/solo/pro/UserFB/workspace/FS9DAA9DCA24EDU9854/attempts/20261114T073142057223Z/repo/include/ozz/base/maths/math_ex.h"
    "/Users/solo/pro/UserFB/workspace/FS9DAA9DCA24EDU9854/attempts/20261114T073142057223Z/repo/include/ozz/base/maths/math_constant.h"
    "/Users/solo/pro/UserFB/workspace/FS9DAA9DCA24EDU9854/attempts/20261114T073142057223Z/repo/include/ozz/base/maths/quaternion.h"
    "/Users/solo/pro/UserFB/workspace/FS9DAA9DCA24EDU9854/attempts/20261114T073142057223Z/repo/include/ozz/base/maths/rect.h"
    "/Users/solo/pro/UserFB/workspace/FS9DAA9DCA24EDU9854/attempts/20261114T073142057223Z/repo/include/ozz/base/maths/simd_math.h"
    "/Users/solo/pro/UserFB/workspace/FS9DAA9DCA24EDU9854/attempts/20261114T073142057223Z/repo/include/ozz/base/maths/simd_quaternion.h"
    "/Users/solo/pro/UserFB/workspace/FS9DAA9DCA24EDU9854/attempts/20261114T073142057223Z/repo/include/ozz/base/maths/soa_float.h"
    "/Users/solo/pro/UserFB/workspace/FS9DAA9DCA24EDU9854/attempts/20261114T073142057223Z/repo/include/ozz/base/maths/soa_quaternion.h"
    "/Users/solo/pro/UserFB/workspace/FS9DAA9DCA24EDU9854/attempts/20261114T073142057223Z/repo/include/ozz/base/maths/soa_transform.h"
    "/Users/solo/pro/UserFB/workspace/FS9DAA9DCA24EDU9854/attempts/20261114T073142057223Z/repo/include/ozz/base/maths/soa_float4x4.h"
    "/Users/solo/pro/UserFB/workspace/FS9DAA9DCA24EDU9854/attempts/20261114T073142057223Z/repo/include/ozz/base/maths/transform.h"
    "/Users/solo/pro/UserFB/workspace/FS9DAA9DCA24EDU9854/attempts/20261114T073142057223Z/repo/include/ozz/base/maths/vec_float.h"
    "/Users/solo/pro/UserFB/workspace/FS9DAA9DCA24EDU9854/attempts/20261114T073142057223Z/repo/include/ozz/base/maths/math_archive.h"
    "/Users/solo/pro/UserFB/workspace/FS9DAA9DCA24EDU9854/attempts/20261114T073142057223Z/repo/include/ozz/base/maths/soa_math_archive.h"
    "/Users/solo/pro/UserFB/workspace/FS9DAA9DCA24EDU9854/attempts/20261114T073142057223Z/repo/include/ozz/base/maths/simd_math_archive.h"
    )
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/include/ozz/base/maths/internal" TYPE FILE FILES
    "/Users/solo/pro/UserFB/workspace/FS9DAA9DCA24EDU9854/attempts/20261114T073142057223Z/repo/include/ozz/base/maths/internal/simd_math_config.h"
    "/Users/solo/pro/UserFB/workspace/FS9DAA9DCA24EDU9854/attempts/20261114T073142057223Z/repo/include/ozz/base/maths/internal/simd_math_ref-inl.h"
    "/Users/solo/pro/UserFB/workspace/FS9DAA9DCA24EDU9854/attempts/20261114T073142057223Z/repo/include/ozz/base/maths/internal/simd_math_sse-inl.h"
    )
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/include/ozz/base/memory" TYPE FILE FILES
    "/Users/solo/pro/UserFB/workspace/FS9DAA9DCA24EDU9854/attempts/20261114T073142057223Z/repo/include/ozz/base/memory/allocator.h"
    "/Users/solo/pro/UserFB/workspace/FS9DAA9DCA24EDU9854/attempts/20261114T073142057223Z/repo/include/ozz/base/memory/unique_ptr.h"
    )
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  include("/Users/solo/pro/UserFB/workspace/FS9DAA9DCA24EDU9854/attempts/20261114T073142057223Z/repo/.task-build-target/ozz/src/base/CMakeFiles/ozz_base.dir/install-cxx-module-bmi-Release.cmake" OPTIONAL)
endif()

string(REPLACE ";" "\n" CMAKE_INSTALL_MANIFEST_CONTENT
       "${CMAKE_INSTALL_MANIFEST_FILES}")
if(CMAKE_INSTALL_LOCAL_ONLY)
  file(WRITE "/Users/solo/pro/UserFB/workspace/FS9DAA9DCA24EDU9854/attempts/20261114T073142057223Z/repo/.task-build-target/ozz/src/base/install_local_manifest.txt"
     "${CMAKE_INSTALL_MANIFEST_CONTENT}")
endif()
