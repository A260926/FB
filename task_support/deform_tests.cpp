// Regression coverage for in-place mesh refitting and versioned instance
// caches.
//
// Expected values come from an independent, explicit world-space triangle set
// built from raw transform math (scale, XYZ euler rotation, translation).
// Nothing here asks Mesh/MeshInstance for the expected values, so these checks
// catch a stale BVH, a stale per-instance cache, or a world-area shortcut that
// merely multiplies a local total by one constant.

#include "Defaults.hpp"
#include "Hittables/BVHNode.hpp"
#include "Hittables/Hittable.hpp"
#include "Hittables/Mesh.hpp"
#include "Hittables/MeshInstance.hpp"
#include "Hittables/Triangle.hpp"
#include "Materials/Emissive.hpp"
#include "Materials/Lambertian.hpp"
#include "Ray/Ray.hpp"
#include "Sampler.hpp"
#include "Vector3.hpp"
#include "check.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <thread>
#include <vector>

namespace
{
	constexpr double	PI = 3.14159265358979323846;
	const double	BOX_PAD = T_MIN;

	// ------------------------------------------------------------- raw math

	Vector3	rsub(const Vector3& a, const Vector3& b)
	{
		return (Vector3(a.getX() - b.getX(), a.getY() - b.getY(), a.getZ() - b.getZ()));
	}

	Vector3	radd(const Vector3& a, const Vector3& b)
	{
		return (Vector3(a.getX() + b.getX(), a.getY() + b.getY(), a.getZ() + b.getZ()));
	}

	Vector3	rscale(const Vector3& a, double f)
	{
		return (Vector3(a.getX() * f, a.getY() * f, a.getZ() * f));
	}

	Vector3	rcomponentwise(const Vector3& a, const Vector3& b)
	{
		return (Vector3(a.getX() * b.getX(), a.getY() * b.getY(), a.getZ() * b.getZ()));
	}

	Vector3	rcross(const Vector3& a, const Vector3& b)
	{
		return (Vector3(
			a.getY() * b.getZ() - a.getZ() * b.getY(),
			a.getZ() * b.getX() - a.getX() * b.getZ(),
			a.getX() * b.getY() - a.getY() * b.getX()
		));
	}

	double	rdot(const Vector3& a, const Vector3& b)
	{
		return (a.getX() * b.getX() + a.getY() * b.getY() + a.getZ() * b.getZ());
	}

	double	rlength(const Vector3& a)
	{
		return (std::sqrt(rdot(a, a)));
	}

	Vector3	rnormalize(const Vector3& a)
	{
		const double l = rlength(a);

		if (!std::isfinite(l) || l <= 0.0)
		{
			return (Vector3());
		}
		return (a / l);
	}

	bool	near(double actual, double expected, double tolerance)
	{
		return (std::isfinite(actual) && std::abs(actual - expected) <= tolerance);
	}

	bool	nearVector(const Vector3& actual, const Vector3& expected, double tolerance)
	{
		return (
			near(actual.getX(), expected.getX(), tolerance)
			&& near(actual.getY(), expected.getY(), tolerance)
			&& near(actual.getZ(), expected.getZ(), tolerance)
		);
	}

	// -------------------------------------------- explicit instance transform

	struct RefTransform
	{
		Vector3	position;
		Vector3	scale;
		double	cosX = 1.0;
		double	sinX = 0.0;
		double	cosY = 1.0;
		double	sinY = 0.0;
		double	cosZ = 1.0;
		double	sinZ = 0.0;

		RefTransform(Vector3 position, Vector3 rotationDegrees, Vector3 scale)
			: position(position), scale(scale)
		{
			const double x = rotationDegrees.getX() * PI / 180.0;
			const double y = rotationDegrees.getY() * PI / 180.0;
			const double z = rotationDegrees.getZ() * PI / 180.0;

			this->cosX = std::cos(x);
			this->sinX = std::sin(x);
			this->cosY = std::cos(y);
			this->sinY = std::sin(y);
			this->cosZ = std::cos(z);
			this->sinZ = std::sin(z);
		}

		// X rotation, then Y, then Z, matching the documented instance order.
		Vector3	rotate(Vector3 v) const
		{
			double x = v.getX();
			double y = v.getY();
			double z = v.getZ();

			double nextY = y * this->cosX - z * this->sinX;
			double nextZ = y * this->sinX + z * this->cosX;
			y = nextY;
			z = nextZ;

			double nextX = x * this->cosY + z * this->sinY;
			nextZ = -x * this->sinY + z * this->cosY;
			x = nextX;
			z = nextZ;

			nextX = x * this->cosZ - y * this->sinZ;
			nextY = x * this->sinZ + y * this->cosZ;

			return (Vector3(nextX, nextY, z));
		}

		Vector3	point(const Vector3& p) const
		{
			return (radd(this->rotate(rcomponentwise(p, this->scale)), this->position));
		}

		Vector3	normal(const Vector3& n) const
		{
			const Vector3 inverseScaled(
				n.getX() / this->scale.getX(),
				n.getY() / this->scale.getY(),
				n.getZ() / this->scale.getZ()
			);

			return (rnormalize(this->rotate(inverseScaled)));
		}
	};

	// ------------------------------------------------- explicit world triangles

	struct RefTriangle
	{
		Vector3	a;
		Vector3	b;
		Vector3	c;
		Vector3	faceNormal;
		double	area = 0.0;

		RefTriangle(const Vector3& a, const Vector3& b, const Vector3& c)
			: a(a), b(b), c(c)
		{
			const Vector3 n = rcross(rsub(b, a), rsub(c, a));
			const double l = rlength(n);

			this->area = l / 2.0;
			this->faceNormal = (l > 0.0) ? (n / l) : Vector3();
		}
	};

	std::vector<RefTriangle>	refWorldTriangles(
		const std::vector<Triangle>& triangles,
		const RefTransform& transform
	)
	{
		std::vector<RefTriangle> result;

		result.reserve(triangles.size());
		for (const Triangle& t : triangles)
		{
			result.push_back(RefTriangle(
				transform.point(t.getVertex0()),
				transform.point(t.getVertex1()),
				transform.point(t.getVertex2())
			));
		}
		return (result);
	}

	double	refTotalArea(const std::vector<RefTriangle>& triangles)
	{
		double total = 0.0;

		for (const RefTriangle& t : triangles)
		{
			total += t.area;
		}
		return (total);
	}

	// Tight local box over the triangles, padded exactly like Triangle does.
	std::array<double, 6>	refLocalBox(const std::vector<Triangle>& triangles)
	{
		std::array<double, 6> box = {
			INFINITY, INFINITY, INFINITY,
			-INFINITY, -INFINITY, -INFINITY
		};

		for (const Triangle& t : triangles)
		{
			const Vector3 corners[3] = { t.getVertex0(), t.getVertex1(), t.getVertex2() };

			for (const Vector3& corner : corners)
			{
				box[0] = std::min(box[0], corner.getX());
				box[1] = std::min(box[1], corner.getY());
				box[2] = std::min(box[2], corner.getZ());
				box[3] = std::max(box[3], corner.getX());
				box[4] = std::max(box[4], corner.getY());
				box[5] = std::max(box[5], corner.getZ());
			}
		}
		box[0] -= BOX_PAD;
		box[1] -= BOX_PAD;
		box[2] -= BOX_PAD;
		box[3] += BOX_PAD;
		box[4] += BOX_PAD;
		box[5] += BOX_PAD;
		return (box);
	}

