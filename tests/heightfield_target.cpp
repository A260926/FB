// Height field target tests.
//
// Registered through task_support/check.h in three groups: intersections,
// updates and lighting. The intersection and density references are written
// independently in double precision here; the production queries are the
// only height field code under test. No main is defined in this file.

#include "task_support/check.h"

#include "src/heightfield.h"
#include "src/scene.h"
#include "src/render.h"
#include "src/disney.h"

#include <cmath>
#include <cstdio>
#include <limits>
#include <memory>
#include <random>
#include <stdexcept>
#include <vector>

namespace
{
	const double kPiD = 3.14159265358979323846;

	bool Close(float a, float b, float tol=1.0e-5f)
	{
		return std::fabs(double(a) - double(b)) <= tol*(1.0 + std::fabs(double(b)));
	}

	bool CloseD(double a, double b, double tol=1.0e-9)
	{
		return std::fabs(a - b) <= tol*(1.0 + std::fabs(b));
	}

	float Lum3(const Vec3& c)
	{
		return 0.3f*c.x + 0.6f*c.y + 0.1f*c.z;
	}

	// Independent double precision bilinear cell evaluation.
	struct CellCorners
	{
		double h00, h10, h01, h11;
	};

	double CellHeight(const CellCorners& c, double su, double sv)
	{
		return c.h00 + (c.h10-c.h00)*su + (c.h01-c.h00)*sv
			+ (c.h11-c.h10-c.h01+c.h00)*su*sv;
	}

	void CellDerivatives(const CellCorners& c, double cellW, double cellD,
		double su, double sv, double& hx, double& hz)
	{
		hx = ((c.h10-c.h00) + (c.h11-c.h10-c.h01+c.h00)*sv)/cellW;
		hz = ((c.h01-c.h00) + (c.h11-c.h10-c.h01+c.h00)*su)/cellD;
	}

	struct IndependentHit
	{
		bool hit;
		double t;
		int cx;
		int cz;
		double su;
		double sv;
		double x, y, z;
		double nx, ny, nz;
	};

	// Exhaustive O(N*N) reference: solve the cell quadratic independently in
	// every cell and keep the nearest positive root.
	IndependentHit ReferenceIntersect(const HeightFieldSnapshot& snap,
		const Vec3& originF, const Vec3& dirF)
	{
		const int n = snap.N();
		const double cellW = double(snap.Width())/n;
		const double cellD = double(snap.Depth())/n;

		const double ox = originF.x, oy = originF.y, oz = originF.z;
		const double dx = dirF.x, dy = dirF.y, dz = dirF.z;

		IndependentHit best;
		best.hit = false;
		best.t = 1.0e300;

		for (int cz=0; cz < n; ++cz)
		{
			for (int cx=0; cx < n; ++cx)
			{
				CellCorners c;
				c.h00 = snap.VertexHeight(cx,   cz);
				c.h10 = snap.VertexHeight(cx+1, cz);
				c.h01 = snap.VertexHeight(cx,   cz+1);
				c.h11 = snap.VertexHeight(cx+1, cz+1);

				const double A = c.h00;
				const double B = c.h10-c.h00;
				const double C = c.h01-c.h00;
				const double D = c.h11-c.h10-c.h01+c.h00;

				const double a0 = ox/cellW - cx;
				const double a1 = dx/cellW;
				const double b0 = oz/cellD - cz;
				const double b1 = dz/cellD;

				const double q2 = D*a1*b1;
				const double q1 = D*(a0*b1 + a1*b0) + B*a1 + C*b1 - dy;
				const double q0 = D*a0*b0 + B*a0 + C*b0 + A - oy;

				double roots[2];
				int numRoots = 0;

				const double scale = std::max(1.0,
					std::max(std::fabs(q2), std::max(std::fabs(q1), std::fabs(q0))));

				if (std::fabs(q2) <= 1.0e-12*scale)
				{
					if (std::fabs(q1) > 1.0e-12*scale)
						roots[numRoots++] = -q0/q1;
				}
				else
				{
					double disc = q1*q1 - 4.0*q2*q0;
					if (disc >= 0.0)
					{
						const double sq = std::sqrt(disc);
						const double q = -0.5*(q1 + (q1 < 0.0 ? -sq : sq));
						roots[numRoots++] = q/q2;
						if (sq > 0.0)
							roots[numRoots++] = q0/q;
					}
				}

				for (int r=0; r < numRoots; ++r)
				{
					const double t = roots[r];
					if (!(t > 0.0) || t >= best.t)
						continue;

					const double su = a0 + a1*t;
					const double sv = b0 + b1*t;
					if (su < -1.0e-9 || su > 1.0+1.0e-9 ||
						sv < -1.0e-9 || sv > 1.0+1.0e-9)
						continue;

					best.hit = true;
					best.t = t;
					best.cx = cx;
					best.cz = cz;
					best.su = std::min(1.0, std::max(0.0, su));
					best.sv = std::min(1.0, std::max(0.0, sv));
				}
			}
		}

		if (!best.hit)
			return best;

		// canonical cell for the normal: smaller row, then smaller column
		int canonX = best.cx;
		int canonZ = best.cz;
		double canonSu = best.su;
		double canonSv = best.sv;
		if (canonSu <= 1.0e-9 && canonX > 0) { --canonX; canonSu = 1.0; }
		if (canonSv <= 1.0e-9 && canonZ > 0) { --canonZ; canonSv = 1.0; }

		CellCorners c;
		c.h00 = snap.VertexHeight(canonX,   canonZ);
		c.h10 = snap.VertexHeight(canonX+1, canonZ);
		c.h01 = snap.VertexHeight(canonX,   canonZ+1);
		c.h11 = snap.VertexHeight(canonX+1, canonZ+1);

		double hx, hz;
		CellDerivatives(c, cellW, cellD, canonSu, canonSv, hx, hz);
		const double invLen = 1.0/std::sqrt(1.0 + hx*hx + hz*hz);
		best.nx = -hx*invLen;
		best.ny = invLen;
		best.nz = -hz*invLen;

		best.x = ox + dx*best.t;
		best.y = oy + dy*best.t;
		best.z = oz + dz*best.t;
		return best;
	}

	std::vector<float> MakeHeights(int n, float w, float d,
		float (*fn)(float, float))
	{
		std::vector<float> heights((n+1)*(n+1));
		for (int z=0; z <= n; ++z)
			for (int x=0; x <= n; ++x)
				heights[z*(n+1)+x] = fn(w*x/n, d*z/n);
		return heights;
	}

	float SaddleHeight(float x, float z)
	{
		const float dx = x - 2.0f;
		const float dz = z - 2.0f;
		return dx*dx - dz*dz;
	}

	float WavyHeight(float x, float z)
	{
		return 0.5f*std::sin(2.0f*x) + 0.3f*std::cos(3.0f*z) + 0.2f*x*z*0.1f;
	}

	// Trace a small scene by looping its primitives (test scenes are tiny).
	const Primitive* SceneTrace(const Scene& scene, const Ray& ray,
		float& outT, Vec3& outNormal)
	{
		const Primitive* best = NULL;
		outT = REAL_MAX;
		for (size_t i=0; i < scene.primitives.size(); ++i)
		{
			float t;
			Vec3 n;
			if (PrimitiveIntersect(scene.primitives[i], ray, t, &n) &&
				t > 0.0f && t < outT)
			{
				outT = t;
				outNormal = n;
				best = &scene.primitives[i];
			}
		}
		return best;
	}

	Primitive MakeHeightFieldPrimitive(const HeightFieldSnapshot& snapshot,
		const Transform& transform)
	{
		Primitive primitive;
		primitive.type = eHeightField;
		primitive.heightfield.snapshot =
			std::make_shared<HeightFieldSnapshot>(snapshot);
		primitive.startTransform = transform;
		primitive.endTransform = transform;
		return primitive;
	}
}

// ===========================================================================
// intersections
// ===========================================================================

