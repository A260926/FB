add_library (progutils OBJECT "utils.c")
target_include_directories (progutils PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
target_include_directories (progutils PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/../src)

function (add_aubio_executable name)

  add_executable (${name} "${name}.c")

  target_link_libraries (${name} PUBLIC aubio)
  target_link_libraries (${name} PRIVATE progutils)
  target_include_directories (${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
  target_include_directories (${name} PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/../src)

endfunction ()

add_aubio_executable (aubioonset)
add_aubio_executable (aubiomfcc)
add_aubio_executable (aubionotes)
add_aubio_executable (aubiopitch)
add_aubio_executable (aubioquiet)
add_aubio_executable (aubiotrack)

# the hpss example is fully self-contained (C-generated material, no files
# or external audio library), so it links against aubio only, not progutils
add_executable (aubiohpss "aubiohpss.c")
target_link_libraries (aubiohpss PUBLIC aubio)
target_include_directories (aubiohpss PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
target_include_directories (aubiohpss PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/../src)
