// Causal rendering regression tests.
//
// The production code works in single precision against analytic cubic
// roots and sweep-box BVHs.  These tests carry an independent double
// precision spacetime reference: the retarded intersection equation is
// re-derived and its roots found by grid bracketing + bisection, light
// densities are checked by finite-differencing barycentric area into solid
// angle, and transport is cross-checked against an independent cosine
// hemisphere estimator.

#include "task_support/check.h"

#include "src/causal.h"
#include "src/render.h"
#include "src/util.h"

#include <cmath>
#include <cstdio>
#include <random>
#include <vector>

namespace
{
	//-------------------------
	// independent double reference

	struct RefFace
	{
		DVec3 a0, b0, c0;
		DVec3 da, db, dc;
	};

	struct RefRoot
	{
		double tau;
		double s;
		double bu, bv, bw;
	};

	DVec3 RefAt(const DVec3& p0, const DVec3& d, double t) { return p0 + d*t; }

	// g(tau) = det(R(tau)-A, B-A, C-A), the same spacetime condition derived
	// independently and solved here by sign bracketing (no analytic cubic)
	double RefG(const RefFace& f, const DVec3& origin, const DVec3& dir,
	            double c, double tObs, double tau)
	{
		const DVec3 R = origin + dir*(c*(tObs - tau));
		const DVec3 A = RefAt(f.a0, f.da, tau);
		const DVec3 B = RefAt(f.b0, f.db, tau);
		const DVec3 C = RefAt(f.c0, f.dc, tau);
		return Dot(R - A, Cross(B - A, C - A));
	}

	void RefBary(const RefFace& f, double tau, const DVec3& p,
	             double& bu, double& bv, double& bw)
	{
		const DVec3 A = RefAt(f.a0, f.da, tau);
		const DVec3 B = RefAt(f.b0, f.db, tau);
		const DVec3 C = RefAt(f.c0, f.dc, tau);
		const DVec3 e1 = B - A;
		const DVec3 e2 = C - A;
		const DVec3 n = Cross(e1, e2);

		int axis = 0;
		const double ax = fabs(n.x), ay = fabs(n.y), az = fabs(n.z);
		if (ay > ax) axis = (ay > az) ? 1 : 2;
		else axis = (ax > az) ? 0 : 2;

		const int i1 = (axis + 1) % 3;
		const int i2 = (axis + 2) % 3;
		const DVec3 w = p - A;
		const double det = e1[i1]*e2[i2] - e1[i2]*e2[i1];
		bu = (w[i1]*e2[i2] - w[i2]*e2[i1]) / det;
		bv = (e1[i1]*w[i2] - e1[i2]*w[i1]) / det;
		bw = 1.0 - bu - bv;
	}

	// every valid retarded root of one moving face, nearest first
	std::vector<RefRoot> RefIntersect(const RefFace& f, const DVec3& origin, const DVec3& dir,
	                                  double c, double tObs)
	{
		std::vector<RefRoot> roots;

		const int kSteps = 40000;
		double prev = RefG(f, origin, dir, c, tObs, 0.0);

		for (int i = 1; i <= kSteps; ++i)
		{
			const double hi = tObs * double(i) / kSteps;
			const double lo = tObs * double(i - 1) / kSteps;
			const double cur = RefG(f, origin, dir, c, tObs, hi);

			if (prev == 0.0 || cur == 0.0 || prev*cur < 0.0)
			{
				double a = lo, b = hi;
				double ga = prev;
				if (ga == 0.0) ga = RefG(f, origin, dir, c, tObs, a + 1.0e-12);

				for (int it = 0; it < 80; ++it)
				{
					const double mid = 0.5*(a + b);
					const double gm = RefG(f, origin, dir, c, tObs, mid);
					if (gm == 0.0 || ga*gm <= 0.0) b = mid;
					else { a = mid; ga = gm; }
				}

				const double tau = 0.5*(a + b);
				const double s = c*(tObs - tau);
				if (s > 1.0e-9)
				{
					const DVec3 R = origin + dir*s;
					double bu, bv, bw;
					RefBary(f, tau, R, bu, bv, bw);

					if (bu >= -1.0e-7 && bv >= -1.0e-7 && bw >= -1.0e-7 &&
						bu <= 1.0 + 1.0e-7 && bv <= 1.0 + 1.0e-7 && bw <= 1.0 + 1.0e-7)
					{
						// reject exact tangency / zero area candidates
						const DVec3 A = RefAt(f.a0, f.da, tau);
						const DVec3 B = RefAt(f.b0, f.db, tau);
						const DVec3 C = RefAt(f.c0, f.dc, tau);
						if (LengthSq(Cross(B - A, C - A)) > 1.0e-14)
						{
							RefRoot r;
							r.tau = tau;
							r.s = s;
							r.bu = bu; r.bv = bv; r.bw = bw;

							bool duplicate = false;
							for (const RefRoot& q : roots)
								if (fabs(q.tau - tau) < 1.0e-6) duplicate = true;
							if (!duplicate)
								roots.push_back(r);
						}
					}
				}
			}

			prev = cur;
		}

		std::sort(roots.begin(), roots.end(),
		          [](const RefRoot& a, const RefRoot& b) { return a.s < b.s; });
		return roots;
	}

