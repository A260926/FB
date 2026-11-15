#pragma once

#include <glm/glm.hpp>
#include <array>
#include <vector>
#include "ModelTriangle.h"

// Bicubic Bezier patch utilities.
//
// A bicubic Bezier patch is the classic curved-surface primitive used by, for
// example, the Utah teapot. It is defined by a 4x4 grid of control points and
// two cubic Bezier basis functions, one in each parametric direction.
//
// Control points for a patch are stored row-major in a flat std::array of 16
// entries. The parametric convention used here is:
//   index(row, col) = row * 4 + col
//   u varies along rows (the row index / first Bernstein factor)
//   v varies along columns (the column index / second Bernstein factor)
// Both parameters u and v are expected in the range [0, 1].

// Evaluate a single cubic Bezier curve at parameter t in [0, 1] given its four
// control points.
glm::vec3 bezierCurvePoint(const std::array<glm::vec3, 4> &cps, float t);

// Evaluate a bicubic Bezier patch surface point at (u, v) in [0, 1]. Control
// points are supplied row-major as a 4x4 grid flattened into 16 entries.
glm::vec3 bezierPatchPoint(const std::array<glm::vec3, 16> &cps, float u, float v);

// Compute the (unit-length) surface normal of a bicubic Bezier patch at (u, v)
// from the partial derivatives, as the normalised cross product of dS/du and
// dS/dv. Falls back to a stable default when the derivatives are degenerate.
glm::vec3 bezierPatchNormal(const std::array<glm::vec3, 16> &cps, float u, float v);

// Tessellate one bicubic Bezier patch into a resolution x resolution grid of
// quads, each split into two triangles, giving 2 * resolution * resolution
// triangles. Every triangle carries correct per-vertex normals sampled from the
// analytic patch normal. resolution is clamped to be at least 1.
std::vector<ModelTriangle> tessellateBezierPatch(const std::array<glm::vec3, 16> &cps, int resolution,
                                                 const Colour &colour);

// Tessellate many bicubic Bezier patches and concatenate the resulting
// triangles into a single mesh.
std::vector<ModelTriangle> tessellateBezierPatches(const std::vector<std::array<glm::vec3, 16>> &patches,
                                                   int resolution, const Colour &colour);