	// Instance box: the eight corners of the padded local box, transformed.
	std::array<double, 6>	refInstanceBox(
		const std::vector<Triangle>& localTriangles,
		const RefTransform& transform
	)
	{
		const std::array<double, 6> local = refLocalBox(localTriangles);
		std::array<double, 6> box = {
			INFINITY, INFINITY, INFINITY,
			-INFINITY, -INFINITY, -INFINITY
		};

		for (int i = 0; i < 8; i++)
		{
			const double x = (i & 1) ? local[3] : local[0];
			const double y = (i & 2) ? local[4] : local[1];
			const double z = (i & 4) ? local[5] : local[2];
			const Vector3 world = transform.point(Vector3(x, y, z));

			box[0] = std::min(box[0], world.getX());
			box[1] = std::min(box[1], world.getY());
			box[2] = std::min(box[2], world.getZ());
			box[3] = std::max(box[3], world.getX());
			box[4] = std::max(box[4], world.getY());
			box[5] = std::max(box[5], world.getZ());
		}
		return (box);
	}

	bool	boxEnclosesPoint(const AABB& box, const Vector3& point, double tolerance)
	{
		return (
			point.getX() >= box.getMinimum().getX() - tolerance
			&& point.getY() >= box.getMinimum().getY() - tolerance
			&& point.getZ() >= box.getMinimum().getZ() - tolerance
			&& point.getX() <= box.getMaximum().getX() + tolerance
			&& point.getY() <= box.getMaximum().getY() + tolerance
			&& point.getZ() <= box.getMaximum().getZ() + tolerance
		);
	}

	// Moller-Trumbore. Direction is normalized first; returned distance is
	// the ray parameter along that normalized direction.
	bool	refRayTriangle(
		const RefTriangle& t,
		const Vector3& origin,
		const Vector3& direction,
		double tMin,
		double tMax,
		double& distance
	)
	{
		const Vector3 edge1 = rsub(t.b, t.a);
		const Vector3 edge2 = rsub(t.c, t.a);
		const double determinant = rdot(edge1, rcross(direction, edge2));

		if (std::fabs(determinant) < 1e-12)
		{
			return (false);
		}
		const double invDet = 1.0 / determinant;
		const Vector3 tvec = rsub(origin, t.a);
		const double u = rdot(tvec, rcross(direction, edge2)) * invDet;

		if (u < 0.0 || u > 1.0)
		{
			return (false);
		}
		const Vector3 q = rcross(tvec, edge1);
		const double v = rdot(direction, q) * invDet;

		if (v < 0.0 || u + v > 1.0)
		{
			return (false);
		}
		const double d = rdot(edge2, q) * invDet;

		if (d < tMin || d > tMax)
		{
			return (false);
		}
		distance = d;
		return (true);
	}

	bool	refClosestHit(
		const std::vector<RefTriangle>& triangles,
		const Vector3& origin,
		const Vector3& direction,
		double tMin,
		double tMax,
		double& distance,
		Vector3& normal
	)
	{
		const Vector3 dir = rnormalize(direction);
		bool found = false;

		distance = tMax;
		for (const RefTriangle& t : triangles)
		{
			double d = 0.0;

			if (!refRayTriangle(t, origin, dir, tMin, distance, d))
			{
				continue;
			}
			distance = d;
			normal = t.faceNormal;
			found = true;
		}
		return (found);
	}

	bool	refAnyHit(
		const std::vector<RefTriangle>& triangles,
		const Vector3& origin,
		const Vector3& direction,
		double tMin,
		double tMax
	)
	{
		const Vector3 dir = rnormalize(direction);
		double d = 0.0;

		for (const RefTriangle& t : triangles)
		{
			if (refRayTriangle(t, origin, dir, tMin, tMax, d))
			{
				return (true);
			}
		}
		return (false);
	}

	// Index of the world triangle containing a sampled point, or size().
	std::size_t	refTriangleAt(
		const std::vector<RefTriangle>& triangles,
		const Vector3& point,
		double tolerance
	)
	{
		for (std::size_t i = 0; i < triangles.size(); i++)
		{
			const RefTriangle& t = triangles[i];
			const Vector3 v0 = rsub(t.c, t.a);
			const Vector3 v1 = rsub(t.b, t.a);
			const Vector3 v2 = rsub(point, t.a);
			const double dot00 = rdot(v0, v0);
			const double dot01 = rdot(v0, v1);
			const double dot02 = rdot(v0, v2);
			const double dot11 = rdot(v1, v1);
			const double dot12 = rdot(v1, v2);
			const double denom = dot00 * dot11 - dot01 * dot01;

			if (std::fabs(denom) <= 1e-18)
			{
				continue;
			}
			const double u = (dot11 * dot02 - dot01 * dot12) / denom;
			const double v = (dot00 * dot12 - dot01 * dot02) / denom;

			if (u >= -tolerance && v >= -tolerance && u + v <= 1.0 + tolerance)
			{
				return (i);
			}
		}
		return (triangles.size());
	}

	// ------------------------------------------------------ mesh builders

	// Two flat, oppositely oriented, mutually non-occluding surfaces of nine
	// triangles each (a 3x3 of half-cells). Both are disjoint in the plane, so
	// a sampled point maps to exactly one reference triangle.
	std::vector<Triangle>	buildTwoSurfaceMesh(std::shared_ptr<Material> material)
	{
		std::vector<Triangle> triangles;

		triangles.reserve(18);
		for (int y = 0; y < 3; y++)
		{
			for (int x = 0; x < 3; x++)
			{
				const double px = static_cast<double>(x);
				const double py = static_cast<double>(y);

				triangles.emplace_back(
					Vector3(px, py, 0.0),
					Vector3(px + 1.0, py, 0.0),
					Vector3(px, py + 1.0, 0.0),
					material
				);
			}
		}
		for (int y = 0; y < 3; y++)
		{
			for (int x = 0; x < 3; x++)
			{
				const double px = static_cast<double>(x);
				const double py = static_cast<double>(y);

				triangles.emplace_back(
					Vector3(px, py, 4.0),
					Vector3(px, py + 1.0, 4.0),
					Vector3(px + 1.0, py, 4.0),
					material
				);
			}
		}
		return (triangles);
	}

	// Rigid motion: rotate about the triangle's anchor vertex, scale about the
	// anchor, then translate. Areas stay finite and positive.
	std::vector<Triangle>	deformMesh(
		const std::vector<Triangle>& base,
		double angleDegrees,
		double scaleFactor,
		Vector3 translation,
		std::shared_ptr<Material> material
	)
	{
		const double radians = angleDegrees * PI / 180.0;
		const double cs = std::cos(radians);
		const double sn = std::sin(radians);
		std::vector<Triangle> replacement;

		replacement.reserve(base.size());
		for (const Triangle& t : base)
		{
			const Vector3 anchor = t.getVertex0();
			auto map = [&](const Vector3& p) {
				const Vector3 d = rscale(rsub(p, anchor), scaleFactor);
				const Vector3 rotated(
					d.getX() * cs - d.getY() * sn,
					d.getX() * sn + d.getY() * cs,
					d.getZ()
				);
				return (radd(radd(anchor, rotated), translation));
			};

			replacement.emplace_back(
				map(t.getVertex0()),
				map(t.getVertex1()),
				map(t.getVertex2()),
				material
			);
		}
		return (replacement);
	}

