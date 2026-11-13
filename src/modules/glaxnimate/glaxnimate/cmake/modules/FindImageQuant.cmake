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
# Tries to find libav
#
# Input:
#   ImageQuant_PREFIX
#   ImageQuant_LIBRARIES
#   ImageQuant_INCLUDE_DIRS
#
# Output:
#   ImageQuant_FOUND
#   ImageQuant_LIBRARIES
#   ImageQuant_INCLUDE_DIRS
# For each component:
#   ImageQuant_<component>_FOUND
#   ImageQuant_<component>_INCLUDE_DIR
#   ImageQuant_<component>_LIBRARY


set(ImageQuant_PREFIX "${CMAKE_SYSTEM_PREFIX_PATH}" CACHE PATH "libimagequant installation prefix")

if(ImageQuant_LIBRARIES AND ImageQuant_INCLUDE_DIRS)
    set(ImageQuant_FOUND TRUE)
else()
    set(ImageQuant_LIBRARIES)
    set(ImageQuant_INCLUDE_DIRS)

    find_path(
        ImageQuant_INCLUDE_DIRS
        NAMES
            "libimagequant.h"
        PATHS
            ${ImageQuant_PREFIX}/include
            /usr/local/include
            /usr/include
            /usr/local/homebrew/opt/imagequant/include/
    )

    find_library(
        ImageQuant_LIBRARIES
        NAMES
            imagequant
        PATHS
            ${ImageQuant_PREFIX}/lib
            /usr/local/lib
            /usr/local/lib/x86_64-linux-gnu
            /usr/lib
            /usr/lib/x86_64-linux-gnu
            /usr/local/homebrew/opt/imagequant/lib/
    )

    list(REMOVE_DUPLICATES ImageQuant_INCLUDE_DIRS)

    include(FindPackageHandleStandardArgs)
    find_package_handle_standard_args(
        ImageQuant
        REQUIRED_VARS
            ImageQuant_LIBRARIES
            ImageQuant_INCLUDE_DIRS
        HANDLE_COMPONENTS
    )
endif()
