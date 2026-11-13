cmake_minimum_required(VERSION 3.12 FATAL_ERROR)
project(import_binding_plan CXX)

# Two ways to build:
#  * in-tree, as part of the pe-parse build (pe-parse::pe-parse already exists)
#  * standalone, against an installed pe-parse via find_package.
if (NOT TARGET pe-parse)
  find_package(pe-parse REQUIRED)
endif ()

add_executable(${PROJECT_NAME} main.cpp)

if (TARGET pe-parse)
  target_link_libraries(${PROJECT_NAME} PRIVATE pe-parse)
else ()
  target_link_libraries(${PROJECT_NAME} PRIVATE pe-parse::pe-parse)
endif ()

set_target_properties(${PROJECT_NAME} PROPERTIES CXX_STANDARD 11
                                               CXX_STANDARD_REQUIRED ON
                                               CXX_EXTENSIONS OFF)

if (MSVC)
  target_compile_options(${PROJECT_NAME} PRIVATE /W4)
else ()
  target_compile_options(${PROJECT_NAME} PRIVATE -Wall -Wextra)
endif ()