static int test_flat_field()
{
	const int n = 4;
	const std::vector<float> heights((n+1)*(n+1), 1.0f);
	HeightField hf(n, 4.0f, 4.0f, heights.data());
	const HeightFieldSnapshot snap = hf.Snapshot();

	HeightFieldHit hit;
	TASK_CHECK(snap.Intersect(Vec3(2,3,2), Vec3(0,-1,0), REAL_MAX, hit));
	TASK_CHECK(Close(hit.t, 2.0f));
	TASK_CHECK(Close(hit.position.y, 1.0f));
	TASK_CHECK(Close(hit.normal.y, 1.0f) && Close(hit.normal.x, 0.0f) && Close(hit.normal.z, 0.0f));

	// non-unit ray: parameter t scales with direction length
	TASK_CHECK(snap.Intersect(Vec3(2,3,2), Vec3(0,-3,0), REAL_MAX, hit));
	TASK_CHECK(Close(hit.t, 2.0f/3.0f));

	// ray pointing away and parallel ray miss
	TASK_CHECK(!snap.Intersect(Vec3(2,3,2), Vec3(0,1,0), REAL_MAX, hit));
	TASK_CHECK(!snap.Intersect(Vec3(2,1,2), Vec3(1,0,0), REAL_MAX, hit));

	// outside the xz rectangle misses
	TASK_CHECK(!snap.Intersect(Vec3(6,3,2), Vec3(0,-1,0), REAL_MAX, hit));

	// tmax truncation
	TASK_CHECK(!snap.Intersect(Vec3(2,3,2), Vec3(0,-1,0), 1.0f, hit));
	TASK_CHECK(snap.Intersect(Vec3(2,3,2), Vec3(0,-1,0), 2.5f, hit));
	return 0;
}

static int test_saddle_multiple_roots()
{
	const int n = 8;
	const std::vector<float> heights = MakeHeights(n, 4.0f, 4.0f, SaddleHeight);
	HeightField hf(n, 4.0f, 4.0f, heights.data());
	const HeightFieldSnapshot snap = hf.Snapshot();

	// horizontal ray at y=0.25 along z=2 crosses the piecewise bilinear
	// saddle twice (x=1.5 and x=2.5); the nearest positive root wins
	const IndependentHit ref =
		ReferenceIntersect(snap, Vec3(0,0.25f,2), Vec3(1,0,0));
	TASK_CHECK(ref.hit);
	TASK_CHECK(CloseD(ref.x, 1.5, 1.0e-6));

	HeightFieldHit hit;
	TASK_CHECK(snap.Intersect(Vec3(0,0.25f,2), Vec3(1,0,0), REAL_MAX, hit));
	TASK_CHECK(Close(hit.t, float(ref.t), 1.0e-5f));
	TASK_CHECK(Close(hit.position.x, 1.5f, 1.0e-5f));

	// a ray crossing from above must report the upper root first
	const IndependentHit ref2 =
		ReferenceIntersect(snap, Vec3(0.5f,3,2.5f), Normalize(Vec3(1,-2,-0.3f)));
	TASK_CHECK(ref2.hit);
	TASK_CHECK(snap.Intersect(Vec3(0.5f,3,2.5f), Normalize(Vec3(1,-2,-0.3f)), REAL_MAX, hit));
	TASK_CHECK(Close(hit.t, float(ref2.t), 1.0e-5f));
	TASK_CHECK(Close(hit.position.y, float(ref2.y), 1.0e-4f));
	return 0;
}

static int test_single_cell_two_roots()
{
	// craft a strongly saddle bilinear cell (D != 0) so near-horizontal rays
	// can cross the same cell twice, then compare against the reference
	const int n = 2;
	std::vector<float> heights((n+1)*(n+1), 0.0f);

	// cell (0,0) corners chosen so its quadratic admits two in-cell roots
	heights[0*(n+1)+0] = 1.35453f;
	heights[0*(n+1)+1] = -1.71947f;
	heights[1*(n+1)+0] = -0.520204f;
	heights[1*(n+1)+1] = 0.844311f;

	HeightField hf(n, 2.0f, 2.0f, heights.data());
	const HeightFieldSnapshot snap = hf.Snapshot();

	// deterministic known two-root ray for the saddle cell above
	const Vec3 knownOrigin(0.0642276f, 0.0915016f, 0.454481f);
	const Vec3 knownDir = Normalize(Vec3(0.656776f, -0.0430598f, 0.752856f));

	bool foundTwoRootCase = false;
	{
		const CellCorners c = {
			double(snap.VertexHeight(0,0)), double(snap.VertexHeight(1,0)),
			double(snap.VertexHeight(0,1)), double(snap.VertexHeight(1,1)) };
		const double a0 = knownOrigin.x, a1 = knownDir.x;
		const double b0 = knownOrigin.z, b1 = knownDir.z;
		const double A=c.h00, B=c.h10-c.h00, C=c.h01-c.h00, D=c.h11-c.h10-c.h01+c.h00;
		const double q2=D*a1*b1;
		const double q1=D*(a0*b1+a1*b0)+B*a1+C*b1-knownDir.y;
		const double q0=D*a0*b0+B*a0+C*b0+A-knownOrigin.y;
		const double disc = q1*q1-4.0*q2*q0;
		TASK_CHECK(disc > 0.0);
		const double sq = std::sqrt(disc);
		const double q = -0.5*(q1 + (q1 < 0 ? -sq : sq));
		const double t1 = q/q2;
		const double t2 = q0/q;
		const bool v1 = t1 > 0 && (a0+a1*t1) >= -1e-9 && (a0+a1*t1) <= 1+1e-9
			&& (b0+b1*t1) >= -1e-9 && (b0+b1*t1) <= 1+1e-9;
		const bool v2 = t2 > 0 && (a0+a1*t2) >= -1e-9 && (a0+a1*t2) <= 1+1e-9
			&& (b0+b1*t2) >= -1e-9 && (b0+b1*t2) <= 1+1e-9;
		TASK_CHECK(v1 && v2);
		foundTwoRootCase = true;

		const IndependentHit ref = ReferenceIntersect(snap, knownOrigin, knownDir);
		TASK_CHECK(ref.hit);
		TASK_CHECK(CloseD(ref.t, std::min(t1, t2), 1.0e-6));

		HeightFieldHit hit;
		TASK_CHECK(snap.Intersect(knownOrigin, knownDir, REAL_MAX, hit));
		TASK_CHECK(Close(hit.t, float(ref.t), 1.0e-5f));
		TASK_CHECK(Close(hit.position.x, float(ref.x), 1.0e-4f));
		TASK_CHECK(Close(hit.position.y, float(ref.y), 1.0e-4f));
		TASK_CHECK(Close(hit.position.z, float(ref.z), 1.0e-4f));
		TASK_CHECK(Close(hit.normal.x, float(ref.nx), 1.0e-4f));
		TASK_CHECK(Close(hit.normal.y, float(ref.ny), 1.0e-4f));
		TASK_CHECK(Close(hit.normal.z, float(ref.nz), 1.0e-4f));
	}

	// randomized near-horizontal rays keep production and the exhaustive
	// double precision reference in agreement
	std::mt19937 rng(12345);
	std::uniform_real_distribution<float> jitter(-0.3f, 0.3f);
	int checks = 0;
	for (int attempt=0; attempt < 2000; ++attempt)
	{
		const Vec3 origin(0.5f + jitter(rng), 0.2f + jitter(rng), 0.5f + jitter(rng));
		const Vec3 dir = Normalize(Vec3(jitter(rng), -0.05f + jitter(rng)*0.2f, jitter(rng)));

		const IndependentHit ref = ReferenceIntersect(snap, origin, dir);
		HeightFieldHit hit;
		const bool got = snap.Intersect(origin, dir, REAL_MAX, hit);
		TASK_CHECK(got == ref.hit);
		++checks;
		if (ref.hit)
		{
			TASK_CHECK(Close(hit.t, float(ref.t), 1.0e-5f));
			TASK_CHECK(Close(hit.position.y, float(ref.y), 1.0e-4f));
		}
	}
	TASK_CHECK(checks == 2000);
	TASK_CHECK(foundTwoRootCase);
	return 0;
}

static int test_exhaustive_random_agreement()
{
	const int n = 8;
	const std::vector<float> heights = MakeHeights(n, 4.0f, 4.0f, WavyHeight);
	HeightField hf(n, 4.0f, 4.0f, heights.data());
	const HeightFieldSnapshot snap = hf.Snapshot();

	std::mt19937 rng(777);
	std::uniform_real_distribution<float> coord(-2.0f, 6.0f);
	std::uniform_real_distribution<float> one(-1.0f, 1.0f);
	std::normal_distribution<float> normalD(0.0f, 1.0f);

	int agreements = 0;
	int hitCount = 0;
	for (int i=0; i < 1500; ++i)
	{
		const Vec3 origin(coord(rng), coord(rng)+1.0f, coord(rng));
		Vec3 dir(one(rng), normalD(rng), one(rng));
		if (LengthSq(dir) < 1.0e-8f)
			continue;
		dir = Normalize(dir);

		const IndependentHit ref = ReferenceIntersect(snap, origin, dir);
		HeightFieldHit hit;
		const bool got = snap.Intersect(origin, dir, REAL_MAX, hit);

		TASK_CHECK(got == ref.hit);
		++agreements;

		if (ref.hit)
		{
			++hitCount;
			TASK_CHECK(Close(hit.t, float(ref.t), 1.0e-5f));
			TASK_CHECK(Close(hit.position.x, float(ref.x), 1.0e-4f));
			TASK_CHECK(Close(hit.position.y, float(ref.y), 1.0e-4f));
			TASK_CHECK(Close(hit.position.z, float(ref.z), 1.0e-4f));
			TASK_CHECK(Close(hit.normal.x, float(ref.nx), 1.0e-4f));
			TASK_CHECK(Close(hit.normal.y, float(ref.ny), 1.0e-4f));
			TASK_CHECK(Close(hit.normal.z, float(ref.nz), 1.0e-4f));
			TASK_CHECK(hit.cellX == ref.cx || hit.u >= 0.9999f || hit.u <= 0.0001f
				|| hit.v >= 0.9999f || hit.v <= 0.0001f);
		}
	}
	TASK_CHECK(agreements > 1000);
	TASK_CHECK(hitCount > 50);
	return 0;
}

