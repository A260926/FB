// Regression tests for the editable Dual Contouring octree.
//
// Each group returns the number of content assertions that were executed.
// CHECK_CHECK increments the shared counter used by support/driver.cpp and
// returns -1 immediately on failure, so a failed comparison prints its local
// inputs / differences below and yields a negative result.

#include "editable_octree.h"

#include "../support/check.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <functional>
#include <map>
#include <vector>

#include <glm/glm.hpp>

using glm::vec3;
using glm::ivec3;

// ----------------------------------------------------------------------------

namespace
{
	struct MeshData
	{
		VertexBuffer vertices;
		IndexBuffer indices;

		void clear()
		{
			vertices.clear();
			indices.clear();
		}
	};

	void Capture(EditableOctree& tree, float threshold, MeshData& out)
	{
		out.clear();
		tree.Mesh(threshold, out.vertices, out.indices);
	}

	MeshData FullModel(const ivec3& min,
	                   int size,
	                   std::function<float(const vec3&)> density,
	                   float threshold)
	{
		MeshData result;
		EditableOctree tree(min, size, density);
		Capture(tree, threshold, result);
		return result;
	}

	bool ValidIndices(const MeshData& mesh)
	{
		if (mesh.indices.size() % 3 != 0)
		{
			return false;
		}
		for (std::size_t i = 0; i < mesh.indices.size(); i++)
		{
			const int index = mesh.indices[i];
			if (index < 0 || static_cast<std::size_t>(index) >= mesh.vertices.size())
			{
				return false;
			}
		}
		return true;
	}

	// Cyclic rotation with the smallest index first.  Orientation is kept,
	// unlike a plain sort, so the direction of every triangle matters.
	std::array<int, 3> DirectedKey(int a, int b, int c)
	{
		std::array<int, 3> key;
		if (a <= b && a <= c)
		{
			key = {{ a, b, c }};
		}
		else if (b <= a && b <= c)
		{
			key = {{ b, c, a }};
		}
		else
		{
			key = {{ c, a, b }};
		}
		return key;
	}

	bool SameMesh(const char* label,
	              const MeshData& actual,
	              const MeshData& expected,
	              double tolerance)
	{
		if (actual.vertices.size() != expected.vertices.size())
		{
			std::fprintf(stderr, "%s: vertex count %zu (actual) vs %zu (expected)\n",
			             label, actual.vertices.size(), expected.vertices.size());
			return false;
		}
		if (actual.indices.size() != expected.indices.size())
		{
			std::fprintf(stderr, "%s: index count %zu (actual) vs %zu (expected)\n",
			             label, actual.indices.size(), expected.indices.size());
			return false;
		}

		const std::size_t n = actual.vertices.size();

		// Fast path: the meshing is deterministic, so corresponding vertices
		// usually already share an index.
		bool identity = true;
		for (std::size_t i = 0; i < n && identity; i++)
		{
			const double dp = glm::length(actual.vertices[i].xyz - expected.vertices[i].xyz);
			const double dn = glm::length(actual.vertices[i].normal - expected.vertices[i].normal);
			if (dp > tolerance || dn > tolerance)
			{
				identity = false;
			}
		}

		std::vector<int> mapping(n, -1);
		if (identity)
		{
			for (std::size_t i = 0; i < n; i++)
			{
				mapping[i] = static_cast<int>(i);
			}
		}
		else
		{
			std::vector<char> used(n, 0);
			for (std::size_t i = 0; i < n; i++)
			{
				double best = 1e300;
				std::size_t bestIndex = n;
				for (std::size_t j = 0; j < n; j++)
				{
					if (used[j])
					{
						continue;
					}
					const double dp = glm::length(actual.vertices[i].xyz -
					                              expected.vertices[j].xyz);
					const double dn = glm::length(actual.vertices[i].normal -
					                              expected.vertices[j].normal);
					if (dp <= tolerance && dn <= tolerance && dp + dn < best)
					{
						best = dp + dn;
						bestIndex = j;
					}
				}

				if (bestIndex == n)
				{
					std::fprintf(stderr,
					             "%s: vertex %zu at (%.9g, %.9g, %.9g) has no "
					             "match within %g\n",
					             label, i,
					             (double)actual.vertices[i].xyz.x,
					             (double)actual.vertices[i].xyz.y,
					             (double)actual.vertices[i].xyz.z,
					             tolerance);
					return false;
				}

				mapping[i] = static_cast<int>(bestIndex);
				used[bestIndex] = 1;
			}
		}

		std::map<std::array<int, 3>, int> actualTriangles;
		std::map<std::array<int, 3>, int> expectedTriangles;

		for (std::size_t i = 0; i + 2 < actual.indices.size(); i += 3)
		{
			actualTriangles[DirectedKey(mapping[actual.indices[i]],
			                            mapping[actual.indices[i + 1]],
			                            mapping[actual.indices[i + 2]])]++;
		}
		for (std::size_t i = 0; i + 2 < expected.indices.size(); i += 3)
		{
			expectedTriangles[DirectedKey(expected.indices[i],
			                              expected.indices[i + 1],
			                              expected.indices[i + 2])]++;
		}

		if (actualTriangles != expectedTriangles)
		{
			std::size_t differing = 0;
			for (std::map<std::array<int, 3>, int>::const_iterator it =
				     actualTriangles.begin(); it != actualTriangles.end(); ++it)
			{
				std::map<std::array<int, 3>, int>::const_iterator found =
					expectedTriangles.find(it->first);
				if (found == expectedTriangles.end() ||
					found->second != it->second)
				{
					if (differing == 0)
					{
						std::fprintf(stderr,
						             "%s: directed triangle (%d,%d,%d) count %d vs %d\n",
						             label, it->first[0], it->first[1],
						             it->first[2], it->second,
						             found == expectedTriangles.end() ? 0 : found->second);
					}
					++differing;
				}
			}
			std::fprintf(stderr, "%s: %zu directed triangle entries differ\n",
			             label, differing);
			return false;
		}

		return true;
	}

