// Surface error certification tests; see task_support/check.h for the driver protocol
#include "task_support/check.h"
#include "src/meshoptimizer.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <vector>

namespace
{

// ---------- small double precision geometry used as an independent reference ----------

struct P3
{
	double x, y, z;
};

static P3 sub(const P3& a, const P3& b)
{
	P3 r = {a.x - b.x, a.y - b.y, a.z - b.z};
	return r;
}

static double dot3(const P3& a, const P3& b)
{
	return a.x * b.x + a.y * b.y + a.z * b.z;
}

static double len3(const P3& a)
{
	return sqrt(dot3(a, a));
}

// distance from p to the closed triangle (a,b,c); independent implementation for test reference
static double refPointTriangle(const P3& p, const P3& a, const P3& b, const P3& c)
{
	// project and clamp in barycentric-ish fashion
	P3 ab = sub(b, a), ac = sub(c, a), ap = sub(p, a);

	double d1 = dot3(ab, ap), d2 = dot3(ac, ap);
	if (d1 <= 0 && d2 <= 0)
		return len3(ap);

	P3 bp = sub(p, b);
	double d3 = dot3(ab, bp), d4 = dot3(ac, bp);
	if (d3 >= 0 && d4 <= d3)
		return len3(bp);

	double vc = d1 * d4 - d3 * d2;
	if (vc <= 0 && d1 >= 0 && d3 <= 0)
	{
		double v = d1 / (d1 - d3);
		P3 q = {a.x + ab.x * v, a.y + ab.y * v, a.z + ab.z * v};
		return len3(sub(p, q));
	}

	P3 cp = sub(p, c);
	double d5 = dot3(ab, cp), d6 = dot3(ac, cp);
	if (d6 >= 0 && d5 <= d6)
		return len3(cp);

	double vb = d5 * d2 - d1 * d6;
	if (vb <= 0 && d2 >= 0 && d6 <= 0)
	{
		double w = d2 / (d2 - d6);
		P3 q = {a.x + ac.x * w, a.y + ac.y * w, a.z + ac.z * w};
		return len3(sub(p, q));
	}

	double va = d3 * d6 - d5 * d4;
	if (va <= 0 && (d4 - d3) >= 0 && (d5 - d6) >= 0)
	{
		double w = (d4 - d3) / ((d4 - d3) + (d5 - d6));
		P3 bc = sub(c, b);
		P3 q = {b.x + bc.x * w, b.y + bc.y * w, b.z + bc.z * w};
		return len3(sub(p, q));
	}

	double denom = va + vb + vc;
	if (denom == 0)
		return 1e30;

	double v = vb / denom, w = vc / denom;
	P3 q = {a.x + ab.x * v + ac.x * w, a.y + ab.y * v + ac.y * w, a.z + ab.z * v + ac.z * w};
	return len3(sub(p, q));
}

struct Mesh
{
	std::vector<float> positions; // 3 per vertex
	std::vector<unsigned int> indices;
	std::vector<float> attributes; // 2 per vertex
	size_t vertex_count() const { return positions.size() / 3; }
};

static P3 vertex(const Mesh& m, unsigned int v)
{
	P3 p = {m.positions[v * 3 + 0], m.positions[v * 3 + 1], m.positions[v * 3 + 2]};
	return p;
}

// brute force one-directional Hausdorff estimate used only to cross-check the certified bound; not used by the algorithm
static double referenceDirected(const Mesh& a, const Mesh& b, int samples)
{
	double worst = 0;

	for (size_t f = 0; f < a.indices.size(); f += 3)
	{
		P3 p0 = vertex(a, a.indices[f]), p1 = vertex(a, a.indices[f + 1]), p2 = vertex(a, a.indices[f + 2]);

		for (int i = 0; i <= samples; ++i)
			for (int j = 0; i + j <= samples; ++j)
			{
				double u = double(i) / samples, v = double(j) / samples, w = 1 - u - v;
				P3 p = {p0.x * w + p1.x * u + p2.x * v, p0.y * w + p1.y * u + p2.y * v, p0.z * w + p1.z * u + p2.z * v};

				double best = 1e30;

				for (size_t g = 0; g < b.indices.size(); g += 3)
				{
					P3 q0 = vertex(b, b.indices[g]), q1 = vertex(b, b.indices[g + 1]), q2 = vertex(b, b.indices[g + 2]);
					double d = refPointTriangle(p, q0, q1, q2);
					best = best < d ? best : d;
				}

				worst = worst < best ? best : worst;
			}
	}

	return worst;
}

static Mesh assemble(const Mesh& src, const std::vector<unsigned int>& indices, size_t count)
{
	Mesh r = src;
	r.indices.assign(indices.begin(), indices.begin() + count);
	return r;
}

// ---------- mesh generators ----------

// flat unit grid, optional narrow tall bump at the center vertex
static Mesh gridMesh(unsigned int n, float bump_height)
{
	Mesh m;

	for (unsigned int j = 0; j <= n; ++j)
		for (unsigned int i = 0; i <= n; ++i)
		{
			float x = float(i) / n, y = float(j) / n, z = 0;

			if (bump_height != 0 && i == n / 2 && j == n / 2)
				z = bump_height;

			m.positions.push_back(x);
			m.positions.push_back(y);
			m.positions.push_back(z);
			m.attributes.push_back(x);
			m.attributes.push_back(y);
		}

	for (unsigned int j = 0; j < n; ++j)
		for (unsigned int i = 0; i < n; ++i)
		{
			unsigned int a = j * (n + 1) + i, b = a + 1, c = a + n + 1, d = c + 1;
			unsigned int f[] = {a, b, d, a, d, c};
			m.indices.insert(m.indices.end(), f, f + 6);
		}

	return m;
}

// folded trough: a long strip folded along the middle with a narrow crest; result collapses across the fold would cut the crest
static Mesh troughMesh(unsigned int n, float crest)
{
	Mesh m;

	for (unsigned int j = 0; j <= n; ++j)
		for (unsigned int i = 0; i <= n; ++i)
		{
			float x = float(i) / n * 2 - 1;
			float y = float(j) / n;
			float z = -fabs(y - 0.5f) * 2; // V shaped trough in z
			// narrow elevated crest along the trough bottom
			if (i == n / 2)
				z += crest;

			m.positions.push_back(x);
			m.positions.push_back(y);
			m.positions.push_back(z);
			m.attributes.push_back(x);
			m.attributes.push_back(y);
		}

	for (unsigned int j = 0; j < n; ++j)
		for (unsigned int i = 0; i < n; ++i)
		{
			unsigned int a = j * (n + 1) + i, b = a + 1, c = a + n + 1, d = c + 1;
			unsigned int f[] = {a, b, d, a, d, c};
			m.indices.insert(m.indices.end(), f, f + 6);
		}

	return m;
}

struct Args
{
	const Mesh* mesh;
	const unsigned char* locks;
	size_t target;
	float target_error;
	unsigned int options;
	float max_surface_error;
	unsigned int max_depth;
};

static size_t run(const Args& args, std::vector<unsigned int>& out, float& error, meshopt_SurfaceErrorStats& stats)
{
	out.assign(args.mesh->indices.size(), 0);
	const float weights[2] = {0.1f, 0.1f};

	error = -1;
	memset(&stats, 0, sizeof(stats));

	return meshopt_simplifyWithSurfaceError(out.data(), args.mesh->indices.data(), args.mesh->indices.size(), args.mesh->positions.data(), args.mesh->vertex_count(), sizeof(float) * 3, args.mesh->attributes.data(), sizeof(float) * 2, weights, 2, args.locks, args.target, args.target_error, args.options, &error, args.max_surface_error, args.max_depth, &stats);
}

// checks that every output index is in range and every triangle has positive area, and that the output is non-empty for non-empty input
static bool validOutput(const Mesh& src, const std::vector<unsigned int>& out, size_t count)
{
	if (count == 0)
		return src.indices.empty();
	if (count % 3)
		return false;

	for (size_t i = 0; i < count; ++i)
		if (out[i] >= src.vertex_count())
			return false;

	for (size_t i = 0; i < count; i += 3)
	{
		P3 a = vertex(src, out[i]), b = vertex(src, out[i + 1]), c = vertex(src, out[i + 2]);
		P3 ab = sub(b, a), ac = sub(c, a);
		P3 n = {ab.y * ac.z - ab.z * ac.y, ab.z * ac.x - ab.x * ac.z, ab.x * ac.y - ab.y * ac.x};
		if (len3(n) <= 0)
			return false;
	}

	return true;
}

} // namespace

