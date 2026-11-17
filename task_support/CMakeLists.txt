cmake_minimum_required(VERSION 3.15)
project(task_fdk LANGUAGES C CXX)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
add_subdirectory("${CMAKE_CURRENT_LIST_DIR}/.." upstream)
get_target_property(existing fdk-aac SOURCES)
file(GLOB task_production CONFIGURE_DEPENDS
 "${CMAKE_CURRENT_LIST_DIR}/../cpp/libAACenc/src/*.cpp"
 "${CMAKE_CURRENT_LIST_DIR}/../cpp/libAACenc/src/*.c"
 "${CMAKE_CURRENT_LIST_DIR}/../cpp/libMpegTPEnc/src/*.cpp"
 "${CMAKE_CURRENT_LIST_DIR}/../cpp/libMpegTPEnc/src/*.c")
foreach(src IN LISTS task_production)
 file(RELATIVE_PATH rel "${CMAKE_CURRENT_LIST_DIR}/.." "${src}")
 list(FIND existing "${rel}" relative_index)
 list(FIND existing "${src}" absolute_index)
 if(relative_index EQUAL -1 AND absolute_index EQUAL -1)
  target_sources(fdk-aac PRIVATE "${src}")
 endif()
endforeach()
