## Copyright 2009-2021 Intel Corporation
## SPDX-License-Identifier: Apache-2.0

# use default install config
INCLUDE("${CMAKE_CURRENT_LIST_DIR}/embree-config-install.cmake")

# and override path variables to match for build directory
SET(EMBREE_INCLUDE_DIRS /Volumes/UserFBWorkspace/questions/FSC0050C137AFFU11839/attempts/20261116T161123843784Z/repo/include)
SET(EMBREE_LIBRARY /Volumes/UserFBWorkspace/questions/FSC0050C137AFFU11839/attempts/20261116T161123843784Z/repo/.task-build/libembree4.4.dylib)
SET(EMBREE_LIBRARIES ${EMBREE_LIBRARY})
