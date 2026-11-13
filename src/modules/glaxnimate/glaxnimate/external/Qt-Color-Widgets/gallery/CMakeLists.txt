# SPDX-FileCopyrightText: 2013-2025 Mattia Basaglia <dev@dragon.best>
# SPDX-License-Identifier: BSD-2-Clause

set (QT_SUPPORTED_VERSIONS 5)


set(SCREENSHOT_SOURCES screenshot.cpp)
set(SCREENSHOT_BINARY screenshot_bin)

add_executable(${SCREENSHOT_BINARY} EXCLUDE_FROM_ALL ${SCREENSHOT_SOURCES})

target_link_libraries(
    ${SCREENSHOT_BINARY}
    PRIVATE
    ${COLOR_WIDGETS_LIBRARY}
    Qt${QT_VERSION_MAJOR}::Widgets
)

add_custom_target(gallery
    COMMAND ${CMAKE_CURRENT_BINARY_DIR}/${SCREENSHOT_BINARY}
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    DEPENDS ${SCREENSHOT_BINARY}
)
