/*
Windowless geometry regression for the xatlas baking path.

Samples are generated in code (no model files, no windowing, no image libs):
  - mesh 0: two planar patches of different shapes (rectangle + scalene triangle)
  - mesh 1: a strip with a 90 degree fold
  - mesh 2: same-position vertices split for hard normals
  - mesh 3: an ordinary non-degenerate face excluded via faceIgnoreData
Every input vertex carries a globally unique integer attribute that follows the
original vertex identity (not its position). All samples go through
ComputeCharts / PackCharts / output mesh reconstruction; AddMesh return values
alone are never trusted. A second atlas exercises AddUvMesh with two islands
that overlap in UV space but use different materials.

The same charts are re-packed with several PackOptions sets (padding,
rotateCharts, resolution and texelsPerUnit all change; one set forces multiple
pages). Output pointers are re-acquired after every PackCharts call and the full
check suite runs again. Chart placement and atlas dimensions are never
hardcoded, only structural/geometric invariants are asserted.
*/
#include "xatlas.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// Minimal check framework. Counts real executed cases/checks; never aborts on
// first failure so a single run reports every broken invariant.
// ---------------------------------------------------------------------------
static int g_caseCount = 0;
static int g_checkCount = 0;
static int g_failureCount = 0;
static const char *g_caseName = "";

static void beginCase(const char *name)
{
	g_caseName = name;
	++g_caseCount;
	printf("[CASE %d] %s\n", g_caseCount, name);
}

#define CHECK(_cond) do { \
	++g_checkCount; \
	if (!(_cond)) { \
		++g_failureCount; \
		printf("[FAIL] case='%s' %s:%d: CHECK(%s)\n", g_caseName, __FILE__, __LINE__, #_cond); \
	} \
} while (0)

#define CHECK_EQ_U32(_a, _b) do { \
	const uint32_t _va = (uint32_t)(_a), _vb = (uint32_t)(_b); \
	++g_checkCount; \
	if (_va != _vb) { \
		++g_failureCount; \
		printf("[FAIL] case='%s' %s:%d: %s=%u != %s=%u\n", g_caseName, __FILE__, __LINE__, #_a, _va, #_b, _vb); \
	} \
} while (0)

#define CHECK_EQ_I32(_a, _b) do { \
	const int32_t _va = (int32_t)(_a), _vb = (int32_t)(_b); \
	++g_checkCount; \
	if (_va != _vb) { \
		++g_failureCount; \
		printf("[FAIL] case='%s' %s:%d: %s=%d != %s=%d\n", g_caseName, __FILE__, __LINE__, #_a, (int)_va, #_b, (int)_vb); \
	} \
} while (0)

// ---------------------------------------------------------------------------
// 2D geometry helpers. Triangle interior overlap is decided by polygon
// clipping (intersection area), not by bounding boxes and not by Atlas::image.
// Shared-edge contact yields zero intersection area and is allowed; vertex
// coordinates are tolerated to 1e-5.
// ---------------------------------------------------------------------------
struct Vec2
{
	double x, y;
};

static const double kCoordEps = 1e-5;
static const double kOverlapAreaEps = 1e-6;

