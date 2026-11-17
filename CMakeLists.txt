cmake_minimum_required(VERSION 3.10)

project(discrete_mirage VERSION 1.2.0 LANGUAGES C CXX)

add_subdirectory(discrete_mirage)
add_subdirectory(demo)

# Headless motion-vector example (no window / graphics API required).
add_executable(two_frame_motion examples/two_frame_motion.cpp)
target_link_libraries(two_frame_motion PRIVATE discrete_mirage_lib)
