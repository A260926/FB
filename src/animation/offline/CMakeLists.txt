
add_library(ozz_animation_offline
  decimate.h
  raw_animation.cc
  raw_animation_archive.cc
  raw_animation_utils.cc
  animation_builder.cc
  animation_optimizer.cc
  additive_animation_builder.cc
  motion_extractor.cc
  raw_skeleton.cc
  raw_skeleton_archive.cc
  skeleton_builder.cc
  raw_track.cc
  raw_track_utils.cc
  track_builder.cc
  track_optimizer.cc)

target_sources(ozz_animation_offline PUBLIC
  FILE_SET HEADERS
  BASE_DIRS ${PROJECT_SOURCE_DIR}/include
  FILES
    ${PROJECT_SOURCE_DIR}/include/ozz/animation/offline/export.h
    ${PROJECT_SOURCE_DIR}/include/ozz/animation/offline/raw_animation.h
    ${PROJECT_SOURCE_DIR}/include/ozz/animation/offline/raw_animation_utils.h
    ${PROJECT_SOURCE_DIR}/include/ozz/animation/offline/animation_builder.h
    ${PROJECT_SOURCE_DIR}/include/ozz/animation/offline/animation_optimizer.h
    ${PROJECT_SOURCE_DIR}/include/ozz/animation/offline/additive_animation_builder.h
    ${PROJECT_SOURCE_DIR}/include/ozz/animation/offline/motion_extractor.h
    ${PROJECT_SOURCE_DIR}/include/ozz/animation/offline/raw_skeleton.h
    ${PROJECT_SOURCE_DIR}/include/ozz/animation/offline/skeleton_builder.h
    ${PROJECT_SOURCE_DIR}/include/ozz/animation/offline/raw_track.h
    ${PROJECT_SOURCE_DIR}/include/ozz/animation/offline/raw_track_utils.h
    ${PROJECT_SOURCE_DIR}/include/ozz/animation/offline/track_builder.h
    ${PROJECT_SOURCE_DIR}/include/ozz/animation/offline/track_optimizer.h)

target_compile_definitions(ozz_animation_offline PRIVATE $<$<BOOL:${BUILD_SHARED_LIBS}>:OZZ_BUILD_ANIMOFFLINE_LIB>)

target_include_directories(ozz_animation_offline PRIVATE ${PROJECT_SOURCE_DIR}/src)

target_link_libraries(ozz_animation_offline PUBLIC ozz_animation)

set_target_properties(ozz_animation_offline PROPERTIES FOLDER "ozz")

install(TARGETS ozz_animation_offline
  DESTINATION lib
  FILE_SET HEADERS DESTINATION include)

fuse_target("ozz_animation_offline")

add_subdirectory(tools) # tools target need to be defined before other targets that uses them
add_subdirectory(fbx)
add_subdirectory(gltf)

