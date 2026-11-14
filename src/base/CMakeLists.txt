
add_library(ozz_base
  memory/allocator.cc
  platform.cc
  log.cc
  containers/string_archive.cc
  encode/group_varint.cc
  io/archive.cc
  io/stream.cc
  maths/box.cc
  maths/simd_math.cc
  maths/math_archive.cc
  maths/soa_math_archive.cc
  maths/simd_math_archive.cc)

target_sources(ozz_base PUBLIC
  FILE_SET HEADERS
  BASE_DIRS ${PROJECT_SOURCE_DIR}/include
  FILES
    ${PROJECT_SOURCE_DIR}/include/ozz/base/export.h
    ${PROJECT_SOURCE_DIR}/include/ozz/base/endianness.h
    ${PROJECT_SOURCE_DIR}/include/ozz/base/gtest_helper.h
    ${PROJECT_SOURCE_DIR}/include/ozz/base/memory/allocator.h
    ${PROJECT_SOURCE_DIR}/include/ozz/base/memory/unique_ptr.h
    ${PROJECT_SOURCE_DIR}/include/ozz/base/platform.h
    ${PROJECT_SOURCE_DIR}/include/ozz/base/span.h
    ${PROJECT_SOURCE_DIR}/include/ozz/base/log.h
    ${PROJECT_SOURCE_DIR}/include/ozz/base/containers/intrusive_list.h
    ${PROJECT_SOURCE_DIR}/include/ozz/base/containers/deque.h
    ${PROJECT_SOURCE_DIR}/include/ozz/base/containers/list.h
    ${PROJECT_SOURCE_DIR}/include/ozz/base/containers/map.h
    ${PROJECT_SOURCE_DIR}/include/ozz/base/containers/queue.h
    ${PROJECT_SOURCE_DIR}/include/ozz/base/containers/set.h
    ${PROJECT_SOURCE_DIR}/include/ozz/base/containers/stack.h
    ${PROJECT_SOURCE_DIR}/include/ozz/base/containers/string.h
    ${PROJECT_SOURCE_DIR}/include/ozz/base/containers/string_archive.h
    ${PROJECT_SOURCE_DIR}/include/ozz/base/containers/unordered_map.h
    ${PROJECT_SOURCE_DIR}/include/ozz/base/containers/unordered_set.h
    ${PROJECT_SOURCE_DIR}/include/ozz/base/containers/vector.h
    ${PROJECT_SOURCE_DIR}/include/ozz/base/containers/vector_archive.h
    ${PROJECT_SOURCE_DIR}/include/ozz/base/containers/std_allocator.h
    ${PROJECT_SOURCE_DIR}/include/ozz/base/encode/group_varint.h
    ${PROJECT_SOURCE_DIR}/include/ozz/base/io/archive.h
    ${PROJECT_SOURCE_DIR}/include/ozz/base/io/archive_traits.h
    ${PROJECT_SOURCE_DIR}/include/ozz/base/io/stream.h
    ${PROJECT_SOURCE_DIR}/include/ozz/base/maths/box.h
    ${PROJECT_SOURCE_DIR}/include/ozz/base/maths/gtest_math_helper.h
    ${PROJECT_SOURCE_DIR}/include/ozz/base/maths/internal/simd_math_config.h
    ${PROJECT_SOURCE_DIR}/include/ozz/base/maths/internal/simd_math_ref-inl.h
    ${PROJECT_SOURCE_DIR}/include/ozz/base/maths/internal/simd_math_sse-inl.h
    ${PROJECT_SOURCE_DIR}/include/ozz/base/maths/math_ex.h
    ${PROJECT_SOURCE_DIR}/include/ozz/base/maths/math_constant.h
    ${PROJECT_SOURCE_DIR}/include/ozz/base/maths/quaternion.h
    ${PROJECT_SOURCE_DIR}/include/ozz/base/maths/rect.h
    ${PROJECT_SOURCE_DIR}/include/ozz/base/maths/simd_math.h
    ${PROJECT_SOURCE_DIR}/include/ozz/base/maths/simd_quaternion.h
    ${PROJECT_SOURCE_DIR}/include/ozz/base/maths/soa_float.h
    ${PROJECT_SOURCE_DIR}/include/ozz/base/maths/soa_quaternion.h
    ${PROJECT_SOURCE_DIR}/include/ozz/base/maths/soa_transform.h
    ${PROJECT_SOURCE_DIR}/include/ozz/base/maths/soa_float4x4.h
    ${PROJECT_SOURCE_DIR}/include/ozz/base/maths/transform.h
    ${PROJECT_SOURCE_DIR}/include/ozz/base/maths/vec_float.h
    ${PROJECT_SOURCE_DIR}/include/ozz/base/maths/math_archive.h
    ${PROJECT_SOURCE_DIR}/include/ozz/base/maths/soa_math_archive.h
    ${PROJECT_SOURCE_DIR}/include/ozz/base/maths/simd_math_archive.h)

target_compile_definitions(ozz_base
  PUBLIC $<$<BOOL:${BUILD_SHARED_LIBS}>:OZZ_USE_DYNAMIC_LINKING>
  PRIVATE $<$<BOOL:${BUILD_SHARED_LIBS}>:OZZ_BUILD_BASE_LIB>)

target_compile_options(ozz_base PUBLIC $<$<CXX_COMPILER_ID:MSVC>:/wd4251>)

target_include_directories(ozz_base
  PRIVATE
    ${PROJECT_SOURCE_DIR}/src)

set_target_properties(ozz_base PROPERTIES FOLDER "ozz")

install(TARGETS ozz_base
  DESTINATION lib
  FILE_SET HEADERS DESTINATION include)

fuse_target("ozz_base")

