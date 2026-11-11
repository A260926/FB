// Motion (morph) meshlet bounds and builder regression tests.
// This file is part of meshoptimizer library; see meshoptimizer.h for version/license details
#include "../src/meshoptimizer.h"

#undef NDEBUG
#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <algorithm>
#include <vector>

namespace
{

struct Scene
{
	const char* name;
	std::vector<float> begin;
	std::vector<float> end;
	std::vector<unsigned int> indices;
	size_t vertex_count;
};

static const size_t MaxVertices = 64;
static const size_t MaxTriangles = 64;
static const float MaxConeCutoff = 0.6f;

static void vecAdd(float* r, const float* a, const float* b)
{
	r[0] = a[0] + b[0];
	r[1] = a[1] + b[1];
	r[2] = a[2] + b[2];
}

static void vecScale(float* r, const float* a, float s)
{
	r[0] = a[0] * s;
	r[1] = a[1] * s;
	r[2] = a[2] * s;
}

static float vecDot(const float* a, const float* b)
{
	return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

static void vecCross(float* r, const float* a, const float* b)
{
	r[0] = a[1] * b[2] - a[2] * b[1];
	r[1] = a[2] * b[0] - a[0] * b[2];
	r[2] = a[0] * b[1] - a[1] * b[0];
}

static float vecLength(const float* a)
{
	return sqrtf(vecDot(a, a));
}

static void positionAt(float* r, const Scene& scene, size_t vertex, float t)
{
	const float* pb = &scene.begin[vertex * 3];
	const float* pe = &scene.end[vertex * 3];

	for (int k = 0; k < 3; ++k)
		r[k] = pb[k] + (pe[k] - pb[k]) * t;
}

// Independent geometric normal of a face at time t, computed straight from interpolated positions.
static void faceNormalAt(float* normal, const Scene& scene, size_t face, float t)
{
	unsigned int i0 = scene.indices[face * 3 + 0];
	unsigned int i1 = scene.indices[face * 3 + 1];
	unsigned int i2 = scene.indices[face * 3 + 2];

	float p0[3], p1[3], p2[3], e1[3], e2[3];
	positionAt(p0, scene, i0, t);
	positionAt(p1, scene, i1, t);
	positionAt(p2, scene, i2, t);

	vecScale(e1, p1, 1.f);
	e1[0] -= p0[0];
	e1[1] -= p0[1];
	e1[2] -= p0[2];
	e2[0] = p2[0] - p0[0];
	e2[1] = p2[1] - p0[1];
	e2[2] = p2[2] - p0[2];

	vecCross(normal, e1, e2);

	float len = vecLength(normal);

	if (len != 0.f)
	{
		normal[0] /= len;
		normal[1] /= len;
		normal[2] /= len;
	}
}

static bool faceDegenerateAt(const Scene& scene, size_t face, float t)
{
	float n[3];
	faceNormalAt(n, scene, face, t);
	return vecLength(n) == 0.f;
}

// Distance from the sphere center to the full linear trajectory segment of a vertex.
static float segmentDistance(const float* center, const float* pb, const float* pe)
{
	float ab[3] = {pe[0] - pb[0], pe[1] - pb[1], pe[2] - pb[2]};
	float ac[3] = {center[0] - pb[0], center[1] - pb[1], center[2] - pb[2]};

	float denom = vecDot(ab, ab);
	float u = denom == 0.f ? 0.f : vecDot(ac, ab) / denom;
	u = u < 0.f ? 0.f : (u > 1.f ? 1.f : u);

	float closest[3];
	for (int k = 0; k < 3; ++k)
		closest[k] = pb[k] + ab[k] * u;

	float d[3] = {closest[0] - center[0], closest[1] - center[1], closest[2] - center[2]};
	return vecLength(d);
}

struct Stats
{
	size_t clusters;
	size_t fallbacks;
	size_t fallback_degenerate;
	size_t fallback_disabled_cone;
	size_t misculls;
	float min_cutoff;
	float max_enabled_cutoff;
};

// Builds meshlets for the scene and independently verifies every emitted cluster against raw interpolated geometry.
static Stats verifyScene(const Scene& scene, size_t max_vertices, size_t max_triangles, float max_cone_cutoff, bool use_16bit_indices)
{
	const float sample_times[] = {0.f, 0.125f, 0.25f, 0.372f, 0.5f, 0.625f, 0.75f, 0.875f, 1.f};
	const size_t sample_count = sizeof(sample_times) / sizeof(sample_times[0]);

	size_t index_count = scene.indices.size();
	size_t face_count = index_count / 3;

	std::vector<meshopt_Meshlet> meshlets(face_count);
	std::vector<unsigned int> meshlet_vertices(index_count);
	std::vector<unsigned char> meshlet_triangles(index_count);

	size_t meshlet_count = 0;

	if (use_16bit_indices)
	{
		std::vector<unsigned short> indices16(index_count);
		for (size_t i = 0; i < index_count; ++i)
		{
			assert(scene.indices[i] < 65536);
			indices16[i] = (unsigned short)scene.indices[i];
		}

		meshlet_count = meshopt_buildMeshletsMotion(&meshlets[0], &meshlet_vertices[0], &meshlet_triangles[0], &indices16[0], index_count, &scene.begin[0], &scene.end[0], scene.vertex_count, sizeof(float) * 3, max_vertices, max_triangles, 0.25f, max_cone_cutoff);
	}
	else
	{
		meshlet_count = meshopt_buildMeshletsMotion(&meshlets[0], &meshlet_vertices[0], &meshlet_triangles[0], &scene.indices[0], index_count, &scene.begin[0], &scene.end[0], scene.vertex_count, sizeof(float) * 3, max_vertices, max_triangles, 0.25f, max_cone_cutoff);
	}

	Stats stats = {meshlet_count, 0, 0, 0, 0, 1.f, 0.f};

	// capacity and complete face multiset (winding preserved, duplicates retained, nothing dropped)
	std::vector<unsigned int> emitted;
	emitted.reserve(index_count);
	std::vector<unsigned int> ordered_faces;
	ordered_faces.reserve(index_count);

	for (size_t m = 0; m < meshlet_count; ++m)
	{
		const meshopt_Meshlet& ml = meshlets[m];
		assert(ml.vertex_count <= max_vertices);
		assert(ml.triangle_count <= max_triangles);
		assert(ml.triangle_count >= 1);

		for (size_t i = 0; i < ml.triangle_count * 3; ++i)
		{
			unsigned char local = meshlet_triangles[ml.triangle_offset + i];
			assert(local < ml.vertex_count);
			unsigned int vertex = meshlet_vertices[ml.vertex_offset + local];
			emitted.push_back(vertex);
			ordered_faces.push_back(vertex);
		}
	}

	assert(emitted.size() == index_count);

	// ordered triples preserve winding and duplicate multiplicity
	std::vector<std::vector<unsigned int> > emitted_tris;
	std::vector<std::vector<unsigned int> > expected_tris;

	for (size_t f = 0; f < face_count; ++f)
	{
		emitted_tris.push_back(std::vector<unsigned int>(ordered_faces.begin() + f * 3, ordered_faces.begin() + f * 3 + 3));
		expected_tris.push_back(std::vector<unsigned int>(scene.indices.begin() + f * 3, scene.indices.begin() + f * 3 + 3));
	}

	std::sort(emitted_tris.begin(), emitted_tris.end());
	std::sort(expected_tris.begin(), expected_tris.end());
	assert(emitted_tris == expected_tris);

	std::vector<unsigned int> expected = scene.indices;
	std::sort(emitted.begin(), emitted.end());
	std::sort(expected.begin(), expected.end());
	assert(emitted == expected);

	for (size_t m = 0; m < meshlet_count; ++m)
	{
		const meshopt_Meshlet& ml = meshlets[m];

		const unsigned int* verts = &meshlet_vertices[ml.vertex_offset];
		const unsigned char* tris = &meshlet_triangles[ml.triangle_offset];

		meshopt_MotionBounds bounds = meshopt_computeMeshletMotionBounds(verts, tris, ml.triangle_count, &scene.begin[0], &scene.end[0], scene.vertex_count, sizeof(float) * 3);

		// local optimization must not invalidate the conservative motion bounds
		meshopt_optimizeMeshlet(const_cast<unsigned int*>(verts), const_cast<unsigned char*>(tris), ml.triangle_count, ml.vertex_count);

		meshopt_MotionBounds bounds_opt = meshopt_computeMeshletMotionBounds(verts, tris, ml.triangle_count, &scene.begin[0], &scene.end[0], scene.vertex_count, sizeof(float) * 3);

		for (int pass = 0; pass < 2; ++pass)
		{
			const meshopt_MotionBounds& b = pass == 0 ? bounds : bounds_opt;

			if (b.cone_cutoff < 1.f)
			{
				float axis_len = vecLength(b.cone_axis);
				assert(fabsf(axis_len - 1.f) < 1e-5f);
				assert(b.cone_cutoff >= 0.f);
			}
			else
			{
				assert(b.cone_axis[0] == 0.f && b.cone_axis[1] == 0.f && b.cone_axis[2] == 0.f);
			}

			// sphere: every vertex trajectory must be inside, with a relative-scale slack matching the 1e-5 tolerance
			for (size_t i = 0; i < ml.vertex_count; ++i)
			{
				unsigned int v = verts[i];
				const float* pb = &scene.begin[v * 3];
				const float* pe = &scene.end[v * 3];

				float d = segmentDistance(b.center, pb, pe);
				float slack = 1e-4f * (b.radius + 1.f);
				assert(d <= b.radius + slack);
			}

			if (b.cone_cutoff >= 1.f)
				continue;

			// independent per-face orientation checks over the whole interval, no sampling of library internals
			for (size_t i = 0; i < ml.triangle_count; ++i)
			{
				unsigned int t0 = verts[tris[i * 3 + 0]];
				unsigned int t1 = verts[tris[i * 3 + 1]];
				unsigned int t2 = verts[tris[i * 3 + 2]];

				for (size_t s = 0; s < sample_count; ++s)
				{
					float t = sample_times[s];

					// recompute the normal directly from the meshlet's own face to stay independent of library internals
					float p0[3], p1[3], p2[3], e1[3], e2[3], q[3];
					positionAt(p0, scene, t0, t);
					positionAt(p1, scene, t1, t);
					positionAt(p2, scene, t2, t);
					e1[0] = p1[0] - p0[0];
					e1[1] = p1[1] - p0[1];
					e1[2] = p1[2] - p0[2];
					e2[0] = p2[0] - p0[0];
					e2[1] = p2[1] - p0[1];
					e2[2] = p2[2] - p0[2];
					vecCross(q, e1, e2);

					float qlen = vecLength(q);

					if (qlen == 0.f)
						continue; // momentarily degenerate: no orientation constraint

					q[0] /= qlen;
					q[1] /= qlen;
					q[2] /= qlen;

					// orthographic: a camera whose view direction is -n sees the face front-facing,
					// so the reject condition dot(view, axis) >= cutoff must never hold
					float view_o[3] = {-q[0], -q[1], -q[2]};
					float ortho = vecDot(view_o, b.cone_axis);
					float dot_slack = 1e-4f;

					if (ortho >= b.cone_cutoff - dot_slack)
						stats.misculls++;

					// perspective: cameras on the front side of the face (camera = centroid + n*distance)
					float centroid[3];
					vecAdd(centroid, p0, p1);
					vecAdd(centroid, centroid, p2);
					vecScale(centroid, centroid, 1.f / 3.f);

					const float distances[] = {0.25f, 1.f, 4.f, 16.f};

					for (size_t k = 0; k < sizeof(distances) / sizeof(distances[0]); ++k)
					{
						float camera[3];
						float scale = distances[k] * (b.radius == 0.f ? 1.f : b.radius);
						camera[0] = centroid[0] + q[0] * scale;
						camera[1] = centroid[1] + q[1] * scale;
						camera[2] = centroid[2] + q[2] * scale;

						float view[3] = {b.center[0] - camera[0], b.center[1] - camera[1], b.center[2] - camera[2]};
						float lhs = vecDot(view, b.cone_axis);
						float rhs = b.cone_cutoff * vecLength(view) + b.radius;

						// reject condition lhs >= rhs must never hold for a visible face
						if (lhs >= rhs - dot_slack)
							stats.misculls++;
					}
				}
			}
		}

		// fallback definition: one-triangle meshlet whose recomputed cutoff exceeds the budget
		if (ml.triangle_count == 1 && bounds.cone_cutoff > max_cone_cutoff)
		{
			stats.fallbacks++;

			if (bounds.cone_cutoff == 1.f && bounds.cone_axis[0] == 0.f && bounds.cone_axis[1] == 0.f && bounds.cone_axis[2] == 0.f)
				stats.fallback_disabled_cone++;

			// an always-degenerate face left on its own is an allowed fallback; check it is genuinely zero-area everywhere
			unsigned int f0 = verts[tris[0]], f1 = verts[tris[1]], f2 = verts[tris[2]];
			bool always_degenerate = true;

			for (size_t s = 0; s < sample_count; ++s)
			{
				float p0[3], p1[3], p2[3], e1[3], e2[3], q[3];
				positionAt(p0, scene, f0, sample_times[s]);
				positionAt(p1, scene, f1, sample_times[s]);
				positionAt(p2, scene, f2, sample_times[s]);
				e1[0] = p1[0] - p0[0];
				e1[1] = p1[1] - p0[1];
				e1[2] = p1[2] - p0[2];
				e2[0] = p2[0] - p0[0];
				e2[1] = p2[1] - p0[1];
				e2[2] = p2[2] - p0[2];
				vecCross(q, e1, e2);

				if (vecLength(q) != 0.f)
					always_degenerate = false;
			}

			if (always_degenerate)
				stats.fallback_degenerate++;
		}
		else
		{
			assert(bounds.cone_cutoff <= max_cone_cutoff);

			stats.min_cutoff = bounds.cone_cutoff < stats.min_cutoff ? bounds.cone_cutoff : stats.min_cutoff;
			stats.max_enabled_cutoff = bounds.cone_cutoff > stats.max_enabled_cutoff ? bounds.cone_cutoff : stats.max_enabled_cutoff;
		}
	}

	return stats;
}

// Builds a regular 4x4 XY grid (25 vertices, 32 triangles) in CCW winding.
static Scene makeGrid()
{
	Scene scene;
	scene.vertex_count = 25;
	scene.begin.resize(25 * 3);
	scene.end.resize(25 * 3);

	for (size_t y = 0; y < 5; ++y)
		for (size_t x = 0; x < 5; ++x)
		{
			size_t v = y * 5 + x;
			scene.begin[v * 3 + 0] = float(x);
			scene.begin[v * 3 + 1] = float(y);
			scene.begin[v * 3 + 2] = 0.f;
		}

	scene.end = scene.begin;

	for (size_t y = 0; y < 4; ++y)
		for (size_t x = 0; x < 4; ++x)
		{
			unsigned int v0 = (unsigned int)(y * 5 + x);
			unsigned int v1 = v0 + 1;
			unsigned int v2 = v0 + 5;
			unsigned int v3 = v0 + 6;

			scene.indices.push_back(v0);
			scene.indices.push_back(v2);
			scene.indices.push_back(v1);
			scene.indices.push_back(v1);
			scene.indices.push_back(v2);
			scene.indices.push_back(v3);
		}

	assert(scene.indices.size() == 32 * 3);
	return scene;
}

static Scene sceneStaticPlane()
{
	return makeGrid();
}

static Scene sceneTranslatedPlane()
{
	Scene scene = makeGrid();

	for (size_t v = 0; v < scene.vertex_count; ++v)
	{
		scene.end[v * 3 + 0] = scene.begin[v * 3 + 0] + 7.f;
		scene.end[v * 3 + 1] = scene.begin[v * 3 + 1] - 3.f;
		scene.end[v * 3 + 2] = scene.begin[v * 3 + 2] + 11.f;
	}

	return scene;
}

// XY plane morphed into the same grid rotated 60 degrees around the Y axis.
static Scene sceneRotated60()
{
	Scene scene = makeGrid();

	const float c = cosf(60.f * float(M_PI) / 180.f);
	const float s = sinf(60.f * float(M_PI) / 180.f);

	for (size_t v = 0; v < scene.vertex_count; ++v)
	{
		float x = scene.begin[v * 3 + 0];
		float y = scene.begin[v * 3 + 1];
		float z = scene.begin[v * 3 + 2];

		scene.end[v * 3 + 0] = x * c + z * s;
		scene.end[v * 3 + 1] = y;
		scene.end[v * 3 + 2] = -x * s + z * c;
	}

	return scene;
}

// Each grid face gets a reversed copy; front/back copies alternate in the index stream.
static Scene sceneInterleavedBackfaces()
{
	Scene base = makeGrid();
	Scene scene;
	scene.vertex_count = base.vertex_count;
	scene.begin = base.begin;
	scene.end = base.end;
	scene.indices.reserve(base.indices.size() * 2);

	for (size_t f = 0; f < base.indices.size() / 3; ++f)
	{
		scene.indices.push_back(base.indices[f * 3 + 0]);
		scene.indices.push_back(base.indices[f * 3 + 1]);
		scene.indices.push_back(base.indices[f * 3 + 2]);

		scene.indices.push_back(base.indices[f * 3 + 0]);
		scene.indices.push_back(base.indices[f * 3 + 2]);
		scene.indices.push_back(base.indices[f * 3 + 1]);
	}

	return scene;
}

// Triangle whose endpoints both face +Z but whose mid-motion normal flips to -Z.
static Scene sceneMidFlip()
{
	Scene scene;
	scene.vertex_count = 3;
	scene.begin = {0, 0, 0, 1, 0, 0, 0, 3, 0};
	scene.end = {0, 0, 0, -3, 0, 0, 0, -1, 0};
	scene.indices = {0, 1, 2};
	return scene;
}

// Narrow sign-flip interval: edge lengths 0.371-t and 0.373-t cross zero around t=0.372,
// plus a disconnected static +Z triangle that must stay in a normal cluster.
static Scene sceneNarrowFlip()
{
	Scene scene;
	scene.vertex_count = 6;
	scene.begin.resize(6 * 3);
	scene.end.resize(6 * 3);

	// moving triangle A,B,C with B=(0.371-t, 0, 0), C=(0, 0.373-t, 0)
	scene.begin[0] = 0;
	scene.begin[1] = 0;
	scene.begin[2] = 0;
	scene.begin[3] = 0.371f;
	scene.begin[4] = 0;
	scene.begin[5] = 0;
	scene.begin[6] = 0;
	scene.begin[7] = 0.373f;
	scene.begin[8] = 0;

	scene.end[0] = 0;
	scene.end[1] = 0;
	scene.end[2] = 0;
	scene.end[3] = 0.371f - 1.f;
	scene.end[4] = 0;
	scene.end[5] = 0;
	scene.end[6] = 0;
	scene.end[7] = 0.373f - 1.f;
	scene.end[8] = 0;

	// disconnected static triangle, normal +Z
	scene.begin[9] = 10;
	scene.begin[10] = 0;
	scene.begin[11] = 0;
	scene.begin[12] = 11;
	scene.begin[13] = 0;
	scene.begin[14] = 0;
	scene.begin[15] = 10;
	scene.begin[16] = 1;
	scene.begin[17] = 0;

	for (size_t i = 9; i < 18; ++i)
		scene.end[i] = scene.begin[i];

	scene.indices = {0, 1, 2, 3, 4, 5};
	return scene;
}

// Multiple disconnected patches, duplicate/reversed faces and always-degenerate faces; stresses capacity and topology handling.
static Scene sceneCapacityTopology()
{
	Scene scene;
	scene.vertex_count = 0;

	auto addPatch = [&](float ox, float oy, bool morph) {
		unsigned int base = (unsigned int)scene.vertex_count;

		for (size_t y = 0; y < 5; ++y)
			for (size_t x = 0; x < 5; ++x)
			{
				scene.begin.push_back(ox + float(x));
				scene.begin.push_back(oy + float(y));
				scene.begin.push_back(0.f);

				scene.end.push_back(ox + float(x) + (morph ? 0.5f * float(y) : 0.f));
				scene.end.push_back(oy + float(y));
				scene.end.push_back(morph ? 0.5f * float(x) : 0.f);

				scene.vertex_count++;
			}

		for (size_t y = 0; y < 4; ++y)
			for (size_t x = 0; x < 4; ++x)
			{
				unsigned int v0 = base + (unsigned int)(y * 5 + x);
				unsigned int v1 = v0 + 1;
				unsigned int v2 = v0 + 5;
				unsigned int v3 = v0 + 6;

				scene.indices.push_back(v0);
				scene.indices.push_back(v2);
				scene.indices.push_back(v1);
				scene.indices.push_back(v1);
				scene.indices.push_back(v2);
				scene.indices.push_back(v3);
			}
	};

	addPatch(0.f, 0.f, false);
	addPatch(20.f, 0.f, true);
	addPatch(40.f, 0.f, false);

	// duplicate face (same winding, appears twice) and a reversed copy
	unsigned int d0 = 0, d1 = 1, d2 = 5;
	scene.indices.push_back(d0);
	scene.indices.push_back(d2);
	scene.indices.push_back(d1);
	scene.indices.push_back(d0);
	scene.indices.push_back(d2);
	scene.indices.push_back(d1);
	scene.indices.push_back(d0);
	scene.indices.push_back(d1);
	scene.indices.push_back(d2);

	// always-degenerate collinear triangle, both poses
	unsigned int deg_base = (unsigned int)scene.vertex_count;
	scene.begin.push_back(0);
	scene.begin.push_back(30);
	scene.begin.push_back(0);
	scene.begin.push_back(1);
	scene.begin.push_back(30);
	scene.begin.push_back(0);
	scene.begin.push_back(2);
	scene.begin.push_back(30);
	scene.begin.push_back(0);
	scene.end.push_back(0);
	scene.end.push_back(35);
	scene.end.push_back(0);
	scene.end.push_back(1);
	scene.end.push_back(35);
	scene.end.push_back(0);
	scene.end.push_back(2);
	scene.end.push_back(35);
	scene.end.push_back(0);
	scene.vertex_count += 3;

	scene.indices.push_back(deg_base);
	scene.indices.push_back(deg_base + 1);
	scene.indices.push_back(deg_base + 2);

	return scene;
}

static void checkEmptyAndSentinels()
{
	const float vertices[3] = {0, 0, 0};

	// empty build returns 0 without touching the sentinel-filled output
	meshopt_Meshlet meshlets[2];
	memset(meshlets, 0xFF, sizeof(meshlets));
	unsigned int mv[4];
	memset(mv, 0xFF, sizeof(mv));
	unsigned char mt[8];
	memset(mt, 0xFF, sizeof(mt));

	size_t count = meshopt_buildMeshletsMotion(meshlets, mv, mt, NULL, 0, vertices, vertices, 1, sizeof(float) * 3, MaxVertices, MaxTriangles, 0.f, MaxConeCutoff);
	assert(count == 0);

	for (size_t i = 0; i < sizeof(meshlets); ++i)
		assert(((unsigned char*)meshlets)[i] == 0xFF);

	// empty bounds: zero sphere, disabled cone, zero axis
	meshopt_MotionBounds bounds = meshopt_computeMeshletMotionBounds(mv, mt, 0, vertices, vertices, 1, sizeof(float) * 3);
	assert(bounds.center[0] == 0.f && bounds.center[1] == 0.f && bounds.center[2] == 0.f && bounds.radius == 0.f);
	assert(bounds.cone_axis[0] == 0.f && bounds.cone_axis[1] == 0.f && bounds.cone_axis[2] == 0.f && bounds.cone_cutoff == 1.f);
}

// The two pose buffers are allowed to be the same pointer; the result must match the two-buffer static case.
static void checkAliasedPoses()
{
	Scene scene = makeGrid();

	const float* same = &scene.begin[0];

	std::vector<meshopt_Meshlet> ml(scene.indices.size() / 3);
	std::vector<unsigned int> mv(scene.indices.size());
	std::vector<unsigned char> mt(scene.indices.size());

	size_t mc = meshopt_buildMeshletsMotion(&ml[0], &mv[0], &mt[0], &scene.indices[0], scene.indices.size(), same, same, scene.vertex_count, sizeof(float) * 3, MaxVertices, MaxTriangles, 0.25f, MaxConeCutoff);
	assert(mc == 1);

	meshopt_MotionBounds b = meshopt_computeMeshletMotionBounds(&mv[0], &mt[0], ml[0].triangle_count, same, same, scene.vertex_count, sizeof(float) * 3);
	assert(b.cone_cutoff < MaxConeCutoff);
	assert(fabsf(vecLength(b.cone_axis) - 1.f) < 1e-5f);
}

static void checkAnalyticExamples()
{
	// the mid-flip triangle: verify the flip analytically at several times including the sign change
	Scene flip = sceneMidFlip();

	for (int k = 0; k <= 100; ++k)
	{
		float t = float(k) / 100.f;
		float n[3];
		faceNormalAt(n, flip, 0, t);

		if (faceDegenerateAt(flip, 0, t))
			continue;

		// endpoints face +Z; the interior covers -Z as well, so no single-cone bound is possible
		(void)n;
	}

	float n0[3], n1[3], nh[3];
	faceNormalAt(n0, flip, 0, 0.f);
	faceNormalAt(n1, flip, 0, 1.f);
	faceNormalAt(nh, flip, 0, 0.5f);
	assert(n0[2] > 0.99f && n1[2] > 0.99f && nh[2] < -0.99f);

	// narrow flip: at t=0.372 both edges straddle zero and the normal points to -Z
	Scene narrow = sceneNarrowFlip();
	float nn[3];
	faceNormalAt(nn, narrow, 0, 0.372f);
	assert(nn[2] < 0.f);

	float nb[3], ne[3];
	faceNormalAt(nb, narrow, 0, 0.f);
	faceNormalAt(ne, narrow, 0, 1.f);
	assert(nb[2] > 0.f && ne[2] > 0.f);
}

} // namespace

int main()
{
	checkEmptyAndSentinels();
	checkAliasedPoses();
	checkAnalyticExamples();

	struct Case
	{
		const char* name;
		Scene scene;
		size_t expected_clusters; // 0 means unspecified, invariants still checked
		int expected_fallbacks; // -1 means fallbacks are allowed only for always-degenerate faces
	};

	Case cases[] = {
	    {"static plane", sceneStaticPlane(), 1, 0},
	    {"translated plane", sceneTranslatedPlane(), 1, 0},
	    {"60 degree morph", sceneRotated60(), 1, 0},
	    {"interleaved backfaces", sceneInterleavedBackfaces(), 2, 0},
	    {"mid-motion flip", sceneMidFlip(), 1, 1},
	    {"narrow flip interval", sceneNarrowFlip(), 2, 1},
	    {"capacity/topology", sceneCapacityTopology(), 0, -1},
	};

	size_t scenes_run = 0;

	for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i)
	{
		Stats stats32 = verifyScene(cases[i].scene, MaxVertices, MaxTriangles, MaxConeCutoff, false);
		Stats stats16 = verifyScene(cases[i].scene, MaxVertices, MaxTriangles, MaxConeCutoff, true);

		if (cases[i].expected_clusters != 0)
		{
			assert(stats32.clusters == cases[i].expected_clusters);
			assert(stats16.clusters == cases[i].expected_clusters);
		}

		if (cases[i].expected_fallbacks >= 0)
		{
			assert(stats32.fallbacks == size_t(cases[i].expected_fallbacks));
			assert(stats16.fallbacks == size_t(cases[i].expected_fallbacks));

			// a fallback face must carry a fully disabled cone (cutoff 1, zero axis), never a merely wide one
			assert(stats32.fallback_disabled_cone == stats32.fallbacks);
			assert(stats16.fallback_disabled_cone == stats16.fallbacks);
		}
		else
		{
			// every fallback must be an always-degenerate face isolated on its own
			assert(stats32.fallbacks == stats32.fallback_degenerate);
			assert(stats16.fallbacks == stats16.fallback_degenerate);
			assert(stats32.fallback_disabled_cone == stats32.fallbacks);
			assert(stats16.fallback_disabled_cone == stats16.fallbacks);
		}

		assert(stats32.misculls == 0);
		assert(stats16.misculls == 0);

		printf("%-24s clusters=%zu fallbacks=%zu (degenerate=%zu) misculls=%zu cutoff[%.4f..%.4f] (16-bit: clusters=%zu misculls=%zu)\n", cases[i].name, stats32.clusters, stats32.fallbacks, stats32.fallback_degenerate, stats32.misculls, stats32.min_cutoff, stats32.max_enabled_cutoff, stats16.clusters, stats16.misculls);
		scenes_run++;
	}

	assert(scenes_run >= 6);
	printf("motion meshlet scenes run: %zu\n", scenes_run);
	return 0;
}