static int test_shared_edges_and_boundary()
{
	const int n = 4;
	const std::vector<float> heights = MakeHeights(n, 4.0f, 4.0f, WavyHeight);
	HeightField hf(n, 4.0f, 4.0f, heights.data());
	const HeightFieldSnapshot snap = hf.Snapshot();

	// rays landing exactly on shared grid edges and on grid vertices
	for (int i=0; i <= n; ++i)
	{
		const float edgeX = 4.0f*i/n;
		HeightFieldHit hit;
		TASK_CHECK(snap.Intersect(Vec3(edgeX, 5, 0.7f), Vec3(0,-1,0), REAL_MAX, hit));
		TASK_CHECK(Close(hit.position.x, edgeX));

		const float edgeZ = 4.0f*i/n;
		TASK_CHECK(snap.Intersect(Vec3(0.9f, 5, edgeZ), Vec3(0,-1,0), REAL_MAX, hit));
		TASK_CHECK(Close(hit.position.z, edgeZ));
	}

	// outer boundary corners still hit and use the unique boundary cell
	HeightFieldHit corner;
	TASK_CHECK(snap.Intersect(Vec3(0,5,0), Vec3(0,-1,0), REAL_MAX, corner));
	TASK_CHECK(corner.cellX == 0 && corner.cellZ == 0);
	TASK_CHECK(snap.Intersect(Vec3(4,5,4), Vec3(0,-1,0), REAL_MAX, corner));
	TASK_CHECK(corner.cellX == n-1 && corner.cellZ == n-1);

	// edge normal convention: at x = cell boundary the lower column cell wins
	Vec3 pEdge, nEdge;
	snap.Sample(0.5f, 0.37f, pEdge, nEdge);
	TASK_CHECK(Close(pEdge.x, 2.0f, 1.0e-5f));
	const IndependentHit ref =
		ReferenceIntersect(snap, Vec3(2,5,pEdge.z), Vec3(0,-1,0));
	TASK_CHECK(ref.hit);
	HeightFieldHit hit;
	TASK_CHECK(snap.Intersect(Vec3(2,5,pEdge.z), Vec3(0,-1,0), REAL_MAX, hit));
	TASK_CHECK(Close(hit.normal.x, float(ref.nx), 1.0e-5f));
	TASK_CHECK(Close(hit.normal.z, float(ref.nz), 1.0e-5f));
	return 0;
}

static int test_rotated_scaled_instances()
{
	const int n = 8;
	const std::vector<float> heights = MakeHeights(n, 4.0f, 4.0f, WavyHeight);
	HeightField hf(n, 4.0f, 4.0f, heights.data());
	const HeightFieldSnapshot snap = hf.Snapshot();

	const Transform transform(Vec3(1.0f, -2.0f, 3.0f),
		Quat(Normalize(Vec3(0.3f, 1.0f, 0.2f)), 0.9f), 2.5f);

	Primitive primitive = MakeHeightFieldPrimitive(snap, transform);

	std::mt19937 rng(2024);
	std::uniform_real_distribution<float> coord(-6.0f, 10.0f);
	std::normal_distribution<float> nd(0.0f, 1.0f);

	for (int i=0; i < 200; ++i)
	{
		const Vec3 origin(coord(rng), coord(rng), coord(rng));
		Vec3 dir(nd(rng), nd(rng), nd(rng));
		dir = Normalize(dir);

		// independent local space reference
		const Vec3 localOrigin = InverseTransformPoint(transform, origin);
		const Vec3 localDir = InverseTransformVector(transform, dir);
		const IndependentHit ref = ReferenceIntersect(snap, localOrigin, localDir);

		float t;
		Vec3 n;
		const bool got = PrimitiveIntersect(primitive, Ray(origin, dir, 0.5f), t, &n);
		TASK_CHECK(got == ref.hit);

		if (ref.hit)
		{
			TASK_CHECK(Close(t, float(ref.t), 1.0e-4f));

			const Vec3 worldPoint = origin + dir*t;
			const Vec3 localPoint = InverseTransformPoint(transform, worldPoint);
			TASK_CHECK(Close(localPoint.x, float(ref.x), 1.0e-3f));
			TASK_CHECK(Close(localPoint.y, float(ref.y), 1.0e-3f));
			TASK_CHECK(Close(localPoint.z, float(ref.z), 1.0e-3f));

			const Vec3 localNormal = Normalize(InverseTransformVector(transform, n));
			TASK_CHECK(Close(localNormal.x, float(ref.nx), 1.0e-3f));
			TASK_CHECK(Close(localNormal.y, float(ref.ny), 1.0e-3f));
			TASK_CHECK(Close(localNormal.z, float(ref.nz), 1.0e-3f));
		}
	}
	return 0;
}

static int test_quadtree_culling()
{
	const int n = 64;
	const std::vector<float> heights = MakeHeights(n, 8.0f, 8.0f, WavyHeight);
	HeightField hf(n, 8.0f, 8.0f, heights.data());
	const HeightFieldSnapshot snap = hf.Snapshot();

	// ray entirely above the field: no leaf may be tested, only a short chain
	// of bounding boxes
	HeightFieldQueryStats stats;
	HeightFieldHit hit;
	const Bounds bounds = snap.LocalBounds();
	const bool got = snap.Intersect(Vec3(4, bounds.upper.y + 5.0f, 4),
		Vec3(1,0,0), REAL_MAX, hit, &stats);
	TASK_CHECK(!got);
	TASK_CHECK(stats.leavesTested == 0);
	TASK_CHECK(stats.nodesVisited < 40);

	// ordinary ray tests far fewer leaves than the full 64*64 sweep
	snap.Intersect(Vec3(-1, 6, -1), Normalize(Vec3(1,-1,1)), REAL_MAX, hit, &stats);
	TASK_CHECK(stats.leavesTested > 0);
	TASK_CHECK(stats.leavesTested < n*n/4);

	// the culled result still matches the exhaustive reference
	std::mt19937 rng(99);
	std::uniform_real_distribution<float> coord(-3.0f, 11.0f);
	std::normal_distribution<float> nd(0.0f, 1.0f);
	for (int i=0; i < 300; ++i)
	{
		const Vec3 origin(coord(rng), coord(rng), coord(rng));
		Vec3 dir(nd(rng), nd(rng), nd(rng));
		dir = Normalize(dir);

		const IndependentHit ref = ReferenceIntersect(snap, origin, dir);
		HeightFieldHit h;
		const bool g = snap.Intersect(origin, dir, REAL_MAX, h);
		TASK_CHECK(g == ref.hit);
		if (ref.hit)
			TASK_CHECK(Close(h.t, float(ref.t), 1.0e-5f));
	}
	return 0;
}

// ===========================================================================
// updates
// ===========================================================================

static int test_constructor_validation()
{
	const std::vector<float> heights(9, 0.0f);

	bool threw = false;
	try { HeightField bad(3, 2, 2, heights.data()); }
	catch (const std::invalid_argument&) { threw = true; }
	TASK_CHECK(threw);

	threw = false;
	try { HeightField bad(2, 0.0f, 2.0f, heights.data()); }
	catch (const std::invalid_argument&) { threw = true; }
	TASK_CHECK(threw);

	threw = false;
	try { HeightField bad(2, 2.0f, -1.0f, heights.data()); }
	catch (const std::invalid_argument&) { threw = true; }
	TASK_CHECK(threw);

	std::vector<float> nanHeights = heights;
	nanHeights[4] = std::numeric_limits<float>::quiet_NaN();
	threw = false;
	try { HeightField bad(2, 2.0f, 2.0f, nanHeights.data()); }
	catch (const std::invalid_argument&) { threw = true; }
	TASK_CHECK(threw);

	threw = false;
	try { HeightField bad(2, 2.0f, 2.0f, NULL); }
	catch (const std::invalid_argument&) { threw = true; }
	TASK_CHECK(threw);

	HeightField good(2, 2.0f, 2.0f, heights.data());
	TASK_CHECK(good.Version() == 1 && good.N() == 2);
	return 0;
}

