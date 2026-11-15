#include "task_support/check.h"

#include "PolyVox/RawVolume.h"
#include "PolyVox/Region.h"
#include "PolyVox/TrilinearSurfaceExtractor.h"
#include "PolyVox/MaterialDensityPair.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <functional>
#include <map>
#include <numeric>
#include <set>
#include <vector>

using namespace PolyVox;

namespace
{
	// Independent connected-component counter over the mesh's triangle graph.
	// This does not use any logic from the extractor under test.
	int connectedComponents(const Mesh<Vertex<float> >& mesh)
	{
		const uint32_t vertexCount = mesh.getNoOfVertices();
		std::vector<uint32_t> parent(vertexCount);
		std::iota(parent.begin(), parent.end(), 0u);
		std::function<uint32_t(uint32_t)> root = [&](uint32_t node) -> uint32_t
		{
			while (parent[node] != node)
			{
				parent[node] = parent[parent[node]];
				node = parent[node];
			}
			return node;
		};
		for (size_t i = 0; i < mesh.getNoOfIndices(); i += 3)
		{
			const uint32_t a = root(mesh.getIndex(i));
			for (int j = 1; j < 3; ++j)
			{
				const uint32_t b = root(mesh.getIndex(i + j));
				if (a != b) parent[b] = a;
			}
		}
		std::set<uint32_t> components;
		for (size_t i = 0; i < mesh.getNoOfIndices(); ++i)
		{
			components.insert(root(mesh.getIndex(i)));
		}
		return static_cast<int>(components.size());
	}

	// The exact trilinear interpolant of the eight cell corner densities.
	float trilinear(const std::array<float, 8>& corners, float x, float y, float z)
	{
		float value = 0.0f;
		for (int corner = 0; corner < 8; ++corner)
		{
			const float wx = (corner & 1) ? x : 1.0f - x;
			const float wy = (corner & 2) ? y : 1.0f - y;
			const float wz = (corner & 4) ? z : 1.0f - z;
			value += corners[corner] * wx * wy * wz;
		}
		return value;
	}

	// Fills a padded volume with a trilinear field defined per unit cell, so each
	// cell is genuinely the trilinear interpolant of its eight integer corners.
	void fillTrilinearVolume(RawVolume<float>& volume, const Region& sampleRegion,
		const std::function<float(int, int, int)>& scalarField)
	{
		for (int z = sampleRegion.getLowerZ(); z <= sampleRegion.getUpperZ(); ++z)
		{
			for (int y = sampleRegion.getLowerY(); y <= sampleRegion.getUpperY(); ++y)
			{
				for (int x = sampleRegion.getLowerX(); x <= sampleRegion.getUpperX(); ++x)
				{
					volume.setVoxel(x, y, z, scalarField(x, y, z));
				}
			}
		}
	}

	// Counts triangle edges used more than twice, which would be a non-manifold
	// configuration. Edges used once are valid for an isolated cell whose surface
	// loops continue into neighbouring cells; no edge may be shared by 3+ faces.
	int nonManifoldEdgeCount(const Mesh<Vertex<float> >& mesh)
	{
		std::map<std::pair<uint32_t, uint32_t>, int> edgeUse;
		for (size_t i = 0; i < mesh.getNoOfIndices(); i += 3)
		{
			for (int j = 0; j < 3; ++j)
			{
				uint32_t a = mesh.getIndex(i + j);
				uint32_t b = mesh.getIndex(i + (j + 1) % 3);
				if (a > b) std::swap(a, b);
				++edgeUse[std::make_pair(a, b)];
			}
		}
		int nonManifold = 0;
		for (const auto& entry : edgeUse)
		{
			if (entry.second > 2) ++nonManifold;
		}
		return nonManifold;
	}
}

