#pragma once

#include <ModelTriangle.h>
#include <RayTriangleIntersection.h>
#include <glm/glm.hpp>
#include <vector>

// Geometric normal of a triangle.
glm::vec3 triangleNormal(const ModelTriangle &triangle);

// Ray-triangle test (matrix-inverse barycentric solve). Returns true and fills
// t (distance), u, v (barycentric) on a valid front hit (t > epsilon).
bool intersectTriangle(const glm::vec3 &origin, const glm::vec3 &direction, const ModelTriangle &triangle, float &t,
                       float &u, float &v);

// Same test solved by Cramer's rule (three scalar determinants) instead of a
// full matrix inverse - fewer operations on the hot path. Identical results.
bool intersectTriangleCramer(const glm::vec3 &origin, const glm::vec3 &direction, const ModelTriangle &triangle,
                             float &t, float &u, float &v);

// Brute-force closest hit over every triangle. `intersection.hit` is false when
// nothing was hit; `ignoreIndex` skips one triangle (e.g. for shadow rays).
RayTriangleIntersection getClosestIntersection(const glm::vec3 &origin, const glm::vec3 &direction,
                                               const std::vector<ModelTriangle> &triangles, int ignoreIndex = -1);
