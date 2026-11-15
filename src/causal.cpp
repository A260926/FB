#include "causal.h"
#include "util.h"

#include <cmath>

namespace
{
	CausalScene* CreateInvalidCausalScene(float lightSpeed)
	{
		CausalScene* cs = new CausalScene();
		cs->enabled = true;
		cs->valid = false;
		cs->lightSpeed = lightSpeed;
		return cs;
	}

	// install the causal implementation into the scene/renderer hooks so the
	// normal renderer translation unit does not have to link this one
	struct HookInstaller
	{
		HookInstaller()
		{
			causal_detail::Hooks hooks;
			hooks.createInvalid = &CreateInvalidCausalScene;
			hooks.build = &BuildCausalScene;
			hooks.destroy = [](CausalScene* cs) { delete cs; };
			hooks.pathTrace = &CausalPathTrace;
			causal_detail::RegisterCausalHooks(hooks);
		}
	};

	static HookInstaller gHookInstaller;
}

namespace
{
	// self-intersection offset, kept small enough to stay inside the 1e-5
	// query error target; it is never counted as propagation length
	const float kRayEpsilon = 1.0e-6f;

	// real roots of a0 + a1 x + a2 x^2 + a3 x^3 = 0, appended without sorting
	void SolveCubic(double a0, double a1, double a2, double a3, std::vector<double>& roots)
	{
		if (fabs(a3) <= 1.0e-14 * (1.0 + fabs(a2) + fabs(a1) + fabs(a0)))
		{
			if (fabs(a2) <= 1.0e-14 * (1.0 + fabs(a1) + fabs(a0)))
			{
				if (fabs(a1) > 1.0e-14 * (1.0 + fabs(a0)))
					roots.push_back(-a0 / a1);
				return;
			}

			double disc = a1*a1 - 4.0*a2*a0;
			if (disc < 0.0)
				return;

			double q = -0.5*(a1 + (a1 < 0.0 ? -1.0 : 1.0)*sqrt(disc));
			roots.push_back(q / a2);
			if (fabs(q) > 0.0)
				roots.push_back(a0 / q);
			return;
		}

		const double inv = 1.0 / a3;
		const double b = a2*inv, c = a1*inv, d = a0*inv;

		const double bover3 = b / 3.0;
		const double p = c - b*b / 3.0;
		const double qq = d - b*c / 3.0 + 2.0*b*b*b / 27.0;
		const double disc = (qq*qq / 4.0) + (p*p*p / 27.0);

		if (disc > 0.0)
		{
			const double sq = sqrt(disc);
			const double tmp1 = -qq / 2.0 + sq;
			const double tmp2 = -qq / 2.0 - sq;
			const double u = (tmp1 >= 0.0 ? 1.0 : -1.0)*pow(fabs(tmp1), 1.0 / 3.0);
			const double v = (tmp2 >= 0.0 ? 1.0 : -1.0)*pow(fabs(tmp2), 1.0 / 3.0);
			roots.push_back(u + v - bover3);
		}
		else if (disc == 0.0)
		{
			const double tmp = (qq >= 0.0 ? -1.0 : 1.0)*pow(fabs(qq / 2.0), 1.0 / 3.0);
			roots.push_back(2.0*tmp - bover3);
			roots.push_back(-tmp - bover3);
		}
		else
		{
			const double r = sqrt(fabs((-p / 3.0)*(-p / 3.0)*(-p / 3.0)));
			const double phi = acos(Clamp(-qq / (2.0*r), -1.0, 1.0));
			const double t = 2.0*sqrt(-p / 3.0);
			roots.push_back(t*cos(phi / 3.0) - bover3);
			roots.push_back(t*cos((phi + 2.0*kPi) / 3.0) - bover3);
			roots.push_back(t*cos((phi + 4.0*kPi) / 3.0) - bover3);
		}
	}

