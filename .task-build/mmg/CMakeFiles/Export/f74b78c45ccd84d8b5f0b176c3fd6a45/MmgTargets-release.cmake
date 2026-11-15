#----------------------------------------------------------------
# Generated CMake target import file for configuration "Release".
#----------------------------------------------------------------

# Commands may need to know the format version.
set(CMAKE_IMPORT_FILE_VERSION 1)

# Import target "Mmg::libmmgs_a" for configuration "Release"
set_property(TARGET Mmg::libmmgs_a APPEND PROPERTY IMPORTED_CONFIGURATIONS RELEASE)
set_target_properties(Mmg::libmmgs_a PROPERTIES
  IMPORTED_LINK_INTERFACE_LANGUAGES_RELEASE "C;CXX"
  IMPORTED_LOCATION_RELEASE "${_IMPORT_PREFIX}/lib/libmmgs.a"
  )

list(APPEND _cmake_import_check_targets Mmg::libmmgs_a )
list(APPEND _cmake_import_check_files_for_Mmg::libmmgs_a "${_IMPORT_PREFIX}/lib/libmmgs.a" )

# Commands beyond this point should not need to know the version.
set(CMAKE_IMPORT_FILE_VERSION)
