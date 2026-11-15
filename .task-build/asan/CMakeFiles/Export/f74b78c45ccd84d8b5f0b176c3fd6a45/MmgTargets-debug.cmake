#----------------------------------------------------------------
# Generated CMake target import file for configuration "Debug".
#----------------------------------------------------------------

# Commands may need to know the format version.
set(CMAKE_IMPORT_FILE_VERSION 1)

# Import target "Mmg::libmmgs_a" for configuration "Debug"
set_property(TARGET Mmg::libmmgs_a APPEND PROPERTY IMPORTED_CONFIGURATIONS DEBUG)
set_target_properties(Mmg::libmmgs_a PROPERTIES
  IMPORTED_LINK_INTERFACE_LANGUAGES_DEBUG "C;CXX"
  IMPORTED_LOCATION_DEBUG "${_IMPORT_PREFIX}/lib/libmmgs.a"
  )

list(APPEND _cmake_import_check_targets Mmg::libmmgs_a )
list(APPEND _cmake_import_check_files_for_Mmg::libmmgs_a "${_IMPORT_PREFIX}/lib/libmmgs.a" )

# Commands beyond this point should not need to know the version.
set(CMAKE_IMPORT_FILE_VERSION)
