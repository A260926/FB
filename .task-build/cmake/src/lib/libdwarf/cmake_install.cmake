# Install script for directory: /Volumes/UserFBWorkspace/questions/FS4F6394F27B1EU16213/attempts/20261128T162910981352Z/repo/src/lib/libdwarf

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

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/lib/cmake/libdwarf" TYPE FILE FILES
    "/Volumes/UserFBWorkspace/questions/FS4F6394F27B1EU16213/attempts/20261128T162910981352Z/repo/.task-build/cmake/src/lib/libdwarf/libdwarfConfig.cmake"
    "/Volumes/UserFBWorkspace/questions/FS4F6394F27B1EU16213/attempts/20261128T162910981352Z/repo/.task-build/cmake/src/lib/libdwarf/libdwarfConfigVersion.cmake"
    )
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/lib" TYPE SHARED_LIBRARY FILES
    "/Volumes/UserFBWorkspace/questions/FS4F6394F27B1EU16213/attempts/20261128T162910981352Z/repo/.task-build/cmake/src/lib/libdwarf/libdwarf.2.3.4.dylib"
    "/Volumes/UserFBWorkspace/questions/FS4F6394F27B1EU16213/attempts/20261128T162910981352Z/repo/.task-build/cmake/src/lib/libdwarf/libdwarf.2.dylib"
    )
  foreach(file
      "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/lib/libdwarf.2.3.4.dylib"
      "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/lib/libdwarf.2.dylib"
      )
    if(EXISTS "${file}" AND
       NOT IS_SYMLINK "${file}")
      if(CMAKE_INSTALL_DO_STRIP)
        execute_process(COMMAND "/usr/bin/strip" -x "${file}")
      endif()
    endif()
  endforeach()
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/lib" TYPE SHARED_LIBRARY FILES "/Volumes/UserFBWorkspace/questions/FS4F6394F27B1EU16213/attempts/20261128T162910981352Z/repo/.task-build/cmake/src/lib/libdwarf/libdwarf.dylib")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/include" TYPE FILE FILES
    "/Volumes/UserFBWorkspace/questions/FS4F6394F27B1EU16213/attempts/20261128T162910981352Z/repo/src/lib/libdwarf/libdwarf.h"
    "/Volumes/UserFBWorkspace/questions/FS4F6394F27B1EU16213/attempts/20261128T162910981352Z/repo/src/lib/libdwarf/dwarf.h"
    )
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  if(EXISTS "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/lib/cmake/libdwarf/libdwarf-targets.cmake")
    file(DIFFERENT _cmake_export_file_changed FILES
         "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/lib/cmake/libdwarf/libdwarf-targets.cmake"
         "/Volumes/UserFBWorkspace/questions/FS4F6394F27B1EU16213/attempts/20261128T162910981352Z/repo/.task-build/cmake/src/lib/libdwarf/CMakeFiles/Export/bd262d3fe68cab2e50f9a02b6a024a8d/libdwarf-targets.cmake")
    if(_cmake_export_file_changed)
      file(GLOB _cmake_old_config_files "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/lib/cmake/libdwarf/libdwarf-targets-*.cmake")
      if(_cmake_old_config_files)
        string(REPLACE ";" ", " _cmake_old_config_files_text "${_cmake_old_config_files}")
        message(STATUS "Old export file \"$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/lib/cmake/libdwarf/libdwarf-targets.cmake\" will be replaced.  Removing files [${_cmake_old_config_files_text}].")
        unset(_cmake_old_config_files_text)
        file(REMOVE ${_cmake_old_config_files})
      endif()
      unset(_cmake_old_config_files)
    endif()
    unset(_cmake_export_file_changed)
  endif()
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/lib/cmake/libdwarf" TYPE FILE FILES "/Volumes/UserFBWorkspace/questions/FS4F6394F27B1EU16213/attempts/20261128T162910981352Z/repo/.task-build/cmake/src/lib/libdwarf/CMakeFiles/Export/bd262d3fe68cab2e50f9a02b6a024a8d/libdwarf-targets.cmake")
  if(CMAKE_INSTALL_CONFIG_NAME MATCHES "^([Dd][Ee][Bb][Uu][Gg])$")
    file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/lib/cmake/libdwarf" TYPE FILE FILES "/Volumes/UserFBWorkspace/questions/FS4F6394F27B1EU16213/attempts/20261128T162910981352Z/repo/.task-build/cmake/src/lib/libdwarf/CMakeFiles/Export/bd262d3fe68cab2e50f9a02b6a024a8d/libdwarf-targets-debug.cmake")
  endif()
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/lib/pkgconfig" TYPE FILE FILES "/Volumes/UserFBWorkspace/questions/FS4F6394F27B1EU16213/attempts/20261128T162910981352Z/repo/.task-build/cmake/src/lib/libdwarf/libdwarf.pc")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/lib/cmake/libdwarf" TYPE FILE FILES "/Volumes/UserFBWorkspace/questions/FS4F6394F27B1EU16213/attempts/20261128T162910981352Z/repo/cmake/Findzstd.cmake")
endif()

string(REPLACE ";" "\n" CMAKE_INSTALL_MANIFEST_CONTENT
       "${CMAKE_INSTALL_MANIFEST_FILES}")
if(CMAKE_INSTALL_LOCAL_ONLY)
  file(WRITE "/Volumes/UserFBWorkspace/questions/FS4F6394F27B1EU16213/attempts/20261128T162910981352Z/repo/.task-build/cmake/src/lib/libdwarf/install_local_manifest.txt"
     "${CMAKE_INSTALL_MANIFEST_CONTENT}")
endif()
