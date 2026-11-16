#ifndef VOXELDIRECT_HPP_
#define VOXELDIRECT_HPP_

#include "VoxelSurfaceLights.hpp"
#include "math/Vec3.hpp"

// Windowless production entry point for one-bounce direct illumination from
// the emissive voxel faces managed by lights. All random inputs are expected
// in [0,1); P and albedo must be finite, N unit length and albedo
// non-negative, otherwise std::invalid_argument is thrown.
//
// The receiver P lies in empty space or on an outer (non-emissive) surface.
// Visibility is resolved against the same immutable voxel tree using an exact
// open-interval test: only occupied intersections with positive length
// strictly between P and the light sample cast shadows.
Vec3 estimateDirect(const VoxelSurfaceLights &lights,
                    const Vec3 &P, const Vec3 &N, const Vec3 &albedo,
                    double u0, double u1, double u2);

#endif /* VOXELDIRECT_HPP_ */
