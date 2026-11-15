#include "Scene.h"

#include <cmath>
#include <limits>

Scene::Scene(const std::vector<ModelTriangle> &triangles, const Primitives &primitives)
    : prims_(primitives), bvh_(triangles) {}

static const float kInf = std::numeric_limits<float>::infinity();
static const float kEps = 1e-4f;

// --- ray/primitive intersection: each returns the nearest valid t, or +inf ---

static float sphereT(const glm::vec3 &o, const glm::vec3 &d, const Sphere &s) {
	glm::vec3 oc = o - s.center;
	float b = glm::dot(oc, d);
	float c = glm::dot(oc, oc) - s.radius * s.radius;
	float disc = b * b - c;
	if (disc < 0.0f)
		return kInf;
	float sq = std::sqrt(disc);
	float t = -b - sq;
	if (t <= kEps)
		t = -b + sq;
	return (t > kEps) ? t : kInf;
}

static float planeT(const glm::vec3 &o, const glm::vec3 &d, const Plane &p) {
	float denom = glm::dot(d, p.normal);
	if (std::abs(denom) < 1e-6f)
		return kInf;
	float t = glm::dot(p.point - o, p.normal) / denom;
	return (t > kEps) ? t : kInf;
}

static float diskT(const glm::vec3 &o, const glm::vec3 &d, const Disk &disk) {
	// Intersect the disk's supporting plane, then keep the hit only if it lies
	// within `radius` of the centre.
	float denom = glm::dot(d, disk.normal);
	if (std::abs(denom) < 1e-6f)
		return kInf;
	float t = glm::dot(disk.centre - o, disk.normal) / denom;
	if (t <= kEps)
		return kInf;
	glm::vec3 hit = o + t * d;
	glm::vec3 offset = hit - disk.centre;
	if (glm::dot(offset, offset) > disk.radius * disk.radius)
		return kInf;
	return t;
}

static float ellipsoidT(const glm::vec3 &o, const glm::vec3 &d, const Ellipsoid &e) {
	// Intersect the equivalent unit sphere in the ellipsoid's scaled space.
	glm::vec3 oc = (o - e.center) / e.radii;
	glm::vec3 rd = d / e.radii;
	float a = glm::dot(rd, rd);
	float b = 2.0f * glm::dot(oc, rd);
	float c = glm::dot(oc, oc) - 1.0f;
	float disc = b * b - 4.0f * a * c;
	if (disc < 0.0f)
		return kInf;
	float sq = std::sqrt(disc);
	float t = (-b - sq) / (2.0f * a);
	if (t <= kEps)
		t = (-b + sq) / (2.0f * a);
	return (t > kEps) ? t : kInf;
}

// Shared helper: nearest root of a t^2 + b t + c = 0 whose hit height
// (oy + t dy) lies within [loY, hiY]. Used by cylinder and cone.
static float cappedQuadratic(float a, float b, float c, float oy, float dy, float loY, float hiY) {
	if (std::abs(a) < 1e-9f)
		return kInf;
	float disc = b * b - 4.0f * a * c;
	if (disc < 0.0f)
		return kInf;
	float sq = std::sqrt(disc);
	float roots[2] = {(-b - sq) / (2.0f * a), (-b + sq) / (2.0f * a)};
	if (roots[0] > roots[1])
		std::swap(roots[0], roots[1]);
	for (float t : roots) {
		if (t <= kEps)
			continue;
		float y = oy + t * dy;
		if (y >= loY && y <= hiY)
			return t;
	}
	return kInf;
}

static float cylinderT(const glm::vec3 &o, const glm::vec3 &d, const Cylinder &cyl) {
	glm::vec3 oc = o - cyl.center;
	float a = d.x * d.x + d.z * d.z;
	float b = 2.0f * (oc.x * d.x + oc.z * d.z);
	float c = oc.x * oc.x + oc.z * oc.z - cyl.radius * cyl.radius;
	return cappedQuadratic(a, b, c, oc.y, d.y, -cyl.halfHeight, cyl.halfHeight);
}

