#include "check.h"

#include "AABB.hpp"
#include "Defaults.hpp"
#include "Hittables/BVHNode.hpp"
#include "Hittables/Mesh.hpp"
#include "Hittables/MeshInstance.hpp"
#include "Hittables/Triangle.hpp"
#include "Materials/Emissive.hpp"
#include "Sampler.hpp"
#include "Utilities.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <thread>
#include <vector>

namespace
{
	struct	ExplicitTransform
	{
		Vector3	position;
		Vector3	rotationDegrees;
		Vector3	scale;
	};

	struct	ExplicitTriangle
	{
		Vector3	vertex0;
		Vector3	vertex1;
		Vector3	vertex2;
	};

	bool	nearValue(double actual, double expected, double tolerance)
	{
		return (std::isfinite(actual) && std::fabs(actual - expected) <= tolerance);
	}

	bool	nearVector(const Vector3& actual, const Vector3& expected, double tolerance)
	{
		return (
			nearValue(actual.getX(), expected.getX(), tolerance)
			&& nearValue(actual.getY(), expected.getY(), tolerance)
			&& nearValue(actual.getZ(), expected.getZ(), tolerance)
		);
	}

	// Explicit reference implementation of the documented instance transform order.
	Vector3	explicitRotate(const Vector3& rotationDegrees, Vector3 vector)
	{
		const double toRadians = 3.14159265358979323846 / 180.0;
		const double cosX = std::cos(rotationDegrees.getX() * toRadians);
		const double sinX = std::sin(rotationDegrees.getX() * toRadians);
		const double cosY = std::cos(rotationDegrees.getY() * toRadians);
		const double sinY = std::sin(rotationDegrees.getY() * toRadians);
		const double cosZ = std::cos(rotationDegrees.getZ() * toRadians);
		const double sinZ = std::sin(rotationDegrees.getZ() * toRadians);
		double x = vector.getX();
		double y = vector.getY();
		double z = vector.getZ();

		double nextY = (y * cosX) - (z * sinX);
		double nextZ = (y * sinX) + (z * cosX);
		y = nextY;
		z = nextZ;

		double nextX = (x * cosY) + (z * sinY);
		nextZ = (-x * sinY) + (z * cosY);
		x = nextX;
		z = nextZ;

		nextX = (x * cosZ) - (y * sinZ);
		nextY = (x * sinZ) + (y * cosZ);

		return (Vector3(nextX, nextY, z));
	}

	Vector3	explicitWorldPoint(const ExplicitTransform& transform, const Vector3& point)
	{
		const Vector3 scaled(
			point.getX() * transform.scale.getX(),
			point.getY() * transform.scale.getY(),
			point.getZ() * transform.scale.getZ()
		);

		return (explicitRotate(transform.rotationDegrees, scaled) + transform.position);
	}

	ExplicitTriangle	explicitWorldTriangle(const ExplicitTransform& transform, const Triangle& triangle)
	{
		return (ExplicitTriangle{
			explicitWorldPoint(transform, triangle.getVertex0()),
			explicitWorldPoint(transform, triangle.getVertex1()),
			explicitWorldPoint(transform, triangle.getVertex2())
		});
	}

	ExplicitTriangle	explicitLocalTriangle(const Triangle& triangle)
	{
		return (ExplicitTriangle{
			triangle.getVertex0(),
			triangle.getVertex1(),
			triangle.getVertex2()
		});
	}

	double	explicitTriangleArea(const ExplicitTriangle& triangle)
	{
		const Vector3 edge1 = triangle.vertex1 - triangle.vertex0;
		const Vector3 edge2 = triangle.vertex2 - triangle.vertex0;

		return (Utilities::vectorLength(Utilities::cross(edge1, edge2)) / 2.0);
	}

	AABB	explicitTriangleBox(const Triangle& triangle)
	{
		const Vector3 vertices[3] = {
			triangle.getVertex0(),
			triangle.getVertex1(),
			triangle.getVertex2()
		};
		Vector3 minimum = vertices[0];
		Vector3 maximum = vertices[0];

		for (const Vector3& vertex : vertices)
		{
			minimum = Vector3(
				std::min(minimum.getX(), vertex.getX()),
				std::min(minimum.getY(), vertex.getY()),
				std::min(minimum.getZ(), vertex.getZ())
			);
			maximum = Vector3(
				std::max(maximum.getX(), vertex.getX()),
				std::max(maximum.getY(), vertex.getY()),
				std::max(maximum.getZ(), vertex.getZ())
			);
		}

		return (AABB(
			Vector3(minimum.getX() - T_MIN, minimum.getY() - T_MIN, minimum.getZ() - T_MIN),
			Vector3(maximum.getX() + T_MIN, maximum.getY() + T_MIN, maximum.getZ() + T_MIN)
		));
	}

	AABB	mergeExplicitBoxes(const AABB& first, const AABB& second)
	{
		return (AABB(
			Vector3(
				std::min(first.getMinimum().getX(), second.getMinimum().getX()),
				std::min(first.getMinimum().getY(), second.getMinimum().getY()),
				std::min(first.getMinimum().getZ(), second.getMinimum().getZ())
			),
			Vector3(
				std::max(first.getMaximum().getX(), second.getMaximum().getX()),
				std::max(first.getMaximum().getY(), second.getMaximum().getY()),
				std::max(first.getMaximum().getZ(), second.getMaximum().getZ())
			)
		));
	}

	AABB	explicitLocalBoundingBox(const std::vector<Triangle>& triangles)
	{
		AABB result;
		bool hasBox = false;

		for (const Triangle& triangle : triangles)
		{
			const AABB box = explicitTriangleBox(triangle);

			result = hasBox ? mergeExplicitBoxes(result, box) : box;
			hasBox = true;
		}
		return (result);
	}

	AABB	explicitInstanceBoundingBox(const ExplicitTransform& transform, const std::vector<Triangle>& triangles)
	{
		const AABB localBox = explicitLocalBoundingBox(triangles);
		const Vector3& localMinimum = localBox.getMinimum();
		const Vector3& localMaximum = localBox.getMaximum();
		const Vector3 corners[8] = {
			Vector3(localMinimum.getX(), localMinimum.getY(), localMinimum.getZ()),
			Vector3(localMaximum.getX(), localMinimum.getY(), localMinimum.getZ()),
			Vector3(localMinimum.getX(), localMaximum.getY(), localMinimum.getZ()),
			Vector3(localMaximum.getX(), localMaximum.getY(), localMinimum.getZ()),
			Vector3(localMinimum.getX(), localMinimum.getY(), localMaximum.getZ()),
			Vector3(localMaximum.getX(), localMinimum.getY(), localMaximum.getZ()),
			Vector3(localMinimum.getX(), localMaximum.getY(), localMaximum.getZ()),
			Vector3(localMaximum.getX(), localMaximum.getY(), localMaximum.getZ())
		};
		Vector3 minimum = explicitWorldPoint(transform, corners[0]);
		Vector3 maximum = minimum;

		for (const Vector3& corner : corners)
		{
			const Vector3 worldCorner = explicitWorldPoint(transform, corner);

			minimum = Vector3(
				std::min(minimum.getX(), worldCorner.getX()),
				std::min(minimum.getY(), worldCorner.getY()),
				std::min(minimum.getZ(), worldCorner.getZ())
			);
			maximum = Vector3(
				std::max(maximum.getX(), worldCorner.getX()),
				std::max(maximum.getY(), worldCorner.getY()),
				std::max(maximum.getZ(), worldCorner.getZ())
			);
		}
		return (AABB(minimum, maximum));
	}

	bool	boxNear(const AABB& actual, const AABB& expected, double tolerance)
	{
		return (
			nearVector(actual.getMinimum(), expected.getMinimum(), tolerance)
			&& nearVector(actual.getMaximum(), expected.getMaximum(), tolerance)
		);
	}