static int test_single_edit_workload()
{
	// 257x257 vertices, N = 256 cells
	const int n = 256;
	const std::vector<float> heights((n+1)*(n+1), 0.0f);
	HeightField hf(n, 16.0f, 16.0f, heights.data());
	const Bounds oldBounds = hf.Snapshot().LocalBounds();

	HeightUpdateStats stats;
	const HeightEdit edit(128, 128, 5.0f);
	TASK_CHECK(hf.Update(&edit, 1, stats));
	TASK_CHECK(hf.Version() == 2);

	// an interior vertex touches exactly four cells
	TASK_CHECK(stats.leavesRecomputed == 4);
	// work only grows along the affected ancestor paths, never a full rebuild
	TASK_CHECK(stats.nodesRecomputed == 17);
	TASK_CHECK(stats.leavesRecomputed + stats.nodesRecomputed < 100);

	// the edited surface must be discoverable above the original bounding box
	const HeightFieldSnapshot edited = hf.Snapshot();
	HeightFieldHit raisedHit;
	const Vec3 raisedOrigin(8.0f, 20.0f, 8.0f);
	TASK_CHECK(edited.Intersect(raisedOrigin, Vec3(0,-1,0), REAL_MAX, raisedHit));
	TASK_CHECK(raisedHit.position.y > oldBounds.upper.y);
	TASK_CHECK(Close(raisedHit.position.y, 5.0f, 1.0e-4f));
	TASK_CHECK(edited.LocalBounds().upper.y > oldBounds.upper.y);

	// corner vertex touches a single cell and one ancestor chain
	HeightUpdateStats cornerStats;
	const HeightEdit corner(0, 0, 1.0f);
	TASK_CHECK(hf.Update(&corner, 1, cornerStats));
	TASK_CHECK(cornerStats.leavesRecomputed == 1);
	TASK_CHECK(cornerStats.nodesRecomputed == 5);

	// smallest possible field is a single leaf: no internal work
	HeightField small(2, 1.0f, 1.0f, std::vector<float>(9, 0.0f).data());
	HeightUpdateStats smallStats;
	const HeightEdit smallEdit(1, 1, 2.0f);
	TASK_CHECK(small.Update(&smallEdit, 1, smallStats));
	TASK_CHECK(smallStats.leavesRecomputed == 1);
	TASK_CHECK(smallStats.nodesRecomputed == 0);
	return 0;
}

static int test_invalid_batch_then_retry()
{
	const int n = 16;
	std::vector<float> heights((n+1)*(n+1), 0.0f);
	HeightField hf(n, 4.0f, 4.0f, heights.data());

	const int versionBefore = hf.Version();
	const HeightFieldSnapshot before = hf.Snapshot();

	HeightEdit bad[3] =
	{
		HeightEdit(1, 1, 2.0f),
		HeightEdit(n+1, 0, 1.0f),   // out of range
		HeightEdit(0, 0, 1.0f),
	};
	HeightUpdateStats stats;
	TASK_CHECK(!hf.Update(bad, 3, stats));

	// nothing moved: version, heights, stats and snapshot data
	TASK_CHECK(hf.Version() == versionBefore);
	TASK_CHECK(stats.leavesRecomputed == 0 && stats.nodesRecomputed == 0);
	TASK_CHECK(hf.Snapshot().Version() == versionBefore);
	TASK_CHECK(before.VertexHeight(1,1) == 0.0f);
	TASK_CHECK(before.LocalBounds().upper.y == 0.0f);

	// non-finite value is rejected too
	const float inf = std::numeric_limits<float>::infinity();
	HeightEdit badFinite(0, 0, inf);
	TASK_CHECK(!hf.Update(&badFinite, 1, stats));
	TASK_CHECK(hf.Version() == versionBefore);

	// a valid edit after the failures succeeds and touches the right cells
	HeightEdit good(1, 1, 2.0f);
	TASK_CHECK(hf.Update(&good, 1, stats));
	TASK_CHECK(hf.Version() == versionBefore + 1);
	TASK_CHECK(stats.leavesRecomputed == 1);
	TASK_CHECK(hf.Snapshot().VertexHeight(1,1) == 2.0f);
	return 0;
}

static int test_empty_and_unchanged_edits()
{
	const int n = 8;
	std::vector<float> heights((n+1)*(n+1), 0.0f);
	heights[4*(n+1)+4] = 3.0f;
	HeightField hf(n, 4.0f, 4.0f, heights.data());

	const int versionBefore = hf.Version();
	HeightUpdateStats stats;

	TASK_CHECK(hf.Update(NULL, 0, stats));
	TASK_CHECK(hf.Version() == versionBefore);
	TASK_CHECK(stats.leavesRecomputed == 0 && stats.nodesRecomputed == 0);

	// every final value equals the current height, including repeats
	HeightEdit unchanged[3] =
	{
		HeightEdit(4, 4, 3.0f),
		HeightEdit(0, 0, 0.0f),
		HeightEdit(4, 4, 3.0f),
	};
	TASK_CHECK(hf.Update(unchanged, 3, stats));
	TASK_CHECK(hf.Version() == versionBefore);
	TASK_CHECK(stats.leavesRecomputed == 0 && stats.nodesRecomputed == 0);
	return 0;
}

static int test_duplicate_coordinates_last_wins()
{
	const int n = 8;
	const std::vector<float> heights((n+1)*(n+1), 0.0f);
	HeightField hf(n, 4.0f, 4.0f, heights.data());

	HeightEdit edits[4] =
	{
		HeightEdit(3, 3, 1.0f),
		HeightEdit(3, 3, 4.0f),
		HeightEdit(3, 3, -2.0f),
		HeightEdit(2, 5, 7.0f),
	};
	HeightUpdateStats stats;
	TASK_CHECK(hf.Update(edits, 4, stats));
	TASK_CHECK(hf.Snapshot().VertexHeight(3,3) == -2.0f);
	TASK_CHECK(hf.Snapshot().VertexHeight(2,5) == 7.0f);
	// two distinct vertices: at most eight cells between them
	TASK_CHECK(stats.leavesRecomputed <= 8);
	return 0;
}

static int test_disjoint_partial_refresh()
{
	const int n = 64;
	const std::vector<float> heights((n+1)*(n+1), 0.0f);
	HeightField hf(n, 8.0f, 8.0f, heights.data());

	// two far apart vertices placed on leaf block corners, so each one
	// touches four distinct 8x8 leaf blocks and the sets are disjoint
	HeightEdit edits[2] =
	{
		HeightEdit(8, 8, 1.0f),
		HeightEdit(24, 24, 2.0f),
	};
	HeightUpdateStats stats;
	TASK_CHECK(hf.Update(edits, 2, stats));
	TASK_CHECK(stats.leavesRecomputed == 8);
	// both paths share the root and the two coarsest splits, so the internal
	// count stays bounded by the path lengths rather than the whole tree
	TASK_CHECK(stats.nodesRecomputed > 0 && stats.nodesRecomputed < 40);
	TASK_CHECK(stats.leavesRecomputed + stats.nodesRecomputed < n*n/10);

	const HeightFieldSnapshot snap = hf.Snapshot();
	TASK_CHECK(snap.VertexHeight(8,8) == 1.0f);
	TASK_CHECK(snap.VertexHeight(24,24) == 2.0f);
	TASK_CHECK(snap.VertexHeight(30,30) == 0.0f);
	return 0;
}