	// segment R(tau) = r0 - dc*tau, tau in [0, tMax] against a sweep box
	bool SegmentSweepBox(const DVec3& r0, const DVec3& dc, double tMax, const Bounds& box)
	{
		double lo = 0.0, hi = tMax;

		for (int axis = 0; axis < 3; ++axis)
		{
			const double lower = box.lower[axis];
			const double upper = box.upper[axis];
			const double o = r0[axis];
			const double v = -dc[axis]; // R(tau) component = o + v*tau

			if (fabs(v) < 1.0e-15)
			{
				if (o < lower || o > upper)
					return false;
			}
			else
			{
				double t1 = (lower - o) / v;
				double t2 = (upper - o) / v;
				if (t1 > t2) { double tmp = t1; t1 = t2; t2 = tmp; }

				lo = Max(lo, t1);
				hi = Min(hi, t2);

				if (lo > hi)
					return false;
			}
		}

		return hi >= 0.0 && lo <= tMax;
	}

	struct MovingFace
	{
		DVec3 a0, b0, c0;
		DVec3 da, db, dc;
	};

	MovingFace GetWorldFace(const CausalMeshEntry& entry, int localFace)
	{
		MovingFace f;
		f.a0 = DVec3(entry.worldA0[localFace]);
		f.b0 = DVec3(entry.worldB0[localFace]);
		f.c0 = DVec3(entry.worldC0[localFace]);
		f.da = DVec3(entry.worldDA[localFace]);
		f.db = DVec3(entry.worldDB[localFace]);
		f.dc = DVec3(entry.worldDC[localFace]);
		return f;
	}

	struct Candidate
	{
		double tau;
		double s;
		double bu, bv, bw;
		DVec3 point;
		DVec3 normal;
		int face;
	};

	// analytic spacetime intersection of one moving triangle against the
	// retarded ray segment.  Every valid root is appended; a single triangle
	// can legitimately produce more than one retarded candidate.
	void IntersectFace(const MovingFace& f, int face, const DVec3& rayOrigin, const DVec3& rayDir,
	                   double c, double tObs, double minS, std::vector<Candidate>& out)
	{
		// ray point at emission time tau: R(tau) = rayOrigin + rayDir*c*(tObs-tau)
		const DVec3 r0 = rayOrigin + rayDir*(c*tObs);
		const DVec3 dc = rayDir*c;

		const DVec3 e10 = f.b0 - f.a0;
		const DVec3 e20 = f.c0 - f.a0;
		const DVec3 de1 = f.db - f.da;
		const DVec3 de2 = f.dc - f.da;

		const DVec3 w0 = r0 - f.a0;
		const DVec3 w1 = -(dc + f.da);

		const DVec3 n0 = Cross(e10, e20);
		const DVec3 n1 = Cross(de1, e20) + Cross(e10, de2);
		const DVec3 n2 = Cross(de1, de2);

		// F(tau) = dot(w0+w1*tau, n0+n1*tau+n2*tau^2)
		const double f0 = Dot(w0, n0);
		const double f1 = Dot(w0, n1) + Dot(w1, n0);
		const double f2 = Dot(w0, n2) + Dot(w1, n1);
		const double f3 = Dot(w1, n2);

		std::vector<double> roots;
		SolveCubic(f0, f1, f2, f3, roots);

		for (double tau : roots)
		{
			if (!(tau >= 0.0 && tau <= tObs))
				continue;

			const double s = c*(tObs - tau);
			if (!(s > minS))
				continue;

			const DVec3 A = f.a0 + f.da*tau;
			const DVec3 B = f.b0 + f.db*tau;
			const DVec3 C = f.c0 + f.dc*tau;
			const DVec3 e1 = B - A;
			const DVec3 e2 = C - A;
			const DVec3 n = Cross(e1, e2);

			const double edgeScale = Max(1.0, LengthSq(e1) + LengthSq(e2));
			if (LengthSq(n) < 1.0e-18*edgeScale*edgeScale)
				continue; // zero area / exact tangency at this time

			// barycentric coordinates from the dominant plane axis
			int axis = 0;
			const double ax = fabs(n.x), ay = fabs(n.y), az = fabs(n.z);
			if (ay > ax) { axis = (ay > az) ? 1 : 2; }
			else { axis = (ax > az) ? 0 : 2; }

			const int i1 = (axis + 1) % 3;
			const int i2 = (axis + 2) % 3;

			const DVec3 wv = (r0 - dc*tau) - A;
			const double det = e1[i1]*e2[i2] - e1[i2]*e2[i1];
			if (fabs(det) < 1.0e-15*edgeScale)
				continue;

			const double bu = (wv[i1]*e2[i2] - wv[i2]*e2[i1]) / det;
			const double bv = (e1[i1]*wv[i2] - e1[i2]*wv[i1]) / det;
			const double bw = 1.0 - bu - bv;

			const double tol = 1.0e-7;
			if (bu < -tol || bv < -tol || bw < -tol ||
				bu > 1.0 + tol || bv > 1.0 + tol || bw > 1.0 + tol)
				continue;

			Candidate cand;
			cand.tau = tau;
			cand.s = s;
			cand.bu = Clamp(bu, 0.0, 1.0);
			cand.bv = Clamp(bv, 0.0, 1.0);
			cand.bw = Clamp(bw, 0.0, 1.0);
			cand.normal = n;
			cand.face = face;
			cand.point = A*cand.bw + B*cand.bu + C*cand.bv;
			out.push_back(cand);
		}
	}