	// Contract each triangle about its centroid by a per-slot factor (< 1)
	// and translate the whole set. Because contraction keeps every triangle
	// inside its original, disjoint footprint, the result remains disjoint and
	// areas differ per slot.
	std::vector<Triangle>	contractTriangles(
		const std::vector<Triangle>& base,
		const std::vector<double>& factors,
		Vector3 translation,
		std::shared_ptr<Material> material
	)
	{
		std::vector<Triangle> replacement;

		replacement.reserve(base.size());
		for (std::size_t i = 0; i < base.size(); i++)
		{
			const Triangle& t = base[i];
			const Vector3 centroid = rscale(
				radd(radd(t.getVertex0(), t.getVertex1()), t.getVertex2()),
				1.0 / 3.0
			);
			const double factor = factors[i % factors.size()];
			auto map = [&](const Vector3& p) {
				return (radd(radd(centroid, rscale(rsub(p, centroid), factor)), translation));
			};

			replacement.emplace_back(
				map(t.getVertex0()),
				map(t.getVertex1()),
				map(t.getVertex2()),
				material
			);
		}
		return (replacement);
	}

	// ------------------------------------------------- structure snapshots

	struct	NodeSnapshot
	{
		bool		isLeaf = false;
		std::size_t	left = 0;
		std::size_t	right = 0;
		std::size_t	start = 0;
		std::size_t	count = 0;
	};

	struct	MeshStructure
	{
		std::size_t			nodeCount = 0;
		std::vector<NodeSnapshot>	nodes;
		std::vector<std::size_t>	order;
	};

	MeshStructure	snapshotStructure(const Mesh& mesh)
	{
		MeshStructure snapshot;

		snapshot.nodeCount = mesh.packedNodeCount();
		snapshot.nodes.reserve(snapshot.nodeCount);
		for (std::size_t i = 0; i < snapshot.nodeCount; i++)
		{
			NodeSnapshot node;

			mesh.packedNodeLayout(i, node.isLeaf, node.left, node.right, node.start, node.count);
			snapshot.nodes.push_back(node);
		}
		snapshot.order = mesh.packedTriangleOrder();
		return (snapshot);
	}

	bool	structuresMatch(const MeshStructure& a, const MeshStructure& b)
	{
		if (a.nodeCount != b.nodeCount || a.nodes.size() != b.nodes.size() || a.order != b.order)
		{
			return (false);
		}
		for (std::size_t i = 0; i < a.nodes.size(); i++)
		{
			if (
				a.nodes[i].isLeaf != b.nodes[i].isLeaf
				|| a.nodes[i].left != b.nodes[i].left
				|| a.nodes[i].right != b.nodes[i].right
				|| a.nodes[i].start != b.nodes[i].start
				|| a.nodes[i].count != b.nodes[i].count
			)
			{
				return (false);
			}
		}
		return (true);
	}

	// Non-triangle legacy child so a BVHNode-backed mesh cannot be refitted.
	class	LegacyMarker : public Hittable
	{
		public:
			bool	hit(Ray& ray, HitRecord& hitRecord, double tMin, double tMax) const override
			{
				(void)ray;
				(void)hitRecord;
				(void)tMin;
				(void)tMax;
				return (false);
			}

			bool	createBoundingBox(AABB& outputBoundingBox) const override
			{
				outputBoundingBox = AABB(Vector3(-1.0, -1.0, -1.0), Vector3(1.0, 1.0, 1.0));
				return (true);
			}

			Material*	getMaterial(void) const override
			{
				return (nullptr);
			}
	};

	double	emissiveLuminance(const std::shared_ptr<Material>& material)
	{
		const Color emitted = material->emitted();

		return (
			emitted.getRed() * 0.2722287168
			+ emitted.getGreen() * 0.6740817658
			+ emitted.getBlue() * 0.0536895174
		);
	}
}

// ---------------------------------------------------------------- group refit

int	refitRejectsUnsupportedAndKeepsData(void)
{
	auto material = std::make_shared<Lambertian>(Color(0.7, 0.7, 0.7));
	std::vector<Triangle> base = buildTwoSurfaceMesh(material);
	Mesh mesh(Vector3(), material, base);

	TASK_CHECK(mesh.geometryRevision() == 0);
	TASK_CHECK(mesh.surfaceElementCount() == base.size());
	TASK_CHECK(near(mesh.surfaceArea(), 9.0, 1e-9));

	AABB boxBefore;
	double areaBefore = mesh.surfaceArea();
	MeshStructure structureBefore = snapshotStructure(mesh);

	TASK_CHECK(mesh.createBoundingBox(boxBefore));
	TASK_CHECK(structureBefore.nodeCount > 1);

	// Count mismatch is rejected without touching any data.
	std::vector<Triangle> shorter = base;

	shorter.pop_back();
	TASK_CHECK(!mesh.refitTriangles(shorter));
	TASK_CHECK(mesh.geometryRevision() == 0);
	TASK_CHECK(near(mesh.surfaceArea(), areaBefore, 1e-12));

	{
		AABB boxAfter;
		HitRecord hitRecord;
		Ray ray(Vector3(0.5, 0.5, -1.0), Vector3(0.0, 0.0, 1.0));

		TASK_CHECK(mesh.createBoundingBox(boxAfter));
		TASK_CHECK(nearVector(boxAfter.getMinimum(), boxBefore.getMinimum(), 1e-12));
		TASK_CHECK(nearVector(boxAfter.getMaximum(), boxBefore.getMaximum(), 1e-12));
		TASK_CHECK(mesh.hit(ray, hitRecord, 0.001, 100.0));
	}

	// Degenerate and non-finite entries are rejected at a matching count.
	std::vector<Triangle> degenerate = base;

	degenerate[0] = Triangle(
		Vector3(0.0, 0.0, 0.0),
		Vector3(1.0, 0.0, 0.0),
		Vector3(2.0, 0.0, 0.0),
		material
	);
	TASK_CHECK(!mesh.refitTriangles(degenerate));
	TASK_CHECK(mesh.geometryRevision() == 0);
	TASK_CHECK(near(mesh.surfaceArea(), areaBefore, 1e-12));

	std::vector<Triangle> nonFinite = base;

	nonFinite[1] = Triangle(
		Vector3(0.0, 0.0, 0.0),
		Vector3(INFINITY, 0.0, 0.0),
		Vector3(0.0, 1.0, 0.0),
		material
	);
	TASK_CHECK(!mesh.refitTriangles(nonFinite));
	TASK_CHECK(mesh.geometryRevision() == 0);
	TASK_CHECK(near(mesh.surfaceArea(), areaBefore, 1e-12));
	TASK_CHECK(structuresMatch(structureBefore, snapshotStructure(mesh)));

	// A legacy (BVHNode-backed) mesh is unsupported too.
	std::vector<std::shared_ptr<Hittable>> legacyChildren = {
		std::make_shared<LegacyMarker>()
	};
	Mesh legacyMesh(Vector3(), material, BVHNode(legacyChildren));

	TASK_CHECK(!legacyMesh.refitTriangles(base));
	TASK_CHECK(legacyMesh.geometryRevision() == 0);

	const Mesh* const legacyConst = &legacyMesh;

	TASK_CHECK(legacyConst->geometryRevision() == 0);
	return (0);
}