	bool	pointInsideTriangle(const Vector3& point, const ExplicitTriangle& triangle, double tolerance)
	{
		const Vector3 edge0 = triangle.vertex1 - triangle.vertex0;
		const Vector3 edge1 = triangle.vertex2 - triangle.vertex0;
		const Vector3 relative = point - triangle.vertex0;
		const double dot00 = Utilities::dot(edge0, edge0);
		const double dot01 = Utilities::dot(edge0, edge1);
		const double dot02 = Utilities::dot(edge0, relative);
		const double dot11 = Utilities::dot(edge1, edge1);
		const double dot12 = Utilities::dot(edge1, relative);
		const double denominator = (dot00 * dot11) - (dot01 * dot01);

		if (std::fabs(denominator) <= 1e-18)
		{
			return (false);
		}
		const double u = ((dot11 * dot02) - (dot01 * dot12)) / denominator;
		const double v = ((dot00 * dot12) - (dot01 * dot02)) / denominator;

		return (u >= -tolerance && v >= -tolerance && (u + v) <= (1.0 + tolerance));
	}

	std::vector<Triangle>	buildTwoTriangles(const std::shared_ptr<Material>& material)
	{
		std::vector<Triangle> triangles;

		triangles.emplace_back(
			Vector3(0.0, 0.0, 0.0),
			Vector3(1.0, 0.0, 0.0),
			Vector3(0.0, 1.0, 0.0),
			material
		);
		triangles.emplace_back(
			Vector3(3.0, 0.0, 0.0),
			Vector3(3.0, 0.0, 1.0),
			Vector3(3.0, 1.0, 0.0),
			material
		);
		return (triangles);
	}

	// A non-trivial grid so the packed tree contains internal nodes and several leaves.
	std::vector<Triangle>	buildGridTriangles(const std::shared_ptr<Material>& material)
	{
		std::vector<Triangle> triangles;

		for (int y = 0; y < 3; y++)
		{
			for (int x = 0; x < 5; x++)
			{
				const double baseX = static_cast<double>(x);
				const double baseY = static_cast<double>(y);

				triangles.emplace_back(
					Vector3(baseX, baseY, 0.0),
					Vector3(baseX + 1.0, baseY, 0.0),
					Vector3(baseX, baseY + 1.0, 0.0),
					material
				);
			}
		}
		return (triangles);
	}

	bool	layoutTopologyMatches(
		const Mesh::PackedGeometryLayout& before,
		const Mesh::PackedGeometryLayout& after
	)
	{
		return (
			before.packed == after.packed
			&& before.triangleCount == after.triangleCount
			&& before.nodeCount == after.nodeCount
			&& before.left == after.left
			&& before.right == after.right
			&& before.start == after.start
			&& before.count == after.count
			&& before.isLeaf == after.isLeaf
			&& before.triangleIndices == after.triangleIndices
		);
	}

	bool	layoutBoundsChanged(
		const Mesh::PackedGeometryLayout& before,
		const Mesh::PackedGeometryLayout& after
	)
	{
		if (before.minimum.size() != after.minimum.size() || before.minimum.empty())
		{
			return (false);
		}
		for (std::size_t index = 0; index < before.minimum.size(); index++)
		{
			if (!nearVector(before.minimum[index], after.minimum[index], 1e-12))
			{
				return (true);
			}
			if (!nearVector(before.maximum[index], after.maximum[index], 1e-12))
			{
				return (true);
			}
		}
		return (false);
	}

	AABB	subtreeBox(
		const Mesh::PackedGeometryLayout& layout,
		const std::vector<AABB>& triangleBoxes,
		std::size_t nodeIndex
	)
	{
		if (layout.isLeaf[nodeIndex] != 0)
		{
			AABB result;
			bool hasBox = false;

			for (std::size_t offset = 0; offset < layout.count[nodeIndex]; offset++)
			{
				const std::size_t triangleIndex = layout.triangleIndices[layout.start[nodeIndex] + offset];
				const AABB& box = triangleBoxes[triangleIndex];

				result = hasBox ? mergeExplicitBoxes(result, box) : box;
				hasBox = true;
			}
			return (result);
		}

		return (mergeExplicitBoxes(
			subtreeBox(layout, triangleBoxes, layout.left[nodeIndex]),
			subtreeBox(layout, triangleBoxes, layout.right[nodeIndex])
		));
	}

	bool	layoutBoundsMatchTriangles(
		const Mesh::PackedGeometryLayout& layout,
		const std::vector<Triangle>& triangles,
		double tolerance
	)
	{
		std::vector<AABB> triangleBoxes;

		triangleBoxes.reserve(triangles.size());
		for (const Triangle& triangle : triangles)
		{
			triangleBoxes.push_back(explicitTriangleBox(triangle));
		}
		for (std::size_t nodeIndex = 0; nodeIndex < layout.nodeCount; nodeIndex++)
		{
			const AABB expected = subtreeBox(layout, triangleBoxes, nodeIndex);
			const AABB actual(layout.minimum[nodeIndex], layout.maximum[nodeIndex]);

			if (!boxNear(actual, expected, tolerance))
			{
				return (false);
			}
		}
		return (true);
	}

	double	luminanceOf(const std::shared_ptr<Emissive>& material)
	{
		return (Utilities::luminance(material->emitted()));
	}
}

/*
	Group: refit
*/

static int	refitUpdatesBoundsAndHits(void)
{
	auto material = std::make_shared<Emissive>(Color(1.0, 1.0, 1.0));
	auto mesh = std::make_shared<Mesh>(Vector3(), material, buildTwoTriangles(material));

	TASK_CHECK(mesh->geometryRevision() == 0);

	const Mesh::PackedGeometryLayout layoutBefore = mesh->packedGeometryLayout();
	TASK_CHECK(layoutBefore.packed);
	TASK_CHECK(layoutBefore.triangleCount == 2);
	TASK_CHECK(layoutBefore.nodeCount >= 1);
	TASK_CHECK(layoutBefore.triangleIndices.size() == 2);

	const std::vector<Triangle> initialTriangles = buildTwoTriangles(material);
	const AABB expectedInitialBox = explicitLocalBoundingBox(initialTriangles);
	AABB initialBox;
	TASK_CHECK(mesh->createBoundingBox(initialBox));
	TASK_CHECK(boxNear(initialBox, expectedInitialBox, 1e-9));

	Ray oldProbe(Vector3(0.25, 0.25, 5.0), Vector3(0.0, 0.0, -1.0));
	HitRecord oldHit;
	TASK_CHECK(mesh->hit(oldProbe, oldHit, T_MIN, T_MAX));
	TASK_CHECK(nearValue(oldHit.t0, 5.0, 1e-9));
	TASK_CHECK(mesh->hitAny(oldProbe, T_MIN, T_MAX));

	std::vector<Triangle> deformed;
	deformed.emplace_back(
		Vector3(10.0, 0.0, 0.0),
		Vector3(11.0, 0.0, 0.0),
		Vector3(10.0, 1.0, 0.0),
		material
	);
	deformed.emplace_back(
		Vector3(20.0, 0.0, 0.0),
		Vector3(20.0, 0.0, 1.0),
		Vector3(20.0, 1.0, 0.0),
		material
	);
	TASK_CHECK(mesh->refitTriangles(deformed));
	TASK_CHECK(mesh->geometryRevision() == 1);

	AABB deformedBox;
	TASK_CHECK(mesh->createBoundingBox(deformedBox));
	TASK_CHECK(boxNear(deformedBox, explicitLocalBoundingBox(deformed), 1e-9));

	Ray newAProbe(Vector3(10.25, 0.25, 5.0), Vector3(0.0, 0.0, -1.0));
	HitRecord newAHit;
	TASK_CHECK(mesh->hit(newAProbe, newAHit, T_MIN, T_MAX));
	TASK_CHECK(nearValue(newAHit.t0, 5.0, 1e-9));
	TASK_CHECK(mesh->hitAny(newAProbe, T_MIN, T_MAX));

	Ray newBProbe(Vector3(30.0, 0.25, 0.25), Vector3(-1.0, 0.0, 0.0));
	HitRecord newBHit;
	TASK_CHECK(mesh->hit(newBProbe, newBHit, T_MIN, T_MAX));
	TASK_CHECK(nearValue(newBHit.t0, 10.0, 1e-9));
	TASK_CHECK(mesh->hitAny(newBProbe, T_MIN, T_MAX));

	// The old location must no longer report a hit through the original tree.
	Ray staleProbe(Vector3(0.25, 0.25, 5.0), Vector3(0.0, 0.0, -1.0));
	HitRecord staleHit;
	TASK_CHECK(!mesh->hit(staleProbe, staleHit, T_MIN, T_MAX));
	TASK_CHECK(!mesh->hitAny(staleProbe, T_MIN, T_MAX));

	const Mesh::PackedGeometryLayout layoutAfter = mesh->packedGeometryLayout();
	TASK_CHECK(layoutTopologyMatches(layoutBefore, layoutAfter));
	TASK_CHECK(layoutBoundsChanged(layoutBefore, layoutAfter));
	TASK_CHECK(layoutBoundsMatchTriangles(layoutAfter, deformed, 1e-9));

	TASK_CHECK(nearValue(mesh->surfaceArea(), 1.0, 1e-9));

	// Direct mesh hits expose the mesh's own material, which survives geometry replacement.
	HitRecord meshHit;
	Ray meshProbe(Vector3(10.25, 0.25, 5.0), Vector3(0.0, 0.0, -1.0));
	TASK_CHECK(mesh->hit(meshProbe, meshHit, T_MIN, T_MAX));
	TASK_CHECK(meshHit.material == material.get());
	TASK_CHECK(mesh->getMaterial() == material.get());

	// The mesh's own material is never taken from the replacement triangles.
	auto otherMaterial = std::make_shared<Emissive>(Color(0.0, 1.0, 0.0));
	std::vector<Triangle> withOtherMaterial = deformed;
	withOtherMaterial[1] = Triangle(
		deformed[1].getVertex0(),
		deformed[1].getVertex1(),
		deformed[1].getVertex2(),
		otherMaterial
	);
	TASK_CHECK(mesh->refitTriangles(withOtherMaterial));
	TASK_CHECK(mesh->geometryRevision() == 2);
	TASK_CHECK(mesh->getMaterial() == material.get());
	return (0);
}

