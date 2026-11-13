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
# Tries to find the cURL library

set(cURL_PREFIX "${CMAKE_SYSTEM_PREFIX_PATH}" CACHE PATH "cURL installation prefix")
set(cURL_HEADER_BASENAME curl/curl.h)

find_path(cURL_INCLUDE_DIR ${cURL_HEADER_BASENAME} PATHS ${cURL_PREFIX})
find_library(cURL_LIBRARY NAMES curl PATHS ${cURL_PREFIX})

set(cURL_ERROR_MESSAGE "")
if(cURL_LIBRARY AND cURL_INCLUDE_DIR)
    set(cURL_FOUND TRUE)

    execute_process(
        COMMAND curl-config --version | grep -E "[0-9.]+"
        OUTPUT_VARIABLE cURL_VERSION_STRING
        ERROR_QUIET
    )
    set(cURL_LIBRARIES ${cURL_LIBRARY})
    if (cURL_FIND_VERSION AND cURL_FIND_VERSION STRGREATER cURL_VERSION_STRING)
        set(cURL_FOUND FALSE)
        set(cURL_ERROR_MESSAGE "cURL not found (required was ${cURL_FIND_VERSION} but found ${cURL_VERSION_STRING})")
    endif()
else()
    set(cURL_ERROR_MESSAGE "cURL not found!")
    set(cURL_FOUND FALSE)
endif()

if(cURL_FOUND)
    message(STATUS "cURL Version: ${cURL_VERSION_STRING}")
else()
    if(cURL_FIND_REQUIRED)
        message(FATAL_ERROR ${cURL_ERROR_MESSAGE})
    elseif(NOT cURL_FIND_QUIETLY)
        message(STATUS ${cURL_ERROR_MESSAGE})
    endif()
endif()
