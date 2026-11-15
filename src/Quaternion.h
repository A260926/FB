#pragma once

#include <glm/glm.hpp>

// Quaternion orientation utilities. A quaternion is stored as a glm::vec4 with
// the imaginary part in .xyz and the real (scalar) part in .w. A unit
// quaternion (w = cos(theta/2), xyz = axis * sin(theta/2)) encodes a rotation
// of angle theta about `axis`.

// Unit quaternion for a rotation of `radians` about `axis` (axis is normalized
// internally, so its length does not matter).
glm::vec4 quatFromAxisAngle(const glm::vec3 &axis, float radians);

// Hamilton product a * b. Composes two rotations: applying the result is the
// same as applying b first, then a.
glm::vec4 quatMul(const glm::vec4 &a, const glm::vec4 &b);

// Rotate vector `v` by unit quaternion `q` using the optimized form that avoids
// building a matrix: t = 2 * cross(q.xyz, v); v + q.w * t + cross(q.xyz, t).
glm::vec3 quatRotate(const glm::vec4 &q, const glm::vec3 &v);

// Rotation matrix equivalent to unit quaternion `q`. Cheaper than quatRotate
// when transforming many vectors by the same orientation (bulk vertex work).
glm::mat3 quatToMat3(const glm::vec4 &q);

// First-order integration of orientation `q` under angular velocity `omega`
// (radians/second, direction is the axis) over timestep `dt`. Adds
// 0.5 * (omega as a pure quaternion) * q * dt, then renormalizes.
glm::vec4 quatIntegrate(const glm::vec4 &q, const glm::vec3 &omega, float dt);

// Shortest-path spherical linear interpolation from `a` to `b` at parameter
// `t` in [0, 1]. `b` is flipped when dot(a, b) < 0 to take the short arc across
// the double cover, and falls back to normalized lerp when the inputs are
// nearly parallel (sin(theta) close to zero).
glm::vec4 slerp(const glm::vec4 &a, glm::vec4 b, float t);

// Angle in radians between the orientations `a` and `b`: 2 * acos(|dot(a, b)|).
// The absolute value keeps antipodal quaternions (same orientation) at angle 0.
float quatAngleBetween(const glm::vec4 &a, const glm::vec4 &b);

// Nearest orthonormal matrix to `m` via modified Gram-Schmidt: orthogonalize
// each column against the columns already fixed, then normalize it. More stable
// than classical Gram-Schmidt for near-degenerate input. Use to remove drift
// from an accumulated rotation matrix.
glm::mat3 orthonormalize(const glm::mat3 &m);
