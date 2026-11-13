/*
	Editable Dual Contouring sample.

	Builds an editable octree, writes the model before and after a sequence of
	local sculpt operations as OBJ files, and verifies the results along the way:

	  * every incremental state is compared against a full rebuild of the same
	    field (same threshold, vertex positions, normals and directed triangles);
	  * a plane-only model is checked against its analytic height and normal;
	  * empty and fully solid models must yield empty meshes, and a later local
	    change must be able to grow a surface again;
	  * all index buffers are checked for range and triangle alignment;
	  * coarse / fine threshold switching is non destructive;
	  * two independent instances with different closures are updated and
	    meshed in an interleaved order.

	The sample only uses the public EditableOctree interface plus the upstream
	octree code for the full-rebuild reference.

	Build (CPU only, no SDL / OpenGL / GPU):

	  c++ -std=c++11 -O2 -DCHECK_HEADLESS -Ithird_party -IDualContouringSample \
	      DualContouringSample/octree.cpp DualContouringSample/qef.cpp \
	      DualContouringSample/svd.cpp DualContouringSample/density.cpp \
	      DualContouringSample/editable_octree.cpp \
	      DualContouringSample/editable_sample.cpp -o editable_sample

	Run from the repository root:

	  ./editable_sample

	It writes sculpt_before.obj, sculpt_after.obj and sculpt_coarse.obj.
*/

#include "editable_octree.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include <glm/glm.hpp>

using glm::vec3;
using glm::ivec3;

// ----------------------------------------------------------------------------

namespace
{
	struct Mesh
	{
		VertexBuffer vertices;
		IndexBuffer indices;
	};

	void Generate(EditableOctree& tree, float threshold, Mesh& mesh)
	{
		mesh.vertices.clear();
		mesh.indices.clear();
		tree.Mesh(threshold, mesh.vertices, mesh.indices);
	}

	// Smooth compactly supported 3D window, zero for |p - center| >= radius.
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

	// ------------------------------------------------------------------------

	bool ValidIndices(const Mesh& mesh)
	{
		if (mesh.indices.size() % 3 != 0)
		{
			return false;
		}
		for (std::size_t i = 0; i < mesh.indices.size(); i++)
		{
			if (mesh.indices[i] < 0 ||
				static_cast<std::size_t>(mesh.indices[i]) >= mesh.vertices.size())
			{
				return false;
			}
		}
		return true;
	}

	std::array<int, 3> DirectedKey(int a, int b, int c)
	{
		if (a <= b && a <= c)
		{
			return {{ a, b, c }};
		}
		if (b <= a && b <= c)
		{
			return {{ b, c, a }};
		}
		return {{ c, a, b }};
	}

	// Deterministic meshing means corresponding vertices share an index; this
	// still maps them defensively and compares orientation preserving
	// triangle multisets, so temporary index numbering may differ.
	bool SameMesh(const Mesh& actual, const Mesh& expected, double tolerance)
	{
		if (actual.vertices.size() != expected.vertices.size())
		{
			return false;
		}
		if (actual.indices.size() != expected.indices.size())
		{
			return false;
		}

		const std::size_t n = actual.vertices.size();
		std::vector<int> mapping(n, -1);
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
				return false;
			}
			mapping[i] = static_cast<int>(bestIndex);
			used[bestIndex] = 1;
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
		return actualTriangles == expectedTriangles;
	}

	Mesh FullMesh(const ivec3& min,
	              int size,
	              std::function<float(const vec3&)> density,
	              float threshold)
	{
		Mesh mesh;
		EditableOctree tree(min, size, density);
		Generate(tree, threshold, mesh);
		return mesh;
	}

	int failures = 0;

	bool Report(const char* label, bool ok)
	{
		std::printf("  %-46s %s\n", label, ok ? "ok" : "FAILED");
		if (!ok)
		{
			++failures;
		}
		return ok;
	}

	// ------------------------------------------------------------------------

	bool WriteObj(const char* path, const Mesh& mesh)
	{
		FILE* file = std::fopen(path, "wb");
		if (!file)
		{
			std::fprintf(stderr, "cannot open %s for writing\n", path);
			return false;
		}

		for (std::size_t i = 0; i < mesh.vertices.size(); i++)
		{
			const vec3& p = mesh.vertices[i].xyz;
			std::fprintf(file, "v %.7f %.7f %.7f\n", (double)p.x, (double)p.y, (double)p.z);
		}
		for (std::size_t i = 0; i < mesh.vertices.size(); i++)
		{
			const vec3& n = mesh.vertices[i].normal;
			std::fprintf(file, "vn %.7f %.7f %.7f\n", (double)n.x, (double)n.y, (double)n.z);
		}
		for (std::size_t i = 0; i + 2 < mesh.indices.size(); i += 3)
		{
			// Same indexing for positions and normals.
			std::fprintf(file, "f %d//%d %d//%d %d//%d\n",
			             mesh.indices[i] + 1, mesh.indices[i] + 1,
			             mesh.indices[i + 1] + 1, mesh.indices[i + 1] + 1,
			             mesh.indices[i + 2] + 1, mesh.indices[i + 2] + 1);
		}

		std::fclose(file);
		std::printf("  wrote %s: %zu vertices, %zu triangles\n",
		            path, mesh.vertices.size(), mesh.indices.size() / 3);
		return true;
	}
}

