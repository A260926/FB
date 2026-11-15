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
set(CPACK_BINARY_BUNDLE "OFF")
set(CPACK_BINARY_DEB "OFF")
set(CPACK_BINARY_DRAGNDROP "OFF")
set(CPACK_BINARY_FREEBSD "OFF")
set(CPACK_BINARY_IFW "OFF")
set(CPACK_BINARY_NSIS "OFF")
set(CPACK_BINARY_PRODUCTBUILD "OFF")
set(CPACK_BINARY_RPM "OFF")
set(CPACK_BINARY_STGZ "ON")
set(CPACK_BINARY_TBZ2 "OFF")
set(CPACK_BINARY_TGZ "ON")
set(CPACK_BINARY_TXZ "OFF")
set(CPACK_BUILD_SOURCE_DIRS "/Users/solo/pro/UserFB/workspace/FS599B0B2E5D2EU9411/attempts/20261114T191253494957Z/repo;/Users/solo/pro/UserFB/workspace/FS599B0B2E5D2EU9411/attempts/20261114T191253494957Z/repo/.task-build/cmake")
set(CPACK_CMAKE_GENERATOR "Unix Makefiles")
set(CPACK_COMPONENT_UNSPECIFIED_HIDDEN "TRUE")
set(CPACK_COMPONENT_UNSPECIFIED_REQUIRED "TRUE")
set(CPACK_DEBIAN_PACKAGE_ARCHITECTURE "arm64")
set(CPACK_DEBIAN_PACKAGE_CONTROL_EXTRA "/Users/solo/pro/UserFB/workspace/FS599B0B2E5D2EU9411/attempts/20261114T191253494957Z/repo/packages/deb/triggers")
set(CPACK_DEBIAN_PACKAGE_DEPENDS "libc6 (>= 2.2.5)")
set(CPACK_DEBIAN_PACKAGE_FILE_NAME "libcapstone-dev_5.0.6_arm64")
set(CPACK_DEBIAN_PACKAGE_MULTIARCH "same")
set(CPACK_DEBIAN_PACKAGE_NAME "libcapstone-dev")
set(CPACK_DEBIAN_PACKAGE_ORIGINAL_MAINTAINER "Debian Security Tools <team+pkg-security@tracker.debian.org>")
set(CPACK_DEBIAN_PACKAGE_PRIORITY "optional")
set(CPACK_DEBIAN_PACKAGE_SECTION "libdevel")
set(CPACK_DEBIAN_PACKAGE_SOURCE "capstone")
set(CPACK_DEBIAN_PACKAGE_VERSION "5.0.6")
set(CPACK_DEFAULT_PACKAGE_DESCRIPTION_FILE "/opt/homebrew/share/cmake/Templates/CPack.GenericDescription.txt")
set(CPACK_DEFAULT_PACKAGE_DESCRIPTION_SUMMARY "capstone built using CMake")
set(CPACK_DMG_PACKAGE_FILE_NAME "capstone-5.0.6")
set(CPACK_DMG_SLA_USE_RESOURCE_FILE_LICENSE "ON")
set(CPACK_GENERATOR "STGZ;TGZ")
set(CPACK_INNOSETUP_ARCHITECTURE "x64")
set(CPACK_INSTALL_CMAKE_PROJECTS "/Users/solo/pro/UserFB/workspace/FS599B0B2E5D2EU9411/attempts/20261114T191253494957Z/repo/.task-build/cmake;capstone;ALL;/")
set(CPACK_INSTALL_PREFIX "/usr/local")
set(CPACK_MODULE_PATH "")
set(CPACK_NSIS_DISPLAY_NAME "capstone 5.0.6")
set(CPACK_NSIS_INSTALLER_ICON_CODE "")
set(CPACK_NSIS_INSTALLER_MUI_ICON_CODE "")
set(CPACK_NSIS_INSTALL_ROOT "$PROGRAMFILES")
set(CPACK_NSIS_PACKAGE_NAME "capstone 5.0.6")
set(CPACK_NSIS_UNINSTALL_NAME "Uninstall")
set(CPACK_OBJDUMP_EXECUTABLE "/usr/bin/objdump")
set(CPACK_OUTPUT_CONFIG_FILE "/Users/solo/pro/UserFB/workspace/FS599B0B2E5D2EU9411/attempts/20261114T191253494957Z/repo/.task-build/cmake/CPackConfig.cmake")
set(CPACK_PACKAGE_CONTACT "Rot127 <unisono@quyllur.org>")
set(CPACK_PACKAGE_DEFAULT_LOCATION "/")
set(CPACK_PACKAGE_DESCRIPTION "Capstone is a lightweight multi-platform, multi-architecture disassembly framework. These are the development headers and libraries.
 Features:
 - Support hardware architectures: AArch64, ARM, Alpha, BPF, EVM, HPPA, LongArch, M680X, M68K, MOS65XX, Mips, PowerPC, RISCV, SH, Sparc, SystemZ, TMS320C64x, TriCore, WASM, x86, XCore, Xtensa.
 - Clean/simple/lightweight/intuitive architecture-neutral API.
 - Provide details on disassembled instructions (called \"decomposer\" by some others).
 - Provide some semantics of the disassembled instruction, such as list of implicit registers read & written.
 - Thread-safe by design.
 - Special support for embedding into firmware or OS kernel.
 - Distributed under the open source BSD license.")
set(CPACK_PACKAGE_DESCRIPTION_FILE "/opt/homebrew/share/cmake/Templates/CPack.GenericDescription.txt")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Lightweight multi-architecture disassembly framework - devel files")
set(CPACK_PACKAGE_FILE_NAME "capstone-5.0.6-Darwin")
set(CPACK_PACKAGE_HOMEPAGE_URL "https://www.capstone-engine.org/")
set(CPACK_PACKAGE_INSTALL_DIRECTORY "capstone 5.0.6")
set(CPACK_PACKAGE_INSTALL_REGISTRY_KEY "capstone 5.0.6")
set(CPACK_PACKAGE_NAME "capstone")
set(CPACK_PACKAGE_RELOCATABLE "true")
set(CPACK_PACKAGE_VENDOR "Rot127")
set(CPACK_PACKAGE_VERSION "5.0.6")
set(CPACK_PACKAGE_VERSION_MAJOR "5")
set(CPACK_PACKAGE_VERSION_MINOR "0")
set(CPACK_PACKAGE_VERSION_PATCH "6")
set(CPACK_PROJECT_CONFIG_FILE "/Users/solo/pro/UserFB/workspace/FS599B0B2E5D2EU9411/attempts/20261114T191253494957Z/repo/CPackConfig.cmake")
set(CPACK_RESOURCE_FILE_LICENSE "/Users/solo/pro/UserFB/workspace/FS599B0B2E5D2EU9411/attempts/20261114T191253494957Z/repo/LICENSE.TXT")
set(CPACK_RESOURCE_FILE_README "/opt/homebrew/share/cmake/Templates/CPack.GenericDescription.txt")
set(CPACK_RESOURCE_FILE_WELCOME "/opt/homebrew/share/cmake/Templates/CPack.GenericWelcome.txt")
set(CPACK_RPM_CHANGELOG_FILE "/Users/solo/pro/UserFB/workspace/FS599B0B2E5D2EU9411/attempts/20261114T191253494957Z/repo/ChangeLog")
set(CPACK_RPM_PACKAGE_ARCHITECTURE "arm64")
set(CPACK_RPM_PACKAGE_DESCRIPTION "Capstone is a lightweight multi-platform, multi-architecture disassembly framework. These are the development headers and libraries.
 Features:
 - Support hardware architectures: AArch64, ARM, Alpha, BPF, EVM, HPPA, LongArch, M680X, M68K, MOS65XX, Mips, PowerPC, RISCV, SH, Sparc, SystemZ, TMS320C64x, TriCore, WASM, x86, XCore, Xtensa.
 - Clean/simple/lightweight/intuitive architecture-neutral API.
 - Provide details on disassembled instructions (called \"decomposer\" by some others).
 - Provide some semantics of the disassembled instruction, such as list of implicit registers read & written.
 - Thread-safe by design.
 - Special support for embedding into firmware or OS kernel.
 - Distributed under the open source BSD license.")
set(CPACK_RPM_PACKAGE_FILE_NAME "capstone-devel-5.0.6.arm64")
set(CPACK_RPM_PACKAGE_GROUP "Development/Libraries")
set(CPACK_RPM_PACKAGE_LICENSE "BSD3, LLVM")
set(CPACK_RPM_PACKAGE_NAME "capstone-devel")
set(CPACK_RPM_PACKAGE_REQUIRES "glibc >= 2.2.5")
set(CPACK_RPM_PACKAGE_VERSION "5.0.6")
set(CPACK_RPM_POST_INSTALL_SCRIPT_FILE "/Users/solo/pro/UserFB/workspace/FS599B0B2E5D2EU9411/attempts/20261114T191253494957Z/repo/packages/rpm/postinstall.sh")
set(CPACK_RPM_POST_UNINSTALL_SCRIPT_FILE "/Users/solo/pro/UserFB/workspace/FS599B0B2E5D2EU9411/attempts/20261114T191253494957Z/repo/packages/rpm/postinstall.sh")
set(CPACK_SET_DESTDIR "OFF")
set(CPACK_SOURCE_GENERATOR "TBZ2;TGZ;TXZ;TZ")
set(CPACK_SOURCE_OUTPUT_CONFIG_FILE "/Users/solo/pro/UserFB/workspace/FS599B0B2E5D2EU9411/attempts/20261114T191253494957Z/repo/.task-build/cmake/CPackSourceConfig.cmake")
set(CPACK_SOURCE_RPM "OFF")
set(CPACK_SOURCE_TBZ2 "ON")
set(CPACK_SOURCE_TGZ "ON")
set(CPACK_SOURCE_TXZ "ON")
set(CPACK_SOURCE_TZ "ON")
set(CPACK_SOURCE_ZIP "OFF")
set(CPACK_STRIP_FILES "false")
set(CPACK_SYSTEM_NAME "Darwin")
set(CPACK_THREADS "1")
set(CPACK_TOPLEVEL_TAG "Darwin")
set(CPACK_WIX_SIZEOF_VOID_P "8")

if(NOT CPACK_PROPERTIES_FILE)
  set(CPACK_PROPERTIES_FILE "/Users/solo/pro/UserFB/workspace/FS599B0B2E5D2EU9411/attempts/20261114T191253494957Z/repo/.task-build/cmake/CPackProperties.cmake")
endif()

if(EXISTS ${CPACK_PROPERTIES_FILE})
  include(${CPACK_PROPERTIES_FILE})
endif()