	bool CheckAnalyticPlane(const char* label,
	                        const MeshData& mesh,
	                        float height,
	                        double tolerance)
	{
		if (mesh.vertices.empty())
		{
			std::fprintf(stderr, "%s: expected a non-empty plane mesh\n", label);
			return false;
		}

		double maxHeight = 0.0;
		double maxNormal = 0.0;
		for (std::size_t i = 0; i < mesh.vertices.size(); i++)
		{
			const vec3& p = mesh.vertices[i].xyz;
			const vec3& n = mesh.vertices[i].normal;
			maxHeight = std::max(maxHeight, std::fabs((double)p.y - height));
			maxNormal = std::max(maxNormal, (double)glm::length(n - vec3(0.f, 1.f, 0.f)));
		}

		if (maxHeight > tolerance || maxNormal > tolerance)
		{
			std::fprintf(stderr,
			             "%s: plane deviation height=%.9g normal=%.9g (tol %g)\n",
			             label, maxHeight, maxNormal, tolerance);
			return false;
		}
		return true;
	}

	bool ContainsVertexNear(const MeshData& mesh, const vec3& p, double tolerance)
	{
		for (std::size_t i = 0; i < mesh.vertices.size(); i++)
		{
			if (glm::length(mesh.vertices[i].xyz - p) <= tolerance)
			{
				return true;
			}
		}
		return false;
	}

	// Highest surface point inside an axis aligned footprint (x/z only).
	double MaxHeightInFootprint(const MeshData& mesh, float x, float z, float halfExtent)
	{
		double best = -1e300;
		for (std::size_t i = 0; i < mesh.vertices.size(); i++)
		{
			const vec3& p = mesh.vertices[i].xyz;
			if (std::fabs(p.x - x) <= halfExtent && std::fabs(p.z - z) <= halfExtent)
			{
				best = std::max(best, (double)p.y);
			}
		}
		return best;
	}

	bool SameVertexSet(const MeshData& a, const MeshData& b, double tolerance)
	{
		if (a.vertices.size() != b.vertices.size())
		{
			return false;
		}
		for (std::size_t i = 0; i < a.vertices.size(); i++)
		{
			if (!ContainsVertexNear(b, a.vertices[i].xyz, tolerance))
			{
				return false;
			}
		}
		return true;
	}

	// Mutable scene state captured by the density closures below.
	struct Scene
	{
		bool addShell;
		bool planeBump;
		bool blob;
		bool cornerChange;
		vec3 sphereCenter;
		float sphereRadius;
	};

	// Smooth compactly supported radial window, zero for r >= radius.
	float Window(const vec3& p, const vec3& center, float radius)
	{
		const float r2 = glm::dot(p - center, p - center);
		const float R2 = radius * radius;
		if (r2 >= R2)
		{
			return 0.f;
		}
		const float u = 1.f - r2 / R2;
		return u * u;
	}

	const vec3 CORNER_CELL_CENTER(4.5f, 4.5f, 4.5f);

	float CornerField(const Scene& scene, const vec3& p)
	{
		float field = p.y - 4.5f;
		if (scene.cornerChange)
		{
			// Small, compactly supported perturbation.  It is zero at the
			// window boundary, so density outside the update box is intact.
			const float window = Window(p, CORNER_CELL_CENTER, 1.5f);
			const float tilt = 0.25f * ((p.x - 4.5f) + (p.z - 4.5f));
			field -= tilt * window;
		}
		return field;
	}
}

// ----------------------------------------------------------------------------
// edit_geometry: incremental results must equal a full rebuild, and the
// resulting geometry must be sane against analytic fields.
// ----------------------------------------------------------------------------

