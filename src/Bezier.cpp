#include "Bezier.h"

#include <algorithm>
#include <cmath>

// This file implements bicubic Bezier patch evaluation and tessellation using
// the Bernstein polynomial form of the cubic Bezier basis.
//
// The cubic Bernstein basis functions for parameter t are:
//   B0(t) = (1 - t)^3
//   B1(t) = 3 * (1 - t)^2 * t
//   B2(t) = 3 * (1 - t) * t^2
//   B3(t) = t^3
// Their derivatives with respect to t are:
//   B0'(t) = -3 * (1 - t)^2
//   B1'(t) =  3 * (1 - t)^2 - 6 * (1 - t) * t
//   B2'(t) =  6 * (1 - t) * t - 3 * t^2
//   B3'(t) =  3 * t^2

namespace {

// Evaluate the four cubic Bernstein basis functions at t.
std::array<float, 4> bernstein(float t) {
	float mt = 1.0f - t;
	float mt2 = mt * mt;
	float t2 = t * t;
	return {mt2 * mt, 3.0f * mt2 * t, 3.0f * mt * t2, t2 * t};
}

// Evaluate the derivatives of the four cubic Bernstein basis functions at t.
std::array<float, 4> bernsteinDerivative(float t) {
	float mt = 1.0f - t;
	float mt2 = mt * mt;
	float t2 = t * t;
	return {-3.0f * mt2, 3.0f * mt2 - 6.0f * mt * t, 6.0f * mt * t - 3.0f * t2, 3.0f * t2};
}

// Weighted combination of four control points by four basis coefficients.
glm::vec3 combine(const glm::vec3 &p0, const glm::vec3 &p1, const glm::vec3 &p2, const glm::vec3 &p3,
                  const std::array<float, 4> &b) {
	return b[0] * p0 + b[1] * p1 + b[2] * p2 + b[3] * p3;
}

// Row-major index helper for the 4x4 control grid.
inline int idx(int row, int col) {
	return row * 4 + col;
}

} // namespace

glm::vec3 bezierCurvePoint(const std::array<glm::vec3, 4> &cps, float t) {
	std::array<float, 4> b = bernstein(t);
	return combine(cps[0], cps[1], cps[2], cps[3], b);
}

glm::vec3 bezierPatchPoint(const std::array<glm::vec3, 16> &cps, float u, float v) {
	// Evaluate the four v-direction curves (one per row) at v, producing four
	// intermediate control points, then evaluate the resulting u-direction
	// curve at u. This is the standard tensor-product Bezier evaluation.
	std::array<float, 4> bv = bernstein(v);
	std::array<glm::vec3, 4> rowPoints{};
	for (int row = 0; row < 4; row++) {
		rowPoints[row] = combine(cps[idx(row, 0)], cps[idx(row, 1)], cps[idx(row, 2)], cps[idx(row, 3)], bv);
	}
	std::array<float, 4> bu = bernstein(u);
	return combine(rowPoints[0], rowPoints[1], rowPoints[2], rowPoints[3], bu);
}

glm::vec3 bezierPatchNormal(const std::array<glm::vec3, 16> &cps, float u, float v) {
	std::array<float, 4> bu = bernstein(u);
	std::array<float, 4> bv = bernstein(v);
	std::array<float, 4> du = bernsteinDerivative(u);
	std::array<float, 4> dv = bernsteinDerivative(v);

	// Partial derivative with respect to u: derivative basis in u, value basis
	// in v. Partial derivative with respect to v: value basis in u, derivative
	// basis in v.
	glm::vec3 dSdu(0.0f);
	glm::vec3 dSdv(0.0f);
	for (int row = 0; row < 4; row++) {
		for (int col = 0; col < 4; col++) {
			const glm::vec3 &p = cps[idx(row, col)];
			dSdu += (du[row] * bv[col]) * p;
			dSdv += (bu[row] * dv[col]) * p;
		}
	}

	glm::vec3 n = glm::cross(dSdu, dSdv);
	float len = glm::length(n);
	if (len > 1e-8f) {
		return n / len;
	}

	// Degenerate derivatives (for example at a collapsed patch corner). Fall
	// back to a nudged sample so callers still receive a usable unit normal.
	float uu = std::clamp(u, 1e-3f, 1.0f - 1e-3f);
	float vv = std::clamp(v, 1e-3f, 1.0f - 1e-3f);
	if (uu != u || vv != v) {
		return bezierPatchNormal(cps, uu, vv);
	}
	return glm::vec3(0.0f, 1.0f, 0.0f);
}

std::vector<ModelTriangle> tessellateBezierPatch(const std::array<glm::vec3, 16> &cps, int resolution,
                                                 const Colour &colour) {
	int res = std::max(resolution, 1);
	std::vector<ModelTriangle> triangles;
	triangles.reserve(static_cast<size_t>(2 * res * res));

	// Pre-sample a (res+1) x (res+1) grid of surface points and normals so each
	// shared vertex is only evaluated once.
	int side = res + 1;
	std::vector<glm::vec3> points(static_cast<size_t>(side * side));
	std::vector<glm::vec3> normals(static_cast<size_t>(side * side));
	for (int i = 0; i <= res; i++) {
		float u = static_cast<float>(i) / static_cast<float>(res);
		for (int j = 0; j <= res; j++) {
			float v = static_cast<float>(j) / static_cast<float>(res);
			size_t k = static_cast<size_t>(i * side + j);
			points[k] = bezierPatchPoint(cps, u, v);
			normals[k] = bezierPatchNormal(cps, u, v);
		}
	}

	// Emit two triangles per grid cell with consistent winding.
	for (int i = 0; i < res; i++) {
		for (int j = 0; j < res; j++) {
			size_t k00 = static_cast<size_t>(i * side + j);
			size_t k01 = static_cast<size_t>(i * side + (j + 1));
			size_t k10 = static_cast<size_t>((i + 1) * side + j);
			size_t k11 = static_cast<size_t>((i + 1) * side + (j + 1));

			ModelTriangle t1(points[k00], points[k10], points[k11], colour);
			t1.vertexNormals = {normals[k00], normals[k10], normals[k11]};
			t1.normal = glm::normalize(normals[k00] + normals[k10] + normals[k11]);
			triangles.push_back(t1);

			ModelTriangle t2(points[k00], points[k11], points[k01], colour);
			t2.vertexNormals = {normals[k00], normals[k11], normals[k01]};
			t2.normal = glm::normalize(normals[k00] + normals[k11] + normals[k01]);
			triangles.push_back(t2);
		}
	}

	return triangles;
}

std::vector<ModelTriangle> tessellateBezierPatches(const std::vector<std::array<glm::vec3, 16>> &patches,
                                                   int resolution, const Colour &colour) {
	std::vector<ModelTriangle> triangles;
	int res = std::max(resolution, 1);
	triangles.reserve(patches.size() * static_cast<size_t>(2 * res * res));
	for (const auto &patch : patches) {
		std::vector<ModelTriangle> patchTris = tessellateBezierPatch(patch, resolution, colour);
		triangles.insert(triangles.end(), patchTris.begin(), patchTris.end());
	}
	return triangles;
}
