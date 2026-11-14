# This file will be configured to contain variables for CPack. These variables
# should be set in the CMake list file of the project before CPack module is
# included. The list of available CPACK_xxx variables and their associated
# documentation may be obtained using
#  cpack --help-variable-list
#
# Some variables are common to all generators (e.g. CPACK_PACKAGE_NAME)
# and some are specific to a generator
# (e.g. CPACK_NSIS_EXTRA_INSTALL_COMMANDS). The generator specific variables
# usually begin with CPACK_<GENNAME>_xxxx.


set(CPACK_ARCHIVE_GID "-1")
set(CPACK_ARCHIVE_UID "-1")
set(CPACK_BUILD_SOURCE_DIRS "/Users/solo/pro/UserFB/workspace/FSC78F0BE38193U9878/attempts/20261114T094010647291Z/repo/task_support;/Users/solo/pro/UserFB/workspace/FSC78F0BE38193U9878/attempts/20261114T094010647291Z/repo/.task-build")
set(CPACK_CMAKE_GENERATOR "Unix Makefiles")
set(CPACK_COMPONENT_UNSPECIFIED_HIDDEN "TRUE")
set(CPACK_COMPONENT_UNSPECIFIED_REQUIRED "TRUE")
set(CPACK_DEFAULT_PACKAGE_DESCRIPTION_FILE "/opt/homebrew/share/cmake/Templates/CPack.GenericDescription.txt")
set(CPACK_DEFAULT_PACKAGE_DESCRIPTION_SUMMARY "arib_macro_task built using CMake")
set(CPACK_GENERATOR "TGZ")
set(CPACK_INNOSETUP_ARCHITECTURE "x64")
set(CPACK_INSTALL_CMAKE_PROJECTS "/Users/solo/pro/UserFB/workspace/FSC78F0BE38193U9878/attempts/20261114T094010647291Z/repo/.task-build;arib_macro_task;ALL;/")
set(CPACK_INSTALL_PREFIX "/usr/local")
set(CPACK_MODULE_PATH "")
set(CPACK_NSIS_DISPLAY_NAME "arib_macro_task 1.1.2")
set(CPACK_NSIS_INSTALLER_ICON_CODE "")
set(CPACK_NSIS_INSTALLER_MUI_ICON_CODE "")
set(CPACK_NSIS_INSTALL_ROOT "$PROGRAMFILES")
set(CPACK_NSIS_PACKAGE_NAME "arib_macro_task 1.1.2")
set(CPACK_NSIS_UNINSTALL_NAME "Uninstall")
set(CPACK_OBJDUMP_EXECUTABLE "/usr/bin/objdump")
set(CPACK_OUTPUT_CONFIG_FILE "/Users/solo/pro/UserFB/workspace/FSC78F0BE38193U9878/attempts/20261114T094010647291Z/repo/.task-build/CPackConfig.cmake")
set(CPACK_PACKAGE_DEFAULT_LOCATION "/")
set(CPACK_PACKAGE_DESCRIPTION_FILE "/opt/homebrew/share/cmake/Templates/CPack.GenericDescription.txt")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "arib_macro_task built using CMake")
set(CPACK_PACKAGE_FILE_NAME "arib_macro_task-1.1.2-Darwin")
set(CPACK_PACKAGE_INSTALL_DIRECTORY "arib_macro_task 1.1.2")
set(CPACK_PACKAGE_INSTALL_REGISTRY_KEY "arib_macro_task 1.1.2")
set(CPACK_PACKAGE_NAME "arib_macro_task")
set(CPACK_PACKAGE_RELOCATABLE "true")
set(CPACK_PACKAGE_VENDOR "magicxqq")
set(CPACK_PACKAGE_VERSION "1.1.2")
set(CPACK_PACKAGE_VERSION_MAJOR "1")
set(CPACK_PACKAGE_VERSION_MINOR "1")
set(CPACK_PACKAGE_VERSION_PATCH "2")
set(CPACK_RESOURCE_FILE_LICENSE "/Users/solo/pro/UserFB/workspace/FSC78F0BE38193U9878/attempts/20261114T094010647291Z/repo/LICENSE")
set(CPACK_RESOURCE_FILE_README "/Users/solo/pro/UserFB/workspace/FSC78F0BE38193U9878/attempts/20261114T094010647291Z/repo/README.md")
set(CPACK_RESOURCE_FILE_WELCOME "/opt/homebrew/share/cmake/Templates/CPack.GenericWelcome.txt")
set(CPACK_SET_DESTDIR "OFF")
set(CPACK_SOURCE_GENERATOR "TGZ")
set(CPACK_SOURCE_IGNORE_FILES "/.idea;/.vs;/.vscode;/.git;/.*build.*;/out;/screenshots;/\\.DS_Store;CMakeSettings.json;compile_commands.json")
set(CPACK_SOURCE_OUTPUT_CONFIG_FILE "/Users/solo/pro/UserFB/workspace/FSC78F0BE38193U9878/attempts/20261114T094010647291Z/repo/.task-build/CPackSourceConfig.cmake")
set(CPACK_SYSTEM_NAME "Darwin")
set(CPACK_THREADS "1")
set(CPACK_TOPLEVEL_TAG "Darwin")
set(CPACK_WIX_SIZEOF_VOID_P "8")

if(NOT CPACK_PROPERTIES_FILE)
  set(CPACK_PROPERTIES_FILE "/Users/solo/pro/UserFB/workspace/FSC78F0BE38193U9878/attempts/20261114T094010647291Z/repo/.task-build/CPackProperties.cmake")
endif()

if(EXISTS ${CPACK_PROPERTIES_FILE})
  include(${CPACK_PROPERTIES_FILE})
endif()