// ---------- continuous group: certify that the bound really holds for the final surface ----------

static int caseContinuousPlane()
{
	Mesh m = gridMesh(16, 0);
	Args a = {&m, NULL, 128 * 3, 1.f, meshopt_SimplifyLockBorder | meshopt_SimplifyErrorAbsolute, 0.01f, 16};
	std::vector<unsigned int> out;
	float error = -1;
	meshopt_SurfaceErrorStats stats;
	size_t count = run(a, out, error, stats);

	TASK_CHECK(count > 0);
	TASK_CHECK(count / 3 <= 256);
	TASK_CHECK(validOutput(m, out, count));
	TASK_CHECK(stats.certified_error <= 0.01 + 1e-9);
	// flat plane: any simplification keeps distance ~0, and the bound must be conservative
	TASK_CHECK(stats.certified_error >= 0);

	// independent geometric reference: both directions of the final surface are within the certified bound
	Mesh result = assemble(m, out, count);
	double d0 = referenceDirected(m, result, 8);
	double d1 = referenceDirected(result, m, 8);
	TASK_CHECK(d0 <= stats.certified_error + 1e-6);
	TASK_CHECK(d1 <= stats.certified_error + 1e-6);

	return 0;
}

static int caseContinuousBumpInterior()
{
	// a narrow tall bump: all original vertices stay in place for small collapses, but a collapse of the tip
	// would make new triangle interiors deviate much more than the vertex-only check suggests
	Mesh m = gridMesh(8, 0.4f);
	Args a = {&m, NULL, 0, 0.02f, 0, 0.01f, 20};
	std::vector<unsigned int> out;
	float error = -1;
	meshopt_SurfaceErrorStats stats;
	size_t count = run(a, out, error, stats);

	TASK_CHECK(count > 0);
	TASK_CHECK(validOutput(m, out, count));
	TASK_CHECK(stats.certified_error <= 0.01 + 1e-9);
	TASK_CHECK(stats.rejected_distance + stats.rejected_budget > 0);

	Mesh result = assemble(m, out, count);
	double d0 = referenceDirected(m, result, 10);
	double d1 = referenceDirected(result, m, 10);
	TASK_CHECK(d0 <= stats.certified_error + 1e-6);
	TASK_CHECK(d1 <= stats.certified_error + 1e-6);

	return 0;
}

