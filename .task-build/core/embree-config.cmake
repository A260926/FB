## Copyright 2009-2021 Intel Corporation
## SPDX-License-Identifier: Apache-2.0

# use default install config
INCLUDE("${CMAKE_CURRENT_LIST_DIR}/embree-config-install.cmake")

# and override path variables to match for build directory
SET(EMBREE_INCLUDE_DIRS /Volumes/UserFBWorkspace/questions/FS644D290A1983U14228/attempts/20261121T004633935188Z/repo/include)
SET(EMBREE_LIBRARY /Volumes/UserFBWorkspace/questions/FS644D290A1983U14228/attempts/20261121T004633935188Z/repo/.task-build/core/libembree4.a)
SET(EMBREE_LIBRARIES ${EMBREE_LIBRARY})
