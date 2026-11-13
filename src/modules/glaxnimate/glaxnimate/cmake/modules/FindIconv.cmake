#
# Copyright (C) 2015-2017 Mattia Basaglia
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
# Tries to find the Iconv library

set(Iconv_PREFIX "${CMAKE_SYSTEM_PREFIX_PATH}" CACHE PATH "Iconv installation prefix")
set(Iconv_HEADER_BASENAME iconv.h)

find_path(Iconv_INCLUDE_DIR ${Iconv_HEADER_BASENAME} PATHS ${Iconv_PREFIX})
find_library(Iconv_LIBRARY NAMES iconv PATHS ${Iconv_PREFIX})

if(Iconv_INCLUDE_DIR)
    message(STATUS "Found Iconv")
    set(Iconv_FOUND TRUE)
    if (NOT Iconv_LIBRARY)
        set(Iconv_LIBRARY "")
    endif()
else()
    set(Iconv_FOUND FALSE)
    set(Iconv_ERROR_MESSAGE "Iconv not found!")
    if(Iconv_FIND_REQUIRED)
        message(FATAL_ERROR ${Iconv_ERROR_MESSAGE})
    elseif(NOT Iconv_FIND_QUIETLY)
        message(STATUS ${Iconv_ERROR_MESSAGE})
    endif()
endif()
