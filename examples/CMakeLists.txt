if(PLUTOVG_ENABLE_IMAGE_WRITE)
   add_executable(smiley smiley.c)
   target_link_libraries(smiley plutovg)
endif()

add_executable(mesh_ppm mesh_ppm.c)
target_link_libraries(mesh_ppm plutovg)