	RefFace FaceFromEntry(const CausalScene& cs, const CausalMeshEntry& e, int face)
	{
		RefFace f;
		f.a0 = DVec3(e.worldA0[face]); f.b0 = DVec3(e.worldB0[face]); f.c0 = DVec3(e.worldC0[face]);
		f.da = DVec3(e.worldDA[face]); f.db = DVec3(e.worldDB[face]); f.dc = DVec3(e.worldDC[face]);
		return f;
	}

	// independent retarded time for a moving point, by monotone bisection
	bool RefRetarded(const DVec3& y0, const DVec3& v, const DVec3& x,
	                 double c, double tObs, double& tau, double& r)
	{
		auto h = [&](double t) { return Length(y0 + v*t - x) - c*(tObs - t); };

		double a = 0.0, b = tObs*(1.0 - 1.0e-12);
		double ha = h(a), hb = h(b);
		if (ha*hb > 0.0)
			return false;

		for (int it = 0; it < 100; ++it)
		{
			const double m = 0.5*(a + b);
			const double hm = h(m);
			if (ha*hm <= 0.0) { b = m; hb = hm; }
			else { a = m; ha = hm; }
		}

		tau = 0.5*(a + b);
		r = c*(tObs - tau);
		return true;
	}

	//-------------------------
	// scene construction helpers

	Mesh* MakeTriMesh(const std::vector<Vec3>& p0, const std::vector<Vec3>& p1)
	{
		Mesh* m = new Mesh();
		m->positions = p0;
		if (!p1.empty())
			m->endPositions = p1;
		for (int i = 0; i < int(p0.size()); ++i)
			m->indices.push_back(i);
		m->CalculateNormals();
		m->RebuildBVH();
		m->RebuildCausalBVH();
		return m;
	}

	Primitive MakePrim(Mesh* mesh, const Vec3& color, const Vec3& emission = Vec3(0.0f),
	                   int lightSamples = 0, const Transform& xform = Transform())
	{
		Primitive p;
		p.type = eMesh;
		p.mesh = GeometryFromMesh(mesh);
		p.startTransform = p.endTransform = xform;
		p.material.color = color;
		p.material.emission = emission;
		p.lightSamples = lightSamples;
		return p;
	}

	bool Near(float a, double b, double tol) { return fabs(double(a) - b) <= tol; }
}

//=========================================================================
// intersection group
//=========================================================================

namespace
{
	int TestTranslation()
	{
		// static horizontal triangle 1 unit below a vertical ray: s=1, tau=t-s/c
		const float c = 4.0f;
		const float tObs = 0.8f;

		std::vector<Vec3> tri =
		{
			Vec3(-2, -1, -2), Vec3(2, -1, -2), Vec3(0, -1, 2)
		};
		Mesh* mesh = MakeTriMesh(tri, {});

		Scene scene;
		scene.sky.horizon = scene.sky.zenith = Vec3(0.0f);
		scene.meshes.push_back(mesh);
		scene.AddPrimitive(MakePrim(mesh, Vec3(0.5f)));
		scene.EnableCausal(c);
		scene.Build();
		TASK_CHECK(scene.causal && scene.causal->valid);

		CausalHit hit;
		const bool ok = CausalTrace(*scene.causal, Vec3(0, 0, 0), Vec3(0, -1, 0),
		                            tObs, 1.0e-6f, hit);
		TASK_CHECK(ok);

		const double expectS = 1.0;
		const double expectTau = tObs - expectS / c;
		TASK_CHECK(Near(hit.distance, expectS, 1.0e-5));
		TASK_CHECK(Near(hit.time, expectTau, 1.0e-5));
		TASK_CHECK(hit.face == 0);
		TASK_CHECK(hit.primitive == 0);

		// independent double reference
		RefFace rf = FaceFromEntry(*scene.causal, scene.causal->entries[0], 0);
		auto roots = RefIntersect(rf, DVec3(0, 0, 0), DVec3(0, -1, 0), c, tObs);
		TASK_CHECK(roots.size() == 1);
		TASK_CHECK(fabs(hit.distance - roots[0].s) <= 1.0e-5);
		TASK_CHECK(fabs(hit.time - roots[0].tau) <= 1.0e-5);
		TASK_CHECK(fabs(hit.u - roots[0].bu) <= 1.0e-5);
		TASK_CHECK(fabs(hit.v - roots[0].bv) <= 1.0e-5);

		// pure translation of the whole triangle (constant vertex velocity,
		// kept strictly below c/4)
		scene.Clear();
		mesh = MakeTriMesh(tri,
		{
			Vec3(-1.5f, -1, -2), Vec3(2.5f, -1, -2), Vec3(0.5f, -1, 2)
		});

		Scene scene2;
		scene2.sky.horizon = scene2.sky.zenith = Vec3(0.0f);
		scene2.meshes.push_back(mesh);
		scene2.AddPrimitive(MakePrim(mesh, Vec3(0.5f)));
		scene2.EnableCausal(c);
		scene2.Build();
		TASK_CHECK(scene2.causal->valid);

		CausalHit hit2;
		TASK_CHECK(CausalTrace(*scene2.causal, Vec3(0, 0, 0), Vec3(0, -1, 0),
		                       tObs, 1.0e-6f, hit2));
		TASK_CHECK(Near(hit2.distance, expectS, 1.0e-5));
		TASK_CHECK(Near(hit2.time, expectTau, 1.0e-5));

		RefFace rf2 = FaceFromEntry(*scene2.causal, scene2.causal->entries[0], 0);
		auto roots2 = RefIntersect(rf2, DVec3(0, 0, 0), DVec3(0, -1, 0), c, tObs);
		TASK_CHECK(roots2.size() == 1);
		TASK_CHECK(fabs(hit2.distance - roots2[0].s) <= 1.0e-5);
		TASK_CHECK(fabs(hit2.time - roots2[0].tau) <= 1.0e-5);

		scene2.Clear();
		return 0;
	}