static int	refitUpdatesInternalNodeBounds(void)
{
	auto material = std::make_shared<Emissive>(Color(1.0, 1.0, 1.0));
	auto mesh = std::make_shared<Mesh>(Vector3(), material, buildGridTriangles(material));

	const Mesh::PackedGeometryLayout layoutBefore = mesh->packedGeometryLayout();
	TASK_CHECK(layoutBefore.packed);
	TASK_CHECK(layoutBefore.triangleCount == 15);
	TASK_CHECK(layoutBefore.nodeCount > 1);
	TASK_CHECK(layoutBefore.triangleIndices.size() == 15);
	TASK_CHECK(layoutBoundsMatchTriangles(layoutBefore, buildGridTriangles(material), 1e-9));

	// Bend the grid: lift the center and shift the last column, keeping every triangle valid.
	std::vector<Triangle> deformed = buildGridTriangles(material);
	for (std::size_t index = 0; index < deformed.size(); index++)
	{
		const Vector3 v0 = deformed[index].getVertex0();
		const Vector3 v1 = deformed[index].getVertex1();
		const Vector3 v2 = deformed[index].getVertex2();
		// Whole triangles in the last two columns are translated, leaving a real gap behind
		// while the remaining triangles are slanted in Z so internal node bounds change too.
		const double shift = v0.getX() >= 3.0 ? 7.0 : 0.0;
		auto warp = [shift](Vector3 vertex) {
			const double lift = 2.0 * std::sin(vertex.getX() * 1.3) + 0.5 * vertex.getY();

			return (Vector3(vertex.getX() + shift, vertex.getY() + 0.25, vertex.getZ() - lift));
		};

		deformed[index] = Triangle(warp(v0), warp(v1), warp(v2), material);
	}
	TASK_CHECK(mesh->refitTriangles(deformed));
	TASK_CHECK(mesh->geometryRevision() == 1);

	const Mesh::PackedGeometryLayout layoutAfter = mesh->packedGeometryLayout();
	TASK_CHECK(layoutTopologyMatches(layoutBefore, layoutAfter));
	TASK_CHECK(layoutBoundsChanged(layoutBefore, layoutAfter));
	TASK_CHECK(layoutBoundsMatchTriangles(layoutAfter, deformed, 1e-9));

	AABB meshBox;
	TASK_CHECK(mesh->createBoundingBox(meshBox));
	TASK_CHECK(boxNear(meshBox, explicitLocalBoundingBox(deformed), 1e-9));

	// A ray through a lifted triangle must hit the refit tree; the flat position must not.
	const Triangle& moved = deformed[7];
	const Vector3 centroid = (moved.getVertex0() + moved.getVertex1() + moved.getVertex2()) / 3.0;
	Ray liftedProbe(Vector3(centroid.getX(), centroid.getY(), 5.0), Vector3(0.0, 0.0, -1.0));
	HitRecord liftedHit;
	TASK_CHECK(mesh->hit(liftedProbe, liftedHit, T_MIN, T_MAX));
	TASK_CHECK(nearValue(liftedHit.position.getZ(), centroid.getZ(), 1e-9));

	// The last column is translated by +7 in X, so its former footprint is now empty.
	Ray vacatedProbe(Vector3(4.5, 0.75, 5.0), Vector3(0.0, 0.0, -1.0));
	HitRecord vacatedHit;
	TASK_CHECK(!mesh->hit(vacatedProbe, vacatedHit, T_MIN, T_MAX));
	TASK_CHECK(!mesh->hitAny(vacatedProbe, T_MIN, T_MAX));

	Ray relocatedProbe(Vector3(11.5, 0.75, 5.0), Vector3(0.0, 0.0, -1.0));
	HitRecord relocatedHit;
	TASK_CHECK(mesh->hit(relocatedProbe, relocatedHit, T_MIN, T_MAX));
	TASK_CHECK(mesh->hitAny(relocatedProbe, T_MIN, T_MAX));
	return (0);
}