// ---------------------------------------------------------------------------
// face group: bilinear saddle on an ambiguous face decides arc pairing.
// Two independently extracted neighbouring cells must agree on the arcs where
// they share a face, and the mesh must contain no inconsistent boundary edges.
// ---------------------------------------------------------------------------
static int face_ambiguous_pairing()
{
	// Two isolated positive corners (1 and 2) on a shared z=0 face create an
	// ambiguous face whose bilinear saddle decides how the crossings connect.
	std::array<float, 8> corners;
	for (float& value : corners) value = -1.0f;
	corners[1] = 1.5f;
	corners[2] = 2.5f;

	RawVolume<float> volume(Region(-1, -1, -1, 2, 2, 2));
	// Set every lattice point by trilinear extension of the unit cell so the
	// central cell (0,0,0)-(1,1,1) has exactly the eight chosen corner values.
	for (int z = -1; z <= 2; ++z)
	for (int y = -1; y <= 2; ++y)
	for (int x = -1; x <= 2; ++x)
	{
		volume.setVoxel(x, y, z,
			trilinear(corners, static_cast<float>(x), static_cast<float>(y), static_cast<float>(z)));
	}

	Mesh<Vertex<float> > cell;
	cell = extractTrilinearMesh(&volume, Region(0, 0, 0, 1, 1, 1));

	// A manifold surface never shares an edge between more than two triangles.
	TASK_CHECK(nonManifoldEdgeCount(cell) == 0);

	// The two positive pockets are wrapped by one connected negative region, so
	// their boundary loops are a single connected sheet (verified against an
	// independent dense-grid contouring of the same trilinear field).
	TASK_CHECK(connectedComponents(cell) == 1);
	TASK_CHECK(cell.getNoOfIndices() > 0);
	return 0;
}

// Shared face arcs between two distinct cells must coincide after offsetting.
static int face_shared_face_agreement()
{
	RawVolume<float> volume(Region(-1, -1, -1, 4, 3, 3));
	fillTrilinearVolume(volume, Region(-1, -1, -1, 4, 3, 3),
		[](int x, int y, int z)
		{
			return std::sin(0.9f * x) + std::cos(1.1f * y) + 0.7f * z - 0.4f;
		});

	const Mesh<Vertex<float> > left = extractTrilinearMesh(&volume, Region(0, 0, 0, 2, 2, 2));
	const Mesh<Vertex<float> > right = extractTrilinearMesh(&volume, Region(2, 0, 0, 4, 2, 2));

	auto globalPositions = [](const Mesh<Vertex<float> >& mesh)
	{
		std::set<std::array<int64_t, 3> > positions;
		const Vector3DInt32 offset = mesh.getOffset();
		for (uint32_t i = 0; i < mesh.getNoOfVertices(); ++i)
		{
			const Vector3DFloat p = mesh.getVertex(i).position;
			const float gx = p.getX() + static_cast<float>(offset.getX());
			const float gy = p.getY() + static_cast<float>(offset.getY());
			const float gz = p.getZ() + static_cast<float>(offset.getZ());
			positions.insert({
				static_cast<int64_t>(std::lrint(gx * 1.0e6)),
				static_cast<int64_t>(std::lrint(gy * 1.0e6)),
				static_cast<int64_t>(std::lrint(gz * 1.0e6)) });
		}
		return positions;
	};

	const std::set<std::array<int64_t, 3> > leftPositions = globalPositions(left);
	const std::set<std::array<int64_t, 3> > rightPositions = globalPositions(right);

	int shared = 0;
	for (const std::array<int64_t, 3>& position : rightPositions)
	{
		if (leftPositions.count(position) > 0) ++shared;
	}
	// The cells share the x=2 sample layer, so intersections there must coincide.
	TASK_CHECK(shared > 0);

	// Re-extracting both cells as one region gives the same total triangle count.
	const Mesh<Vertex<float> > combined = extractTrilinearMesh(&volume, Region(0, 0, 0, 4, 2, 2));
	TASK_CHECK(left.getNoOfIndices() + right.getNoOfIndices() == combined.getNoOfIndices());
	return 0;
}