	int TestDeformationMultiRoot()
	{
		// genuinely deforming triangle that the retarded ray crosses twice;
		// both roots are interior and non-degenerate, so the production query
		// must consider every candidate and return the nearest one rather
		// than iterating once from a snapshot solution
		const float c = 4.0f;
		const float tObs = 1.0f;

		std::vector<Vec3> p0 =
		{
			Vec3(3.5656f, 2.8752f, 5.8701f),
			Vec3(3.7823f, -2.3322f, -4.9300f),
			Vec3(2.4077f, 2.0687f, 4.7087f)
		};
		std::vector<Vec3> p1 =
		{
			Vec3(3.2433f, 2.0305f, 5.8897f),
			Vec3(3.7253f, -2.4001f, -5.3788f),
			Vec3(2.6313f, 2.5936f, 4.0157f)
		};
		Mesh* mesh = MakeTriMesh(p0, p1);

		Scene scene;
		scene.sky.horizon = scene.sky.zenith = Vec3(0.0f);
		scene.meshes.push_back(mesh);
		scene.AddPrimitive(MakePrim(mesh, Vec3(0.5f)));
		scene.EnableCausal(c);
		scene.Build();
		TASK_CHECK(scene.causal->valid);

		RefFace rf = FaceFromEntry(*scene.causal, scene.causal->entries[0], 0);
		auto roots = RefIntersect(rf, DVec3(0, 0, 0), DVec3(1, 0, 0), c, tObs);
		TASK_CHECK(roots.size() >= 2); // multiple retarded candidates

		CausalHit bvh, brute;
		TASK_CHECK(CausalTrace(*scene.causal, Vec3(0, 0, 0), Vec3(1, 0, 0),
		                       tObs, 1.0e-6f, bvh));
		TASK_CHECK(CausalTraceBruteForce(*scene.causal, Vec3(0, 0, 0), Vec3(1, 0, 0),
		                                 tObs, 1.0e-6f, brute));

		TASK_CHECK(fabs(bvh.distance - brute.distance) <= 1.0e-5*Max(1.0f, brute.distance));
		TASK_CHECK(fabs(bvh.time - brute.time) <= 1.0e-5);
		TASK_CHECK(bvh.face == brute.face);
		TASK_CHECK(fabs(bvh.u - brute.u) <= 1.0e-5);
		TASK_CHECK(fabs(bvh.v - brute.v) <= 1.0e-5);

		TASK_CHECK(fabs(bvh.distance - roots[0].s) <= 1.0e-5*Max(1.0, roots[0].s));
		TASK_CHECK(fabs(bvh.time - roots[0].tau) <= 1.0e-5);
		TASK_CHECK(fabs(bvh.u - roots[0].bu) <= 1.0e-5);
		TASK_CHECK(fabs(bvh.v - roots[0].bv) <= 1.0e-5);

		// light cone identity on the production hit
		const Vec3 y = bvh.point;
		TASK_CHECK(fabsf(bvh.time - (tObs - Length(y)/c)) <= 1.0e-5);

		scene.Clear();
		return 0;
	}

	int TestTimeCutoff()
	{
		// surface 2 units away, c=4: needs tau=t-0.5; t=0.4 cuts off to black
		const float c = 4.0f;

		std::vector<Vec3> tri =
		{
			Vec3(-2, -2, -2), Vec3(2, -2, -2), Vec3(0, -2, 2)
		};
		Mesh* mesh = MakeTriMesh(tri, {});

		Scene scene;
		scene.sky.horizon = scene.sky.zenith = Vec3(0.0f);
		scene.meshes.push_back(mesh);
		scene.AddPrimitive(MakePrim(mesh, Vec3(0.5f)));
		scene.EnableCausal(c);
		scene.Build();

		CausalHit cut;
		TASK_CHECK(!CausalTrace(*scene.causal, Vec3(0, 0, 0), Vec3(0, -1, 0),
		                        0.4f, 1.0e-6f, cut));

		// exactly t = s/c gives tau = 0 and still hits
		CausalHit boundary;
		TASK_CHECK(CausalTrace(*scene.causal, Vec3(0, 0, 0), Vec3(0, -1, 0),
		                       0.5f, 1.0e-6f, boundary));
		TASK_CHECK(Near(boundary.time, 0.0, 1.0e-5));
		TASK_CHECK(Near(boundary.distance, 2.0, 1.0e-5));

		scene.Clear();
		return 0;
	}

