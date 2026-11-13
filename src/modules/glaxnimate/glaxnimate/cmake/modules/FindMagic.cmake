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
# Tries to find libmagic

set(Magic_PREFIX "${CMAKE_SYSTEM_PREFIX_PATH}" CACHE PATH "libmagic installation prefix")
set(Magic_HEADER_BASENAME magic.h)

find_path(Magic_INCLUDE_DIR ${Magic_HEADER_BASENAME} PATHS ${Magic_PREFIX})
find_library(Magic_LIBRARY NAMES magic PATHS ${Magic_PREFIX})

if(Magic_INCLUDE_DIR)
    set(Magic_FOUND TRUE)
    message(STATUS "Found libmagic")
else()
    set(Magic_FOUND FALSE)
    set(Magic_ERROR_MESSAGE "libmagic not found!")
    if(Magic_FIND_REQUIRED)
        message(FATAL_ERROR ${Magic_ERROR_MESSAGE})
    elseif(NOT Magic_FIND_QUIETLY)
        message(STATUS ${Magic_ERROR_MESSAGE})
    endif()
endif()