static int caseContinuousTrough()
{
	// folded trough with a narrow crest: the crest is a thin protrusion; collapsing across it would remove it,
	// so a small max_surface_error must stop before the crest disappears
	Mesh m = troughMesh(8, 0.3f);
	Args a = {&m, NULL, 0, 0.02f, 0, 0.005f, 20};
	std::vector<unsigned int> out;
	float error = -1;
	meshopt_SurfaceErrorStats stats;
	size_t count = run(a, out, error, stats);

	TASK_CHECK(count > 0);
	TASK_CHECK(validOutput(m, out, count));
	TASK_CHECK(stats.certified_error <= 0.005 + 1e-9);

	Mesh result = assemble(m, out, count);
	double d0 = referenceDirected(m, result, 8);
	double d1 = referenceDirected(result, m, 8);
	TASK_CHECK(d0 <= stats.certified_error + 1e-6);
	TASK_CHECK(d1 <= stats.certified_error + 1e-6);

	return 0;
}

static int caseContinuousZero()
{
	// zero tolerance: only exactly preserving changes may be accepted; for a flat plane the result still shrinks
	Mesh m = gridMesh(4, 0);
	Args a = {&m, NULL, 0, 1.f, 0, 0.0f, 16};
	std::vector<unsigned int> out;
	float error = -1;
	meshopt_SurfaceErrorStats stats;
	size_t count = run(a, out, error, stats);

	TASK_CHECK(validOutput(m, out, count));
	TASK_CHECK(stats.certified_error <= 0);

	// certified error of exactly zero is only possible for coplanar geometry
	TASK_CHECK(stats.certified_error <= 1e-12);

	return 0;
}

// ---------- invalidation group ----------