int	refitUpdatesBoundsAndKeepsStructure(void)
{
	auto material = std::make_shared<Lambertian>(Color(0.7, 0.7, 0.7));
	Mesh mesh(Vector3(), material, buildTwoSurfaceMesh(material));
	const MeshStructure structureBefore = snapshotStructure(mesh);

	TASK_CHECK(structureBefore.nodeCount > 1);
	bool hasInternal = false;

	for (const NodeSnapshot& node : structureBefore.nodes)
	{
		if (!node.isLeaf)
		{
			hasInternal = true;
		}
	}
	TASK_CHECK(hasInternal);

	// Warm all construction-only caches first.
	{
		AABB warmBox;
		HitRecord warmHit;
		Ray warmRay(Vector3(0.5, 0.5, -1.0), Vector3(0.0, 0.0, 1.0));

		TASK_CHECK(mesh.createBoundingBox(warmBox));
		TASK_CHECK(mesh.hit(warmRay, warmHit, 0.001, 100.0));
		TASK_CHECK(mesh.hitAny(warmRay, 0.001, 100.0));
		(void)mesh.pdfValue(warmRay.getOrigin(), warmRay.getDirection());
		(void)mesh.surfaceArea();
	}

	// Two consecutive refits with no query in between.
	std::vector<Triangle> step1 = deformMesh(
		buildTwoSurfaceMesh(material), 15.0, 1.3, Vector3(0.5, -1.0, 3.0), material
	);
	std::vector<Triangle> step2 = deformMesh(
		buildTwoSurfaceMesh(material), 40.0, 2.0, Vector3(20.0, 10.0, 50.0), material
	);

	TASK_CHECK(mesh.refitTriangles(step1));
	TASK_CHECK(mesh.geometryRevision() == 1);
	TASK_CHECK(mesh.refitTriangles(step2));
	TASK_CHECK(mesh.geometryRevision() == 2);

	// Only now query: the tree must reflect step2.
	const std::vector<RefTriangle> expected = refWorldTriangles(
		step2,
		RefTransform(Vector3(0.0, 0.0, 0.0), Vector3(0.0, 0.0, 0.0), Vector3(1.0, 1.0, 1.0))
	);
	const std::array<double, 6> expectedBox = refLocalBox(step2);

	TASK_CHECK(mesh.surfaceElementCount() == step2.size());
	TASK_CHECK(near(mesh.surfaceArea(), refTotalArea(expected), 1e-9));

	AABB box;
	TASK_CHECK(mesh.createBoundingBox(box));
	TASK_CHECK(near(box.getMinimum().getX(), expectedBox[0], 1e-9));
	TASK_CHECK(near(box.getMinimum().getY(), expectedBox[1], 1e-9));
	TASK_CHECK(near(box.getMinimum().getZ(), expectedBox[2], 1e-9));
	TASK_CHECK(near(box.getMaximum().getX(), expectedBox[3], 1e-9));
	TASK_CHECK(near(box.getMaximum().getY(), expectedBox[4], 1e-9));
	TASK_CHECK(near(box.getMaximum().getZ(), expectedBox[5], 1e-9));

	// Every stored node bound encloses the triangles it owns.
	const std::vector<std::size_t> order = mesh.packedTriangleOrder();

	TASK_CHECK(order.size() == step2.size());
	for (std::size_t i = 0; i < mesh.packedNodeCount(); i++)
	{
		bool isLeaf = false;
		std::size_t left = 0;
		std::size_t right = 0;
		std::size_t start = 0;
		std::size_t count = 0;
		AABB nodeBox;

		TASK_CHECK(mesh.packedNodeLayout(i, isLeaf, left, right, start, count));
		TASK_CHECK(mesh.packedNodeBoundingBox(i, nodeBox));
		if (!isLeaf)
		{
			continue;
		}
		for (std::size_t e = start; e < start + count; e++)
		{
			const RefTriangle& t = expected[order[e]];

			TASK_CHECK(boxEnclosesPoint(nodeBox, t.a, 1e-9));
			TASK_CHECK(boxEnclosesPoint(nodeBox, t.b, 1e-9));
			TASK_CHECK(boxEnclosesPoint(nodeBox, t.c, 1e-9));
		}
	}

	// Node connectivity, leaf ranges and triangle permutation are unchanged.
	TASK_CHECK(structuresMatch(structureBefore, snapshotStructure(mesh)));

	// The old hit/hitAny entry points, on the same tree, follow the new data.
	Ray newRay(Vector3(20.0, 10.0, 48.0), Vector3(0.0, 0.0, 1.0));
	HitRecord hitRecord;
	double expectedDistance = 0.0;
	Vector3 expectedNormal;

	TASK_CHECK(refClosestHit(expected, newRay.getOrigin(), newRay.getDirection(), 0.001, 100.0, expectedDistance, expectedNormal));
	TASK_CHECK(mesh.hit(newRay, hitRecord, 0.001, 100.0));
	TASK_CHECK(near(hitRecord.t0, expectedDistance, 1e-9));
	TASK_CHECK(nearVector(hitRecord.position, newRay.pointAtRay(expectedDistance), 1e-9));
	TASK_CHECK(mesh.hitAny(newRay, 0.001, 100.0));

	// The construction-space ray no longer hits anything.
	Ray staleRay(Vector3(0.5, 0.5, -1.0), Vector3(0.0, 0.0, 1.0));

	TASK_CHECK(!mesh.hit(staleRay, hitRecord, 0.001, 100.0));
	TASK_CHECK(!mesh.hitAny(staleRay, 0.001, 100.0));
	return (0);
}

// ------------------------------------------------------------ group instances

