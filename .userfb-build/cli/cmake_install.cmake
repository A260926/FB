# Install script for directory: /Users/solo/pro/UserFB/workspace/Z1230/attempts/20261111T163631180204Z/repo/cli

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

if(CMAKE_INSTALL_COMPONENT STREQUAL "applications" OR NOT CMAKE_INSTALL_COMPONENT)
  list(APPEND CMAKE_ABSOLUTE_DESTINATION_FILES
   "/usr/local/bin/cppcheck")
  if(CMAKE_WARN_ON_ABSOLUTE_INSTALL_DESTINATION)
    message(WARNING "ABSOLUTE path INSTALL DESTINATION : ${CMAKE_ABSOLUTE_DESTINATION_FILES}")
  endif()
  if(CMAKE_ERROR_ON_ABSOLUTE_INSTALL_DESTINATION)
    message(FATAL_ERROR "ABSOLUTE path INSTALL DESTINATION forbidden (by caller): ${CMAKE_ABSOLUTE_DESTINATION_FILES}")
  endif()
  file(INSTALL DESTINATION "/usr/local/bin" TYPE EXECUTABLE FILES "/Users/solo/pro/UserFB/workspace/Z1230/attempts/20261111T163631180204Z/repo/.userfb-build/bin/cppcheck")
  if(EXISTS "$ENV{DESTDIR}/usr/local/bin/cppcheck" AND
     NOT IS_SYMLINK "$ENV{DESTDIR}/usr/local/bin/cppcheck")
    if(CMAKE_INSTALL_DO_STRIP)
      execute_process(COMMAND "/usr/bin/strip" "$ENV{DESTDIR}/usr/local/bin/cppcheck")
    endif()
  endif()
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "applications" OR NOT CMAKE_INSTALL_COMPONENT)
  list(APPEND CMAKE_ABSOLUTE_DESTINATION_FILES
   "/usr/local/bin/cppcheck-htmlreport")
  if(CMAKE_WARN_ON_ABSOLUTE_INSTALL_DESTINATION)
    message(WARNING "ABSOLUTE path INSTALL DESTINATION : ${CMAKE_ABSOLUTE_DESTINATION_FILES}")
  endif()
  if(CMAKE_ERROR_ON_ABSOLUTE_INSTALL_DESTINATION)
    message(FATAL_ERROR "ABSOLUTE path INSTALL DESTINATION forbidden (by caller): ${CMAKE_ABSOLUTE_DESTINATION_FILES}")
  endif()
  file(INSTALL DESTINATION "/usr/local/bin" TYPE PROGRAM FILES "/Users/solo/pro/UserFB/workspace/Z1230/attempts/20261111T163631180204Z/repo/htmlreport/cppcheck-htmlreport")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "headers" OR NOT CMAKE_INSTALL_COMPONENT)
  list(APPEND CMAKE_ABSOLUTE_DESTINATION_FILES
   "/usr/local/share/Cppcheck/addons/__init__.py;/usr/local/share/Cppcheck/addons/cppcheck.py;/usr/local/share/Cppcheck/addons/cppcheckdata.py;/usr/local/share/Cppcheck/addons/findcasts.py;/usr/local/share/Cppcheck/addons/misc.py;/usr/local/share/Cppcheck/addons/misra.py;/usr/local/share/Cppcheck/addons/misra_9.py;/usr/local/share/Cppcheck/addons/naming.py;/usr/local/share/Cppcheck/addons/namingng.py;/usr/local/share/Cppcheck/addons/runaddon.py;/usr/local/share/Cppcheck/addons/threadsafety.py;/usr/local/share/Cppcheck/addons/y2038.py")
  if(CMAKE_WARN_ON_ABSOLUTE_INSTALL_DESTINATION)
    message(WARNING "ABSOLUTE path INSTALL DESTINATION : ${CMAKE_ABSOLUTE_DESTINATION_FILES}")
  endif()
  if(CMAKE_ERROR_ON_ABSOLUTE_INSTALL_DESTINATION)
    message(FATAL_ERROR "ABSOLUTE path INSTALL DESTINATION forbidden (by caller): ${CMAKE_ABSOLUTE_DESTINATION_FILES}")
  endif()
  file(INSTALL DESTINATION "/usr/local/share/Cppcheck/addons" TYPE FILE FILES
    "/Users/solo/pro/UserFB/workspace/Z1230/attempts/20261111T163631180204Z/repo/addons/__init__.py"
    "/Users/solo/pro/UserFB/workspace/Z1230/attempts/20261111T163631180204Z/repo/addons/cppcheck.py"
    "/Users/solo/pro/UserFB/workspace/Z1230/attempts/20261111T163631180204Z/repo/addons/cppcheckdata.py"
    "/Users/solo/pro/UserFB/workspace/Z1230/attempts/20261111T163631180204Z/repo/addons/findcasts.py"
    "/Users/solo/pro/UserFB/workspace/Z1230/attempts/20261111T163631180204Z/repo/addons/misc.py"
    "/Users/solo/pro/UserFB/workspace/Z1230/attempts/20261111T163631180204Z/repo/addons/misra.py"
    "/Users/solo/pro/UserFB/workspace/Z1230/attempts/20261111T163631180204Z/repo/addons/misra_9.py"
    "/Users/solo/pro/UserFB/workspace/Z1230/attempts/20261111T163631180204Z/repo/addons/naming.py"
    "/Users/solo/pro/UserFB/workspace/Z1230/attempts/20261111T163631180204Z/repo/addons/namingng.py"
    "/Users/solo/pro/UserFB/workspace/Z1230/attempts/20261111T163631180204Z/repo/addons/runaddon.py"
    "/Users/solo/pro/UserFB/workspace/Z1230/attempts/20261111T163631180204Z/repo/addons/threadsafety.py"
    "/Users/solo/pro/UserFB/workspace/Z1230/attempts/20261111T163631180204Z/repo/addons/y2038.py"
    )
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "headers" OR NOT CMAKE_INSTALL_COMPONENT)
  list(APPEND CMAKE_ABSOLUTE_DESTINATION_FILES
   "/usr/local/share/Cppcheck/addons/ROS_naming.json;/usr/local/share/Cppcheck/addons/namingng.config.json;/usr/local/share/Cppcheck/addons/namingng.json")
  if(CMAKE_WARN_ON_ABSOLUTE_INSTALL_DESTINATION)
    message(WARNING "ABSOLUTE path INSTALL DESTINATION : ${CMAKE_ABSOLUTE_DESTINATION_FILES}")
  endif()
  if(CMAKE_ERROR_ON_ABSOLUTE_INSTALL_DESTINATION)
    message(FATAL_ERROR "ABSOLUTE path INSTALL DESTINATION forbidden (by caller): ${CMAKE_ABSOLUTE_DESTINATION_FILES}")
  endif()
  file(INSTALL DESTINATION "/usr/local/share/Cppcheck/addons" TYPE FILE FILES
    "/Users/solo/pro/UserFB/workspace/Z1230/attempts/20261111T163631180204Z/repo/addons/ROS_naming.json"
    "/Users/solo/pro/UserFB/workspace/Z1230/attempts/20261111T163631180204Z/repo/addons/namingng.config.json"
    "/Users/solo/pro/UserFB/workspace/Z1230/attempts/20261111T163631180204Z/repo/addons/namingng.json"
    )
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "headers" OR NOT CMAKE_INSTALL_COMPONENT)
  list(APPEND CMAKE_ABSOLUTE_DESTINATION_FILES
   "/usr/local/share/Cppcheck/cfg/avr.cfg;/usr/local/share/Cppcheck/cfg/bento4.cfg;/usr/local/share/Cppcheck/cfg/boost.cfg;/usr/local/share/Cppcheck/cfg/bsd.cfg;/usr/local/share/Cppcheck/cfg/cairo.cfg;/usr/local/share/Cppcheck/cfg/cppcheck-lib.cfg;/usr/local/share/Cppcheck/cfg/cppunit.cfg;/usr/local/share/Cppcheck/cfg/dpdk.cfg;/usr/local/share/Cppcheck/cfg/embedded_sql.cfg;/usr/local/share/Cppcheck/cfg/emscripten.cfg;/usr/local/share/Cppcheck/cfg/ginac.cfg;/usr/local/share/Cppcheck/cfg/gnu.cfg;/usr/local/share/Cppcheck/cfg/googletest.cfg;/usr/local/share/Cppcheck/cfg/gtk.cfg;/usr/local/share/Cppcheck/cfg/icu.cfg;/usr/local/share/Cppcheck/cfg/kde.cfg;/usr/local/share/Cppcheck/cfg/libcerror.cfg;/usr/local/share/Cppcheck/cfg/libcurl.cfg;/usr/local/share/Cppcheck/cfg/libsigc++.cfg;/usr/local/share/Cppcheck/cfg/lua.cfg;/usr/local/share/Cppcheck/cfg/mfc.cfg;/usr/local/share/Cppcheck/cfg/microsoft_atl.cfg;/usr/local/share/Cppcheck/cfg/microsoft_sal.cfg;/usr/local/share/Cppcheck/cfg/microsoft_unittest.cfg;/usr/local/share/Cppcheck/cfg/motif.cfg;/usr/local/share/Cppcheck/cfg/nspr.cfg;/usr/local/share/Cppcheck/cfg/ntl.cfg;/usr/local/share/Cppcheck/cfg/opencv2.cfg;/usr/local/share/Cppcheck/cfg/opengl.cfg;/usr/local/share/Cppcheck/cfg/openmp.cfg;/usr/local/share/Cppcheck/cfg/openssl.cfg;/usr/local/share/Cppcheck/cfg/pcre.cfg;/usr/local/share/Cppcheck/cfg/posix.cfg;/usr/local/share/Cppcheck/cfg/protobuf.cfg;/usr/local/share/Cppcheck/cfg/python.cfg;/usr/local/share/Cppcheck/cfg/qt.cfg;/usr/local/share/Cppcheck/cfg/ruby.cfg;/usr/local/share/Cppcheck/cfg/sdl.cfg;/usr/local/share/Cppcheck/cfg/selinux.cfg;/usr/local/share/Cppcheck/cfg/sfml.cfg;/usr/local/share/Cppcheck/cfg/sqlite3.cfg;/usr/local/share/Cppcheck/cfg/std.cfg;/usr/local/share/Cppcheck/cfg/tinyxml2.cfg;/usr/local/share/Cppcheck/cfg/vcl.cfg;/usr/local/share/Cppcheck/cfg/windows.cfg;/usr/local/share/Cppcheck/cfg/wxsqlite3.cfg;/usr/local/share/Cppcheck/cfg/wxsvg.cfg;/usr/local/share/Cppcheck/cfg/wxwidgets.cfg;/usr/local/share/Cppcheck/cfg/zephyr.cfg;/usr/local/share/Cppcheck/cfg/zlib.cfg")
  if(CMAKE_WARN_ON_ABSOLUTE_INSTALL_DESTINATION)
    message(WARNING "ABSOLUTE path INSTALL DESTINATION : ${CMAKE_ABSOLUTE_DESTINATION_FILES}")
  endif()
  if(CMAKE_ERROR_ON_ABSOLUTE_INSTALL_DESTINATION)
    message(FATAL_ERROR "ABSOLUTE path INSTALL DESTINATION forbidden (by caller): ${CMAKE_ABSOLUTE_DESTINATION_FILES}")
  endif()
  file(INSTALL DESTINATION "/usr/local/share/Cppcheck/cfg" TYPE FILE FILES
    "/Users/solo/pro/UserFB/workspace/Z1230/attempts/20261111T163631180204Z/repo/cfg/avr.cfg"
    "/Users/solo/pro/UserFB/workspace/Z1230/attempts/20261111T163631180204Z/repo/cfg/bento4.cfg"
    "/Users/solo/pro/UserFB/workspace/Z1230/attempts/20261111T163631180204Z/repo/cfg/boost.cfg"
    "/Users/solo/pro/UserFB/workspace/Z1230/attempts/20261111T163631180204Z/repo/cfg/bsd.cfg"
    "/Users/solo/pro/UserFB/workspace/Z1230/attempts/20261111T163631180204Z/repo/cfg/cairo.cfg"
    "/Users/solo/pro/UserFB/workspace/Z1230/attempts/20261111T163631180204Z/repo/cfg/cppcheck-lib.cfg"
    "/Users/solo/pro/UserFB/workspace/Z1230/attempts/20261111T163631180204Z/repo/cfg/cppunit.cfg"
    "/Users/solo/pro/UserFB/workspace/Z1230/attempts/20261111T163631180204Z/repo/cfg/dpdk.cfg"
    "/Users/solo/pro/UserFB/workspace/Z1230/attempts/20261111T163631180204Z/repo/cfg/embedded_sql.cfg"
    "/Users/solo/pro/UserFB/workspace/Z1230/attempts/20261111T163631180204Z/repo/cfg/emscripten.cfg"
    "/Users/solo/pro/UserFB/workspace/Z1230/attempts/20261111T163631180204Z/repo/cfg/ginac.cfg"
    "/Users/solo/pro/UserFB/workspace/Z1230/attempts/20261111T163631180204Z/repo/cfg/gnu.cfg"
    "/Users/solo/pro/UserFB/workspace/Z1230/attempts/20261111T163631180204Z/repo/cfg/googletest.cfg"
    "/Users/solo/pro/UserFB/workspace/Z1230/attempts/20261111T163631180204Z/repo/cfg/gtk.cfg"
    "/Users/solo/pro/UserFB/workspace/Z1230/attempts/20261111T163631180204Z/repo/cfg/icu.cfg"
    "/Users/solo/pro/UserFB/workspace/Z1230/attempts/20261111T163631180204Z/repo/cfg/kde.cfg"
    "/Users/solo/pro/UserFB/workspace/Z1230/attempts/20261111T163631180204Z/repo/cfg/libcerror.cfg"
    "/Users/solo/pro/UserFB/workspace/Z1230/attempts/20261111T163631180204Z/repo/cfg/libcurl.cfg"
    "/Users/solo/pro/UserFB/workspace/Z1230/attempts/20261111T163631180204Z/repo/cfg/libsigc++.cfg"
    "/Users/solo/pro/UserFB/workspace/Z1230/attempts/20261111T163631180204Z/repo/cfg/lua.cfg"
    "/Users/solo/pro/UserFB/workspace/Z1230/attempts/20261111T163631180204Z/repo/cfg/mfc.cfg"
    "/Users/solo/pro/UserFB/workspace/Z1230/attempts/20261111T163631180204Z/repo/cfg/microsoft_atl.cfg"
    "/Users/solo/pro/UserFB/workspace/Z1230/attempts/20261111T163631180204Z/repo/cfg/microsoft_sal.cfg"
    "/Users/solo/pro/UserFB/workspace/Z1230/attempts/20261111T163631180204Z/repo/cfg/microsoft_unittest.cfg"
    "/Users/solo/pro/UserFB/workspace/Z1230/attempts/20261111T163631180204Z/repo/cfg/motif.cfg"
    "/Users/solo/pro/UserFB/workspace/Z1230/attempts/20261111T163631180204Z/repo/cfg/nspr.cfg"
    "/Users/solo/pro/UserFB/workspace/Z1230/attempts/20261111T163631180204Z/repo/cfg/ntl.cfg"
    "/Users/solo/pro/UserFB/workspace/Z1230/attempts/20261111T163631180204Z/repo/cfg/opencv2.cfg"
    "/Users/solo/pro/UserFB/workspace/Z1230/attempts/20261111T163631180204Z/repo/cfg/opengl.cfg"
    "/Users/solo/pro/UserFB/workspace/Z1230/attempts/20261111T163631180204Z/repo/cfg/openmp.cfg"
    "/Users/solo/pro/UserFB/workspace/Z1230/attempts/20261111T163631180204Z/repo/cfg/openssl.cfg"
    "/Users/solo/pro/UserFB/workspace/Z1230/attempts/20261111T163631180204Z/repo/cfg/pcre.cfg"
    "/Users/solo/pro/UserFB/workspace/Z1230/attempts/20261111T163631180204Z/repo/cfg/posix.cfg"
    "/Users/solo/pro/UserFB/workspace/Z1230/attempts/20261111T163631180204Z/repo/cfg/protobuf.cfg"
    "/Users/solo/pro/UserFB/workspace/Z1230/attempts/20261111T163631180204Z/repo/cfg/python.cfg"
    "/Users/solo/pro/UserFB/workspace/Z1230/attempts/20261111T163631180204Z/repo/cfg/qt.cfg"
    "/Users/solo/pro/UserFB/workspace/Z1230/attempts/20261111T163631180204Z/repo/cfg/ruby.cfg"
    "/Users/solo/pro/UserFB/workspace/Z1230/attempts/20261111T163631180204Z/repo/cfg/sdl.cfg"
    "/Users/solo/pro/UserFB/workspace/Z1230/attempts/20261111T163631180204Z/repo/cfg/selinux.cfg"
    "/Users/solo/pro/UserFB/workspace/Z1230/attempts/20261111T163631180204Z/repo/cfg/sfml.cfg"
    "/Users/solo/pro/UserFB/workspace/Z1230/attempts/20261111T163631180204Z/repo/cfg/sqlite3.cfg"
    "/Users/solo/pro/UserFB/workspace/Z1230/attempts/20261111T163631180204Z/repo/cfg/std.cfg"
    "/Users/solo/pro/UserFB/workspace/Z1230/attempts/20261111T163631180204Z/repo/cfg/tinyxml2.cfg"
    "/Users/solo/pro/UserFB/workspace/Z1230/attempts/20261111T163631180204Z/repo/cfg/vcl.cfg"
    "/Users/solo/pro/UserFB/workspace/Z1230/attempts/20261111T163631180204Z/repo/cfg/windows.cfg"
    "/Users/solo/pro/UserFB/workspace/Z1230/attempts/20261111T163631180204Z/repo/cfg/wxsqlite3.cfg"
    "/Users/solo/pro/UserFB/workspace/Z1230/attempts/20261111T163631180204Z/repo/cfg/wxsvg.cfg"
    "/Users/solo/pro/UserFB/workspace/Z1230/attempts/20261111T163631180204Z/repo/cfg/wxwidgets.cfg"
    "/Users/solo/pro/UserFB/workspace/Z1230/attempts/20261111T163631180204Z/repo/cfg/zephyr.cfg"
    "/Users/solo/pro/UserFB/workspace/Z1230/attempts/20261111T163631180204Z/repo/cfg/zlib.cfg"
    )
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "headers" OR NOT CMAKE_INSTALL_COMPONENT)
  list(APPEND CMAKE_ABSOLUTE_DESTINATION_FILES
   "/usr/local/share/Cppcheck/platforms/aix_ppc64.xml;/usr/local/share/Cppcheck/platforms/arm32-wchar_t2.xml;/usr/local/share/Cppcheck/platforms/arm32-wchar_t4.xml;/usr/local/share/Cppcheck/platforms/arm64-wchar_t2.xml;/usr/local/share/Cppcheck/platforms/arm64-wchar_t4.xml;/usr/local/share/Cppcheck/platforms/avr8.xml;/usr/local/share/Cppcheck/platforms/cray_sv1.xml;/usr/local/share/Cppcheck/platforms/elbrus-e1cp.xml;/usr/local/share/Cppcheck/platforms/mips32.xml;/usr/local/share/Cppcheck/platforms/msp430_eabi_large_datamodel.xml;/usr/local/share/Cppcheck/platforms/pic16.xml;/usr/local/share/Cppcheck/platforms/pic8-enhanced.xml;/usr/local/share/Cppcheck/platforms/pic8.xml;/usr/local/share/Cppcheck/platforms/unix32-unsigned.xml;/usr/local/share/Cppcheck/platforms/unix64-unsigned.xml")
  if(CMAKE_WARN_ON_ABSOLUTE_INSTALL_DESTINATION)
    message(WARNING "ABSOLUTE path INSTALL DESTINATION : ${CMAKE_ABSOLUTE_DESTINATION_FILES}")
  endif()
  if(CMAKE_ERROR_ON_ABSOLUTE_INSTALL_DESTINATION)
    message(FATAL_ERROR "ABSOLUTE path INSTALL DESTINATION forbidden (by caller): ${CMAKE_ABSOLUTE_DESTINATION_FILES}")
  endif()
  file(INSTALL DESTINATION "/usr/local/share/Cppcheck/platforms" TYPE FILE FILES
    "/Users/solo/pro/UserFB/workspace/Z1230/attempts/20261111T163631180204Z/repo/platforms/aix_ppc64.xml"
    "/Users/solo/pro/UserFB/workspace/Z1230/attempts/20261111T163631180204Z/repo/platforms/arm32-wchar_t2.xml"
    "/Users/solo/pro/UserFB/workspace/Z1230/attempts/20261111T163631180204Z/repo/platforms/arm32-wchar_t4.xml"
    "/Users/solo/pro/UserFB/workspace/Z1230/attempts/20261111T163631180204Z/repo/platforms/arm64-wchar_t2.xml"
    "/Users/solo/pro/UserFB/workspace/Z1230/attempts/20261111T163631180204Z/repo/platforms/arm64-wchar_t4.xml"
    "/Users/solo/pro/UserFB/workspace/Z1230/attempts/20261111T163631180204Z/repo/platforms/avr8.xml"
    "/Users/solo/pro/UserFB/workspace/Z1230/attempts/20261111T163631180204Z/repo/platforms/cray_sv1.xml"
    "/Users/solo/pro/UserFB/workspace/Z1230/attempts/20261111T163631180204Z/repo/platforms/elbrus-e1cp.xml"
    "/Users/solo/pro/UserFB/workspace/Z1230/attempts/20261111T163631180204Z/repo/platforms/mips32.xml"
    "/Users/solo/pro/UserFB/workspace/Z1230/attempts/20261111T163631180204Z/repo/platforms/msp430_eabi_large_datamodel.xml"
    "/Users/solo/pro/UserFB/workspace/Z1230/attempts/20261111T163631180204Z/repo/platforms/pic16.xml"
    "/Users/solo/pro/UserFB/workspace/Z1230/attempts/20261111T163631180204Z/repo/platforms/pic8-enhanced.xml"
    "/Users/solo/pro/UserFB/workspace/Z1230/attempts/20261111T163631180204Z/repo/platforms/pic8.xml"
    "/Users/solo/pro/UserFB/workspace/Z1230/attempts/20261111T163631180204Z/repo/platforms/unix32-unsigned.xml"
    "/Users/solo/pro/UserFB/workspace/Z1230/attempts/20261111T163631180204Z/repo/platforms/unix64-unsigned.xml"
    )
endif()

string(REPLACE ";" "\n" CMAKE_INSTALL_MANIFEST_CONTENT
       "${CMAKE_INSTALL_MANIFEST_FILES}")
if(CMAKE_INSTALL_LOCAL_ONLY)
  file(WRITE "/Users/solo/pro/UserFB/workspace/Z1230/attempts/20261111T163631180204Z/repo/.userfb-build/cli/install_local_manifest.txt"
     "${CMAKE_INSTALL_MANIFEST_CONTENT}")
endif()