static int caseInvalidationWitnessReplacement()
{
	// a plane with several bumps: certified leaves of the original surface point at target faces; as collapses
	// happen, witness faces disappear and the certifier must find other witnesses or reject
	Mesh m = gridMesh(8, 0.2f);
	Args a = {&m, NULL, 32 * 3, 0.05f, 0, 0.01f, 18};
	std::vector<unsigned int> out;
	float error = -1;
	meshopt_SurfaceErrorStats stats;
	size_t count = run(a, out, error, stats);

	TASK_CHECK(count > 0);
	TASK_CHECK(validOutput(m, out, count));
	TASK_CHECK(stats.certified_error <= 0.01 + 1e-9);
	TASK_CHECK(stats.invalidated_certificates > 0);

	return 0;
}

static int caseInvalidationNoWitness()
{
	// same plane but with a max_surface_error small enough that the bump cannot be flattened; some candidates
	// must be rejected because no replacement witness exists
	Mesh m = gridMesh(6, 0.5f);
	Args a = {&m, NULL, 0, 0.05f, 0, 0.002f, 20};
	std::vector<unsigned int> out;
	float error = -1;
	meshopt_SurfaceErrorStats stats;
	size_t count = run(a, out, error, stats);

	TASK_CHECK(count > 0);
	TASK_CHECK(validOutput(m, out, count));
	TASK_CHECK(stats.rejected_distance + stats.rejected_budget > 0);
	TASK_CHECK(stats.certified_error <= 0.002 + 1e-9);

	return 0;
}

static int caseInvalidationDistanceRejection()
{
	// a single quad (two triangles) with a huge QEM target error: collapsing one edge would leave a triangle that
	// covers only half of the original surface, provably more than 0.4 away from the removed corner
	Mesh m = gridMesh(1, 0);
	Args a = {&m, NULL, 0, 1e6f, 0, 0.4f, 16};
	std::vector<unsigned int> out;
	float error = -1;
	meshopt_SurfaceErrorStats stats;
	size_t count = run(a, out, error, stats);

	TASK_CHECK(count > 0);
	TASK_CHECK(validOutput(m, out, count));
	TASK_CHECK(stats.rejected_distance > 0);
	TASK_CHECK(stats.certified_error <= 0.4 + 1e-9);

	return 0;
}

static int caseInvalidationSeamCopies()
{
	// attribute seam: the middle column of a flat grid is split into two coincident vertices with different
	// attributes; triangles on either side reference different copies, so collapses have to move both together
	Mesh m;
	unsigned int n = 6;

	for (unsigned int j = 0; j <= n; ++j)
		for (unsigned int i = 0; i <= n; ++i)
		{
			float x = float(i) / n, y = float(j) / n;

			for (int copy = 0; copy <= (i == n / 2 ? 1 : 0); ++copy)
			{
				m.positions.push_back(x);
				m.positions.push_back(y);
				m.positions.push_back(0.0f);
				m.attributes.push_back(x + 0.25f * copy);
				m.attributes.push_back(y);
			}
		}

	// per-row vertex ids: row_base[i] is the id of column i copy 0; the seam column n/2 also has copy 1 at +1
	std::vector<unsigned int> row_base(n + 1);
	unsigned int per_row = 0;
	for (unsigned int i = 0; i <= n; ++i)
	{
		row_base[i] = per_row;
		per_row += (i == n / 2 ? 2 : 1);
	}
	TASK_CHECK(per_row == n + 2);

	for (unsigned int j = 0; j < n; ++j)
		for (unsigned int i = 0; i < n; ++i)
		{
			unsigned int left_copy = i < n / 2 ? 0 : 1;
			unsigned int right_copy = i + 1 <= n / 2 ? 0 : 1;

			if (i == n / 2)
				continue; // the quad straddling the seam does not exist; the seam column is shared by both sides

			unsigned int a = j * per_row + row_base[i] + (i == n / 2 ? left_copy : 0);
			unsigned int b = j * per_row + row_base[i + 1] + (i + 1 == n / 2 ? right_copy : 0);
			unsigned int c = (j + 1) * per_row + row_base[i] + (i == n / 2 ? left_copy : 0);
			unsigned int d = (j + 1) * per_row + row_base[i + 1] + (i + 1 == n / 2 ? right_copy : 0);

			unsigned int f[] = {a, b, d, a, d, c};
			m.indices.insert(m.indices.end(), f, f + 6);
		}

	Args a = {&m, NULL, 32 * 3, 0.1f, 0, 0.01f, 16};
	std::vector<unsigned int> out;
	float error = -1;
	meshopt_SurfaceErrorStats stats;
	size_t count = run(a, out, error, stats);

	TASK_CHECK(count > 0);
	TASK_CHECK(validOutput(m, out, count));
	TASK_CHECK(stats.certified_error <= 0.01 + 1e-9);
	TASK_CHECK(stats.accepted_collapses > 0);

	return 0;
}

