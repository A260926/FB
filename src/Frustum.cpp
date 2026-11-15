#include "Frustum.h"

#include <cmath>

// glm stores matrices column-major: m[col][row]. The Gribb-Hartmann method
// works with the matrix "rows", so rowI.k reads across the columns as m[k][I].
namespace {
glm::vec4 matrixRow(const glm::mat4 &m, int i) {
	return glm::vec4(m[0][i], m[1][i], m[2][i], m[3][i]);
}

glm::vec4 normalisePlane(glm::vec4 p) {
	float len = std::sqrt(p.x * p.x + p.y * p.y + p.z * p.z);
	if (len > 0.0f) {
		p /= len;
	}
	return p;
}
} // namespace

Frustum extractFrustum(const glm::mat4 &viewProj) {
	const glm::vec4 r0 = matrixRow(viewProj, 0);
	const glm::vec4 r1 = matrixRow(viewProj, 1);
	const glm::vec4 r2 = matrixRow(viewProj, 2);
	const glm::vec4 r3 = matrixRow(viewProj, 3);

	Frustum f;
	f.planes[0] = normalisePlane(r3 + r0); // Left
	f.planes[1] = normalisePlane(r3 - r0); // Right
	f.planes[2] = normalisePlane(r3 + r1); // Bottom
	f.planes[3] = normalisePlane(r3 - r1); // Top
	f.planes[4] = normalisePlane(r3 + r2); // Near
	f.planes[5] = normalisePlane(r3 - r2); // Far
	return f;
}

Cull testAABB(const Frustum &f, const glm::vec3 &centre, const glm::vec3 &halfExtent) {
	bool intersecting = false;
	for (int i = 0; i < 6; ++i) {
		const glm::vec3 n(f.planes[i]);
		// Projected radius of the box onto the plane normal.
		float r = glm::dot(halfExtent, glm::abs(n));
		// Signed distance of the box centre from the plane.
		float s = glm::dot(centre, n) + f.planes[i].w;
		if (s + r < 0.0f) {
			return Cull::Outside; // Whole box behind this plane.
		}
		if (s - r < 0.0f) {
			intersecting = true; // Box straddles this plane.
		}
	}
	return intersecting ? Cull::Intersect : Cull::Inside;
}

bool aabbOutside(const Frustum &f, const glm::vec3 &mn, const glm::vec3 &mx) {
	const glm::vec3 centre = (mn + mx) * 0.5f;
	const glm::vec3 halfExtent = (mx - mn) * 0.5f;
	return testAABB(f, centre, halfExtent) == Cull::Outside;
}

bool pointInside(const Frustum &f, const glm::vec3 &p) {
	for (int i = 0; i < 6; ++i) {
		if (glm::dot(glm::vec3(f.planes[i]), p) + f.planes[i].w < 0.0f) {
			return false;
		}
	}
	return true;
}
