#include "Camera.h"

#include <cmath>

Camera::Camera(int width, int height, float focal, const glm::vec3 &pos)
    : position(pos), focalLength(focal), imageWidth(width), imageHeight(height) {}

glm::vec3 Camera::toCameraSpace(const glm::vec3 &vertex) const {
	// Offset into the camera frame: transpose (== inverse for an orthonormal
	// matrix) maps the world-space offset onto the camera's axes.
	return glm::transpose(orientation) * (vertex - position);
}

CanvasPoint Camera::projectCameraPoint(const glm::vec3 &cam) const {
	// In front of the camera means cam.z < 0. Report a positive depth so a
	// smaller depth is closer, which suits a simple z-buffer.
	float depth = -cam.z;
	float u = -focalLength * (cam.x / cam.z) * scale + imageWidth / 2.0f;
	float v = focalLength * (cam.y / cam.z) * scale + imageHeight / 2.0f;
	return CanvasPoint(u, v, depth);
}

CanvasPoint Camera::projectVertex(const glm::vec3 &vertex) const {
	return projectCameraPoint(toCameraSpace(vertex));
}

void Camera::lookAt(const glm::vec3 &target) {
	glm::vec3 forward = glm::normalize(position - target); // camera's +z (points away from target)
	glm::vec3 worldUp(0.0f, 1.0f, 0.0f);
	glm::vec3 right = glm::normalize(glm::cross(worldUp, forward));
	glm::vec3 up = glm::cross(forward, right);
	orientation = glm::mat3(right, up, forward);
}

void Camera::translate(const glm::vec3 &delta) {
	position += delta;
}

void Camera::rotateX(float radians) {
	float c = std::cos(radians);
	float s = std::sin(radians);
	glm::mat3 rot(1.0f, 0.0f, 0.0f, 0.0f, c, s, 0.0f, -s, c);
	orientation = orientation * rot;
}

void Camera::rotateY(float radians) {
	float c = std::cos(radians);
	float s = std::sin(radians);
	glm::mat3 rot(c, 0.0f, -s, 0.0f, 1.0f, 0.0f, s, 0.0f, c);
	orientation = orientation * rot;
}

// Roll: rotate about the local forward (z) axis, tilting the horizon.
void Camera::rotateZ(float radians) {
	float c = std::cos(radians);
	float s = std::sin(radians);
	glm::mat3 rot(c, s, 0.0f, -s, c, 0.0f, 0.0f, 0.0f, 1.0f);
	orientation = orientation * rot;
}

void Camera::orbitY(float radians, const glm::vec3 &target) {
	float c = std::cos(radians);
	float s = std::sin(radians);
	glm::mat3 rot(c, 0.0f, -s, 0.0f, 1.0f, 0.0f, s, 0.0f, c);
	position = target + rot * (position - target);
	lookAt(target);
}

void Camera::primaryRay(float sx, float sy, float f, glm::vec3 &origin, glm::vec3 &direction) const {
	if (orthographic) {
		// Parallel rays: all point down the camera's forward axis; the origin
		// slides across the image plane instead of the direction fanning out.
		origin = position + orientation * glm::vec3(sx / f * orthoScale, sy / f * orthoScale, 0.0f);
		direction = glm::normalize(orientation * glm::vec3(0.0f, 0.0f, -1.0f));
	} else {
		origin = position;
		direction = glm::normalize(orientation * glm::vec3(sx / f, sy / f, -1.0f));
	}
}