static int caseInvalidationCompaction()
{
	// the destination aliases a separately copied index buffer and is remapped in place; face identity and
	// dependencies must still line up after the final compaction
	Mesh m = gridMesh(8, 0.15f);
	std::vector<unsigned int> indices = m.indices;
	const float weights[2] = {0.1f, 0.1f};
	float error = -1;
	meshopt_SurfaceErrorStats stats;
	memset(&stats, 0, sizeof(stats));

	size_t count = meshopt_simplifyWithSurfaceError(indices.data(), indices.data(), indices.size(), m.positions.data(), m.vertex_count(), sizeof(float) * 3, m.attributes.data(), sizeof(float) * 2, weights, 2, NULL, 48 * 3, 0.1f, 0, &error, 0.01f, 16, &stats);

	TASK_CHECK(count > 0);
	TASK_CHECK(validOutput(m, indices, count));
	TASK_CHECK(stats.certified_error <= 0.01 + 1e-9);

	return 0;
}

// ---------- budget group ----------

static int caseBudgetDepthLimit()
{
	// a flat plane with the maximum subdivision depth very small; the certifier still has to accept some
	// collapses and continue legally reducing triangles, while never exceeding the bound
	Mesh m = gridMesh(8, 0);
	Args a = {&m, NULL, 32 * 3, 0.5f, 0, 0.01f, 4};
	std::vector<unsigned int> out;
	float error = -1;
	meshopt_SurfaceErrorStats stats;
	size_t count = run(a, out, error, stats);

	TASK_CHECK(count > 0);
	TASK_CHECK(validOutput(m, out, count));
	TASK_CHECK(count < m.indices.size()); // still reduced
	TASK_CHECK(count <= 32 * 3);
	TASK_CHECK(stats.accepted_collapses > 0);
	TASK_CHECK(stats.certified_error <= 0.01 + 1e-9);

	return 0;
}

static int caseBudgetDepthSubdivisions()
{
	// a curved surface with a tight depth limit: root cells cannot be proven, subdivision must be used,
	// and collapses that still fit the (small) distance bound continue to be accepted
	Mesh m = gridMesh(8, 0.15f);
	Args a = {&m, NULL, 32 * 3, 0.5f, 0, 0.02f, 3};
	std::vector<unsigned int> out;
	float error = -1;
	meshopt_SurfaceErrorStats stats;
	size_t count = run(a, out, error, stats);

	TASK_CHECK(count > 0);
	TASK_CHECK(validOutput(m, out, count));
	TASK_CHECK(count < m.indices.size());
	TASK_CHECK(stats.accepted_collapses > 0);
	TASK_CHECK(stats.certified_error <= 0.02 + 1e-9);

	return 0;
}

static int caseBudgetDepthZero()
{
	// depth 0: no subdivision; a flat plane can still be proven at the root
	Mesh m = gridMesh(6, 0);
	Args a = {&m, NULL, 0, 1.f, 0, 0.01f, 0};
	std::vector<unsigned int> out;
	float error = -1;
	meshopt_SurfaceErrorStats stats;
	size_t count = run(a, out, error, stats);

	TASK_CHECK(count > 0);
	TASK_CHECK(validOutput(m, out, count));
	TASK_CHECK(stats.certified_error <= 0.01 + 1e-9);

	return 0;
}

