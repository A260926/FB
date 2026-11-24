cmake_minimum_required(VERSION 3.1.3)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

project (RenderHelp VERSION 1.0.0)

file(GLOB SAMPLE_HEAD_FILE ${CMAKE_CURRENT_SOURCE_DIR}/*.h)
file(GLOB SAMPLE_SOURCE_FILE ${CMAKE_CURRENT_SOURCE_DIR}/*.cpp)

# moment_oit.cpp 是矩透明帧的库实现（无 main），由 examples/moment_oit.cpp
# 与 tests/moment_oit.cpp 链接，不作为独立示例可执行文件
list(REMOVE_ITEM SAMPLE_SOURCE_FILE
    ${CMAKE_CURRENT_SOURCE_DIR}/moment_oit.cpp)

# 矩透明示例：链接库实现
add_executable(moment_oit_example
    examples/moment_oit.cpp moment_oit.cpp moment_oit.h RenderHelp.h)
target_include_directories(moment_oit_example PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR})

foreach(SAMPLE_MAIN_FILE IN LISTS SAMPLE_SOURCE_FILE)
    get_filename_component(SAMPLE_NAME ${SAMPLE_MAIN_FILE} NAME_WE)
    add_executable(${SAMPLE_NAME} ${SAMPLE_MAIN_FILE} ${SAMPLE_HEAD_FILE})
endforeach()
