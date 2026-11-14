add_library(ozz_options
  options.cc)

target_sources(ozz_options PUBLIC
  FILE_SET HEADERS
  BASE_DIRS ${PROJECT_SOURCE_DIR}/include
  FILES
    ${PROJECT_SOURCE_DIR}/include/ozz/options/export.h
    ${PROJECT_SOURCE_DIR}/include/ozz/options/options.h)

target_include_directories(ozz_options PRIVATE ${PROJECT_SOURCE_DIR}/src)

target_link_libraries(ozz_options PUBLIC ozz_base)

target_compile_definitions(ozz_options PRIVATE $<$<BOOL:${BUILD_SHARED_LIBS}>:OZZ_BUILD_OPTIONS_LIB>)

set_target_properties(ozz_options PROPERTIES FOLDER "ozz")

install(TARGETS ozz_options
  DESTINATION lib
  FILE_SET HEADERS DESTINATION include)

fuse_target("ozz_options")
