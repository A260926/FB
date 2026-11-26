#include "task_support/check.h"
#include "source/xatlas/xatlas.h"

#include <cmath>
#include <array>
#include <cstdint>
#include <cstdio>
#include <vector>

using namespace xatlas;

namespace {

constexpr float kGeomEps = 1e-5f;

struct GeoMesh {
	uint32_t nx = 1, ny = 1;
	bool ring = false;
	bool bend = false;
	std::vector<float> positions;
	std::vector<uint32_t> indices;
	std::vector<uint32_t> materials;
	uint32_t faceCount() const { return uint32_t(indices.size() / 3); }
};

static void addVertex(GeoMesh &m, float x, float y, float z)
{
	m.positions.push_back(x);
	m.positions.push_back(y);
	m.positions.push_back(z);
}

static GeoMesh makeGrid(uint32_t nx, uint32_t ny, bool bend = false, bool ring = false)
{
	GeoMesh m;
	m.nx = nx;
	m.ny = ny;
	m.bend = bend;
	m.ring = ring;
	const uint32_t vx = nx + 1, vy = ny + 1;
	for (uint32_t j = 0; j <= ny; j++) {
		for (uint32_t i = 0; i <= nx; i++) {
			float x = float(i), y = float(j) * 0.45f, z = 0.0f;
			if (bend) {
				const float t = -2.15f + 4.3f * float(i) / float(nx);
				const float r = 2.0f;
				x = r * sinf(t);
				z = r * (cosf(t) - 1.0f);
				y = float(j) * 0.38f;
			}
			addVertex(m, x, y, z);
		}
	}
	for (uint32_t j = 0; j < ny; j++) {
		for (uint32_t i = 0; i < nx; i++) {
			if (ring && nx >= 3 && ny >= 3 && i == nx / 2 && j == ny / 2)
				continue; // Omit the middle cell: an annulus rather than a disk.
			uint32_t a = j * vx + i;
			uint32_t b = a + 1;
			uint32_t c = a + vx;
			uint32_t d = c + 1;
			m.indices.push_back(a); m.indices.push_back(b); m.indices.push_back(c);
			m.indices.push_back(b); m.indices.push_back(d); m.indices.push_back(c);
			m.materials.push_back(0);
			m.materials.push_back(0);
		}
	}
	return m;
}

static AddMeshError addGeoMesh(Atlas *atlas, const GeoMesh &m, bool materials = false)
{
	MeshDecl d;
	d.vertexPositionData = m.positions.data();
	d.vertexCount = uint32_t(m.positions.size() / 3);
	d.vertexPositionStride = 12;
	d.indexData = m.indices.data();
	d.indexCount = uint32_t(m.indices.size());
	d.indexFormat = IndexFormat::UInt32;
	if (materials)
		d.faceMaterialData = m.materials.data();
	return AddMesh(atlas, d);
}

static int32_t oppositeCorner(const std::vector<uint32_t> &idx, uint32_t faceCount, uint32_t corner)
{
	uint32_t f = corner / 3;
	uint32_t c = corner % 3;
	uint32_t a = idx[f * 3 + c];
	uint32_t b = idx[f * 3 + (c + 1) % 3];
	for (uint32_t g = 0; g < faceCount; g++) {
		for (uint32_t k = 0; k < 3; k++) {
			uint32_t ga = idx[g * 3 + k];
			uint32_t gb = idx[g * 3 + (k + 1) % 3];
			if (ga == b && gb == a)
				return int32_t(g * 3 + k);
		}
	}
	return -1;
}

static std::vector<uint8_t> allSharedJoins(const GeoMesh &m)
{
	std::vector<uint8_t> flags(m.faceCount() * 3, 0);
	for (uint32_t c = 0; c < flags.size(); c++) {
		int32_t o = oppositeCorner(m.indices, m.faceCount(), c);
		if (o >= 0) {
			flags[c] = 2;
			flags[o] = 2;
		}
	}
	return flags;
}

static void setEdgeBoth(std::vector<uint8_t> &flags, const GeoMesh &m, uint32_t face, uint32_t corner, uint8_t value)
{
	uint32_t c = face * 3 + corner;
	flags[c] = value;
	int32_t o = oppositeCorner(m.indices, m.faceCount(), c);
	if (o >= 0)
		flags[uint32_t(o)] = value;
}

static bool setConstraints(Atlas *a, uint32_t mesh, const std::vector<uint8_t> &flags)
{
	return SetMeshChartConstraints(a, mesh, flags.empty() ? nullptr : flags.data(), uint32_t(flags.size()));
}

static PackOptions testPackOptions()
{
	PackOptions po;
	po.resolution = 512;
	po.texelsPerUnit = 16.0f;
	po.padding = 2;
	po.bilinear = false;
	po.bruteForce = false;
	return po;
}

static ChartOptions testChartOptions(uint32_t iterations = 1)
{
	ChartOptions co;
	co.maxIterations = iterations;
	return co;
}

struct UV { float x, y; };

static UV cornerUV(const Mesh &m, uint32_t face, uint32_t corner)
{
	uint32_t vi = m.indexArray[face * 3 + corner];
	return UV{m.vertexArray[vi].uv[0], m.vertexArray[vi].uv[1]};
}

static float cross2(UV a, UV b, UV c)
{
	return (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
}

static float orient(UV a, UV b, UV c)
{
	return cross2(a, b, c);
}

static bool strictlyInside(UV p, UV a, UV b, UV c)
{
	float o1 = orient(a, b, p);
	float o2 = orient(b, c, p);
	float o3 = orient(c, a, p);
	bool pos = o1 > kGeomEps && o2 > kGeomEps && o3 > kGeomEps;
	bool neg = o1 < -kGeomEps && o2 < -kGeomEps && o3 < -kGeomEps;
	return pos || neg;
}

static bool properIntersection(UV a, UV b, UV c, UV d)
{
	float o1 = orient(a, b, c), o2 = orient(a, b, d);
	float o3 = orient(c, d, a), o4 = orient(c, d, b);
	return ((o1 > kGeomEps && o2 < -kGeomEps) || (o1 < -kGeomEps && o2 > kGeomEps)) &&
	       ((o3 > kGeomEps && o4 < -kGeomEps) || (o3 < -kGeomEps && o4 > kGeomEps));
}

static bool shareInputEdge(const std::vector<uint32_t> &idx, uint32_t f, uint32_t g)
{
	for (uint32_t i = 0; i < 3; i++) {
		uint32_t a = idx[f * 3 + i], b = idx[f * 3 + (i + 1) % 3];
		for (uint32_t j = 0; j < 3; j++) {
			uint32_t c = idx[g * 3 + j], d = idx[g * 3 + (j + 1) % 3];
			if ((a == c && b == d) || (a == d && b == c))
				return true;
		}
	}
	return false;
}

static bool validateChartGeometry(const Mesh &out, const std::vector<uint32_t> &idx, uint32_t localChart)
{
	const Chart &chart = out.chartArray[localChart];
	std::vector<std::array<UV, 3>> tris;
	tris.reserve(chart.faceCount);
	int expectedSign = 0;
	for (uint32_t i = 0; i < chart.faceCount; i++) {
		uint32_t f = chart.faceArray[i];
		if (f >= idx.size() / 3)
			return false;
		UV a = cornerUV(out, f, 0), b = cornerUV(out, f, 1), c = cornerUV(out, f, 2);
		float area2 = cross2(a, b, c);
		if (!std::isfinite(area2) || std::fabs(area2) <= 1e-10f)
			return false; // Zero-area/degenerate UV triangle.
		int sign = area2 > 0 ? 1 : -1;
		if (!expectedSign)
			expectedSign = sign;
		else if (sign != expectedSign)
			return false; // Inconsistent winding inside one chart.
		tris.push_back(std::array<UV, 3>{{a, b, c}});
	}
	for (uint32_t i = 0; i < chart.faceCount; i++) {
		uint32_t fi = chart.faceArray[i];
		for (uint32_t j = i + 1; j < chart.faceCount; j++) {
			uint32_t gj = chart.faceArray[j];
			bool adjacent = shareInputEdge(idx, fi, gj);
			const auto &A = tris[i];
			const auto &B = tris[j];
			// Crossing edges are never valid overlap, including adjacent folds.
			for (int e = 0; e < 3; e++) {
				UV a = A[e], b = A[(e + 1) % 3];
				for (int q = 0; q < 3; q++) {
					UV c = B[q], d = B[(q + 1) % 3];
					if (properIntersection(a, b, c, d))
						return false;
				}
			}
			// A vertex strictly inside the other triangle means area overlap.
			for (int v = 0; v < 3; v++) {
				if (strictlyInside(A[v], B[0], B[1], B[2]))
					return false;
				if (strictlyInside(B[v], A[0], A[1], A[2]))
					return false;
			}
			(void)adjacent; // Shared-edge endpoint contact is tolerated by the predicates.
		}
	}
	return true;
}

static bool validateAtlasResult(Atlas *a, const std::vector<const GeoMesh *> &meshes)
{
	if (!a || a->meshCount != meshes.size())
		return false;
	uint32_t globalChartOffset = 0;
	uint32_t globalChartTotal = 0;
	for (uint32_t mi = 0; mi < meshes.size(); mi++) {
		const GeoMesh &input = *meshes[mi];
		const Mesh &out = a->meshes[mi];
		const uint32_t F = input.faceCount();
		if (out.indexCount != F * 3 || !out.indexArray || !out.vertexArray || !out.chartArray)
			return false;
		if (out.chartCount == 0)
			return false;
		std::vector<int> faceChart(F, -1);
		for (uint32_t c = 0; c < out.chartCount; c++) {
			const Chart &ch = out.chartArray[c];
			if (!ch.faceArray || ch.faceCount == 0)
				return false;
			if (int(ch.atlasIndex) < 0)
				return false;
			for (uint32_t i = 0; i < ch.faceCount; i++) {
				uint32_t f = ch.faceArray[i];
				if (f >= F || faceChart[f] != -1)
					return false; // Every valid source face exactly once.
				faceChart[f] = int(c);
			}
			if (!validateChartGeometry(out, input.indices, c))
				return false;
		}
		for (uint32_t f = 0; f < F; f++) {
			if (faceChart[f] < 0)
				return false;
			for (uint32_t k = 0; k < 3; k++) {
				uint32_t vi = out.indexArray[f * 3 + k];
				if (vi >= out.vertexCount)
					return false;
				const Vertex &v = out.vertexArray[vi];
				if (v.xref != input.indices[f * 3 + k])
					return false; // Corner xref returns to original AddMesh index.
				if (v.chartIndex != int32_t(globalChartOffset + uint32_t(faceChart[f])))
					return false; // Global chart numbering.
				if (v.atlasIndex != int32_t(out.chartArray[faceChart[f]].atlasIndex))
					return false; // Page and UV belong to the same packed chart.
				if (!std::isfinite(v.uv[0]) || !std::isfinite(v.uv[1]))
					return false;
			}
		}
		globalChartOffset += out.chartCount;
		globalChartTotal += out.chartCount;
	}
	return a->chartCount == globalChartTotal;
}

static bool validateHardFlags(Atlas *a, uint32_t meshIndex, const GeoMesh &m, const std::vector<uint8_t> &flags)
{
	const Mesh &out = a->meshes[meshIndex];
	std::vector<int> faceChart(m.faceCount(), -1);
	for (uint32_t c = 0; c < out.chartCount; c++)
		for (uint32_t i = 0; i < out.chartArray[c].faceCount; i++)
			faceChart[out.chartArray[c].faceArray[i]] = int(c);
	for (uint32_t corner = 0; corner < flags.size(); corner++) {
		if (flags[corner] == 0)
			continue;
		int32_t o = oppositeCorner(m.indices, m.faceCount(), corner);
		if (o < 0) {
			if (flags[corner] == 2)
				return false;
			continue; // Boundary cut is legal and already open.
		}
		uint32_t f = corner / 3, g = uint32_t(o) / 3;
		if (faceChart[f] < 0 || faceChart[g] < 0)
			return false;
		if (flags[corner] == 2 && faceChart[f] != faceChart[g])
			return false;
		if (flags[corner] == 1 && faceChart[f] == faceChart[g])
			return false;
	}
	return true;
}

static bool constrainedPack(Atlas *a, const ChartOptions &co = testChartOptions(), const PackOptions &po = testPackOptions())
{
	if (!ComputeChartsConstrained(a, co))
		return false;
	PackCharts(a, po);
	return a->meshCount > 0 && a->meshes[0].chartCount > 0;
}

// ---------------------------------------------------------------------------
// grouping
// ---------------------------------------------------------------------------

static int case_transitive_join()
{
	SetPrint(nullptr, false);
	GeoMesh m = makeGrid(4, 3);
	Atlas *a = Create();
	TASK_CHECK(addGeoMesh(a, m) == AddMeshError::Success);
	std::vector<uint8_t> flags = allSharedJoins(m);
	TASK_CHECK(setConstraints(a, 0, flags));
	TASK_CHECK(ComputeChartsConstrained(a));
	PackCharts(a, testPackOptions());
	std::vector<const GeoMesh *> meshes; meshes.push_back(&m);
	TASK_CHECK(validateAtlasResult(a, meshes));
	TASK_CHECK(validateHardFlags(a, 0, m, flags));
	TASK_CHECK(a->meshes[0].chartCount == 1);
	Destroy(a);
	return 0;
}

static int case_cut_alternative_path_conflict()
{
	SetPrint(nullptr, false);
	GeoMesh m = makeGrid(3, 3);
	Atlas *a = Create();
	TASK_CHECK(addGeoMesh(a, m) == AddMeshError::Success);
	std::vector<uint8_t> flags = allSharedJoins(m);
	// Vertical edge between two central cells. The grid has a path around it.
	uint32_t cutFace = 2 * (1 * m.nx + 1) + 1;
	setEdgeBoth(flags, m, cutFace, 0, 1);
	int32_t cutOpposite = oppositeCorner(m.indices, m.faceCount(), cutFace * 3 + 0);
	TASK_CHECK(cutOpposite >= 0);
	flags[uint32_t(cutOpposite)] = 0;
	TASK_CHECK(SetMeshChartConstraints(a, 0, flags.data(), uint32_t(flags.size())));
	TASK_CHECK(!ComputeChartsConstrained(a));
	Destroy(a);
	return 0;
}

static int case_non_disk_join_rejected()
{
	SetPrint(nullptr, false);
	GeoMesh m = makeGrid(3, 3, false, true);
	TASK_CHECK(m.faceCount() == 16); // Eight ring cells.
	Atlas *a = Create();
	TASK_CHECK(addGeoMesh(a, m) == AddMeshError::Success);
	std::vector<uint8_t> flags = allSharedJoins(m);
	TASK_CHECK(setConstraints(a, 0, flags));
	TASK_CHECK(!ComputeChartsConstrained(a));
	Destroy(a);
	return 0;
}

static int case_planar_fast_path()
{
	SetPrint(nullptr, false);
	GeoMesh m = makeGrid(5, 1);
	Atlas *a = Create();
	TASK_CHECK(addGeoMesh(a, m) == AddMeshError::Success);
	std::vector<uint8_t> flags(m.faceCount() * 3, 0);
	setEdgeBoth(flags, m, 0, 1, 2); // Join across the first quad diagonal.
	TASK_CHECK(setConstraints(a, 0, flags));
	TASK_CHECK(ComputeChartsConstrained(a));
	PackCharts(a, testPackOptions());
	std::vector<const GeoMesh *> meshes; meshes.push_back(&m);
	TASK_CHECK(validateAtlasResult(a, meshes));
	TASK_CHECK(validateHardFlags(a, 0, m, flags));
	TASK_CHECK(a->meshes[0].chartCount == 1);
	TASK_CHECK(a->meshes[0].chartArray[0].type == ChartType::Planar);
	Destroy(a);
	return 0;
}

static int case_multi_reseed()
{
	SetPrint(nullptr, false);
	GeoMesh m = makeGrid(7, 4);
	Atlas *a = Create();
	TASK_CHECK(addGeoMesh(a, m) == AddMeshError::Success);
	std::vector<uint8_t> flags(m.faceCount() * 3, 0);
	// Join each horizontal row into a long component; exercise repeated reseeding.
	for (uint32_t j = 0; j < m.ny; j++)
		for (uint32_t i = 0; i + 1 < m.nx; i++)
			setEdgeBoth(flags, m, 2 * (j * m.nx + i) + 1, 0, 2);
	TASK_CHECK(setConstraints(a, 0, flags));
	ChartOptions co = testChartOptions(4);
	TASK_CHECK(ComputeChartsConstrained(a, co));
	PackCharts(a, testPackOptions());
	std::vector<const GeoMesh *> meshes; meshes.push_back(&m);
	TASK_CHECK(validateAtlasResult(a, meshes));
	TASK_CHECK(validateHardFlags(a, 0, m, flags));
	Destroy(a);
	return 0;
}

static int case_material_conflict()
{
	SetPrint(nullptr, false);
	GeoMesh m = makeGrid(1, 1);
	m.materials[0] = 0;
	m.materials[1] = 7;
	Atlas *a = Create();
	TASK_CHECK(addGeoMesh(a, m, true) == AddMeshError::Success);
	std::vector<uint8_t> flags(m.faceCount() * 3, 0);
	setEdgeBoth(flags, m, 0, 1, 2); // Shared diagonal.
	TASK_CHECK(setConstraints(a, 0, flags));
	TASK_CHECK(!ComputeChartsConstrained(a));
	Destroy(a);
	return 0;
}

static int case_multi_mesh_numbering()
{
	SetPrint(nullptr, false);
	GeoMesh m0 = makeGrid(2, 1);
	GeoMesh m1 = makeGrid(2, 2);
	Atlas *a = Create();
	TASK_CHECK(addGeoMesh(a, m0) == AddMeshError::Success);
	TASK_CHECK(addGeoMesh(a, m1) == AddMeshError::Success);
	std::vector<uint8_t> f0(m0.faceCount() * 3, 0);
	setEdgeBoth(f0, m0, 1, 1, 1); // Cut the only quad diagonal? This separates its two tris.
	std::vector<uint8_t> f1 = allSharedJoins(m1);
	TASK_CHECK(SetMeshChartConstraints(a, 0, f0.data(), uint32_t(f0.size())));
	TASK_CHECK(SetMeshChartConstraints(a, 1, f1.data(), uint32_t(f1.size())));
	TASK_CHECK(ComputeChartsConstrained(a));
	PackCharts(a, testPackOptions());
	std::vector<const GeoMesh *> meshes;
	meshes.push_back(&m0);
	meshes.push_back(&m1);
	TASK_CHECK(validateAtlasResult(a, meshes));
	TASK_CHECK(validateHardFlags(a, 0, m0, f0));
	TASK_CHECK(validateHardFlags(a, 1, m1, f1));
	TASK_CHECK(a->chartCount == a->meshes[0].chartCount + a->meshes[1].chartCount);
	Destroy(a);
	return 0;
}

static int case_input_release()
{
	SetPrint(nullptr, false);
	GeoMesh m = makeGrid(3, 2);
	Atlas *a = Create();
	TASK_CHECK(addGeoMesh(a, m) == AddMeshError::Success);
	{
		std::vector<uint8_t> flags = allSharedJoins(m);
		TASK_CHECK(SetMeshChartConstraints(a, 0, flags.data(), uint32_t(flags.size())));
	}
	TASK_CHECK(ComputeChartsConstrained(a));
	PackCharts(a, testPackOptions());
	std::vector<const GeoMesh *> meshes; meshes.push_back(&m);
	TASK_CHECK(validateAtlasResult(a, meshes));
	TASK_CHECK(a->meshes[0].chartCount == 1);
	Destroy(a);
	return 0;
}

// ---------------------------------------------------------------------------
// parameterization
// ---------------------------------------------------------------------------

static int case_bent_disk_fallback()
{
	SetPrint(nullptr, false);
	GeoMesh m = makeGrid(9, 5, true);
	Atlas *a = Create();
	TASK_CHECK(addGeoMesh(a, m) == AddMeshError::Success);
	std::vector<uint8_t> flags = allSharedJoins(m);
	TASK_CHECK(setConstraints(a, 0, flags));
	ChartOptions co = testChartOptions(2);
	TASK_CHECK(ComputeChartsConstrained(a, co));
	PackOptions po = testPackOptions();
	po.texelsPerUnit = 8.0f;
	PackCharts(a, po);
	std::vector<const GeoMesh *> meshes; meshes.push_back(&m);
	TASK_CHECK(validateAtlasResult(a, meshes));
	TASK_CHECK(validateHardFlags(a, 0, m, flags));
	TASK_CHECK(a->meshes[0].chartCount == 1);
	Destroy(a);
	return 0;
}

static int case_free_region_not_one_face_island()
{
	SetPrint(nullptr, false);
	GeoMesh m = makeGrid(6, 1);
	Atlas *a = Create();
	TASK_CHECK(addGeoMesh(a, m) == AddMeshError::Success);
	TASK_CHECK(ComputeChartsConstrained(a));
	PackCharts(a, testPackOptions());
	std::vector<const GeoMesh *> meshes; meshes.push_back(&m);
	TASK_CHECK(validateAtlasResult(a, meshes));
	// A coplanar connected strip must use the planar fast path as one chart.
	TASK_CHECK(a->meshes[0].chartCount == 1);
	Destroy(a);
	return 0;
}

// ---------------------------------------------------------------------------
// lifecycle
// ---------------------------------------------------------------------------

static int case_failed_compute_repack()
{
	SetPrint(nullptr, false);
	GeoMesh m = makeGrid(2, 2);
	Atlas *a = Create();
	TASK_CHECK(addGeoMesh(a, m) == AddMeshError::Success);
	std::vector<uint8_t> good = allSharedJoins(m);
	TASK_CHECK(setConstraints(a, 0, good));
	TASK_CHECK(constrainedPack(a));
	TASK_CHECK(a->meshes[0].chartCount == 1);
	uint32_t oldCharts = a->chartCount;

	std::vector<uint8_t> bad = allSharedJoins(m);
	uint32_t badCutFace = 2 * (m.ny > 1 ? m.nx : 0) + 1;
	setEdgeBoth(bad, m, badCutFace, 0, 1);
	int32_t badOpposite = oppositeCorner(m.indices, m.faceCount(), badCutFace * 3);
	TASK_CHECK(badOpposite >= 0);
	bad[uint32_t(badOpposite)] = 0;
	TASK_CHECK(setConstraints(a, 0, bad));
	TASK_CHECK(!ComputeChartsConstrained(a));
	// The previous param charts and packed atlas must still be repackable.
	PackCharts(a, testPackOptions());
	std::vector<const GeoMesh *> meshes; meshes.push_back(&m);
	TASK_CHECK(validateAtlasResult(a, meshes));
	TASK_CHECK(a->chartCount == oldCharts);
	TASK_CHECK(validateHardFlags(a, 0, m, good));

	// A legal follow-up constraint recovers.
	TASK_CHECK(SetMeshChartConstraints(a, 0, good.data(), uint32_t(good.size())));
	TASK_CHECK(constrainedPack(a));
	TASK_CHECK(validateAtlasResult(a, meshes));
	Destroy(a);
	return 0;
}

static int case_a_b_a_recompute()
{
	SetPrint(nullptr, false);
	GeoMesh m = makeGrid(3, 2);
	Atlas *a = Create();
	TASK_CHECK(addGeoMesh(a, m) == AddMeshError::Success);
	std::vector<uint8_t> join = allSharedJoins(m);
	TASK_CHECK(setConstraints(a, 0, join));
	TASK_CHECK(constrainedPack(a));
	std::vector<const GeoMesh *> meshes; meshes.push_back(&m);
	TASK_CHECK(validateAtlasResult(a, meshes));
	TASK_CHECK(a->meshes[0].chartCount == 1);

	// B: a complete cut line between cell columns 1 and 2.
	std::vector<uint8_t> cut(m.faceCount() * 3, 0);
	for (uint32_t j = 0; j < m.ny; j++) {
		uint32_t tri = 2 * (j * m.nx + 1) + 1; // Second triangle of cell ix=1.
		setEdgeBoth(cut, m, tri, 0, 1);
	}
	TASK_CHECK(setConstraints(a, 0, cut));
	TASK_CHECK(constrainedPack(a));
	TASK_CHECK(validateAtlasResult(a, meshes));
	TASK_CHECK(validateHardFlags(a, 0, m, cut));
	TASK_CHECK(a->meshes[0].chartCount == 2);

	// A again: recomputation starts from original indices and this closure.
	TASK_CHECK(setConstraints(a, 0, join));
	TASK_CHECK(constrainedPack(a));
	TASK_CHECK(validateAtlasResult(a, meshes));
	TASK_CHECK(validateHardFlags(a, 0, m, join));
	TASK_CHECK(a->meshes[0].chartCount == 1);
	Destroy(a);
	return 0;
}

static int case_setter_failure_keeps_old_constraints()
{
	SetPrint(nullptr, false);
	GeoMesh m = makeGrid(2, 1);
	Atlas *a = Create();
	TASK_CHECK(addGeoMesh(a, m) == AddMeshError::Success);
	std::vector<uint8_t> good = allSharedJoins(m);
	TASK_CHECK(setConstraints(a, 0, good));

	uint8_t unknown[6] = {0, 3, 0, 0, 0, 0};
	TASK_CHECK(!SetMeshChartConstraints(a, 0, unknown, 6));
	TASK_CHECK(!SetMeshChartConstraints(a, 0, good.data(), uint32_t(good.size()) + 3));
	TASK_CHECK(!SetMeshChartConstraints(a, 4, good.data(), uint32_t(good.size())));
	TASK_CHECK(!SetMeshChartConstraints(a, 0, nullptr, 6));
	std::vector<uint8_t> conflict = good;
	for (uint32_t c = 0; c < conflict.size(); c++) {
		int32_t o = oppositeCorner(m.indices, m.faceCount(), c);
		if (o >= 0 && conflict[c] == 2 && conflict[o] == 2) {
			conflict[c] = 1; // Direct 1+2 edge conflict.
			break;
		}
	}
	TASK_CHECK(!SetMeshChartConstraints(a, 0, conflict.data(), uint32_t(conflict.size())));

	TASK_CHECK(constrainedPack(a));
	std::vector<const GeoMesh *> meshes; meshes.push_back(&m);
	TASK_CHECK(validateAtlasResult(a, meshes));
	TASK_CHECK(validateHardFlags(a, 0, m, good));
	TASK_CHECK(a->meshes[0].chartCount == 1);
	Destroy(a);
	return 0;
}

static int case_no_hard_relations_reuses_behavior()
{
	SetPrint(nullptr, false);
	GeoMesh m = makeGrid(4, 2);
	Atlas *a = Create();
	TASK_CHECK(addGeoMesh(a, m) == AddMeshError::Success);
	ComputeCharts(a);
	unsigned ordinaryCharts = a->chartCount;
	PackCharts(a, testPackOptions());
	unsigned ordinaryPackedCharts = a->chartCount;
	Destroy(a);

	Atlas *b = Create();
	TASK_CHECK(addGeoMesh(b, m) == AddMeshError::Success);
	TASK_CHECK(ComputeChartsConstrained(b));
	PackCharts(b, testPackOptions());
	std::vector<const GeoMesh *> meshes; meshes.push_back(&m);
	TASK_CHECK(validateAtlasResult(b, meshes));
	TASK_CHECK(b->chartCount == ordinaryPackedCharts);
	TASK_CHECK(ordinaryPackedCharts >= 1);
	Destroy(b);
	return 0;
}

} // namespace

extern const struct task_case task_cases[] = {
	{"grouping", "transitive_join", case_transitive_join},
	{"grouping", "cut_alternative_path_conflict", case_cut_alternative_path_conflict},
	{"grouping", "non_disk_join_rejected", case_non_disk_join_rejected},
	{"grouping", "planar_fast_path", case_planar_fast_path},
	{"grouping", "multi_reseed", case_multi_reseed},
	{"grouping", "material_conflict", case_material_conflict},
	{"grouping", "multi_mesh_numbering", case_multi_mesh_numbering},
	{"grouping", "input_release", case_input_release},
	{"parameterization", "bent_disk_fallback", case_bent_disk_fallback},
	{"parameterization", "free_region_not_one_face_island", case_free_region_not_one_face_island},
	{"lifecycle", "failed_compute_repack", case_failed_compute_repack},
	{"lifecycle", "a_b_a_recompute", case_a_b_a_recompute},
	{"lifecycle", "setter_failure_keeps_old_constraints", case_setter_failure_keeps_old_constraints},
	{"lifecycle", "no_hard_relations_reuses_behavior", case_no_hard_relations_reuses_behavior},
};

extern const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
