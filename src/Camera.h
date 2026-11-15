#pragma once

#include <CanvasPoint.h>
#include <glm/glm.hpp>

// A pinhole camera. Free of any SDL/window dependency so the projection maths
// can be unit-tested headlessly.
//
// Convention: the camera looks down its local -z axis. `orientation` is an
// orthonormal mat3 whose columns are the camera's right (x), up (y) and
// backward (z) axes in world space, so transpose(orientation) maps a
// world-space offset into camera space.
class Camera {
public:
	glm::vec3 position{0.0f, 0.0f, 4.0f};
	glm::mat3 orientation{1.0f};
	float focalLength = 2.0f;
	float scale = 150.0f; // world units -> pixels
	int imageWidth = 320;
	int imageHeight = 240;

	// Orthographic projection: parallel primary rays instead of a pinhole. Off by
	// default; orthoScale sets the world-space half-extent mapped across the image.
	bool orthographic = false;
	float orthoScale = 1.0f;

	Camera() = default;
	Camera(int width, int height, float focal, const glm::vec3 &pos);

	// Build a primary ray for image-plane offset (sx, sy) (sx = x - W/2,
	// sy = -(y - H/2)) at focal-plane scale f. Perspective by default; parallel
	// rays from a shifted origin when orthographic.
	void primaryRay(float sx, float sy, float f, glm::vec3 &origin, glm::vec3 &direction) const;

	// Transform a world-space vertex into camera space (camera at origin looking -z).
	glm::vec3 toCameraSpace(const glm::vec3 &vertex) const;

	// Project a camera-space point onto the image plane (used after clipping).
	CanvasPoint projectCameraPoint(const glm::vec3 &cameraSpacePoint) const;

	// Project a world-space vertex onto the image plane. The returned CanvasPoint
	// carries a positive `depth` (distance in front of the camera); depth <= 0
	// means the vertex is level with or behind the camera.
	CanvasPoint projectVertex(const glm::vec3 &vertex) const;

	// Aim the camera at a target point, keeping a world-up of +y.
	void lookAt(const glm::vec3 &target);

	// Movement helpers.
	void translate(const glm::vec3 &delta);
	void rotateX(float radians); // pitch
	void rotateY(float radians); // yaw
	void rotateZ(float radians); // roll
	void orbitY(float radians, const glm::vec3 &target);
};
