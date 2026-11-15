#pragma once

#include <glm/glm.hpp>

// View-frustum culling (after Fabian "ryg" Giesen's frustum-culling posts).
//
// A frustum is six half-spaces. Each plane is stored as a vec4 (a, b, c, d)
// where n = (a, b, c) is the inward-pointing normal and the signed distance of a
// point p from the plane is dot(n, p) + d. A point is inside the frustum when
// that value is >= 0 for all six planes.
struct Frustum {
	glm::vec4 planes[6]; // Left, Right, Bottom, Top, Near, Far
};

// Extract the six clip-space planes from a view-projection matrix using the
// Gribb-Hartmann row-sum/difference method (OpenGL clip volume, -w <= x,y,z <= w).
// Each plane is normalised so that dot(plane.xyz, p) + plane.w is a true signed
// distance.
Frustum extractFrustum(const glm::mat4 &viewProj);

// Classification of an AABB against the frustum.
enum class Cull { Outside, Intersect, Inside };

// Test an axis-aligned box given in centre / half-extent form. Uses the
// projection-radius trick: for each plane r is the box's extent projected onto
// the plane normal and s is the centre's signed distance. s + r < 0 means the
// whole box is on the outside of that plane (fully culled); s - r < 0 means the
// box straddles the plane.
Cull testAABB(const Frustum &f, const glm::vec3 &centre, const glm::vec3 &halfExtent);

// Convenience wrapper taking the box as min / max corners. Returns true when the
// box is completely outside the frustum (i.e. it can be skipped).
bool aabbOutside(const Frustum &f, const glm::vec3 &mn, const glm::vec3 &mx);

// True when the point lies inside (or on) all six planes.
bool pointInside(const Frustum &f, const glm::vec3 &p);