static float coneT(const glm::vec3 &o, const glm::vec3 &d, const Cone &cone) {
	// x^2 + z^2 = k y^2 relative to the apex, opening downward over [-height, 0].
	glm::vec3 rel = o - cone.apex;
	float k = (cone.radius / cone.height) * (cone.radius / cone.height);
	float a = d.x * d.x + d.z * d.z - k * d.y * d.y;
	float b = 2.0f * (rel.x * d.x + rel.z * d.z - k * rel.y * d.y);
	float c = rel.x * rel.x + rel.z * rel.z - k * rel.y * rel.y;
	return cappedQuadratic(a, b, c, rel.y, d.y, -cone.height, 0.0f);
}

RayTriangleIntersection Scene::intersect(const glm::vec3 &origin, const glm::vec3 &direction, int ignoreIndex) const {
	RayTriangleIntersection closest = bvh_.intersect(origin, direction, ignoreIndex);

	// If a primitive is nearer than the current closest, overwrite the hit with a
	// synthetic triangle carrying its surface properties and geometric normal.
	auto consider = [&](float t, const glm::vec3 &normal, const Colour &colour, Material material, float roughness) {
		if (t >= closest.distanceFromCamera)
			return;
		glm::vec3 point = origin + t * direction;
		ModelTriangle tri;
		tri.colour = colour;
		tri.material = material;
		tri.roughness = roughness;
		tri.normal = normal;
		tri.vertexNormals = {normal, normal, normal};
		closest.hit = true;
		closest.distanceFromCamera = t;
		closest.intersectionPoint = point;
		closest.intersectedTriangle = tri;
		closest.triangleIndex = std::numeric_limits<size_t>::max();
		closest.u = 0.0f;
		closest.v = 0.0f;
	};

	for (const Sphere &s : prims_.spheres) {
		float t = sphereT(origin, direction, s);
		if (t < closest.distanceFromCamera)
			consider(t, glm::normalize(origin + t * direction - s.center), s.colour, s.material, s.roughness);
	}
	for (const Plane &p : prims_.planes) {
		float t = planeT(origin, direction, p);
		if (t < closest.distanceFromCamera)
			consider(t, glm::normalize(p.normal), p.colour, p.material, p.roughness);
	}
	for (const Disk &disk : prims_.disks) {
		float t = diskT(origin, direction, disk);
		if (t < closest.distanceFromCamera)
			consider(t, glm::normalize(disk.normal), disk.colour, disk.material, disk.roughness);
	}
	for (const Ellipsoid &e : prims_.ellipsoids) {
		float t = ellipsoidT(origin, direction, e);
		if (t < closest.distanceFromCamera) {
			glm::vec3 rel = origin + t * direction - e.center;
			consider(t, glm::normalize(rel / (e.radii * e.radii)), e.colour, e.material, e.roughness);
		}
	}
	for (const Cylinder &c : prims_.cylinders) {
		float t = cylinderT(origin, direction, c);
		if (t < closest.distanceFromCamera) {
			glm::vec3 h = origin + t * direction;
			consider(t, glm::normalize(glm::vec3(h.x - c.center.x, 0.0f, h.z - c.center.z)), c.colour, c.material,
			         c.roughness);
		}
	}
	for (const Cone &c : prims_.cones) {
		float t = coneT(origin, direction, c);
		if (t < closest.distanceFromCamera) {
			glm::vec3 rel = origin + t * direction - c.apex;
			float k = (c.radius / c.height) * (c.radius / c.height);
			consider(t, glm::normalize(glm::vec3(rel.x, -k * rel.y, rel.z)), c.colour, c.material, c.roughness);
		}
	}
	return closest;
}

bool Scene::occluded(const glm::vec3 &origin, const glm::vec3 &direction, float maxDistance, int ignoreIndex) const {
	if (bvh_.occluded(origin, direction, maxDistance, ignoreIndex))
		return true;
	for (const Sphere &s : prims_.spheres)
		if (sphereT(origin, direction, s) < maxDistance)
			return true;
	for (const Plane &p : prims_.planes)
		if (planeT(origin, direction, p) < maxDistance)
			return true;
	for (const Disk &disk : prims_.disks)
		if (diskT(origin, direction, disk) < maxDistance)
			return true;
	for (const Ellipsoid &e : prims_.ellipsoids)
		if (ellipsoidT(origin, direction, e) < maxDistance)
			return true;
	for (const Cylinder &c : prims_.cylinders)
		if (cylinderT(origin, direction, c) < maxDistance)
			return true;
	for (const Cone &c : prims_.cones)
		if (coneT(origin, direction, c) < maxDistance)
			return true;
	return false;
}
