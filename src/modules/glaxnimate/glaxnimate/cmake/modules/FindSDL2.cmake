# Copyright 2016-2020 Mattia Basaglia
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
# =====
# Finds the SDL2 library and various components
#
# Defines the following variable
# SDL2_FOUND        Whether the ibrary has been found
# SDL2_LIBRARIES    A list of linker options
# SDL2_INCLUDE_DIRS A list of paths to include
# SDL2_VERSION      Version string in the form "major.minor.patch"
#
# It supports finding SDL2 components, a component is a library that follows the
# following pattern:
# Header in SDL2/SDL_(name).h
# Library as libSDL2_(name)
#
# Some example of these components are SDL_image and SDL_ttf
# (which would be called "image" and "ttf" respectively)
#
# The component "main" links to SDLMain.

# Like find_path() buth tailored to work with SDL2 include paths
function(sdl2_find_include_path output_var header_name)
    find_path(${output_var}_tmp ${header_name}
        HINTS
        ENV SDL2DIR
        PATH_SUFFIXES include/SDL2 include
        PATHS
        ~/Library/Frameworks
        /Library/Frameworks
        /usr/local/include/SDL2
        /usr/include/SDL2
        /sw # Fink
        /opt/local # DarwinPorts
        /opt/csw # Blastwave
        /opt
    )
    set(${output_var} ${${output_var}_tmp} PARENT_SCOPE)
endfunction()

# Like find_library() but tailored to work with SDL2 libraries
function(sdl2_find_library output_var lib_name)
    find_library(${output_var}_tmp
        NAMES ${lib_name}
        HINTS
        ENV SDL2DIR
        PATH_SUFFIXES lib64 lib
        PATHS
        /sw
        /opt/local
        /opt/csw
        /opt
    )
    set(${output_var} ${${output_var}_tmp} PARENT_SCOPE)
endfunction()

# Helper function to parse version macros from a header
# Arguments:
#   header_path:    Full path to the header file to parse
#   macro_prefix:   Prefix of the _MAJOR_VERSION macro and friends
#   output_var:     Variable to store the version insto
function(sdl2_parse_version header_path macro_prefix output_var)
    if(EXISTS ${header_path})
        file(STRINGS ${header_path} SDL2_COMP_VERSION_MAJOR_LINE REGEX "^#define[ \t]+${macro_prefix}_MAJOR_VERSION[ \t]+[0-9]+$")
        file(STRINGS ${header_path} SDL2_COMP_VERSION_MINOR_LINE REGEX "^#define[ \t]+${macro_prefix}_MINOR_VERSION[ \t]+[0-9]+$")
        file(STRINGS ${header_path} SDL2_COMP_VERSION_PATCH_LINE REGEX "^#define[ \t]+${macro_prefix}_PATCHLEVEL[ \t]+[0-9]+$")
        string(REGEX REPLACE "^#define[ \t]+${macro_prefix}_MAJOR_VERSION[ \t]+([0-9]+)$" "\\1" SDL2_COMP_VERSION_MAJOR "${SDL2_COMP_VERSION_MAJOR_LINE}")
        string(REGEX REPLACE "^#define[ \t]+${macro_prefix}_MINOR_VERSION[ \t]+([0-9]+)$" "\\1" SDL2_COMP_VERSION_MINOR "${SDL2_COMP_VERSION_MINOR_LINE}")
        string(REGEX REPLACE "^#define[ \t]+${macro_prefix}_PATCHLEVEL[ \t]+([0-9]+)$"    "\\1" SDL2_COMP_VERSION_PATCH "${SDL2_COMP_VERSION_PATCH_LINE}")
        set(${output_var} ${SDL2_COMP_VERSION_MAJOR}.${SDL2_COMP_VERSION_MINOR}.${SDL2_COMP_VERSION_PATCH} PARENT_SCOPE)
    endif()
endfunction()

