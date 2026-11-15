#pragma once

#include <array>
#include <vector>

#include <glm/glm.hpp>

#include "ModelTriangle.h"

// 2D / 2.5D meshing toolkit (Lecture 17 techniques).
//
// Provides Delaunay triangulation of planar point sets, reconstruction of a
// surface mesh from a height-field point cloud, and mesh simplification via
// gravitational nearest-vertex merging (edge collapse) with anchor support.
namespace meshing {

// Returns true iff point p lies strictly inside the circumcircle of triangle
// (a, b, c). Orientation independent: works for both clockwise and
// counter-clockwise winding of a, b, c.
bool inCircumcircle(glm::vec2 a, glm::vec2 b, glm::vec2 c, glm::vec2 p);

// Bowyer-Watson incremental Delaunay triangulation.
//
// Returns a list of triangles, each expressed as a triple of indices into the
// supplied `points` array. Duplicate / collinear inputs are tolerated (they may
// simply not appear in any triangle). Fewer than three points yields no
// triangles.
std::vector<std::array<int, 3>> delaunayTriangulate(const std::vector<glm::vec2> &points);

// Reconstructs a surface mesh from a height-field point cloud.
//
// Each point is projected onto the x/z plane, Delaunay-triangulated there, then
// lifted back to 3D using its original y value. Emits grey ModelTriangles with
// per-face normals (also copied into the three vertex normals).
std::vector<ModelTriangle> pointCloudToMesh(const std::vector<glm::vec3> &points);

// Simplifies a triangle mesh toward `targetRatio` (0..1) of its original
// triangle count by iterative edge collapse: shortest edges are collapsed
// first, merging their endpoints (gravitational nearest-vertex merging).
//
// "Anchor" vertices - those on a boundary edge or at a sharp crease - resist
// merging: an anchor never moves, and an edge joining two anchors is never
// collapsed. Face colours and materials are preserved from the input.
std::vector<ModelTriangle> decimate(const std::vector<ModelTriangle> &mesh, float targetRatio);

} // namespace meshing