int	instancesRefreshIndependently(void)
{
	auto geometryMaterial = std::make_shared<Emissive>(Color(1.0, 1.0, 1.0));
	auto instanceMaterialA = std::make_shared<Emissive>(Color(0.9, 0.2, 0.2));
	auto instanceMaterialB = std::make_shared<Emissive>(Color(0.2, 0.4, 0.9));
	std::vector<Triangle> base = buildTwoSurfaceMesh(geometryMaterial);
	auto mesh = std::make_shared<Mesh>(Vector3(), geometryMaterial, base);

	// Different rotations and different non-uniform scales on one shared Mesh.
	const Vector3 posA(0.0, 0.0, 0.0);
	const Vector3 rotA(0.0, 0.0, 90.0);
	const Vector3 scaleA(2.0, 1.0, 1.0);
	const Vector3 posB(6.0, 0.0, 1.0);
	const Vector3 rotB(30.0, 0.0, 0.0);
	const Vector3 scaleB(1.0, 3.0, 0.5);
	const RefTransform transformA(posA, rotA, scaleA);
	const RefTransform transformB(posB, rotB, scaleB);

	MeshInstance instanceA(mesh, posA, rotA, scaleA, instanceMaterialA);
	MeshInstance instanceB(mesh, posB, rotB, scaleB, instanceMaterialB);

	TASK_CHECK(instanceA.getGeometry().get() == mesh.get());
	TASK_CHECK(instanceB.getGeometry().get() == mesh.get());
	TASK_CHECK(instanceA.getMaterial() == instanceMaterialA.get());
	TASK_CHECK(instanceB.getMaterial() == instanceMaterialB.get());
	TASK_CHECK(mesh->getMaterial() == geometryMaterial.get());

	const double luminance = emissiveLuminance(instanceMaterialA);

	// Pre-warm every cached entry point at the construction revision.
	std::vector<RefTriangle> worldA = refWorldTriangles(base, transformA);
	std::vector<RefTriangle> worldB = refWorldTriangles(base, transformB);
	const double areaA = refTotalArea(worldA);
	const double areaB = refTotalArea(worldB);

	{
		AABB boxA;
		AABB boxB;
		HittableLightSample sampleA;
		HittableEmissionSample emissionA;
		Ray ray(Vector3(0.0, 0.0, -20.0), Vector3(0.0, 0.0, 1.0));
		HitRecord hitRecord;

		TASK_CHECK(instanceA.createBoundingBox(boxA));
		TASK_CHECK(instanceB.createBoundingBox(boxB));
		TASK_CHECK(near(instanceA.lightSelectionWeight(), areaA * luminance, 1e-9));
		TASK_CHECK(near(instanceB.lightSelectionWeight(), areaB * emissiveLuminance(instanceMaterialB), 1e-9));
		TASK_CHECK(!near(instanceA.lightSelectionWeight(), instanceB.lightSelectionWeight(), 1e-6));
		TASK_CHECK(instanceA.sampleLight(Vector3(0.0, 0.0, -20.0), sampleA));
		TASK_CHECK(sampleA.valid);
		TASK_CHECK(refTriangleAt(worldA, sampleA.direction * 0.0 + sampleA.direction, 1e-6) <= worldA.size());
		TASK_CHECK(instanceA.sampleEmission(emissionA));
		TASK_CHECK(emissionA.valid);
		(void)instanceA.pdfValue(ray.getOrigin(), ray.getDirection());
		TASK_CHECK(instanceA.hit(ray, hitRecord, 0.001, 100.0));
		TASK_CHECK(hitRecord.material == instanceMaterialA.get());
	}
	TASK_CHECK(instanceA.geometryCacheBuildCount() == 1);
	TASK_CHECK(instanceB.geometryCacheBuildCount() == 1);

	// Revision 1: query right away.
	std::vector<Triangle> step1 = deformMesh(base, 25.0, 1.7, Vector3(1.0, 2.0, 5.0), geometryMaterial);

	TASK_CHECK(mesh->refitTriangles(step1));
	TASK_CHECK(mesh->geometryRevision() == 1);
	TASK_CHECK(instanceA.cachedGeometryRevision() == 0);

	const std::vector<RefTriangle> worldAStep1 = refWorldTriangles(step1, transformA);
	const std::vector<RefTriangle> worldBStep1 = refWorldTriangles(step1, transformB);
	const std::array<double, 6> expectedBoxA = refInstanceBox(step1, transformA);
	const std::array<double, 6> expectedBoxB = refInstanceBox(step1, transformB);
	const double luminanceA = emissiveLuminance(instanceMaterialA);
	const double luminanceB = emissiveLuminance(instanceMaterialB);

	{
		AABB boxA;
		AABB boxB;

		TASK_CHECK(instanceA.createBoundingBox(boxA));
		TASK_CHECK(instanceA.lightSelectionWeight() == instanceA.lightSelectionWeight());
		TASK_CHECK(instanceA.lightSelectionWeight() == instanceA.lightSelectionWeight());
		const std::uint64_t buildAfterRev1 = instanceA.geometryCacheBuildCount();

		// Repeat queries on the same revision must not rebuild the traversal.
		(void)instanceA.lightSelectionWeight();
		(void)instanceA.createBoundingBox(boxA);
		TASK_CHECK(instanceA.geometryCacheBuildCount() == buildAfterRev1);
		TASK_CHECK(buildAfterRev1 == 2);

		TASK_CHECK(near(boxA.getMinimum().getX(), expectedBoxA[0], 1e-9));
		TASK_CHECK(near(boxA.getMinimum().getY(), expectedBoxA[1], 1e-9));
		TASK_CHECK(near(boxA.getMinimum().getZ(), expectedBoxA[2], 1e-9));
		TASK_CHECK(near(boxA.getMaximum().getX(), expectedBoxA[3], 1e-9));
		TASK_CHECK(near(boxA.getMaximum().getY(), expectedBoxA[4], 1e-9));
		TASK_CHECK(near(boxA.getMaximum().getZ(), expectedBoxA[5], 1e-9));

		TASK_CHECK(instanceB.createBoundingBox(boxB));
		TASK_CHECK(near(boxB.getMinimum().getX(), expectedBoxB[0], 1e-9));
		TASK_CHECK(near(boxB.getMinimum().getY(), expectedBoxB[1], 1e-9));
		TASK_CHECK(near(boxB.getMinimum().getZ(), expectedBoxB[2], 1e-9));
		TASK_CHECK(near(boxB.getMaximum().getX(), expectedBoxB[3], 1e-9));
		TASK_CHECK(near(boxB.getMaximum().getY(), expectedBoxB[4], 1e-9));
		TASK_CHECK(near(boxB.getMaximum().getZ(), expectedBoxB[5], 1e-9));

		TASK_CHECK(near(instanceA.lightSelectionWeight(), refTotalArea(worldAStep1) * luminanceA, 1e-9));
		TASK_CHECK(near(instanceB.lightSelectionWeight(), refTotalArea(worldBStep1) * luminanceB, 1e-9));
		TASK_CHECK(!near(refTotalArea(worldAStep1), refTotalArea(worldBStep1), 1e-6));

		// Two instances of one mesh must not borrow each other's area prefix.
		HittableLightSample lightA;
		HittableLightSample lightB;

		Sampler::setRenderSeed(7001);
		Sampler::beginPixelSample(0, 0, 0);
		const bool sampledA = instanceA.sampleLight(Vector3(0.0, 0.0, -20.0), lightA);
		const bool sampledB = instanceB.sampleLight(Vector3(10.0, 0.0, -20.0), lightB);

		Sampler::endPixelSample();
		TASK_CHECK(sampledA && lightA.valid);
		TASK_CHECK(sampledB && lightB.valid);

		// Hit + PDF against the explicit world triangles. Aim at an interior
		// point (the centroid) so the ray does not graze a shared vertex edge.
		const RefTriangle& pick = worldAStep1[0];
		const Vector3 pickCentroid = rscale(radd(radd(pick.a, pick.b), pick.c), 1.0 / 3.0);
		const Vector3 origin = radd(pickCentroid, rscale(pick.faceNormal, -5.0));
		const Vector3 direction = pick.faceNormal;
		double distance = 0.0;
		Vector3 normal;

		TASK_CHECK(refClosestHit(worldAStep1, origin, direction, 0.001, 100.0, distance, normal));

		Ray ray(origin, direction);
		HitRecord hitRecord;

		TASK_CHECK(instanceA.hit(ray, hitRecord, 0.001, 100.0));
		TASK_CHECK(near(hitRecord.t0, distance, 1e-9));
		TASK_CHECK(hitRecord.material == instanceMaterialA.get());
		TASK_CHECK(mesh->getMaterial() == geometryMaterial.get());

		const double vecLength = rlength(direction);
		const double cosine = std::fabs(rdot(direction, normal)) / vecLength;
		const double expectedPdf = (distance * distance * vecLength * vecLength)
			/ (cosine * refTotalArea(worldAStep1));

		TASK_CHECK(near(instanceA.pdfValue(origin, direction), expectedPdf, 1e-9));
	}

	// Revisions 2 and 3 happen with no query in between: a skipped frame.
	std::vector<Triangle> step2 = deformMesh(base, 55.0, 0.8, Vector3(-8.0, -3.0, 12.0), geometryMaterial);
	std::vector<Triangle> step3 = deformMesh(base, 70.0, 1.1, Vector3(2.0, 4.0, -6.0), geometryMaterial);

	TASK_CHECK(mesh->refitTriangles(step2));
	TASK_CHECK(mesh->refitTriangles(step3));
	TASK_CHECK(mesh->geometryRevision() == 3);
	TASK_CHECK(instanceA.cachedGeometryRevision() == 1);
	TASK_CHECK(instanceB.cachedGeometryRevision() == 1);

	const std::vector<RefTriangle> worldAStep3 = refWorldTriangles(step3, transformA);
	const std::vector<RefTriangle> worldBStep3 = refWorldTriangles(step3, transformB);

	{
		AABB boxA;
		AABB boxB;

		TASK_CHECK(instanceA.createBoundingBox(boxA));
		TASK_CHECK(instanceB.createBoundingBox(boxB));
		TASK_CHECK(instanceA.cachedGeometryRevision() == 3);
		TASK_CHECK(instanceB.cachedGeometryRevision() == 3);
		TASK_CHECK(instanceA.geometryCacheBuildCount() == 3);
		TASK_CHECK(instanceB.geometryCacheBuildCount() == 3);
		TASK_CHECK(near(instanceA.lightSelectionWeight(), refTotalArea(worldAStep3) * luminanceA, 1e-9));
		TASK_CHECK(near(instanceB.lightSelectionWeight(), refTotalArea(worldBStep3) * luminanceB, 1e-9));

		const std::array<double, 6> expected = refInstanceBox(step3, transformA);

		TASK_CHECK(near(boxA.getMinimum().getX(), expected[0], 1e-9));
		TASK_CHECK(near(boxA.getMinimum().getY(), expected[1], 1e-9));
		TASK_CHECK(near(boxA.getMinimum().getZ(), expected[2], 1e-9));
		TASK_CHECK(near(boxA.getMaximum().getX(), expected[3], 1e-9));
		TASK_CHECK(near(boxA.getMaximum().getY(), expected[4], 1e-9));
		TASK_CHECK(near(boxA.getMaximum().getZ(), expected[5], 1e-9));

		// random() and sampleEmission() also observe the newest geometry.
		HittableEmissionSample emission;
		Vector3 randomDirection;

		Sampler::setRenderSeed(7002);
		Sampler::beginPixelSample(0, 0, 0);
		randomDirection = instanceA.random(Vector3(0.0, 0.0, -20.0));
		const bool emitted = instanceA.sampleEmission(emission);

		Sampler::endPixelSample();
		TASK_CHECK(
			std::isfinite(randomDirection.getX())
			&& std::isfinite(randomDirection.getY())
			&& std::isfinite(randomDirection.getZ())
		);
		TASK_CHECK(emitted && emission.valid);
		TASK_CHECK(boxEnclosesPoint(boxA, emission.position, 2e-3));
		TASK_CHECK(refTriangleAt(worldAStep3, emission.position, 1e-5) < worldAStep3.size());
	}
	return (0);
}

