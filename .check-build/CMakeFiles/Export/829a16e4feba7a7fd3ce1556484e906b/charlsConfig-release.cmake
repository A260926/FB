#----------------------------------------------------------------
# Generated CMake target import file for configuration "Release".
#----------------------------------------------------------------

# Commands may need to know the format version.
set(CMAKE_IMPORT_FILE_VERSION 1)

# Import target "charls" for configuration "Release"
set_property(TARGET charls APPEND PROPERTY IMPORTED_CONFIGURATIONS RELEASE)
set_target_properties(charls PROPERTIES
  IMPORTED_LINK_INTERFACE_LANGUAGES_RELEASE "CXX"
  IMPORTED_LOCATION_RELEASE "${_IMPORT_PREFIX}/lib/libcharls.a"
  )

list(APPEND _cmake_import_check_targets charls )
list(APPEND _cmake_import_check_files_for_charls "${_IMPORT_PREFIX}/lib/libcharls.a" )

# Commands beyond this point should not need to know the version.
set(CMAKE_IMPORT_FILE_VERSION)