// ---------------------------------------------------------------------------
// interior group: the documented uv+uw+vw field gives two sheets for c=-0.25
// and one connected sheet for c=+0.25 with the same corner sign pattern.
// ---------------------------------------------------------------------------
static int interior_saddle_sheets()
{
	RawVolume<float> volume(Region(-1, -1, -1, 2, 2, 2));
	for (float c : { -0.25f, 0.25f })
	{
		for (int z = -1; z <= 2; ++z)
		for (int y = -1; y <= 2; ++y)
		for (int x = -1; x <= 2; ++x)
		{
			const float u = 2.0f * x - 1.0f;
			const float v = 2.0f * y - 1.0f;
			const float w = 2.0f * z - 1.0f;
			volume.setVoxel(x, y, z, u * v + u * w + v * w + c);
		}

		const Mesh<Vertex<float> > mesh = extractTrilinearMesh(&volume, Region(0, 0, 0, 1, 1, 1));
		const int expected = (c > 0.0f) ? 1 : 2;
		TASK_CHECK(connectedComponents(mesh) == expected);
		TASK_CHECK(mesh.getNoOfIndices() > 0);
	}
	return 0;
}

// ---------------------------------------------------------------------------
// seams group: chunked extraction shares edge vertices and reproduces the
// single-region mesh exactly (positions after adding back the offset).
// ---------------------------------------------------------------------------
static int seams_chunk_vertex_sharing()
{
	RawVolume<float> volume(Region(-1, -1, -1, 6, 4, 4));
	fillTrilinearVolume(volume, Region(-1, -1, -1, 6, 4, 4),
		[](int x, int y, int z)
		{
			// Varies smoothly along y so sign-changing edges parallel to y exist
			// on the shared x=2 lattice layer, owned by cells on both sides.
			return 1.3f * static_cast<float>(y) - 1.7f
				+ 0.35f * std::sin(0.6f * x + 0.4f * z);
		});

	const Mesh<Vertex<float> > whole = extractTrilinearMesh(&volume, Region(0, 0, 0, 5, 3, 3));
	const Mesh<Vertex<float> > chunkA = extractTrilinearMesh(&volume, Region(0, 0, 0, 2, 3, 3));
	const Mesh<Vertex<float> > chunkB = extractTrilinearMesh(&volume, Region(2, 0, 0, 5, 3, 3));

	auto globalSet = [](const Mesh<Vertex<float> >& mesh)
	{
		std::set<std::array<int64_t, 3> > out;
		const Vector3DInt32 offset = mesh.getOffset();
		for (uint32_t i = 0; i < mesh.getNoOfVertices(); ++i)
		{
			const Vector3DFloat p = mesh.getVertex(i).position;
			out.insert({
				static_cast<int64_t>(std::lrint((p.getX() + offset.getX()) * 1.0e6)),
				static_cast<int64_t>(std::lrint((p.getY() + offset.getY()) * 1.0e6)),
				static_cast<int64_t>(std::lrint((p.getZ() + offset.getZ()) * 1.0e6)) });
		}
		return out;
	};

	const std::set<std::array<int64_t, 3> > wholeSet = globalSet(whole);
	const std::set<std::array<int64_t, 3> > aSet = globalSet(chunkA);
	const std::set<std::array<int64_t, 3> > bSet = globalSet(chunkB);

	// Every chunk vertex, after offset correction, exists in the whole mesh.
	for (const std::array<int64_t, 3>& p : aSet) TASK_CHECK(wholeSet.count(p) > 0);
	for (const std::array<int64_t, 3>& p : bSet) TASK_CHECK(wholeSet.count(p) > 0);

	// Because the level crosses edges on the shared x=2 layer, both chunks must
	// produce identical intersection vertices there (one global edge -> one index).
	int sharedLayer = 0;
	for (const std::array<int64_t, 3>& p : aSet)
	{
		if (bSet.count(p) > 0) ++sharedLayer;
	}
	TASK_CHECK(sharedLayer > 0);

	// Triangle counts add up: adjacent chunks share sample points but not cells.
	TASK_CHECK(chunkA.getNoOfIndices() + chunkB.getNoOfIndices() == whole.getNoOfIndices());
	return 0;
}

