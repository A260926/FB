# SoX Resampler Library       Copyright (c) 2007-13 robs@users.sourceforge.net
# Licence for this file: LGPL v2.1                  See LICENCE for details.

if (NOT EXISTS "/Users/solo/pro/UserFB/workspace/FSE41FEA1E43C5U6918/attempts/20261113T202029870395Z/repo/.task-build/lib/install_manifest.txt")
  message (FATAL_ERROR "Cannot find install manifest")
endif ()

file (READ "/Users/solo/pro/UserFB/workspace/FSE41FEA1E43C5U6918/attempts/20261113T202029870395Z/repo/.task-build/lib/install_manifest.txt" files)
string (REGEX REPLACE "\n" ";" files "${files}")
foreach (file ${files})
  set (dest "$ENV{DESTDIR}${file}")
  message (STATUS "Deinstalling \"${dest}\"")
  if (EXISTS "${dest}" OR IS_SYMLINK "${dest}")
    execute_process (
      COMMAND "/opt/homebrew/bin/cmake" -E remove "${dest}"
      OUTPUT_VARIABLE rm_out
      RESULT_VARIABLE rm_retval
    )
    if (NOT ${rm_retval} EQUAL 0)
      message (FATAL_ERROR "Problem when removing \"${dest}\"")
    endif ()
  else ()
    message (STATUS "File \"${dest}\" does not exist.")
  endif ()
endforeach ()
