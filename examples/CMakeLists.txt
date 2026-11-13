CMAKE_MINIMUM_REQUIRED(VERSION 3.22)

PROJECT(depex_offline_example LANGUAGES C CXX)

# Standalone offline example: only C++ and the in-tree common/ sources are
# used. There is no Qt dependency and no third-party library is required.
SET(CMAKE_CXX_STANDARD 11)
SET(CMAKE_CXX_STANDARD_REQUIRED ON)
SET(CMAKE_CXX_EXTENSIONS OFF)

SET(COMMON_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../common)

ADD_EXECUTABLE(depex_offline_example
 depex_offline_example.cpp
 ${COMMON_DIR}/depex.cpp
 ${COMMON_DIR}/treemodel.cpp
 ${COMMON_DIR}/treeitem.cpp
 ${COMMON_DIR}/types.cpp
 ${COMMON_DIR}/ffs.cpp
 ${COMMON_DIR}/guiddatabase.cpp
 ${COMMON_DIR}/ustring.cpp
 ${COMMON_DIR}/bstrlib/bstrlib.c
 ${COMMON_DIR}/bstrlib/bstrwrap.cpp
)

TARGET_INCLUDE_DIRECTORIES(depex_offline_example PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