static int caseBudgetSamplingGrowth()
{
	// two identical 4x4 grids placed far apart; the number of distance_tests per accepted collapse must not
	// grow proportionally to the number of shells (excludes per-candidate full-mesh scans)
	std::vector<float> tests_per_collapse;

	for (int shells = 16; shells <= 64; shells *= 4)
	{
		Mesh m;
		unsigned int n = 4;

		for (int s = 0; s < shells; ++s)
		{
			float ox = float(s % 8) * 100.f, oy = float(s / 8) * 100.f;
			unsigned int base = unsigned(m.vertex_count());

			for (unsigned int j = 0; j <= n; ++j)
				for (unsigned int i = 0; i <= n; ++i)
				{
					m.positions.push_back(ox + float(i) / n);
					m.positions.push_back(oy + float(j) / n);
					m.positions.push_back(0);
					m.attributes.push_back(float(i) / n);
					m.attributes.push_back(float(j) / n);
				}

			for (unsigned int j = 0; j < n; ++j)
				for (unsigned int i = 0; i < n; ++i)
				{
					unsigned int a = base + j * (n + 1) + i, b = a + 1, c = a + n + 1, d = c + 1;
					unsigned int f[] = {a, b, d, a, d, c};
					m.indices.insert(m.indices.end(), f, f + 6);
				}
		}

		// far-apart shells means the source and target surfaces are entirely disconnected; certified distance is finite
		// because each shell maps to itself
		Args a = {&m, NULL, 6 * unsigned(m.indices.size()) / 6, 0.5f, 0, 0.01f, 10};
		// reduce each shell to 2 triangles: 16 shells -> 32 triangles
		a.target = size_t(shells) * 2 * 3;

		std::vector<unsigned int> out;
		float error = -1;
		meshopt_SurfaceErrorStats stats;
		size_t count = run(a, out, error, stats);

		TASK_CHECK(count > 0);
		TASK_CHECK(validOutput(m, out, count));
		TASK_CHECK(stats.accepted_collapses > 0);

		tests_per_collapse.push_back(double(stats.distance_tests) / double(stats.accepted_collapses));
	}

	TASK_CHECK(tests_per_collapse.size() == 2);
	TASK_CHECK(tests_per_collapse[1] < tests_per_collapse[0] * 3.0);

	return 0;
}

// ---------- compatibility group ----------

static int caseCompatibilityDisabled()
{
	Mesh m = gridMesh(8, 0.1f);
	std::vector<unsigned int> out_new(m.indices.size()), out_ref(m.indices.size());
	float err_new = -1, err_ref = -1;
	meshopt_SurfaceErrorStats stats;
	memset(&stats, 0, sizeof(stats));

	const float weights[2] = {0.1f, 0.1f};

	size_t a = meshopt_simplifyWithSurfaceError(out_new.data(), m.indices.data(), m.indices.size(), m.positions.data(), m.vertex_count(), sizeof(float) * 3, m.attributes.data(), sizeof(float) * 2, weights, 2, NULL, 128 * 3, 0.5f, meshopt_SimplifyLockBorder | meshopt_SimplifyErrorAbsolute, &err_new, -1.f, 8, &stats);
	size_t b = meshopt_simplifyWithAttributes(out_ref.data(), m.indices.data(), m.indices.size(), m.positions.data(), m.vertex_count(), sizeof(float) * 3, m.attributes.data(), sizeof(float) * 2, weights, 2, NULL, 128 * 3, 0.5f, meshopt_SimplifyLockBorder | meshopt_SimplifyErrorAbsolute, &err_ref);

	TASK_CHECK(a == b);
	TASK_CHECK(memcmp(out_new.data(), out_ref.data(), a * sizeof(unsigned int)) == 0);
	TASK_CHECK(err_new == err_ref);
	TASK_CHECK(stats.accepted_collapses == 0);
	TASK_CHECK(stats.rejected_distance == 0);
	TASK_CHECK(stats.rejected_budget == 0);
	TASK_CHECK(stats.invalidated_certificates == 0);
	TASK_CHECK(stats.distance_tests == 0);
	TASK_CHECK(stats.certified_error == 0);

	return 0;
}

