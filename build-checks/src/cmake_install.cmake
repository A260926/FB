# Install script for directory: /Volumes/UserFBWorkspace/questions/FS3739AB23DC8DU6610R8A9CD2F70/attempts/20261123T165238360736Z/repo/src

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
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/lib" TYPE STATIC_LIBRARY FILES "/Volumes/UserFBWorkspace/questions/FS3739AB23DC8DU6610R8A9CD2F70/attempts/20261123T165238360736Z/repo/build-checks/src/libfoonathan_memory-0.7.4.a")
  if(EXISTS "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/lib/libfoonathan_memory-0.7.4.a" AND
     NOT IS_SYMLINK "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/lib/libfoonathan_memory-0.7.4.a")
    execute_process(COMMAND "/usr/bin/ranlib" "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/lib/libfoonathan_memory-0.7.4.a")
  endif()
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/include/foonathan/memory" TYPE FILE FILES "/Volumes/UserFBWorkspace/questions/FS3739AB23DC8DU6610R8A9CD2F70/attempts/20261123T165238360736Z/repo/build-checks/src/config_impl.hpp")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/include/foonathan/memory/detail" TYPE FILE FILES "/Volumes/UserFBWorkspace/questions/FS3739AB23DC8DU6610R8A9CD2F70/attempts/20261123T165238360736Z/repo/build-checks/src/container_node_sizes_impl.hpp")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/include/foonathan/memory" TYPE FILE FILES
    "/Volumes/UserFBWorkspace/questions/FS3739AB23DC8DU6610R8A9CD2F70/attempts/20261123T165238360736Z/repo/include/foonathan/memory/aligned_allocator.hpp"
    "/Volumes/UserFBWorkspace/questions/FS3739AB23DC8DU6610R8A9CD2F70/attempts/20261123T165238360736Z/repo/include/foonathan/memory/allocator_storage.hpp"
    "/Volumes/UserFBWorkspace/questions/FS3739AB23DC8DU6610R8A9CD2F70/attempts/20261123T165238360736Z/repo/include/foonathan/memory/allocator_traits.hpp"
    "/Volumes/UserFBWorkspace/questions/FS3739AB23DC8DU6610R8A9CD2F70/attempts/20261123T165238360736Z/repo/include/foonathan/memory/config.hpp"
    "/Volumes/UserFBWorkspace/questions/FS3739AB23DC8DU6610R8A9CD2F70/attempts/20261123T165238360736Z/repo/include/foonathan/memory/container.hpp"
    "/Volumes/UserFBWorkspace/questions/FS3739AB23DC8DU6610R8A9CD2F70/attempts/20261123T165238360736Z/repo/include/foonathan/memory/debugging.hpp"
    "/Volumes/UserFBWorkspace/questions/FS3739AB23DC8DU6610R8A9CD2F70/attempts/20261123T165238360736Z/repo/include/foonathan/memory/default_allocator.hpp"
    "/Volumes/UserFBWorkspace/questions/FS3739AB23DC8DU6610R8A9CD2F70/attempts/20261123T165238360736Z/repo/include/foonathan/memory/deleter.hpp"
    "/Volumes/UserFBWorkspace/questions/FS3739AB23DC8DU6610R8A9CD2F70/attempts/20261123T165238360736Z/repo/include/foonathan/memory/error.hpp"
    "/Volumes/UserFBWorkspace/questions/FS3739AB23DC8DU6610R8A9CD2F70/attempts/20261123T165238360736Z/repo/include/foonathan/memory/fallback_allocator.hpp"
    "/Volumes/UserFBWorkspace/questions/FS3739AB23DC8DU6610R8A9CD2F70/attempts/20261123T165238360736Z/repo/include/foonathan/memory/malloc_allocator.hpp"
    "/Volumes/UserFBWorkspace/questions/FS3739AB23DC8DU6610R8A9CD2F70/attempts/20261123T165238360736Z/repo/include/foonathan/memory/heap_allocator.hpp"
    "/Volumes/UserFBWorkspace/questions/FS3739AB23DC8DU6610R8A9CD2F70/attempts/20261123T165238360736Z/repo/include/foonathan/memory/iteration_allocator.hpp"
    "/Volumes/UserFBWorkspace/questions/FS3739AB23DC8DU6610R8A9CD2F70/attempts/20261123T165238360736Z/repo/include/foonathan/memory/joint_allocator.hpp"
    "/Volumes/UserFBWorkspace/questions/FS3739AB23DC8DU6610R8A9CD2F70/attempts/20261123T165238360736Z/repo/include/foonathan/memory/memory_arena.hpp"
    "/Volumes/UserFBWorkspace/questions/FS3739AB23DC8DU6610R8A9CD2F70/attempts/20261123T165238360736Z/repo/include/foonathan/memory/memory_pool.hpp"
    "/Volumes/UserFBWorkspace/questions/FS3739AB23DC8DU6610R8A9CD2F70/attempts/20261123T165238360736Z/repo/include/foonathan/memory/memory_pool_collection.hpp"
    "/Volumes/UserFBWorkspace/questions/FS3739AB23DC8DU6610R8A9CD2F70/attempts/20261123T165238360736Z/repo/include/foonathan/memory/memory_pool_type.hpp"
    "/Volumes/UserFBWorkspace/questions/FS3739AB23DC8DU6610R8A9CD2F70/attempts/20261123T165238360736Z/repo/include/foonathan/memory/memory_resource_adapter.hpp"
    "/Volumes/UserFBWorkspace/questions/FS3739AB23DC8DU6610R8A9CD2F70/attempts/20261123T165238360736Z/repo/include/foonathan/memory/memory_stack.hpp"
    "/Volumes/UserFBWorkspace/questions/FS3739AB23DC8DU6610R8A9CD2F70/attempts/20261123T165238360736Z/repo/include/foonathan/memory/namespace_alias.hpp"
    "/Volumes/UserFBWorkspace/questions/FS3739AB23DC8DU6610R8A9CD2F70/attempts/20261123T165238360736Z/repo/include/foonathan/memory/new_allocator.hpp"
    "/Volumes/UserFBWorkspace/questions/FS3739AB23DC8DU6610R8A9CD2F70/attempts/20261123T165238360736Z/repo/include/foonathan/memory/segregator.hpp"
    "/Volumes/UserFBWorkspace/questions/FS3739AB23DC8DU6610R8A9CD2F70/attempts/20261123T165238360736Z/repo/include/foonathan/memory/smart_ptr.hpp"
    "/Volumes/UserFBWorkspace/questions/FS3739AB23DC8DU6610R8A9CD2F70/attempts/20261123T165238360736Z/repo/include/foonathan/memory/static_allocator.hpp"
    "/Volumes/UserFBWorkspace/questions/FS3739AB23DC8DU6610R8A9CD2F70/attempts/20261123T165238360736Z/repo/include/foonathan/memory/std_allocator.hpp"
    "/Volumes/UserFBWorkspace/questions/FS3739AB23DC8DU6610R8A9CD2F70/attempts/20261123T165238360736Z/repo/include/foonathan/memory/temporary_allocator.hpp"
    "/Volumes/UserFBWorkspace/questions/FS3739AB23DC8DU6610R8A9CD2F70/attempts/20261123T165238360736Z/repo/include/foonathan/memory/threading.hpp"
    "/Volumes/UserFBWorkspace/questions/FS3739AB23DC8DU6610R8A9CD2F70/attempts/20261123T165238360736Z/repo/include/foonathan/memory/tracking.hpp"
    "/Volumes/UserFBWorkspace/questions/FS3739AB23DC8DU6610R8A9CD2F70/attempts/20261123T165238360736Z/repo/include/foonathan/memory/virtual_memory.hpp"
    "/Volumes/UserFBWorkspace/questions/FS3739AB23DC8DU6610R8A9CD2F70/attempts/20261123T165238360736Z/repo/build-checks/src/container_node_sizes_impl.hpp"
    )
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/include/foonathan/memory/detail" TYPE FILE FILES
    "/Volumes/UserFBWorkspace/questions/FS3739AB23DC8DU6610R8A9CD2F70/attempts/20261123T165238360736Z/repo/include/foonathan/memory/detail/align.hpp"
    "/Volumes/UserFBWorkspace/questions/FS3739AB23DC8DU6610R8A9CD2F70/attempts/20261123T165238360736Z/repo/include/foonathan/memory/detail/asan.hpp"
    "/Volumes/UserFBWorkspace/questions/FS3739AB23DC8DU6610R8A9CD2F70/attempts/20261123T165238360736Z/repo/include/foonathan/memory/detail/assert.hpp"
    "/Volumes/UserFBWorkspace/questions/FS3739AB23DC8DU6610R8A9CD2F70/attempts/20261123T165238360736Z/repo/include/foonathan/memory/detail/container_node_sizes.hpp"
    "/Volumes/UserFBWorkspace/questions/FS3739AB23DC8DU6610R8A9CD2F70/attempts/20261123T165238360736Z/repo/include/foonathan/memory/detail/debug_helpers.hpp"
    "/Volumes/UserFBWorkspace/questions/FS3739AB23DC8DU6610R8A9CD2F70/attempts/20261123T165238360736Z/repo/include/foonathan/memory/detail/ebo_storage.hpp"
    "/Volumes/UserFBWorkspace/questions/FS3739AB23DC8DU6610R8A9CD2F70/attempts/20261123T165238360736Z/repo/include/foonathan/memory/detail/free_list.hpp"
    "/Volumes/UserFBWorkspace/questions/FS3739AB23DC8DU6610R8A9CD2F70/attempts/20261123T165238360736Z/repo/include/foonathan/memory/detail/free_list_array.hpp"
    "/Volumes/UserFBWorkspace/questions/FS3739AB23DC8DU6610R8A9CD2F70/attempts/20261123T165238360736Z/repo/include/foonathan/memory/detail/ilog2.hpp"
    "/Volumes/UserFBWorkspace/questions/FS3739AB23DC8DU6610R8A9CD2F70/attempts/20261123T165238360736Z/repo/include/foonathan/memory/detail/lowlevel_allocator.hpp"
    "/Volumes/UserFBWorkspace/questions/FS3739AB23DC8DU6610R8A9CD2F70/attempts/20261123T165238360736Z/repo/include/foonathan/memory/detail/memory_stack.hpp"
    "/Volumes/UserFBWorkspace/questions/FS3739AB23DC8DU6610R8A9CD2F70/attempts/20261123T165238360736Z/repo/include/foonathan/memory/detail/small_free_list.hpp"
    "/Volumes/UserFBWorkspace/questions/FS3739AB23DC8DU6610R8A9CD2F70/attempts/20261123T165238360736Z/repo/include/foonathan/memory/detail/utility.hpp"
    )
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/lib/foonathan_memory/cmake" TYPE FILE FILES "/Volumes/UserFBWorkspace/questions/FS3739AB23DC8DU6610R8A9CD2F70/attempts/20261123T165238360736Z/repo/build-checks/src/cmake/foonathan_memory-config-version.cmake")
endif()

string(REPLACE ";" "\n" CMAKE_INSTALL_MANIFEST_CONTENT
       "${CMAKE_INSTALL_MANIFEST_FILES}")
if(CMAKE_INSTALL_LOCAL_ONLY)
  file(WRITE "/Volumes/UserFBWorkspace/questions/FS3739AB23DC8DU6610R8A9CD2F70/attempts/20261123T165238360736Z/repo/build-checks/src/install_local_manifest.txt"
     "${CMAKE_INSTALL_MANIFEST_CONTENT}")
endif()