	// nearest candidate over every face of one mesh entry, using the shared
	// mesh motion-domain BVH and sweep-box pruning in static mesh space
	void QueryEntry(const CausalScene& scene, const CausalMeshEntry& entry,
	                const DVec3& origin, const DVec3& dir, double c, double tObs,
	                double minS, Candidate& best, bool bruteForce)
	{
		auto testFace = [&](int face)
		{
			MovingFace f = GetWorldFace(entry, face);
			std::vector<Candidate> cands;
			IntersectFace(f, face, origin, dir, c, tObs, minS, cands);
			for (const Candidate& cand : cands)
			{
				if (cand.s < best.s)
					best = cand;
			}
		};

		if (bruteForce)
		{
			for (int face = 0; face < entry.triCount; ++face)
				testFace(face);
			return;
		}

		const BVHNode* nodes = entry.mesh->causalBVH.nodes;
		if (!nodes)
		{
			for (int face = 0; face < entry.triCount; ++face)
				testFace(face);
			return;
		}

		// transform the retarded segment into static mesh local space
		const DVec3 localOrigin(InverseTransformPoint(entry.xform, ToVec3(origin)));
		const DVec3 localDir(InverseTransformVector(entry.xform, ToVec3(dir)));
		const DVec3 r0 = localOrigin + localDir*(c*tObs);
		const DVec3 dc = localDir*c;

		int stack[64];
		stack[0] = 0;
		int count = 1;

		while (count)
		{
			const BVHNode node = nodes[stack[--count]];

			if (node.leaf)
			{
				testFace(node.leftIndex);
			}
			else
			{
				const BVHNode& left = nodes[node.leftIndex];
				const BVHNode& right = nodes[node.rightIndex];

				if (SegmentSweepBox(r0, dc, tObs, left.bounds))
					stack[count++] = node.leftIndex;
				if (SegmentSweepBox(r0, dc, tObs, right.bounds))
					stack[count++] = node.rightIndex;
			}
		}
	}

