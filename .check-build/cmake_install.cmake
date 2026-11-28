# Install script for directory: /Volumes/UserFBWorkspace/questions/FSFE895A4B656CU6060/attempts/20261127T160801599810Z/repo

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
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/lib" TYPE STATIC_LIBRARY FILES "/Volumes/UserFBWorkspace/questions/FSFE895A4B656CU6060/attempts/20261127T160801599810Z/repo/.check-build/libcharls.a")
  if(EXISTS "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/lib/libcharls.a" AND
     NOT IS_SYMLINK "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/lib/libcharls.a")
    execute_process(COMMAND "/usr/bin/ranlib" "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/lib/libcharls.a")
  endif()
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/include/charls" TYPE FILE FILES
    "/Volumes/UserFBWorkspace/questions/FSFE895A4B656CU6060/attempts/20261127T160801599810Z/repo/include/charls/annotations.h"
    "/Volumes/UserFBWorkspace/questions/FSFE895A4B656CU6060/attempts/20261127T160801599810Z/repo/include/charls/api_abi.h"
    "/Volumes/UserFBWorkspace/questions/FSFE895A4B656CU6060/attempts/20261127T160801599810Z/repo/include/charls/charls.h"
    "/Volumes/UserFBWorkspace/questions/FSFE895A4B656CU6060/attempts/20261127T160801599810Z/repo/include/charls/charls.hpp"
    "/Volumes/UserFBWorkspace/questions/FSFE895A4B656CU6060/attempts/20261127T160801599810Z/repo/include/charls/charls_jpegls_decoder.h"
    "/Volumes/UserFBWorkspace/questions/FSFE895A4B656CU6060/attempts/20261127T160801599810Z/repo/include/charls/charls_jpegls_encoder.h"
    "/Volumes/UserFBWorkspace/questions/FSFE895A4B656CU6060/attempts/20261127T160801599810Z/repo/include/charls/jpegls_decoder.hpp"
    "/Volumes/UserFBWorkspace/questions/FSFE895A4B656CU6060/attempts/20261127T160801599810Z/repo/include/charls/jpegls_encoder.hpp"
    "/Volumes/UserFBWorkspace/questions/FSFE895A4B656CU6060/attempts/20261127T160801599810Z/repo/include/charls/jpegls_stream_decoder.hpp"
    "/Volumes/UserFBWorkspace/questions/FSFE895A4B656CU6060/attempts/20261127T160801599810Z/repo/include/charls/jpegls_error.h"
    "/Volumes/UserFBWorkspace/questions/FSFE895A4B656CU6060/attempts/20261127T160801599810Z/repo/include/charls/jpegls_error.hpp"
    "/Volumes/UserFBWorkspace/questions/FSFE895A4B656CU6060/attempts/20261127T160801599810Z/repo/include/charls/public_types.h"
    "/Volumes/UserFBWorkspace/questions/FSFE895A4B656CU6060/attempts/20261127T160801599810Z/repo/include/charls/undef_macros.h"
    "/Volumes/UserFBWorkspace/questions/FSFE895A4B656CU6060/attempts/20261127T160801599810Z/repo/include/charls/validate_spiff_header.h"
    "/Volumes/UserFBWorkspace/questions/FSFE895A4B656CU6060/attempts/20261127T160801599810Z/repo/include/charls/version.h"
    "/Volumes/UserFBWorkspace/questions/FSFE895A4B656CU6060/attempts/20261127T160801599810Z/repo/include/charls/version.hpp"
    )
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/lib/pkgconfig" TYPE FILE FILES "/Volumes/UserFBWorkspace/questions/FSFE895A4B656CU6060/attempts/20261127T160801599810Z/repo/.check-build/charls.pc")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/lib/cmake/charls" TYPE FILE FILES "/Volumes/UserFBWorkspace/questions/FSFE895A4B656CU6060/attempts/20261127T160801599810Z/repo/.check-build/charlsConfigVersion.cmake")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  if(EXISTS "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/lib/cmake/charls/charlsConfig.cmake")
    file(DIFFERENT _cmake_export_file_changed FILES
         "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/lib/cmake/charls/charlsConfig.cmake"
         "/Volumes/UserFBWorkspace/questions/FSFE895A4B656CU6060/attempts/20261127T160801599810Z/repo/.check-build/CMakeFiles/Export/829a16e4feba7a7fd3ce1556484e906b/charlsConfig.cmake")
    if(_cmake_export_file_changed)
      file(GLOB _cmake_old_config_files "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/lib/cmake/charls/charlsConfig-*.cmake")
      if(_cmake_old_config_files)
        string(REPLACE ";" ", " _cmake_old_config_files_text "${_cmake_old_config_files}")
        message(STATUS "Old export file \"$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/lib/cmake/charls/charlsConfig.cmake\" will be replaced.  Removing files [${_cmake_old_config_files_text}].")
        unset(_cmake_old_config_files_text)
        file(REMOVE ${_cmake_old_config_files})
      endif()
      unset(_cmake_old_config_files)
    endif()
    unset(_cmake_export_file_changed)
  endif()
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/lib/cmake/charls" TYPE FILE FILES "/Volumes/UserFBWorkspace/questions/FSFE895A4B656CU6060/attempts/20261127T160801599810Z/repo/.check-build/CMakeFiles/Export/829a16e4feba7a7fd3ce1556484e906b/charlsConfig.cmake")
  if(CMAKE_INSTALL_CONFIG_NAME MATCHES "^([Rr][Ee][Ll][Ee][Aa][Ss][Ee])$")
    file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/lib/cmake/charls" TYPE FILE FILES "/Volumes/UserFBWorkspace/questions/FSFE895A4B656CU6060/attempts/20261127T160801599810Z/repo/.check-build/CMakeFiles/Export/829a16e4feba7a7fd3ce1556484e906b/charlsConfig-release.cmake")
  endif()
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("/Volumes/UserFBWorkspace/questions/FSFE895A4B656CU6060/attempts/20261127T160801599810Z/repo/.check-build/test/cmake_install.cmake")
endif()

string(REPLACE ";" "\n" CMAKE_INSTALL_MANIFEST_CONTENT
       "${CMAKE_INSTALL_MANIFEST_FILES}")
if(CMAKE_INSTALL_LOCAL_ONLY)
  file(WRITE "/Volumes/UserFBWorkspace/questions/FSFE895A4B656CU6060/attempts/20261127T160801599810Z/repo/.check-build/install_local_manifest.txt"
     "${CMAKE_INSTALL_MANIFEST_CONTENT}")
endif()
if(CMAKE_INSTALL_COMPONENT)
  if(CMAKE_INSTALL_COMPONENT MATCHES "^[a-zA-Z0-9_.+-]+$")
    set(CMAKE_INSTALL_MANIFEST "install_manifest_${CMAKE_INSTALL_COMPONENT}.txt")
  else()
    string(MD5 CMAKE_INST_COMP_HASH "${CMAKE_INSTALL_COMPONENT}")
    set(CMAKE_INSTALL_MANIFEST "install_manifest_${CMAKE_INST_COMP_HASH}.txt")
    unset(CMAKE_INST_COMP_HASH)
  endif()
else()
  set(CMAKE_INSTALL_MANIFEST "install_manifest.txt")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  file(WRITE "/Volumes/UserFBWorkspace/questions/FSFE895A4B656CU6060/attempts/20261127T160801599810Z/repo/.check-build/${CMAKE_INSTALL_MANIFEST}"
     "${CMAKE_INSTALL_MANIFEST_CONTENT}")
endif()