int edit_geometry()
{
	const ivec3 rootMin(-16, -4, -16);
	const int rootSize = 32;

	Scene scene;
	scene.addShell = false;
	scene.planeBump = false;
	scene.blob = false;
	scene.cornerChange = false;
	scene.sphereCenter = vec3(15.f, 2.5f, 1.f);
	scene.sphereRadius = 16.f;

	// Field: a large sphere, optionally with a second small shell that only
	// exists while scene.addShell is set.  sphereCenter and sphereRadius are
	// mutable so the same closure can be reused by a full rebuild.
	std::function<float(const vec3&)> sphereDensity =
		[&scene](const vec3& p) -> float
		{
			float value = glm::length(p - scene.sphereCenter) - scene.sphereRadius;
			if (scene.addShell)
			{
				const float shell =
					glm::length(p - vec3(-4.f, 8.f, -4.f)) - 2.5f;
				value = std::min(value, shell);
			}
			return value;
		};

	// Field: a plane at y = 8.5 with an optional compactly supported bump.
	std::function<float(const vec3&)> planeDensity =
		[&scene](const vec3& p) -> float
		{
			float height = 8.5f;
			if (scene.planeBump)
			{
				height += 1.5f * Window(p, vec3(4.f, 8.5f, 4.f), 4.f);
			}
			return p.y - height;
		};

	// 1. Sphere at the fine resolution matches a fresh full construction.
	{
		EditableOctree tree(rootMin, rootSize, sphereDensity);
		MeshData actual;
		Capture(tree, -1.f, actual);
		const MeshData expected = FullModel(rootMin, rootSize, sphereDensity, -1.f);
		CHECK_CHECK(SameMesh("sphere-fine", actual, expected, 1e-5));
		CHECK_CHECK(ValidIndices(actual));
	}

	// 2. Sphere geometry actually lies near the analytic sphere and indices
	//    are valid.
	{
		EditableOctree tree(rootMin, rootSize, sphereDensity);
		MeshData mesh;
		Capture(tree, -1.f, mesh);
		double maxError = 0.0;
		for (std::size_t i = 0; i < mesh.vertices.size(); i++)
		{
			const double radial = std::fabs(
				(double)glm::length(mesh.vertices[i].xyz - scene.sphereCenter) -
				(double)scene.sphereRadius);
			maxError = std::max(maxError, radial);
		}
		if (maxError > 0.25)
		{
			std::fprintf(stderr, "sphere-fine: radial error %.9g\n", maxError);
		}
		CHECK_CHECK(maxError <= 0.25);
		CHECK_CHECK(ValidIndices(mesh));
	}

	// 3. Analytic plane at the fine resolution: vertices lie on the plane and
	//    normals point along +y.
	{
		EditableOctree tree(rootMin, rootSize, planeDensity);
		MeshData mesh;
		Capture(tree, -1.f, mesh);
		CHECK_CHECK(CheckAnalyticPlane("plane-fine", mesh, 8.5f, 1e-3));
		CHECK_CHECK(ValidIndices(mesh));
	}

	// 4. A compactly supported bump changes only the announced region; the
	//    incremental result matches a full rebuild of the new field.
	{
		EditableOctree tree(rootMin, rootSize, planeDensity);
		MeshData before;
		Capture(tree, -1.f, before);
		CHECK_CHECK(CheckAnalyticPlane("plane-before-bump", before, 8.5f, 1e-3));

		scene.planeBump = true;
		tree.Update(vec3(0.f, 4.f, 0.f), vec3(8.f, 13.f, 8.f));

		MeshData after;
		Capture(tree, -1.f, after);
		const MeshData expected = FullModel(rootMin, rootSize, planeDensity, -1.f);
		CHECK_CHECK(SameMesh("plane-bump", after, expected, 1e-5));
		CHECK_CHECK(ValidIndices(after));
		CHECK_CHECK(MaxHeightInFootprint(after, 4.f, 4.f, 2.f) > 9.5);
		scene.planeBump = false;
	}

	// 5. A new shell grows in an area that was empty of surface before.
	{
		EditableOctree tree(rootMin, rootSize, sphereDensity);
		MeshData before;
		Capture(tree, -1.f, before);
		CHECK_CHECK(!ContainsVertexNear(before, vec3(-4.f, 8.f, -4.f), 2.0));

		scene.addShell = true;
		tree.Update(vec3(-8.f, 4.f, -8.f), vec3(0.f, 12.f, 0.f));

		MeshData after;
		Capture(tree, -1.f, after);
		const MeshData expected = FullModel(rootMin, rootSize, sphereDensity, -1.f);
		CHECK_CHECK(SameMesh("shell-add", after, expected, 1e-5));
		CHECK_CHECK(ValidIndices(after));
		CHECK_CHECK(MaxHeightInFootprint(after, -4.f, -4.f, 1.5f) > 10.2);
	}

	// 6. Several overlapping updates, applied in sequence, match a full
	//    rebuild of the final state.  Both features are bumps on the same
	//    infinite plane, each with compact support, so every edit has a
	//    well-defined local box.
	{
		bool leftBump = false;
		bool rightBump = false;
		std::function<float(const vec3&)> combinedDensity =
			[&leftBump, &rightBump](const vec3& p) -> float
			{
				float height = 8.5f;
				if (leftBump)
				{
					height += 1.5f * Window(p, vec3(-4.f, 8.5f, -4.f), 4.f);
				}
				if (rightBump)
				{
					height += 1.5f * Window(p, vec3(4.f, 8.5f, 4.f), 4.f);
				}
				return p.y - height;
			};
		EditableOctree tree(rootMin, rootSize, combinedDensity);

		rightBump = true;
		tree.Update(vec3(0.f, 4.f, 0.f), vec3(8.f, 13.f, 8.f));
		leftBump = true;
		tree.Update(vec3(-8.f, 4.f, -8.f), vec3(0.f, 13.f, 0.f));
		// Overlapping box that re-samples the right bump's whole support and
		// switches it off again.
		rightBump = false;
		tree.Update(vec3(0.f, 4.f, 0.f), vec3(8.f, 13.f, 8.f));

		MeshData actual;
		Capture(tree, -1.f, actual);
		const MeshData expected = FullModel(rootMin, rootSize, combinedDensity, -1.f);
		CHECK_CHECK(SameMesh("overlapping", actual, expected, 1e-5));
		CHECK_CHECK(ValidIndices(actual));
		CHECK_CHECK(MaxHeightInFootprint(actual, -4.f, -4.f, 1.5f) > 9.5);
		CHECK_CHECK(MaxHeightInFootprint(actual, 4.f, 4.f, 1.5f) < 9.0);
	}

	// 7. Update box straddling the tree's middle planes (x = 0 and z = 0).
	//    A compactly supported local blob appears exactly across the median.
	{
			bool bump = false;
		std::function<float(const vec3&)> centerField =
			[&bump](const vec3& p) -> float
			{
				float height = 4.5f;
				if (bump)
				{
					// Radius 4 ball centred on the surface at the origin.
					height += 2.f * Window(p, vec3(0.f, 4.5f, 0.f), 4.f);
				}
				return p.y - height;
			};
		EditableOctree tree(rootMin, rootSize, centerField);

		bump = true;
		tree.Update(vec3(-4.f, 0.f, -4.f), vec3(4.f, 9.f, 4.f));

		MeshData actual;
		Capture(tree, -1.f, actual);
		const MeshData expected = FullModel(rootMin, rootSize, centerField, -1.f);
		CHECK_CHECK(SameMesh("median-box", actual, expected, 1e-5));
		CHECK_CHECK(ValidIndices(actual));
		CHECK_CHECK(MaxHeightInFootprint(actual, 0.f, 0.f, 1.f) > 5.5);
		}

	// 8. Corner signs stay the same, but edge crossing positions and finite
	//    difference normals change; the incremental mesh must still match.
	{
		// Local helpers: locate the zero crossing and the central difference
		// normal exactly the way the contouring code does.
		struct Crossing
		{
			vec3 position;
			vec3 normal;
			bool operator==(const Crossing& o) const
			{
				return position == o.position && normal == o.normal;
			}
			bool operator!=(const Crossing& o) const { return !(*this == o); }
		};

		auto crossing = [](std::function<float(const vec3&)>& field,
		                   const vec3& p0,
		                   const vec3& p1) -> Crossing
		{
			float minValue = 1e9f;
			float t = 0.f;
			float currentT = 0.f;
			const int steps = 8;
			const float increment = 1.f / (float)steps;
			while (currentT <= 1.f)
			{
				const vec3 p = p0 + (p1 - p0) * currentT;
				const float value = std::fabs(field(p));
				if (value < minValue)
				{
					minValue = value;
					t = currentT;
				}
				currentT += increment;
			}
			Crossing result;
			result.position = p0 + (p1 - p0) * t;
			const float H = 0.001f;
			result.normal = glm::normalize(vec3(
				field(result.position + vec3(H, 0.f, 0.f)) -
					field(result.position - vec3(H, 0.f, 0.f)),
				field(result.position + vec3(0.f, H, 0.f)) -
					field(result.position - vec3(0.f, H, 0.f)),
				field(result.position + vec3(0.f, 0.f, H)) -
					field(result.position - vec3(0.f, 0.f, H))));
			return result;
		};

		scene.cornerChange = false;
		std::function<float(const vec3&)> cornerDensity =
			[&scene](const vec3& p) -> float { return CornerField(scene, p); };

		EditableOctree tree(ivec3(0, 0, 0), 16, cornerDensity);
		MeshData before;
		Capture(tree, -1.f, before);

		int oldSigns = 0;
		int newSigns = 0;
		const ivec3 corners[8] = {
			ivec3(4, 4, 4), ivec3(4, 4, 5), ivec3(4, 5, 4), ivec3(4, 5, 5),
			ivec3(5, 4, 4), ivec3(5, 4, 5), ivec3(5, 5, 4), ivec3(5, 5, 5),
		};
		for (int i = 0; i < 8; i++)
		{
			const vec3 p(corners[i]);
			if (cornerDensity(p) >= 0.f)
			{
				oldSigns |= (1 << i);
			}
		}

		// An edge of the cell with a zero crossing in both states.
		const vec3 e0(4.f, 4.f, 4.f);
		const vec3 e1(4.f, 5.f, 4.f);
		const Crossing crossingBefore = crossing(cornerDensity, e0, e1);

		scene.cornerChange = true;
		for (int i = 0; i < 8; i++)
		{
			const vec3 p(corners[i]);
			if (cornerDensity(p) >= 0.f)
			{
				newSigns |= (1 << i);
			}
		}
		const Crossing crossingAfter = crossing(cornerDensity, e0, e1);

		CHECK_CHECK(oldSigns == newSigns);
		CHECK_CHECK(crossingBefore != crossingAfter);
		CHECK_CHECK(glm::length(crossingBefore.position - crossingAfter.position) > 1e-4);
		CHECK_CHECK(glm::length(crossingBefore.normal - crossingAfter.normal) > 1e-4);

		// The perturbation is only non-zero inside this box.
		tree.Update(vec3(3.f, 3.f, 3.f), vec3(6.f, 6.f, 6.f));

		MeshData after;
		Capture(tree, -1.f, after);
		const MeshData expected = FullModel(ivec3(0, 0, 0), 16, cornerDensity, -1.f);
		CHECK_CHECK(SameMesh("corner-signs", after, expected, 1e-5));
		CHECK_CHECK(ValidIndices(after));
		// The surface really moved inside the affected cell.
		CHECK_CHECK(!SameVertexSet(before, after, 1e-4));
		scene.cornerChange = false;
	}

	// 9. Removing a previously added feature restores the original mesh.
	{
		scene.addShell = true;
		EditableOctree tree(rootMin, rootSize, sphereDensity);
		MeshData withShell;
		Capture(tree, -1.f, withShell);

		scene.addShell = false;
		tree.Update(vec3(-8.f, 4.f, -8.f), vec3(0.f, 12.f, 0.f));

		MeshData actual;
		Capture(tree, -1.f, actual);
		const MeshData expected = FullModel(rootMin, rootSize, sphereDensity, -1.f);
		CHECK_CHECK(SameMesh("shell-remove", actual, expected, 1e-5));
		CHECK_CHECK(ValidIndices(actual));
		CHECK_CHECK(!SameVertexSet(withShell, actual, 1e-4));
	}

	// 10. Update box flush against an internal octree partition boundary.
	//     The root spans [-16, 16); x = 0 and z = 0 are middle partition
	//     planes, and the box starts exactly on them.
	{
		bool bump = false;
		std::function<float(const vec3&)> field =
			[&bump](const vec3& p) -> float
			{
				float height = 8.5f;
				if (bump)
				{
					// Support is the radius 4 ball around (4, 8.5, 4).  Its
					// footprint spans x,z in [0, 8], so the change lies in the
					// positive quadrant yet touches the median planes x=0,z=0.
					height += 1.5f * Window(p, vec3(4.f, 8.5f, 4.f), 4.f);
				}
				return p.y - height;
			};
		EditableOctree tree(rootMin, rootSize, field);

		bump = true;
		tree.Update(vec3(0.f, 4.5f, 0.f), vec3(8.f, 12.5f, 8.f));

		MeshData actual;
		Capture(tree, -1.f, actual);
		const MeshData expected = FullModel(rootMin, rootSize, field, -1.f);
		CHECK_CHECK(SameMesh("partition-flush", actual, expected, 1e-5));
		CHECK_CHECK(ValidIndices(actual));
		CHECK_CHECK(MaxHeightInFootprint(actual, 4.f, 4.f, 1.5f) > 9.5);
	}

	return static_cast<int>(check_check_count);
}