static int	refitRejectsUnsupportedAndInvalidUpdates(void)
{
	auto material = std::make_shared<Emissive>(Color(1.0, 1.0, 1.0));
	auto mesh = std::make_shared<Mesh>(Vector3(), material, buildTwoTriangles(material));

	const Mesh::PackedGeometryLayout layoutBefore = mesh->packedGeometryLayout();
	AABB beforeBox;
	Ray probe(Vector3(0.25, 0.25, 5.0), Vector3(0.0, 0.0, -1.0));
	HitRecord beforeHit;
	TASK_CHECK(mesh->createBoundingBox(beforeBox));
	TASK_CHECK(mesh->hit(probe, beforeHit, T_MIN, T_MAX));

	std::vector<Triangle> wrongCount;
	wrongCount.emplace_back(
		Vector3(10.0, 0.0, 0.0),
		Vector3(11.0, 0.0, 0.0),
		Vector3(10.0, 1.0, 0.0),
		material
	);
	TASK_CHECK(!mesh->refitTriangles(wrongCount));
	TASK_CHECK(mesh->geometryRevision() == 0);

	std::vector<Triangle> degenerate = buildTwoTriangles(material);
	degenerate[1] = Triangle(
		Vector3(3.0, 0.0, 0.0),
		Vector3(3.0, 0.0, 1.0),
		Vector3(3.0, 0.0, 2.0),
		material
	);
	TASK_CHECK(!mesh->refitTriangles(degenerate));
	TASK_CHECK(mesh->geometryRevision() == 0);

	std::vector<Triangle> nonFinite = buildTwoTriangles(material);
	nonFinite[0] = Triangle(
		Vector3(0.0, 0.0, 0.0),
		Vector3(std::numeric_limits<double>::quiet_NaN(), 0.0, 0.0),
		Vector3(0.0, 1.0, 0.0),
		material
	);
	TASK_CHECK(!mesh->refitTriangles(nonFinite));
	TASK_CHECK(mesh->geometryRevision() == 0);

	AABB afterRejectedBox;
	HitRecord afterRejectedHit;
	Ray unchangedProbe(Vector3(0.25, 0.25, 5.0), Vector3(0.0, 0.0, -1.0));
	TASK_CHECK(mesh->createBoundingBox(afterRejectedBox));
	TASK_CHECK(boxNear(afterRejectedBox, beforeBox, 1e-12));
	TASK_CHECK(mesh->hit(unchangedProbe, afterRejectedHit, T_MIN, T_MAX));
	TASK_CHECK(nearValue(afterRejectedHit.t0, 5.0, 1e-9));
	TASK_CHECK(layoutTopologyMatches(layoutBefore, mesh->packedGeometryLayout()));

	std::vector<std::shared_ptr<Hittable>> legacyTriangles;
	legacyTriangles.push_back(std::make_shared<Triangle>(
		Vector3(0.0, 0.0, 0.0),
		Vector3(1.0, 0.0, 0.0),
		Vector3(0.0, 1.0, 0.0),
		material
	));
	Mesh legacyMesh(Vector3(), material, BVHNode(legacyTriangles));
	std::vector<Triangle> replacement;
	replacement.emplace_back(
		Vector3(0.0, 0.0, 0.0),
		Vector3(1.0, 0.0, 0.0),
		Vector3(0.0, 1.0, 0.0),
		material
	);
	TASK_CHECK(!legacyMesh.refitTriangles(replacement));
	TASK_CHECK(legacyMesh.geometryRevision() == 0);

	std::vector<Triangle> valid = buildTwoTriangles(material);
	valid[0] = Triangle(
		Vector3(0.0, 0.0, 0.0),
		Vector3(2.0, 0.0, 0.0),
		Vector3(0.0, 1.0, 0.0),
		material
	);
	TASK_CHECK(mesh->refitTriangles(valid));
	TASK_CHECK(mesh->geometryRevision() == 1);
	return (0);
}

static int	repeatedRefitsWithSkippedQueries(void)
{
	auto material = std::make_shared<Emissive>(Color(1.0, 1.0, 1.0));
	auto mesh = std::make_shared<Mesh>(Vector3(), material, buildTwoTriangles(material));

	for (int step = 1; step <= 3; step++)
	{
		const double offset = 10.0 * step;
		std::vector<Triangle> deformed;
		deformed.emplace_back(
			Vector3(offset, 0.0, 0.0),
			Vector3(offset + 1.0, 0.0, 0.0),
			Vector3(offset, 1.0, 0.0),
			material
		);
		deformed.emplace_back(
			Vector3(offset + 10.0, 0.0, 0.0),
			Vector3(offset + 10.0, 0.0, 1.0),
			Vector3(offset + 10.0, 1.0, 0.0),
			material
		);
		TASK_CHECK(mesh->refitTriangles(deformed));
		TASK_CHECK(mesh->geometryRevision() == static_cast<std::uint64_t>(step));

		AABB stepBox;
		TASK_CHECK(mesh->createBoundingBox(stepBox));
		TASK_CHECK(nearValue(stepBox.getMinimum().getX(), offset - T_MIN, 1e-9));
		TASK_CHECK(nearValue(stepBox.getMaximum().getX(), offset + 10.0 + T_MIN, 1e-9));
	}

	Ray finalProbe(Vector3(30.25, 0.25, 5.0), Vector3(0.0, 0.0, -1.0));
	HitRecord finalHit;
	TASK_CHECK(mesh->hit(finalProbe, finalHit, T_MIN, T_MAX));
	TASK_CHECK(nearValue(finalHit.t0, 5.0, 1e-9));
	return (0);
}

/*
	Group: instances
*/

