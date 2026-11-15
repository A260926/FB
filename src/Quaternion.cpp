#include "Quaternion.h"

#include <cmath>

const float kEpsilon = 1e-6f;

glm::vec4 quatFromAxisAngle(const glm::vec3 &axis, float radians) {
	glm::vec3 n = glm::normalize(axis);
	float half = 0.5f * radians;
	float s = std::sin(half);
	return glm::vec4(n * s, std::cos(half));
}

glm::vec4 quatMul(const glm::vec4 &a, const glm::vec4 &b) {
	glm::vec3 av(a.x, a.y, a.z);
	glm::vec3 bv(b.x, b.y, b.z);
	glm::vec3 xyz = a.w * bv + b.w * av + glm::cross(av, bv);
	float w = a.w * b.w - glm::dot(av, bv);
	return glm::vec4(xyz, w);
}

glm::vec3 quatRotate(const glm::vec4 &q, const glm::vec3 &v) {
	glm::vec3 qv(q.x, q.y, q.z);
	glm::vec3 t = 2.0f * glm::cross(qv, v);
	return v + q.w * t + glm::cross(qv, t);
}

glm::mat3 quatToMat3(const glm::vec4 &q) {
	float x = q.x, y = q.y, z = q.z, w = q.w;
	float xx = x * x, yy = y * y, zz = z * z;
	float xy = x * y, xz = x * z, yz = y * z;
	float wx = w * x, wy = w * y, wz = w * z;
	// Column-major, matching glm: columns are the rotated basis vectors.
	return glm::mat3(1.0f - 2.0f * (yy + zz), 2.0f * (xy + wz), 2.0f * (xz - wy), 2.0f * (xy - wz),
	                 1.0f - 2.0f * (xx + zz), 2.0f * (yz + wx), 2.0f * (xz + wy), 2.0f * (yz - wx),
	                 1.0f - 2.0f * (xx + yy));
}

glm::vec4 quatIntegrate(const glm::vec4 &q, const glm::vec3 &omega, float dt) {
	glm::vec4 omegaQuat(omega, 0.0f);
	glm::vec4 dq = 0.5f * quatMul(omegaQuat, q) * dt;
	return glm::normalize(q + dq);
}

glm::vec4 slerp(const glm::vec4 &a, glm::vec4 b, float t) {
	float d = glm::dot(a, b);
	// Take the shorter arc across the double cover.
	if (d < 0.0f) {
		b = -b;
		d = -d;
	}
	// Nearly parallel: slerp is numerically unstable, use normalized lerp.
	if (d > 1.0f - kEpsilon) {
		return glm::normalize(a + t * (b - a));
	}
	float theta = std::acos(d);
	float sinTheta = std::sin(theta);
	float wa = std::sin((1.0f - t) * theta) / sinTheta;
	float wb = std::sin(t * theta) / sinTheta;
	return wa * a + wb * b;
}

float quatAngleBetween(const glm::vec4 &a, const glm::vec4 &b) {
	float d = std::fabs(glm::dot(a, b));
	d = glm::clamp(d, -1.0f, 1.0f);
	return 2.0f * std::acos(d);
}

glm::mat3 orthonormalize(const glm::mat3 &m) {
	glm::vec3 c0 = m[0];
	glm::vec3 c1 = m[1];
	glm::vec3 c2 = m[2];

	c0 = glm::normalize(c0);

	c1 = c1 - glm::dot(c1, c0) * c0;
	c1 = glm::normalize(c1);

	c2 = c2 - glm::dot(c2, c0) * c0;
	c2 = c2 - glm::dot(c2, c1) * c1;
	c2 = glm::normalize(c2);

	return glm::mat3(c0, c1, c2);
}
