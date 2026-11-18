#----------------------------------------------------------------
# Generated CMake target import file.
#----------------------------------------------------------------

# Commands may need to know the format version.
set(CMAKE_IMPORT_FILE_VERSION 1)

# Import target "libdwarf::dwarf" for configuration ""
set_property(TARGET libdwarf::dwarf APPEND PROPERTY IMPORTED_CONFIGURATIONS NOCONFIG)
set_target_properties(libdwarf::dwarf PROPERTIES
  IMPORTED_LOCATION_NOCONFIG "${_IMPORT_PREFIX}/lib/libdwarf.2.3.4.dylib"
  IMPORTED_SONAME_NOCONFIG "@rpath/libdwarf.2.dylib"
  )

list(APPEND _cmake_import_check_targets libdwarf::dwarf )
list(APPEND _cmake_import_check_files_for_libdwarf::dwarf "${_IMPORT_PREFIX}/lib/libdwarf.2.3.4.dylib" )

# Commands beyond this point should not need to know the version.
set(CMAKE_IMPORT_FILE_VERSION)