// ------------------------------------------------------------- group sampling

int	samplingFollowsRefittedDistribution(void)
{
	auto geometryMaterial = std::make_shared<Emissive>(Color(1.0, 1.0, 1.0));
	auto instanceMaterial = std::make_shared<Emissive>(Color(0.6, 0.6, 0.6));
	std::vector<Triangle> base = buildTwoSurfaceMesh(geometryMaterial);
	auto mesh = std::make_shared<Mesh>(Vector3(), geometryMaterial, base);
	const Vector3 position(2.0, 1.0, 0.0);
	const Vector3 rotation(0.0, 0.0, 35.0);
	const Vector3 scale(2.5, 0.75, 1.5);
	const RefTransform transform(position, rotation, scale);
	MeshInstance instance(mesh, position, rotation, scale, instanceMaterial);

	// Pre-warm sampling caches at the construction revision.
	Sampler::setRenderSeed(9101);
	for (int i = 0; i < 32; i++)
	{
		HittableLightSample sample;

		Sampler::beginPixelSample(0, 0, static_cast<std::uint32_t>(i));
		(void)instance.sampleLight(Vector3(0.0, 0.0, -40.0), sample);
		Sampler::endPixelSample();
	}
	TASK_CHECK(instance.geometryCacheBuildCount() == 1);

	// Refit to a deliberately uneven area distribution. Contracting about each
	// centroid keeps the triangles disjoint while giving slots different areas.
	const std::vector<double> factors = { 0.35, 0.9 };
	std::vector<Triangle> deformed = contractTriangles(
		deformMesh(base, 20.0, 1.0, Vector3(3.0, -2.0, 6.0), geometryMaterial),
		factors,
		Vector3(0.0, 0.0, 0.0),
		geometryMaterial
	);

	TASK_CHECK(mesh->refitTriangles(deformed));
	TASK_CHECK(mesh->geometryRevision() == 1);

	const std::vector<RefTriangle> world = refWorldTriangles(deformed, transform);
	const double totalArea = refTotalArea(world);
	const double luminance = emissiveLuminance(instanceMaterial);

	TASK_CHECK(near(instance.lightSelectionWeight(), totalArea * luminance, 1e-9));
	TASK_CHECK(instance.geometryCacheBuildCount() == 2);

	// Fixed-seed point sampling frequency tracks area_i / totalArea.
	const std::size_t sampleCount = 2400;
	std::vector<std::size_t> observed(world.size(), 0);
	std::size_t accepted = 0;

	Sampler::setRenderSeed(4242);
	for (std::size_t i = 0; i < sampleCount; i++)
	{
		Sampler::beginPixelSample(0, 0, static_cast<std::uint32_t>(i));
		HittableLightSample sample;
		const bool ok = instance.sampleLight(Vector3(0.0, 0.0, -40.0), sample);

		Sampler::endPixelSample();
		if (!ok || !sample.valid)
		{
			continue;
		}

		// Accepted points must land on the refitted world geometry, never on
		// the construction positions. sampleLight reports a direction + tMax,
		// so reconstruct the surface point the light emitted from.
		const Vector3 origin(0.0, 0.0, -40.0);
		const Vector3 samplePoint = radd(origin, rscale(sample.direction, sample.tMax));
		const std::size_t which = refTriangleAt(world, samplePoint, 1e-6);

		if (which >= world.size())
		{
			return (1);
		}
		observed[which]++;
		accepted++;
	}
	TASK_CHECK(accepted > sampleCount * 3 / 4);

	for (std::size_t i = 0; i < world.size(); i++)
	{
		const double expectedShare = world[i].area / totalArea;
		const double observedShare = static_cast<double>(observed[i]) / static_cast<double>(accepted);

		TASK_CHECK(near(observedShare, expectedShare, 0.02));
	}
	// Repeated queries at the same revision do not rebuild the distribution.
	(void)instance.lightSelectionWeight();
	TASK_CHECK(instance.geometryCacheBuildCount() == 2);
	return (0);
}

// ----------------------------------------------------------- group visibility

