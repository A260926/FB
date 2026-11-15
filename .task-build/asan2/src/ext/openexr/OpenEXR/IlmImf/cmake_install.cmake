# Install script for directory: /Volumes/UserFBWorkspace/questions/FS066D145C7435U11592/attempts/20261115T160953783474Z/repo/src/ext/openexr/IlmBase/Imath

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
    set(CMAKE_INSTALL_CONFIG_NAME "RelWithDebInfo")
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
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/lib" TYPE STATIC_LIBRARY FILES "/Volumes/UserFBWorkspace/questions/FS066D145C7435U11592/attempts/20261115T160953783474Z/repo/.task-build/asan2/src/ext/openexr/IlmBase/Imath/libImath.a")
  if(EXISTS "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/lib/libImath.a" AND
     NOT IS_SYMLINK "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/lib/libImath.a")
    execute_process(COMMAND "/usr/bin/ranlib" "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/lib/libImath.a")
  endif()
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  include("/Volumes/UserFBWorkspace/questions/FS066D145C7435U11592/attempts/20261115T160953783474Z/repo/.task-build/asan2/src/ext/openexr/IlmBase/Imath/CMakeFiles/Imath.dir/install-cxx-module-bmi-RelWithDebInfo.cmake" OPTIONAL)
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/include/OpenEXR" TYPE FILE FILES
    "/Volumes/UserFBWorkspace/questions/FS066D145C7435U11592/attempts/20261115T160953783474Z/repo/src/ext/openexr/IlmBase/Imath/ImathBoxAlgo.h"
    "/Volumes/UserFBWorkspace/questions/FS066D145C7435U11592/attempts/20261115T160953783474Z/repo/src/ext/openexr/IlmBase/Imath/ImathBox.h"
    "/Volumes/UserFBWorkspace/questions/FS066D145C7435U11592/attempts/20261115T160953783474Z/repo/src/ext/openexr/IlmBase/Imath/ImathColorAlgo.h"
    "/Volumes/UserFBWorkspace/questions/FS066D145C7435U11592/attempts/20261115T160953783474Z/repo/src/ext/openexr/IlmBase/Imath/ImathColor.h"
    "/Volumes/UserFBWorkspace/questions/FS066D145C7435U11592/attempts/20261115T160953783474Z/repo/src/ext/openexr/IlmBase/Imath/ImathEuler.h"
    "/Volumes/UserFBWorkspace/questions/FS066D145C7435U11592/attempts/20261115T160953783474Z/repo/src/ext/openexr/IlmBase/Imath/ImathExc.h"
    "/Volumes/UserFBWorkspace/questions/FS066D145C7435U11592/attempts/20261115T160953783474Z/repo/src/ext/openexr/IlmBase/Imath/ImathExport.h"
    "/Volumes/UserFBWorkspace/questions/FS066D145C7435U11592/attempts/20261115T160953783474Z/repo/src/ext/openexr/IlmBase/Imath/ImathForward.h"
    "/Volumes/UserFBWorkspace/questions/FS066D145C7435U11592/attempts/20261115T160953783474Z/repo/src/ext/openexr/IlmBase/Imath/ImathFrame.h"
    "/Volumes/UserFBWorkspace/questions/FS066D145C7435U11592/attempts/20261115T160953783474Z/repo/src/ext/openexr/IlmBase/Imath/ImathFrustum.h"
    "/Volumes/UserFBWorkspace/questions/FS066D145C7435U11592/attempts/20261115T160953783474Z/repo/src/ext/openexr/IlmBase/Imath/ImathFrustumTest.h"
    "/Volumes/UserFBWorkspace/questions/FS066D145C7435U11592/attempts/20261115T160953783474Z/repo/src/ext/openexr/IlmBase/Imath/ImathFun.h"
    "/Volumes/UserFBWorkspace/questions/FS066D145C7435U11592/attempts/20261115T160953783474Z/repo/src/ext/openexr/IlmBase/Imath/ImathGL.h"
    "/Volumes/UserFBWorkspace/questions/FS066D145C7435U11592/attempts/20261115T160953783474Z/repo/src/ext/openexr/IlmBase/Imath/ImathGLU.h"
    "/Volumes/UserFBWorkspace/questions/FS066D145C7435U11592/attempts/20261115T160953783474Z/repo/src/ext/openexr/IlmBase/Imath/ImathHalfLimits.h"
    "/Volumes/UserFBWorkspace/questions/FS066D145C7435U11592/attempts/20261115T160953783474Z/repo/src/ext/openexr/IlmBase/Imath/ImathInt64.h"
    "/Volumes/UserFBWorkspace/questions/FS066D145C7435U11592/attempts/20261115T160953783474Z/repo/src/ext/openexr/IlmBase/Imath/ImathInterval.h"
    "/Volumes/UserFBWorkspace/questions/FS066D145C7435U11592/attempts/20261115T160953783474Z/repo/src/ext/openexr/IlmBase/Imath/ImathLimits.h"
    "/Volumes/UserFBWorkspace/questions/FS066D145C7435U11592/attempts/20261115T160953783474Z/repo/src/ext/openexr/IlmBase/Imath/ImathLineAlgo.h"
    "/Volumes/UserFBWorkspace/questions/FS066D145C7435U11592/attempts/20261115T160953783474Z/repo/src/ext/openexr/IlmBase/Imath/ImathLine.h"
    "/Volumes/UserFBWorkspace/questions/FS066D145C7435U11592/attempts/20261115T160953783474Z/repo/src/ext/openexr/IlmBase/Imath/ImathMath.h"
    "/Volumes/UserFBWorkspace/questions/FS066D145C7435U11592/attempts/20261115T160953783474Z/repo/src/ext/openexr/IlmBase/Imath/ImathMatrixAlgo.h"
    "/Volumes/UserFBWorkspace/questions/FS066D145C7435U11592/attempts/20261115T160953783474Z/repo/src/ext/openexr/IlmBase/Imath/ImathMatrix.h"
    "/Volumes/UserFBWorkspace/questions/FS066D145C7435U11592/attempts/20261115T160953783474Z/repo/src/ext/openexr/IlmBase/Imath/ImathNamespace.h"
    "/Volumes/UserFBWorkspace/questions/FS066D145C7435U11592/attempts/20261115T160953783474Z/repo/src/ext/openexr/IlmBase/Imath/ImathPlane.h"
    "/Volumes/UserFBWorkspace/questions/FS066D145C7435U11592/attempts/20261115T160953783474Z/repo/src/ext/openexr/IlmBase/Imath/ImathPlatform.h"
    "/Volumes/UserFBWorkspace/questions/FS066D145C7435U11592/attempts/20261115T160953783474Z/repo/src/ext/openexr/IlmBase/Imath/ImathQuat.h"
    "/Volumes/UserFBWorkspace/questions/FS066D145C7435U11592/attempts/20261115T160953783474Z/repo/src/ext/openexr/IlmBase/Imath/ImathRandom.h"
    "/Volumes/UserFBWorkspace/questions/FS066D145C7435U11592/attempts/20261115T160953783474Z/repo/src/ext/openexr/IlmBase/Imath/ImathRoots.h"
    "/Volumes/UserFBWorkspace/questions/FS066D145C7435U11592/attempts/20261115T160953783474Z/repo/src/ext/openexr/IlmBase/Imath/ImathShear.h"
    "/Volumes/UserFBWorkspace/questions/FS066D145C7435U11592/attempts/20261115T160953783474Z/repo/src/ext/openexr/IlmBase/Imath/ImathSphere.h"
    "/Volumes/UserFBWorkspace/questions/FS066D145C7435U11592/attempts/20261115T160953783474Z/repo/src/ext/openexr/IlmBase/Imath/ImathVecAlgo.h"
    "/Volumes/UserFBWorkspace/questions/FS066D145C7435U11592/attempts/20261115T160953783474Z/repo/src/ext/openexr/IlmBase/Imath/ImathVec.h"
    )
endif()

string(REPLACE ";" "\n" CMAKE_INSTALL_MANIFEST_CONTENT
       "${CMAKE_INSTALL_MANIFEST_FILES}")
if(CMAKE_INSTALL_LOCAL_ONLY)
  file(WRITE "/Volumes/UserFBWorkspace/questions/FS066D145C7435U11592/attempts/20261115T160953783474Z/repo/.task-build/asan2/src/ext/openexr/IlmBase/Imath/install_local_manifest.txt"
     "${CMAKE_INSTALL_MANIFEST_CONTENT}")
endif()
