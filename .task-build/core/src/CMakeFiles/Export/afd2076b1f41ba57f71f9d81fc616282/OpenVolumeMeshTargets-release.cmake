#----------------------------------------------------------------
# Generated CMake target import file for configuration "Release".
#----------------------------------------------------------------

# Commands may need to know the format version.
set(CMAKE_IMPORT_FILE_VERSION 1)

# Import target "OpenVolumeMesh::OpenVolumeMesh" for configuration "Release"
set_property(TARGET OpenVolumeMesh::OpenVolumeMesh APPEND PROPERTY IMPORTED_CONFIGURATIONS RELEASE)
set_target_properties(OpenVolumeMesh::OpenVolumeMesh PROPERTIES
  IMPORTED_LINK_INTERFACE_LANGUAGES_RELEASE "CXX"
  IMPORTED_LOCATION_RELEASE "${_IMPORT_PREFIX}/lib/libOpenVolumeMesh.a"
  )

list(APPEND _cmake_import_check_targets OpenVolumeMesh::OpenVolumeMesh )
list(APPEND _cmake_import_check_files_for_OpenVolumeMesh::OpenVolumeMesh "${_IMPORT_PREFIX}/lib/libOpenVolumeMesh.a" )

# Commands beyond this point should not need to know the version.
set(CMAKE_IMPORT_FILE_VERSION)