# Helper function to find SDL2 components
# Outputs the given variables (all uppercase):
#   SDL2_(ID)_FOUND         (Boolean, whether the component has been found)
#   SDL2_(ID)_VERSION       (String, the detected version as "major.minor.patch")
#   SDL2_(ID)_INCLUDE_DIRS  (Cached, List of paths to include)
#   SDL2_(ID)_LIBRARIES     (Cached, List of libraries to include)
function(sdl2_find_component SDL2_COMPONENT_ID)
    set(SDL2_VARPREFIX "SDL2_${SDL2_COMPONENT_ID}")
    set(SDL2_COMPONENT_HEADER "SDL_${SDL2_COMPONENT_ID}.h")

    sdl2_find_include_path(${SDL2_VARPREFIX}_INCLUDE_DIR ${SDL2_COMPONENT_HEADER})
    sdl2_find_library(${SDL2_VARPREFIX}_LIBRARY "SDL2_${SDL2_COMPONENT_ID}")

    set(SDL2_COMPONENT_HEADER_PATH "${${SDL2_VARPREFIX}_INCLUDE_DIR}/${SDL2_COMPONENT_HEADER}")

    if(${SDL2_VARPREFIX}_INCLUDE_DIR)
        string(TOUPPER "SDL_${SDL2_COMPONENT_ID}" SDL2_COMPONENT_MACRO)
        sdl2_parse_version(${SDL2_COMPONENT_HEADER_PATH} ${SDL2_COMPONENT_MACRO} "${SDL2_VARPREFIX}_VERSION")
        set("${SDL2_VARPREFIX}_VERSION" ${${SDL2_VARPREFIX}_VERSION} PARENT_SCOPE)
    endif()

    if(${SDL2_VARPREFIX}_LIBRARY AND ${SDL2_VARPREFIX}_INCLUDE_DIR)
        set("${SDL2_VARPREFIX}_LIBRARIES" ${${SDL2_VARPREFIX}_LIBRARY} PARENT_SCOPE)
        set("${SDL2_VARPREFIX}_INCLUDE_DIRS" ${${SDL2_VARPREFIX}_INCLUDE_DIR} PARENT_SCOPE)
        set("${SDL2_VARPREFIX}_FOUND" ON PARENT_SCOPE)
    else()
        set("${SDL2_VARPREFIX}_FOUND" OFF PARENT_SCOPE)
    endif()
endfunction()

# Find the library proper
set(SDL2_INCLUDE_DIRS "")
sdl2_find_include_path(SDL2_INCLUDE_DIR SDL.h)
list(APPEND SDL2_INCLUDE_DIRS ${SDL2_INCLUDE_DIR})
sdl2_parse_version("${SDL2_INCLUDE_DIR}/SDL_version.h" "SDL" SDL2_VERSION)

set(SDL2_LIBRARIES "")
sdl2_find_library(SDL2_LIBRARY SDL2)

# Link extra libraries
if(SDL2_LIBRARY)
    list(APPEND SDL2_LIBRARIES ${SDL2_LIBRARY})

    # On OS X, SDL needs to link against the Cocoa framework,
    # on other systems, it needs to link against the threading library
    if(NOT APPLE)
        find_package(Threads)
        list(APPEND SDL2_LIBRARIES ${CMAKE_THREAD_LIBS_INIT})
    else()
        list(APPEND SDL2_LIBRARIES "-framework Cocoa")
    endif()

    # MinGW needs a few more libraries
    if(MINGW)
        list(APPEND SDL2_LIBRARIES "mingw32" "mwindows")
    endif()

endif()

# Search for required and optional components
if(SDL2_FIND_COMPONENTS)
    message(STATUS "Searching SDL components:")

    foreach(component ${SDL2_FIND_COMPONENTS})
        # main is a bit special, it doesn't have a header and doesn't follow
        # the library naming convention
        # SDL2_main_FOUND is for find_package_handle_standard_args
        if(${component} STREQUAL "main")
            sdl2_find_library(SDL2MAIN_LIBRARY SDL2main)
            if(SDL2_LIBRARIES)
                list(APPEND SDL2_LIBRARIES ${SDL2MAIN_LIBRARY})
                set(SDL2_main_FOUND ON)
            else()
                set(SDL2_main_FOUND OFF)
            endif()
            unset(SDL2MAIN_LIBRARY)
        # Other components use a helper function
        else()
            sdl2_find_component(${component})
            if(SDL2_${component}_FOUND)
                message(STATUS "  ${component} found ${SDL2_${component}_VERSION}")
                list(APPEND SDL2_LIBRARIES ${SDL2_${component}_LIBRARIES})
                list(APPEND SDL2_INCLUDE_DIRS ${SDL2_${component}_INCLUDE_DIR})
            else()
                message(STATUS "  ${component} not found")
            endif()
        endif()
    endforeach()
endif()


# Handle common functionality for Find* scripts
include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(
    SDL2
    REQUIRED_VARS SDL2_LIBRARY SDL2_INCLUDE_DIR
    VERSION_VAR SDL2_VERSION
    HANDLE_COMPONENTS
)

# Set up cached variables
if(SDL2_FOUND)
    set(SDL2_LIBRARIES ${SDL2_LIBRARIES} CACHE STRING "Linker flags for SDL2")
    list(REMOVE_DUPLICATES SDL2_INCLUDE_DIRS)
    set(SDL2_INCLUDE_DIRS ${SDL2_INCLUDE_DIRS} CACHE STRING "Search paths for SDL2")
endif()

mark_as_advanced(SDL2_LIBRARIES SDL2_INCLUDE_DIRS)

# Clean temporaries
unset(SDL2_INCLUDE_DIR)
unset(SDL2_LIBRARY)