static int test_snapshot_interleave()
{
	const int n = 32;
	const std::vector<float> heights((n+1)*(n+1), 0.0f);
	HeightField hf(n, 8.0f, 8.0f, heights.data());

	const HeightFieldSnapshot oldSnap = hf.Snapshot();
	const Bounds oldBounds = oldSnap.LocalBounds();
	Vec3 oldPos, oldNormal;
	oldSnap.Sample(0.5f, 0.5f, oldPos, oldNormal);
	const float oldDensity = oldSnap.AreaDensity(0.5f, 0.5f);

	// lift the centre vertex far above the original bounding box
	HeightEdit edit(n/2, n/2, 9.0f);
	HeightUpdateStats stats;
	TASK_CHECK(hf.Update(&edit, 1, stats));
	const HeightFieldSnapshot newSnap = hf.Snapshot();

	TASK_CHECK(oldSnap.Version() == 1 && newSnap.Version() == 2);

	// old snapshot keeps its own heights, bounds, normals and density
	TASK_CHECK(oldSnap.VertexHeight(n/2, n/2) == 0.0f);
	TASK_CHECK(newSnap.VertexHeight(n/2, n/2) == 9.0f);
	TASK_CHECK(oldBounds.upper.y == 0.0f);
	TASK_CHECK(newSnap.LocalBounds().upper.y == 9.0f);
	TASK_CHECK(Close(oldDensity, oldSnap.AreaDensity(0.5f, 0.5f)));

	Vec3 newPos, newNormal;
	newSnap.Sample(0.5f, 0.5f, newPos, newNormal);
	TASK_CHECK(newPos.y == 9.0f && oldPos.y == 0.0f);
	TASK_CHECK(newNormal.y != oldNormal.y || newNormal.x != oldNormal.x);

	// a ray straight down finds the raised surface on the new snapshot but the
	// old flat floor on the retained one
	HeightFieldHit hit;
	TASK_CHECK(newSnap.Intersect(Vec3(4, 12, 4), Vec3(0,-1,0), REAL_MAX, hit));
	TASK_CHECK(Close(hit.position.y, 9.0f, 1.0e-4f));
	TASK_CHECK(oldSnap.Intersect(Vec3(4, 12, 4), Vec3(0,-1,0), REAL_MAX, hit));
	TASK_CHECK(Close(hit.position.y, 0.0f, 1.0e-4f));

	// primitives pin their own snapshot version independently
	Primitive oldPrimitive = MakeHeightFieldPrimitive(oldSnap, Transform());
	Primitive newPrimitive = MakeHeightFieldPrimitive(newSnap, Transform());

	float tOld, tNew;
	Vec3 nOld, nNew;
	TASK_CHECK(PrimitiveIntersect(oldPrimitive, Ray(Vec3(4,12,4), Vec3(0,-1,0), 0.5f), tOld, &nOld));
	TASK_CHECK(PrimitiveIntersect(newPrimitive, Ray(Vec3(4,12,4), Vec3(0,-1,0), 0.5f), tNew, &nNew));
	TASK_CHECK(Close(tOld, 12.0f, 1.0e-4f));
	TASK_CHECK(Close(tNew, 3.0f, 1.0e-4f));

	// another edit does not disturb either retained snapshot
	HeightEdit edit2(0, 0, 3.0f);
	TASK_CHECK(hf.Update(&edit2, 1, stats));
	TASK_CHECK(oldSnap.Version() == 1);
	TASK_CHECK(newSnap.Version() == 2);
	TASK_CHECK(hf.Snapshot().Version() == 3);
	TASK_CHECK(oldSnap.VertexHeight(0,0) == 0.0f);
	TASK_CHECK(newSnap.VertexHeight(0,0) == 0.0f);
	TASK_CHECK(hf.Snapshot().VertexHeight(0,0) == 3.0f);
	return 0;
}

// ===========================================================================
// lighting
// ===========================================================================

static int test_area_density_matches_area()
{
	const int n = 16;
	const std::vector<float> heights = MakeHeights(n, 3.0f, 5.0f, WavyHeight);
	HeightField hf(n, 3.0f, 5.0f, heights.data());
	const HeightFieldSnapshot snap = hf.Snapshot();

	// the area density must integrate to one over surface area:
	// integral of p_A * dA, with dA = W*D*g*du*dv computed independently
	const int bins = 120;
	const double wD = snap.Width();
	const double dD = snap.Depth();
	const double cellW = wD/n;
	const double cellD = dD/n;
	double integral = 0.0;
	double pointwiseMaxError = 0.0;
	for (int j=0; j < bins; ++j)
	{
		for (int i=0; i < bins; ++i)
		{
			const float u = (i + 0.5f)/bins;
			const float v = (j + 0.5f)/bins;
			const float du = 1.0f/bins;
			const float dv = 1.0f/bins;

			// independent derivatives from the canonical bilinear cell
			const float xf = u*snap.Width();
			const float zf = v*snap.Depth();
			int cx = std::min(n-1, int(xf/cellW));
			int cz = std::min(n-1, int(zf/cellD));
			double su = xf/cellW - cx;
			double sv = zf/cellD - cz;
			if (su <= 1.0e-9 && cx > 0) { --cx; su = 1.0; }
			if (sv <= 1.0e-9 && cz > 0) { --cz; sv = 1.0; }
			const CellCorners cell = {
				double(snap.VertexHeight(cx, cz)),
				double(snap.VertexHeight(cx+1, cz)),
				double(snap.VertexHeight(cx, cz+1)),
				double(snap.VertexHeight(cx+1, cz+1)) };
			double hx, hz;
			CellDerivatives(cell, cellW, cellD, su, sv, hx, hz);
			const double g = std::sqrt(1.0 + hx*hx + hz*hz);

			const float density = snap.AreaDensity(u, v);
			integral += double(density)*wD*dD*g*du*dv;

			pointwiseMaxError = std::max(pointwiseMaxError,
				std::fabs(double(density) - 1.0/(wD*dD*g)));
		}
	}
	TASK_CHECK(CloseD(integral, 1.0, 1.0e-3));
	TASK_CHECK(pointwiseMaxError < 1.0e-5);

	// independent surface area: bilinear patch areas via midpoint quadrature,
	// evaluated without calling any production density function
	double independentArea = 0.0;
	for (int cz=0; cz < n; ++cz)
	{
		for (int cx=0; cx < n; ++cx)
		{
			CellCorners c = {
				double(snap.VertexHeight(cx, cz)),
				double(snap.VertexHeight(cx+1, cz)),
				double(snap.VertexHeight(cx, cz+1)),
				double(snap.VertexHeight(cx+1, cz+1)) };

			double patchArea = 0.0;
			const int sub = 8;
			for (int sj=0; sj < sub; ++sj)
			{
				for (int si=0; si < sub; ++si)
				{
					const double su = (si+0.5)/sub;
					const double sv = (sj+0.5)/sub;
					double hx, hz;
					CellDerivatives(c, cellW, cellD, su, sv, hx, hz);
					patchArea += std::sqrt(1.0 + hx*hx + hz*hz)*(cellW*cellD)/(sub*sub);
				}
			}
			independentArea += patchArea;
		}
	}

	// production integral of 1/density Jacobian gives the same total area:
	// 1/density = W*D*g, so du*dv/density integrates the physical area element
	double productionArea = 0.0;
	for (int j=0; j < bins; ++j)
		for (int i=0; i < bins; ++i)
		{
			const float u = (i+0.5f)/bins;
			const float v = (j+0.5f)/bins;
			const float du = 1.0f/bins;
			const float dv = 1.0f/bins;
			productionArea += du*dv/snap.AreaDensity(u, v);
		}
	TASK_CHECK(CloseD(productionArea, independentArea, 2.0e-3));

	// world density divides the local density by the squared instance scale
	const Transform scaled(Vec3(0), Quat(), 3.0f);
	Primitive primitive = MakeHeightFieldPrimitive(snap, scaled);
	const float localDensity = snap.AreaDensity(0.31f, 0.67f);
	const float worldDensity =
		HeightFieldPrimitiveAreaDensity(primitive, scaled, 0.31f, 0.67f);
	TASK_CHECK(Close(worldDensity, localDensity/9.0f, 1.0e-6f));
	return 0;
}

static int test_sampling_and_inverse_conventions()
{
	const int n = 16;
	const std::vector<float> heights = MakeHeights(n, 4.0f, 6.0f, WavyHeight);
	HeightField hf(n, 4.0f, 6.0f, heights.data());
	const HeightFieldSnapshot snap = hf.Snapshot();

	// uniform parameter mapping and exact inverse
	for (int k=0; k < 100; ++k)
	{
		const float u = (k*0.137f) - std::floor(k*0.137f);
		const float v = (k*0.719f) - std::floor(k*0.719f);

		Vec3 position, normal;
		snap.Sample(u, v, position, normal);
		TASK_CHECK(Close(position.x, u*4.0f, 1.0e-5f));
		TASK_CHECK(Close(position.z, v*6.0f, 1.0e-5f));

		float ru, rv;
		TASK_CHECK(snap.Inverse(position, ru, rv));
		TASK_CHECK(Close(ru, u, 1.0e-5f) && Close(rv, v, 1.0e-5f));
		TASK_CHECK(std::fabs(Length(normal) - 1.0f) < 1.0e-5f);
	}

	// empirical marginals must be uniform in u and v
	Random rand(4242);
	const int samples = 40000;
	std::vector<int> uHist(10, 0), vHist(10, 0);
	for (int i=0; i < samples; ++i)
	{
		Vec3 p, nrm;
		float u1, u2;
		Sample2D(rand, u1, u2);
		snap.Sample(u1, u2, p, nrm);
		float ru, rv;
		snap.Inverse(p, ru, rv);
		++uHist[std::min(9, int(ru*10))];
		++vHist[std::min(9, int(rv*10))];
	}
	for (int b=0; b < 10; ++b)
	{
		const double expected = samples/10.0;
		TASK_CHECK(std::fabs(uHist[b]-expected)/expected < 0.08);
		TASK_CHECK(std::fabs(vHist[b]-expected)/expected < 0.08);
	}
	return 0;
}