// ----------------------------------------------------------------------------
// edit_invalidation: update bookkeeping must reflect real samples and must not
// rebuild unaffected leaves.
// ----------------------------------------------------------------------------

int edit_invalidation()
{
	const ivec3 rootMin(-16, -4, -16);
	const int rootSize = 32;

	Scene scene;
	scene.addShell = false;
	scene.planeBump = false;
	scene.blob = false;
	scene.cornerChange = false;
	scene.sphereCenter = vec3(15.f, 2.5f, 1.f);
	scene.sphereRadius = 16.f;

	// External counter used to prove that the reported statistics come from
	// real invocations of the user closure.
	std::size_t externalCalls = 0;
	std::function<float(const vec3&)> planeDensity =
		[&scene, &externalCalls](const vec3& p) -> float
		{
			++externalCalls;
			float height = 8.5f;
			if (scene.planeBump)
			{
				height += 1.5f * Window(p, vec3(4.f, 8.5f, 4.f), 4.f);
			}
			return p.y - height;
		};

	// 1. Constructor reports exactly the calls actually made.
	externalCalls = 0;
	EditableOctree tree(rootMin, rootSize, planeDensity);
	const EditableOctree::Stats constructionStats = tree.LastUpdateStats();
	CHECK_CHECK(constructionStats.density_calls > 0);
	CHECK_CHECK(constructionStats.density_calls == externalCalls);
	CHECK_CHECK(constructionStats.rebuilt_leaves ==
	            static_cast<std::size_t>(rootSize) * rootSize * rootSize);

	const std::size_t fullCalls = constructionStats.density_calls;

	// 2a. The spec's canonical local edit: a side-2 box strictly inside the
	//     32-cube root.  It must cost less than a quarter of a full build and
	//     rebuild far fewer than 1/4 of the unit cells.
	{
		bool bump = false;
		std::size_t external = 0;
		std::function<float(const vec3&)> density =
			[&bump, &external](const vec3& p) -> float
			{
				++external;
				float height = 8.5f;
				if (bump)
				{
					height += 1.5f * Window(p, vec3(9.f, 8.5f, 9.f), 4.f);
				}
				return p.y - height;
			};
		EditableOctree local(rootMin, rootSize, density);
		const std::size_t localFullCalls = local.LastUpdateStats().density_calls;

		bump = true;
		external = 0;
		local.Update(vec3(8.f, 8.f, 8.f), vec3(10.f, 10.f, 10.f));
		const EditableOctree::Stats sideTwo = local.LastUpdateStats();

		CHECK_CHECK(sideTwo.density_calls == external);
		CHECK_CHECK(sideTwo.density_calls > 0);
		CHECK_CHECK(sideTwo.density_calls * 4 < localFullCalls);
		CHECK_CHECK(sideTwo.rebuilt_leaves > 0);
		CHECK_CHECK(sideTwo.rebuilt_leaves * 4 <
		            static_cast<std::size_t>(rootSize) * rootSize * rootSize);
	}

	// 2. An internal 2x2x2 box is far cheaper than a quarter of a full build,
	//    and the reported figure is again the real number of evaluations.
	//    The box contains the whole support of the plane bump, so this is a
	//    genuine local edit and the final model matches a full rebuild.
	scene.planeBump = true;
	externalCalls = 0;
	tree.Update(vec3(0.f, 4.f, 0.f), vec3(8.f, 13.f, 8.f));
	const EditableOctree::Stats updateStats = tree.LastUpdateStats();
	CHECK_CHECK(updateStats.density_calls == externalCalls);
	CHECK_CHECK(updateStats.density_calls * 4 < fullCalls);
	CHECK_CHECK(updateStats.rebuilt_leaves <
	            static_cast<std::size_t>(rootSize) * rootSize * rootSize / 4);
	CHECK_CHECK(updateStats.density_calls > 0);

	MeshData edited;
	Capture(tree, -1.f, edited);
	CHECK_CHECK(SameMesh("invalidation-edited",
	                     edited, FullModel(rootMin, rootSize, planeDensity, -1.f),
	                     1e-5));
	scene.planeBump = false;

	// 3. Meshing, including threshold switches, never resamples the field and
	//    never disturbs the last update statistics.
	{
		externalCalls = 0;
		MeshData fine, coarse, fineAgain;
		Capture(tree, -1.f, fine);
		Capture(tree, 1.f, coarse);
		Capture(tree, -1.f, fineAgain);
		CHECK_CHECK(externalCalls == 0);
		CHECK_CHECK(tree.LastUpdateStats().density_calls == updateStats.density_calls);
		CHECK_CHECK(tree.LastUpdateStats().rebuilt_leaves == updateStats.rebuilt_leaves);
		CHECK_CHECK(fine.vertices.size() >= coarse.vertices.size());
		CHECK_CHECK(SameMesh("invalidation-fine-restore", fineAgain, fine, 1e-5));
	}

	// 4. An update box spanning the tree's middle plane stays local.  A local
	//    bump sits across x = 0 / z = 0; its support is the radius 3 ball
	//    around the origin, entirely inside the box.
	{
		bool medianBump = false;
		std::function<float(const vec3&)> medianField =
			[&medianBump, &externalCalls](const vec3& p) -> float
			{
				++externalCalls;
				float height = 8.5f;
				if (medianBump)
				{
					height += 1.5f * Window(p, vec3(0.f, 8.5f, 0.f), 3.f);
				}
				return p.y - height;
			};
		EditableOctree medianTree(rootMin, rootSize, medianField);

		medianBump = true;
		externalCalls = 0;
		medianTree.Update(vec3(-3.f, 5.f, -3.f), vec3(3.f, 12.f, 3.f));
		const EditableOctree::Stats medianStats = medianTree.LastUpdateStats();
		CHECK_CHECK(medianStats.density_calls == externalCalls);
		CHECK_CHECK(medianStats.density_calls * 4 < fullCalls);
		CHECK_CHECK(medianStats.rebuilt_leaves <
		            static_cast<std::size_t>(rootSize) * rootSize * rootSize / 4);

		MeshData medianMesh;
		Capture(medianTree, -1.f, medianMesh);
		CHECK_CHECK(SameMesh("invalidation-median",
		                     medianMesh,
		                     FullModel(rootMin, rootSize, medianField, -1.f),
		                     1e-5));
	}

	// 5. Completely empty and completely solid fields produce empty meshes.
	{
		bool solid = false;
		std::function<float(const vec3&)> stateField =
			[&solid](const vec3&) -> float { return solid ? -1.f : 1.f; };

		EditableOctree emptyTree(ivec3(0, 0, 0), 16, stateField);
		MeshData emptyMesh;
		Capture(emptyTree, -1.f, emptyMesh);
		CHECK_CHECK(emptyMesh.vertices.empty());
		CHECK_CHECK(emptyMesh.indices.empty());

		solid = true;
		emptyTree.Update(vec3(0.f, 0.f, 0.f), vec3(16.f, 16.f, 16.f));
		MeshData solidMesh;
		Capture(emptyTree, -1.f, solidMesh);
		CHECK_CHECK(solidMesh.vertices.empty());
		CHECK_CHECK(solidMesh.indices.empty());

		// A local change in an all-solid model grows a bounded surface.
		scene.blob = false;
		std::function<float(const vec3&)> blobField =
			[&scene](const vec3& p) -> float
			{
				if (!scene.blob)
				{
					return -1.f;
				}
				return -1.f + 2.f * Window(p, vec3(8.f, 8.f, 8.f), 4.f);
			};
		EditableOctree growTree(ivec3(0, 0, 0), 16, blobField);
		MeshData beforeGrowth;
		Capture(growTree, -1.f, beforeGrowth);
		CHECK_CHECK(beforeGrowth.vertices.empty());
		scene.blob = true;
		growTree.Update(vec3(4.f, 4.f, 4.f), vec3(12.f, 12.f, 12.f));
		MeshData afterGrowth;
		Capture(growTree, -1.f, afterGrowth);
		CHECK_CHECK(!afterGrowth.vertices.empty());
		CHECK_CHECK(ValidIndices(afterGrowth));
		// Full rebuild of the same state agrees.
		const MeshData growthExpected =
			FullModel(ivec3(0, 0, 0), 16, blobField, -1.f);
		CHECK_CHECK(SameMesh("grow-from-solid", afterGrowth, growthExpected, 1e-5));
		scene.blob = false;
	}

	return static_cast<int>(check_check_count);
}

