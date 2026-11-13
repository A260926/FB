# SPDX-FileCopyrightText: 2013-2025 Mattia Basaglia <dev@dragon.best>
# SPDX-License-Identifier: BSD-2-Clause

cmake_minimum_required (VERSION 3.1...3.27 FATAL_ERROR)

if (CMAKE_VERSION VERSION_GREATER_EQUAL "3.13.0")
    cmake_policy(SET CMP0077 NEW)
endif()

set(COLORWIDGET_PROJECT_NAME QtColorWidgets)
project(${COLORWIDGET_PROJECT_NAME} CXX)


set (CMAKE_MODULE_PATH
  ${CMAKE_MODULE_PATH}
  ${CMAKE_CURRENT_SOURCE_DIR}/cmake/modules
  ${CMAKE_CURRENT_LIST_DIR})


include (CheckCXXCompilerFlag)

include (cmake/install.cmake)
include (cmake/versioning.cmake)


SET (${COLORWIDGET_PROJECT_NAME}_VERSION_MAJOR 2)
SET (${COLORWIDGET_PROJECT_NAME}_VERSION_MINOR 2)
SET (${COLORWIDGET_PROJECT_NAME}_VERSION_PATCH 0)
SET (${COLORWIDGET_PROJECT_NAME}_VERSION "${${COLORWIDGET_PROJECT_NAME}_VERSION_MAJOR}.${${COLORWIDGET_PROJECT_NAME}_VERSION_MINOR}.${${COLORWIDGET_PROJECT_NAME}_VERSION_PATCH}")


if(NOT QT_VERSION_MAJOR)
    set(QT_VERSION_MAJOR 5 CACHE STRING "Qt version to use (4, 5 or 6), defaults to 5")
endif()
find_package(Qt${QT_VERSION_MAJOR} COMPONENTS Core Widgets REQUIRED)

# These definitions do mostly match what with what KDE software uses
# See https://api.kde.org/ecm/kde-module/KDECompilerSettings.html
add_definitions(
    -DQT_NO_URL_CAST_FROM_STRING
    -DQT_NO_CAST_TO_ASCII
    -DQT_NO_CAST_FROM_ASCII
    -DQT_NO_FOREACH
    -DQT_NO_KEYWORDS
    -DQT_USE_QSTRINGBUILDER
    -DQT_NO_CAST_FROM_BYTEARRAY
    -DQT_NO_NARROWING_CONVERSIONS_IN_CONNECT
)
if (NOT WIN32)
    add_definitions(-DQT_STRICT_ITERATORS)
endif()

# Auto generate moc files
set(CMAKE_AUTOMOC ON)
# Auto generate moc files
set(CMAKE_AUTOUIC ON)
# Auto generate moc files
set(CMAKE_AUTORCC ON)


if ( BUILD_SHARED_LIBS )
    option(QTCOLORWIDGETS_BUILD_STATIC_LIBS "Build the static library" OFF)
else()
    option(QTCOLORWIDGETS_BUILD_STATIC_LIBS "Build the static library" ON)
endif()

set (TARGET_NAME ${COLORWIDGET_PROJECT_NAME})
option(COLOR_WIDGETS_QT_SUFFIX "Append qt version to the target output files" OFF)
if ( COLOR_WIDGETS_QT_SUFFIX )
    set (TARGET_OUTPUT_SUFFIX "-Qt${QT_VERSION}${${COLORWIDGET_PROJECT_NAME}_VERSION_MAJOR}")
else()
    set (TARGET_OUTPUT_SUFFIX "")
endif()

set (INCLUDE_PREFIX "QtColorWidgets")
set (COLOR_WIDGETS_LIBRARY "${TARGET_NAME}")

if ( ${QTCOLORWIDGETS_BUILD_STATIC_LIBS} )
    add_library (${TARGET_NAME} STATIC "")
    target_compile_definitions(${TARGET_NAME} PUBLIC QTCOLORWIDGETS_STATICALLY_LINKED)
else()
    add_library (${TARGET_NAME} "")
endif()


set_target_properties(${TARGET_NAME}
  PROPERTIES
  EXPORT_NAME "${TARGET_NAME}${TARGET_OUTPUT_SUFFIX}")