static int caseCompatibilitySparse()
{
	// sparse mode renumbers vertices; the surface certifier must track the sparse indexing
	Mesh m = gridMesh(6, 0.2f);
	std::vector<unsigned int> out(m.indices.size());
	float error = -1;
	meshopt_SurfaceErrorStats stats;
	const float weights[2] = {0.1f, 0.1f};

	size_t count = meshopt_simplifyWithSurfaceError(out.data(), m.indices.data(), m.indices.size(), m.positions.data(), m.vertex_count(), sizeof(float) * 3, m.attributes.data(), sizeof(float) * 2, weights, 2, NULL, 24 * 3, 0.5f, meshopt_SimplifySparse, &error, 0.005f, 12, &stats);

	TASK_CHECK(count > 0);
	TASK_CHECK(validOutput(m, out, count));
	TASK_CHECK(stats.certified_error <= 0.005 + 1e-9);

	return 0;
}

static int caseCompatibilityShort16()
{
	// 16-bit index wrapper: the template converts to unsigned and back correctly
	Mesh m = gridMesh(6, 0);
	std::vector<unsigned short> in(m.indices.begin(), m.indices.end());
	std::vector<unsigned short> out(m.indices.size());
	float error = -1;
	meshopt_SurfaceErrorStats stats;
	const float weights[2] = {0.1f, 0.1f};

	size_t count = meshopt_simplifyWithSurfaceError(out.data(), in.data(), in.size(), m.positions.data(), m.vertex_count(), sizeof(float) * 3, m.attributes.data(), sizeof(float) * 2, weights, 2, NULL, 24 * 3, 0.5f, 0, &error, 0.01f, 10, &stats);

	TASK_CHECK(count > 0);
	TASK_CHECK(count % 3 == 0);
	for (size_t i = 0; i < count; ++i)
		TASK_CHECK(out[i] < m.vertex_count());
	TASK_CHECK(stats.certified_error <= 0.01 + 1e-9);

	return 0;
}

static int caseCompatibilityEmpty()
{
	// empty input is allowed and must produce an empty result and zeroed counters
	float positions[3] = {0, 0, 0};
	unsigned int out[1] = {0};
	unsigned int indices[1] = {0};
	float error = -1;
	meshopt_SurfaceErrorStats stats;
	memset(&stats, 0xff, sizeof(stats));
	const float weights[1] = {0.1f};

	size_t count = meshopt_simplifyWithSurfaceError(out, indices, 0, positions, 1, sizeof(float) * 3, positions, sizeof(float) * 2, weights, 1, NULL, 0, 1.f, 0, &error, 0.01f, 8, &stats);

	TASK_CHECK(count == 0);
	TASK_CHECK(stats.accepted_collapses == 0);
	TASK_CHECK(stats.distance_tests == 0);
	TASK_CHECK(stats.certified_error == 0);

	return 0;
}

static int caseCompatibilityLocks()
{
	// vertex locks must be honoured and not leak state between the locked and unlocked calls
	Mesh m = gridMesh(6, 0);
	std::vector<unsigned char> locks(m.vertex_count(), 0);

	// lock the entire border ring
	unsigned int n = 6;
	for (unsigned int j = 0; j <= n; ++j)
		for (unsigned int i = 0; i <= n; ++i)
			if (i == 0 || j == 0 || i == n || j == n)
				locks[j * (n + 1) + i] = meshopt_SimplifyVertex_Lock;

	Args a = {&m, locks.data(), 0, 1.f, 0, 0.01f, 12};
	std::vector<unsigned int> out;
	float error = -1;
	meshopt_SurfaceErrorStats stats;
	size_t count = run(a, out, error, stats);

	TASK_CHECK(count > 0);
	TASK_CHECK(validOutput(m, out, count));
	TASK_CHECK(stats.certified_error <= 0.01 + 1e-9);

	// all border vertices must still be present in the output
	std::vector<unsigned char> present(m.vertex_count(), 0);
	for (size_t i = 0; i < count; ++i)
		present[out[i]] = 1;
	for (unsigned int j = 0; j <= n; ++j)
		for (unsigned int i = 0; i <= n; ++i)
			if (i == 0 || j == 0 || i == n || j == n)
				TASK_CHECK(present[j * (n + 1) + i]);

	return 0;
}

