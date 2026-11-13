add_library(ozz_geometry
  skinning_job.cc)

target_sources(ozz_geometry PUBLIC
  FILE_SET HEADERS
  BASE_DIRS ${PROJECT_SOURCE_DIR}/include
  FILES
    ${PROJECT_SOURCE_DIR}/include/ozz/geometry/runtime/export.h
    ${PROJECT_SOURCE_DIR}/include/ozz/geometry/runtime/skinning_job.h)

target_compile_definitions(ozz_geometry PRIVATE $<$<BOOL:${BUILD_SHARED_LIBS}>:OZZ_BUILD_GEOMETRY_LIB>)

target_include_directories(ozz_geometry PRIVATE ${PROJECT_SOURCE_DIR}/src)

target_link_libraries(ozz_geometry PUBLIC ozz_base)
set_target_properties(ozz_geometry PROPERTIES FOLDER "ozz")

install(TARGETS ozz_geometry
  DESTINATION lib
  FILE_SET HEADERS DESTINATION include)

fuse_target("ozz_geometry")