	int TestBVHMatchesBruteForce()
	{
		// multi-face deforming mesh, many rays: the sweep BVH must agree with
		// the exhaustive per-face traversal and the double reference
		const float c = 5.0f;
		const float tObs = 0.7f;

		std::vector<Vec3> p0, p1;
		std::vector<Vec3> grid;
		const int N = 5;
		for (int iz = 0; iz < N; ++iz)
			for (int ix = 0; ix < N; ++ix)
				grid.push_back(Vec3(-2.0f + ix, 0.0f, -2.0f + iz));

		for (int i = 0; i < N - 1; ++i)
		{
			for (int j = 0; j < N - 1; ++j)
			{
				const int q00 = i*N + j, q10 = i*N + j + 1;
				const int q11 = (i + 1)*N + j + 1, q01 = (i + 1)*N + j;
				int ids[4] = { q00, q10, q11, q01 };
				for (int k = 0; k < 4; ++k)
				{
					p0.push_back(grid[ids[k]]);
					Vec3 e = grid[ids[k]];
					e.y = -1.0f + 0.25f*k; // endpoint below, per-vertex shape change
					p1.push_back(e);
				}
			}
		}

		Mesh* mesh = MakeTriMesh(p0, p1);
		TASK_CHECK(mesh->causalBVH.nodes != NULL);
		TASK_CHECK(mesh->causalBVH.numNodes > 1);

		Scene scene;
		scene.sky.horizon = scene.sky.zenith = Vec3(0.0f);
		scene.meshes.push_back(mesh);
		scene.AddPrimitive(MakePrim(mesh, Vec3(0.5f)));
		scene.EnableCausal(c);
		scene.Build();
		TASK_CHECK(scene.causal->valid);

		int compared = 0;
		for (int a = 0; a < 8; ++a)
		{
			for (int b = 0; b < 8; ++b)
			{
				const Vec3 origin(-1.7f + 0.5f*a, 1.5f, -1.7f + 0.5f*b);
				const Vec3 dir = Normalize(Vec3(0.1f*a - 0.35f, -1.0f, 0.1f*b - 0.35f));

				CausalHit hBvh, hBrute;
				const bool okBvh = CausalTrace(*scene.causal, origin, dir, tObs, 1.0e-6f, hBvh);
				const bool okBrute = CausalTraceBruteForce(*scene.causal, origin, dir,
				                                            tObs, 1.0e-6f, hBrute);
				TASK_CHECK(okBvh == okBrute);
				if (!okBvh) continue;

				TASK_CHECK(hBvh.face == hBrute.face);
				TASK_CHECK(fabs(hBvh.distance - hBrute.distance) <= 1.0e-5*Max(1.0f, hBrute.distance));
				TASK_CHECK(fabs(hBvh.time - hBrute.time) <= 1.0e-5);
				TASK_CHECK(fabs(hBvh.u - hBrute.u) <= 1.0e-5);
				TASK_CHECK(fabs(hBvh.v - hBrute.v) <= 1.0e-5);

				RefFace rf = FaceFromEntry(*scene.causal, scene.causal->entries[0], hBvh.face);
				auto roots = RefIntersect(rf, DVec3(origin), DVec3(dir), c, tObs);
				TASK_CHECK(!roots.empty());
				TASK_CHECK(fabs(hBvh.distance - roots[0].s) <= 1.0e-5*Max(1.0, roots[0].s));
				++compared;
			}
		}
		TASK_CHECK(compared > 20);

		scene.Clear();
		return 0;
	}