	bool TraceInternal(const CausalScene& scene, const Vec3& originIn, const Vec3& dirIn,
	                   float observationTime, float rayEpsilon, CausalHit& outHit, bool bruteForce)
	{
		outHit = CausalHit();

		if (!scene.valid || observationTime <= 0.0f)
			return false;

		const double c = scene.lightSpeed;
		const DVec3 dirReal = DVec3(dirIn);
		const DVec3 dir = dirReal*(1.0 / Length(dirReal));
		// offset to avoid self-intersection; never counted in propagation length
		const DVec3 originOffset = DVec3(originIn) + dir*double(rayEpsilon);
		const double tObs = observationTime;
		const double minS = 1.0e-9;

		const DVec3 r0 = originOffset + dir*(c*tObs);
		const DVec3 dc = dir*c;

		Candidate best;
		best.s = 1.0e300;
		best.face = -1;
		int bestEntry = -1;

		const BVHNode* sceneNodes = scene.bvh.nodes;
		if (!sceneNodes)
			return false;

		int stack[64];
		stack[0] = 0;
		int count = 1;

		while (count)
		{
			const BVHNode node = sceneNodes[stack[--count]];

			if (node.leaf)
			{
				const int entryIndex = node.leftIndex;
				const CausalMeshEntry& entry = scene.entries[entryIndex];

				const double before = best.s;
				QueryEntry(scene, entry, originOffset, dir, c, tObs, minS, best, bruteForce);

				if (best.s < before)
					bestEntry = entryIndex;
			}
			else
			{
				const BVHNode& left = sceneNodes[node.leftIndex];
				const BVHNode& right = sceneNodes[node.rightIndex];

				if (SegmentSweepBox(r0, dc, tObs, left.bounds))
					stack[count++] = node.leftIndex;
				if (SegmentSweepBox(r0, dc, tObs, right.bounds))
					stack[count++] = node.rightIndex;
			}
		}

		if (bestEntry < 0 || best.face < 0)
			return false;

		const CausalMeshEntry& entry = scene.entries[bestEntry];

		outHit.hit = true;
		// best.s is measured from the offset origin; restore the true
		// propagation length between the real vertices and the matching
		// emission time.  The self-intersection offset never counts.
		const float realDistance = float(best.s + double(rayEpsilon));
		outHit.distance = realDistance;
		outHit.time = float(observationTime - double(realDistance)/c);
		outHit.u = float(best.bu);
		outHit.v = float(best.bv);
		outHit.w = float(best.bw);
		outHit.face = best.face;
		outHit.primitive = entry.primitive;
		outHit.point = ToVec3(best.point);

		Vec3 n = ToVec3(best.normal);
		if (Dot(n, dirIn) > 0.0f)
			n = n * -1.0f;
		outHit.normal = SafeNormalize(n);

		return true;
	}

	// retarded time solve for a linearly moving point y(tau) = y0 + v*tau:
	// |y(tau) - x| = c*(tObs - tau), tau in [0, tObs)
	bool SolveRetardedPoint(const DVec3& y0, const DVec3& v, const DVec3& x,
	                        double c, double tObs, double& tauOut, double& rOut)
	{
		const DVec3 d0 = y0 - x;

		const double A = LengthSq(v) - c*c;
		const double B = 2.0*(Dot(v, d0) + c*c*tObs);
		const double C = LengthSq(d0) - c*c*tObs*tObs;

		std::vector<double> roots;
		SolveCubic(C, B, A, 0.0, roots);

		bool found = false;
		double bestTau = -1.0;

		for (double tau : roots)
		{
			if (!(tau >= 0.0 && tau < tObs))
				continue;

			const double r = c*(tObs - tau);
			const DVec3 y = y0 + v*tau;
			if (fabs(Length(y - x) - r) > 1.0e-6*Max(1.0, r))
				continue;

			// subluminal motion makes the retarded time unique; keep the latest
			if (tau > bestTau)
			{
				bestTau = tau;
				found = true;
			}
		}

		if (!found)
			return false;

		tauOut = bestTau;
		rOut = c*(tObs - bestTau);
		return true;
	}