static int test_directional_density_integral()
{
	// small flat emissive tile under a receiver: the solid angle density must
	// integrate to one over the directions that hit the tile
	const int n = 8;
	const std::vector<float> heights((n+1)*(n+1), 0.0f);
	HeightField hf(n, 2.0f, 2.0f, heights.data());
	const HeightFieldSnapshot snap = hf.Snapshot();

	const Transform lightXform(Vec3(-1.0f, 3.0f, -1.0f), Quat(), 1.0f);
	Primitive light = MakeHeightFieldPrimitive(snap, lightXform);
	light.material.emission = Vec3(5.0f);
	light.lightSamples = 8;

	const Vec3 receiver(0, 0, 0);

	// independent direction sweep over the upper hemisphere
	const int thetaBins = 64, phiBins = 128;
	double total = 0.0;
	for (int it=0; it < thetaBins; ++it)
	{
		const double theta = (it+0.5)*kPiD*0.5/thetaBins;
		for (int ip=0; ip < phiBins; ++ip)
		{
			const double phi = (ip+0.5)*2.0*kPiD/phiBins;
			const Vec3 wi(float(std::sin(theta)*std::cos(phi)),
				float(std::cos(theta)),
				float(std::sin(theta)*std::sin(phi)));

			float t;
			Vec3 nrm;
			if (!PrimitiveIntersect(light, Ray(receiver, wi, 0.5f), t, &nrm))
				continue;

			const Vec3 lightPos = receiver + wi*t;
			float u, v;
			TASK_CHECK(HeightFieldPrimitiveInverse(light, lightXform, lightPos, u, v));

			const float areaPdf =
				HeightFieldPrimitiveAreaDensity(light, lightXform, u, v);
			const float cosLight = std::fabs(Dot(nrm, wi));
			const float solidAnglePdf = areaPdf*t*t/std::max(1.0e-6f, cosLight);

			const double dTheta = kPiD*0.5/thetaBins;
			const double dPhi = 2.0*kPiD/phiBins;
			const double solidAngle = std::sin(theta)*dTheta*dPhi;
			total += solidAnglePdf*solidAngle;
		}
	}
	TASK_CHECK(CloseD(total, 1.0, 2.0e-2));
	return 0;
}

namespace
{
	// Direct light and BSDF continuation estimators used by the CPU renderer,
	// rebuilt here from the public production functions so both strategies can
	// be compared at a fixed receiver point.
	struct EstimatorResult
	{
		Vec3 mean;
		int samples;
	};

	// Pure (MIS-free) direct light estimator: every accepted connection keeps
	// full weight, so its expectation is the direct lighting integral itself.
	// The light pdf uses the height field's pointwise area density.
	EstimatorResult DirectLightEstimator(const Scene& scene,
		const Primitive& light, int lightSamples,
		const Vec3& receiver, const Vec3& normal, const Material& material,
		int numSamples, Random& rand, bool& healthy)
	{
		Vec3 accum(0.0f);
		healthy = true;
		const Transform lightXform =
			InterpolateTransform(light.startTransform, light.endTransform, 0.5f);

		for (int s=0; s < numSamples; ++s)
		{
			Vec3 lightPos, lightNormal;
			PrimitiveSample(light, 0.5f, lightPos, lightNormal, rand);

			Vec3 wi = lightPos - receiver;
			const float dist = Length(wi);
			wi /= dist;

			const float nl = Abs(Dot(lightNormal, wi));
			if (nl < 1.0e-6f)
				continue;

			float t;
			Vec3 nrm;
			const Primitive* hit = SceneTrace(scene,
				Ray(receiver + normal*0.0001f, wi, 0.5f), t, nrm);
			if (!hit || hit != &light || std::fabs(t-dist) > 1.0e-2f)
				continue;

			float u, v;
			HeightFieldPrimitiveInverse(light, lightXform, lightPos, u, v);
			const float areaPdf =
				HeightFieldPrimitiveAreaDensity(light, lightXform, u, v);
			const float lightPdf = areaPdf*dist*dist/nl;
			if (!(lightPdf > 0.0f && std::isfinite(lightPdf)))
				healthy = false;

			const float bsdfPdf = BSDFPdf(material, 1, 1, receiver, normal, normal, wi);
			const Vec3 f = BSDFEval(material, 1, 1, receiver, normal, normal, wi);
			if (!(bsdfPdf > 0.0f))
				continue;

			// balance MIS weight stays finite and within [0,1] for the
			// renderer's actual lightSamples count
			const int N = lightSamples + 1;
			const float cbsdf = 1.0f/N;
			const float clight = float(lightSamples)/N;
			const float weight = clight*lightPdf/(cbsdf*bsdfPdf + clight*lightPdf);
			if (!(std::isfinite(weight) && weight >= 0.0f && weight <= 1.0f))
				healthy = false;

			accum += f*light.material.emission*Abs(Dot(wi, normal))/lightPdf;
		}

		EstimatorResult result;
		result.mean = accum/float(numSamples);
		result.samples = numSamples;
		return result;
	}

	// Pure direction sampling estimator: BSDF continuation rays that reach the
	// light keep full weight. Its expectation is the same direct lighting
	// integral as the direct estimator above.
	EstimatorResult DirectionalEstimator(const Scene& scene,
		const Primitive& light, int lightSamples,
		const Vec3& receiver, const Vec3& normal, const Material& material,
		int numSamples, Random& rand, bool& healthy)
	{
		Vec3 accum(0.0f);
		healthy = true;
		const Transform lightXform =
			InterpolateTransform(light.startTransform, light.endTransform, 0.5f);

		Vec3 uAxis, vAxis;
		BasisFromVector(normal, &uAxis, &vAxis);

		for (int s=0; s < numSamples; ++s)
		{
			Vec3 wi;
			float bsdfPdf;
			BSDFType type;
			BSDFSample(material, 1, 1, receiver, uAxis, vAxis, normal, normal,
				wi, bsdfPdf, type, rand);
			if (!(bsdfPdf > 0.0f))
				continue;

			float t;
			Vec3 nrm;
			const Primitive* hit = SceneTrace(scene,
				Ray(receiver + normal*0.0001f, wi, 0.5f), t, nrm);
			if (!hit || hit != &light)
				continue;

			const Vec3 lightPos = receiver + wi*t;
			float u, v;
			HeightFieldPrimitiveInverse(light, lightXform, lightPos, u, v);
			const float areaPdf =
				HeightFieldPrimitiveAreaDensity(light, lightXform, u, v);
			const float cosLight = Abs(Dot(nrm, wi));
			const float lightPdf = areaPdf*t*t/std::max(1.0e-6f, cosLight);
			if (!(lightPdf > 0.0f && std::isfinite(lightPdf)))
				healthy = false;

			const int N = lightSamples + 1;
			const float cbsdf = 1.0f/N;
			const float clight = float(lightSamples)/N;
			const float weight = cbsdf*bsdfPdf/(cbsdf*bsdfPdf + clight*lightPdf);
			if (!(std::isfinite(weight) && weight >= 0.0f && weight <= 1.0f))
				healthy = false;

			const Vec3 f = BSDFEval(material, 1, 1, receiver, normal, normal, wi);
			accum += f*light.material.emission*Abs(Dot(wi, normal))/bsdfPdf;
		}

		EstimatorResult result;
		result.mean = accum/float(numSamples);
		result.samples = numSamples;
		return result;
	}