	int TestSharedMeshInstances()
	{
		// one moving mesh instanced twice with different materials; face ids and
		// the primitive/material association must stay distinct
		const float c = 5.0f;
		const float tObs = 0.6f;

		std::vector<Vec3> p0 = { Vec3(-1, 1, -1), Vec3(1, 1, -1), Vec3(0, 1, 1) };
		std::vector<Vec3> p1 = { Vec3(-1, 0.5f, -1), Vec3(1, 0.5f, -1), Vec3(0, 0.5f, 1) };
		Mesh* mesh = MakeTriMesh(p0, p1);

		Scene scene;
		scene.sky.horizon = scene.sky.zenith = Vec3(0.0f);
		scene.meshes.push_back(mesh);

		Primitive left = MakePrim(mesh, Vec3(0.9f, 0.1f, 0.1f), Vec3(0.0f), 0,
		                          Transform(Vec3(-3, 0, 0)));
		Primitive right = MakePrim(mesh, Vec3(0.1f, 0.1f, 0.9f), Vec3(0.0f), 0,
		                           Transform(Vec3(3, 0, 0)));
		scene.AddPrimitive(left);
		scene.AddPrimitive(right);
		scene.EnableCausal(c);
		scene.Build();
		TASK_CHECK(scene.causal->valid);
		TASK_CHECK(scene.causal->entries.size() == 2);
		TASK_CHECK(scene.causal->entries[0].mesh == scene.causal->entries[1].mesh);

		CausalHit hl, hr;
		TASK_CHECK(CausalTrace(*scene.causal, Vec3(-3, 0, 0), Vec3(0, 1, 0),
		                       tObs, 1.0e-6f, hl));
		TASK_CHECK(CausalTrace(*scene.causal, Vec3(3, 0, 0), Vec3(0, 1, 0),
		                       tObs, 1.0e-6f, hr));
		TASK_CHECK(hl.primitive == 0 && hr.primitive == 1);
		TASK_CHECK(hl.face == 0 && hr.face == 0);
		TASK_CHECK(scene.primitives[0].material.color.x >
		           scene.primitives[1].material.color.x);

		// double reference against the world-space instance data
		RefFace rf = FaceFromEntry(*scene.causal, scene.causal->entries[1], 0);
		auto roots = RefIntersect(rf, DVec3(3, 0, 0), DVec3(0, 1, 0), c, tObs);
		TASK_CHECK(roots.size() == 1);
		TASK_CHECK(fabs(hr.distance - roots[0].s) <= 1.0e-5*Max(1.0, roots[0].s));
		TASK_CHECK(fabs(hr.time - roots[0].tau) <= 1.0e-5);

		scene.Clear();
		return 0;
	}
}

//=========================================================================
// density group
//=========================================================================

namespace
{
	// finite-difference the solid angle of a small barycentric patch and
	// divide its sampling probability mass by it: an independent density check
	float FiniteDifferenceDensity(const CausalScene& cs, const CausalMeshEntry& e,
	                              const Vec3& receiver, float tObs, int face,
	                              float u0, float v0, float h)
	{
		const float w0 = 1.0f - u0 - v0;
		const float h2 = h;
		struct Bary { float u, v; };
		Bary corners[4] =
		{
			{ u0,      v0 },
			{ u0 + h2, v0 },
			{ u0 + h2, v0 + h2 },
			{ u0,      v0 + h2 }
		};
		(void)w0;

		DVec3 dirs[4];
		for (int i = 0; i < 4; ++i)
		{
			const float bu = corners[i].u;
			const float bv = corners[i].v;
			const float bw = 1.0f - bu - bv;

			const DVec3 y0 = DVec3(e.worldA0[face])*bw + DVec3(e.worldB0[face])*bu +
			                 DVec3(e.worldC0[face])*bv;
			const DVec3 vel = DVec3(e.worldDA[face])*bw + DVec3(e.worldDB[face])*bu +
			                  DVec3(e.worldDC[face])*bv;

			double tau, r;
			if (!RefRetarded(y0, vel, DVec3(receiver), cs.lightSpeed, tObs, tau, r))
				return 0.0f;
			const DVec3 y = y0 + vel*tau;
			dirs[i] = (y - DVec3(receiver))*(1.0 / r);
		}

		// solid angle of the spherical quad as two triangles
		const double omega = fabs(Dot(Cross(dirs[1] - dirs[0], dirs[2] - dirs[0]), dirs[0]))*0.5
		                   + fabs(Dot(Cross(dirs[2] - dirs[0], dirs[3] - dirs[0]), dirs[0]))*0.5;

		// uniform barycentric sampling has density 2 over the triangle domain
		const double probability = e.faceProb[face] * 2.0 * h * h;
		return float(probability / omega);
	}

	int TestDensityFormula()
	{
		const float c = 4.0f;
		const float tObs = 0.9f;

		std::vector<Vec3> p0 = { Vec3(-1, 2, -1), Vec3(1, 2, -1), Vec3(0, 2, 1) };
		std::vector<Vec3> p1 = { Vec3(-0.8f, 2, -1), Vec3(1.2f, 2.1f, -1), Vec3(0.1f, 2, 1.1f) };
		Mesh* mesh = MakeTriMesh(p0, p1);

		Scene scene;
		scene.sky.horizon = scene.sky.zenith = Vec3(0.0f);
		scene.meshes.push_back(mesh);
		scene.AddPrimitive(MakePrim(mesh, Vec3(0.5f), Vec3(3.0f), 1));
		scene.EnableCausal(c);
		scene.Build();
		TASK_CHECK(scene.causal->valid);

		const CausalMeshEntry& e = scene.causal->entries[0];
		TASK_CHECK(Near(e.faceProb[0], 1.0, 1.0e-6));

		const Vec3 receiver(0.1f, 0.0f, 0.2f);

		for (int k = 0; k < 5; ++k)
		{
			const float u0 = 0.2f + 0.1f*k;
			const float v0 = 0.15f + 0.05f*k;
			const float h = 1.0e-3f;

			const float analytic = CausalLightPdf(*scene.causal, 0, receiver, tObs,
			                                      0, u0, v0, 1.0f - u0 - v0);
			const float numeric = FiniteDifferenceDensity(*scene.causal, e, receiver,
			                                               tObs, 0, u0, v0, h);
			TASK_CHECK(analytic > 0.0f && numeric > 0.0f);
			TASK_CHECK(fabsf(analytic - numeric) / numeric < 0.03f);
		}

		// the sampler must report the same density and satisfy the light cone
		Random rand(77);
		int validSamples = 0;
		for (int s = 0; s < 32; ++s)
		{
			CausalLightSample ls = CausalSampleLight(*scene.causal, 0, receiver, tObs, rand);
			TASK_CHECK(ls.valid);
			const float pdf = CausalLightPdf(*scene.causal, 0, receiver, tObs,
			                                 ls.face, ls.bu, ls.bv, ls.bw);
			TASK_CHECK(fabsf(pdf - ls.pdf) <= 1.0e-5f*Max(1.0f, ls.pdf));
			TASK_CHECK(fabsf(ls.te - (tObs - ls.r / c)) <= 1.0e-5f);
			TASK_CHECK(fabsf(Length(ls.point - receiver) - ls.r) <= 1.0e-4f);
			++validSamples;
		}
		TASK_CHECK(validSamples == 32);

		scene.Clear();
		return 0;
	}

