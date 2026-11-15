# Install script for directory: /Volumes/UserFBWorkspace/questions/FSEAA0B143EEF7U11624/attempts/20261115T161037375964Z/repo

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
    set(CMAKE_INSTALL_CONFIG_NAME "Debug")
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

if(CMAKE_INSTALL_COMPONENT STREQUAL "headers" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/include/mmg/common" TYPE FILE FILES
    "/Volumes/UserFBWorkspace/questions/FSEAA0B143EEF7U11624/attempts/20261115T161037375964Z/repo/src/common/mmg_export.h"
    "/Volumes/UserFBWorkspace/questions/FSEAA0B143EEF7U11624/attempts/20261115T161037375964Z/repo/src/common/libmmgtypes.h"
    "/Volumes/UserFBWorkspace/questions/FSEAA0B143EEF7U11624/attempts/20261115T161037375964Z/repo/.task-build/asan/src/common/mmgcmakedefines.h"
    "/Volumes/UserFBWorkspace/questions/FSEAA0B143EEF7U11624/attempts/20261115T161037375964Z/repo/.task-build/asan/src/common/mmgcmakedefinesf.h"
    "/Volumes/UserFBWorkspace/questions/FSEAA0B143EEF7U11624/attempts/20261115T161037375964Z/repo/.task-build/asan/src/common/mmgversion.h"
    "/Volumes/UserFBWorkspace/questions/FSEAA0B143EEF7U11624/attempts/20261115T161037375964Z/repo/.task-build/asan/src/common/libmmgtypesf.h"
    "/Volumes/UserFBWorkspace/questions/FSEAA0B143EEF7U11624/attempts/20261115T161037375964Z/repo/.task-build/asan/src/common/git_log_mmg.h"
    )
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/lib" TYPE STATIC_LIBRARY FILES "/Volumes/UserFBWorkspace/questions/FSEAA0B143EEF7U11624/attempts/20261115T161037375964Z/repo/.task-build/asan/lib/libmmgs.a")
  if(EXISTS "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/lib/libmmgs.a" AND
     NOT IS_SYMLINK "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/lib/libmmgs.a")
    execute_process(COMMAND "/usr/bin/ranlib" "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/lib/libmmgs.a")
  endif()
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/share/man/man1" TYPE FILE FILES "/Volumes/UserFBWorkspace/questions/FSEAA0B143EEF7U11624/attempts/20261115T161037375964Z/repo/doc/man/mmgs.1.gz")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "headers" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/include/mmg/mmgs" TYPE FILE FILES
    "/Volumes/UserFBWorkspace/questions/FSEAA0B143EEF7U11624/attempts/20261115T161037375964Z/repo/src/mmgs/mmgs_export.h"
    "/Volumes/UserFBWorkspace/questions/FSEAA0B143EEF7U11624/attempts/20261115T161037375964Z/repo/src/mmgs/libmmgs.h"
    "/Volumes/UserFBWorkspace/questions/FSEAA0B143EEF7U11624/attempts/20261115T161037375964Z/repo/.task-build/asan/src/mmgs/libmmgsf.h"
    )
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "appli" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/bin" TYPE EXECUTABLE FILES "/Volumes/UserFBWorkspace/questions/FSEAA0B143EEF7U11624/attempts/20261115T161037375964Z/repo/.task-build/asan/bin/mmgs_debug")
  if(EXISTS "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/mmgs_debug" AND
     NOT IS_SYMLINK "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/mmgs_debug")
    if(CMAKE_INSTALL_DO_STRIP)
      execute_process(COMMAND "/usr/bin/strip" -u -r "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/mmgs_debug")
    endif()
  endif()
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  if(EXISTS "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/lib/cmake/mmg/MmgTargets.cmake")
    file(DIFFERENT _cmake_export_file_changed FILES
         "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/lib/cmake/mmg/MmgTargets.cmake"
         "/Volumes/UserFBWorkspace/questions/FSEAA0B143EEF7U11624/attempts/20261115T161037375964Z/repo/.task-build/asan/CMakeFiles/Export/f74b78c45ccd84d8b5f0b176c3fd6a45/MmgTargets.cmake")
    if(_cmake_export_file_changed)
      file(GLOB _cmake_old_config_files "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/lib/cmake/mmg/MmgTargets-*.cmake")
      if(_cmake_old_config_files)
        string(REPLACE ";" ", " _cmake_old_config_files_text "${_cmake_old_config_files}")
        message(STATUS "Old export file \"$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/lib/cmake/mmg/MmgTargets.cmake\" will be replaced.  Removing files [${_cmake_old_config_files_text}].")
        unset(_cmake_old_config_files_text)
        file(REMOVE ${_cmake_old_config_files})
      endif()
      unset(_cmake_old_config_files)
    endif()
    unset(_cmake_export_file_changed)
  endif()
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/lib/cmake/mmg" TYPE FILE FILES "/Volumes/UserFBWorkspace/questions/FSEAA0B143EEF7U11624/attempts/20261115T161037375964Z/repo/.task-build/asan/CMakeFiles/Export/f74b78c45ccd84d8b5f0b176c3fd6a45/MmgTargets.cmake")
  if(CMAKE_INSTALL_CONFIG_NAME MATCHES "^([Dd][Ee][Bb][Uu][Gg])$")
    file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/lib/cmake/mmg" TYPE FILE FILES "/Volumes/UserFBWorkspace/questions/FSEAA0B143EEF7U11624/attempts/20261115T161037375964Z/repo/.task-build/asan/CMakeFiles/Export/f74b78c45ccd84d8b5f0b176c3fd6a45/MmgTargets-debug.cmake")
  endif()
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/lib/cmake/mmg" TYPE FILE FILES "/Volumes/UserFBWorkspace/questions/FSEAA0B143EEF7U11624/attempts/20261115T161037375964Z/repo/.task-build/asan/mmgConfig.cmake")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/lib/cmake/mmg" TYPE FILE FILES
    "/Volumes/UserFBWorkspace/questions/FSEAA0B143EEF7U11624/attempts/20261115T161037375964Z/repo/cmake/modules/FindSCOTCH.cmake"
    "/Volumes/UserFBWorkspace/questions/FSEAA0B143EEF7U11624/attempts/20261115T161037375964Z/repo/cmake/modules/FindElas.cmake"
    )
endif()

string(REPLACE ";" "\n" CMAKE_INSTALL_MANIFEST_CONTENT
       "${CMAKE_INSTALL_MANIFEST_FILES}")
if(CMAKE_INSTALL_LOCAL_ONLY)
  file(WRITE "/Volumes/UserFBWorkspace/questions/FSEAA0B143EEF7U11624/attempts/20261115T161037375964Z/repo/.task-build/asan/install_local_manifest.txt"
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
  file(WRITE "/Volumes/UserFBWorkspace/questions/FSEAA0B143EEF7U11624/attempts/20261115T161037375964Z/repo/.task-build/asan/${CMAKE_INSTALL_MANIFEST}"
     "${CMAKE_INSTALL_MANIFEST_CONTENT}")
endif()