	const CausalMeshEntry* FindEntry(const CausalScene& scene, int primitive)
	{
		for (const CausalMeshEntry& entry : scene.entries)
		{
			if (entry.primitive == primitive)
				return &entry;
		}
		return NULL;
	}

	// world-space endpoints, barycentric velocity and time-te edges of a face
	void LightSampleGeometry(const CausalScene& scene, const CausalMeshEntry& entry, int face,
	                         float bu, float bv, float bw, double te,
	                         DVec3& y0, DVec3& vel, DVec3& e1, DVec3& e2)
	{
		y0 = DVec3(entry.worldA0[face])*bw + DVec3(entry.worldB0[face])*bu +
		     DVec3(entry.worldC0[face])*bv;
		vel = DVec3(entry.worldDA[face])*bw + DVec3(entry.worldDB[face])*bu +
		      DVec3(entry.worldDC[face])*bv;

		const DVec3 a0 = DVec3(entry.worldA0[face]);
		const DVec3 b0 = DVec3(entry.worldB0[face]);
		const DVec3 c0 = DVec3(entry.worldC0[face]);
		const DVec3 da = DVec3(entry.worldDA[face]);
		const DVec3 db = DVec3(entry.worldDB[face]);
		const DVec3 dc = DVec3(entry.worldDC[face]);

		e1 = (b0 - a0) + (db - da)*te;
		e2 = (c0 - a0) + (dc - da)*te;
		(void)scene;
	}

	float LightDensity(const CausalScene& scene, const CausalMeshEntry& entry, int face,
	                   const DVec3& receiver, double observationTime,
	                   float bu, float bv, float bw)
	{
		DVec3 y0, vel, e10, e20;
		LightSampleGeometry(scene, entry, face, bu, bv, bw, 0.0, y0, vel, e10, e20);

		double tau, r;
		if (!SolveRetardedPoint(y0, vel, receiver, scene.lightSpeed, observationTime, tau, r))
			return 0.0f;

		const DVec3 y = y0 + vel*tau;
		const DVec3 u = (y - receiver)*(1.0 / r);

		DVec3 e1, e2;
		LightSampleGeometry(scene, entry, face, bu, bv, bw, tau, y0, vel, e1, e2);

		const DVec3 cross = Cross(e1, e2);
		const DVec3 rhs = u + vel*(1.0 / scene.lightSpeed);
		const double denom = fabs(Dot(cross, rhs));
		if (denom < 1.0e-12)
			return 0.0f;

		const double density = 2.0*r*r*(1.0 + Dot(u, vel) / scene.lightSpeed) / denom;
		if (!(density > 0.0) || !isfinite(density))
			return 0.0f;

		return float(density * entry.faceProb[face]);
	}
}

CausalScene::~CausalScene()
{
	delete[] bvh.nodes;
}

void CausalScene::Clear()
{
	tris.clear();
	entries.clear();
	delete[] bvh.nodes;
	bvh.nodes = NULL;
	bvh.numNodes = 0;
	enabled = false;
	valid = false;
	lightSpeed = 0.0f;
}

bool CausalTrace(const CausalScene& scene, const Vec3& origin, const Vec3& dir,
                 float observationTime, float rayEpsilon, CausalHit& outHit)
{
	return TraceInternal(scene, origin, dir, observationTime, rayEpsilon, outHit, false);
}

bool CausalTraceBruteForce(const CausalScene& scene, const Vec3& origin, const Vec3& dir,
                           float observationTime, float rayEpsilon, CausalHit& outHit)
{
	return TraceInternal(scene, origin, dir, observationTime, rayEpsilon, outHit, true);
}

