## Copyright 2009-2021 Intel Corporation
## SPDX-License-Identifier: Apache-2.0

# Tutorials present in this source tree are added individually so the
# build also works with partially trimmed checkouts. The photon beam
# tutorial is windowless and depends only on the native embree library,
# the C++17 standard library and the sources in this repository.

IF (EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/minimal/CMakeLists.txt")
  ADD_SUBDIRECTORY(minimal)
ENDIF()

IF (EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/photon_beams/CMakeLists.txt")
  ADD_SUBDIRECTORY(photon_beams)
ENDIF()
