cmake_minimum_required(VERSION 3.5)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

project (RenderHelp VERSION 1.0.0)

file(GLOB SAMPLE_HEAD_FILE ${CMAKE_CURRENT_SOURCE_DIR}/*.h)
file(GLOB SAMPLE_SOURCE_FILE ${CMAKE_CURRENT_SOURCE_DIR}/*.cpp)

# moment_oit.cpp 是库实现（没有 main），从示例可执行文件里排除
list(REMOVE_ITEM SAMPLE_SOURCE_FILE ${CMAKE_CURRENT_SOURCE_DIR}/moment_oit.cpp)

foreach(SAMPLE_MAIN_FILE IN LISTS SAMPLE_SOURCE_FILE)
    get_filename_component(SAMPLE_NAME ${SAMPLE_MAIN_FILE} NAME_WE)
    add_executable(${SAMPLE_NAME} ${SAMPLE_MAIN_FILE} ${SAMPLE_HEAD_FILE})
endforeach()

# 矩透明帧示例：examples/moment_oit.cpp + moment_oit.cpp
add_executable(moment_oit_example examples/moment_oit.cpp moment_oit.cpp ${SAMPLE_HEAD_FILE})
target_include_directories(moment_oit_example PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