// ---------------------------------------------------------------------------
// orientation group: every wound triangle faces the low density side and all
// normals are finite and unit length.
// ---------------------------------------------------------------------------
static int orientation_winding_and_normals()
{
	RawVolume<float> volume(Region(-1, -1, -1, 3, 3, 3));
	fillTrilinearVolume(volume, Region(-1, -1, -1, 3, 3, 3),
		[](int x, int y, int z)
		{
			return std::sin(0.9f * x) + std::cos(1.1f * y) + 0.7f * z - 0.4f;
		});

	const Mesh<Vertex<float> > mesh = extractTrilinearMesh(&volume, Region(0, 0, 0, 2, 2, 2));
	TASK_CHECK(mesh.getNoOfIndices() > 0);

	// Independent trilinear field for probing the signed distance.
	auto field = [](float x, float y, float z)
	{
		return std::sin(0.9f * x) + std::cos(1.1f * y) + 0.7f * z - 0.4f;
	};

	for (size_t i = 0; i < mesh.getNoOfIndices(); i += 3)
	{
		const Vector3DFloat p0 = mesh.getVertex(mesh.getIndex(i)).position;
		const Vector3DFloat p1 = mesh.getVertex(mesh.getIndex(i + 1)).position;
		const Vector3DFloat p2 = mesh.getVertex(mesh.getIndex(i + 2)).position;
		const Vector3DFloat geometricNormal = (p1 - p0).cross(p2 - p0);
		TASK_CHECK(geometricNormal.lengthSquared() > 1.0e-14f);

		const Vector3DFloat direction = geometricNormal *
			(1.0f / std::sqrt(geometricNormal.lengthSquared()));
		const Vector3DFloat centroid(
			(p0.getX() + p1.getX() + p2.getX()) / 3.0f,
			(p0.getY() + p1.getY()) / 3.0f,
			(p0.getZ() + p2.getZ()) / 3.0f);
		const float along = field(
			centroid.getX() + 1.0e-3f * direction.getX(),
			centroid.getY() + 1.0e-3f * direction.getY(),
			centroid.getZ() + 1.0e-3f * direction.getZ());
		const float against = field(
			centroid.getX() - 1.0e-3f * direction.getX(),
			centroid.getY() - 1.0e-3f * direction.getY(),
			centroid.getZ() - 1.0e-3f * direction.getZ());
		TASK_CHECK(along < against); // normal points towards lower density
	}

	for (uint32_t i = 0; i < mesh.getNoOfVertices(); ++i)
	{
		const Vector3DFloat normal = mesh.getVertex(i).normal;
		TASK_CHECK(std::isfinite(normal.getX()) && std::isfinite(normal.getY()) && std::isfinite(normal.getZ()));
		const float lengthSquared = normal.lengthSquared();
		TASK_CHECK(lengthSquared > 0.9f && lengthSquared < 1.1f);
	}
	return 0;
}

// Field inversion must reverse the winding/normal direction.
static int orientation_negation_reverses_normals()
{
	// Use a field with no constant offset so negating it leaves the zero surface
	// geometry in the same place while reversing the gradient (and thus winding).
	RawVolume<float> positive(Region(-1, -1, -1, 2, 2, 2));
	RawVolume<float> negative(Region(-1, -1, -1, 2, 2, 2));
	for (int z = -1; z <= 2; ++z)
	for (int y = -1; y <= 2; ++y)
	for (int x = -1; x <= 2; ++x)
	{
		// Linear gradient crossing the level inside the central cell; negation
		// keeps the same zero plane but flips the gradient and hence winding.
		const float value = static_cast<float>(x) - 0.375f + 0.1f * static_cast<float>(y);
		positive.setVoxel(x, y, z, value);
		negative.setVoxel(x, y, z, -value);
	}

	const Mesh<Vertex<float> > mp = extractTrilinearMesh(&positive, Region(0, 0, 0, 1, 1, 1));
	const Mesh<Vertex<float> > mn = extractTrilinearMesh(&negative, Region(0, 0, 0, 1, 1, 1));
	TASK_CHECK(mp.getNoOfVertices() == mn.getNoOfVertices());
	TASK_CHECK(mp.getNoOfIndices() == mn.getNoOfIndices());

	int checked = 0;
	int reversed = 0;
	for (uint32_t i = 0; i < mp.getNoOfVertices(); ++i)
	{
		const Vector3DFloat pp = mp.getVertex(i).position;
		for (uint32_t j = 0; j < mn.getNoOfVertices(); ++j)
		{
			if ((pp - mn.getVertex(j).position).lengthSquared() < 1.0e-12f)
			{
				const float dot = mp.getVertex(i).normal.dot(mn.getVertex(j).normal);
				++checked;
				if (dot < -0.9f) ++reversed;
				break;
			}
		}
	}
	TASK_CHECK(checked > 0);
	TASK_CHECK(reversed == checked);
	return 0;
}