int	visibilityRefitMovesHitsAndOcclusion(void)
{
	auto material = std::make_shared<Lambertian>(Color(0.7, 0.7, 0.7));
	auto instanceMaterial = std::make_shared<Lambertian>(Color(0.4, 0.4, 0.4));
	std::vector<Triangle> base = buildTwoSurfaceMesh(material);
	auto mesh = std::make_shared<Mesh>(Vector3(), material, base);

	// Near instance (z=0 surface plane) and far instance (z=10) share one Mesh.
	const Vector3 nearPos(0.0, 0.0, 0.0);
	const Vector3 farPos(0.0, 0.0, 10.0);
	const Vector3 rotation(0.0, 0.0, 0.0);
	const Vector3 scale(1.0, 1.0, 1.0);
	const RefTransform transformNear(nearPos, rotation, scale);
	const RefTransform transformFar(farPos, rotation, scale);
	auto nearInstance = std::make_shared<MeshInstance>(mesh, nearPos, rotation, scale, instanceMaterial);
	auto farInstance = std::make_shared<MeshInstance>(mesh, farPos, rotation, scale, instanceMaterial);
	BVHNode worldTree(std::vector<std::shared_ptr<Hittable>>{ nearInstance, farInstance });

	// Warm both the instances and the top-level tree.
	{
		Ray ray(Vector3(0.5, 0.5, -20.0), Vector3(0.0, 0.0, 1.0));
		HitRecord hitRecord;
		AABB box;

		TASK_CHECK(worldTree.hit(ray, hitRecord, 0.001, 100.0));
		TASK_CHECK(worldTree.hitAny(ray, 0.001, 100.0));
		TASK_CHECK(worldTree.createBoundingBox(box));
		TASK_CHECK(near(hitRecord.t0, 20.0, 1e-6));

		const Color warmTransmittance = worldTree.shadowTransmittance(ray, 0.001, 100.0);

		TASK_CHECK(
			warmTransmittance.getRed() == 0.0
			&& warmTransmittance.getGreen() == 0.0
			&& warmTransmittance.getBlue() == 0.0
		);
	}

	// Deform the shared mesh far away; the two instances keep their transforms.
	std::vector<Triangle> moved = deformMesh(base, 0.0, 1.0, Vector3(500.0, 300.0, 120.0), material);

	TASK_CHECK(mesh->refitTriangles(moved));
	TASK_CHECK(mesh->geometryRevision() == 1);

	// Top-level refit: existing subtree and leaf objects, refreshed bounds.
	TASK_CHECK(worldTree.refit());

	const std::vector<RefTriangle> worldNear = refWorldTriangles(moved, transformNear);
	const std::vector<RefTriangle> worldFar = refWorldTriangles(moved, transformFar);
	const std::array<double, 6> expectedNear = refInstanceBox(moved, transformNear);
	const std::array<double, 6> expectedFar = refInstanceBox(moved, transformFar);
	std::array<double, 6> treeBox = {
		INFINITY, INFINITY, INFINITY, -INFINITY, -INFINITY, -INFINITY
	};

	for (int i = 0; i < 6; i++)
	{
		if (i < 3)
		{
			treeBox[i] = std::min(expectedNear[i], expectedFar[i]);
		}
		else
		{
			treeBox[i] = std::max(expectedNear[i], expectedFar[i]);
		}
	}
	{
		AABB box;

		TASK_CHECK(worldTree.createBoundingBox(box));
		TASK_CHECK(near(box.getMinimum().getX(), treeBox[0], 1e-9));
		TASK_CHECK(near(box.getMinimum().getY(), treeBox[1], 1e-9));
		TASK_CHECK(near(box.getMinimum().getZ(), treeBox[2], 1e-9));
		TASK_CHECK(near(box.getMaximum().getX(), treeBox[3], 1e-9));
		TASK_CHECK(near(box.getMaximum().getY(), treeBox[4], 1e-9));
		TASK_CHECK(near(box.getMaximum().getZ(), treeBox[5], 1e-9));
	}

	// New hits outside the old box, through the top-level tree.
	{
		Ray ray(Vector3(500.5, 300.5, 100.0), Vector3(0.0, 0.0, 1.0));
		HitRecord hitRecord;
		double nearest = 0.0;
		Vector3 normal;

		// Near instance plane is at z=120; far at 130. Both are hit by this ray.
		TASK_CHECK(refClosestHit(worldNear, ray.getOrigin(), ray.getDirection(), 0.001, 100.0, nearest, normal));
		TASK_CHECK(worldTree.hit(ray, hitRecord, 0.001, 100.0));
		TASK_CHECK(near(hitRecord.t0, nearest, 1e-9));
		TASK_CHECK(hitRecord.material == instanceMaterial.get());
		TASK_CHECK(worldTree.hitAny(ray, 0.001, 100.0));
		TASK_CHECK(refAnyHit(worldFar, ray.getOrigin(), ray.getDirection(), 0.001, 100.0));
	}

	// Old position yields no hit at any of the entry points.
	{
		Ray staleRay(Vector3(0.5, 0.5, -20.0), Vector3(0.0, 0.0, 1.0));
		HitRecord hitRecord;
		const Color transmittance = worldTree.shadowTransmittance(staleRay, 0.001, 100.0);

		TASK_CHECK(!worldTree.hit(staleRay, hitRecord, 0.001, 100.0));
		TASK_CHECK(!worldTree.hitAny(staleRay, 0.001, 100.0));
		TASK_CHECK(!refAnyHit(worldNear, staleRay.getOrigin(), staleRay.getDirection(), 0.001, 100.0));
		TASK_CHECK(transmittance.getRed() == 1.0 && transmittance.getGreen() == 1.0 && transmittance.getBlue() == 1.0);
	}

	// Occlusion: a segment that ends before the far plane is blocked by the
	// near plane after refit.
	{
		Ray blocked(Vector3(500.5, 300.5, 100.0), Vector3(0.0, 0.0, 1.0));
		const Color transmittance = worldTree.shadowTransmittance(blocked, 0.001, 100.0);

		TASK_CHECK(transmittance.getRed() == 0.0 && transmittance.getGreen() == 0.0 && transmittance.getBlue() == 0.0);
	}

	// A ray grazing the old, empty path stays unoccluded.
	return (0);
}

