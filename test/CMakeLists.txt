# Adds GTest library
add_subdirectory(../extern/gtest gtest)
target_link_libraries(gtest PRIVATE ozz_suppress_warnings)

# Propagate to all targets that include gtest headers
target_compile_definitions(gtest PUBLIC GTEST_HAS_EXCEPTIONS=0)

# Adds test file directories
add_subdirectory(base)
add_subdirectory(animation)
add_subdirectory(geometry)
add_subdirectory(options)
