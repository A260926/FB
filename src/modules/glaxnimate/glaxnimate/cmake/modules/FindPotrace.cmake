#
# Copyright (C) 2015-2020 Mattia Basaglia
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program.  If not, see <http://www.gnu.org/licenses/>.
#
# Tries to find libpotrace
#
# Input:
#   Potrace_PREFIX
#   Potrace_LIBRARIES
#   Potrace_INCLUDE_DIRS
# Output:
#   Potrace_FOUND
#   Potrace_LIBRARIES
#   Potrace_INCLUDE_DIRS

set(Potrace_PREFIX "${CMAKE_SYSTEM_PREFIX_PATH}" CACHE PATH "libpotrace installation prefix")
set(Potrace_HEADER_BASENAME potracelib.h)

if(Potrace_LIBRARIES AND Potrace_INCLUDE_DIRS)
    set(Potrace_FOUND TRUE)
else()
    find_path(
        Potrace_INCLUDE_DIR
        NAMES
            ${Potrace_HEADER_BASENAME}
        PATHS
            /usr/include
            /usr/local/include
            /usr/local/homebrew/opt/potrace/include
            ${Potrace_PREFIX}/include
    )

    find_library(
        Potrace_LIBRARY
        NAMES
            potrace
            libpotrace
            potracelib
        PATHS
            /usr/lib
            /usr/local/lib
            /usr/lib/x86_64-linux-gnu
            /usr/local/lib/x86_64-linux-gnu
            /usr/local/homebrew/opt/potrace/lib
            ${Potrace_PREFIX}/lib
        )

    if(Potrace_INCLUDE_DIR AND Potrace_LIBRARY)
        set(Potrace_FOUND TRUE)
        set(Potrace_LIBRARIES ${Potrace_LIBRARIES} ${Potrace_LIBRARY})
        set(Potrace_INCLUDE_DIRS ${Potrace_INCLUDE_DIRS} ${Potrace_INCLUDE_DIR})
        message(STATUS "Found Potrace")
    else()
        set(Potrace_FOUND FALSE)
        set(Potrace_ERROR_MESSAGE "Potrace not found!")
        if(Potrace_FIND_REQUIRED)
            message(FATAL_ERROR ${Potrace_ERROR_MESSAGE})
        elseif(NOT Potrace_FIND_QUIETLY)
            message(STATUS ${Potrace_ERROR_MESSAGE})
        endif()
    endif()
endif()
