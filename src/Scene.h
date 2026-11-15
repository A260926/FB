#pragma once

#include "BVH.h"
#include <ModelTriangle.h>
#include <RayTriangleIntersection.h>
#include <glm/glm.hpp>
#include <vector>

// Analytic primitives (exact, no tessellation). Each carries a colour, a
// material and (for Metal) a roughness, exactly like a triangle surface.

struct Sphere {
	glm::vec3 center{};
	float radius = 1.0f;
	Colour colour{};
	Material material = Material::Diffuse;
	float roughness = 0.2f;
};

struct Plane {
	glm::vec3 point{};
	glm::vec3 normal{0.0f, 1.0f, 0.0f};
	Colour colour{};
	Material material = Material::Diffuse;
	float roughness = 0.2f;
};

struct Ellipsoid {
	glm::vec3 center{};
	glm::vec3 radii{1.0f, 1.0f, 1.0f};
	Colour colour{};
	Material material = Material::Diffuse;
	float roughness = 0.2f;
};

// Bounded circular plane: an infinite plane clipped to `radius` about `centre`.
struct Disk {
	glm::vec3 centre{};
	glm::vec3 normal{0.0f, 1.0f, 0.0f};
	float radius = 1.0f;
	Colour colour{};
	Material material = Material::Diffuse;
	float roughness = 0.2f;
};

// Finite cylinder about the y-axis (no end caps), spanning center.y +/- halfHeight.
struct Cylinder {
	glm::vec3 center{};
	float radius = 1.0f;
	float halfHeight = 1.0f;
	Colour colour{};
	Material material = Material::Diffuse;
	float roughness = 0.2f;
};

// Finite cone about the y-axis, apex at the top, opening downward over `height`
// to base radius `radius`.
struct Cone {
	glm::vec3 apex{};
	float radius = 1.0f;
	float height = 1.0f;
	Colour colour{};
	Material material = Material::Diffuse;
	float roughness = 0.2f;
};

// The analytic primitives in a scene, alongside the triangle meshes.
struct Primitives {
	std::vector<Sphere> spheres;
	std::vector<Plane> planes;
	std::vector<Disk> disks;
	std::vector<Ellipsoid> ellipsoids;
	std::vector<Cylinder> cylinders;
	std::vector<Cone> cones;
};

// A scene of triangle meshes (accelerated by a BVH) plus analytic primitives.
// Ray queries return the closest hit across all of them; a primitive hit is
// reported as a RayTriangleIntersection whose synthetic triangle carries the
// primitive's normal, colour, material and roughness (index SIZE_MAX so it
// never aliases a real triangle).
class Scene {
public:
	Scene(const std::vector<ModelTriangle> &triangles, const Primitives &primitives);

	RayTriangleIntersection intersect(const glm::vec3 &origin, const glm::vec3 &direction, int ignoreIndex = -1) const;
	bool occluded(const glm::vec3 &origin, const glm::vec3 &direction, float maxDistance, int ignoreIndex) const;

private:
	const Primitives &prims_;
	BVH bvh_;
};
