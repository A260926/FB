cmake_minimum_required(VERSION 3.21)
project("SurfaceEditExample")
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
add_compile_options("$<$<C_COMPILER_ID:MSVC>:/utf-8>")

add_executable(${PROJECT_NAME} main.cpp)
target_link_libraries(${PROJECT_NAME} PRIVATE libnurbs)

# The example only needs Eigen (bundled with this repository); nothing is
# downloaded at configure or build time.
target_link_libraries(${PROJECT_NAME} PRIVATE Eigen3::Eigen)