static int	sharedGeometryRefreshesBothInstances(void)
{
	auto geometryMaterial = std::make_shared<Emissive>(Color(1.0, 1.0, 1.0));
	auto instanceMaterial = std::make_shared<Emissive>(Color(0.5, 0.5, 0.5));
	auto mesh = std::make_shared<Mesh>(Vector3(), geometryMaterial, buildTwoTriangles(geometryMaterial));

	const ExplicitTransform transformA{
		Vector3(0.0, 0.0, 0.0),
		Vector3(0.0, 0.0, 0.0),
		Vector3(2.0, 1.0, 1.0)
	};
	const ExplicitTransform transformB{
		Vector3(5.0, 1.0, -2.0),
		Vector3(0.0, 0.0, 90.0),
		Vector3(1.0, 3.0, 2.0)
	};
	auto instanceA = std::make_shared<MeshInstance>(
		mesh,
		transformA.position,
		transformA.rotationDegrees,
		transformA.scale,
		instanceMaterial
	);
	auto instanceB = std::make_shared<MeshInstance>(
		mesh,
		transformB.position,
		transformB.rotationDegrees,
		transformB.scale,
		instanceMaterial
	);

	TASK_CHECK(instanceA->getGeometry().get() == instanceB->getGeometry().get());

	const std::vector<Triangle> initialTriangles = buildTwoTriangles(geometryMaterial);
	const AABB expectedInitialBoxA = explicitInstanceBoundingBox(transformA, initialTriangles);
	const AABB expectedInitialBoxB = explicitInstanceBoundingBox(transformB, initialTriangles);
	const double initialAreaA =
		explicitTriangleArea(explicitWorldTriangle(transformA, initialTriangles[0]))
		+ explicitTriangleArea(explicitWorldTriangle(transformA, initialTriangles[1]));
	const double initialAreaB =
		explicitTriangleArea(explicitWorldTriangle(transformB, initialTriangles[0]))
		+ explicitTriangleArea(explicitWorldTriangle(transformB, initialTriangles[1]));
	TASK_CHECK(!nearValue(initialAreaA, initialAreaB, 1e-3));

	// Prewarm every cached entry point on the initial geometry.
	AABB warmBoxA;
	AABB warmBoxB;
	TASK_CHECK(instanceA->createBoundingBox(warmBoxA));
	TASK_CHECK(instanceB->createBoundingBox(warmBoxB));
	TASK_CHECK(boxNear(warmBoxA, expectedInitialBoxA, 1e-9));
	TASK_CHECK(boxNear(warmBoxB, expectedInitialBoxB, 1e-9));

	const Vector3 originA = explicitWorldPoint(transformA, Vector3(0.5, 0.25, 0.0)) + Vector3(0.0, 0.0, 5.0);
	const Vector3 originB = explicitWorldPoint(transformB, Vector3(0.5, 0.25, 0.0)) + Vector3(0.0, 0.0, 5.0);
	const Vector3 direction(0.0, 0.0, -1.0);

	TASK_CHECK(nearValue(instanceA->pdfValue(originA, direction), 25.0 / initialAreaA, 1e-6));
	TASK_CHECK(nearValue(instanceB->pdfValue(originB, direction), 25.0 / initialAreaB, 1e-6));

	HittableLightSample warmSampleA;
	HittableLightSample warmSampleB;
	TASK_CHECK(instanceA->sampleLight(originA, warmSampleA));
	TASK_CHECK(instanceB->sampleLight(originB, warmSampleB));
	TASK_CHECK(warmSampleA.valid && warmSampleB.valid);

	HittableEmissionSample warmEmissionA;
	HittableEmissionSample warmEmissionB;
	TASK_CHECK(instanceA->sampleEmission(warmEmissionA));
	TASK_CHECK(instanceB->sampleEmission(warmEmissionB));
	TASK_CHECK(warmEmissionA.valid && warmEmissionB.valid);

	const double luminance = luminanceOf(instanceMaterial);
	TASK_CHECK(nearValue(instanceA->lightSelectionWeight(), initialAreaA * luminance, 1e-6));
	TASK_CHECK(nearValue(instanceB->lightSelectionWeight(), initialAreaB * luminance, 1e-6));

	TASK_CHECK(Utilities::vectorLengthSquared(instanceA->random(originA)) > 0.0);
	TASK_CHECK(Utilities::vectorLengthSquared(instanceB->random(originB)) > 0.0);

	const Mesh::PackedGeometryLayout layoutBefore = mesh->packedGeometryLayout();

	std::vector<Triangle> deformed;
	deformed.emplace_back(
		Vector3(0.0, 0.0, -4.0),
		Vector3(4.0, 0.0, -4.0),
		Vector3(0.0, 3.0, -4.0),
		geometryMaterial
	);
	deformed.emplace_back(
		Vector3(6.0, 0.0, 0.0),
		Vector3(6.0, 0.0, 4.0),
		Vector3(6.0, 5.0, 0.0),
		geometryMaterial
	);
	TASK_CHECK(mesh->refitTriangles(deformed));
	TASK_CHECK(mesh->geometryRevision() == 1);

	const double deformedAreaA =
		explicitTriangleArea(explicitWorldTriangle(transformA, deformed[0]))
		+ explicitTriangleArea(explicitWorldTriangle(transformA, deformed[1]));
	const double deformedAreaB =
		explicitTriangleArea(explicitWorldTriangle(transformB, deformed[0]))
		+ explicitTriangleArea(explicitWorldTriangle(transformB, deformed[1]));
	const AABB expectedBoxA = explicitInstanceBoundingBox(transformA, deformed);
	const AABB expectedBoxB = explicitInstanceBoundingBox(transformB, deformed);
	TASK_CHECK(!nearValue(deformedAreaA, deformedAreaB, 1e-3));
	TASK_CHECK(!nearValue(deformedAreaA, initialAreaA * 4.0, 1e-3));

	AABB refreshedBoxA;
	AABB refreshedBoxB;
	TASK_CHECK(instanceA->createBoundingBox(refreshedBoxA));
	TASK_CHECK(instanceB->createBoundingBox(refreshedBoxB));
	TASK_CHECK(boxNear(refreshedBoxA, expectedBoxA, 1e-9));
	TASK_CHECK(boxNear(refreshedBoxB, expectedBoxB, 1e-9));
	TASK_CHECK(!boxNear(refreshedBoxA, warmBoxA, 1e-6));

	TASK_CHECK(nearValue(instanceA->lightSelectionWeight(), deformedAreaA * luminance, 1e-6));
	TASK_CHECK(nearValue(instanceB->lightSelectionWeight(), deformedAreaB * luminance, 1e-6));

	// Nested BVH nodes are refreshed bottom-up; leaf topology stays intact.
	BVHNode world(std::vector<std::shared_ptr<Hittable>>{instanceA, instanceB});
	TASK_CHECK(world.refit());
	AABB worldBox;
	TASK_CHECK(world.createBoundingBox(worldBox));
	TASK_CHECK(boxNear(
		worldBox,
		mergeExplicitBoxes(expectedBoxA, expectedBoxB),
		1e-9
	));
	TASK_CHECK(layoutTopologyMatches(layoutBefore, mesh->packedGeometryLayout()));

	const Vector3 probeA = explicitWorldPoint(transformA, Vector3(1.0, 1.0, -4.0)) + Vector3(0.0, 0.0, 20.0);
	HitRecord deformedHitA;
	Ray deformedRayA(probeA, direction);
	TASK_CHECK(instanceA->hit(deformedRayA, deformedHitA, T_MIN, T_MAX));
	TASK_CHECK(nearValue(
		deformedHitA.t0,
		probeA.getZ() - explicitWorldPoint(transformA, Vector3(1.0, 1.0, -4.0)).getZ(),
		1e-9
	));
	TASK_CHECK(nearValue(instanceA->pdfValue(probeA, direction), 400.0 / deformedAreaA, 1e-6));
	TASK_CHECK(deformedHitA.material == instanceMaterial.get());

	const Vector3 worldPointB = explicitWorldPoint(transformB, Vector3(1.0, 1.0, -4.0));
	const Vector3 probeB(worldPointB.getX(), worldPointB.getY(), 15.0);
	HitRecord deformedHitB;
	Ray deformedRayB(probeB, direction);
	TASK_CHECK(instanceB->hit(deformedRayB, deformedHitB, T_MIN, T_MAX));
	TASK_CHECK(nearValue(deformedHitB.t0, 15.0 - worldPointB.getZ(), 1e-9));
	TASK_CHECK(nearValue(instanceB->pdfValue(probeB, direction),
		(15.0 - worldPointB.getZ()) * (15.0 - worldPointB.getZ()) / deformedAreaB, 1e-6));

	// Post-refit light sampling and random directions must also use the new geometry.
	HittableLightSample deformedSampleA;
	TASK_CHECK(instanceA->sampleLight(originA, deformedSampleA));
	TASK_CHECK(deformedSampleA.valid && deformedSampleA.pdf > 0.0);
	TASK_CHECK(deformedSampleA.material == instanceMaterial.get());
	TASK_CHECK(nearValue(
		deformedSampleA.pdf,
		instanceA->pdfValue(originA, deformedSampleA.direction),
		1e-6
	));
	Ray sampleRayA(originA, deformedSampleA.direction);
	HitRecord sampleHitA;
	TASK_CHECK(instanceA->hit(sampleRayA, sampleHitA, T_MIN, T_MAX));
	TASK_CHECK(nearValue(sampleHitA.t0, deformedSampleA.tMax, 1e-7));

	HittableLightSample deformedSampleB;
	TASK_CHECK(instanceB->sampleLight(originB, deformedSampleB));
	TASK_CHECK(deformedSampleB.valid && deformedSampleB.pdf > 0.0);
	TASK_CHECK(nearValue(
		deformedSampleB.pdf,
		instanceB->pdfValue(originB, deformedSampleB.direction),
		1e-6
	));

	HittableEmissionSample deformedEmissionA;
	TASK_CHECK(instanceA->sampleEmission(deformedEmissionA));
	TASK_CHECK(deformedEmissionA.valid);
	TASK_CHECK(nearValue(deformedEmissionA.powerScale, 2.0 * 3.14159265358979323846 * deformedAreaA, 1e-6));
	const ExplicitTriangle worldA0 = explicitWorldTriangle(transformA, deformed[0]);
	const ExplicitTriangle worldA1 = explicitWorldTriangle(transformA, deformed[1]);
	TASK_CHECK(
		pointInsideTriangle(deformedEmissionA.position, worldA0, 1e-7)
		|| pointInsideTriangle(deformedEmissionA.position, worldA1, 1e-7)
	);

	HittableEmissionSample deformedEmissionB;
	TASK_CHECK(instanceB->sampleEmission(deformedEmissionB));
	TASK_CHECK(deformedEmissionB.valid);
	TASK_CHECK(nearValue(deformedEmissionB.powerScale, 2.0 * 3.14159265358979323846 * deformedAreaB, 1e-6));

	const Vector3 randomA = instanceA->random(originA);
	const Vector3 randomB = instanceB->random(originB);
	TASK_CHECK(Utilities::vectorLengthSquared(randomA) > 0.0);
	TASK_CHECK(Utilities::vectorLengthSquared(randomB) > 0.0);
	Ray randomRayA(originA, randomA);
	HitRecord randomHitA;
	TASK_CHECK(instanceA->hit(randomRayA, randomHitA, T_MIN, T_MAX));
	Ray randomRayB(originB, randomB);
	HitRecord randomHitB;
	TASK_CHECK(instanceB->hit(randomRayB, randomHitB, T_MIN, T_MAX));

	// Instance material override survives geometry replacement.
	TASK_CHECK(instanceA->getMaterial() == instanceMaterial.get());
	TASK_CHECK(instanceB->getMaterial() == instanceMaterial.get());
	TASK_CHECK(mesh->getMaterial() == geometryMaterial.get());
	return (0);
}

