#pragma once

#include <glm/glm.hpp>
#include <array>
#include <vector>
#include "ModelTriangle.h"
#include "Bezier.h"

// Hardware-style adaptive tessellation, following the domain/topology split used
// by a real GPU tessellation stage (see Fabian Giesen's GPU pipeline series,
// part 12). The tessellator only ever works in abstract domain space: it emits
// (u, v) sample positions and a triangle index list describing how those samples
// connect. It never touches world-space geometry itself. A separate evaluator
// (here the bicubic Bezier patch in Bezier.h) turns each domain (u, v) into an
// actual surface point.
//
// Splitting the problem this way is what lets tessellation factors be chosen
// per edge at runtime: the domain topology is generated to match the requested
// factors, then evaluated. This is the key difference from the fixed-resolution
// tessellateBezierPatch, which always uses the same count on both axes.
//
// Crack-free (watertight) note: two patches that share an edge stay watertight
// as long as they request the same integer tessellation factor along that shared
// edge, because both then generate identical domain samples on it and therefore
// identical evaluated surface points. Fractional (continuous) partitioning would
// be needed to blend smoothly between differing factors without T-junctions;
// that is intentionally not implemented here.

// Quad domain. Generate a grid of (u, v) samples in [0, 1]^2 using uniform
// integer partitioning. edgeFactorU / edgeFactorV are the number of samples
// along each axis (each clamped to at least 1). The result is row-major with
// edgeFactorV samples per row: index(iu, iv) = iu * edgeFactorV + iv. So a call
// of (5, 5) yields a 5x5 grid of 25 points.
std::vector<glm::vec2> tessellateQuadDomain(int edgeFactorU, int edgeFactorV);

// Triangle index list connecting a grid of nu x nv samples laid out row-major as
// produced by tessellateQuadDomain. Every interior cell becomes two triangles
// with consistent winding, giving 2 * (nu - 1) * (nv - 1) triangles. Indices
// refer into the row-major sample array.
std::vector<std::array<int, 3>> quadDomainTriangles(int nu, int nv);

// Triangle domain. Generate a concentric-ring barycentric sample pattern for a
// triangular patch at the given inside tessellation factor. Rings are inset from
// the boundary towards the centre. An odd factor terminates in a small centre
// triangle; an even factor terminates in a single centre vertex. Each returned
// glm::vec2 holds (u, v) barycentric-ish coordinates with the third weight
// w = 1 - u - v implied.
std::vector<glm::vec2> tessellateTriangleDomain(int factor);

// Adaptive quad-patch tessellation of a bicubic Bezier patch. Builds the domain
// grid with tessellateQuadDomain, connects it with quadDomainTriangles, then
// evaluates every sample through bezierPatchPoint (with per-vertex normals from
// bezierPatchNormal) to emit ModelTriangles. Because edgeFactorU and edgeFactorV
// are independent, the two axes can be tessellated at different resolutions,
// unlike tessellateBezierPatch.
std::vector<ModelTriangle> adaptiveTessellateBezier(const std::array<glm::vec3, 16> &cps, int edgeFactorU,
                                                    int edgeFactorV, const Colour &colour);