CausalLightSample CausalSampleLight(const CausalScene& scene, int primitive,
                                    const Vec3& receiverIn, float observationTime,
                                    Random& rand)
{
	CausalLightSample out;

	const CausalMeshEntry* entryPtr = FindEntry(scene, primitive);
	if (!entryPtr || observationTime <= 0.0f)
		return out;

	const CausalMeshEntry& entry = *entryPtr;

	// face selection: fixed probability from time-0 areas
	const float r = rand.Randf();
	int face = 0;
	while (face < entry.triCount - 1 && r > entry.areaCDF[face])
		++face;

	// uniform barycentric coordinates inside the face
	float bu, bv;
	UniformSampleTriangle(rand, bu, bv);
	const float bw = 1.0f - bu - bv;

	const DVec3 receiver(receiverIn);
	DVec3 y0, vel, e10, e20;
	LightSampleGeometry(scene, entry, face, bu, bv, bw, 0.0, y0, vel, e10, e20);

	double tau, dist;
	if (!SolveRetardedPoint(y0, vel, receiver, scene.lightSpeed, observationTime, tau, dist))
		return out; // out of domain: zero contribution, no re-sampling

	const DVec3 y = y0 + vel*tau;
	const DVec3 u = (y - receiver)*(1.0 / dist);

	DVec3 e1, e2;
	LightSampleGeometry(scene, entry, face, bu, bv, bw, tau, y0, vel, e1, e2);
	const DVec3 n = Cross(e1, e2);

	const DVec3 rhs = u + vel*(1.0 / scene.lightSpeed);
	const double denom = fabs(Dot(n, rhs));
	if (denom < 1.0e-12)
		return out;

	const double density = 2.0*dist*dist*(1.0 + Dot(u, vel) / scene.lightSpeed) / denom;
	if (!(density > 0.0) || !isfinite(density))
		return out;

	out.valid = true;
	out.te = float(tau);
	out.r = float(dist);
	out.point = ToVec3(y);
	out.u = ToVec3(u);
	out.velocity = ToVec3(vel);
	out.normal = SafeNormalize(ToVec3(n));
	out.pdf = float(density * entry.faceProb[face]);
	out.bu = bu;
	out.bv = bv;
	out.bw = bw;
	out.face = face;
	out.primitive = primitive;

	return out;
}

float CausalLightPdf(const CausalScene& scene, int primitive,
                     const Vec3& receiverIn, float observationTime,
                     int face, float bu, float bv, float bw)
{
	const CausalMeshEntry* entryPtr = FindEntry(scene, primitive);
	if (!entryPtr || face < 0 || face >= entryPtr->triCount || observationTime <= 0.0f)
		return 0.0f;

	return LightDensity(scene, *entryPtr, face, DVec3(receiverIn), observationTime, bu, bv, bw);
}

namespace
{
	Vec3 LambertCosineSample(const Vec3& normal, Random& rand, Vec3& wi, float& pdf)
	{
		Vec3 u, v;
		BasisFromVector(normal, &u, &v);

		const Vec3 s = CosineSampleHemisphere(rand.Randf(), rand.Randf());
		wi = Normalize(u*s.x + v*s.y + normal*s.z);

		pdf = Abs(Dot(wi, normal))*kInvPi;
		return wi;
	}
}

