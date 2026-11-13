# Dual quaternion skinning example. Requires only runtime and offline libraries,
# no window nor third party dependency.
add_executable(dq_skinning
  dq_skinning.cc)

target_link_libraries(dq_skinning
  ozz_geometry
  ozz_animation_offline)

set_target_properties(dq_skinning PROPERTIES FOLDER "examples")

install(TARGETS dq_skinning DESTINATION bin/examples)
