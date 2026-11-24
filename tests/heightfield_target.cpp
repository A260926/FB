// Target regression tests for the editable bilinear height field.
//
// The production surface is intersected as an exact quadratic per cell; these
// tests re-derive every equation independently in double precision and only
// then compare against the float production results. No triangle proxies,
// marching or Newton iteration are used on either side.

#include "task_support/check.h"

#include "src/render.h"
#include "src/disney.h"

#include <vector>
#include <cmath>
#include <cstdio>
#include <random>
#include <algorithm>

namespace {

const double kPiD = 3.14159265358979323846;

float p1_at_2_2(const HeightField::Snapshot& s);

// ---------------------------------------------------------------------------
// Independent double-precision reference
// ---------------------------------------------------------------------------

struct DVec3 { double x, y, z; };
DVec3 operator+(DVec3 a, DVec3 b) { return {a.x+b.x, a.y+b.y, a.z+b.z}; }
DVec3 operator-(DVec3 a, DVec3 b) { return {a.x-b.x, a.y-b.y, a.z-b.z}; }
DVec3 operator*(DVec3 a, double s) { return {a.x*s, a.y*s, a.z*s}; }
double dot(DVec3 a, DVec3 b) { return a.x*b.x + a.y*b.y + a.z*b.z; }
double length(DVec3 a) { return std::sqrt(dot(a, a)); }
DVec3 cross(DVec3 a, DVec3 b)
{
	return {a.y*b.z - a.z*b.y, a.z*b.x - a.x*b.z, a.x*b.y - a.y*b.x};
}
DVec3 normalize(DVec3 a) { double l = length(a); return {a.x/l, a.y/l, a.z/l}; }

struct RefHit
{
	bool hit;
	double t;
	int cx, cz;
	double u, v;
	DVec3 point;
	DVec3 normal;
};

// Independent cell equation: with u(t), v(t) linear in the ray parameter the
// bilinear surface becomes A t^2 + B t + C = 0. All real roots are examined.
RefHit ReferenceIntersect(const std::vector<double>& heights, int N, double W, double D,
                          DVec3 origin, DVec3 dir)
{
	const int stride = N + 1;
	const double dx = W / N;
	const double dz = D / N;

	RefHit best;
	best.hit = false;
	best.t = 1e300;

	auto H = [&](int x, int z) { return heights[z*stride + x]; };

	for (int cz = 0; cz < N; ++cz)
	{
		for (int cx = 0; cx < N; ++cx)
		{
			const double h00 = H(cx,   cz);
			const double h10 = H(cx+1, cz);
			const double h01 = H(cx,   cz+1);
			const double h11 = H(cx+1, cz+1);
			const double a   = h00;
			const double bu  = h10 - h00;
			const double bv  = h01 - h00;
			const double buv = h00 - h10 - h01 + h11;

			const double u0 = (origin.x - cx*dx) / dx;
			const double ud = dir.x / dx;
			const double v0 = (origin.z - cz*dz) / dz;
			const double vd = dir.z / dz;

			const double A = buv*ud*vd;
			const double B = bu*ud + bv*vd + buv*(u0*vd + v0*ud) - dir.y;
			const double C = a + bu*u0 + bv*v0 + buv*u0*v0 - origin.y;

			double roots[2];
			int nroots = 0;
			if (std::fabs(A) < 1e-25)
			{
				if (std::fabs(B) < 1e-30)
					continue;
				roots[nroots++] = -C / B;
			}
			else
			{
				const double disc = B*B - 4.0*A*C;
				if (disc < 0.0)
					continue;
				const double sq = std::sqrt(disc);
				const double q = -0.5*(B + (B < 0.0 ? -sq : sq));
				roots[nroots++] = q / A;
				roots[nroots++] = C / q;
			}

			for (int i = 0; i < nroots; ++i)
			{
				const double t = roots[i];
				if (!(t > 0.0) || t >= best.t)
					continue;
				double u = u0 + ud*t;
				double v = v0 + vd*t;
				if (u < -1e-9 || v < -1e-9 || u > 1.0 + 1e-9 || v > 1.0 + 1e-9)
					continue;
				u = std::min(1.0, std::max(0.0, u));
				v = std::min(1.0, std::max(0.0, v));

				best.hit = true;
				best.t = t;
				best.cx = cx;
				best.cz = cz;
				best.u = u;
				best.v = v;
				best.point = {origin.x + dir.x*t, origin.y + dir.y*t, origin.z + dir.z*t};
				const double hx = (bu + buv*v) / dx;
				const double hz = (bv + buv*u) / dz;
				best.normal = normalize({-hx, 1.0, -hz});
			}
		}
	}
	return best;
}

bool Approx(double a, double b, double eps)
{
	return std::fabs(a - b) <= eps + eps*std::fabs(b);
}

float lum3(const Vec3& c) { return c.x*0.3f + c.y*0.6f + c.z*0.1f; }

// Independent double quaternion rotation (q v q'), used to check instances
// without calling the production transform code.
DVec3 RotateQuat(double qx, double qy, double qz, double qw, DVec3 v)
{
	// t = 2 * cross(q.xyz, v)
	DVec3 q = {qx, qy, qz};
	DVec3 t = cross(q, v) * 2.0;
	// v' = v + qw*t + cross(q, t)
	DVec3 c = cross(q, t);
	return {v.x + qw*t.x + c.x, v.y + qw*t.y + c.y, v.z + qw*t.z + c.z};
}

std::vector<double> ToDouble(const std::vector<float>& h)
{
	return std::vector<double>(h.begin(), h.end());
}

// ---------------------------------------------------------------------------
// Grid builders
// ---------------------------------------------------------------------------
std::vector<float> MakeGrid(int N, float W, float D, int selector)
{
	std::vector<float> h((N+1)*(N+1));
	for (int z = 0; z <= N; ++z)
	{
		for (int x = 0; x <= N; ++x)
		{
			const double u = double(x) / N;
			const double v = double(z) / N;
			double y = 0.0;
			switch (selector)
			{
			case 0: // saddle-ish analytic surface
				y = 0.8*(u - 0.5)*(v - 0.5) + 0.25*std::sin(2.0*kPiD*u)*std::cos(2.0*kPiD*v);
				break;
			case 1: // waves
				y = 0.3*std::sin(3.0*kPiD*u) + 0.2*std::cos(4.0*kPiD*v) + 0.15*u*v;
				break;
			default: // planar ramp, still bilinear per cell
				y = 0.4*u - 0.3*v + 0.1;
				break;
			}
			h[z*(N+1) + x] = float(y);
		}
	}
	return h;
}

// ---------------------------------------------------------------------------
// intersections group
// ---------------------------------------------------------------------------

int case_saddle_roots()
{
	// Single saddle cell h(u,v) = (2u-1)(2v-1): corners (1,-1,-1,1).
	// A horizontal ray along the diagonal crosses the patch twice; both roots
	// are valid and the nearest positive one must be returned.
	const int N = 2;
	const float W = 2.0f, D = 2.0f;
	std::vector<float> h((N+1)*(N+1), 0.0f);
	auto set = [&](int x, int z, float y) { h[z*(N+1)+x] = y; };
	set(0,0, 1.0f); set(1,0,-1.0f); set(0,1,-1.0f); set(1,1, 1.0f);

	HeightField field;
	TASK_CHECK(field.Initialize(N, W, D, h.data(), (int)h.size()));
	const auto snap = field.TakeSnapshot();

	// ray along the u=v diagonal at constant height 0.5: (2t-1)^2 = 0.5
	const Vec3 origin(0.0f, 0.5f, 0.0f);
	const Vec3 dir(1.0f, 0.0f, 1.0f); // deliberately non-unit
	hf::Hit hit;
	TASK_CHECK(snap.Intersect(origin, dir, &hit));

	const double s1 = (1.0 - std::sqrt(0.5)) / 2.0; // first diagonal crossing
	TASK_CHECK(Approx(hit.t, s1, 1e-5));
	TASK_CHECK(hit.cellX == 0 && hit.cellZ == 0);
	TASK_CHECK(Approx(hit.u, s1, 1e-5));
	TASK_CHECK(Approx(hit.v, s1, 1e-5));

	// independent double solver must see both roots and agree on the nearest
	auto ref = ReferenceIntersect(ToDouble(h), N, W, D,
	                              {origin.x, origin.y, origin.z}, {dir.x, dir.y, dir.z});
	TASK_CHECK(ref.hit);
	TASK_CHECK(Approx(hit.t, ref.t, 1e-5));
	{
		// explicitly enumerate both roots of the cell equation
		const double h00=1, h10=-1, h01=-1, h11=1;
		const double bu=h10-h00, bv=h01-h00, buv=h00-h10-h01+h11;
		const double dx=1.0, dz=1.0;
		const double u0=0, ud=1.0/dx, v0=0, vd=1.0/dz;
		const double A=buv*ud*vd;
		const double B=bu*ud+bv*vd+buv*(u0*vd+v0*ud)-0.0;
		const double C=h00-0.5;
		const double disc=B*B-4*A*C;
		TASK_CHECK(disc > 0.0);
		const double r1=(-B-std::sqrt(disc))/(2*A);
		const double r2=(-B+std::sqrt(disc))/(2*A);
		TASK_CHECK(r1 > 0.0 && r2 > r1 && r2 < 1.0);
		TASK_CHECK(Approx(r1, s1, 1e-12));
	}

	// point and geometric normal against the double reference
	TASK_CHECK(Approx(hit.localPoint.x, ref.point.x, 1e-5));
	TASK_CHECK(Approx(hit.localPoint.y, ref.point.y, 1e-5));
	TASK_CHECK(Approx(hit.localPoint.z, ref.point.z, 1e-5));
	TASK_CHECK(Approx(hit.localNormal.x, ref.normal.x, 1e-4));
	TASK_CHECK(Approx(hit.localNormal.y, ref.normal.y, 1e-4));
	TASK_CHECK(Approx(hit.localNormal.z, ref.normal.z, 1e-4));
	return 0;
}

int case_exhaustive_grids()
{
	struct Cfg { int N; int selector; };
	const Cfg cfgs[] = {{2, 0}, {2, 1}, {4, 1}, {4, 2}};

	for (const Cfg& cfg : cfgs)
	{
		const int N = cfg.N;
		const float W = 3.0f, D = 2.0f;
		std::vector<float> h = MakeGrid(N, W, D, cfg.selector);
		HeightField field;
		TASK_CHECK(field.Initialize(N, W, D, h.data(), (int)h.size()));
		const auto snap = field.TakeSnapshot();
		auto refH = ToDouble(h);

		std::mt19937 rng(1234 + N*7 + cfg.selector);
		std::uniform_real_distribution<float> jitter(0.0f, 1.0f);

		int rays = 0;
		for (int oy = 0; oy < 6; ++oy)
		{
			for (int ox = 0; ox < 8; ++ox)
			{
				for (int oz = 0; oz < 8; ++oz)
				{
					Vec3 origin((ox + jitter(rng))*W/8.0f,
					            1.5f + oy*0.4f,
					            (oz + jitter(rng))*D/8.0f);
					for (int k = 0; k < 8; ++k)
					{
						// downward directions with varied slopes
						const float ang = k*kPi/8.0f + jitter(rng)*0.2f;
						Vec3 dir(std::cos(ang)*0.7f, -1.0f, std::sin(ang)*0.7f);
						if (k == 7) dir = Vec3(0.35f, 0.0f, 0.5f); // grazing/horizontal

						hf::Hit hit;
						const bool got = snap.Intersect(origin, dir, &hit);
						auto ref = ReferenceIntersect(refH, N, W, D,
						                              {origin.x, origin.y, origin.z},
						                              {dir.x, dir.y, dir.z});
						if (got != ref.hit)
						{
							// the only allowed disagreement is an exact tangent
							TASK_CHECK(false);
						}
						if (got)
						{
							TASK_CHECK(hit.cellX == ref.cx && hit.cellZ == ref.cz);
							TASK_CHECK(Approx(hit.t, ref.t, 1e-5));
							TASK_CHECK(Approx(hit.u, ref.u, 1e-5));
							TASK_CHECK(Approx(hit.v, ref.v, 1e-5));
							TASK_CHECK(Approx(hit.localPoint.x, ref.point.x, 1e-5));
							TASK_CHECK(Approx(hit.localPoint.y, ref.point.y, 1e-5));
							TASK_CHECK(Approx(hit.localPoint.z, ref.point.z, 1e-5));
							TASK_CHECK(Approx(hit.localNormal.x, ref.normal.x, 1e-4));
							TASK_CHECK(Approx(hit.localNormal.z, ref.normal.z, 1e-4));
							TASK_CHECK(hit.t > 0.0f);
						}
						++rays;
					}
				}
			}
		}
		TASK_CHECK(rays == 6*8*8*8);
	}
	return 0;
}

int case_shared_edges()
{
	// Rays running exactly through interior grid lines must not leak through
	// shared cell edges. Slopes are steep enough that the ray is forced below
	// the surface band before leaving the xz domain, so a crossing is certain.
	const int N = 4;
	const float W = 4.0f, D = 4.0f;
	std::vector<float> h = MakeGrid(N, W, D, 1);
	HeightField field;
	TASK_CHECK(field.Initialize(N, W, D, h.data(), (int)h.size()));
	const auto snap = field.TakeSnapshot();
	auto refH = ToDouble(h);

	for (int line = 1; line < N; ++line)
	{
		const float zEdge = line * D / N;
		for (int k = 0; k < 5; ++k)
		{
			const Vec3 origin(0.0f, 2.5f + 0.1f*k, zEdge);
			const Vec3 dir(0.6f, -1.0f - 0.05f*k, 0.0f);
			hf::Hit hit;
			TASK_CHECK(snap.Intersect(origin, dir, &hit));
			auto ref = ReferenceIntersect(refH, N, W, D,
			                              {origin.x, origin.y, origin.z},
			                              {dir.x, dir.y, dir.z});
			TASK_CHECK(ref.hit);
			TASK_CHECK(Approx(hit.t, ref.t, 1e-5));
			// the hit stays on the shared edge line in z
			TASK_CHECK(Approx(hit.localPoint.z, zEdge, 1e-4));
		}
	}

	// edge-parallel rays with a z component, crossing interior x and z lines
	for (int line = 1; line < N; ++line)
	{
		const float xEdge = line * W / N;
		const Vec3 origin(xEdge, 3.0f, 0.0f);
		const Vec3 dir(0.0f, -0.9f, 0.7f);
		hf::Hit hit;
		TASK_CHECK(snap.Intersect(origin, dir, &hit));
		auto ref = ReferenceIntersect(refH, N, W, D,
		                              {origin.x, origin.y, origin.z},
		                              {dir.x, dir.y, dir.z});
		TASK_CHECK(ref.hit && Approx(hit.t, ref.t, 1e-5));
		TASK_CHECK(Approx(hit.localPoint.x, xEdge, 1e-4));
	}

	// ray entering from outside through the outer border corner region
	{
		const Vec3 origin(-1.0f, 0.8f, -1.0f);
		const Vec3 dir(1.0f, -0.5f, 1.0f);
		hf::Hit hit;
		TASK_CHECK(snap.Intersect(origin, dir, &hit));
		auto ref = ReferenceIntersect(refH, N, W, D,
		                              {origin.x, origin.y, origin.z},
		                              {dir.x, dir.y, dir.z});
		TASK_CHECK(ref.hit && Approx(hit.t, ref.t, 1e-5));
	}

	// a ray that genuinely stays above the surface remains a miss
	{
		const Vec3 origin(0.0f, 10.0f, 0.0f);
		const Vec3 dir(1.0f, 0.0f, 0.0f);
		hf::Hit hit;
		TASK_CHECK(!snap.Intersect(origin, dir, &hit));
	}
	return 0;
}

int case_instances()
{
	const int N = 4;
	const float W = 4.0f, D = 4.0f;
	std::vector<float> h = MakeGrid(N, W, D, 0);
	HeightField field;
	TASK_CHECK(field.Initialize(N, W, D, h.data(), (int)h.size()));
	const auto snap = field.TakeSnapshot();
	auto refH = ToDouble(h);

	Primitive prim;
	SetHeightField(prim, snap);
	TASK_CHECK(IsHeightField(prim));
	TASK_CHECK(GetHeightField(prim).snapshot.Version() == snap.Version());
	// static instance: 90 degree rotation about y, scale 2, translated
	const Quat rot(Vec3(0.0f, 1.0f, 0.0f), kPi*0.5f);
	const Transform xform(Vec3(1.5f, -0.5f, 2.0f), rot, 2.0f);
	prim.startTransform = prim.endTransform = xform;

	// build a guaranteed-hitting non-unit ray in local space, then transform
	// it into world space with the production transform
	const Vec3 localOrigin(2.0f, 2.5f, 1.5f);
	const Vec3 localDir(0.4f, -2.0f, 0.6f); // deliberately non-unit
	const Vec3 worldOrigin = TransformPoint(xform, localOrigin);
	const Vec3 worldDir = TransformVector(xform, localDir);
	const Ray ray(worldOrigin, worldDir, 0.5f);
	float t;
	Vec3 n;
	TASK_CHECK(PrimitiveIntersect(prim, ray, t, &n));

	// independent double inverse transform: local = q^-1 (world - p) / s
	const DVec3 p = {1.5, -0.5, 2.0};
	const double s = 2.0;
	DVec3 lo = RotateQuat(-rot.x, -rot.y, -rot.z, rot.w,
	                      DVec3{worldOrigin.x, worldOrigin.y, worldOrigin.z} - p) * (1.0 / s);
	DVec3 ld = RotateQuat(-rot.x, -rot.y, -rot.z, rot.w,
	                      DVec3{worldDir.x, worldDir.y, worldDir.z}) * (1.0 / s);
	auto ref = ReferenceIntersect(refH, N, W, D, lo, ld);
	TASK_CHECK(ref.hit);
	// local and world ray parameters coincide: d_world = R (s d_local)
	TASK_CHECK(Approx(t, ref.t, 1e-5));
	// the production inverse transform must recover the original local ray
	{
		const Vec3 plo = InverseTransformPoint(xform, ray.origin);
		const Vec3 pld = InverseTransformVector(xform, ray.dir);
		TASK_CHECK(Approx(plo.x, localOrigin.x, 1e-5));
		TASK_CHECK(Approx(pld.y, localDir.y, 1e-5));
	}

	// world hit point and normal re-derived independently
	DVec3 localPoint = {lo.x + ld.x*ref.t, lo.y + ld.y*ref.t, lo.z + ld.z*ref.t};
	DVec3 worldPoint = p + RotateQuat(rot.x, rot.y, rot.z, rot.w, localPoint*s);
	DVec3 worldNormal = normalize(RotateQuat(rot.x, rot.y, rot.z, rot.w, ref.normal));
	const Vec3 prodPoint = ray.origin + ray.dir*t;
	TASK_CHECK(Approx(prodPoint.x, worldPoint.x, 1e-4));
	TASK_CHECK(Approx(prodPoint.y, worldPoint.y, 1e-4));
	TASK_CHECK(Approx(prodPoint.z, worldPoint.z, 1e-4));
	TASK_CHECK(Approx(n.x, worldNormal.x, 1e-4));
	TASK_CHECK(Approx(n.y, worldNormal.y, 1e-4));
	TASK_CHECK(Approx(n.z, worldNormal.z, 1e-4));

	// positive uniform scale is required: a non-positive scale rejects hits
	Primitive bad = prim;
	bad.startTransform.s = bad.endTransform.s = -1.0f;
	float t2; Vec3 n2;
	TASK_CHECK(!PrimitiveIntersect(bad, ray, t2, &n2));

	// bounds enter the scene BVH path and contain the transformed surface
	const Bounds b = PrimitiveBounds(prim);
	TASK_CHECK(b.lower.x < b.upper.x && b.lower.y < b.upper.y && b.lower.z < b.upper.z);
	return 0;
}

int case_hierarchy_invariants()
{
	const int N = 8;
	const float W = 8.0f, D = 8.0f;
	std::vector<float> h = MakeGrid(N, W, D, 1);
	HeightField field;
	TASK_CHECK(field.Initialize(N, W, D, h.data(), (int)h.size()));
	auto snap = field.TakeSnapshot();
	const auto& data = *snap.Data();

	// every node bounds must equal the union of its children (or the exact
	// cell corner AABB for a leaf), and leaves cover exactly one cell.
	std::vector<const hf::HFNode*> stack(1, data.root.get());
	int internalNodes = 0, leafNodes = 0;
	while (!stack.empty())
	{
		const hf::HFNode* node = stack.back();
		stack.pop_back();

		if (node->IsLeaf())
		{
			TASK_CHECK(node->size == 1);
			++leafNodes;
			const float dx = W / N, dz = D / N;
			float ymin = 1e30f, ymax = -1e30f;
			for (int zz = node->cellZ; zz <= node->cellZ+1; ++zz)
				for (int xx = node->cellX; xx <= node->cellX+1; ++xx)
				{
					const float hh = data.Height(xx, zz);
					ymin = std::min(ymin, hh);
					ymax = std::max(ymax, hh);
				}
			TASK_CHECK(Approx(node->bounds.lower.x, node->cellX*dx, 1e-6));
			TASK_CHECK(Approx(node->bounds.upper.z, (node->cellZ+1)*dz, 1e-6));
			TASK_CHECK(Approx(node->bounds.lower.y, ymin, 1e-6));
			TASK_CHECK(Approx(node->bounds.upper.y, ymax, 1e-6));
		}
		else
		{
			++internalNodes;
			Bounds u = node->child[0]->bounds;
			for (int c = 1; c < 4; ++c)
			{
				u = Union(u, node->child[c]->bounds);
				stack.push_back(node->child[c].get());
			}
			stack.push_back(node->child[0].get());
			TASK_CHECK(Approx(node->bounds.lower.x, u.lower.x, 1e-6));
			TASK_CHECK(Approx(node->bounds.lower.y, u.lower.y, 1e-6));
			TASK_CHECK(Approx(node->bounds.lower.z, u.lower.z, 1e-6));
			TASK_CHECK(Approx(node->bounds.upper.x, u.upper.x, 1e-6));
			TASK_CHECK(Approx(node->bounds.upper.y, u.upper.y, 1e-6));
			TASK_CHECK(Approx(node->bounds.upper.z, u.upper.z, 1e-6));
		}
	}
	TASK_CHECK(leafNodes == N*N);

	// a spike edited beyond the old root bounds must remain intersectable:
	// the ancestor chain's bounds grow with the edit.
	const Bounds oldBounds = HeightFieldBounds(snap);
	const float spikeY = oldBounds.upper.y + 5.0f;
	hf::Edit edit{N/2, N/2, spikeY};
	hf::EditStats stats;
	TASK_CHECK(field.Update(&edit, 1, &stats));
	auto snap2 = field.TakeSnapshot();
	const Bounds newBounds = HeightFieldBounds(snap2);
	TASK_CHECK(newBounds.upper.y >= spikeY);
	TASK_CHECK(newBounds.upper.y > oldBounds.upper.y);

	const Vec3 origin(W*0.5f, spikeY + 1.0f, D*0.5f);
	const Vec3 dir(0.0f, -1.0f, 0.0f);
	hf::Hit hit;
	TASK_CHECK(snap2.Intersect(origin, dir, &hit));
	TASK_CHECK(Approx(hit.localPoint.y, spikeY, 1e-5));
	// the old snapshot keeps the old surface: its first hit is still below
	// the old bounds ceiling, never at the spike height
	hf::Hit oldHit;
	TASK_CHECK(snap.Intersect(origin, dir, &oldHit));
	TASK_CHECK(oldHit.localPoint.y <= oldBounds.upper.y + 1e-5f);
	TASK_CHECK(oldHit.localPoint.y < spikeY - 1.0f);
	TASK_CHECK(oldHit.t > hit.t);
	return 0;
}

// ---------------------------------------------------------------------------
// updates group
// ---------------------------------------------------------------------------

int case_invalid_then_retry()
{
	const int N = 4;
	std::vector<float> h = MakeGrid(N, 4.0f, 4.0f, 0);
	HeightField field;
	TASK_CHECK(field.Initialize(N, 4.0f, 4.0f, h.data(), (int)h.size()));
	TASK_CHECK(field.Initialized());
	TASK_CHECK(field.GridSize() == N);
	const HeightField::Snapshot goodSnap = field.TakeSnapshot();
	TASK_CHECK(goodSnap.Valid() && goodSnap.GridSize() == N);
	TASK_CHECK(Approx(goodSnap.Width(), 4.0f, 0.0) && Approx(goodSnap.Depth(), 4.0f, 0.0));
	HeightField::Snapshot emptySnap;
	TASK_CHECK(!emptySnap.Valid());
	const int version0 = field.Version();
	auto before = field.TakeSnapshot();

	hf::Edit bad1[]{ {0, 0, 1.0f}, {N+1, 0, 2.0f} };
	hf::Edit bad2[]{ {0, -1, 1.0f} };
	hf::Edit bad3[]{ {0, 0, std::numeric_limits<float>::infinity()} };
	hf::EditStats stats;
	TASK_CHECK(!field.Update(bad1, 2, &stats));
	TASK_CHECK(!field.Update(bad2, 1, &stats));
	TASK_CHECK(!field.Update(bad3, 1, &stats));
	TASK_CHECK(stats.leavesRecomputed == 0 && stats.nodesRecomputed == 0);
	TASK_CHECK(field.Version() == version0);

	// nothing moved: heights and bounds unchanged
	auto still = field.TakeSnapshot();
	TASK_CHECK(Approx(still.SamplePosition(0.0f, 0.0f).y, before.SamplePosition(0.0f, 0.0f).y, 0.0));
	TASK_CHECK(HeightFieldBounds(still).upper.y == HeightFieldBounds(before).upper.y);

	// a valid retry now succeeds and bumps the version exactly once
	hf::Edit good[]{ {0, 0, h[0] + 0.7f} };
	TASK_CHECK(field.Update(good, 1, &stats));
	TASK_CHECK(field.Version() == version0 + 1);
	TASK_CHECK(stats.leavesRecomputed == 1); // corner vertex touches one cell
	TASK_CHECK(Approx(field.TakeSnapshot().SamplePosition(0.0f, 0.0f).y, h[0] + 0.7f, 1e-6));

	// uninitialized field rejects edits
	HeightField empty;
	TASK_CHECK(!empty.Update(good, 1, &stats));
	// invalid construction arguments
	HeightField bad;
	TASK_CHECK(!bad.Initialize(3, 1.0f, 1.0f, h.data(), (int)h.size())); // not power of two
	TASK_CHECK(!bad.Initialize(2, 0.0f, 1.0f, h.data(), 9));
	TASK_CHECK(!bad.Initialize(2, 1.0f, -1.0f, h.data(), 9));
	return 0;
}

int case_empty_and_unchanged()
{
	const int N = 4;
	std::vector<float> h = MakeGrid(N, 4.0f, 4.0f, 1);
	HeightField field;
	TASK_CHECK(field.Initialize(N, 4.0f, 4.0f, h.data(), (int)h.size()));
	const int v0 = field.Version();
	hf::EditStats stats;

	TASK_CHECK(field.Update(NULL, 0, &stats));
	TASK_CHECK(field.Version() == v0);
	TASK_CHECK(stats.leavesRecomputed == 0 && stats.nodesRecomputed == 0);

	// duplicate coordinates: last value wins, and if it equals the current
	// height no version is produced
	const float current = h[2*(N+1) + 1];
	hf::Edit none[]{ {1, 2, current + 1.0f}, {1, 2, current} };
	TASK_CHECK(field.Update(none, 2, &stats));
	TASK_CHECK(field.Version() == v0);
	TASK_CHECK(stats.leavesRecomputed == 0 && stats.nodesRecomputed == 0);

	// same final value through a duplicate still counts as a real change
	hf::Edit change[]{ {1, 2, current + 1.0f}, {1, 2, current + 0.5f} };
	TASK_CHECK(field.Update(change, 2, &stats));
	TASK_CHECK(field.Version() == v0 + 1);
	TASK_CHECK(Approx(field.TakeSnapshot().SamplePosition(1.0f, 2.0f).y, current + 0.5f, 1e-6));
	return 0;
}

int case_single_vertex_work()
{
	// 257x257 vertices. The exact rebuilt node count depends on how the
	// touched cells align with the fixed quadtree, so it is predicted by an
	// independent ancestor-union computation rather than a hardcoded formula.
	const int N = 256;
	std::vector<float> h((N+1)*(N+1), 0.0f);
	for (int z = 0; z <= N; ++z)
		for (int x = 0; x <= N; ++x)
			h[z*(N+1)+x] = 0.01f*std::sin(x*0.3f) + 0.02f*std::cos(z*0.17f);

	HeightField field;
	TASK_CHECK(field.Initialize(N, float(N), float(N), h.data(), (int)h.size()));

	int logN = 0; for (int s = N; s > 1; s /= 2) ++logN;
	const int fullTreeNodes = (4*N*N - 1) / 3;

	// independent prediction: union of touched leaves and every ancestor block
	auto expectedWork = [&](const std::vector<hf::Edit>& edits, int& leaves)
	{
		std::vector<long long> blocks;
		auto mark = [&](int level, int x0, int z0)
		{
			const long long key = ((long long)level*N + z0)*N + x0;
			if (std::find(blocks.begin(), blocks.end(), key) == blocks.end())
				blocks.push_back(key);
		};
		leaves = 0;
		for (const hf::Edit& e : edits)
		{
			const int x0 = std::max(0, e.x - 1);
			const int x1 = std::min(N - 1, e.x);
			const int z0 = std::max(0, e.z - 1);
			const int z1 = std::min(N - 1, e.z);
			for (int cz = z0; cz <= z1; ++cz)
				for (int cx = x0; cx <= x1; ++cx)
				{
					// level logN is the leaf level
					if (std::find(blocks.begin(), blocks.end(),
					              ((long long)logN*N + cz)*N + cx) == blocks.end())
					{
						mark(logN, cx, cz);
						++leaves;
					}
				}
		}
		// grow every block's ancestor chain up to the root
		std::vector<long long> frontier = blocks;
		for (int level = logN; level > 0; --level)
		{
			std::vector<long long> parents;
			for (long long key : frontier)
			{
				const int x0 = int(key % N);
				const int z0 = int((key / N) % N);
				const int lvl = int(key / ((long long)N*N));
				(void)lvl;
				const int size = N >> level;
				const int px = (x0 / (2*size)) * (2*size);
				const int pz = (z0 / (2*size)) * (2*size);
				const long long pkey = ((long long)(level - 1)*N + pz)*N + px;
				if (std::find(parents.begin(), parents.end(), pkey) == parents.end())
					parents.push_back(pkey);
			}
			for (long long key : parents)
				if (std::find(blocks.begin(), blocks.end(), key) == blocks.end())
					blocks.push_back(key);
			frontier = parents;
		}
		return (int)blocks.size();
	};

	hf::EditStats stats;

	// interior center vertex: exactly four leaves, four independent paths that
	// only coalesce at the root (the center lines split the root quadrants)
	const hf::Edit interior{N/2, N/2, 3.0f};
	int expLeaves;
	const int expNodes = expectedWork({interior}, expLeaves);
	TASK_CHECK(field.Update(&interior, 1, &stats));
	TASK_CHECK(stats.leavesRecomputed <= 4);
	TASK_CHECK(stats.leavesRecomputed == 4 && expLeaves == 4);
	TASK_CHECK(stats.nodesRecomputed == expNodes);
	TASK_CHECK(stats.nodesRecomputed == 4*logN + 1); // worst-case path spread
	TASK_CHECK(stats.nodesRecomputed < fullTreeNodes / 1000); // strictly local

	// corner vertex: one leaf, one straight path to the root
	const hf::Edit corner{0, 0, -2.0f};
	const int cornerNodes = expectedWork({corner}, expLeaves);
	TASK_CHECK(field.Update(&corner, 1, &stats));
	TASK_CHECK(stats.leavesRecomputed == 1 && expLeaves == 1);
	TASK_CHECK(stats.nodesRecomputed == cornerNodes);
	TASK_CHECK(stats.nodesRecomputed == 1 + logN);

	// border (non-corner) vertex: two leaves
	const hf::Edit border{N, 3, 1.5f};
	const int borderNodes = expectedWork({border}, expLeaves);
	TASK_CHECK(field.Update(&border, 1, &stats));
	TASK_CHECK(stats.leavesRecomputed == 2 && expLeaves == 2);
	TASK_CHECK(stats.nodesRecomputed == borderNodes);
	TASK_CHECK(stats.nodesRecomputed == 2 + logN);

	// two far-apart interior edits: eight leaves, merging only at the root
	const hf::Edit pair[]{ {10, 10, 2.2f}, {200, 200, -2.2f} };
	const int pairNodes = expectedWork({pair[0], pair[1]}, expLeaves);
	TASK_CHECK(field.Update(pair, 2, &stats));
	TASK_CHECK(stats.leavesRecomputed == 8 && expLeaves == 8);
	TASK_CHECK(stats.nodesRecomputed == pairNodes);
	TASK_CHECK(stats.nodesRecomputed < fullTreeNodes / 1000); // strictly local

	// storage GC and sharing: with no snapshot retained, the live graph must
	// stay exactly the fixed quadtree size, and untouched subtrees must be
	// shared by pointer identity with the previous generation rather than copied
	{
		auto countNodes = [](const hf::HFData& dd)
		{
			std::vector<const hf::HFNode*> walk(1, dd.root.get());
			int n = 0;
			while (!walk.empty())
			{
				const hf::HFNode* node = walk.back(); walk.pop_back();
				++n;
				if (!node->IsLeaf())
					for (int c = 0; c < 4; ++c) walk.push_back(node->child[c].get());
			}
			return n;
		};

		const HeightField::Snapshot before = field.TakeSnapshot();
		const std::shared_ptr<const hf::HFNode> farSubtreeBefore =
			before.Data()->root->child[0]->child[0]; // untouched region
		for (int i = 0; i < 12; ++i)
		{
			const hf::Edit e{128 + (i % 16), 128 + (i % 16), 1.0f + 0.05f*i};
			TASK_CHECK(field.Update(&e, 1, &stats));
		}
		const HeightField::Snapshot after = field.TakeSnapshot();
		// the live tree never grows beyond the fixed node count: superseded
		// nodes are released as older generations are dropped
		TASK_CHECK(countNodes(*after.Data()) == fullTreeNodes);
		// the far, untouched subtree is the exact same node object (no copy)
		TASK_CHECK(after.Data()->root->child[0]->child[0].get() == farSubtreeBefore.get());
		// the old snapshot still owns a complete, independent generation
		TASK_CHECK(countNodes(*before.Data()) == fullTreeNodes);
	}
	return 0;
}

int case_snapshot_interleave()
{
	const int N = 8;
	std::vector<float> h = MakeGrid(N, 4.0f, 4.0f, 0);
	HeightField field;
	TASK_CHECK(field.Initialize(N, 4.0f, 4.0f, h.data(), (int)h.size()));

	const HeightField::Snapshot snap1 = field.TakeSnapshot();
	const Bounds b1 = HeightFieldBounds(snap1);
	const Vec3 p1 = snap1.SamplePosition(2.0f, 1.5f);
	const Vec3 n1 = snap1.SampleNormal(2.0f, 1.5f);
	const float pdf1 = snap1.LocalAreaPdf(2.0f, 1.5f);

	hf::Edit e1{4, 4, b1.upper.y + 4.0f};
	hf::EditStats stats;
	TASK_CHECK(field.Update(&e1, 1, &stats));
	const HeightField::Snapshot snap2 = field.TakeSnapshot();

	hf::Edit e2{1, 1, -3.0f};
	TASK_CHECK(field.Update(&e2, 1, &stats));
	const HeightField::Snapshot snap3 = field.TakeSnapshot();

	// old snapshots keep the old surface, bounds, normals, densities
	TASK_CHECK(snap1.Version() == 1 && snap2.Version() == 2 && snap3.Version() == 3);
	TASK_CHECK(snap1.SamplePosition(2.0f, 1.5f).y == p1.y);
	TASK_CHECK(snap1.SampleNormal(2.0f, 1.5f).y == n1.y);
	TASK_CHECK(snap1.LocalAreaPdf(2.0f, 1.5f) == pdf1);
	TASK_CHECK(HeightFieldBounds(snap1).upper.y == b1.upper.y);
	TASK_CHECK(HeightFieldBounds(snap2).upper.y >= b1.upper.y + 4.0f - 1e-6f);
	TASK_CHECK(HeightFieldBounds(snap3).lower.y <= -3.0f);

	// primitives pin their snapshot generation independently
	Primitive oldPrim, newPrim;
	SetHeightField(oldPrim, snap1);
	SetHeightField(newPrim, snap3);
	oldPrim.startTransform = oldPrim.endTransform = Transform(Vec3(0.0f));
	newPrim.startTransform = newPrim.endTransform = Transform(Vec3(0.0f));

	const Ray ray(Vec3(2.0f, 5.0f, 2.0f), Vec3(0.0f, -1.0f, 0.0f), 0.5f);
	float tOld, tNew; Vec3 nOld, nNew;
	TASK_CHECK(PrimitiveIntersect(oldPrim, ray, tOld, &nOld));
	TASK_CHECK(PrimitiveIntersect(newPrim, ray, tNew, &nNew));
	TASK_CHECK(tOld != tNew);
	TASK_CHECK(Approx(tOld, 5.0f - p1_at_2_2(snap1), 1e-4));

	// scene BVH accepts both generations in one scene and filters instances
	Scene scene;
	scene.AddPrimitive(oldPrim);
	scene.AddPrimitive(newPrim);
	scene.Build();
	TASK_CHECK(scene.bvh.numNodes > 0);
	float tScene; Vec3 nScene; const Primitive* hitPrim = NULL;
	// simple closest-hit loop over the scene primitives as an independent check
	float best = 1e30f;
	for (const Primitive& pr : scene.primitives)
	{
		float tt; Vec3 nn;
		if (PrimitiveIntersect(pr, ray, tt, &nn) && tt < best)
		{
			best = tt;
			hitPrim = &pr;
		}
	}
	(void)tOld; (void)tNew; (void)nOld; (void)nNew; (void)tScene; (void)nScene;
	TASK_CHECK(hitPrim != NULL);

	delete[] scene.bvh.nodes;
	scene.bvh.nodes = NULL;
	return 0;
}

// helper used above (declared after to keep the case body readable)
float p1_at_2_2(const HeightField::Snapshot& s)
{
	return s.SamplePosition(2.0f, 2.0f).y;
}

// ---------------------------------------------------------------------------
// lighting group
// ---------------------------------------------------------------------------

int case_area_pdf()
{
	const int N = 8;
	const float W = 4.0f, D = 3.0f;
	std::vector<float> h = MakeGrid(N, W, D, 1);
	HeightField field;
	TASK_CHECK(field.Initialize(N, W, D, h.data(), (int)h.size()));
	const auto snap = field.TakeSnapshot();
	auto refH = ToDouble(h);

	auto refDeriv = [&](double x, double z, double& hx, double& hz)
	{
		const double dx = W / N, dz = D / N;
		int cx = std::min(N-1, std::max(0, int(x / dx)));
		int cz = std::min(N-1, std::max(0, int(z / dz)));
		const int stride = N + 1;
		auto H = [&](int xx, int zz) { return refH[zz*stride+xx]; };
		const double h00=H(cx,cz), h10=H(cx+1,cz), h01=H(cx,cz+1), h11=H(cx+1,cz+1);
		const double bu=h10-h00, bv=h01-h00, buv=h00-h10-h01+h11;
		const double u=(x-cx*dx)/dx, v=(z-cz*dz)/dz;
		hx = (bu + buv*v) / dx;
		hz = (bv + buv*u) / dz;
	};

	// pointwise values against the independent double derivatives
	for (int i = 0; i < 20; ++i)
	{
		const float x = 0.2f + 0.15f*i;
		const float z = 0.15f + 0.11f*i;
		double hx, hz;
		refDeriv(x, z, hx, hz);
		const double expected = 1.0 / (W*D*std::sqrt(1.0 + hx*hx + hz*hz));
		TASK_CHECK(Approx(snap.LocalAreaPdf(x, z), expected, 1e-5));
		// world density divides by the instance scale squared
		TASK_CHECK(Approx(snap.WorldAreaPdf(x, z, 2.0f), expected/4.0, 1e-5));
		TASK_CHECK(Approx(snap.WorldAreaPdf(x, z, 0.5f), expected*4.0, 1e-5));
	}

	// normalization: integral of p_A dA over the surface is one
	{
		const int res = 120;
		double sum = 0.0;
		for (int iz = 0; iz < res; ++iz)
			for (int ix = 0; ix < res; ++ix)
			{
				const double x = (ix + 0.5)*W/res;
				const double z = (iz + 0.5)*D/res;
				double hx, hz;
				refDeriv(x, z, hx, hz);
				const double jac = std::sqrt(1.0 + hx*hx + hz*hz);
				sum += snap.LocalAreaPdf(float(x), float(z)) * jac * (W/res) * (D/res);
			}
		TASK_CHECK(Approx(sum, 1.0, 1e-3));
	}

	// independent area-difference check on a small parameter rectangle
	{
		const float x0 = 1.1f, z0 = 0.7f, ddx = 0.05f, ddz = 0.07f;
		DVec3 p00 = {snap.SamplePosition(x0, z0).x, snap.SamplePosition(x0, z0).y, snap.SamplePosition(x0, z0).z};
		DVec3 p10 = {snap.SamplePosition(x0+ddx, z0).x, snap.SamplePosition(x0+ddx, z0).y, snap.SamplePosition(x0+ddx, z0).z};
		DVec3 p01 = {snap.SamplePosition(x0, z0+ddz).x, snap.SamplePosition(x0, z0+ddz).y, snap.SamplePosition(x0, z0+ddz).z};
		DVec3 p11 = {snap.SamplePosition(x0+ddx, z0+ddz).x, snap.SamplePosition(x0+ddx, z0+ddz).y, snap.SamplePosition(x0+ddx, z0+ddz).z};
		const double area = 0.5*length(cross(p10-p00, p01-p00)) + 0.5*length(cross(p11-p10, p11-p01));
		const double measuredPdf = (ddx*ddz/(W*D)) / area;
		TASK_CHECK(Approx(snap.LocalAreaPdf(x0, z0), measuredPdf, 5e-3));
	}

	// sampling and pdf share one convention: uniform parameters over xz
	{
		Random rand(7);
		double sx = 0.0, sz = 0.0;
		const int M = 4000;
		for (int i = 0; i < M; ++i)
		{
			Primitive prim;
			SetHeightField(prim, snap);
			prim.startTransform = prim.endTransform = Transform(Vec3(0.0f));
			Vec3 wp, wn;
			HeightFieldSample(prim, rand.Randf(), rand.Randf(), wp, wn);
			sx += wp.x;
			sz += wp.z;
		}
		sx /= M; sz /= M;
		TASK_CHECK(std::fabs(sx - W*0.5) < 0.05);
		TASK_CHECK(std::fabs(sz - D*0.5) < 0.05);
	}
	return 0;
}

int case_direction_pdf()
{
	const int N = 8;
	const float W = 4.0f, D = 3.0f;
	const float scale = 1.5f;
	std::vector<float> h = MakeGrid(N, W, D, 0);
	HeightField field;
	TASK_CHECK(field.Initialize(N, W, D, h.data(), (int)h.size()));
	const auto snap = field.TakeSnapshot();
	auto refH = ToDouble(h);

	Primitive prim;
	SetHeightField(prim, snap);
	const Quat rot(Vec3(0.0f, 1.0f, 0.0f), 0.6f);
	prim.startTransform = prim.endTransform = Transform(Vec3(0.5f, 0.0f, -0.5f), rot, scale);

	const DVec3 refPoint = {1.2, 2.4, 0.8};

	auto refWorld = [&](double lx, double ly, double lz)
	{
		DVec3 lp = {lx, ly, lz};
		DVec3 r = RotateQuat(rot.x, rot.y, rot.z, rot.w, lp*scale);
		return DVec3{0.5 + r.x, 0.0 + r.y, -0.5 + r.z};
	};

	// pointwise solid-angle density vs an independent double derivation
	for (int i = 0; i < 12; ++i)
	{
		const float u = 0.1f + 0.07f*i;
		const float v = 0.15f + 0.06f*i;
		const Vec3 local = snap.SampleParam(u, v);
		Vec3 wp, wn;
		HeightFieldSample(prim, u, v, wp, wn);

		const DVec3 wpd = refWorld(local.x, local.y, local.z);
		const DVec3 towards = normalize(refPoint - wpd);
		const double r2 = dot(refPoint - wpd, refPoint - wpd);

		// independent derivatives in local space
		const double dx = W/N, dz = D/N;
		int cx = std::min(N-1, std::max(0, int(local.x/dx)));
		int cz = std::min(N-1, std::max(0, int(local.z/dz)));
		const int stride = N+1;
		auto H = [&](int xx, int zz){ return refH[zz*stride+xx]; };
		const double h00=H(cx,cz), h10=H(cx+1,cz), h01=H(cx,cz+1), h11=H(cx+1,cz+1);
		const double bu=h10-h00, bv=h01-h00, buv=h00-h10-h01+h11;
		const double uu=(local.x-cx*dx)/dx, vv=(local.z-cz*dz)/dz;
		const double hx=(bu+buv*vv)/dx, hz=(bv+buv*uu)/dz;
		const double jac = std::sqrt(1.0+hx*hx+hz*hz);
		const double areaPdfWorld = 1.0/(W*D*jac*scale*scale);

		// world normal independently, cosine against the connection
		const DVec3 nLocal = normalize({-hx, 1.0, -hz});
		const DVec3 nWorld = normalize(RotateQuat(rot.x, rot.y, rot.z, rot.w, nLocal));
		const double cosAbs = std::fabs(dot(nWorld, towards));
		const double expectedPdf = areaPdfWorld*r2/cosAbs;

		const float gotPdf = HeightFieldWorldAreaPdf(prim, wp, 0.5f)*r2/cosAbs;
		TASK_CHECK(Approx(gotPdf, expectedPdf, 1e-4));
		// the public solid-angle helper must encode the same convention
		const float helperPdf = HeightFieldDirectionPdf(prim, wp,
		                                                Vec3(float(refPoint.x), float(refPoint.y), float(refPoint.z)), 0.5f);
		TASK_CHECK(Approx(helperPdf, expectedPdf, 1e-4));

		// zero cosine is allowed but contributes nothing; pdf diverges
		TASK_CHECK(cosAbs > 1e-3);
	}

	// direction integration over the whole patch: sum p_omega d_omega = 1
	{
		const int res = 60;
		double total = 0.0;
		for (int iz = 0; iz < res; ++iz)
			for (int ix = 0; ix < res; ++ix)
			{
				const float u = (ix + 0.5f)/res;
				const float v = (iz + 0.5f)/res;
				const Vec3 local = snap.SampleParam(u, v);
				Vec3 wp, wn;
				HeightFieldSample(prim, u, v, wp, wn);
				const DVec3 wpd = refWorld(local.x, local.y, local.z);
				const DVec3 dir = refPoint - wpd;
				const double r2 = dot(dir, dir);
				const double cosAbs = std::fabs(dot(normalize({wn.x, wn.y, wn.z}), normalize(dir)));
				// exact local jacobian recovered from the local area pdf:
				// p_A,local = 1/(W D J)  =>  J = 1/(W D p_A,local)
				const double J = 1.0/(W*D*snap.LocalAreaPdf(local.x, local.z));
				const double dOmega = cosAbs*(scale*scale*J*(W/res)*(D/res))/r2;
				const double areaPdf = HeightFieldWorldAreaPdf(prim, wp, 0.5f);
				const double pOmega = areaPdf*r2/cosAbs;
				total += pOmega*dOmega;
			}
		TASK_CHECK(Approx(total, 1.0, 2e-2));
	}
	return 0;
}

// Closest hit over a small scene using production intersection, mirroring the
// renderer's scene query without depending on its file-local Trace helper.
const Primitive* TracePrims(const std::vector<Primitive>& prims, const Ray& ray,
                            float& outT, Vec3& outN)
{
	const Primitive* best = NULL;
	outT = 1e30f;
	for (const Primitive& p : prims)
	{
		float t; Vec3 n;
		if (PrimitiveIntersect(p, ray, t, &n) && t > 0.0f && t < outT)
		{
			outT = t;
			outN = n;
			best = &p;
		}
	}
	return best;
}

int case_cpu_sampling_means()
{
	// Fixed-seed comparison of the two unbiased estimators of the same direct
	// illumination integral:
	//   * light sampling driven by the uniform-parameter convention and the
	//     pointwise solid-angle density,
	//   * BSDF (direction) sampling with the production BSDF pdf.
	// The balance-heuristic combination used by CpuRenderer is exercised by the
	// end-to-end renderer check below; here the unweighted means must agree
	// because both estimator expectations equal the same integral.
	const int N = 16;
	const float W = 10.0f, D = 10.0f;
	// large, almost flat overhead light so direction sampling gets many hits
	std::vector<float> lightH((N+1)*(N+1), 0.3f);
	std::vector<float> floorH((N+1)*(N+1), 0.0f);

	HeightField lightField, floorField;
	TASK_CHECK(lightField.Initialize(N, W, D, lightH.data(), (int)lightH.size()));
	TASK_CHECK(floorField.Initialize(N, W, D, floorH.data(), (int)floorH.size()));

	Primitive light, floor;
	SetHeightField(light, lightField.TakeSnapshot());
	SetHeightField(floor, floorField.TakeSnapshot());
	light.startTransform = light.endTransform = Transform(Vec3(-W*0.5f, 3.0f, -D*0.5f));
	floor.startTransform = floor.endTransform = Transform(Vec3(-W*0.5f, -1.0f, -D*0.5f));
	light.material.emission = Vec3(3.0f, 2.5f, 2.0f);
	light.lightSamples = 4;
	Material receiver;
	receiver.color = Vec3(0.8f);
	receiver.specular = 0.0f;
	receiver.metallic = 0.0f;
	receiver.roughness = 0.9f;
	floor.material = receiver;

	std::vector<Primitive> scene;
	scene.push_back(light);
	scene.push_back(floor);

	const Vec3 p(0.0f, -0.98f, 0.0f);
	const Vec3 n(0.0f, 1.0f, 0.0f);
	const Vec3 wo(0.0f, 1.0f, 0.0f);
	const int M = 12000;

	Random randA(101), randB(202);
	Vec3 meanLight(0.0f), meanBsdf(0.0f);
	int lightHits = 0, bsdfHits = 0;

	for (int i = 0; i < M; ++i)
	{
		// --- unbiased light-sampling estimator (no MIS weight) ---
		Vec3 lp, ln;
		PrimitiveSample(light, 0.5f, lp, ln, randA);
		Vec3 wi = lp - p;
		const float dSq = LengthSq(wi);
		const float dist = sqrtf(dSq);
		wi = wi / dist;
		const Ray shadow(p + n*0.0001f, wi, 0.5f);
		float t; Vec3 nHit;
		const Primitive* hit = TracePrims(scene, shadow, t, nHit);
		if (hit == &scene[0] && std::fabs(t - dist) < 1e-2f)
		{
			const float nl = Abs(Dot(ln, wi));
			if (nl >= 1e-6f)
			{
				const float areaPdf = HeightFieldWorldAreaPdf(light, lp, 0.5f);
				const float lightPdf = areaPdf*dSq/nl;
				const Vec3 f = BSDFEval(receiver, 1.0f, 1.0f, p, n, wo, wi);
				meanLight += f*light.material.emission*(Abs(Dot(wi, n))/lightPdf);
				++lightHits;
			}
		}

		// --- unbiased BSDF (direction) estimator (no MIS weight) ---
		Vec3 u, v;
		BasisFromVector(n, &u, &v);
		Vec3 wiB;
		float bsdfPdf;
		BSDFType type;
		BSDFSample(receiver, 1.0f, 1.0f, p, u, v, n, wo, wiB, bsdfPdf, type, randB);
		if (bsdfPdf > 0.0f && Dot(wiB, n) > 0.0f)
		{
			const Ray ray(p + n*0.0001f, wiB, 0.5f);
			float tB; Vec3 nB;
			const Primitive* hitB = TracePrims(scene, ray, tB, nB);
			if (hitB == &scene[0])
			{
				const Vec3 f = BSDFEval(receiver, 1.0f, 1.0f, p, n, wo, wiB);
				meanBsdf += f*light.material.emission*(Abs(Dot(n, wiB))/bsdfPdf);
				++bsdfHits;
			}
		}
	}
	meanLight = meanLight * (1.0f/M);
	meanBsdf = meanBsdf * (1.0f/M);

	const double rel = std::fabs(double(lum3(meanLight) - lum3(meanBsdf)))
	                  / std::max(1e-6, double(lum3(meanLight)));
	std::printf("LIGHTING samples=%d lightHits=%d bsdfHits=%d direct=(%.4f,%.4f,%.4f) bsdf=(%.4f,%.4f,%.4f) relErr=%.4f\n",
	            M, lightHits, bsdfHits,
	            meanLight.x, meanLight.y, meanLight.z, meanBsdf.x, meanBsdf.y, meanBsdf.z, rel);
	TASK_CHECK(lum3(meanLight) > 0.0f && lum3(meanBsdf) > 0.0f);
	TASK_CHECK(lightHits > M/2 && bsdfHits > M/10);
	TASK_CHECK(rel < 0.10);

	// end-to-end CpuRenderer: the real path tracer handles the HF light
	Scene scene2;
	scene2.AddPrimitive(light);
	scene2.AddPrimitive(floor);
	scene2.Build();
	std::unique_ptr<Renderer> renderer(CreateCpuRenderer(&scene2));
	Camera camera;
	camera.position = Vec3(0.0f, 0.6f, 3.2f);
	camera.rotation = Quat();
	camera.fov = DegToRad(45.0f);
	Options o{};
	o.width = o.height = 16;
	o.mode = ePathTrace;
	o.maxDepth = 3;
	o.maxSamples = 1;
	o.clamp = 100.0f;
	o.filter = Filter(eFilterBox, 0.0f);
	std::vector<Color> pixels(256, Color(0.0f));
	renderer->Render(camera, o, pixels.data());
	int finiteCount = 0, lit = 0;
	for (const Color& c : pixels)
	{
		if (std::isfinite(c.x) && std::isfinite(c.y) && std::isfinite(c.z))
			++finiteCount;
		if (lum3(Vec3(c)) > 0.0f)
			++lit;
	}
	TASK_CHECK(finiteCount == 256);
	TASK_CHECK(lit > 50);
	delete[] scene2.bvh.nodes;
	scene2.bvh.nodes = NULL;
	return 0;
}

int case_edited_light_and_occluder()
{
	// Edit both the emitting height field and an occluding one, swap in new
	// snapshots, and confirm the CPU render changes while staying finite.
	const int N = 16;
	const float W = 6.0f, D = 6.0f;
	std::vector<float> lightH((N+1)*(N+1), 0.3f);
	std::vector<float> occH((N+1)*(N+1), 0.0f);

	HeightField lightField, occField;
	TASK_CHECK(lightField.Initialize(N, W, D, lightH.data(), (int)lightH.size()));
	TASK_CHECK(occField.Initialize(N, W, D, occH.data(), (int)occH.size()));
	// retain the pre-edit generations; they must keep rendering the old scene
	const HeightField::Snapshot oldLightSnap = lightField.TakeSnapshot();
	const HeightField::Snapshot oldOccSnap = occField.TakeSnapshot();

	Primitive light, occluder;
	SetHeightField(light, lightField.TakeSnapshot());
	SetHeightField(occluder, occField.TakeSnapshot());
	light.startTransform = light.endTransform = Transform(Vec3(-W*0.5f, 3.0f, -D*0.5f));
	occluder.startTransform = occluder.endTransform = Transform(Vec3(-W*0.5f, 0.2f, -D*0.5f));
	light.material.emission = Vec3(4.0f);
	light.lightSamples = 2;

	auto renderOne = [&](const HeightField::Snapshot& ls, const HeightField::Snapshot& os) -> double
	{
		Primitive l = light, o = occluder;
		SetHeightField(l, ls);
		SetHeightField(o, os);
		Scene scene;
		scene.AddPrimitive(l);
		scene.AddPrimitive(o);
		scene.Build();
		std::unique_ptr<Renderer> renderer(CreateCpuRenderer(&scene));
		Camera camera;
		camera.position = Vec3(0.0f, 0.8f, 3.0f);
		camera.rotation = Quat();
		camera.fov = DegToRad(45.0f);
		Options opt{};
		opt.width = opt.height = 16;
		opt.mode = ePathTrace;
		opt.maxDepth = 3;
		opt.maxSamples = 1;
		opt.clamp = 100.0f;
		opt.filter = Filter(eFilterBox, 0.0f);
		std::vector<Color> px(256, Color(0.0f));
		renderer->Render(camera, opt, px.data());
		double sum = 0.0;
		for (const Color& c : px)
		{
			TASK_CHECK(std::isfinite(c.x) && std::isfinite(c.y) && std::isfinite(c.z));
			sum += lum3(Vec3(c));
		}
		delete[] scene.bvh.nodes;
		scene.bvh.nodes = NULL;
		return sum;
	};

	const double before = renderOne(lightField.TakeSnapshot(), occField.TakeSnapshot());

	hf::EditStats stats;
	hf::Edit lightEdit{N/2, N/2, 1.5f};
	TASK_CHECK(lightField.Update(&lightEdit, 1, &stats));
	hf::Edit occEdits[]{ {N/2, N/2, 2.0f}, {N/2+1, N/2, 2.0f} };
	TASK_CHECK(occField.Update(occEdits, 2, &stats));
	TASK_CHECK(stats.leavesRecomputed <= 6);

	const double after = renderOne(lightField.TakeSnapshot(), occField.TakeSnapshot());
	std::printf("LIGHTING edited_scene before=%.4f after=%.4f samples=256\n", before, after);
	TASK_CHECK(before > 0.0 && after > 0.0);
	TASK_CHECK(std::fabs(after - before) > 1e-4);

	// the retained pre-edit snapshots still render the old scene exactly
	const double retained = renderOne(oldLightSnap, oldOccSnap);
	TASK_CHECK(Approx(retained, before, 1e-9));
	return 0;
}

} // namespace

// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------

const task_case task_cases[] =
{
	{"intersections", "saddle_roots", case_saddle_roots},
	{"intersections", "exhaustive_grids", case_exhaustive_grids},
	{"intersections", "shared_edges", case_shared_edges},
	{"intersections", "instances", case_instances},
	{"intersections", "hierarchy_invariants", case_hierarchy_invariants},

	{"updates", "invalid_then_retry", case_invalid_then_retry},
	{"updates", "empty_and_unchanged", case_empty_and_unchanged},
	{"updates", "single_vertex_work_257", case_single_vertex_work},
	{"updates", "snapshot_interleave", case_snapshot_interleave},

	{"lighting", "area_pdf", case_area_pdf},
	{"lighting", "direction_pdf", case_direction_pdf},
	{"lighting", "cpu_sampling_means", case_cpu_sampling_means},
	{"lighting", "edited_light_and_occluder", case_edited_light_and_occluder},
};

const size_t task_case_count = sizeof(task_cases)/sizeof(task_cases[0]);
