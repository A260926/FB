cmake_minimum_required(VERSION 3.13)
project(trimesh_volume_preserve)

if (VCG_HEADER_ONLY)
	set(SOURCES
		trimesh_volume_preserve.cpp)
endif()

add_executable(trimesh_volume_preserve
	${SOURCES})

target_link_libraries(
	trimesh_volume_preserve
	PUBLIC
		vcglib
	)
