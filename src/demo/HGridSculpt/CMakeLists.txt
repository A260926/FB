cmake_minimum_required(VERSION 3.25)
project(HGridSculpt LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(GEL REQUIRED HINTS "$ENV{HOME}/.local")

add_executable(hgrid_sculpt hgrid_sculpt.cpp)
target_link_libraries(hgrid_sculpt PRIVATE GEL::GEL)