static int	instanceSkipsFramesAndRepeatsRevision(void)
{
	auto material = std::make_shared<Emissive>(Color(1.0, 1.0, 1.0));
	auto mesh = std::make_shared<Mesh>(Vector3(), material, buildTwoTriangles(material));
	const ExplicitTransform transform{
		Vector3(0.0, 0.0, 0.0),
		Vector3(0.0, 0.0, 0.0),
		Vector3(2.0, 3.0, 1.0)
	};
	MeshInstance instance(mesh, transform.position, transform.rotationDegrees, transform.scale, material);

	HittableLightSample warmSample;
	TASK_CHECK(instance.sampleLight(Vector3(0.5, 0.5, 5.0), warmSample));

	std::vector<Triangle> first;
	first.emplace_back(
		Vector3(0.0, 0.0, 0.0),
		Vector3(2.0, 0.0, 0.0),
		Vector3(0.0, 2.0, 0.0),
		material
	);
	first.emplace_back(
		Vector3(3.0, 0.0, 0.0),
		Vector3(3.0, 0.0, 2.0),
		Vector3(3.0, 2.0, 0.0),
		material
	);
	std::vector<Triangle> second;
	second.emplace_back(
		Vector3(0.0, 0.0, -3.0),
		Vector3(5.0, 0.0, -3.0),
		Vector3(0.0, 4.0, -3.0),
		material
	);
	second.emplace_back(
		Vector3(6.0, 0.0, 0.0),
		Vector3(6.0, 0.0, 3.0),
		Vector3(6.0, 4.0, 0.0),
		material
	);

	// Two updates with no query in between: only the newest revision may be observed.
	TASK_CHECK(mesh->refitTriangles(first));
	TASK_CHECK(mesh->refitTriangles(second));
	TASK_CHECK(mesh->geometryRevision() == 2);

	const double expectedArea =
		explicitTriangleArea(explicitWorldTriangle(transform, second[0]))
		+ explicitTriangleArea(explicitWorldTriangle(transform, second[1]));
	const AABB expectedBox = explicitInstanceBoundingBox(transform, second);
	const double luminance = luminanceOf(material);

	const double firstRead = instance.lightSelectionWeight();
	AABB firstBox;
	TASK_CHECK(instance.createBoundingBox(firstBox));
	const double secondRead = instance.lightSelectionWeight();
	AABB secondBox;
	TASK_CHECK(instance.createBoundingBox(secondBox));

	TASK_CHECK(nearValue(firstRead, expectedArea * luminance, 1e-6));
	TASK_CHECK(nearValue(secondRead, expectedArea * luminance, 1e-6));
	TASK_CHECK(boxNear(firstBox, expectedBox, 1e-9));
	TASK_CHECK(boxNear(secondBox, firstBox, 1e-12));
	TASK_CHECK(mesh->geometryRevision() == 2);
	return (0);
}

static int	multithreadedFirstReadAfterRefit(void)
{
	auto material = std::make_shared<Emissive>(Color(1.0, 1.0, 1.0));
	auto mesh = std::make_shared<Mesh>(Vector3(), material, buildTwoTriangles(material));
	const ExplicitTransform transform{
		Vector3(1.0, -2.0, 0.5),
		Vector3(0.0, 0.0, 90.0),
		Vector3(2.0, 1.0, 3.0)
	};
	MeshInstance instance(mesh, transform.position, transform.rotationDegrees, transform.scale, material);

	HittableEmissionSample warm;
	TASK_CHECK(instance.sampleEmission(warm));

	std::vector<Triangle> deformed;
	deformed.emplace_back(
		Vector3(0.0, 0.0, -4.0),
		Vector3(4.0, 0.0, -4.0),
		Vector3(0.0, 3.0, -4.0),
		material
	);
	deformed.emplace_back(
		Vector3(6.0, 0.0, 0.0),
		Vector3(6.0, 0.0, 4.0),
		Vector3(6.0, 5.0, 0.0),
		material
	);
	TASK_CHECK(mesh->refitTriangles(deformed));

	const double expectedArea =
		explicitTriangleArea(explicitWorldTriangle(transform, deformed[0]))
		+ explicitTriangleArea(explicitWorldTriangle(transform, deformed[1]));
	const AABB expectedBox = explicitInstanceBoundingBox(transform, deformed);
	const double luminance = luminanceOf(material);
	const Vector3 worldPoint = explicitWorldPoint(transform, Vector3(1.0, 1.0, -4.0));
	const Vector3 origin(worldPoint.getX(), worldPoint.getY(), worldPoint.getZ() + 20.0);
	const Vector3 direction(0.0, 0.0, -1.0);
	const double expectedPDF = 400.0 / expectedArea;

	constexpr int threadCount = 4;
	std::atomic<int> ready(0);
	std::atomic<bool> go(false);
	std::atomic<int> failures(0);
	std::vector<std::thread> threads;

	for (int threadIndex = 0; threadIndex < threadCount; threadIndex++)
	{
		threads.emplace_back([&]() {
			ready.fetch_add(1);
			while (!go.load())
			{
				std::this_thread::yield();
			}

			int localFailures = 0;
			if (!nearValue(instance.lightSelectionWeight(), expectedArea * luminance, 1e-6))
			{
				localFailures++;
			}
			AABB box;
			if (!instance.createBoundingBox(box) || !boxNear(box, expectedBox, 1e-9))
			{
				localFailures++;
			}
			if (!nearValue(instance.pdfValue(origin, direction), expectedPDF, 1e-6))
			{
				localFailures++;
			}
			HittableLightSample lightSample;
			if (!instance.sampleLight(origin, lightSample) || !lightSample.valid || lightSample.pdf <= 0.0)
			{
				localFailures++;
			}
			if (localFailures != 0)
			{
				failures.fetch_add(1);
			}
		});
	}
	while (ready.load() != threadCount)
	{
		std::this_thread::yield();
	}
	go.store(true);
	for (std::thread& thread : threads)
	{
		thread.join();
	}

	TASK_CHECK(failures.load() == 0);
	TASK_CHECK(nearValue(instance.lightSelectionWeight(), expectedArea * luminance, 1e-6));

	// A later revision must still be picked up after the concurrent first read settled.
	deformed[0] = Triangle(
		Vector3(0.0, 0.0, -8.0),
		Vector3(2.0, 0.0, -8.0),
		Vector3(0.0, 1.0, -8.0),
		material
	);
	TASK_CHECK(mesh->refitTriangles(deformed));
	TASK_CHECK(mesh->geometryRevision() == 2);
	const double secondArea =
		explicitTriangleArea(explicitWorldTriangle(transform, deformed[0]))
		+ explicitTriangleArea(explicitWorldTriangle(transform, deformed[1]));
	TASK_CHECK(nearValue(instance.lightSelectionWeight(), secondArea * luminance, 1e-6));
	TASK_CHECK(!nearValue(secondArea, expectedArea, 1e-6));
	return (0);
}

/*
	Group: sampling
*/

