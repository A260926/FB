#include "Nurbs.h"

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace {

// Uniform cubic B-spline basis functions evaluated at local parameter t in
// [0, 1]. Writes the four blending weights for the four control points that
// influence a single span.
void cubicBasis(float t, float b[4]) {
	const float t2 = t * t;
	const float t3 = t2 * t;
	const float oneMinusT = 1.0f - t;
	b[0] = (oneMinusT * oneMinusT * oneMinusT) / 6.0f;
	b[1] = (3.0f * t3 - 6.0f * t2 + 4.0f) / 6.0f;
	b[2] = (-3.0f * t3 + 3.0f * t2 + 3.0f * t + 1.0f) / 6.0f;
	b[3] = t3 / 6.0f;
}

// Map a global parameter g in [0, 1] onto a uniform cubic B-spline defined by
// `count` control points (count >= 4). Produces the index of the first of the
// four control points for the active span and the local parameter within it.
void resolveSpan(float g, int count, int &baseIndex, float &localT) {
	const int spans = count - 3; // number of cubic spans
	g = std::clamp(g, 0.0f, 1.0f);
	float scaled = g * static_cast<float>(spans);
	int span = static_cast<int>(std::floor(scaled));
	if (span < 0) {
		span = 0;
	}
	if (span > spans - 1) {
		span = spans - 1;
	}
	baseIndex = span;
	localT = scaled - static_cast<float>(span);
}

} // namespace

glm::vec3 bsplineSurfacePoint(const std::vector<std::vector<glm::vec3>> &controlGrid, float u, float v,
                              const std::vector<std::vector<float>> &weights) {
	const int rows = static_cast<int>(controlGrid.size());
	if (rows < 4) {
		return glm::vec3(0.0f);
	}
	const int cols = static_cast<int>(controlGrid[0].size());
	if (cols < 4) {
		return glm::vec3(0.0f);
	}

	int iu = 0;
	int iv = 0;
	float tu = 0.0f;
	float tv = 0.0f;
	resolveSpan(u, rows, iu, tu);
	resolveSpan(v, cols, iv, tv);

	float bu[4];
	float bv[4];
	cubicBasis(tu, bu);
	cubicBasis(tv, bv);

	const bool rational = !weights.empty();

	glm::vec3 numerator(0.0f);
	float denominator = 0.0f;
	for (int a = 0; a < 4; ++a) {
		for (int b = 0; b < 4; ++b) {
			const int ri = iu + a;
			const int ci = iv + b;
			const float basis = bu[a] * bv[b];
			float w = 1.0f;
			if (rational) {
				w = weights[static_cast<std::size_t>(ri)][static_cast<std::size_t>(ci)];
			}
			const float coeff = basis * w;
			numerator += coeff * controlGrid[static_cast<std::size_t>(ri)][static_cast<std::size_t>(ci)];
			denominator += coeff;
		}
	}

	if (std::fabs(denominator) < 1e-8f) {
		return glm::vec3(0.0f);
	}
	return numerator / denominator;
}

std::vector<ModelTriangle> tessellateSplineSurface(const std::vector<std::vector<glm::vec3>> &controlGrid, int uSteps,
                                                   int vSteps, Colour colour,
                                                   const std::vector<std::vector<float>> &weights) {
	std::vector<ModelTriangle> triangles;
	if (uSteps < 1 || vSteps < 1) {
		return triangles;
	}
	if (controlGrid.size() < 4 || controlGrid[0].size() < 4) {
		return triangles;
	}

	const int uCount = uSteps + 1;
	const int vCount = vSteps + 1;

	// Sample surface positions on the parameter grid.
	std::vector<std::vector<glm::vec3>> points(static_cast<std::size_t>(uCount),
	                                           std::vector<glm::vec3>(static_cast<std::size_t>(vCount)));
	std::vector<std::vector<glm::vec3>> normals(static_cast<std::size_t>(uCount),
	                                            std::vector<glm::vec3>(static_cast<std::size_t>(vCount)));

	// Finite-difference step in parameter space (small relative to a step).
	const float du = 1.0f / static_cast<float>(uSteps);
	const float dv = 1.0f / static_cast<float>(vSteps);
	const float hu = du * 1e-3f;
	const float hv = dv * 1e-3f;

	for (int i = 0; i < uCount; ++i) {
		const float u = static_cast<float>(i) / static_cast<float>(uSteps);
		for (int j = 0; j < vCount; ++j) {
			const float v = static_cast<float>(j) / static_cast<float>(vSteps);
			points[static_cast<std::size_t>(i)][static_cast<std::size_t>(j)] =
			    bsplineSurfacePoint(controlGrid, u, v, weights);

			// Central finite differences for the partial derivatives, clamping
			// the sample points to keep them inside the [0, 1] domain.
			const float u0 = std::max(0.0f, u - hu);
			const float u1 = std::min(1.0f, u + hu);
			const float v0 = std::max(0.0f, v - hv);
			const float v1 = std::min(1.0f, v + hv);
			const glm::vec3 dPdu =
			    bsplineSurfacePoint(controlGrid, u1, v, weights) - bsplineSurfacePoint(controlGrid, u0, v, weights);
			const glm::vec3 dPdv =
			    bsplineSurfacePoint(controlGrid, u, v1, weights) - bsplineSurfacePoint(controlGrid, u, v0, weights);
			glm::vec3 n = glm::cross(dPdu, dPdv);
			const float len = glm::length(n);
			if (len > 1e-8f) {
				n /= len;
			} else {
				n = glm::vec3(0.0f, 1.0f, 0.0f);
			}
			normals[static_cast<std::size_t>(i)][static_cast<std::size_t>(j)] = n;
		}
	}

	triangles.reserve(static_cast<std::size_t>(2 * uSteps * vSteps));
	for (int i = 0; i < uSteps; ++i) {
		for (int j = 0; j < vSteps; ++j) {
			const std::size_t i0 = static_cast<std::size_t>(i);
			const std::size_t i1 = static_cast<std::size_t>(i + 1);
			const std::size_t j0 = static_cast<std::size_t>(j);
			const std::size_t j1 = static_cast<std::size_t>(j + 1);

			const glm::vec3 &p00 = points[i0][j0];
			const glm::vec3 &p10 = points[i1][j0];
			const glm::vec3 &p01 = points[i0][j1];
			const glm::vec3 &p11 = points[i1][j1];

			const glm::vec3 &n00 = normals[i0][j0];
			const glm::vec3 &n10 = normals[i1][j0];
			const glm::vec3 &n01 = normals[i0][j1];
			const glm::vec3 &n11 = normals[i1][j1];

			ModelTriangle t1(p00, p10, p11, colour);
			t1.vertexNormals = {{n00, n10, n11}};
			glm::vec3 fn1 = glm::cross(p10 - p00, p11 - p00);
			const float l1 = glm::length(fn1);
			t1.normal = (l1 > 1e-8f) ? fn1 / l1 : n00;
			triangles.push_back(t1);

			ModelTriangle t2(p00, p11, p01, colour);
			t2.vertexNormals = {{n00, n11, n01}};
			glm::vec3 fn2 = glm::cross(p11 - p00, p01 - p00);
			const float l2 = glm::length(fn2);
			t2.normal = (l2 > 1e-8f) ? fn2 / l2 : n00;
			triangles.push_back(t2);
		}
	}

	return triangles;
}