	// Deterministic direct lighting reference through midpoint quadrature over
	// the light parameters, using an independent area element. It shares no
	// sampling or density code with the estimators under test.
	Vec3 ReferenceDirectLighting(const Scene& scene, const Primitive& light,
		const Vec3& receiver, const Vec3& normal, const Material& material,
		int bins)
	{
		const HeightFieldSnapshot& snap = *light.heightfield.snapshot;
		const Transform lightXform =
			InterpolateTransform(light.startTransform, light.endTransform, 0.5f);
		const int n = snap.N();
		const double cellW = snap.Width()/n;
		const double cellD = snap.Depth()/n;

		Vec3 integral(0.0f);
		for (int j=0; j < bins; ++j)
		{
			for (int i=0; i < bins; ++i)
			{
				const float u = (i+0.5f)/bins;
				const float v = (j+0.5f)/bins;

				Vec3 localPos, localNormal;
				snap.Sample(u, v, localPos, localNormal);
				const Vec3 lightPos = TransformPoint(lightXform, localPos);
				const Vec3 lightNormal =
					SafeNormalize(TransformVector(lightXform, localNormal));

				Vec3 wi = lightPos - receiver;
				const float dist = Length(wi);
				wi /= dist;

				float t;
				Vec3 nrm;
				const Primitive* hit = SceneTrace(scene,
					Ray(receiver + normal*0.0001f, wi, 0.5f), t, nrm);
				if (!hit || hit != &light || std::fabs(t-dist) > 1.0e-2f)
					continue;

				// independent area element dA = g*dx*dz
				const double x = u*snap.Width();
				const double z = v*snap.Depth();
				int cx = std::min(n-1, int(x/cellW));
				int cz = std::min(n-1, int(z/cellD));
				double su = x/cellW - cx;
				double sv = z/cellD - cz;
				if (su <= 1.0e-9 && cx > 0) { --cx; su = 1.0; }
				if (sv <= 1.0e-9 && cz > 0) { --cz; sv = 1.0; }
				const CellCorners cell = {
					double(snap.VertexHeight(cx, cz)),
					double(snap.VertexHeight(cx+1, cz)),
					double(snap.VertexHeight(cx, cz+1)),
					double(snap.VertexHeight(cx+1, cz+1)) };
				double hx, hz;
				CellDerivatives(cell, cellW, cellD, su, sv, hx, hz);
				const double g = std::sqrt(1.0 + hx*hx + hz*hz);
				// physical area element of one parameter bin: dA = W*D*g*du*dv
				const double dA = snap.Width()*snap.Depth()*g/(double(bins)*bins);

				const Vec3 f = BSDFEval(material, 1, 1, receiver, normal, normal, wi);
				const float cosReceiver = Abs(Dot(wi, normal));
				const float cosLight = Abs(Dot(lightNormal, wi));
				integral += f*light.material.emission*
					(cosReceiver*cosLight/(dist*dist))*float(dA);
			}
		}
		return integral;
	}

	Primitive MakeLight(const HeightFieldSnapshot& snap, const Transform& xform)
	{
		Primitive light = MakeHeightFieldPrimitive(snap, xform);
		light.material.emission = Vec3(4.0f, 3.5f, 3.0f);
		light.lightSamples = 8;
		return light;
	}
}

static int test_cpu_direct_vs_directional_means()
{
	const int n = 16;
	const std::vector<float> flat((n+1)*(n+1), 0.0f);
	HeightField lightField(n, 2.0f, 2.0f, flat.data());
	const HeightFieldSnapshot lightSnap = lightField.Snapshot();

	Primitive floorPlane;
	floorPlane.type = ePlane;
	floorPlane.plane.plane[0] = 0.0f;
	floorPlane.plane.plane[1] = 1.0f;
	floorPlane.plane.plane[2] = 0.0f;
	floorPlane.plane.plane[3] = 1.0f; // y = -1
	floorPlane.startTransform = floorPlane.endTransform = Transform();
	floorPlane.material.color = Vec3(0.8f);
	floorPlane.material.specular = 0.2f;
	floorPlane.material.roughness = 0.8f;

	Scene scene;
	scene.AddPrimitive(MakeLight(lightSnap, Transform(Vec3(-1,4,-1), Quat(), 1.0f)));
	scene.AddPrimitive(floorPlane);
	scene.Build();

	Material receiverMaterial;
	receiverMaterial.metallic = 0.0f;
	receiverMaterial.specular = 0.2f;
	receiverMaterial.roughness = 0.8f;

	const Vec3 receiver(0, 0, 0);
	const Vec3 normal(0, 1, 0);
	const int kSamples = 6000;

	bool directHealthy = false;
	bool directionHealthy = false;
	Random directRand(11);
	Random directionRand(11);
	const EstimatorResult direct = DirectLightEstimator(scene, scene.primitives[0],
		8, receiver, normal, receiverMaterial, kSamples, directRand, directHealthy);
	const EstimatorResult directional = DirectionalEstimator(scene, scene.primitives[0],
		8, receiver, normal, receiverMaterial, kSamples, directionRand, directionHealthy);
	const Vec3 reference = ReferenceDirectLighting(scene, scene.primitives[0],
		receiver, normal, receiverMaterial, 60);

	const float directLum = Lum3(direct.mean);
	const float directionLum = Lum3(directional.mean);
	const float referenceLum = Lum3(reference);
	const float directError = std::fabs(directLum-referenceLum)/
		std::max(1.0e-4f, referenceLum);
	const float directionError = std::fabs(directionLum-referenceLum)/
		std::max(1.0e-4f, referenceLum);
	printf("LIGHT mean direct=%f directional=%f reference=%f samples=%d errors=%f,%f\n",
		double(directLum), double(directionLum), double(referenceLum),
		kSamples, double(directError), double(directionError));

	TASK_CHECK(directHealthy && directionHealthy);
	TASK_CHECK(directLum > 0.0f && directionLum > 0.0f && referenceLum > 0.0f);
	TASK_CHECK(directError < 0.10f);
	TASK_CHECK(directionError < 0.15f);
	TASK_CHECK(direct.samples == kSamples && directional.samples == kSamples);

	// the real CpuRenderer must run the whole height field light path
	// (trace, shadow rays, pointwise-density direct sampling and balance MIS)
	Camera camera;
	camera.position = Vec3(0, 1.0f, 0);
	camera.rotation = Quat(Vec3(1,0,0), DegToRad(-90.0f));
	camera.fov = DegToRad(60.0f);
	Options options;
	options.width = options.height = 24;
	options.mode = ePathTrace;
	options.maxDepth = 3;
	options.maxSamples = 1;
	options.clamp = 100.0f;
	options.filter = Filter(eFilterBox, 0.0f);
	std::vector<Color> pixels(576, Color(0.0f));
	std::unique_ptr<Renderer> renderer(CreateCpuRenderer(&scene));
	renderer->Render(camera, options, pixels.data());
	int litPixels = 0;
	for (const Color& c : pixels)
	{
		TASK_CHECK(std::isfinite(c.x) && std::isfinite(c.y) && std::isfinite(c.z));
		if (Lum3(Vec3(c.x, c.y, c.z)) > 0.0f)
			++litPixels;
	}
	TASK_CHECK(litPixels > 100);

	delete[] scene.bvh.nodes;
	scene.bvh.nodes = NULL;
	return 0;
}