// ----------------------------------------------------------------------------
// edit_views: meshing at different thresholds is a non-destructive view of the
// same persistent geometry, and independent instances never interfere.
// ----------------------------------------------------------------------------

int edit_views()
{
	const ivec3 rootMin(-16, -4, -16);
	const int rootSize = 32;

	Scene scene;
	scene.addShell = false;
	scene.planeBump = false;
	scene.blob = false;
	scene.cornerChange = false;
	scene.sphereCenter = vec3(15.f, 2.5f, 1.f);
	scene.sphereRadius = 16.f;

	std::size_t externalCalls = 0;
	std::function<float(const vec3&)> sphereDensity =
		[&scene, &externalCalls](const vec3& p) -> float
		{
			++externalCalls;
			float value = glm::length(p - scene.sphereCenter) - scene.sphereRadius;
			if (scene.addShell)
			{
				const float shell =
					glm::length(p - vec3(-4.f, 8.f, -4.f)) - 2.5f;
				value = std::min(value, shell);
			}
			return value;
		};

	auto planeDensity = [&scene](const vec3& p) -> float
		{
			float height = 8.5f;
			if (scene.planeBump)
			{
				height += 1.5f * Window(p, vec3(4.f, 8.5f, 4.f), 4.f);
			}
			return p.y - height;
		};

	// 1. Coarse view followed by the unsimplified view restores the full fine
	//    mesh exactly, and neither threshold resamples the field.
	{
		externalCalls = 0;
		EditableOctree tree(rootMin, rootSize, sphereDensity);

		MeshData fine;
		Capture(tree, -1.f, fine);

		const std::size_t callsAfterBuild = externalCalls;
		CHECK_CHECK(callsAfterBuild > 0);

		MeshData coarse;
		Capture(tree, 1.f, coarse);
		CHECK_CHECK(externalCalls == callsAfterBuild);

		MeshData fineAgain;
		Capture(tree, -1.f, fineAgain);
		CHECK_CHECK(externalCalls == callsAfterBuild);

		CHECK_CHECK(coarse.vertices.size() < fine.vertices.size());
		CHECK_CHECK(SameMesh("view-fine-restore", fineAgain, fine, 1e-5));
		CHECK_CHECK(ValidIndices(coarse));
	}

	// 2. The coarse view of an edited model matches a full rebuild at the
	//    coarse threshold.
	{
		scene.addShell = true;
		EditableOctree tree(rootMin, rootSize, sphereDensity);
		tree.Update(vec3(-8.f, 4.f, -8.f), vec3(0.f, 12.f, 0.f));

		MeshData actual;
		Capture(tree, 1.f, actual);
		const MeshData expected = FullModel(rootMin, rootSize, sphereDensity, 1.f);
		CHECK_CHECK(SameMesh("view-coarse-edited", actual, expected, 1e-5));
		CHECK_CHECK(ValidIndices(actual));
		scene.addShell = false;
	}

	// 3. A coarse view is not edit data: after a local edit the unsimplified
	//    mesh still contains all fine geometry, matching a full rebuild.
	{
		scene.addShell = false;
		EditableOctree tree(rootMin, rootSize, sphereDensity);
		MeshData fineBefore;
		Capture(tree, -1.f, fineBefore);

		// Produce and discard a coarse view.
		MeshData coarse;
		Capture(tree, 10.f, coarse);
		CHECK_CHECK(coarse.vertices.size() < fineBefore.vertices.size());

		scene.addShell = true;
		tree.Update(vec3(-8.f, 4.f, -8.f), vec3(0.f, 12.f, 0.f));

		MeshData fineAfter;
		Capture(tree, -1.f, fineAfter);
		const MeshData expected = FullModel(rootMin, rootSize, sphereDensity, -1.f);
		CHECK_CHECK(SameMesh("view-not-edit-data", fineAfter, expected, 1e-5));
		CHECK_CHECK(ValidIndices(fineAfter));
		CHECK_CHECK(fineAfter.vertices.size() > coarse.vertices.size());
		scene.addShell = false;
	}

	// 4. Repeated meshing at different thresholds is order independent.
	{
		EditableOctree tree(rootMin, rootSize, sphereDensity);
		MeshData coarseFirst, fineMiddle, coarseAgain, fineAgain;
		Capture(tree, 1.f, coarseFirst);
		Capture(tree, -1.f, fineMiddle);
		Capture(tree, 1.f, coarseAgain);
		Capture(tree, -1.f, fineAgain);
		CHECK_CHECK(SameMesh("view-repeat-coarse", coarseAgain, coarseFirst, 1e-5));
		CHECK_CHECK(SameMesh("view-repeat-fine", fineAgain, fineMiddle, 1e-5));
		CHECK_CHECK(SameVertexSet(coarseFirst, coarseAgain, 1e-5));
		CHECK_CHECK(ValidIndices(fineAgain));
	}

	// 5. Two instances with different closures, updated and meshed in an
	//    interleaved order, never leak state into each other.
	{
		EditableOctree sphereTree(rootMin, rootSize, sphereDensity);
		EditableOctree planeTree(rootMin, rootSize, planeDensity);

		// Interleave: update one, mesh the other, then swap.
		scene.addShell = true;
		sphereTree.Update(vec3(-8.f, 4.f, -8.f), vec3(0.f, 12.f, 0.f));

		MeshData planeBefore;
		Capture(planeTree, -1.f, planeBefore);
		const MeshData planeExpected = FullModel(rootMin, rootSize, planeDensity, -1.f);
		CHECK_CHECK(SameMesh("view-two-instances-plane", planeBefore, planeExpected, 1e-5));

		scene.planeBump = true;
		planeTree.Update(vec3(0.f, 4.f, 0.f), vec3(8.f, 13.f, 8.f));

		MeshData sphereAfter;
		Capture(sphereTree, -1.f, sphereAfter);
		const MeshData sphereExpected = FullModel(rootMin, rootSize, sphereDensity, -1.f);
		CHECK_CHECK(SameMesh("view-two-instances-sphere", sphereAfter, sphereExpected, 1e-5));

		MeshData planeAfter;
		Capture(planeTree, -1.f, planeAfter);
		const MeshData planeExpectedAfter = FullModel(rootMin, rootSize, planeDensity, -1.f);
		CHECK_CHECK(SameMesh("view-two-instances-plane-edited", planeAfter,
		                     planeExpectedAfter, 1e-5));
		CHECK_CHECK(!SameVertexSet(sphereAfter, planeAfter, 1e-4));
		CHECK_CHECK(ValidIndices(sphereAfter));
		CHECK_CHECK(ValidIndices(planeAfter));

		scene.addShell = false;
		scene.planeBump = false;
	}

	// 6. Larger thresholds simplify at least as aggressively, and every view
	//    remains a valid mesh of the same persistent model.
	{
		EditableOctree tree(rootMin, rootSize, sphereDensity);
		MeshData reference;
		Capture(tree, -1.f, reference);

		const float thresholds[4] = { 0.1f, 1.f, 10.f, 50.f };
		std::size_t previousVertices = reference.vertices.size();
		for (int i = 0; i < 4; i++)
		{
			MeshData view;
			Capture(tree, thresholds[i], view);
			CHECK_CHECK(view.vertices.size() <= previousVertices);
			CHECK_CHECK(ValidIndices(view));
			previousVertices = view.vertices.size();
		}

		// After all those coarse views the fine model is still intact.
		MeshData fine;
		Capture(tree, -1.f, fine);
		CHECK_CHECK(SameMesh("view-many-thresholds", fine, reference, 1e-5));
	}

	return static_cast<int>(check_check_count);
}

// ----------------------------------------------------------------------------