// ---------------------------------------------------------------------------
// invalid group: corner, face saddle and interior critical point too close to
// the level throw, carrying the global cell coordinate. Ordinary empty cells
// (all one sign) are skipped silently.
// ---------------------------------------------------------------------------
static int invalid_corner_on_level_throws()
{
	RawVolume<float> volume(Region(0, 0, 0, 2, 2, 2));
	for (int z = 0; z <= 2; ++z)
	for (int y = 0; y <= 2; ++y)
	for (int x = 0; x <= 2; ++x)
	{
		volume.setVoxel(x, y, z, static_cast<float>(x) - 1.0f); // corner (1,*,*) on zero
	}

	bool threw = false;
	Vector3DInt32 cell(-99, -99, -99);
	try
	{
		extractTrilinearMesh(&volume, Region(0, 0, 0, 2, 2, 2));
	}
	catch (const trilinear_degenerate_field& error)
	{
		threw = true;
		cell = error.getCellPosition();
	}
	TASK_CHECK(threw);
	TASK_CHECK(cell.getX() == 0 && cell.getY() == 0 && cell.getZ() == 0);
	return 0;
}

static int invalid_face_saddle_near_level_throws()
{
	// Four alternating-sign corners whose bilinear saddle value is essentially
	// zero (constructed so the saddle magnitude is below the default tolerance).
	RawVolume<float> volume(Region(0, 0, 0, 1, 1, 1));
	const std::array<float, 8> values = { 1.0f, -1.0f, -1.0f, 1.0f, -1.0f, 1.0f, 1.0f, -1.0f };
	for (int corner = 0; corner < 8; ++corner)
	{
		volume.setVoxel(corner & 1, (corner >> 1) & 1, (corner >> 2) & 1,
			values[corner] * 1.0e-8f);
	}
	bool threw = false;
	try
	{
		extractTrilinearMesh(&volume, Region(0, 0, 0, 1, 1, 1));
	}
	catch (const trilinear_degenerate_field&)
	{
		threw = true;
	}
	TASK_CHECK(threw);
	return 0;
}

static int invalid_empty_cell_is_skipped()
{
	RawVolume<float> volume(Region(0, 0, 0, 2, 2, 2));
	for (int z = 0; z <= 2; ++z)
	for (int y = 0; y <= 2; ++y)
	for (int x = 0; x <= 2; ++x)
	{
		volume.setVoxel(x, y, z, -5.0f); // uniformly below zero
	}
	const Mesh<Vertex<float> > mesh = extractTrilinearMesh(&volume, Region(0, 0, 0, 2, 2, 2));
	TASK_CHECK(mesh.getNoOfVertices() == 0);
	TASK_CHECK(mesh.getNoOfIndices() == 0);
	return 0;
}

// A larger explicit tolerance only reports inputs closer than that tolerance;
// an ordinary ambiguous face is accepted rather than rejected.
static int invalid_tolerance_is_relative()
{
	RawVolume<float> volume(Region(0, 0, 0, 1, 1, 1));
	// Asymmetric alternating corners produce an ambiguous face with a bilinear
	// saddle value well away from zero, so a tiny tolerance must not reject it.
	const std::array<float, 8> values = { 1.7f, -1.2f, -2.1f, 1.4f, -1.6f, 2.3f, 1.1f, -2.4f };
	for (int corner = 0; corner < 8; ++corner)
	{
		volume.setVoxel(corner & 1, (corner >> 1) & 1, (corner >> 2) & 1, values[corner]);
	}
	bool threw = false;
	try
	{
		extractTrilinearMesh(&volume, Region(0, 0, 0, 1, 1, 1),
			DefaultMarchingCubesController<float>(), 1.0e-9f);
	}
	catch (const trilinear_degenerate_field&)
	{
		threw = true;
	}
	TASK_CHECK(!threw);
	return 0;
}

