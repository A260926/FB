#include "Geometry.h"

#include <cmath>

glm::vec3 triangleNormal(const ModelTriangle &triangle) {
	glm::vec3 e0 = triangle.vertices[1] - triangle.vertices[0];
	glm::vec3 e1 = triangle.vertices[2] - triangle.vertices[0];
	return glm::normalize(glm::cross(e0, e1));
}

bool intersectTriangle(const glm::vec3 &origin, const glm::vec3 &direction, const ModelTriangle &triangle, float &t,
                       float &u, float &v) {
	glm::vec3 e0 = triangle.vertices[1] - triangle.vertices[0];
	glm::vec3 e1 = triangle.vertices[2] - triangle.vertices[0];
	glm::vec3 spVector = origin - triangle.vertices[0];
	glm::mat3 deMatrix(-direction, e0, e1);
	float det = glm::determinant(deMatrix);
	if (std::abs(det) < 1e-8f)
		return false; // ray parallel to the triangle plane
	glm::vec3 possibleSolution = glm::inverse(deMatrix) * spVector;
	t = possibleSolution.x;
	u = possibleSolution.y;
	v = possibleSolution.z;
	return (u >= 0.0f) && (v >= 0.0f) && (u + v <= 1.0f) && (t > 1e-5f);
}

bool intersectTriangleCramer(const glm::vec3 &origin, const glm::vec3 &direction, const ModelTriangle &triangle,
                             float &t, float &u, float &v) {
	// Solve [-d, e0, e1] [t u v]^T = (o - v0) by Cramer's rule: each unknown is a
	// determinant ratio. Same system as the matrix-inverse form, fewer ops.
	glm::vec3 e0 = triangle.vertices[1] - triangle.vertices[0];
	glm::vec3 e1 = triangle.vertices[2] - triangle.vertices[0];
	glm::vec3 b = origin - triangle.vertices[0];
	glm::vec3 nd = -direction;
	float det = glm::determinant(glm::mat3(nd, e0, e1));
	if (std::abs(det) < 1e-8f)
		return false;
	float inv = 1.0f / det;
	t = glm::determinant(glm::mat3(b, e0, e1)) * inv;
	u = glm::determinant(glm::mat3(nd, b, e1)) * inv;
	v = glm::determinant(glm::mat3(nd, e0, b)) * inv;
	return (u >= 0.0f) && (v >= 0.0f) && (u + v <= 1.0f) && (t > 1e-5f);
}

RayTriangleIntersection getClosestIntersection(const glm::vec3 &origin, const glm::vec3 &direction,
                                               const std::vector<ModelTriangle> &triangles, int ignoreIndex) {
	RayTriangleIntersection closest; // hit = false, distance = +inf
	for (size_t i = 0; i < triangles.size(); i++) {
		if (static_cast<int>(i) == ignoreIndex)
			continue;
		float t, u, v;
		if (intersectTriangle(origin, direction, triangles[i], t, u, v) && t < closest.distanceFromCamera) {
			closest.hit = true;
			closest.distanceFromCamera = t;
			closest.intersectionPoint = origin + t * direction;
			closest.intersectedTriangle = triangles[i];
			closest.triangleIndex = i;
			closest.u = u;
			closest.v = v;
		}
	}
	return closest;
}
