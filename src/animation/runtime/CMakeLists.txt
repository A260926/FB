
add_library(ozz_animation
  animation_keyframe.h
  animation.cc
  animation_utils.cc
  blending_job.cc
  ik_aim_job.cc
  ik_two_bone_job.cc
  local_to_model_job.cc
  motion_blending_job.cc
  sampling_job.cc
  skeleton.cc
  skeleton_utils.cc
  track.cc
  track_sampling_job.cc
  track_triggering_job.cc)

target_sources(ozz_animation PUBLIC
  FILE_SET HEADERS
  BASE_DIRS ${PROJECT_SOURCE_DIR}/include
  FILES
    ${PROJECT_SOURCE_DIR}/include/ozz/animation/runtime/export.h
    ${PROJECT_SOURCE_DIR}/include/ozz/animation/runtime/animation.h
    ${PROJECT_SOURCE_DIR}/include/ozz/animation/runtime/animation_utils.h
    ${PROJECT_SOURCE_DIR}/include/ozz/animation/runtime/blending_job.h
    ${PROJECT_SOURCE_DIR}/include/ozz/animation/runtime/ik_aim_job.h
    ${PROJECT_SOURCE_DIR}/include/ozz/animation/runtime/ik_two_bone_job.h
    ${PROJECT_SOURCE_DIR}/include/ozz/animation/runtime/local_to_model_job.h
    ${PROJECT_SOURCE_DIR}/include/ozz/animation/runtime/motion_blending_job.h
    ${PROJECT_SOURCE_DIR}/include/ozz/animation/runtime/sampling_job.h
    ${PROJECT_SOURCE_DIR}/include/ozz/animation/runtime/skeleton.h
    ${PROJECT_SOURCE_DIR}/include/ozz/animation/runtime/skeleton_utils.h
    ${PROJECT_SOURCE_DIR}/include/ozz/animation/runtime/track.h
    ${PROJECT_SOURCE_DIR}/include/ozz/animation/runtime/track_sampling_job.h
    ${PROJECT_SOURCE_DIR}/include/ozz/animation/runtime/track_triggering_job.h
    ${PROJECT_SOURCE_DIR}/include/ozz/animation/runtime/track_triggering_job_trait.h)

target_compile_definitions(ozz_animation PRIVATE $<$<BOOL:${BUILD_SHARED_LIBS}>:OZZ_BUILD_ANIMATION_LIB>)

target_include_directories(ozz_animation PRIVATE ${PROJECT_SOURCE_DIR}/src)

target_link_libraries(ozz_animation
  PUBLIC
  ozz_base)

set_target_properties(ozz_animation
  PROPERTIES FOLDER "ozz")

install(TARGETS ozz_animation
  DESTINATION lib
  FILE_SET HEADERS DESTINATION include)

fuse_target("ozz_animation")
