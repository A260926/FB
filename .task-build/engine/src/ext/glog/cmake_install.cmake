# Install script for directory: /Volumes/UserFBWorkspace/questions/FSC4E0CBFBA75AU14179/attempts/20261121T181124651953Z/repo/src/ext/glog

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
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/lib" TYPE STATIC_LIBRARY FILES "/Volumes/UserFBWorkspace/questions/FSC4E0CBFBA75AU14179/attempts/20261121T181124651953Z/repo/.task-build/engine/src/ext/glog/libglog.a")
  if(EXISTS "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/lib/libglog.a" AND
     NOT IS_SYMLINK "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/lib/libglog.a")
    execute_process(COMMAND "/usr/bin/ranlib" "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/lib/libglog.a")
  endif()
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/include/glog" TYPE FILE FILES
    "/Volumes/UserFBWorkspace/questions/FSC4E0CBFBA75AU14179/attempts/20261121T181124651953Z/repo/.task-build/engine/src/ext/glog/config.h"
    "/Volumes/UserFBWorkspace/questions/FSC4E0CBFBA75AU14179/attempts/20261121T181124651953Z/repo/.task-build/engine/src/ext/glog/glog/logging.h"
    "/Volumes/UserFBWorkspace/questions/FSC4E0CBFBA75AU14179/attempts/20261121T181124651953Z/repo/.task-build/engine/src/ext/glog/glog/raw_logging.h"
    "/Volumes/UserFBWorkspace/questions/FSC4E0CBFBA75AU14179/attempts/20261121T181124651953Z/repo/.task-build/engine/src/ext/glog/glog/stl_logging.h"
    "/Volumes/UserFBWorkspace/questions/FSC4E0CBFBA75AU14179/attempts/20261121T181124651953Z/repo/.task-build/engine/src/ext/glog/glog/vlog_is_on.h"
    "/Volumes/UserFBWorkspace/questions/FSC4E0CBFBA75AU14179/attempts/20261121T181124651953Z/repo/src/ext/glog/src/glog/log_severity.h"
    )
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/lib/cmake/glog" TYPE FILE FILES
    "/Volumes/UserFBWorkspace/questions/FSC4E0CBFBA75AU14179/attempts/20261121T181124651953Z/repo/.task-build/engine/src/ext/glog/CMakeFiles/glog-config.cmake"
    "/Volumes/UserFBWorkspace/questions/FSC4E0CBFBA75AU14179/attempts/20261121T181124651953Z/repo/.task-build/engine/src/ext/glog/glog-config-version.cmake"
    )
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  if(EXISTS "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/lib/cmake/glog/glog-targets.cmake")
    file(DIFFERENT _cmake_export_file_changed FILES
         "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/lib/cmake/glog/glog-targets.cmake"
         "/Volumes/UserFBWorkspace/questions/FSC4E0CBFBA75AU14179/attempts/20261121T181124651953Z/repo/.task-build/engine/src/ext/glog/CMakeFiles/Export/3b58dfe2d8c90c3a489d71c8991c4dd3/glog-targets.cmake")
    if(_cmake_export_file_changed)
      file(GLOB _cmake_old_config_files "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/lib/cmake/glog/glog-targets-*.cmake")
      if(_cmake_old_config_files)
        string(REPLACE ";" ", " _cmake_old_config_files_text "${_cmake_old_config_files}")
        message(STATUS "Old export file \"$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/lib/cmake/glog/glog-targets.cmake\" will be replaced.  Removing files [${_cmake_old_config_files_text}].")
        unset(_cmake_old_config_files_text)
        file(REMOVE ${_cmake_old_config_files})
      endif()
      unset(_cmake_old_config_files)
    endif()
    unset(_cmake_export_file_changed)
  endif()
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/lib/cmake/glog" TYPE FILE FILES "/Volumes/UserFBWorkspace/questions/FSC4E0CBFBA75AU14179/attempts/20261121T181124651953Z/repo/.task-build/engine/src/ext/glog/CMakeFiles/Export/3b58dfe2d8c90c3a489d71c8991c4dd3/glog-targets.cmake")
  if(CMAKE_INSTALL_CONFIG_NAME MATCHES "^([Rr][Ee][Ll][Ee][Aa][Ss][Ee])$")
    file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/lib/cmake/glog" TYPE FILE FILES "/Volumes/UserFBWorkspace/questions/FSC4E0CBFBA75AU14179/attempts/20261121T181124651953Z/repo/.task-build/engine/src/ext/glog/CMakeFiles/Export/3b58dfe2d8c90c3a489d71c8991c4dd3/glog-targets-release.cmake")
  endif()
endif()

string(REPLACE ";" "\n" CMAKE_INSTALL_MANIFEST_CONTENT
       "${CMAKE_INSTALL_MANIFEST_FILES}")
if(CMAKE_INSTALL_LOCAL_ONLY)
  file(WRITE "/Volumes/UserFBWorkspace/questions/FSC4E0CBFBA75AU14179/attempts/20261121T181124651953Z/repo/.task-build/engine/src/ext/glog/install_local_manifest.txt"
     "${CMAKE_INSTALL_MANIFEST_CONTENT}")
endif()
