# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2000-2024 Kitware, Inc. and Contributors
# Source: https://gitlab.kitware.com/cmake/community/-/wikis/FAQ#can-i-do-make-uninstall-with-cmake

if(NOT EXISTS "/Volumes/UserFBWorkspace/questions/FS5963D6D5F777U16842/attempts/20261127T161606663927Z/repo/.build/install_manifest.txt")
  message(FATAL_ERROR "Cannot find install manifest: /Volumes/UserFBWorkspace/questions/FS5963D6D5F777U16842/attempts/20261127T161606663927Z/repo/.build/install_manifest.txt")
endif()

file(READ "/Volumes/UserFBWorkspace/questions/FS5963D6D5F777U16842/attempts/20261127T161606663927Z/repo/.build/install_manifest.txt" files)
string(REGEX REPLACE "\n" ";" files "${files}")
foreach(file ${files})
  message(STATUS "Uninstalling $ENV{DESTDIR}${file}")
  if(IS_SYMLINK "$ENV{DESTDIR}${file}" OR EXISTS "$ENV{DESTDIR}${file}")
    exec_program(
      "/opt/homebrew/bin/cmake" ARGS
      "-E remove \"$ENV{DESTDIR}${file}\""
      OUTPUT_VARIABLE rm_out
      RETURN_VALUE rm_retval)
    if(NOT "${rm_retval}" STREQUAL 0)
      message(FATAL_ERROR "Problem when removing $ENV{DESTDIR}${file}")
    endif()
  else(IS_SYMLINK "$ENV{DESTDIR}${file}" OR EXISTS "$ENV{DESTDIR}${file}")
    message(STATUS "File $ENV{DESTDIR}${file} does not exist.")
  endif()
endforeach()
