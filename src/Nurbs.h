#pragma once

#include <glm/glm.hpp>
#include <vector>
#include "Colour.h"
#include "ModelTriangle.h"

// Uniform bicubic B-spline / NURBS surface utilities.
//
// A control grid is a row-major 2D array of control points. It must be at
// least 4x4 (the minimum required for a bicubic basis). Larger grids are
// supported: the surface is evaluated across the full grid by mapping the
// (u, v) parameter domain 0..1 onto the interior span range of the grid.
//
// The parametric convention used here treats the outer std::vector index as
// the u direction and the inner std::vector index as the v direction:
//   controlGrid[i][j]  ->  point at u-row i, v-column j.

// Evaluate a uniform bicubic B-spline surface point for (u, v) in [0, 1].
// Uses the uniform cubic B-spline basis over the control grid. Optionally a
// per-control-point weight grid (same dimensions as controlGrid) turns the
// evaluation into a rational (NURBS) surface. Pass an empty weights vector
// for the non-rational B-spline case (all weights == 1).
glm::vec3 bsplineSurfacePoint(const std::vector<std::vector<glm::vec3>> &controlGrid, float u, float v,
                              const std::vector<std::vector<float>> &weights = {});

// Tessellate a spline surface into a triangle mesh. The surface is sampled on
// a (uSteps+1) x (vSteps+1) grid of points and split into 2*uSteps*vSteps
// triangles. Per-vertex normals are computed from the surface partial
// derivatives estimated by central finite differences in u and v.
std::vector<ModelTriangle> tessellateSplineSurface(const std::vector<std::vector<glm::vec3>> &controlGrid, int uSteps,
                                                   int vSteps, Colour colour,
                                                   const std::vector<std::vector<float>> &weights = {});