static int caseCompatibilityAttributesNotCrossContaminated()
{
	// attribute weights must not leak into surface error (surface error is positional only) and vice versa:
	// two calls with different attribute weights must both keep the positional bound
	Mesh m = gridMesh(6, 0.25f);
	const float w1[2] = {0.5f, 0.f};
	const float w2[2] = {0.f, 0.5f};

	for (int k = 0; k < 2; ++k)
	{
		const float* weights = k == 0 ? w1 : w2;
		std::vector<unsigned int> out(m.indices.size());
		float error = -1;
		meshopt_SurfaceErrorStats stats;

		size_t count = meshopt_simplifyWithSurfaceError(out.data(), m.indices.data(), m.indices.size(), m.positions.data(), m.vertex_count(), sizeof(float) * 3, m.attributes.data(), sizeof(float) * 2, weights, 2, NULL, 0, 0.5f, 0, &error, 0.004f, 14, &stats);

		TASK_CHECK(count > 0);
		TASK_CHECK(validOutput(m, out, count));
		TASK_CHECK(stats.certified_error <= 0.004 + 1e-9);
	}

	return 0;
}

// the example is built into the test as a separate translation unit; it runs the new entry point and writes an OBJ
int surface_lod_run(const char* output_path, float max_surface_error, size_t target_triangles);

static int caseCompatibilityExample()
{
	const char* path = ".task-build/surface_lod_test.obj";
	int triangles = surface_lod_run(path, 0.01f, 40);

	TASK_CHECK(triangles > 0);

	// read the OBJ back and validate the face indices
	FILE* file = fopen(path, "rb");
	TASK_CHECK(file != NULL);

	int vertices = 0, faces = 0;
	char line[512];
	std::vector<unsigned int> face_indices;

	while (fgets(line, sizeof(line), file))
	{
		if (line[0] == 'v' && line[1] == ' ')
			vertices++;
		else if (line[0] == 'f' && line[1] == ' ')
		{
			unsigned int a = 0, b = 0, c = 0, ta = 0, tb = 0, tc = 0;
			int parsed = sscanf(line, "f %u/%u %u/%u %u/%u", &a, &ta, &b, &tb, &c, &tc);
			TASK_CHECK(parsed == 6);
			TASK_CHECK(a >= 1 && b >= 1 && c >= 1 && a <= unsigned(vertices) && b <= unsigned(vertices) && c <= unsigned(vertices));
			face_indices.push_back(a - 1);
			face_indices.push_back(b - 1);
			face_indices.push_back(c - 1);
			faces++;
		}
	}

	fclose(file);

	TASK_CHECK(faces == triangles);
	TASK_CHECK(int(face_indices.size()) == triangles * 3);

	// every face index must reference a written vertex and every triangle must be non-degenerate
	for (int i = 0; i < triangles; ++i)
	{
		unsigned int a = face_indices[i * 3], b = face_indices[i * 3 + 1], c = face_indices[i * 3 + 2];
		TASK_CHECK(a < unsigned(vertices) && b < unsigned(vertices) && c < unsigned(vertices));
		TASK_CHECK(a != b && b != c && a != c);
	}

	return 0;
}

// ---------- registration ----------

const struct task_case task_cases[] = {
    {"continuous", "plane", caseContinuousPlane},
    {"continuous", "bump_interior", caseContinuousBumpInterior},
    {"continuous", "trough", caseContinuousTrough},
    {"continuous", "zero_tolerance", caseContinuousZero},
    {"invalidation", "witness_replacement", caseInvalidationWitnessReplacement},
    {"invalidation", "no_witness", caseInvalidationNoWitness},
    {"invalidation", "distance_rejection", caseInvalidationDistanceRejection},
    {"invalidation", "seam_copies", caseInvalidationSeamCopies},
    {"invalidation", "compaction", caseInvalidationCompaction},
    {"budget", "depth_limit", caseBudgetDepthLimit},
    {"budget", "depth_subdivisions", caseBudgetDepthSubdivisions},
    {"budget", "depth_zero", caseBudgetDepthZero},
    {"budget", "sampling_growth", caseBudgetSamplingGrowth},
    {"compatibility", "disabled", caseCompatibilityDisabled},
    {"compatibility", "sparse", caseCompatibilitySparse},
    {"compatibility", "short16", caseCompatibilityShort16},
    {"compatibility", "empty", caseCompatibilityEmpty},
    {"compatibility", "locks", caseCompatibilityLocks},
    {"compatibility", "attributes", caseCompatibilityAttributesNotCrossContaminated},
    {"compatibility", "example", caseCompatibilityExample},
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
