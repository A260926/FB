cmake_minimum_required(VERSION 3.1.3)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

project (RenderHelp VERSION 1.0.0)

file(GLOB SAMPLE_HEAD_FILE ${CMAKE_CURRENT_SOURCE_DIR}/*.h)
file(GLOB SAMPLE_SOURCE_FILE ${CMAKE_CURRENT_SOURCE_DIR}/*.cpp)

# 软光栅生产模块：无 main，单独编译为静态库供测试与示例链接
add_library(soft_raster STATIC soft_raster.cpp soft_raster.h)
target_include_directories(soft_raster PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

foreach(SAMPLE_MAIN_FILE IN LISTS SAMPLE_SOURCE_FILE)
    get_filename_component(SAMPLE_NAME ${SAMPLE_MAIN_FILE} NAME_WE)
    # soft_raster.cpp 是库模块，不作为独立可执行
    if(SAMPLE_NAME STREQUAL "soft_raster")
        continue()
    endif()
    add_executable(${SAMPLE_NAME} ${SAMPLE_MAIN_FILE} ${SAMPLE_HEAD_FILE})
endforeach()

# 软光栅任务测试（main 由 task_support/driver.c 提供）
add_executable(soft_target
    tests/soft_target.cpp
    task_support/driver.c
)
target_link_libraries(soft_target PRIVATE soft_raster)

# 软光栅几何拟合示例
add_executable(soft_fit examples/soft_fit.cpp)
target_link_libraries(soft_fit PRIVATE soft_raster)
