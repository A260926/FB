# Copyright 2015-2019 Mattia Basaglia
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


# Variables:
#   COVERAGE_TOOL - (lcov or gcovr) Which coverage reporting tool to use
#   COVERAGE_SOURCE_BASE_DIR - Base dir to extract the sources from for coverage
#   COVERAGE_REMOVE_PATTERNS  - Patterns to remove from the trace file
#   COVERAGE_FILTER_PATTERNS  - Patterns to keep in the trace file
#

if(NOT DEFINED COVERAGE_TARGET_PREFIX)
    set(COVERAGE_TARGET_PREFIX tests_)
endif()

enable_testing()
add_custom_target(${COVERAGE_TARGET_PREFIX}compile
    COMMENT "Building all tests"
)

add_custom_target(${COVERAGE_TARGET_PREFIX}run
    COMMAND ctest -V
    DEPENDS ${COVERAGE_TARGET_PREFIX}compile
    COMMENT "Running all tests"
)

set(TRACEFILE "${CMAKE_BINARY_DIR}/coverage.info")
set(COVERAGE_DIR "${CMAKE_BINARY_DIR}/coverage")

if(NOT COVERAGE_SOURCE_BASE_DIR)
    set(COVERAGE_SOURCE_BASE_DIR "${CMAKE_SOURCE_DIR}")
endif()

if (NOT COVERAGE_TOOL)
    set(COVERAGE_TOOL "lcov")
endif()

message(STATUS "Coverage tool is ${COVERAGE_TOOL}")

if (${COVERAGE_TOOL} STREQUAL lcov)
    add_custom_target(${COVERAGE_TARGET_PREFIX}coverage
        COMMAND cd ${CMAKE_BINARY_DIR}
        COMMAND lcov -c -d "${CMAKE_CURRENT_BINARY_DIR}" -b "${COVERAGE_SOURCE_BASE_DIR}" -o ${TRACEFILE} --no-external --rc lcov_branch_coverage=1
        COMMAND lcov --remove ${TRACEFILE} ${COVERAGE_REMOVE_PATTERNS} -o ${TRACEFILE} --rc lcov_branch_coverage=1
        COMMAND genhtml ${TRACEFILE} -o ${COVERAGE_DIR} -p "${COVERAGE_SOURCE_BASE_DIR}" --demangle-cpp --branch-coverage --rc genhtml_hi_limit=80
        DEPENDS ${COVERAGE_TARGET_PREFIX}run
    )
elseif(${COVERAGE_TOOL} STREQUAL gcovr)
    set(GCOVR_CMD
        gcovr --root ${COVERAGE_SOURCE_BASE_DIR} --object-directory ${CMAKE_BINARY_DIR}
        -s --branches --exclude-throw-branches
        --html --html-details "${COVERAGE_DIR}/index.html"
    )

    foreach(remove IN ITEMS ${COVERAGE_REMOVE_PATTERNS})
        list(APPEND GCOVR_CMD -e ${remove})
    endforeach()

    foreach(filter IN ITEMS ${COVERAGE_FILTER_PATTERNS})
        list(APPEND GCOVR_CMD -f ${filter})
    endforeach()

    add_custom_target(${COVERAGE_TARGET_PREFIX}coverage
        COMMAND cd ${CMAKE_BINARY_DIR}
        COMMAND mkdir -p ${COVERAGE_DIR}
        COMMAND ${GCOVR_CMD}
        DEPENDS ${COVERAGE_TARGET_PREFIX}run
    )
else()
endif()

add_custom_target(${COVERAGE_TARGET_PREFIX}coverage_view
    COMMAND xdg-open "${COVERAGE_DIR}/index.html"
)