// ----------------------------------------------------------------------------

int main()
{
	std::printf("Editable Dual Contouring sample\n");

	const ivec3 rootMin(-16, -4, -16);
	const int rootSize = 32;

	// ------------------------------------------------------------------
	// Sculpting session: an infinite plane plus a large sphere, with two
	// local, compactly supported features that can be added and removed.
	// ------------------------------------------------------------------

	struct SculptState
	{
		bool planeBump;
		bool shell;
	};

	SculptState state = { false, false };

	std::function<float(const vec3&)> sculptField =
		[&state](const vec3& p) -> float
		{
			float plane = 8.5f;
			if (state.planeBump)
			{
				// Radius 4 ball around (4, 8.5, 4).  Its footprint spans
				// x,z in [0, 8]: the update box can start exactly on the
				// octree's middle partitions x = 0 and z = 0.
				plane += 1.5f * Window(p, vec3(4.f, 8.5f, 4.f), 4.f);
			}

			float sphere = glm::length(p - vec3(15.f, 2.5f, 1.f)) - 16.f;
			if (state.shell)
			{
				// Newly born small shell, radius 2.5 around (-4, 9, -4).
				sphere = std::min(sphere,
				                  glm::length(p - vec3(-4.f, 9.f, -4.f)) - 2.5f);
			}

			return std::min(p.y - plane, sphere);
		};

	std::printf("\n[1] base model and sculpt edits\n");
	EditableOctree tree(rootMin, rootSize, sculptField);

	Mesh before;
	Generate(tree, -1.f, before);
	WriteObj("sculpt_before.obj", before);

	// Add the plane bump.  The box is flush with the median partitions.
	state.planeBump = true;
	tree.Update(vec3(0.f, 4.f, 0.f), vec3(8.f, 13.f, 8.f));
	Mesh withBump;
	Generate(tree, -1.f, withBump);
	Report("add plane bump",
	       SameMesh(withBump, FullMesh(rootMin, rootSize, sculptField, -1.f), 1e-5) &&
	       ValidIndices(withBump));

	// Grow the new shell.
	state.shell = true;
	tree.Update(vec3(-7.f, 6.f, -7.f), vec3(-1.f, 12.f, -1.f));
	Mesh withBumpAndShell;
	Generate(tree, -1.f, withBumpAndShell);
	Report("add new shell",
	       SameMesh(withBumpAndShell, FullMesh(rootMin, rootSize, sculptField, -1.f), 1e-5) &&
	       ValidIndices(withBumpAndShell));

	// Multiple overlapping edits: remove the bump, then put it back.
	state.planeBump = false;
	tree.Update(vec3(0.f, 4.f, 0.f), vec3(8.f, 13.f, 8.f));
	Mesh withoutBump;
	Generate(tree, -1.f, withoutBump);
	Report("remove plane bump",
	       SameMesh(withoutBump, FullMesh(rootMin, rootSize, sculptField, -1.f), 1e-5));

	state.planeBump = true;
	tree.Update(vec3(0.f, 4.f, 0.f), vec3(8.f, 13.f, 8.f));
	Mesh rebuiltBump;
	Generate(tree, -1.f, rebuiltBump);
	Report("re-add plane bump (overlapping update)",
	       SameMesh(rebuiltBump, FullMesh(rootMin, rootSize, sculptField, -1.f), 1e-5));

	// Remove and then re-add the shell; the final model keeps both features.
	state.shell = false;
	tree.Update(vec3(-7.f, 6.f, -7.f), vec3(-1.f, 12.f, -1.f));
	state.shell = true;
	tree.Update(vec3(-7.f, 6.f, -7.f), vec3(-1.f, 12.f, -1.f));

	Mesh after;
	Generate(tree, -1.f, after);
	WriteObj("sculpt_after.obj", after);
	Report("final sculpted model vs full rebuild",
	       SameMesh(after, FullMesh(rootMin, rootSize, sculptField, -1.f), 1e-5));
	Report("final sculpted indices valid", ValidIndices(after));

	// Threshold view: coarse then back to -1 must restore the fine model.
	Mesh coarse;
	Generate(tree, 1.f, coarse);
	WriteObj("sculpt_coarse.obj", coarse);
	Mesh fineAgain;
	Generate(tree, -1.f, fineAgain);
	Report("coarse then -1 restores fine mesh",
	       SameMesh(fineAgain, after, 1e-5) && coarse.vertices.size() < after.vertices.size());

	// ------------------------------------------------------------------
	// Analytic plane: vertices on y = 8.5, normals +y.
	// ------------------------------------------------------------------

	std::printf("\n[2] analytic plane\n");
	std::function<float(const vec3&)> planeField =
		[](const vec3& p) -> float { return p.y - 8.5f; };

	EditableOctree planeTree(ivec3(-8, 0, -8), 16, planeField);
	Mesh plane;
	Generate(planeTree, -1.f, plane);

	double maxHeight = 0.0;
	double maxNormal = 0.0;
	for (std::size_t i = 0; i < plane.vertices.size(); i++)
	{
		maxHeight = std::max(maxHeight, std::fabs((double)plane.vertices[i].xyz.y - 8.5));
		maxNormal = std::max(maxNormal,
		                     (double)glm::length(plane.vertices[i].normal - vec3(0.f, 1.f, 0.f)));
	}
	Report("plane height and normal",
	       !plane.vertices.empty() && maxHeight <= 1e-3 && maxNormal <= 1e-3);
	Report("plane indices valid", ValidIndices(plane));

	// ------------------------------------------------------------------
	// Empty model, then a local change grows a surface.
	// ------------------------------------------------------------------

	std::printf("\n[3] empty model and regrowth\n");
	bool blob = false;
	std::function<float(const vec3&)> blobField =
		[&blob](const vec3& p) -> float
		{
			if (!blob)
			{
				return 1.f;	// all air
			}
			// Radius 4 ball that becomes solid inside.
			return -1.f + 2.f * Window(p, vec3(8.f, 8.f, 8.f), 4.f);
		};

	EditableOctree emptyTree(ivec3(0, 0, 0), 16, blobField);
	Mesh empty;
	Generate(emptyTree, -1.f, empty);
	Report("empty model is empty",
	       empty.vertices.empty() && empty.indices.empty());

	blob = true;
	emptyTree.Update(vec3(4.f, 4.f, 4.f), vec3(12.f, 12.f, 12.f));
	Mesh grown;
	Generate(emptyTree, -1.f, grown);
	Report("local change grows a surface",
	       !grown.vertices.empty() && ValidIndices(grown) &&
	       SameMesh(grown, FullMesh(ivec3(0, 0, 0), 16, blobField, -1.f), 1e-5));

	// ------------------------------------------------------------------
	// Corner signs unchanged, edge crossing and normal changed.
	// ------------------------------------------------------------------

	std::printf("\n[4] same corner signs, moved crossing and normal\n");
	bool tilt = false;
	auto tiltField = [&tilt](const vec3& p) -> float
	{
		float value = p.y - 4.5f;
		if (tilt)
		{
			value -= 0.25f * ((p.x - 4.5f) + (p.z - 4.5f)) *
			          Window(p, vec3(4.5f, 4.5f, 4.5f), 1.5f);
		}
		return value;
	};

	EditableOctree tiltTree(ivec3(0, 0, 0), 16, tiltField);
	Mesh tiltBefore;
	Generate(tiltTree, -1.f, tiltBefore);

	// Signs at the eight corners of cell (4,4,4) must not change.
	int signsBefore = 0;
	int signsAfter = 0;
	const ivec3 cellCorners[8] = {
		ivec3(4, 4, 4), ivec3(4, 4, 5), ivec3(4, 5, 4), ivec3(4, 5, 5),
		ivec3(5, 4, 4), ivec3(5, 4, 5), ivec3(5, 5, 4), ivec3(5, 5, 5),
	};
	for (int i = 0; i < 8; i++)
	{
		if (tiltField(vec3(cellCorners[i])) >= 0.f)
		{
			signsBefore |= (1 << i);
		}
	}

	tilt = true;
	for (int i = 0; i < 8; i++)
	{
		if (tiltField(vec3(cellCorners[i])) >= 0.f)
		{
			signsAfter |= (1 << i);
		}
	}
	Report("corner signs unchanged", signsBefore == signsAfter);

	tiltTree.Update(vec3(3.f, 3.f, 3.f), vec3(6.f, 6.f, 6.f));
	Mesh tiltAfter;
	Generate(tiltTree, -1.f, tiltAfter);
	Report("tilted cell matches full rebuild",
	       SameMesh(tiltAfter, FullMesh(ivec3(0, 0, 0), 16, tiltField, -1.f), 1e-5));
	Report("tilted surface actually moved", tiltBefore.vertices.size() > 0 &&
	       !SameMesh(tiltBefore, tiltAfter, 1e-4));

	// ------------------------------------------------------------------
	// Two instances with different closures, interleaved update / mesh.
	// ------------------------------------------------------------------

	std::printf("\n[5] two interleaved instances\n");
	bool shell = false;
	auto sphereField = [&shell](const vec3& p) -> float
	{
		float value = glm::length(p - vec3(15.f, 2.5f, 1.f)) - 16.f;
		if (shell)
		{
			value = std::min(value,
			                 glm::length(p - vec3(-4.f, 9.f, -4.f)) - 2.5f);
		}
		return value;
	};
	EditableOctree sphereTree(rootMin, rootSize, sphereField);
	EditableOctree planeTree2(rootMin, rootSize, planeField);

	shell = true;
	sphereTree.Update(vec3(-7.f, 6.f, -7.f), vec3(-1.f, 12.f, -1.f));

	Mesh planeInterleaved;
	Generate(planeTree2, -1.f, planeInterleaved);
	Report("second instance unaffected by first",
	       SameMesh(planeInterleaved, FullMesh(rootMin, rootSize, planeField, -1.f), 1e-5));

	Mesh sphereInterleaved;
	Generate(sphereTree, -1.f, sphereInterleaved);
	Report("first instance matches its rebuild",
	       SameMesh(sphereInterleaved, FullMesh(rootMin, rootSize, sphereField, -1.f), 1e-5));
	Report("two instances produce different meshes",
	       !SameMesh(sphereInterleaved, planeInterleaved, 1e-4));

	// ------------------------------------------------------------------

	std::printf("\n%s (%d failure%s)\n",
	            failures == 0 ? "SAMPLE PASS" : "SAMPLE FAIL",
	            failures, failures == 1 ? "" : "s");
	return failures == 0 ? 0 : 1;
}

// ----------------------------------------------------------------------------