// Top-level refit over a BVHNode that actually splits into child subtrees:
// more than eight leaves forces real internal nodes, so the recursive refit
// path (parent bounds derived from refreshed child nodes) is exercised too.
int	visibilityRecursiveTreeRefit(void)
{
	auto material = std::make_shared<Lambertian>(Color(0.7, 0.7, 0.7));
	auto instanceMaterial = std::make_shared<Lambertian>(Color(0.3, 0.3, 0.3));
	std::vector<Triangle> base = buildTwoSurfaceMesh(material);
	auto mesh = std::make_shared<Mesh>(Vector3(), material, base);

	// Twelve instances spread along +x. BVHNode's leaf threshold is 8, so the
	// root becomes a parent with two BVHNode children.
	std::vector<std::shared_ptr<Hittable>> leaves;
	std::vector<RefTransform> transforms;
	const std::size_t instanceCount = 12;

	for (std::size_t i = 0; i < instanceCount; i++)
	{
		const Vector3 position(15.0 * static_cast<double>(i), 0.0, 0.0);

		transforms.push_back(RefTransform(position, Vector3(0.0, 0.0, 0.0), Vector3(1.0, 1.0, 1.0)));
		leaves.push_back(std::make_shared<MeshInstance>(
			mesh,
			position,
			Vector3(0.0, 0.0, 0.0),
			Vector3(1.0, 1.0, 1.0),
			instanceMaterial
		));
	}
	BVHNode worldTree(leaves);

	// The root has internal children, not just leaves.
	TASK_CHECK(leaves.size() > 8);
	{
		AABB warmBox;
		Ray warmRay(Vector3(0.5, 0.5, -20.0), Vector3(0.0, 0.0, 1.0));
		HitRecord hitRecord;

		TASK_CHECK(worldTree.createBoundingBox(warmBox));
		TASK_CHECK(worldTree.hit(warmRay, hitRecord, 0.001, 100.0));
		(void)worldTree.hitAny(warmRay, 0.001, 100.0);
	}

	// Move the geometry to a whole new region and refit the split tree.
	std::vector<Triangle> moved = deformMesh(base, 0.0, 1.0, Vector3(0.0, 250.0, 330.0), material);

	TASK_CHECK(mesh->refitTriangles(moved));
	TASK_CHECK(mesh->geometryRevision() == 1);
	TASK_CHECK(worldTree.refit());

	std::array<double, 6> expected = {
		INFINITY, INFINITY, INFINITY, -INFINITY, -INFINITY, -INFINITY
	};
	std::vector<std::vector<RefTriangle>> worldInstances;

	for (std::size_t i = 0; i < instanceCount; i++)
	{
		const std::vector<RefTriangle> world = refWorldTriangles(moved, transforms[i]);
		const std::array<double, 6> box = refInstanceBox(moved, transforms[i]);

		worldInstances.push_back(world);
		expected[0] = std::min(expected[0], box[0]);
		expected[1] = std::min(expected[1], box[1]);
		expected[2] = std::min(expected[2], box[2]);
		expected[3] = std::max(expected[3], box[3]);
		expected[4] = std::max(expected[4], box[4]);
		expected[5] = std::max(expected[5], box[5]);
	}

	{
		AABB box;

		TASK_CHECK(worldTree.createBoundingBox(box));
		TASK_CHECK(near(box.getMinimum().getX(), expected[0], 1e-9));
		TASK_CHECK(near(box.getMinimum().getY(), expected[1], 1e-9));
		TASK_CHECK(near(box.getMinimum().getZ(), expected[2], 1e-9));
		TASK_CHECK(near(box.getMaximum().getX(), expected[3], 1e-9));
		TASK_CHECK(near(box.getMaximum().getY(), expected[4], 1e-9));
		TASK_CHECK(near(box.getMaximum().getZ(), expected[5], 1e-9));
	}

	// Hits land on the moved instances, chosen by the fresh child bounds.
	// The translation moved the surfaces to y ~ 250..253 and z ~ 330..334.
	{
		Ray ray(Vector3(0.5, 251.5, 310.0), Vector3(0.0, 0.0, 1.0));
		HitRecord hitRecord;

		TASK_CHECK(worldTree.hit(ray, hitRecord, 0.001, 100.0));
		TASK_CHECK(near(hitRecord.t0, 20.0, 1e-6));
		TASK_CHECK(worldTree.hitAny(ray, 0.001, 100.0));
		TASK_CHECK(refAnyHit(worldInstances[0], ray.getOrigin(), ray.getDirection(), 0.001, 100.0));
	}

	// The old layout no longer produces any hit anywhere in the split tree.
	{
		Ray staleRay(Vector3(0.5, 0.5, -20.0), Vector3(0.0, 0.0, 1.0));
		HitRecord hitRecord;

		TASK_CHECK(!worldTree.hit(staleRay, hitRecord, 0.001, 100.0));
		TASK_CHECK(!worldTree.hitAny(staleRay, 0.001, 100.0));
	}
	return (0);
}

int	visibilityMultithreadedFirstReadOfNewRevision(void)
{
	auto geometryMaterial = std::make_shared<Emissive>(Color(1.0, 1.0, 1.0));
	auto instanceMaterial = std::make_shared<Emissive>(Color(0.5, 0.5, 0.5));
	std::vector<Triangle> base = buildTwoSurfaceMesh(geometryMaterial);
	auto mesh = std::make_shared<Mesh>(Vector3(), geometryMaterial, base);
	const Vector3 position(1.0, 2.0, 3.0);
	const Vector3 rotation(10.0, 20.0, 30.0);
	const Vector3 scale(1.5, 0.5, 2.0);
	const RefTransform transform(position, rotation, scale);
	auto instance = std::make_shared<MeshInstance>(mesh, position, rotation, scale, instanceMaterial);

	// Warm revision 0.
	{
		AABB box;

		TASK_CHECK(instance->createBoundingBox(box));
		(void)instance->lightSelectionWeight();
	}
	TASK_CHECK(instance->geometryCacheBuildCount() == 1);

	std::vector<Triangle> deformed = deformMesh(base, 45.0, 1.2, Vector3(4.0, -1.0, 7.0), geometryMaterial);

	TASK_CHECK(mesh->refitTriangles(deformed));
	TASK_CHECK(mesh->geometryRevision() == 1);

	const std::vector<RefTriangle> world = refWorldTriangles(deformed, transform);
	const double expectedArea = refTotalArea(world);
	const std::array<double, 6> expectedBox = refInstanceBox(deformed, transform);
	const double luminance = emissiveLuminance(instanceMaterial);

	// Many threads race the first read of revision 1.
	const std::size_t threadCount = 8;
	const std::size_t readsPerThread = 32;
	std::vector<std::thread> threads;
	std::atomic<unsigned> failures{ 0 };
	std::vector<std::array<double, 6>> boxes(threadCount);

	for (std::size_t i = 0; i < threadCount; i++)
	{
		threads.emplace_back([&, i]() {
			for (std::size_t r = 0; r < readsPerThread; r++)
			{
				AABB box;

				if (!instance->createBoundingBox(box))
				{
					failures.fetch_add(1);
					return;
				}
				boxes[i] = {
					box.getMinimum().getX(), box.getMinimum().getY(), box.getMinimum().getZ(),
					box.getMaximum().getX(), box.getMaximum().getY(), box.getMaximum().getZ()
				};

				const double weight = instance->lightSelectionWeight();

				if (!near(weight, expectedArea * luminance, 1e-9))
				{
					failures.fetch_add(1);
					return;
				}
				HitRecord hitRecord;
				Ray ray(Vector3(4.0, -1.0, -20.0), Vector3(0.0, 3.0, 20.0));

				if (instance->hit(ray, hitRecord, 0.001, 100.0))
				{
					double distance = 0.0;
					Vector3 normal;

					if (
						!refClosestHit(world, ray.getOrigin(), ray.getDirection(), 0.001, 100.0, distance, normal)
						|| !near(hitRecord.t0, distance, 1e-9)
					)
					{
						failures.fetch_add(1);
						return;
					}
				}
			}
		});
	}
	for (auto& thread : threads)
	{
		thread.join();
	}
	TASK_CHECK(failures.load() == 0);

	for (std::size_t i = 0; i < threadCount; i++)
	{
		TASK_CHECK(nearVector(
			Vector3(boxes[i][0], boxes[i][1], boxes[i][2]),
			Vector3(expectedBox[0], expectedBox[1], expectedBox[2]),
			1e-9
		));
		TASK_CHECK(nearVector(
			Vector3(boxes[i][3], boxes[i][4], boxes[i][5]),
			Vector3(expectedBox[3], expectedBox[4], expectedBox[5]),
			1e-9
		));
	}

	// Exactly one additional build for revision 1 despite the racing first reads.
	TASK_CHECK(instance->cachedGeometryRevision() == 1);
	TASK_CHECK(instance->geometryCacheBuildCount() == 2);
	return (0);
}

extern const struct task_case task_cases[] = {
	{ "refit", "rejects_unsupported_and_keeps_data", &refitRejectsUnsupportedAndKeepsData },
	{ "refit", "updates_bounds_and_keeps_structure", &refitUpdatesBoundsAndKeepsStructure },
	{ "instances", "refresh_independently", &instancesRefreshIndependently },
	{ "sampling", "follows_refitted_distribution", &samplingFollowsRefittedDistribution },
	{ "visibility", "refit_moves_hits_and_occlusion", &visibilityRefitMovesHitsAndOcclusion },
	{ "visibility", "recursive_tree_refit", &visibilityRecursiveTreeRefit },
	{ "visibility", "multithreaded_first_read_of_new_revision", &visibilityMultithreadedFirstReadOfNewRevision }
};

extern const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