Vec3 CausalPathTrace(const Scene& scene, const Vec3& startOrigin, const Vec3& startDir,
                     float observationTime, int maxDepth, Random& rand)
{
	const CausalScene& cs = *scene.causal;

	Vec3 throughput(1.0f);
	Vec3 radiance(0.0f);

	Vec3 origin = startOrigin;
	Vec3 dir = Normalize(startDir);
	float time = observationTime;

	for (int depth = 0; depth < maxDepth; ++depth)
	{
		if (time <= 0.0f)
			break;

		CausalHit hit;
		if (!CausalTrace(cs, origin, dir, time, kRayEpsilon, hit))
			break; // light path would start before t=0, or nothing there: black

		const Primitive& prim = scene.primitives[hit.primitive];

		const bool emissive = prim.material.emission.x > 0.0f ||
		                      prim.material.emission.y > 0.0f ||
		                      prim.material.emission.z > 0.0f;
		if (emissive)
		{
			if (depth == 0)
			{
				// emission seen directly by the camera contributes unchanged
				radiance += throughput*prim.material.emission;
			}
			else
			{
				// reverse density from the real previous vertex, its time and
				// the hit face; never the static area formula or a new time
				const float lightPdf = CausalLightPdf(cs, hit.primitive, origin, time,
				                                      hit.face, hit.u, hit.v, hit.w);
				const float bsdfPdf = Max(1.0e-6f, Abs(Dot(hit.normal, dir))*kInvPi);

				const int N = prim.lightSamples + 1;
				const float cbsdf = 1.0f / N;
				const float clight = float(prim.lightSamples) / N;
				const float weight = cbsdf*bsdfPdf / (cbsdf*bsdfPdf + clight*lightPdf);

				radiance += weight*throughput*prim.material.emission;
			}
			break;
		}

		// direct lighting: the same lightSamples count as the normal path
		for (int li = 0; li < scene.primitives.size(); ++li)
		{
			const Primitive& light = scene.primitives[li];
			if (light.lightSamples <= 0)
				continue;

			for (int s = 0; s < light.lightSamples; ++s)
			{
				CausalLightSample ls = CausalSampleLight(cs, li, hit.point, hit.time, rand);
				if (!ls.valid)
					continue;

				// occlusion through the same spacetime shadow line
				CausalHit shadow;
				if (!CausalTrace(cs, hit.point, ls.u, hit.time, kRayEpsilon, shadow))
					continue;

				if (shadow.primitive != li ||
				    fabsf(shadow.time - ls.te) > 1.0e-4f*Max(ls.te, 1.0f) ||
				    fabsf(shadow.distance - ls.r) > 1.0e-3f*Max(ls.r, 1.0f))
					continue;

				const Vec3 wi = ls.u;
				const float cosTheta = Abs(Dot(hit.normal, wi));
				if (cosTheta < 1.0e-6f)
					continue;

				const float bsdfPdf = cosTheta*kInvPi;

				const int N = light.lightSamples + 1;
				const float cbsdf = 1.0f / N;
				const float clight = float(light.lightSamples) / N;
				const float weight = clight*ls.pdf / (cbsdf*bsdfPdf + clight*ls.pdf);

				const Vec3 f = prim.material.color*kInvPi;
				radiance += throughput*f*weight*light.material.emission*cosTheta / Max(ls.pdf, 1.0e-6f);
			}
		}

		// Lambert continuation (two-sided)
		Vec3 wi;
		float bsdfPdf;
		LambertCosineSample(hit.normal, rand, wi, bsdfPdf);
		if (bsdfPdf <= 0.0f)
			break;

		const float cosTheta = Abs(Dot(hit.normal, wi));
		throughput *= prim.material.color * (cosTheta / bsdfPdf);

		dir = wi;
		origin = hit.point;
		time = hit.time;
	}

	return radiance;
}

