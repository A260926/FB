## Copyright 2009-2021 Intel Corporation
## SPDX-License-Identifier: Apache-2.0

PROJECT(photon_beams)

# Windowless progressive photon-beam renderer. No TBB, ISPC, networking,
# windowing system or GPU is required; the target only links embree.
ADD_EXECUTABLE(embree_photon_beams
  renderer.cpp
  "${CMAKE_SOURCE_DIR}/examples/photon_beams.cpp")

TARGET_INCLUDE_DIRECTORIES(embree_photon_beams PRIVATE "${CMAKE_SOURCE_DIR}")
TARGET_LINK_LIBRARIES(embree_photon_beams embree)
SET_TARGET_PROPERTIES(embree_photon_beams PROPERTIES FOLDER tutorials/single)

IF (COMMAND SIGN_TARGET)
  SIGN_TARGET(embree_photon_beams)
ENDIF()
