#include "Tessellator.h"

#include <algorithm>

// See Tessellator.h for the design rationale (domain topology first, evaluation
// second). This file implements the domain generation in abstract (u, v) space
// plus one concrete evaluator hook onto the bicubic Bezier patch.

std::vector<glm::vec2> tessellateQuadDomain(int edgeFactorU, int edgeFactorV) {
	int nu = std::max(edgeFactorU, 1);
	int nv = std::max(edgeFactorV, 1);

	std::vector<glm::vec2> samples;
	samples.reserve(static_cast<size_t>(nu * nv));

	// Uniform integer partitioning of each axis. With a single sample the axis
	// collapses to u = 0; otherwise the samples span the closed range [0, 1].
	for (int iu = 0; iu < nu; iu++) {
		float u = (nu > 1) ? static_cast<float>(iu) / static_cast<float>(nu - 1) : 0.0f;
		for (int iv = 0; iv < nv; iv++) {
			float v = (nv > 1) ? static_cast<float>(iv) / static_cast<float>(nv - 1) : 0.0f;
			samples.emplace_back(u, v);
		}
	}

	return samples;
}

std::vector<std::array<int, 3>> quadDomainTriangles(int nu, int nv) {
	std::vector<std::array<int, 3>> triangles;
	if (nu < 2 || nv < 2) {
		return triangles;
	}
	triangles.reserve(static_cast<size_t>(2 * (nu - 1) * (nv - 1)));

	// Row-major indexing must match tessellateQuadDomain: index = iu * nv + iv.
	for (int iu = 0; iu < nu - 1; iu++) {
		for (int iv = 0; iv < nv - 1; iv++) {
			int k00 = iu * nv + iv;
			int k01 = iu * nv + (iv + 1);
			int k10 = (iu + 1) * nv + iv;
			int k11 = (iu + 1) * nv + (iv + 1);

			triangles.push_back({k00, k10, k11});
			triangles.push_back({k00, k11, k01});
		}
	}

	return triangles;
}

std::vector<glm::vec2> tessellateTriangleDomain(int factor) {
	int n = std::max(factor, 1);

	std::vector<glm::vec2> samples;

	// Walk concentric rings from the boundary inward. Ring i is inset by s = i/n
	// from every edge; its three corners are the barycentric points
	//   (1 - 2s, s, s), (s, 1 - 2s, s), (s, s, 1 - 2s)
	// mapped to (u, v) = (second weight, third weight) with w = 1 - u - v. The
	// number of segments along each edge of ring i is seg = n - 2 * i.
	for (int i = 0;; i++) {
		int seg = n - 2 * i;
		if (seg < 0) {
			break;
		}

		float s = static_cast<float>(i) / static_cast<float>(n);

		if (seg == 0) {
			// Even factor: the rings collapse to a single centre vertex.
			samples.emplace_back(1.0f / 3.0f, 1.0f / 3.0f);
			break;
		}

		// Ring corners as (u, v). At seg == 1 (odd factor, innermost ring) these
		// three corners are exactly the small centre triangle.
		glm::vec2 c0(s, s);
		glm::vec2 c1(1.0f - 2.0f * s, s);
		glm::vec2 c2(s, 1.0f - 2.0f * s);
		std::array<glm::vec2, 3> corners{c0, c1, c2};

		// Emit points along each edge, including the start corner but excluding
		// the end corner so shared corners are not duplicated. This yields
		// 3 * seg points around the ring.
		for (int e = 0; e < 3; e++) {
			const glm::vec2 &a = corners[e];
			const glm::vec2 &b = corners[(e + 1) % 3];
			for (int k = 0; k < seg; k++) {
				float t = static_cast<float>(k) / static_cast<float>(seg);
				samples.push_back(a + (b - a) * t);
			}
		}

		if (seg == 1) {
			break;
		}
	}

	return samples;
}

std::vector<ModelTriangle> adaptiveTessellateBezier(const std::array<glm::vec3, 16> &cps, int edgeFactorU,
                                                    int edgeFactorV, const Colour &colour) {
	int nu = std::max(edgeFactorU, 1);
	int nv = std::max(edgeFactorV, 1);

	// Domain topology: (u, v) samples and the triangles that connect them.
	std::vector<glm::vec2> domain = tessellateQuadDomain(nu, nv);
	std::vector<std::array<int, 3>> indices = quadDomainTriangles(nu, nv);

	// Evaluate every domain sample once through the Bezier patch evaluator.
	std::vector<glm::vec3> points(domain.size());
	std::vector<glm::vec3> normals(domain.size());
	for (size_t k = 0; k < domain.size(); k++) {
		points[k] = bezierPatchPoint(cps, domain[k].x, domain[k].y);
		normals[k] = bezierPatchNormal(cps, domain[k].x, domain[k].y);
	}

	std::vector<ModelTriangle> triangles;
	triangles.reserve(indices.size());
	for (const std::array<int, 3> &tri : indices) {
		int a = tri[0];
		int b = tri[1];
		int c = tri[2];
		ModelTriangle t(points[a], points[b], points[c], colour);
		t.vertexNormals = {normals[a], normals[b], normals[c]};
		t.normal = glm::normalize(normals[a] + normals[b] + normals[c]);
		triangles.push_back(t);
	}

	return triangles;
}