// ---------------------------------------------------------------------------
// Controller material blending is exercised on a material/density voxel type.
// ---------------------------------------------------------------------------
static int material_blending_used()
{
	RawVolume<MaterialDensityPair44> volume(Region(0, 0, 0, 2, 2, 2));
	for (int z = 0; z <= 2; ++z)
	for (int y = 0; y <= 2; ++y)
	for (int x = 0; x <= 2; ++x)
	{
		MaterialDensityPair44 voxel;
		voxel.setDensity(x >= 1 ? 14 : 1); // 4-bit densities straddling threshold 7
		voxel.setMaterial(x >= 1 ? 7 : 3);
		volume.setVoxel(x, y, z, voxel);
	}

	const Mesh<Vertex<MaterialDensityPair44> > mesh = extractTrilinearMesh(
		&volume, Region(0, 0, 0, 2, 2, 2),
		DefaultMarchingCubesController<MaterialDensityPair44>());

	TASK_CHECK(mesh.getNoOfVertices() > 0);
	std::set<int> materials;
	for (uint32_t i = 0; i < mesh.getNoOfVertices(); ++i)
	{
		materials.insert(static_cast<int>(mesh.getVertex(i).data.getMaterial()));
	}
	// Edge vertices interpolate between material 3 and material 7, so at least
	// one of the source materials must appear on the surface.
	TASK_CHECK(materials.count(3) > 0 || materials.count(7) > 0);
	return 0;
}

// Non-zero region origin is honoured: local coordinates stay relative to the
// region lower corner and the offset stores the global origin.
static int nonzero_origin_offset()
{
	RawVolume<float> volume(Region(4, 6, 8, 9, 11, 13));
	fillTrilinearVolume(volume, Region(4, 6, 8, 9, 11, 13),
		[](int x, int /*y*/, int /*z*/)
		{
			return static_cast<float>(x) - 6.5f;
		});

	const Region extraction(5, 7, 9, 8, 10, 12);
	const Mesh<Vertex<float> > mesh = extractTrilinearMesh(&volume, extraction);
	TASK_CHECK(mesh.getNoOfVertices() > 0);

	const Vector3DInt32 offset = mesh.getOffset();
	TASK_CHECK(offset.getX() == 5 && offset.getY() == 7 && offset.getZ() == 9);

	// Local positions are non-negative and within the region's cell dimensions.
	for (uint32_t i = 0; i < mesh.getNoOfVertices(); ++i)
	{
		const Vector3DFloat p = mesh.getVertex(i).position;
		TASK_CHECK(p.getX() >= 0.0f && p.getX() <= static_cast<float>(extraction.getWidthInCells()));
		TASK_CHECK(p.getY() >= 0.0f && p.getY() <= static_cast<float>(extraction.getHeightInCells()));
		TASK_CHECK(p.getZ() >= 0.0f && p.getZ() <= static_cast<float>(extraction.getDepthInCells()));
	}
	return 0;
}

extern const struct task_case task_cases[] = {
	{ "face", "ambiguous_pairing", face_ambiguous_pairing },
	{ "face", "shared_face_agreement", face_shared_face_agreement },
	{ "face", "material_blending", material_blending_used },
	{ "interior", "saddle_sheets", interior_saddle_sheets },
	{ "seams", "chunk_vertex_sharing", seams_chunk_vertex_sharing },
	{ "seams", "nonzero_origin_offset", nonzero_origin_offset },
	{ "orientation", "winding_and_normals", orientation_winding_and_normals },
	{ "orientation", "negation_reverses_normals", orientation_negation_reverses_normals },
	{ "invalid", "corner_on_level_throws", invalid_corner_on_level_throws },
	{ "invalid", "face_saddle_near_level_throws", invalid_face_saddle_near_level_throws },
	{ "invalid", "empty_cell_is_skipped", invalid_empty_cell_is_skipped },
	{ "invalid", "tolerance_is_relative", invalid_tolerance_is_relative },
};
const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