static int	samplingDensityMatchesWorldAreaAfterRefit(void)
{
	auto material = std::make_shared<Emissive>(Color(1.0, 1.0, 1.0));
	auto mesh = std::make_shared<Mesh>(Vector3(), material, buildTwoTriangles(material));
	const ExplicitTransform transformA{
		Vector3(0.0, 0.0, 0.0),
		Vector3(0.0, 0.0, 0.0),
		Vector3(2.0, 1.0, 1.0)
	};
	const ExplicitTransform transformB{
		Vector3(5.0, 1.0, -2.0),
		Vector3(0.0, 0.0, 90.0),
		Vector3(1.0, 3.0, 2.0)
	};
	MeshInstance instanceA(mesh, transformA.position, transformA.rotationDegrees, transformA.scale, material);
	MeshInstance instanceB(mesh, transformB.position, transformB.rotationDegrees, transformB.scale, material);

	// Warm the old distribution first.
	HittableEmissionSample warm;
	TASK_CHECK(instanceA.sampleEmission(warm));
	TASK_CHECK(instanceB.sampleEmission(warm));

	std::vector<Triangle> deformed;
	deformed.emplace_back(
		Vector3(0.0, 0.0, -4.0),
		Vector3(4.0, 0.0, -4.0),
		Vector3(0.0, 3.0, -4.0),
		material
	);
	deformed.emplace_back(
		Vector3(6.0, 0.0, 0.0),
		Vector3(6.0, 0.0, 4.0),
		Vector3(6.0, 5.0, 0.0),
		material
	);

	// A rejected update must not disturb the sampling data or the revision.
	std::vector<Triangle> rejected(deformed.begin(), deformed.begin() + 1);
	TASK_CHECK(!mesh->refitTriangles(rejected));
	TASK_CHECK(mesh->geometryRevision() == 0);
	TASK_CHECK(mesh->refitTriangles(deformed));
	TASK_CHECK(mesh->geometryRevision() == 1);

	const ExplicitTriangle worldA0 = explicitWorldTriangle(transformA, deformed[0]);
	const ExplicitTriangle worldA1 = explicitWorldTriangle(transformA, deformed[1]);
	const ExplicitTriangle worldB0 = explicitWorldTriangle(transformB, deformed[0]);
	const ExplicitTriangle worldB1 = explicitWorldTriangle(transformB, deformed[1]);
	const double areaA0 = explicitTriangleArea(worldA0);
	const double areaA1 = explicitTriangleArea(worldA1);
	const double areaB0 = explicitTriangleArea(worldB0);
	const double areaB1 = explicitTriangleArea(worldB1);
	const double totalA = areaA0 + areaA1;
	const double totalB = areaB0 + areaB1;
	const double localArea0 = explicitTriangleArea(explicitLocalTriangle(deformed[0]));
	const double localArea1 = explicitTriangleArea(explicitLocalTriangle(deformed[1]));

	TASK_CHECK(nearValue(mesh->surfaceArea(), localArea0 + localArea1, 1e-9));

	constexpr int sampleCount = 4000;
	std::size_t countA0 = 0;
	std::size_t countA1 = 0;
	std::size_t countB0 = 0;
	std::size_t countB1 = 0;

	Sampler::setRenderSeed(20241117u);
	for (int sampleIndex = 0; sampleIndex < sampleCount; sampleIndex++)
	{
		HittableEmissionSample sampleA;
		Sampler::beginPixelSample(0, 0, static_cast<std::uint32_t>(sampleIndex));
		const bool sampledA = instanceA.sampleEmission(sampleA);
		Sampler::endPixelSample();
		TASK_CHECK(sampledA && sampleA.valid);
		if (pointInsideTriangle(sampleA.position, worldA0, 1e-7))
		{
			countA0++;
		}
		else if (pointInsideTriangle(sampleA.position, worldA1, 1e-7))
		{
			countA1++;
		}
		else
		{
			TASK_CHECK(false);
		}

		HittableEmissionSample sampleB;
		Sampler::beginPixelSample(1, 0, static_cast<std::uint32_t>(sampleIndex));
		const bool sampledB = instanceB.sampleEmission(sampleB);
		Sampler::endPixelSample();
		TASK_CHECK(sampledB && sampleB.valid);
		if (pointInsideTriangle(sampleB.position, worldB0, 1e-7))
		{
			countB0++;
		}
		else if (pointInsideTriangle(sampleB.position, worldB1, 1e-7))
		{
			countB1++;
		}
		else
		{
			TASK_CHECK(false);
		}
	}

	TASK_CHECK(countA0 + countA1 == static_cast<std::size_t>(sampleCount));
	TASK_CHECK(countB0 + countB1 == static_cast<std::size_t>(sampleCount));
	TASK_CHECK(nearValue(
		static_cast<double>(countA0),
		static_cast<double>(sampleCount) * (areaA0 / totalA),
		220.0
	));
	TASK_CHECK(nearValue(
		static_cast<double>(countA1),
		static_cast<double>(sampleCount) * (areaA1 / totalA),
		220.0
	));
	TASK_CHECK(nearValue(
		static_cast<double>(countB0),
		static_cast<double>(sampleCount) * (areaB0 / totalB),
		220.0
	));
	TASK_CHECK(nearValue(
		static_cast<double>(countB1),
		static_cast<double>(sampleCount) * (areaB1 / totalB),
		220.0
	));
	TASK_CHECK(nearValue(static_cast<double>(countA0) / static_cast<double>(countA1), areaA0 / areaA1, 0.12));
	TASK_CHECK(nearValue(static_cast<double>(countB0) / static_cast<double>(countB1), areaB0 / areaB1, 0.12));

	// Direct mesh sampling follows the refit local areas as well.
	std::size_t meshCount0 = 0;
	std::size_t meshCount1 = 0;
	const ExplicitTriangle local0 = explicitLocalTriangle(deformed[0]);
	const ExplicitTriangle local1 = explicitLocalTriangle(deformed[1]);
	for (int sampleIndex = 0; sampleIndex < 1000; sampleIndex++)
	{
		Vector3 position;
		Vector3 normal;
		Sampler::beginPixelSample(2, 0, static_cast<std::uint32_t>(sampleIndex));
		const bool sampled = mesh->sampleSurface(position, normal);
		Sampler::endPixelSample();
		TASK_CHECK(sampled);
		if (pointInsideTriangle(position, local0, 1e-7))
		{
			meshCount0++;
		}
		else if (pointInsideTriangle(position, local1, 1e-7))
		{
			meshCount1++;
		}
		else
		{
			TASK_CHECK(false);
		}
	}
	TASK_CHECK(nearValue(
		static_cast<double>(meshCount0),
		1000.0 * localArea0 / (localArea0 + localArea1),
		90.0
	));
	TASK_CHECK(nearValue(
		static_cast<double>(meshCount1),
		1000.0 * localArea1 / (localArea0 + localArea1),
		90.0
	));

	// The per-instance selection probabilities must be independently correct.
	TASK_CHECK(nearValue(
		instanceA.lightSelectionWeight(),
		totalA * luminanceOf(material),
		1e-6
	));
	TASK_CHECK(nearValue(
		instanceB.lightSelectionWeight(),
		totalB * luminanceOf(material),
		1e-6
	));
	return (0);
}

/*
	Group: visibility
*/

