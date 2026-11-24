cmake_minimum_required(VERSION 3.10)
project(TimeCodeProductionBinding LANGUAGES C CXX)
set(BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(BUILD_UTILS OFF CACHE BOOL "" FORCE)
add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/.." production)
# Required production implementation is compiled into the real library even if a build list is missed.
target_sources(mp4v2 PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../src/mp4timecode.cpp")