	int TestDensityStaticLimit()
	{
		// zero velocity: the moving-light density must collapse to the classic
		// r^2 / (area * |cos|) solid angle area measure
		const float c = 4.0f;
		const float tObs = 1.0f;

		const Vec3 A(-1, 2, -1), B(1, 2, -1), C(0, 2, 1);
		std::vector<Vec3> p0 = { A, B, C };
		Mesh* mesh = MakeTriMesh(p0, {});

		Scene scene;
		scene.sky.horizon = scene.sky.zenith = Vec3(0.0f);
		scene.meshes.push_back(mesh);
		scene.AddPrimitive(MakePrim(mesh, Vec3(0.5f), Vec3(3.0f), 1));
		scene.EnableCausal(c);
		scene.Build();

		const Vec3 receiver(0, 0, 0);
		const float bu = 0.3f, bv = 0.3f, bw = 0.4f;
		const float pdf = CausalLightPdf(*scene.causal, 0, receiver, tObs, 0, bu, bv, bw);

		const Vec3 e1 = B - A, e2 = C - A;
		const float area = 0.5f*Length(Cross(e1, e2));
		const Vec3 point = bw*A + bu*B + bv*C;
		const float r = Length(point - receiver);
		const Vec3 u = (point - receiver) / r;
		const Vec3 n = SafeNormalize(Cross(e1, e2));
		const float expected = r*r / (area*fabsf(Dot(n, u)));

		TASK_CHECK(fabsf(pdf - expected) <= 1.0e-5f*expected);

		const float numeric = FiniteDifferenceDensity(*scene.causal, scene.causal->entries[0],
		                                               receiver, tObs, 0, bu, bv, 1.0e-3f);
		TASK_CHECK(fabsf(numeric - expected) / expected < 0.03f);

		scene.Clear();
		return 0;
	}

	int TestDensityOutOfDomain()
	{
		const float c = 4.0f;
		const float tObs = 0.2f;

		std::vector<Vec3> p0 = { Vec3(-1, 5, -1), Vec3(1, 5, -1), Vec3(0, 5, 1) };
		Mesh* mesh = MakeTriMesh(p0, {});

		Scene scene;
		scene.sky.horizon = scene.sky.zenith = Vec3(0.0f);
		scene.meshes.push_back(mesh);
		scene.AddPrimitive(MakePrim(mesh, Vec3(0.5f), Vec3(3.0f), 1));
		scene.EnableCausal(c);
		scene.Build();

		// light is ~5 units away but the observation window only reaches 0.8
		Random rand(5);
		CausalLightSample ls = CausalSampleLight(*scene.causal, 0, Vec3(0, 0, 0), tObs, rand);
		TASK_CHECK(!ls.valid);
		TASK_CHECK(CausalLightPdf(*scene.causal, 0, Vec3(0, 0, 0), tObs,
		                          0, 0.3f, 0.3f, 0.4f) == 0.0f);

		scene.Clear();
		return 0;
	}
}

//=========================================================================
// transport group
//=========================================================================

namespace
{
	struct TransportScene
	{
		Scene scene;
		Mesh* light;
		Mesh* receiver;
		Mesh* occluder;
	};

