# Install script for directory: /Users/solo/pro/UserFB/workspace/FSE41FEA1E43C5U6918/attempts/20261113T202029870395Z/repo/examples

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
  list(APPEND CMAKE_ABSOLUTE_DESTINATION_FILES
   "/usr/local/share/doc/libsoxr/examples/1-single-block.c;/usr/local/share/doc/libsoxr/examples/1a-lsr.c;/usr/local/share/doc/libsoxr/examples/2-stream.C;/usr/local/share/doc/libsoxr/examples/3-options-input-fn.c;/usr/local/share/doc/libsoxr/examples/4-split-channels.c;/usr/local/share/doc/libsoxr/examples/5-variable-rate.c;/usr/local/share/doc/libsoxr/examples/examples-common.h;/usr/local/share/doc/libsoxr/examples/README")
  if(CMAKE_WARN_ON_ABSOLUTE_INSTALL_DESTINATION)
    message(WARNING "ABSOLUTE path INSTALL DESTINATION : ${CMAKE_ABSOLUTE_DESTINATION_FILES}")
  endif()
  if(CMAKE_ERROR_ON_ABSOLUTE_INSTALL_DESTINATION)
    message(FATAL_ERROR "ABSOLUTE path INSTALL DESTINATION forbidden (by caller): ${CMAKE_ABSOLUTE_DESTINATION_FILES}")
  endif()
  file(INSTALL DESTINATION "/usr/local/share/doc/libsoxr/examples" TYPE FILE FILES
    "/Users/solo/pro/UserFB/workspace/FSE41FEA1E43C5U6918/attempts/20261113T202029870395Z/repo/examples/1-single-block.c"
    "/Users/solo/pro/UserFB/workspace/FSE41FEA1E43C5U6918/attempts/20261113T202029870395Z/repo/examples/1a-lsr.c"
    "/Users/solo/pro/UserFB/workspace/FSE41FEA1E43C5U6918/attempts/20261113T202029870395Z/repo/examples/2-stream.C"
    "/Users/solo/pro/UserFB/workspace/FSE41FEA1E43C5U6918/attempts/20261113T202029870395Z/repo/examples/3-options-input-fn.c"
    "/Users/solo/pro/UserFB/workspace/FSE41FEA1E43C5U6918/attempts/20261113T202029870395Z/repo/examples/4-split-channels.c"
    "/Users/solo/pro/UserFB/workspace/FSE41FEA1E43C5U6918/attempts/20261113T202029870395Z/repo/examples/5-variable-rate.c"
    "/Users/solo/pro/UserFB/workspace/FSE41FEA1E43C5U6918/attempts/20261113T202029870395Z/repo/examples/examples-common.h"
    "/Users/solo/pro/UserFB/workspace/FSE41FEA1E43C5U6918/attempts/20261113T202029870395Z/repo/examples/README"
    )
endif()

string(REPLACE ";" "\n" CMAKE_INSTALL_MANIFEST_CONTENT
       "${CMAKE_INSTALL_MANIFEST_FILES}")
if(CMAKE_INSTALL_LOCAL_ONLY)
  file(WRITE "/Users/solo/pro/UserFB/workspace/FSE41FEA1E43C5U6918/attempts/20261113T202029870395Z/repo/.task-build/lib/examples/install_local_manifest.txt"
     "${CMAKE_INSTALL_MANIFEST_CONTENT}")
endif()
