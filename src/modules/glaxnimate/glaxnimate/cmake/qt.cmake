#
# Copyright (C) 2015-2022 Mattia Basaglia
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

if (NOT DEFINED QT_VERSION_MAJOR)
    set(QT_VERSION_MAJOR 5)
endif()

function(qt_linguist_command)
    cmake_parse_arguments(PARSE_ARGV 0 ARGS "" "COMMAND;OUTPUT" "")

    string(TOUPPER ${ARGS_COMMAND} CMD_UPPER)
    set(varcheck "Qt${QT_VERSION_MAJOR}_${CMD_UPPER}_EXECUTABLE")

    if(NOT "${${varcheck}}" STREQUAL "")
        set(${ARGS_OUTPUT} ${${varcheck}} PARENT_SCOPE)
    elseif(NOT "${QT_CMAKE_EXPORT_NAMESPACE}" STREQUAL "")
        set(${ARGS_OUTPUT} "${QT_CMAKE_EXPORT_NAMESPACE}::${ARGS_COMMAND}" PARENT_SCOPE)
    else()
        find_program(program NAMES ${ARGS_COMMAND})
        set(${ARGS_OUTPUT} ${program} PARENT_SCOPE)
    endif()
endfunction()

# Creates a target to compile Qt linguist translations
# Synopsis:
#   create_qt_linguist_translations(
#       TARGET target           # Name of the target used to build/update translations
#       [DESTINATION directory] # Where to put the qm files
#       TRANSLATIONS file...    # Source files for translations
#       SOURCES directory...    # Where to find source files
#       [OUTPUT varname]        # output variable name for the list of qm files (defaults to LINGUIST_OUTPUT_FILES)
#   )
function(create_qt_linguist_translations)
    include(misc)

    set(options)
    set(one_value DESTINATION TARGET)
    set(multi_value TRANSLATIONS SOURCES)
    cmake_parse_arguments(PARSE_ARGV 0 CREATE_QT_LINGUIST "${options}" "${one_value}" "${multi_value}")
    if ( NOT CREATE_QT_LINGUIST_DESTINATION )
        set(CREATE_QT_LINGUIST_DESTINATION "${CMAKE_CURRENT_BINARY_DIR}/translations")
    endif()
    if ( NOT CREATE_QT_LINGUIST_OUTPUT )
        set(CREATE_QT_LINGUIST_OUTPUT LINGUIST_OUTPUT_FILES)
    endif()

    find_package(Qt${QT_VERSION_MAJOR}LinguistTools QUIET)
    if(${Qt${QT_VERSION_MAJOR}LinguistTools_FOUND})
        message(STATUS "Translations enabled")
        set(abs_ts)
        foreach(file ${CREATE_QT_LINGUIST_TRANSLATIONS})
            get_filename_component(file_abs ${file} ABSOLUTE BASE_DIR ${CMAKE_CURRENT_SOURCE_DIR})
            list(APPEND abs_ts ${file_abs})
        endforeach()
        add_custom_target(
            ${CREATE_QT_LINGUIST_TARGET}
            DEPENDS ${CREATE_QT_LINGUIST_TARGET}_cmd
        )

        qt_linguist_command(COMMAND lupdate OUTPUT lupdate_cmd)
        qt_linguist_command(COMMAND lrelease OUTPUT lrelease_cmd)

        message(STATUS "Qt linguist commands: '${lupdate_cmd}' '${lrelease_cmd}'")
        add_custom_command(
            OUTPUT ${CREATE_QT_LINGUIST_TARGET}_cmd
            COMMAND ${lupdate_cmd} ${CREATE_QT_LINGUIST_SOURCES} -ts ${abs_ts}
            COMMAND ${CMAKE_COMMAND} -E make_directory ${CREATE_QT_LINGUIST_DESTINATION}
        )
        set(abs_qm)
        foreach(file ${abs_ts})
            get_filename_component(file_basename ${file} NAME_WLE)
            add_custom_command(
                OUTPUT ${CREATE_QT_LINGUIST_TARGET}_cmd APPEND
                COMMAND ${lrelease_cmd} ${file} -qm ${CREATE_QT_LINGUIST_DESTINATION}/${file_basename}.qm
            )
            list(APPEND abs_qm ${CREATE_QT_LINGUIST_DESTINATION}/${file_basename}.qm)
        endforeach()
        set(${CREATE_QT_LINGUIST_OUTPUT} ${abs_qm} PARENT_SCOPE)
    else()
        set(${CREATE_QT_LINGUIST_OUTPUT} PARENT_SCOPE)
    endif()
endfunction()