static int	visibilityRefreshesWorldBVHAfterRefit(void)
{
	auto material = std::make_shared<Emissive>(Color(1.0, 1.0, 1.0));
	auto mesh = std::make_shared<Mesh>(Vector3(), material, buildTwoTriangles(material));
	auto instance = std::make_shared<MeshInstance>(
		mesh,
		Vector3(0.0, 0.0, 0.0),
		Vector3(0.0, 0.0, 0.0),
		Vector3(1.0, 1.0, 1.0),
		material
	);
	BVHNode world(std::vector<std::shared_ptr<Hittable>>{instance});

	Ray probe(Vector3(0.25, 0.25, 5.0), Vector3(0.0, 0.0, -1.0));
	HitRecord hitRecord;
	TASK_CHECK(world.hit(probe, hitRecord, T_MIN, T_MAX));
	TASK_CHECK(nearValue(hitRecord.t0, 5.0, 1e-9));
	TASK_CHECK(world.hitAny(probe, T_MIN, T_MAX));

	// The second face moves in front of the first one along this ray.
	std::vector<Triangle> occluding;
	occluding.emplace_back(
		Vector3(0.0, 0.0, 0.0),
		Vector3(1.0, 0.0, 0.0),
		Vector3(0.0, 1.0, 0.0),
		material
	);
	occluding.emplace_back(
		Vector3(0.0, 0.0, 2.0),
		Vector3(2.0, 0.0, 2.0),
		Vector3(0.0, 2.0, 2.0),
		material
	);
	TASK_CHECK(mesh->refitTriangles(occluding));
	TASK_CHECK(mesh->geometryRevision() == 1);
	TASK_CHECK(world.refit());

	Ray occludedProbe(Vector3(0.25, 0.25, 5.0), Vector3(0.0, 0.0, -1.0));
	HitRecord occludedHit;
	TASK_CHECK(world.hit(occludedProbe, occludedHit, T_MIN, T_MAX));
	TASK_CHECK(nearValue(occludedHit.t0, 3.0, 1e-9));
	TASK_CHECK(world.hitAny(occludedProbe, T_MIN, T_MAX));

	AABB refreshedBox;
	TASK_CHECK(instance->createBoundingBox(refreshedBox));
	TASK_CHECK(boxNear(
		refreshedBox,
		explicitInstanceBoundingBox(ExplicitTransform{Vector3(), Vector3(), Vector3(1.0, 1.0, 1.0)}, occluding),
		1e-9
	));

	// Move both faces out of the old bounds: new location hits, old location misses.
	std::vector<Triangle> moved;
	moved.emplace_back(
		Vector3(20.0, 0.0, 0.0),
		Vector3(22.0, 0.0, 0.0),
		Vector3(20.0, 2.0, 0.0),
		material
	);
	moved.emplace_back(
		Vector3(30.0, 0.0, 0.0),
		Vector3(30.0, 0.0, 2.0),
		Vector3(30.0, 2.0, 0.0),
		material
	);
	TASK_CHECK(mesh->refitTriangles(moved));
	TASK_CHECK(mesh->geometryRevision() == 2);

	// Before the top-level refit the world still holds the previous bounds, so a ray at the new
	// location must be culled. After refit it must hit.
	Ray earlyProbe(Vector3(20.5, 0.5, 5.0), Vector3(0.0, 0.0, -1.0));
	HitRecord earlyHit;
	TASK_CHECK(!world.hit(earlyProbe, earlyHit, T_MIN, T_MAX));
	TASK_CHECK(world.refit());

	HitRecord movedHit;
	TASK_CHECK(world.hit(earlyProbe, movedHit, T_MIN, T_MAX));
	TASK_CHECK(nearValue(movedHit.t0, 5.0, 1e-9));
	TASK_CHECK(world.hitAny(earlyProbe, T_MIN, T_MAX));

	Ray oldProbe(Vector3(0.25, 0.25, 5.0), Vector3(0.0, 0.0, -1.0));
	HitRecord oldHit;
	TASK_CHECK(!world.hit(oldProbe, oldHit, T_MIN, T_MAX));
	TASK_CHECK(!world.hitAny(oldProbe, T_MIN, T_MAX));

	Ray sideProbe(Vector3(40.0, 0.5, 0.5), Vector3(-1.0, 0.0, 0.0));
	HitRecord sideHit;
	TASK_CHECK(world.hit(sideProbe, sideHit, T_MIN, T_MAX));
	TASK_CHECK(nearValue(sideHit.t0, 10.0, 1e-9));

	// Connectivity and face ordering are untouched by refits.
	const Mesh::PackedGeometryLayout layout = mesh->packedGeometryLayout();
	TASK_CHECK(layout.packed);
	TASK_CHECK(layout.triangleCount == 2);
	TASK_CHECK(layout.triangleIndices.size() == 2);
	TASK_CHECK(layout.nodeCount >= 1);
	TASK_CHECK(layoutBoundsMatchTriangles(layout, moved, 1e-9));
	return (0);
}

static int	visibilityStaleWorldMissesNewGeometry(void)
{
	auto material = std::make_shared<Emissive>(Color(1.0, 1.0, 1.0));
	auto mesh = std::make_shared<Mesh>(Vector3(), material, buildTwoTriangles(material));
	auto instance = std::make_shared<MeshInstance>(
		mesh,
		Vector3(0.0, 0.0, 0.0),
		Vector3(0.0, 0.0, 0.0),
		Vector3(1.0, 1.0, 1.0),
		material
	);
	BVHNode world(std::vector<std::shared_ptr<Hittable>>{instance});

	Ray probe(Vector3(0.5, 0.5, 5.0), Vector3(0.0, 0.0, -1.0));
	HitRecord hitRecord;
	TASK_CHECK(world.hit(probe, hitRecord, T_MIN, T_MAX));

	std::vector<Triangle> moved;
	moved.emplace_back(
		Vector3(0.0, 100.0, 0.0),
		Vector3(1.0, 100.0, 0.0),
		Vector3(0.0, 101.0, 0.0),
		material
	);
	moved.emplace_back(
		Vector3(3.0, 100.0, 0.0),
		Vector3(3.0, 100.0, 1.0),
		Vector3(3.0, 101.0, 0.0),
		material
	);
	TASK_CHECK(mesh->refitTriangles(moved));

	// The instance's own queries see the new geometry immediately...
	Ray oldProbe(Vector3(0.5, 0.5, 5.0), Vector3(0.0, 0.0, -1.0));
	HitRecord oldHit;
	TASK_CHECK(!instance->hit(oldProbe, oldHit, T_MIN, T_MAX));
	Ray newProbe(Vector3(0.5, 100.5, 5.0), Vector3(0.0, 0.0, -1.0));
	HitRecord directHit;
	TASK_CHECK(instance->hit(newProbe, directHit, T_MIN, T_MAX));
	TASK_CHECK(nearValue(directHit.t0, 5.0, 1e-9));

	// ...but the enclosing world keeps stale child bounds until it is explicitly refitted.
	HitRecord staleHit;
	TASK_CHECK(!world.hit(newProbe, staleHit, T_MIN, T_MAX));
	TASK_CHECK(world.refit());
	HitRecord refittedHit;
	TASK_CHECK(world.hit(newProbe, refittedHit, T_MIN, T_MAX));
	TASK_CHECK(nearValue(refittedHit.t0, 5.0, 1e-9));
	TASK_CHECK(world.hitAny(newProbe, T_MIN, T_MAX));

	HitRecord missAfterRefit;
	TASK_CHECK(!world.hit(oldProbe, missAfterRefit, T_MIN, T_MAX));
	TASK_CHECK(!world.hitAny(oldProbe, T_MIN, T_MAX));

	AABB newBox;
	TASK_CHECK(instance->createBoundingBox(newBox));
	TASK_CHECK(nearValue(newBox.getMinimum().getY(), 100.0 - T_MIN, 1e-9));
	TASK_CHECK(nearValue(newBox.getMaximum().getY(), 101.0 + T_MIN, 1e-9));
	return (0);
}

/*
	Registration
*/

extern const struct task_case	task_cases[] = {
	{"refit", "bounds_and_hits", refitUpdatesBoundsAndHits},
	{"refit", "internal_node_bounds", refitUpdatesInternalNodeBounds},
	{"refit", "rejects_unsupported_and_invalid", refitRejectsUnsupportedAndInvalidUpdates},
	{"refit", "repeated_refits_skipped_queries", repeatedRefitsWithSkippedQueries},
	{"instances", "shared_geometry_refreshes_both", sharedGeometryRefreshesBothInstances},
	{"instances", "skip_frames_and_repeat_revision", instanceSkipsFramesAndRepeatsRevision},
	{"instances", "multithreaded_first_read", multithreadedFirstReadAfterRefit},
	{"sampling", "density_matches_world_area", samplingDensityMatchesWorldAreaAfterRefit},
	{"visibility", "world_bvh_refit", visibilityRefreshesWorldBVHAfterRefit},
	{"visibility", "stale_world_misses_new_geometry", visibilityStaleWorldMissesNewGeometry}
};

extern const size_t	task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