void BuildCausalScene(Scene& scene)
{
	CausalScene* cs = new CausalScene();
	cs->enabled = true;
	cs->valid = false;
	cs->lightSpeed = scene.lightSpeed;

	for (int pi = 0; pi < scene.primitives.size(); ++pi)
	{
		const Primitive& p = scene.primitives[pi];

		const Mesh* mesh = NULL;
		for (Mesh* m : scene.meshes)
		{
			if ((unsigned long)m == p.mesh.id)
			{
				mesh = m;
				break;
			}
		}
		if (!mesh)
		{
			scene.causal = cs;
			return;
		}

		const bool moving = !mesh->endPositions.empty();

		CausalMeshEntry entry;
		entry.mesh = mesh;
		entry.primitive = pi;
		entry.xform = p.startTransform;
		entry.triStart = int(cs->tris.size());
		entry.triCount = p.mesh.numIndices / 3;
		entry.totalArea0 = 0.0f;
		entry.areaCDF.resize(entry.triCount);
		entry.faceProb.resize(entry.triCount);
		entry.worldA0.resize(entry.triCount);
		entry.worldB0.resize(entry.triCount);
		entry.worldC0.resize(entry.triCount);
		entry.worldDA.resize(entry.triCount);
		entry.worldDB.resize(entry.triCount);
		entry.worldDC.resize(entry.triCount);

		for (int face = 0; face < entry.triCount; ++face)
		{
			const int i0 = mesh->indices[face*3 + 0];
			const int i1 = mesh->indices[face*3 + 1];
			const int i2 = mesh->indices[face*3 + 2];

			// local-space endpoints, shared geometry is never rewritten
			const Vec3 la0 = mesh->positions[i0];
			const Vec3 lb0 = mesh->positions[i1];
			const Vec3 lc0 = mesh->positions[i2];
			const Vec3 la1 = moving ? mesh->endPositions[i0] : la0;
			const Vec3 lb1 = moving ? mesh->endPositions[i1] : lb0;
			const Vec3 lc1 = moving ? mesh->endPositions[i2] : lc0;

			CausalTri tri;
			tri.a0 = la0; tri.b0 = lb0; tri.c0 = lc0;
			tri.da = la1 - la0; tri.db = lb1 - lb0; tri.dc = lc1 - lc0;
			tri.primitive = pi;
			tri.localFace = face;
			cs->tris.push_back(tri);

			// world-space endpoints with the static transform baked in
			const Vec3 a0 = TransformPoint(p.startTransform, la0);
			const Vec3 b0 = TransformPoint(p.startTransform, lb0);
			const Vec3 c0 = TransformPoint(p.startTransform, lc0);
			const Vec3 a1 = TransformPoint(p.startTransform, la1);
			const Vec3 b1 = TransformPoint(p.startTransform, lb1);
			const Vec3 c1 = TransformPoint(p.startTransform, lc1);

			entry.worldA0[face] = a0;
			entry.worldB0[face] = b0;
			entry.worldC0[face] = c0;
			entry.worldDA[face] = a1 - a0;
			entry.worldDB[face] = b1 - b0;
			entry.worldDC[face] = c1 - c0;

			const float area0 = 0.5f*Length(Cross(b0 - a0, c0 - a0));
			entry.totalArea0 += area0;
			entry.areaCDF[face] = entry.totalArea0;
		}

		if (entry.totalArea0 <= 0.0f)
		{
			scene.causal = cs;
			return;
		}

		for (int face = 0; face < entry.triCount; ++face)
		{
			entry.areaCDF[face] /= entry.totalArea0;
			const float area0 = 0.5f*Length(Cross(entry.worldB0[face] - entry.worldA0[face],
			                                       entry.worldC0[face] - entry.worldA0[face]));
			entry.faceProb[face] = area0 / entry.totalArea0;
		}

		cs->entries.push_back(entry);
	}

	// scene level BVH over per-entry world sweep boxes
	std::vector<Bounds> entryBoxes(cs->entries.size());
	for (int e = 0; e < int(cs->entries.size()); ++e)
	{
		const CausalMeshEntry& entry = cs->entries[e];
		Bounds box;
		for (int face = 0; face < entry.triCount; ++face)
		{
			box.AddPoint(entry.worldA0[face]);
			box.AddPoint(entry.worldB0[face]);
			box.AddPoint(entry.worldC0[face]);
			box.AddPoint(entry.worldA0[face] + entry.worldDA[face]);
			box.AddPoint(entry.worldB0[face] + entry.worldDB[face]);
			box.AddPoint(entry.worldC0[face] + entry.worldDC[face]);
		}
		entryBoxes[e] = box;
	}

	if (!entryBoxes.empty())
	{
		BVHBuilder builder;
		cs->bvh = builder.Build(&entryBoxes[0], int(entryBoxes.size()));
	}

	cs->valid = true;
	scene.causal = cs;
}
