# Install script for directory: /Volumes/UserFBWorkspace/questions/FSC0050C137AFFU11839/attempts/20261116T161123843784Z/repo

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

if(CMAKE_INSTALL_COMPONENT STREQUAL "devel" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/include" TYPE DIRECTORY FILES "/Volumes/UserFBWorkspace/questions/FSC0050C137AFFU11839/attempts/20261116T161123843784Z/repo/include/embree4")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "devel" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/share/man" TYPE DIRECTORY FILES "/Volumes/UserFBWorkspace/questions/FSC0050C137AFFU11839/attempts/20261116T161123843784Z/repo/man/man3")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "lib" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/share/doc/embree4" TYPE FILE FILES "/Volumes/UserFBWorkspace/questions/FSC0050C137AFFU11839/attempts/20261116T161123843784Z/repo/LICENSE.txt")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "lib" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/share/doc/embree4" TYPE FILE FILES "/Volumes/UserFBWorkspace/questions/FSC0050C137AFFU11839/attempts/20261116T161123843784Z/repo/CHANGELOG.md")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "lib" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/share/doc/embree4" TYPE FILE FILES "/Volumes/UserFBWorkspace/questions/FSC0050C137AFFU11839/attempts/20261116T161123843784Z/repo/README.md")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "lib" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/share/doc/embree4" TYPE FILE FILES "/Volumes/UserFBWorkspace/questions/FSC0050C137AFFU11839/attempts/20261116T161123843784Z/repo/readme.pdf")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "lib" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/share/doc/embree4" TYPE FILE FILES "/Volumes/UserFBWorkspace/questions/FSC0050C137AFFU11839/attempts/20261116T161123843784Z/repo/third-party-programs.txt")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "lib" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/share/doc/embree4" TYPE FILE FILES "/Volumes/UserFBWorkspace/questions/FSC0050C137AFFU11839/attempts/20261116T161123843784Z/repo/third-party-programs-TBB.txt")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "lib" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/share/doc/embree4" TYPE FILE FILES "/Volumes/UserFBWorkspace/questions/FSC0050C137AFFU11839/attempts/20261116T161123843784Z/repo/third-party-programs-OIDN.txt")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "lib" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/share/doc/embree4" TYPE FILE FILES "/Volumes/UserFBWorkspace/questions/FSC0050C137AFFU11839/attempts/20261116T161123843784Z/repo/third-party-programs-DPCPP.txt")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "lib" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/share/doc/embree4" TYPE FILE FILES "/Volumes/UserFBWorkspace/questions/FSC0050C137AFFU11839/attempts/20261116T161123843784Z/repo/third-party-programs-oneAPI-DPCPP.txt")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "lib" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}" TYPE FILE FILES "/Volumes/UserFBWorkspace/questions/FSC0050C137AFFU11839/attempts/20261116T161123843784Z/repo/.task-build/embree-vars.sh")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "lib" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}" TYPE FILE FILES "/Volumes/UserFBWorkspace/questions/FSC0050C137AFFU11839/attempts/20261116T161123843784Z/repo/.task-build/embree-vars.csh")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "devel" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/lib/cmake/embree-4.5.0" TYPE FILE RENAME "embree-config.cmake" FILES "/Volumes/UserFBWorkspace/questions/FSC0050C137AFFU11839/attempts/20261116T161123843784Z/repo/.task-build/embree-config-install.cmake")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "devel" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/lib/cmake/embree-4.5.0" TYPE FILE FILES "/Volumes/UserFBWorkspace/questions/FSC0050C137AFFU11839/attempts/20261116T161123843784Z/repo/.task-build/embree-config-version.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for each subdirectory.
  include("/Volumes/UserFBWorkspace/questions/FSC0050C137AFFU11839/attempts/20261116T161123843784Z/repo/.task-build/common/cmake_install.cmake")
  include("/Volumes/UserFBWorkspace/questions/FSC0050C137AFFU11839/attempts/20261116T161123843784Z/repo/.task-build/kernels/cmake_install.cmake")
  include("/Volumes/UserFBWorkspace/questions/FSC0050C137AFFU11839/attempts/20261116T161123843784Z/repo/.task-build/tests/cmake_install.cmake")

endif()

string(REPLACE ";" "\n" CMAKE_INSTALL_MANIFEST_CONTENT
       "${CMAKE_INSTALL_MANIFEST_FILES}")
if(CMAKE_INSTALL_LOCAL_ONLY)
  file(WRITE "/Volumes/UserFBWorkspace/questions/FSC0050C137AFFU11839/attempts/20261116T161123843784Z/repo/.task-build/install_local_manifest.txt"
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
  file(WRITE "/Volumes/UserFBWorkspace/questions/FSC0050C137AFFU11839/attempts/20261116T161123843784Z/repo/.task-build/${CMAKE_INSTALL_MANIFEST}"
     "${CMAKE_INSTALL_MANIFEST_CONTENT}")
endif()
