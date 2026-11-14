# Install script for directory: /Users/solo/pro/UserFB/workspace/FS9DAA9DCA24EDU9854/attempts/20261114T073142057223Z/repo/src/animation/offline

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
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/lib" TYPE STATIC_LIBRARY FILES "/Users/solo/pro/UserFB/workspace/FS9DAA9DCA24EDU9854/attempts/20261114T073142057223Z/repo/.task-build-baseline/ozz/src/animation/offline/libozz_animation_offline_r.a")
  if(EXISTS "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/lib/libozz_animation_offline_r.a" AND
     NOT IS_SYMLINK "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/lib/libozz_animation_offline_r.a")
    execute_process(COMMAND "/usr/bin/ranlib" "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/lib/libozz_animation_offline_r.a")
  endif()
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/include/ozz/animation/offline" TYPE FILE FILES
    "/Users/solo/pro/UserFB/workspace/FS9DAA9DCA24EDU9854/attempts/20261114T073142057223Z/repo/include/ozz/animation/offline/export.h"
    "/Users/solo/pro/UserFB/workspace/FS9DAA9DCA24EDU9854/attempts/20261114T073142057223Z/repo/include/ozz/animation/offline/raw_animation.h"
    "/Users/solo/pro/UserFB/workspace/FS9DAA9DCA24EDU9854/attempts/20261114T073142057223Z/repo/include/ozz/animation/offline/raw_animation_utils.h"
    "/Users/solo/pro/UserFB/workspace/FS9DAA9DCA24EDU9854/attempts/20261114T073142057223Z/repo/include/ozz/animation/offline/animation_builder.h"
    "/Users/solo/pro/UserFB/workspace/FS9DAA9DCA24EDU9854/attempts/20261114T073142057223Z/repo/include/ozz/animation/offline/animation_optimizer.h"
    "/Users/solo/pro/UserFB/workspace/FS9DAA9DCA24EDU9854/attempts/20261114T073142057223Z/repo/include/ozz/animation/offline/additive_animation_builder.h"
    "/Users/solo/pro/UserFB/workspace/FS9DAA9DCA24EDU9854/attempts/20261114T073142057223Z/repo/include/ozz/animation/offline/motion_extractor.h"
    "/Users/solo/pro/UserFB/workspace/FS9DAA9DCA24EDU9854/attempts/20261114T073142057223Z/repo/include/ozz/animation/offline/raw_skeleton.h"
    "/Users/solo/pro/UserFB/workspace/FS9DAA9DCA24EDU9854/attempts/20261114T073142057223Z/repo/include/ozz/animation/offline/skeleton_builder.h"
    "/Users/solo/pro/UserFB/workspace/FS9DAA9DCA24EDU9854/attempts/20261114T073142057223Z/repo/include/ozz/animation/offline/skeleton_reducer.h"
    "/Users/solo/pro/UserFB/workspace/FS9DAA9DCA24EDU9854/attempts/20261114T073142057223Z/repo/include/ozz/animation/offline/raw_track.h"
    "/Users/solo/pro/UserFB/workspace/FS9DAA9DCA24EDU9854/attempts/20261114T073142057223Z/repo/include/ozz/animation/offline/raw_track_utils.h"
    "/Users/solo/pro/UserFB/workspace/FS9DAA9DCA24EDU9854/attempts/20261114T073142057223Z/repo/include/ozz/animation/offline/track_builder.h"
    "/Users/solo/pro/UserFB/workspace/FS9DAA9DCA24EDU9854/attempts/20261114T073142057223Z/repo/include/ozz/animation/offline/track_optimizer.h"
    )
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  include("/Users/solo/pro/UserFB/workspace/FS9DAA9DCA24EDU9854/attempts/20261114T073142057223Z/repo/.task-build-baseline/ozz/src/animation/offline/CMakeFiles/ozz_animation_offline.dir/install-cxx-module-bmi-Release.cmake" OPTIONAL)
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("/Users/solo/pro/UserFB/workspace/FS9DAA9DCA24EDU9854/attempts/20261114T073142057223Z/repo/.task-build-baseline/ozz/src/animation/offline/tools/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("/Users/solo/pro/UserFB/workspace/FS9DAA9DCA24EDU9854/attempts/20261114T073142057223Z/repo/.task-build-baseline/ozz/src/animation/offline/fbx/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("/Users/solo/pro/UserFB/workspace/FS9DAA9DCA24EDU9854/attempts/20261114T073142057223Z/repo/.task-build-baseline/ozz/src/animation/offline/gltf/cmake_install.cmake")
endif()

string(REPLACE ";" "\n" CMAKE_INSTALL_MANIFEST_CONTENT
       "${CMAKE_INSTALL_MANIFEST_FILES}")
if(CMAKE_INSTALL_LOCAL_ONLY)
  file(WRITE "/Users/solo/pro/UserFB/workspace/FS9DAA9DCA24EDU9854/attempts/20261114T073142057223Z/repo/.task-build-baseline/ozz/src/animation/offline/install_local_manifest.txt"
     "${CMAKE_INSTALL_MANIFEST_CONTENT}")
endif()