set_target_properties (${TARGET_NAME}
    PROPERTIES
    CXX_STANDARD 11
    CXX_STANDARD_REQUIRED ON
    VERSION ${${COLORWIDGET_PROJECT_NAME}_VERSION}
    SOVERSION ${${COLORWIDGET_PROJECT_NAME}_VERSION_MAJOR}
    "INTERFACE_${COLORWIDGET_PROJECT_NAME}_MAJOR_VERSION" ${${COLORWIDGET_PROJECT_NAME}_VERSION_MAJOR}
    COMPATIBLE_INTERFACE_STRING "${COLORWIDGET_PROJECT_NAME}_MAJOR_VERSION"
    COMPILE_DEFINITIONS QTCOLORWIDGETS_LIBRARY
    OUTPUT_NAME "${TARGET_NAME}${TARGET_OUTPUT_SUFFIX}")

check_cxx_compiler_flag ("-Wall" Wall_FLAG_SUPPORTED)

if (Wall_FLAG_SUPPORTED)
  set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -Wall")
endif (Wall_FLAG_SUPPORTED)

check_cxx_compiler_flag ("-pedantic" pedantic_FLAG_SUPPORTED)

if (pedantic_FLAG_SUPPORTED)
    set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -pedantic")
endif (pedantic_FLAG_SUPPORTED)

check_cxx_compiler_flag ("-Wextra" Wextra_FLAG_SUPPORTED)

if (Wextra_FLAG_SUPPORTED)
  set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -Wextra")
endif (Wextra_FLAG_SUPPORTED)


set (EXPORT_HEADER "${INCLUDE_PREFIX}/colorwidgets_global.hpp")


generate_versioning_information (
    TARGET_NAME ${TARGET_NAME}
    EXPORT_HEADER ${EXPORT_HEADER}
    EXPORT_MACRO QCP_EXPORT
    VERSIONED_ENTITY ${COLORWIDGET_PROJECT_NAME}
    INCLUDE_PREFIX ${INCLUDE_PREFIX}
    COMPANY_NAME "Mattia Basaglia"
    COMPANY_COPYRIGHT "Mattia Basaglia Copyright (C) 2013-2025"
    FILE_DESCRIPTION "Color wheel widget and dialog for Qt${QT_VERSION}"
)


target_include_directories(${TARGET_NAME}
    PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
    PUBLIC
    $<BUILD_INTERFACE:${PROJECT_BINARY_DIR}>
    $<INSTALL_INTERFACE:include>
)


add_subdirectory (${CMAKE_CURRENT_SOURCE_DIR}/include/${INCLUDE_PREFIX})
add_subdirectory (${CMAKE_CURRENT_SOURCE_DIR}/resources/${INCLUDE_PREFIX})
add_subdirectory (${CMAKE_CURRENT_SOURCE_DIR}/src/${INCLUDE_PREFIX})


target_link_libraries (${TARGET_NAME} PRIVATE Qt${QT_VERSION_MAJOR}::Widgets)


install_project (
    PROJECT_NAME ${COLORWIDGET_PROJECT_NAME}
    TARGET_NAME ${TARGET_NAME}
    TARGET_OUTPUT_SUFFIX "${TARGET_OUTPUT_SUFFIX}"
    EXPORT_HEADER ${EXPORT_HEADER}
    INCLUDE_PREFIX ${INCLUDE_PREFIX}
    HEADER_MATCHING_REGEX "^.*\.h$|^.*\.hpp$|^.*$"
    VERSION_HEADER ${CMAKE_CURRENT_BINARY_DIR}/${COLORWIDGET_PROJECT_NAME}_version.h
    NAMESPACE ""
)


add_subdirectory (gallery)

option(QTCOLORWIDGETS_DESIGNER_PLUGIN "Build QtDesigner plugin" ON)
if (${QTCOLORWIDGETS_DESIGNER_PLUGIN})
    find_package (Qt${QT_VERSION_MAJOR}Designer QUIET)
    if (Qt${QT_VERSION_MAJOR}Designer_FOUND)
        add_subdirectory (color_widgets_designer_plugin)
    endif(Qt${QT_VERSION_MAJOR}Designer_FOUND)
endif()

configure_file(Doxyfile.in ${CMAKE_CURRENT_BINARY_DIR}/Doxyfile)
add_custom_target(
    ${COLORWIDGET_PROJECT_NAME}_docs
    COMMAND doxygen Doxyfile
    COMMAND cp ${CMAKE_CURRENT_SOURCE_DIR}/gallery/*.png ${CMAKE_CURRENT_BINARY_DIR}/doxygen/html
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
)