	int BuildTransport(TransportScene& ts, bool movingOccluder, float c)
	{
		ts.scene.sky.horizon = ts.scene.sky.zenith = Vec3(0.0f);

		// overhead emissive triangle
		std::vector<Vec3> lp = { Vec3(-1, 3, -1), Vec3(1, 3, -1), Vec3(0, 3, 1) };
		ts.light = MakeTriMesh(lp, {});

		// large ground receiver
		ts.receiver = new Mesh();
		ts.receiver->positions =
		{
			Vec3(-6, -1, -6), Vec3(6, -1, -6), Vec3(6, -1, 6), Vec3(-6, -1, 6)
		};
		ts.receiver->indices = { 0, 1, 2, 2, 3, 0 };
		ts.receiver->CalculateNormals();
		ts.receiver->RebuildBVH();

		// thin wall between the receiver point and the light
		std::vector<Vec3> w0 =
		{
			Vec3(-0.5f, -1, 0.5f), Vec3(0.5f, -1, 0.5f),
			Vec3(0.5f, 2, 0.5f), Vec3(-0.5f, 2, 0.5f)
		};
		std::vector<Vec3> w1 = w0;
		if (movingOccluder)
		{
			// slides sideways out of the shadow line over the time domain;
			// per-vertex speed 0.95 stays strictly below c/4
			for (Vec3& v : w1) v.x += 0.95f;
		}
		ts.occluder = new Mesh();
		ts.occluder->positions = w0;
		ts.occluder->endPositions = w1;
		ts.occluder->indices = { 0, 1, 2, 2, 3, 0 };
		ts.occluder->CalculateNormals();
		ts.occluder->RebuildBVH();

		ts.scene.meshes.push_back(ts.light);
		ts.scene.meshes.push_back(ts.receiver);
		ts.scene.meshes.push_back(ts.occluder);

		ts.scene.AddPrimitive(MakePrim(ts.light, Vec3(0.0f), Vec3(4.0f), 4));
		ts.scene.AddPrimitive(MakePrim(ts.receiver, Vec3(0.7f)));
		ts.scene.AddPrimitive(MakePrim(ts.occluder, Vec3(0.2f)));

		ts.scene.EnableCausal(c);
		ts.scene.Build();
		TASK_CHECK(ts.scene.causal && ts.scene.causal->valid);
		return 0;
	}

	// estimator A: the production direct-light sampling routine
	Vec3 EstimateLightSampling(TransportScene& ts, const Vec3& p, const Vec3& n,
	                           float tObs, int samples, std::mt19937& rng)
	{
		Random rand((int)rng());
		Vec3 sum(0.0f);

		for (int s = 0; s < samples; ++s)
		{
			CausalLightSample ls = CausalSampleLight(*ts.scene.causal, 0, p, tObs, rand);
			if (!ls.valid)
				continue;

			CausalHit shadow;
			if (!CausalTraceBruteForce(*ts.scene.causal, p, ls.u, tObs, 1.0e-6f, shadow))
				continue;
			if (shadow.primitive != 0)
				continue;

			const float cosTheta = Abs(Dot(n, ls.u));
			const Vec3 f = ts.scene.primitives[1].material.color*kInvPi;
			sum += f*ts.scene.primitives[0].material.emission*cosTheta / ls.pdf;
		}

		return sum*(1.0f / samples);
	}

	// estimator B: independent cosine hemisphere integration with its own RNG
	Vec3 EstimateDirection(TransportScene& ts, const Vec3& p, const Vec3& n,
	                       float tObs, int samples, std::mt19937& rng)
	{
		std::uniform_real_distribution<float> uni(0.0f, 1.0f);
		Vec3 tx, ty;
		BasisFromVector(n, &tx, &ty);

		Vec3 sum(0.0f);
		int hitLight = 0;

		for (int s = 0; s < samples; ++s)
		{
			const float r1 = uni(rng), r2 = uni(rng);
			const float r = sqrtf(r1);
			const float phi = k2Pi*r2;
			const Vec3 local(r*cosf(phi), r*sinf(phi), sqrtf(Max(0.0f, 1.0f - r1)));
			const Vec3 wi = Normalize(tx*local.x + ty*local.y + n*local.z);

			CausalHit hit;
			if (!CausalTraceBruteForce(*ts.scene.causal, p, wi, tObs, 1.0e-6f, hit))
				continue;

			if (hit.primitive == 0)
			{
				sum += ts.scene.primitives[1].material.color *
				       ts.scene.primitives[0].material.emission;
				++hitLight;
			}
		}

		std::printf("  direction estimator light hits=%d/%d\n", hitLight, samples);
		return sum*(1.0f / samples);
	}