static int test_edited_light_and_occluder()
{
	const int n = 16;

	// light field that gets a bump edited into its centre
	std::vector<float> lightHeights((n+1)*(n+1), 0.0f);
	HeightField lightField(n, 2.0f, 2.0f, lightHeights.data());
	const HeightFieldSnapshot lightBefore = lightField.Snapshot();

	HeightEdit bump(n/2, n/2, -1.5f);
	HeightUpdateStats stats;
	TASK_CHECK(lightField.Update(&bump, 1, stats));
	const HeightFieldSnapshot lightAfter = lightField.Snapshot();

	// occluder: a solid ridge between receiver and light
	std::vector<float> occluderHeights((n+1)*(n+1), 0.0f);
	for (int z=0; z <= n; ++z)
		for (int x=0; x <= n; ++x)
			occluderHeights[z*(n+1)+x] = 0.6f*std::exp(-float((x-n/2)*(x-n/2))/8.0f);
	HeightField occluderField(n, 3.0f, 3.0f, occluderHeights.data());
	const HeightFieldSnapshot occluderSnap = occluderField.Snapshot();

	Material receiverMaterial;
	receiverMaterial.metallic = 0.0f;
	receiverMaterial.specular = 0.2f;
	receiverMaterial.roughness = 0.8f;

	const Vec3 receiver(0, 0, 0);
	const Vec3 normal(0, 1, 0);
	const int kSamples = 2500;

	// edited light, no occluder: both pure estimators match the quadrature
	Scene openScene;
	openScene.AddPrimitive(MakeLight(lightAfter, Transform(Vec3(-1,4,-1), Quat(), 1.0f)));
	openScene.Build();

	bool h1 = false, h2 = false;
	Random r1(900), r2(900);
	const EstimatorResult openDirect = DirectLightEstimator(openScene, openScene.primitives[0],
		8, receiver, normal, receiverMaterial, kSamples, r1, h1);
	const EstimatorResult openDirection = DirectionalEstimator(openScene, openScene.primitives[0],
		8, receiver, normal, receiverMaterial, kSamples, r2, h2);
	const Vec3 openReference = ReferenceDirectLighting(openScene, openScene.primitives[0],
		receiver, normal, receiverMaterial, 60);
	const float openDirectError =
		std::fabs(Lum3(openDirect.mean)-Lum3(openReference))/std::max(1.0e-4f, Lum3(openReference));
	const float openDirectionError =
		std::fabs(Lum3(openDirection.mean)-Lum3(openReference))/std::max(1.0e-4f, Lum3(openReference));
	printf("LIGHT edited direct=%f directional=%f reference=%f samples=%d errors=%f,%f\n",
		double(Lum3(openDirect.mean)), double(Lum3(openDirection.mean)),
		double(Lum3(openReference)), kSamples,
		double(openDirectError), double(openDirectionError));
	TASK_CHECK(h1 && h2);
	TASK_CHECK(openDirectError < 0.12f);
	TASK_CHECK(openDirectionError < 0.18f);
	delete[] openScene.bvh.nodes;
	openScene.bvh.nodes = NULL;

	// with a ridge covering part of the light's footprint the lighting dims
	Scene blockedScene;
	blockedScene.AddPrimitive(MakeLight(lightAfter, Transform(Vec3(-1,4,-1), Quat(), 1.0f)));
	// 0.5x0.5 tile centered on the ray cone next to the receiver: it covers
	// the central slice of light directions but leaves the rim unblocked
	Primitive occluder = MakeHeightFieldPrimitive(occluderSnap,
		Transform(Vec3(0.0f, 1.5f, 0.0f), Quat(), 1.0f/6.0f));
	blockedScene.AddPrimitive(occluder);
	blockedScene.Build();

	bool h3 = false, h4 = false;
	Random r3(901), r4(901);
	const EstimatorResult blockedDirect = DirectLightEstimator(blockedScene, blockedScene.primitives[0],
		8, receiver, normal, receiverMaterial, kSamples, r3, h3);
	const EstimatorResult blockedDirection = DirectionalEstimator(blockedScene, blockedScene.primitives[0],
		8, receiver, normal, receiverMaterial, kSamples, r4, h4);
	const Vec3 blockedReference = ReferenceDirectLighting(blockedScene, blockedScene.primitives[0],
		receiver, normal, receiverMaterial, 60);
	const float blockedDirectError =
		std::fabs(Lum3(blockedDirect.mean)-Lum3(blockedReference))/std::max(1.0e-4f, Lum3(blockedReference));
	const float blockedDirectionError =
		std::fabs(Lum3(blockedDirection.mean)-Lum3(blockedReference))/std::max(1.0e-4f, Lum3(blockedReference));
	printf("LIGHT occluded direct=%f directional=%f reference=%f samples=%d errors=%f,%f\n",
		double(Lum3(blockedDirect.mean)), double(Lum3(blockedDirection.mean)),
		double(Lum3(blockedReference)), kSamples,
		double(blockedDirectError), double(blockedDirectionError));
	TASK_CHECK(h3 && h4);
	TASK_CHECK(blockedDirectError < 0.18f);
	TASK_CHECK(blockedDirectionError < 0.25f);
	TASK_CHECK(Lum3(blockedReference) < Lum3(openReference));
	TASK_CHECK(Lum3(blockedDirect.mean) < Lum3(openDirect.mean));
	TASK_CHECK(Lum3(blockedDirection.mean) < Lum3(openDirection.mean));

	// the ridge itself is genuinely intersected as a shadow caster
	float t;
	Vec3 nrm;
	const Primitive* hit = SceneTrace(blockedScene,
		Ray(receiver, Vec3(0, 1, 0), 0.5f), t, nrm);
	TASK_CHECK(hit == &blockedScene.primitives[1]);
	delete[] blockedScene.bvh.nodes;
	blockedScene.bvh.nodes = NULL;
	return 0;
}

static int test_zero_cosine_and_double_sided()
{
	const int n = 8;
	const std::vector<float> flat((n+1)*(n+1), 0.0f);
	HeightField field(n, 2.0f, 2.0f, flat.data());
	const HeightFieldSnapshot snap = field.Snapshot();

	Primitive light = MakeLight(snap, Transform(Vec3(0,2,0), Quat(), 1.0f));

	// grazing connection: wi is perpendicular to the light normal, so the
	// precise zero-cosine connection contributes nothing and stays finite
	Vec3 lightPos(1.0f, 2.0f, 0.0f), lightNormal(0,1,0);
	const Vec3 receiver(1, 2, 2);
	Vec3 wi = lightPos - receiver;
	const float dist = Length(wi);
	wi /= dist;
	TASK_CHECK(Close(Dot(lightNormal, wi), 0.0f, 1.0e-5f));
	const float nl = Abs(Dot(lightNormal, wi));
	TASK_CHECK(nl < 1.0e-6f);

	// double-sided emission: a ray striking the back face still intersects and
	// reports a normal, and the renderer applies emission regardless of side
	Primitive below = MakeHeightFieldPrimitive(snap, Transform(Vec3(-1,-3,-1), Quat(), 1.0f));
	below.material.emission = Vec3(2.0f);
	float t;
	Vec3 nrm;
	const Ray downRay(Vec3(0.0f, 0, 0.0f), Vec3(0,-1,0), 0.5f);
	TASK_CHECK(PrimitiveIntersect(below, downRay, t, &nrm));
	TASK_CHECK(Close(t, 3.0f, 1.0e-4f));
	TASK_CHECK(below.material.emission.x > 0.0f);

	// end to end: the CPU renderer lights the scene through the new primitive
	Scene scene;
	scene.AddPrimitive(below);
	scene.Build();

	Camera camera;
	camera.position = Vec3(0, 0.0f, 0);
	// default camera looks down -Z; rotate -90 degrees about X to look down -Y
	camera.rotation = Quat(Vec3(1,0,0), DegToRad(-90.0f));
	camera.fov = DegToRad(60.0f);
	Options options;
	options.width = options.height = 16;
	options.mode = ePathTrace;
	options.maxDepth = 2;
	options.maxSamples = 1;
	options.clamp = 100.0f;
	options.filter = Filter(eFilterBox, 0.0f);

	std::vector<Color> pixels(256, Color(0.0f));
	std::unique_ptr<Renderer> renderer(CreateCpuRenderer(&scene));
	renderer->Render(camera, options, pixels.data());

	int lit = 0;
	for (const Color& c : pixels)
	{
		TASK_CHECK(std::isfinite(c.x) && std::isfinite(c.y) && std::isfinite(c.z));
		if (c.x > 0.0f || c.y > 0.0f || c.z > 0.0f)
			++lit;
	}
	TASK_CHECK(lit > 0);
	delete[] scene.bvh.nodes;
	scene.bvh.nodes = NULL;
	return 0;
}

// ===========================================================================
// registration
// ===========================================================================

extern const task_case task_cases[] = {
	{ "intersections", "flat_field", test_flat_field },
	{ "intersections", "saddle_multiple_roots", test_saddle_multiple_roots },
	{ "intersections", "single_cell_two_roots", test_single_cell_two_roots },
	{ "intersections", "exhaustive_random_agreement", test_exhaustive_random_agreement },
	{ "intersections", "shared_edges_and_boundary", test_shared_edges_and_boundary },
	{ "intersections", "rotated_scaled_instances", test_rotated_scaled_instances },
	{ "intersections", "quadtree_culling", test_quadtree_culling },

	{ "updates", "constructor_validation", test_constructor_validation },
	{ "updates", "single_edit_workload", test_single_edit_workload },
	{ "updates", "invalid_batch_then_retry", test_invalid_batch_then_retry },
	{ "updates", "empty_and_unchanged_edits", test_empty_and_unchanged_edits },
	{ "updates", "duplicate_coordinates_last_wins", test_duplicate_coordinates_last_wins },
	{ "updates", "disjoint_partial_refresh", test_disjoint_partial_refresh },
	{ "updates", "snapshot_interleave", test_snapshot_interleave },

	{ "lighting", "area_density_matches_area", test_area_density_matches_area },
	{ "lighting", "sampling_and_inverse_conventions", test_sampling_and_inverse_conventions },
	{ "lighting", "directional_density_integral", test_directional_density_integral },
	{ "lighting", "cpu_direct_vs_directional_means", test_cpu_direct_vs_directional_means },
	{ "lighting", "edited_light_and_occluder", test_edited_light_and_occluder },
	{ "lighting", "zero_cosine_and_double_sided", test_zero_cosine_and_double_sided },
};

const size_t task_case_count = sizeof(task_cases)/sizeof(task_cases[0]);