static double cross2(const Vec2 &a, const Vec2 &b, const Vec2 &c)
{
	return (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
}

// Area of the subject polygon clipped against the CCW clip triangle.
static double clippedAreaAgainstTriangle(const Vec2 *subject, int subjectCount, const Vec2 *clipTri)
{
	std::vector<Vec2> poly(subject, subject + subjectCount);
	for (int e = 0; e < 3 && !poly.empty(); ++e) {
		const Vec2 &a = clipTri[e];
		const Vec2 &b = clipTri[(e + 1) % 3];
		std::vector<Vec2> out;
		for (size_t i = 0; i < poly.size(); ++i) {
			const Vec2 &p = poly[i];
			const Vec2 &q = poly[(i + 1) % poly.size()];
			const double dp = cross2(a, b, p);
			const double dq = cross2(a, b, q);
			const bool pin = dp >= -kCoordEps;
			const bool qin = dq >= -kCoordEps;
			if (pin)
				out.push_back(p);
			if (pin != qin) {
				const double t = dp / (dp - dq);
				Vec2 x;
				x.x = p.x + t * (q.x - p.x);
				x.y = p.y + t * (q.y - p.y);
				out.push_back(x);
			}
		}
		poly.swap(out);
	}
	if (poly.size() < 3)
		return 0.0;
	double area = 0.0;
	for (size_t i = 0; i < poly.size(); ++i) {
		const Vec2 &p = poly[i];
		const Vec2 &q = poly[(i + 1) % poly.size()];
		area += p.x * q.y - q.x * p.y;
	}
	return std::fabs(area) * 0.5;
}

static bool trianglesInteriorOverlap(const Vec2 *ta, const Vec2 *tb)
{
	Vec2 clip[3] = { tb[0], tb[1], tb[2] };
	if (cross2(clip[0], clip[1], clip[2]) < 0.0) {
		const Vec2 tmp = clip[1];
		clip[1] = clip[2];
		clip[2] = tmp;
	}
	return clippedAreaAgainstTriangle(ta, 3, clip) > kOverlapAreaEps;
}

// ---------------------------------------------------------------------------
// Input sample description. The integer attribute is unique per input vertex
// across all meshes and follows vertex identity, not position.
// ---------------------------------------------------------------------------
struct MeshSpec
{
	std::vector<float> positions;   // 3 per vertex
	std::vector<float> normals;     // 3 per vertex (used only if hasNormals)
	std::vector<uint32_t> indices;  // 3 per face
	std::vector<uint32_t> attributes; // 1 per vertex, globally unique
	std::vector<uint32_t> materials;  // 1 per face
	std::vector<uint8_t> ignore;      // 1 per face
	bool hasNormals = false;
	bool hasMaterials = false;
	bool hasIgnore = false;
	uint32_t vertexCount() const { return (uint32_t)(positions.size() / 3); }
	uint32_t faceCount() const { return (uint32_t)(indices.size() / 3); }
	bool faceIgnored(uint32_t f) const { return hasIgnore && ignore[f] != 0; }
};

static uint32_t g_nextAttribute = 1;

static uint32_t addVertex(MeshSpec &mesh, float x, float y, float z, float nx, float ny, float nz)
{
	mesh.positions.push_back(x);
	mesh.positions.push_back(y);
	mesh.positions.push_back(z);
	mesh.normals.push_back(nx);
	mesh.normals.push_back(ny);
	mesh.normals.push_back(nz);
	mesh.attributes.push_back(g_nextAttribute++);
	return (uint32_t)(mesh.positions.size() / 3 - 1);
}

static void addFace(MeshSpec &mesh, uint32_t a, uint32_t b, uint32_t c, uint32_t material, bool ignore)
{
	mesh.indices.push_back(a);
	mesh.indices.push_back(b);
	mesh.indices.push_back(c);
	mesh.materials.push_back(material);
	mesh.ignore.push_back(ignore ? 1 : 0);
}

static void buildMeshSamples(std::vector<MeshSpec> &specs)
{
	// Mesh 0: two disconnected planar patches of different shapes.
	{
		MeshSpec m;
		m.hasNormals = true;
		m.hasMaterials = true;
		// Rectangle 2x1 in the z=0 plane.
		const uint32_t r0 = addVertex(m, 0.0f, 0.0f, 0.0f, 0, 0, 1);
		const uint32_t r1 = addVertex(m, 2.0f, 0.0f, 0.0f, 0, 0, 1);
		const uint32_t r2 = addVertex(m, 2.0f, 1.0f, 0.0f, 0, 0, 1);
		const uint32_t r3 = addVertex(m, 0.0f, 1.0f, 0.0f, 0, 0, 1);
		addFace(m, r0, r1, r2, 3, false);
		addFace(m, r0, r2, r3, 3, false);
		// Scalene triangle patch, different shape and material.
		const uint32_t t0 = addVertex(m, 0.2f, 2.0f, 0.0f, 0, 0, 1);
		const uint32_t t1 = addVertex(m, 1.7f, 2.3f, 0.0f, 0, 0, 1);
		const uint32_t t2 = addVertex(m, 0.4f, 3.4f, 0.0f, 0, 0, 1);
		addFace(m, t0, t1, t2, 4, false);
		specs.push_back(m);
	}
	// Mesh 1: strip with a 90 degree fold (horizontal quad + vertical quad
	// sharing a real edge, no vertex normals supplied).
	{
		MeshSpec m;
		m.hasNormals = false;
		m.hasMaterials = true;
		const uint32_t h0 = addVertex(m, 3.0f, 0.0f, 0.0f, 0, 0, 0);
		const uint32_t h1 = addVertex(m, 5.0f, 0.0f, 0.0f, 0, 0, 0);
		const uint32_t h2 = addVertex(m, 5.0f, 1.0f, 0.0f, 0, 0, 0);
		const uint32_t h3 = addVertex(m, 3.0f, 1.0f, 0.0f, 0, 0, 0);
		const uint32_t v0 = addVertex(m, 3.0f, 0.0f, 1.0f, 0, 0, 0);
		const uint32_t v1 = addVertex(m, 5.0f, 0.0f, 1.0f, 0, 0, 0);
		addFace(m, h0, h1, h2, 5, false);
		addFace(m, h0, h2, h3, 5, false);
		addFace(m, h0, v0, v1, 5, false);
		addFace(m, h0, v1, h1, 5, false);
		specs.push_back(m);
	}
	// Mesh 2: two quads meeting at a hard edge. The seam vertices exist twice
	// at identical positions with different normals (hard normal split).
	{
		MeshSpec m;
		m.hasNormals = true;
		m.hasMaterials = true;
		const uint32_t a0 = addVertex(m, 6.0f, 0.0f, 0.0f, 0, 0, 1);
		const uint32_t a1 = addVertex(m, 8.0f, 0.0f, 0.0f, 0, 0, 1);
		const uint32_t a2 = addVertex(m, 8.0f, 1.0f, 0.0f, 0, 0, 1);
		const uint32_t a3 = addVertex(m, 6.0f, 1.0f, 0.0f, 0, 0, 1);
		addFace(m, a0, a1, a2, 6, false);
		addFace(m, a0, a2, a3, 6, false);
		// b0/b1 are colocal with a0/a1 but carry different normals and attributes.
		const uint32_t b0 = addVertex(m, 6.0f, 0.0f, 0.0f, 0, -1, 0);
		const uint32_t b1 = addVertex(m, 8.0f, 0.0f, 0.0f, 0, -1, 0);
		const uint32_t b2 = addVertex(m, 8.0f, 0.0f, 1.0f, 0, -1, 0);
		const uint32_t b3 = addVertex(m, 6.0f, 0.0f, 1.0f, 0, -1, 0);
		addFace(m, b0, b1, b2, 6, false);
		addFace(m, b0, b2, b3, 6, false);
		specs.push_back(m);
	}
	// Mesh 3: three ordinary non-degenerate triangles, the middle one excluded
	// via faceIgnoreData.
	{
		MeshSpec m;
		m.hasNormals = true;
		m.hasMaterials = true;
		m.hasIgnore = true;
		const uint32_t p0 = addVertex(m, 10.0f, 0.0f, 0.0f, 0, 0, 1);
		const uint32_t p1 = addVertex(m, 12.0f, 0.0f, 0.0f, 0, 0, 1);
		const uint32_t p2 = addVertex(m, 10.0f, 1.0f, 0.0f, 0, 0, 1);
		const uint32_t p3 = addVertex(m, 12.0f, 1.0f, 0.0f, 0, 0, 1);
		addFace(m, p0, p1, p2, 7, false);
		addFace(m, p1, p3, p2, 7, true); // ordinary non-degenerate, ignored
		addFace(m, p0, p2, p3, 7, false);
		specs.push_back(m);
	}
}

static bool addMeshToAtlas(xatlas::Atlas *atlas, const MeshSpec &spec)
{
	xatlas::MeshDecl decl;
	decl.vertexPositionData = spec.positions.data();
	decl.vertexPositionStride = sizeof(float) * 3;
	decl.vertexCount = spec.vertexCount();
	if (spec.hasNormals) {
		decl.vertexNormalData = spec.normals.data();
		decl.vertexNormalStride = sizeof(float) * 3;
	}
	decl.indexData = spec.indices.data();
	decl.indexCount = (uint32_t)spec.indices.size();
	decl.indexFormat = xatlas::IndexFormat::UInt32;
	if (spec.hasMaterials)
		decl.faceMaterialData = spec.materials.data();
	if (spec.hasIgnore)
		decl.faceIgnoreData = reinterpret_cast<const bool *>(spec.ignore.data());
	return xatlas::AddMesh(atlas, decl) == xatlas::AddMeshError::Success;
}

// ---------------------------------------------------------------------------
// Output mesh introspection shared by the AddMesh and AddUvMesh checks.
// ---------------------------------------------------------------------------
struct TriangleRef
{
	uint32_t page, chart, mesh, face;
	Vec2 p[3];
};

static void collectChartTriangles(const xatlas::Atlas *atlas, std::vector<TriangleRef> &tris)
{
	uint32_t base = 0;
	for (uint32_t i = 0; i < atlas->meshCount; ++i) {
		const xatlas::Mesh &mesh = atlas->meshes[i];
		for (uint32_t c = 0; c < mesh.chartCount; ++c) {
			const xatlas::Chart &chart = mesh.chartArray[c];
			for (uint32_t f = 0; f < chart.faceCount; ++f) {
				const uint32_t face = chart.faceArray[f];
				TriangleRef t;
				t.page = chart.atlasIndex;
				t.chart = base + c;
				t.mesh = i;
				t.face = face;
				for (uint32_t j = 0; j < 3; ++j) {
					const xatlas::Vertex &v = mesh.vertexArray[mesh.indexArray[face * 3 + j]];
					t.p[j].x = v.uv[0];
					t.p[j].y = v.uv[1];
				}
				tris.push_back(t);
			}
		}
		base += mesh.chartCount;
	}
}

static bool triangleBboxesDisjoint(const TriangleRef &a, const TriangleRef &b)
{
	double aminX = a.p[0].x, amaxX = a.p[0].x, aminY = a.p[0].y, amaxY = a.p[0].y;
	double bminX = b.p[0].x, bmaxX = b.p[0].x, bminY = b.p[0].y, bmaxY = b.p[0].y;
	for (int j = 1; j < 3; ++j) {
		if (a.p[j].x < aminX) aminX = a.p[j].x;
		if (a.p[j].x > amaxX) amaxX = a.p[j].x;
		if (a.p[j].y < aminY) aminY = a.p[j].y;
		if (a.p[j].y > amaxY) amaxY = a.p[j].y;
		if (b.p[j].x < bminX) bminX = b.p[j].x;
		if (b.p[j].x > bmaxX) bmaxX = b.p[j].x;
		if (b.p[j].y < bminY) bminY = b.p[j].y;
		if (b.p[j].y > bmaxY) bmaxY = b.p[j].y;
	}
	return amaxX < bminX - kCoordEps || bmaxX < aminX - kCoordEps ||
	       amaxY < bminY - kCoordEps || bmaxY < aminY - kCoordEps;
}

// Triangles of different charts on the same page must not overlap in their
// interiors. Pure geometry: no bounding-box-only verdict, no Atlas::image.
static void checkChartsDoNotOverlap(const xatlas::Atlas *atlas)
{
	std::vector<TriangleRef> tris;
	collectChartTriangles(atlas, tris);
	for (size_t a = 0; a < tris.size(); ++a) {
		for (size_t b = a + 1; b < tris.size(); ++b) {
			if (tris[a].page != tris[b].page || tris[a].chart == tris[b].chart)
				continue;
			if (triangleBboxesDisjoint(tris[a], tris[b]))
				continue;
			++g_checkCount;
			if (trianglesInteriorOverlap(tris[a].p, tris[b].p)) {
				++g_failureCount;
				printf("[FAIL] case='%s': chart %u (mesh %u face %u) overlaps chart %u (mesh %u face %u) on page %u\n",
					g_caseName, tris[a].chart, tris[a].mesh, tris[a].face, tris[b].chart, tris[b].mesh, tris[b].face, tris[a].page);
			}
		}
	}
}

// ---------------------------------------------------------------------------
// SVG dump: one page frame per atlas page, UV triangles labeled with source
// mesh, source face and global chart index. No image library involved.
// ---------------------------------------------------------------------------
static bool writeAtlasSvg(const xatlas::Atlas *atlas, const char *path)
{
	FILE *file = fopen(path, "w");
	if (!file)
		return false;
	const double scale = 2.0;
	const double margin = 16.0;
	const double labelH = 12.0;
	const double pageW = atlas->width * scale;
	const double pageH = atlas->height * scale;
	const double totalW = margin + atlas->atlasCount * (pageW + margin);
	const double totalH = margin + labelH + pageH + margin;
	fprintf(file, "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"%g\" height=\"%g\" viewBox=\"0 0 %g %g\">\n", totalW, totalH, totalW, totalH);
	fprintf(file, "<rect x=\"0\" y=\"0\" width=\"%g\" height=\"%g\" fill=\"#ffffff\"/>\n", totalW, totalH);
	for (uint32_t page = 0; page < atlas->atlasCount; ++page) {
		const double ox = margin + page * (pageW + margin);
		const double oy = margin + labelH;
		fprintf(file, "<rect x=\"%g\" y=\"%g\" width=\"%g\" height=\"%g\" fill=\"none\" stroke=\"#333333\" stroke-width=\"1\"/>\n", ox, oy, pageW, pageH);
		fprintf(file, "<text x=\"%g\" y=\"%g\" font-size=\"9\" fill=\"#333333\">page %u (%ux%u)</text>\n", ox, oy - 4.0, page, atlas->width, atlas->height);
	}
	uint32_t base = 0;
	for (uint32_t i = 0; i < atlas->meshCount; ++i) {
		const xatlas::Mesh &mesh = atlas->meshes[i];
		for (uint32_t c = 0; c < mesh.chartCount; ++c) {
			const xatlas::Chart &chart = mesh.chartArray[c];
			const uint32_t globalChart = base + c;
			const unsigned cr = 60 + (globalChart * 97) % 160;
			const unsigned cg = 60 + (globalChart * 57 + 40) % 160;
			const unsigned cb = 60 + (globalChart * 37 + 80) % 160;
			const double ox = margin + chart.atlasIndex * (pageW + margin);
			const double oy = margin + labelH;
			for (uint32_t f = 0; f < chart.faceCount; ++f) {
				const uint32_t face = chart.faceArray[f];
				double cx = 0.0, cy = 0.0;
				fprintf(file, "<polygon points=\"");
				for (uint32_t j = 0; j < 3; ++j) {
					const xatlas::Vertex &v = mesh.vertexArray[mesh.indexArray[face * 3 + j]];
					const double x = ox + v.uv[0] * scale;
					const double y = oy + v.uv[1] * scale;
					cx += x;
					cy += y;
					fprintf(file, "%g,%g ", x, y);
				}
				fprintf(file, "\" fill=\"rgb(%u,%u,%u)\" fill-opacity=\"0.5\" stroke=\"#111111\" stroke-width=\"0.4\"/>\n", cr, cg, cb);
				fprintf(file, "<text x=\"%g\" y=\"%g\" font-size=\"3\" fill=\"#000000\">m%u f%u c%u</text>\n", cx / 3.0, cy / 3.0, i, face, globalChart);
			}
		}
		base += mesh.chartCount;
	}
	fprintf(file, "</svg>\n");
	fclose(file);
	return true;
}

static std::string joinPath(const std::string &dir, const char *file)
{
	if (dir.empty() || dir == ".")
		return file;
	if (dir[dir.size() - 1] == '/')
		return dir + file;
	return dir + "/" + file;
}

// ---------------------------------------------------------------------------
// Full check suite for the AddMesh atlas. Runs after every PackCharts call on
// freshly acquired output pointers.
// ---------------------------------------------------------------------------
static void runFullAtlasChecks(const xatlas::Atlas *atlas, const std::vector<MeshSpec> &specs, const char *tag, int pageExpect, const std::string &svgDir)
{
	char name[160];

	snprintf(name, sizeof(name), "%s: atlas counts and pages", tag);
	beginCase(name);
	{
		CHECK_EQ_U32(atlas->meshCount, (uint32_t)specs.size());
		uint32_t totalCharts = 0;
		for (uint32_t i = 0; i < atlas->meshCount; ++i)
			totalCharts += atlas->meshes[i].chartCount;
		CHECK_EQ_U32(atlas->chartCount, totalCharts);
		CHECK(atlas->atlasCount >= 1);
		if (pageExpect < 0)
			CHECK_EQ_U32(atlas->atlasCount, 1); // resolution 0 packs into a single growing page
		if (pageExpect > 0)
			CHECK(atlas->atlasCount >= 2); // forced multi-page pack
		CHECK(atlas->width > 0 && atlas->height > 0);
		CHECK(atlas->texelsPerUnit > 0.0f);
		CHECK(atlas->utilization != nullptr);
		if (atlas->utilization) {
			for (uint32_t p = 0; p < atlas->atlasCount; ++p)
				CHECK(atlas->utilization[p] >= 0.0f && atlas->utilization[p] <= 1.0f);
		}
		CHECK(atlas->image != nullptr);
		for (uint32_t i = 0; i < atlas->meshCount && i < specs.size(); ++i) {
			CHECK_EQ_U32(atlas->meshes[i].indexCount, (uint32_t)specs[i].indices.size());
			CHECK(atlas->meshes[i].chartCount > 0);
			CHECK(atlas->meshes[i].vertexCount >= specs[i].vertexCount());
		}
	}

	snprintf(name, sizeof(name), "%s: per-corner xref reconstructs source vertices and attributes", tag);
	beginCase(name);
	for (uint32_t i = 0; i < atlas->meshCount && i < specs.size(); ++i) {
		const xatlas::Mesh &mesh = atlas->meshes[i];
		const MeshSpec &spec = specs[i];
		std::vector<uint8_t> seen(spec.vertexCount(), 0);
		for (uint32_t k = 0; k < mesh.indexCount; ++k) {
			const uint32_t outIndex = mesh.indexArray[k];
			CHECK(outIndex < mesh.vertexCount);
			if (outIndex >= mesh.vertexCount)
				continue;
			const xatlas::Vertex &v = mesh.vertexArray[outIndex];
			const uint32_t expected = spec.indices[k];
			// xref must name the exact source vertex of this corner.
			CHECK_EQ_U32(v.xref, expected);
			if (v.xref < spec.attributes.size() && expected < spec.attributes.size())
				CHECK_EQ_U32(spec.attributes[v.xref], spec.attributes[expected]);
			if (v.xref < spec.vertexCount()) {
				for (int c = 0; c < 3; ++c)
					CHECK(spec.positions[v.xref * 3 + c] == spec.positions[expected * 3 + c]);
			}
			if (v.xref < seen.size())
				seen[v.xref] = 1;
		}
		// Every input vertex survives with its identity; nothing got merged away.
		uint32_t seenCount = 0;
		for (uint32_t s = 0; s < seen.size(); ++s)
			seenCount += seen[s];
		CHECK_EQ_U32(seenCount, spec.vertexCount());
	}

	snprintf(name, sizeof(name), "%s: ignored faces keep order and have no chart/atlas/uv", tag);
	beginCase(name);
	for (uint32_t i = 0; i < atlas->meshCount && i < specs.size(); ++i) {
		const xatlas::Mesh &mesh = atlas->meshes[i];
		const MeshSpec &spec = specs[i];
		for (uint32_t f = 0; f < spec.faceCount(); ++f) {
			const bool ignored = spec.faceIgnored(f);
			for (uint32_t j = 0; j < 3; ++j) {
				const uint32_t outIndex = mesh.indexArray[f * 3 + j];
				if (outIndex >= mesh.vertexCount)
					continue;
				const xatlas::Vertex &v = mesh.vertexArray[outIndex];
				if (ignored) {
					CHECK_EQ_I32(v.chartIndex, -1);
					CHECK_EQ_I32(v.atlasIndex, -1);
					CHECK(v.uv[0] == 0.0f && v.uv[1] == 0.0f);
				} else {
					CHECK(v.chartIndex >= 0 && (uint32_t)v.chartIndex < atlas->chartCount);
					CHECK(v.atlasIndex >= 0 && (uint32_t)v.atlasIndex < atlas->atlasCount);
				}
			}
		}
	}

	snprintf(name, sizeof(name), "%s: chart face membership partitions valid faces", tag);
	beginCase(name);
	for (uint32_t i = 0; i < atlas->meshCount && i < specs.size(); ++i) {
		const xatlas::Mesh &mesh = atlas->meshes[i];
		const MeshSpec &spec = specs[i];
		const uint32_t faceCount = spec.faceCount();
		std::vector<int32_t> faceChart(faceCount, -1);
		uint32_t validFaces = 0, chartFaces = 0;
		for (uint32_t f = 0; f < faceCount; ++f)
			if (!spec.faceIgnored(f))
				++validFaces;
		for (uint32_t c = 0; c < mesh.chartCount; ++c) {
			const xatlas::Chart &chart = mesh.chartArray[c];
			CHECK(chart.faceCount > 0);
			CHECK(chart.faceArray != nullptr);
			CHECK(chart.atlasIndex < atlas->atlasCount);
			chartFaces += chart.faceCount;
			for (uint32_t fi = 0; fi < chart.faceCount; ++fi) {
				const uint32_t face = chart.faceArray[fi];
				CHECK(face < faceCount);
				if (face >= faceCount)
					continue;
				CHECK(!spec.faceIgnored(face)); // ignored faces never join a chart
				CHECK(faceChart[face] == -1); // no face in two charts
				faceChart[face] = (int32_t)c;
				if (spec.hasMaterials)
					CHECK_EQ_U32(spec.materials[face], spec.materials[chart.faceArray[0]]);
			}
		}
		CHECK_EQ_U32(chartFaces, validFaces);
		for (uint32_t f = 0; f < faceCount; ++f)
			if (!spec.faceIgnored(f))
				CHECK(faceChart[f] >= 0);
	}

	snprintf(name, sizeof(name), "%s: vertex chartIndex/atlasIndex cross-reference (atlas-global chart ids)", tag);
	beginCase(name);
	{
		uint32_t base = 0; // chartIndex is global to the whole Atlas, not per-mesh
		for (uint32_t i = 0; i < atlas->meshCount && i < specs.size(); ++i) {
			const xatlas::Mesh &mesh = atlas->meshes[i];
			const MeshSpec &spec = specs[i];
			const uint32_t faceCount = spec.faceCount();
			std::vector<int32_t> faceChart(faceCount, -1);
			for (uint32_t c = 0; c < mesh.chartCount; ++c)
				for (uint32_t fi = 0; fi < mesh.chartArray[c].faceCount; ++fi) {
					const uint32_t face = mesh.chartArray[c].faceArray[fi];
					if (face < faceCount)
						faceChart[face] = (int32_t)c;
				}
			std::vector<std::vector<uint32_t> > vertexFaces(mesh.vertexCount);
			for (uint32_t k = 0; k < mesh.indexCount; ++k)
				if (mesh.indexArray[k] < mesh.vertexCount)
					vertexFaces[mesh.indexArray[k]].push_back(k / 3);
			for (uint32_t vi = 0; vi < mesh.vertexCount; ++vi) {
				const xatlas::Vertex &v = mesh.vertexArray[vi];
				CHECK(!vertexFaces[vi].empty()); // every output vertex is referenced
				if (v.chartIndex == -1) {
					CHECK_EQ_I32(v.atlasIndex, -1);
					for (uint32_t n = 0; n < vertexFaces[vi].size(); ++n)
						CHECK(spec.faceIgnored(vertexFaces[vi][n]));
				} else {
					const int64_t local = (int64_t)v.chartIndex - (int64_t)base;
					CHECK(local >= 0 && local < (int64_t)mesh.chartCount);
					if (local >= 0 && local < (int64_t)mesh.chartCount) {
						const xatlas::Chart &chart = mesh.chartArray[local];
						CHECK_EQ_I32(v.atlasIndex, (int32_t)chart.atlasIndex);
						for (uint32_t n = 0; n < vertexFaces[vi].size(); ++n)
							CHECK(faceChart[vertexFaces[vi][n]] == (int32_t)local);
					}
				}
			}
			for (uint32_t c = 0; c < mesh.chartCount; ++c) {
				const xatlas::Chart &chart = mesh.chartArray[c];
				const uint32_t globalChart = base + c;
				for (uint32_t fi = 0; fi < chart.faceCount; ++fi) {
					const uint32_t face = chart.faceArray[fi];
					if (face >= faceCount)
						continue;
					for (uint32_t j = 0; j < 3; ++j) {
						const uint32_t outIndex = mesh.indexArray[face * 3 + j];
						if (outIndex >= mesh.vertexCount)
							continue;
						const xatlas::Vertex &v = mesh.vertexArray[outIndex];
						CHECK_EQ_I32(v.chartIndex, (int32_t)globalChart);
						CHECK_EQ_I32(v.atlasIndex, (int32_t)chart.atlasIndex);
					}
				}
			}
			base += mesh.chartCount;
		}
		CHECK_EQ_U32(base, atlas->chartCount);
	}

	snprintf(name, sizeof(name), "%s: valid face UVs finite, inside page, non-zero area", tag);
	beginCase(name);
	{
		const double tol = 1e-4;
		for (uint32_t i = 0; i < atlas->meshCount && i < specs.size(); ++i) {
			const xatlas::Mesh &mesh = atlas->meshes[i];
			const MeshSpec &spec = specs[i];
			for (uint32_t f = 0; f < spec.faceCount(); ++f) {
				if (spec.faceIgnored(f))
					continue;
				Vec2 t[3];
				bool indicesOk = true;
				for (uint32_t j = 0; j < 3; ++j) {
					const uint32_t outIndex = mesh.indexArray[f * 3 + j];
					if (outIndex >= mesh.vertexCount) {
						indicesOk = false;
						break;
					}
					const xatlas::Vertex &v = mesh.vertexArray[outIndex];
					t[j].x = v.uv[0];
					t[j].y = v.uv[1];
				}
				if (!indicesOk)
					continue;
				for (uint32_t j = 0; j < 3; ++j) {
					CHECK(std::isfinite(t[j].x) && std::isfinite(t[j].y));
					CHECK(t[j].x >= -tol && t[j].x <= (double)atlas->width + tol);
					CHECK(t[j].y >= -tol && t[j].y <= (double)atlas->height + tol);
				}
				const double area = std::fabs(cross2(t[0], t[1], t[2])) * 0.5;
				CHECK(area > 1e-6);
			}
		}
	}

	snprintf(name, sizeof(name), "%s: colocal (hard-normal split) vertices keep distinct identity", tag);
	beginCase(name);
	{
		uint32_t splitPairs = 0;
		for (uint32_t i = 0; i < atlas->meshCount && i < specs.size(); ++i) {
			const xatlas::Mesh &mesh = atlas->meshes[i];
			const MeshSpec &spec = specs[i];
			for (uint32_t a = 0; a < mesh.vertexCount; ++a) {
				const uint32_t xa = mesh.vertexArray[a].xref;
				if (xa >= spec.vertexCount())
					continue;
				for (uint32_t b = a + 1; b < mesh.vertexCount; ++b) {
					const uint32_t xb = mesh.vertexArray[b].xref;
					if (xb >= spec.vertexCount() || xb == xa)
						continue;
					bool samePos = true;
					for (int c = 0; c < 3; ++c)
						samePos = samePos && (spec.positions[xa * 3 + c] == spec.positions[xb * 3 + c]);
					// Same position must never be a reason to merge identities.
					if (samePos && spec.attributes[xa] != spec.attributes[xb])
						++splitPairs;
				}
			}
		}
		CHECK(splitPairs > 0);
	}

	snprintf(name, sizeof(name), "%s: chart triangles do not overlap (geometric)", tag);
	beginCase(name);
	checkChartsDoNotOverlap(atlas);

	snprintf(name, sizeof(name), "%s: atlas image chart indices in range", tag);
	beginCase(name);
	{
		CHECK(atlas->image != nullptr);
		if (atlas->image) {
			const uint64_t texelCount = (uint64_t)atlas->atlasCount * atlas->width * atlas->height;
			uint64_t chartTexels = 0, badTexels = 0;
			for (uint64_t t = 0; t < texelCount; ++t) {
				const uint32_t texel = atlas->image[t];
				if (texel & xatlas::kImageHasChartIndexBit) {
					++chartTexels;
					if ((texel & xatlas::kImageChartIndexMask) >= atlas->chartCount)
						++badTexels;
				}
			}
			CHECK(chartTexels > 0);
			CHECK(badTexels == 0);
		}
	}

	snprintf(name, sizeof(name), "%s: svg dump written", tag);
	beginCase(name);
	{
		snprintf(name, sizeof(name), "atlas_geometry_%s.svg", tag);
		const std::string path = joinPath(svgDir, name);
		CHECK(writeAtlasSvg(atlas, path.c_str()));
		if (g_failureCount == 0)
			printf("       wrote %s\n", path.c_str());
	}
}

// ---------------------------------------------------------------------------
// AddUvMesh atlas: two islands with identical (overlapping) UVs but different
// materials must be packed as separate, non-overlapping charts.
// ---------------------------------------------------------------------------
struct UvMeshSpec
{
	std::vector<float> uvs;           // 2 per vertex
	std::vector<uint32_t> indices;    // 3 per face
	std::vector<uint32_t> materials;  // 1 per face
	std::vector<uint32_t> attributes; // 1 per vertex
	uint32_t vertexCount() const { return (uint32_t)(uvs.size() / 2); }
	uint32_t faceCount() const { return (uint32_t)(indices.size() / 3); }
};

static uint32_t addUvVertex(UvMeshSpec &mesh, float u, float v)
{
	mesh.uvs.push_back(u);
	mesh.uvs.push_back(v);
	mesh.attributes.push_back(g_nextAttribute++);
	return (uint32_t)(mesh.uvs.size() / 2 - 1);
}

static void addUvFace(UvMeshSpec &mesh, uint32_t a, uint32_t b, uint32_t c, uint32_t material)
{
	mesh.indices.push_back(a);
	mesh.indices.push_back(b);
	mesh.indices.push_back(c);
	mesh.materials.push_back(material);
}

static void runUvMeshTest(const std::string &svgDir)
{
	UvMeshSpec spec;
	// Island A: unit square in UV space, material 7.
	const uint32_t a0 = addUvVertex(spec, 0.0f, 0.0f);
	const uint32_t a1 = addUvVertex(spec, 1.0f, 0.0f);
	const uint32_t a2 = addUvVertex(spec, 1.0f, 1.0f);
	const uint32_t a3 = addUvVertex(spec, 0.0f, 1.0f);
	addUvFace(spec, a0, a1, a2, 7);
	addUvFace(spec, a0, a2, a3, 7);
	// Island B: same UVs (fully overlapping island A), material 19.
	const uint32_t b0 = addUvVertex(spec, 0.0f, 0.0f);
	const uint32_t b1 = addUvVertex(spec, 1.0f, 0.0f);
	const uint32_t b2 = addUvVertex(spec, 1.0f, 1.0f);
	const uint32_t b3 = addUvVertex(spec, 0.0f, 1.0f);
	addUvFace(spec, b0, b1, b2, 19);
	addUvFace(spec, b0, b2, b3, 19);

	beginCase("uvmesh: overlapping islands accepted and source UVs really overlap");
	xatlas::Atlas *atlas = xatlas::Create();
	xatlas::UvMeshDecl decl;
	decl.vertexUvData = spec.uvs.data();
	decl.vertexStride = sizeof(float) * 2;
	decl.vertexCount = spec.vertexCount();
	decl.indexData = spec.indices.data();
	decl.indexCount = (uint32_t)spec.indices.size();
	decl.indexFormat = xatlas::IndexFormat::UInt32;
	decl.faceMaterialData = spec.materials.data();
	CHECK(xatlas::AddUvMesh(atlas, decl) == xatlas::AddMeshError::Success);
	{
		// Prove the two source islands overlap in UV space (bbox intersection area > 0).
		float minA[2] = { 1e30f, 1e30f }, maxA[2] = { -1e30f, -1e30f };
		float minB[2] = { 1e30f, 1e30f }, maxB[2] = { -1e30f, -1e30f };
		for (uint32_t f = 0; f < spec.faceCount(); ++f) {
			for (uint32_t j = 0; j < 3; ++j) {
				const uint32_t v = spec.indices[f * 3 + j];
				float *mn = f < 2 ? minA : minB;
				float *mx = f < 2 ? maxA : maxB;
				for (int c = 0; c < 2; ++c) {
					if (spec.uvs[v * 2 + c] < mn[c]) mn[c] = spec.uvs[v * 2 + c];
					if (spec.uvs[v * 2 + c] > mx[c]) mx[c] = spec.uvs[v * 2 + c];
				}
			}
		}
		const float overlapW = (maxA[0] < maxB[0] ? maxA[0] : maxB[0]) - (minA[0] > minB[0] ? minA[0] : minB[0]);
		const float overlapH = (maxA[1] < maxB[1] ? maxA[1] : maxB[1]) - (minA[1] > minB[1] ? minA[1] : minB[1]);
		CHECK(overlapW > 0.0f && overlapH > 0.0f);
	}

	// The samples must really go through charting, packing and output rebuild.
	xatlas::ComputeCharts(atlas);
	xatlas::PackOptions packOptions;
	packOptions.padding = 2;
	packOptions.texelsPerUnit = 32.0f;
	packOptions.createImage = true;
	xatlas::PackCharts(atlas, packOptions);

	beginCase("uvmesh: charts split by material, faces partitioned");
	CHECK_EQ_U32(atlas->meshCount, 1);
	CHECK_EQ_U32(atlas->chartCount, 2);
	if (atlas->meshCount == 1) {
		const xatlas::Mesh &mesh = atlas->meshes[0];
		CHECK_EQ_U32(mesh.chartCount, 2);
		CHECK_EQ_U32(mesh.vertexCount, spec.vertexCount());
		CHECK_EQ_U32(mesh.indexCount, (uint32_t)spec.indices.size());
		if (mesh.chartCount == 2) {
			const uint32_t m0 = mesh.chartArray[0].material;
			const uint32_t m1 = mesh.chartArray[1].material;
			CHECK((m0 == 7 && m1 == 19) || (m0 == 19 && m1 == 7));
		}
		uint32_t chartFaces = 0;
		std::vector<int32_t> faceChart(spec.faceCount(), -1);
		for (uint32_t c = 0; c < mesh.chartCount; ++c) {
			const xatlas::Chart &chart = mesh.chartArray[c];
			chartFaces += chart.faceCount;
			for (uint32_t fi = 0; fi < chart.faceCount; ++fi) {
				const uint32_t face = chart.faceArray[fi];
				CHECK(face < spec.faceCount());
				if (face >= spec.faceCount())
					continue;
				CHECK(faceChart[face] == -1);
				faceChart[face] = (int32_t)c;
				CHECK_EQ_U32(spec.materials[face], chart.material);
			}
		}
		CHECK_EQ_U32(chartFaces, spec.faceCount());
		for (uint32_t f = 0; f < spec.faceCount(); ++f)
			CHECK(faceChart[f] >= 0);
	}

	beginCase("uvmesh: output identity, attributes and page-local UVs");
	if (atlas->meshCount == 1) {
		const xatlas::Mesh &mesh = atlas->meshes[0];
		for (uint32_t k = 0; k < mesh.indexCount && k < spec.indices.size(); ++k)
			CHECK_EQ_U32(mesh.indexArray[k], spec.indices[k]); // index order preserved
		for (uint32_t v = 0; v < mesh.vertexCount && v < spec.vertexCount(); ++v) {
			const xatlas::Vertex &vert = mesh.vertexArray[v];
			CHECK_EQ_U32(vert.xref, v);
			if (vert.xref < spec.attributes.size())
				CHECK_EQ_U32(spec.attributes[vert.xref], spec.attributes[v]);
			CHECK(vert.chartIndex >= 0 && (uint32_t)vert.chartIndex < mesh.chartCount);
			if (vert.chartIndex >= 0 && (uint32_t)vert.chartIndex < mesh.chartCount)
				CHECK_EQ_I32(vert.atlasIndex, (int32_t)mesh.chartArray[vert.chartIndex].atlasIndex);
			CHECK(std::isfinite(vert.uv[0]) && std::isfinite(vert.uv[1]));
			CHECK(vert.uv[0] >= -1e-4f && vert.uv[0] <= (float)atlas->width + 1e-4f);
			CHECK(vert.uv[1] >= -1e-4f && vert.uv[1] <= (float)atlas->height + 1e-4f);
		}
	}

	beginCase("uvmesh: overlapping source islands separated after packing");
	checkChartsDoNotOverlap(atlas);

	beginCase("uvmesh: svg dump written");
	{
		const std::string path = joinPath(svgDir, "atlas_geometry_uvmesh.svg");
		CHECK(writeAtlasSvg(atlas, path.c_str()));
		if (g_failureCount == 0)
			printf("       wrote %s\n", path.c_str());
	}

	xatlas::Destroy(atlas);
}

int main(int argc, char **argv)
{
	const std::string svgDir = argc > 1 ? argv[1] : ".";

	std::vector<MeshSpec> specs;
	buildMeshSamples(specs);

	beginCase("addmesh: all samples accepted");
	xatlas::Atlas *atlas = xatlas::Create();
	bool addOk = true;
	for (size_t i = 0; i < specs.size(); ++i)
		addOk = addMeshToAtlas(atlas, specs[i]) && addOk;
	CHECK(addOk);

	xatlas::ChartOptions chartOptions;
	chartOptions.normalSeamWeight = 1001.0f; // hard normal splits must be respected
	xatlas::ComputeCharts(atlas, chartOptions);

	// Pack 1: single growing page. rotateCharts on, small padding, fixed scale.
	xatlas::PackOptions pack1;
	pack1.padding = 1;
	pack1.rotateCharts = true;
	pack1.resolution = 0;
	pack1.texelsPerUnit = 24.0f;
	pack1.createImage = true;
	xatlas::PackCharts(atlas, pack1);
	runFullAtlasChecks(atlas, specs, "pack1", -1, svgDir);

	// Pack 2: same computed charts, all four switched options, forced multi-page.
	// Output pointers are re-acquired inside runFullAtlasChecks; nothing is kept.
	xatlas::PackOptions pack2;
	pack2.padding = 3;
	pack2.rotateCharts = false;
	pack2.resolution = 64;
	pack2.texelsPerUnit = 64.0f;
	pack2.bilinear = false;
	pack2.createImage = true;
	xatlas::PackCharts(atlas, pack2);
	runFullAtlasChecks(atlas, specs, "pack2", 1, svgDir);

	// Pack 3: brute force placement, block aligned, automatic scale/resolution.
	xatlas::PackOptions pack3;
	pack3.padding = 2;
	pack3.rotateCharts = true;
	pack3.resolution = 0;
	pack3.texelsPerUnit = 0.0f;
	pack3.bruteForce = true;
	pack3.blockAlign = true;
	pack3.createImage = true;
	xatlas::PackCharts(atlas, pack3);
	runFullAtlasChecks(atlas, specs, "pack3", -1, svgDir);

	xatlas::Destroy(atlas);

	runUvMeshTest(svgDir);

	printf("ATLAS_GEOMETRY_CASES=%d\n", g_caseCount);
	printf("ATLAS_GEOMETRY_CHECKS=%d\n", g_checkCount);
	printf("ATLAS_GEOMETRY_FAILURES=%d\n", g_failureCount);
	return g_failureCount == 0 ? 0 : 1;
}