	int RunTransportCase(bool moving, float tObs, float c, const char* name)
	{
		TransportScene ts;
		if (BuildTransport(ts, moving, c))
			return 1;

		const Vec3 p(0, -0.99f, 0.0f);
		const Vec3 n(0, 1, 0);
		const int samples = 4000;

		std::mt19937 rngA(1234), rngB(5678);
		const Vec3 ea = EstimateLightSampling(ts, p, n, tObs, samples, rngA);
		const Vec3 eb = EstimateDirection(ts, p, n, tObs, samples, rngB);

		const float err = Length(ea - eb);
		const float scale = Max(1.0e-3f, 0.5f*(Length(ea) + Length(eb)));
		std::printf("  %s: light-sample=(%.4f,%.4f,%.4f) direction=(%.4f,%.4f,%.4f) "
		            "samples=%d relErr=%.4f\n",
		            name, ea.x, ea.y, ea.z, eb.x, eb.y, eb.z, samples, err / scale);

		TASK_CHECK(err / scale < 0.15f);

		// full path tracer: camera-direct emission, one bounce, and black void
		Random rand(99);
		const Vec3 direct = CausalPathTrace(ts.scene, Vec3(0, 1.5f, 1.5f),
		                                    Normalize(Vec3(0, 3, 0) - Vec3(0, 1.5f, 1.5f)),
		                                    tObs, 4, rand);
		TASK_CHECK(Length(direct - Vec3(4.0f)) <= 1.0e-4f);

		const Vec3 voidRay = CausalPathTrace(ts.scene, Vec3(0, 0, 0),
		                                     Normalize(Vec3(1, -0.05f, -1)), tObs, 4, rand);
		TASK_CHECK(voidRay.x == 0.0f && voidRay.y == 0.0f && voidRay.z == 0.0f);

		const Vec3 bounce = CausalPathTrace(ts.scene, p + Vec3(0, 0.02f, 0), n,
		                                    tObs, 4, rand);
		TASK_CHECK(isfinite(bounce.x) && isfinite(bounce.y) && isfinite(bounce.z));

		ts.scene.Clear();
		return 0;
	}

	int TestTransportMovingOcclusion()
	{
		// at this observation time the retarded sight line meets the wall
		// while it is still crossing; both estimators must agree on the shadow
		if (RunTransportCase(true, 0.9f, 8.0f, "moving occlusion"))
			return 1;

		// the moving wall must change the answer relative to a static wall:
		// along a ray crossing the wall plane, retarded visibility flips
		TransportScene moving, stationary;
		if (BuildTransport(moving, true, 8.0f) || BuildTransport(stationary, false, 8.0f))
			return 1;

		const Vec3 p(0, -0.97f, 0);
		const Vec3 dir = Normalize(Vec3(0, 3, 1) - p);
		Random ra(7), rb(7);
		Vec3 ma(0.0f), sa(0.0f);
		const int N = 256;
		for (int i = 0; i < N; ++i)
		{
			ma += CausalPathTrace(moving.scene, p, dir, 0.9f, 3, ra);
			sa += CausalPathTrace(stationary.scene, p, dir, 0.9f, 3, rb);
		}
		ma *= 1.0f / N;
		sa *= 1.0f / N;
		std::printf("  moving vs static wall contrast: moving=%.4f static=%.4f\n", ma.x, sa.x);
		TASK_CHECK(fabsf(ma.x - sa.x) > 0.02f);

		moving.scene.Clear();
		stationary.scene.Clear();
		return 0;
	}

	int TestTransportStaticLimit()
	{
		// zero velocity limit: same machinery, everything stationary
		return RunTransportCase(false, 0.9f, 8.0f, "static limit");
	}

	int TestValidationRejects()
	{
		const float c = 4.0f;

		// sphere geometry is rejected in causal mode
		Scene sphereScene;
		sphereScene.sky.horizon = sphereScene.sky.zenith = Vec3(0.0f);
		Primitive sp;
		sp.type = eSphere;
		sp.sphere.radius = 1.0f;
		sphereScene.AddPrimitive(sp);
		sphereScene.EnableCausal(c);
		sphereScene.Build();
		TASK_CHECK(sphereScene.causal && !sphereScene.causal->valid);
		sphereScene.Clear();

		// superluminal vertex motion is rejected
		std::vector<Vec3> p0 = { Vec3(-1, 1, -1), Vec3(1, 1, -1), Vec3(0, 1, 1) };
		std::vector<Vec3> p1 = { Vec3(-1, 3, -1), Vec3(1, 3, -1), Vec3(0, 3, 1) };
		Mesh* mesh = MakeTriMesh(p0, p1);

		Scene fast;
		fast.sky.horizon = fast.sky.zenith = Vec3(0.0f);
		fast.meshes.push_back(mesh);
		fast.AddPrimitive(MakePrim(mesh, Vec3(0.5f)));
		fast.EnableCausal(c);
		fast.Build();
		TASK_CHECK(fast.causal && !fast.causal->valid);
		fast.Clear();

		// default scene stays on the normal non-causal path
		Scene normal;
		TASK_CHECK(!normal.CausalEnabled());
		TASK_CHECK(normal.causal == NULL);

		return 0;
	}
}

//=========================================================================
// registration
//=========================================================================

const struct task_case task_cases[] =
{
	{ "intersections", "translation",        TestTranslation },
	{ "intersections", "deformation_multiroot", TestDeformationMultiRoot },
	{ "intersections", "time_cutoff",        TestTimeCutoff },
	{ "intersections", "bvh_vs_bruteforce",  TestBVHMatchesBruteForce },
	{ "intersections", "shared_instances",   TestSharedMeshInstances },

	{ "density", "formula_fd",         TestDensityFormula },
	{ "density", "static_limit",       TestDensityStaticLimit },
	{ "density", "out_of_domain",      TestDensityOutOfDomain },

	{ "transport", "moving_occlusion", TestTransportMovingOcclusion },
	{ "transport", "static_limit",     TestTransportStaticLimit },
	{ "transport", "validation",       TestValidationRejects },
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
