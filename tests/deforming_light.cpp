
/*
    deforming_light.cpp -- task tests for deforming triangle mesh area
    lights.  The cases are registered with the task driver in
    task_support/driver.c and therefore define no main().
 */
#include "task_support/check.h"
#include "task_support/native.h"
#include <cmath>
#include <cstdio>
#include <limits>
#include <map>
#include <string>
#include <vector>

using std::vector;
using std::string;
using std::map;

// Minimal film: camera tests never write an image, so the abstract
// Film interface is satisfied with no-op overrides.
class TaskFilm : public Film {
public:
	TaskFilm(int x, int y) : Film(x, y) { }
	void AddSample(const Sample &, const Ray &, const Spectrum &,
			float) override { }
	void WriteImage() override { }
	void GetSampleExtent(int *xs, int *xe, int *ys,
			int *ye) const override {
		*xs = *ys = 0; *xe = xResolution; *ye = yResolution;
	}
};

static const float INF_F = std::numeric_limits<float>::infinity();

static bool Near(float a, float b, float eps = 1e-4f) {
	return std::fabs(a - b) <= eps * (1.f + std::fabs(b));
}
static bool NearPoint(const Point &a, const Point &b,
		float eps = 1e-4f) {
	return Near(a.x, b.x, eps) && Near(a.y, b.y, eps) &&
	       Near(a.z, b.z, eps);
}
// Independent reimplementation of core/mc.cpp::UniformSampleTriangle
static void LocalSampleTriangle(float u1, float u2,
		float *b1, float *b2) {
	float su1 = std::sqrt(u1);
	*b1 = 1.f - su1;
	*b2 = u2 * su1;
}
// Independent linear interpolation, clamped to [mstart, mend]
static Point LocalLerp(float time, float mstart, float mend,
		const Point &a, const Point &b) {
	float alpha = (time - mstart) / (mend - mstart);
	if (alpha < 0.f) alpha = 0.f;
	if (alpha > 1.f) alpha = 1.f;
	return Point((1.f - alpha) * a.x + alpha * b.x,
	             (1.f - alpha) * a.y + alpha * b.y,
	             (1.f - alpha) * a.z + alpha * b.z);
}
static float TriArea(const Point &a, const Point &b,
		const Point &c) {
	return 0.5f * Cross(b - a, c - a).Length();
}
static Reference<Material> MakeMatte() {
	ParamSet lp;
	map<string, Reference<Texture<float> > > ft;
	map<string, Reference<Texture<Spectrum> > > st;
	TextureParams tp(lp, lp, ft, st);
	return CreateMatteMaterial(Transform(), tp);
}
// Build a deforming (or static) triangle mesh shape.
static Reference<Shape> MakeMesh(const vector<int> &indices,
		const vector<Point> &P, const vector<Point> *P1,
		const Transform &o2w = Transform(),
		float mstart = 0.f, float mend = 1.f,
		const vector<Normal> *N = NULL) {
	ParamSet p;
	p.AddInt("indices", indices.data(), (int)indices.size());
	p.AddPoint("P", P.data(), (int)P.size());
	if (P1) p.AddPoint("P1", P1->data(), (int)P1->size());
	if (N) p.AddNormal("N", N->data(), (int)N->size());
	if (P1) {
		p.AddFloat("motionstart", &mstart);
		p.AddFloat("motionend", &mend);
	}
	return CreateTriangleMeshShape(o2w, false, p);
}
static Light *MakeAreaLight(Reference<Shape> mesh) {
	ParamSet lp;
	return CreateAreaLight(Transform(), lp, mesh);
}
struct BuiltScene {
	Scene *scene;
	Light *light;
	Reference<Shape> mesh;
	Reference<Material> matte;
};
static BuiltScene MakeScene(Reference<Shape> mesh, Light *light,
		Reference<Material> matte, const string &accel,
		const string &strategy = "all", int maxDepth = 5) {
	vector<Reference<Primitive> > objects;
	objects.push_back(new GeometricPrimitive(mesh, matte,
		static_cast<AreaLight *>(light)));
	vector<Light *> lights;
	lights.push_back(light);
	ParamSet ap;
	Primitive *acc = accel == "kdtree"
		? CreateKDTreeAccelerator(objects, ap)
		: CreateGridAccelerator(objects, ap);
	ParamSet ip;
	ip.AddString("strategy", &strategy);
	ip.AddInt("maxdepth", &maxDepth);
	SurfaceIntegrator *integ = CreateDirectIntegrator(ip);
	Scene *scene = new Scene(NULL, integ, new TaskEmptyVolume(),
		new TaskFixedSampler(), acc, lights, NULL);
	return BuiltScene{ scene, light, mesh, matte };
}
static Sample *MakeFilledSample(const Scene *scene, float v = 0.5f) {
	Sample *s = new Sample(scene->surfaceIntegrator,
		scene->volumeIntegrator, scene);
	for (u_int i = 0; i < s->n1D.size(); ++i)
		for (u_int j = 0; j < s->n1D[i]; ++j)
			s->oneD[i][j] = v;
	for (u_int i = 0; i < s->n2D.size(); ++i)
		for (u_int j = 0; j < 2 * s->n2D[i]; ++j)
			s->twoD[i][j] = v;
	s->imageX = s->imageY = s->lensU = s->lensV = 0.5f;
	s->time = 0.f;
	return s;
}
static Spectrum RenderTime(Scene *scene, const Point &origin,
		const Vector &dir, float time) {
	Sample *s = MakeFilledSample(scene);
	RayDifferential ray(Ray(origin, dir, 0.f, INF_F, time));
	ray.hasDifferentials = true;
	ray.rx = Ray(origin + Vector(0.01f, 0.f, 0.f), dir, 0.f,
		INF_F, time);
	ray.ry = Ray(origin + Vector(0.f, 0.01f, 0.f), dir, 0.f,
		INF_F, time);
	float alpha = 0.f;
	Spectrum L = scene->Li(ray, s, &alpha);
	delete s;
	BSDF::FreeAll();
	return L;
}
// A single deforming triangle translated along +z, in the xy plane.
static void TranslatingTri(vector<Point> *P, vector<Point> *P1,
		vector<int> *ix) {
	*P = { Point(-1, 0, 0), Point(1, 0, 0), Point(0, 1, 0) };
	*P1 = { Point(-1, 0, 2), Point(1, 0, 2), Point(0, 1, 2) };
	*ix = { 0, 1, 2 };
}

// ----------------------------- geometry -----------------------------
static int geo_interpolated_hit() {
	vector<Point> P, P1;
	vector<int> ix;
	TranslatingTri(&P, &P1, &ix);
	Reference<Shape> mesh = MakeMesh(ix, P, &P1);
	TASK_CHECK(mesh);
	vector<Reference<Shape> > tris;
	mesh->Refine(tris);
	TASK_CHECK(tris.size() == 1);
	const Reference<Shape> tri = tris[0];
	const float times[5] = { -1.f, 0.f, 0.5f, 1.f, 2.f };
	const float zExpect[5] = { 0.f, 0.f, 1.f, 2.f, 2.f };
	for (int k = 0; k < 5; ++k) {
		// Independently interpolated expected vertex positions
		Point e0 = LocalLerp(times[k], 0.f, 1.f, P[0], P1[0]);
		Point e1 = LocalLerp(times[k], 0.f, 1.f, P[1], P1[1]);
		Point e2 = LocalLerp(times[k], 0.f, 1.f, P[2], P1[2]);
		TASK_CHECK(NearPoint(e0, Point(-1, 0, zExpect[k])));
		Ray ray(Point(0, 0.25f, -3.f), Vector(0, 0, 1), 0.f,
			INF_F, times[k]);
		float tHit;
		DifferentialGeometry dg;
		TASK_CHECK(tri->Intersect(ray, &tHit, &dg));
		TASK_CHECK(Near(dg.p.z, zExpect[k]));
		TASK_CHECK(Near(dg.p.x, 0.f));
		TASK_CHECK(Near(dg.p.y, 0.25f));
		TASK_CHECK(Near(tHit, zExpect[k] + 3.f));
		TASK_CHECK(std::fabs(dg.nn.z) > 0.999f);
		// The hit point is an affine combination of the
		// time-interpolated vertices.
		Point inside = e0; // silence unused worry, real check below
		TASK_CHECK(dg.p.x >= -1.0001f && dg.p.x <= 1.0001f);
		TASK_CHECK(NearPoint(e1, Point(1, 0, zExpect[k])));
		TASK_CHECK(NearPoint(e2, Point(0, 1, zExpect[k])));
		(void)inside;
		// UVs of the default triangle parameterization survive
		TASK_CHECK(dg.u >= -1e-5f && dg.v >= -1e-5f);
	}
	return 0;
}
static int geo_area_motion() {
	// P0 triangle area 1, P1 triangle area 4; midpoint area 2.25.
	vector<Point> P = { Point(-1, 0, 0), Point(1, 0, 0),
	                    Point(0, 1, 0) };
	vector<Point> P1 = { Point(-2, 0, 0), Point(2, 0, 0),
	                     Point(0, 2, 0) };
	vector<int> ix = { 0, 1, 2 };
	Reference<Shape> mesh = MakeMesh(ix, P, &P1);
	vector<Reference<Shape> > tris;
	mesh->Refine(tris);
	const Reference<Shape> tri = tris[0];
	TASK_CHECK(Near(tri->Area(0.f), 1.f));
	TASK_CHECK(Near(tri->Area(1.f), 4.f));
	TASK_CHECK(Near(tri->Area(0.5f), 2.25f));
	Point m0 = LocalLerp(0.5f, 0.f, 1.f, P[0], P1[0]);
	Point m1 = LocalLerp(0.5f, 0.f, 1.f, P[1], P1[1]);
	Point m2 = LocalLerp(0.5f, 0.f, 1.f, P[2], P1[2]);
	TASK_CHECK(Near(TriArea(m0, m1, m2), 2.25f));
	// The old, time-less entry still means time zero.
	TASK_CHECK(Near(tri->Area(), 1.f));
	return 0;
}
static int geo_zero_area_instant() {
	// Third vertex crosses the base edge at t = 0.5.
	vector<Point> P = { Point(-1, 0, 0), Point(1, 0, 0),
	                    Point(0, 1, 0) };
	vector<Point> P1 = { Point(-1, 0, 0), Point(1, 0, 0),
	                     Point(0, -1, 0) };
	vector<int> ix = { 0, 1, 2 };
	Reference<Shape> mesh = MakeMesh(ix, P, &P1);
	vector<Reference<Shape> > tris;
	mesh->Refine(tris);
	Reference<Shape> tri = tris[0];
	TASK_CHECK(Near(tri->Area(0.5f), 0.f));
	TASK_CHECK(tri->Area(0.f) > 0.5f);
	Ray ray(Point(0, 0, -2.f), Vector(0, 0, 1), 0.f, INF_F, 0.5f);
	float tHit;
	DifferentialGeometry dg;
	TASK_CHECK(!tri->Intersect(ray, &tHit, &dg));
	TASK_CHECK(!tri->IntersectP(ray));
	Normal ns(7, 7, 7);
	Point ps = tri->Sample(0.3f, 0.4f, &ns, 0.5f);
	TASK_CHECK(Near(ns.LengthSquared(), 0.f));
	TASK_CHECK(!std::isnan(ps.x) && !std::isnan(ps.y) &&
	           !std::isnan(ps.z));
	TASK_CHECK(Near(tri->Pdf(Point(0, 0, 3.f), 0.5f), 0.f));
	TASK_CHECK(Near(tri->Pdf(Point(0, 0, 3.f), Vector(0, 0, -1), 0.5f), 0.f));
	Light *light = MakeAreaLight(mesh);
	TASK_CHECK(Near(light->Pdf(Point(0, 0, 3.f), Normal(0, 0, 1),
		Vector(0, 0, -1), 0.5f), 0.f));
	delete light;
	return 0;
}
static int geo_invalid_specs() {
	vector<Point> P = { Point(0, 0, 0), Point(1, 0, 0),
	                    Point(0, 1, 0) };
	vector<Point> P1 = { Point(0, 0, 1), Point(1, 0, 1),
	                     Point(0, 1, 1) };
	vector<int> ix = { 0, 1, 2 };
	// Non-finite motion times and non-increasing intervals
	TASK_CHECK(!MakeMesh(ix, P, &P1, Transform(), INF_F, 2.f));
	TASK_CHECK(!MakeMesh(ix, P, &P1, Transform(), 0.f, 0.f));
	TASK_CHECK(!MakeMesh(ix, P, &P1, Transform(), 1.f, 0.f));
	TASK_CHECK(!MakeMesh(ix, P, &P1, Transform(),
		std::numeric_limits<float>::quiet_NaN(), 1.f));
	// Non-finite vertices
	vector<Point> bad = P1;
	bad[1].y = std::numeric_limits<float>::quiet_NaN();
	TASK_CHECK(!MakeMesh(ix, P, &bad));
	// Illegal indices
	vector<int> badIx = { 0, 1, 7 };
	TASK_CHECK(!MakeMesh(badIx, P, &P1));
	badIx = { 0, -1, 2 };
	TASK_CHECK(!MakeMesh(badIx, P, &P1));
	// P1 length mismatch
	vector<Point> shortP1(P1.begin(), P1.begin() + 2);
	ParamSet q;
	q.AddInt("indices", ix.data(), 3);
	q.AddPoint("P", P.data(), 3);
	q.AddPoint("P1", shortP1.data(), 2);
	TASK_CHECK(!CreateTriangleMeshShape(Transform(), false, q));
	// Deforming mesh rejects N (and S) arrays, even if the array
	// length does not match the vertex count.
	vector<Normal> N = { Normal(0, 0, 1), Normal(0, 0, 1),
	                     Normal(0, 0, 1) };
	TASK_CHECK(!MakeMesh(ix, P, &P1, Transform(), 0.f, 1.f, &N));
	vector<Normal> shortN(N.begin(), N.begin() + 2);
	TASK_CHECK(!MakeMesh(ix, P, &P1, Transform(), 0.f, 1.f,
		&shortN));
	{
		ParamSet sp;
		vector<Vector> S = { Vector(1, 0, 0), Vector(1, 0, 0),
		                     Vector(1, 0, 0) };
		sp.AddInt("indices", ix.data(), 3);
		sp.AddPoint("P", P.data(), 3);
		sp.AddPoint("P1", P1.data(), 3);
		sp.AddVector("S", S.data(), 3);
		TASK_CHECK(!CreateTriangleMeshShape(Transform(), false, sp));
	}
	// A mesh without P1 ignores the motion parameters and builds
	Reference<Shape> legacy = MakeMesh(ix, P, NULL);
	TASK_CHECK(legacy);
	return 0;
}
static int geo_swept_bounds_and_accelerators() {
	vector<Point> P, P1;
	vector<int> ix;
	TranslatingTri(&P, &P1, &ix);
	const char *accels[2] = { "grid", "kdtree" };
	for (int a = 0; a < 2; ++a) {
		Reference<Shape> mesh = MakeMesh(ix, P, &P1);
		BBox wb = mesh->WorldBound();
		TASK_CHECK(wb.pMin.z <= 0.f && wb.pMax.z >= 2.f);
		vector<Reference<Shape> > tris;
		mesh->Refine(tris);
		BBox tb = tris[0]->WorldBound();
		TASK_CHECK(tb.pMin.z <= 0.f && tb.pMax.z >= 2.f);
		Light *light = MakeAreaLight(mesh);
		BuiltScene bs = MakeScene(mesh, light, MakeMatte(),
			accels[a]);
		// A ray outside the swept bound never intersects.
		Ray miss(Point(5, 5, -2), Vector(0, 0, 1), 0.f, INF_F,
			0.5f);
		Intersection is;
		TASK_CHECK(!bs.scene->Intersect(miss, &is));
		// At t=0 the triangle is at z=0, at t=1 at z=2.
		for (int k = 0; k < 3; ++k) {
			float t = k == 0 ? 0.f : (k == 1 ? 0.5f : 1.f);
			float z = k == 0 ? 0.f : (k == 1 ? 1.f : 2.f);
			Ray ray(Point(0, 0.25f, -3.f), Vector(0, 0, 1),
				0.f, INF_F, t);
			Intersection hit;
			TASK_CHECK(bs.scene->Intersect(ray, &hit));
			TASK_CHECK(Near(hit.dg.p.z, z));
			TASK_CHECK(hit.primitive->GetAreaLight() == light);
		}
		delete bs.scene;
	}
	return 0;
}
static int geo_scale_and_reflection() {
	vector<Point> P = { Point(0, -1, 0), Point(0, 1, 0),
	                    Point(0, 0, 1) };
	vector<Point> P1 = { Point(0, -1, 2), Point(0, 1, 2),
	                     Point(0, 0, 3) };
	vector<int> ix = { 0, 1, 2 };
	// Non-uniform scale
	Transform scaled = Scale(2.f, 1.f, 3.f);
	Reference<Shape> sm = MakeMesh(ix, P, &P1, scaled);
	vector<Reference<Shape> > st;
	sm->Refine(st);
	Normal ns;
	Point sp = st[0]->Sample(0.25f, 0.6f, &ns, 0.5f);
	float b1, b2;
	LocalSampleTriangle(0.25f, 0.6f, &b1, &b2);
	Point w0 = scaled(LocalLerp(0.5f, 0.f, 1.f, P[0], P1[0]));
	Point w1 = scaled(LocalLerp(0.5f, 0.f, 1.f, P[1], P1[1]));
	Point w2 = scaled(LocalLerp(0.5f, 0.f, 1.f, P[2], P1[2]));
	Point expect = b1 * w0 + b2 * w1 + (1.f - b1 - b2) * w2;
	TASK_CHECK(NearPoint(sp, expect));
	// Reflection swaps handedness; pbrt flips the geometric normal
	// back to the object orientation.
	Transform mirror = Scale(-1.f, 1.f, 1.f);
	TASK_CHECK(mirror.SwapsHandedness());
	Reference<Shape> rm = MakeMesh(ix, P, &P1, mirror);
	vector<Reference<Shape> > rt;
	rm->Refine(rt);
	Normal mn;
	rt[0]->Sample(0.2f, 0.3f, &mn, 0.f);
	TASK_CHECK(mn.x > 0.999f);
	Point mp = rt[0]->Sample(0.2f, 0.3f, &mn, 0.5f);
	Point r0 = mirror(LocalLerp(0.5f, 0.f, 1.f, P[0], P1[0]));
	TASK_CHECK(Near(mp.x, r0.x) || std::fabs(mp.x) < 1e-4f);
	return 0;
}
static int geo_interleaved_shared_mesh() {
	vector<Point> P, P1;
	vector<int> ix;
	TranslatingTri(&P, &P1, &ix);
	Reference<Shape> mesh = MakeMesh(ix, P, &P1);
	vector<Reference<Shape> > tris;
	mesh->Refine(tris);
	Reference<Shape> tri = tris[0];
	float first[3], second[3];
	for (int round = 0; round < 2; ++round) {
		const float ts[3] = { 0.f, 1.f, 0.5f };
		for (int k = 0; k < 3; ++k) {
			Ray ray(Point(0, 0.25f, -3.f), Vector(0, 0, 1),
				0.f, INF_F, ts[k]);
			float tHit;
			DifferentialGeometry dg;
			TASK_CHECK(tri->Intersect(ray, &tHit, &dg));
			if (round == 0) first[k] = dg.p.z;
			else second[k] = dg.p.z;
		}
	}
	for (int k = 0; k < 3; ++k)
		TASK_CHECK(Near(first[k], second[k]));
	TASK_CHECK(Near(first[0], 0.f) && Near(first[1], 2.f) &&
	           Near(first[2], 1.f));
	// Repeated sampling at one time is stable and unaffected by
	// queries at other times on the same shared mesh.
	Normal na, nb;
	Point pa = tri->Sample(0.13f, 0.71f, &na, 0.37f);
	tri->Sample(0.81f, 0.22f, &nb, 0.91f);
	Point pc = tri->Sample(0.13f, 0.71f, &nb, 0.37f);
	TASK_CHECK(NearPoint(pa, pc));
	TASK_CHECK(Near(na.x, nb.x) && Near(na.y, nb.y) &&
	           Near(na.z, nb.z));
	return 0;
}
static int geo_static_compatibility() {
	vector<Point> P = { Point(-1, 0, 0), Point(1, 0, 0),
	                    Point(0, 1, 0) };
	vector<int> ix = { 0, 1, 2 };
	Reference<Shape> mesh = MakeMesh(ix, P, NULL);
	vector<Reference<Shape> > tris;
	mesh->Refine(tris);
	Reference<Shape> tri = tris[0];
	for (int k = 0; k < 4; ++k) {
		float t = k * 0.33f;
		TASK_CHECK(Near(tri->Area(t), tri->Area()));
		Ray ray(Point(0, 0.25f, -2.f), Vector(0, 0, 1), 0.f,
			INF_F, t);
		float tHit;
		DifferentialGeometry dg;
		TASK_CHECK(tri->Intersect(ray, &tHit, &dg));
		TASK_CHECK(Near(dg.p.z, 0.f));
	}
	return 0;
}

// ------------------------------ density -----------------------------
static int den_triangle_pdf_consistency() {
	vector<Point> P = { Point(-1, -1, 0), Point(1, -1, 0),
	                    Point(0, 1, 0) };
	vector<Point> P1 = { Point(-1, -1, 2), Point(1, -1, 2),
	                     Point(0, 1, 2) };
	vector<int> ix = { 0, 1, 2 };
	Reference<Shape> mesh = MakeMesh(ix, P, &P1);
	vector<Reference<Shape> > tris;
	mesh->Refine(tris);
	Reference<Shape> tri = tris[0];
	Light *light = MakeAreaLight(mesh);
	const float ts[3] = { 0.f, 0.5f, 1.f };
	for (int k = 0; k < 3; ++k) {
		float time = ts[k];
		Point ref(0.2f, -0.3f, 4.f);
		Normal ns;
		Point ps = tri->Sample(0.31f, 0.67f, &ns, time);
		Vector wi = Normalize(ps - ref);
		// Independent solid angle density from the actual hit.
		Ray ray(ref, wi, 0.f, INF_F, time);
		float tHit;
		DifferentialGeometry dg;
		TASK_CHECK(tri->Intersect(ray, &tHit, &dg));
		float area = tri->Area(time);
		float cosine = AbsDot(dg.nn, -wi);
		TASK_CHECK(cosine > 0.f);
		float expect = DistanceSquared(ref, ray(tHit)) /
			(area * cosine);
		TASK_CHECK(Near(tri->Pdf(ref, wi, time), expect, 2e-4f));
		// The area light, reached through a Light* base pointer,
		// reports the same number through both Pdf overloads.
		TASK_CHECK(Near(light->Pdf(ref, wi, time), expect, 2e-4f));
		TASK_CHECK(Near(light->Pdf(ref, Normal(0, 0, 1), wi, time),
			expect, 2e-4f));
		// Time-zero overloads match the explicit time-zero call.
		if (time == 0.f) {
			TASK_CHECK(Near(light->Pdf(ref, wi), expect, 2e-4f));
			TASK_CHECK(Near(light->Pdf(ref, Normal(0, 0, 1), wi),
				expect, 2e-4f));
		}
		// Sample_L carries the same time into its shadow ray.
		Vector swi;
		float spdf;
		VisibilityTester vis;
		Spectrum L = light->Sample_L(ref, Normal(0, 0, 1),
			0.31f, 0.67f, &swi, &spdf, &vis, time);
		TASK_CHECK(Near(vis.r.time, time));
		TASK_CHECK(spdf > 0.f && L.y() > 0.f);
		TASK_CHECK(Near(spdf, light->Pdf(ref, swi, time), 2e-4f));
		// The no-normal direct-sampling overload, called through a
		// Light* base pointer, is time-consistent as well.
		Light *base = light;
		Vector nwi;
		float npdf;
		VisibilityTester nvis;
		Spectrum nL = base->Sample_L(ref, 0.31f, 0.67f, &nwi,
			&npdf, &nvis, time);
		TASK_CHECK(Near(nvis.r.time, time));
		TASK_CHECK(npdf > 0.f && nL.y() > 0.f);
		TASK_CHECK(Near(npdf, base->Pdf(ref, nwi, time), 2e-4f));
	}
	delete light;
	return 0;
}
static int den_two_layer_shape_set() {
	// Two coincident-footprint triangles at z=0 and z=1; a -z ray
	// from (0,0,4) crosses both layers.
	vector<Point> P = {
		Point(-1, -1, 0), Point(1, -1, 0), Point(0, 1, 0),
		Point(-1, -1, 1), Point(1, -1, 1), Point(0, 1, 1) };
	vector<int> ix = { 0, 1, 2, 3, 4, 5 };
	Reference<Shape> mesh = MakeMesh(ix, P, NULL);
	vector<Reference<Shape> > tris;
	mesh->Refine(tris);
	TASK_CHECK(tris.size() == 2);
	Light *light = MakeAreaLight(mesh);
	Point ref(0, 0, 4);
	Vector wi(0, 0, -1);
	// Independently computed multi-layer density.  Each triangle has
	// area 2 (base 2, height 2), so the total area is 4; a -z ray
	// from (0,0,4) hits the layers at distances 4 and 3.
	float totalArea = 0.f;
	for (u_int i = 0; i < tris.size(); ++i) {
		Ray ray(ref, wi, 0.f, INF_F, 0.5f);
		float tHit;
		DifferentialGeometry dg;
		TASK_CHECK(tris[i]->Intersect(ray, &tHit, &dg));
		totalArea += tris[i]->Area(0.5f);
		TASK_CHECK(AbsDot(dg.nn, -wi) > 0.999f);
	}
	TASK_CHECK(Near(totalArea, 4.f));
	float expect = (16.f + 9.f) / totalArea;
	TASK_CHECK(Near(expect, 6.25f));
	TASK_CHECK(Near(light->Pdf(ref, Normal(0, 0, 1), wi, 0.5f),
		6.25f, 1e-4f));
	TASK_CHECK(Near(light->Pdf(ref, wi, 0.5f), 6.25f, 1e-4f));
	// Sampling only ever returns points on one of the two layers and
	// always produces a unit normal; the same inputs are deterministic.
	for (int i = 0; i <= 8; ++i) {
		float u1 = i / 8.f;
		Normal n1, n2;
		Point ps = tris[0]->Sample(u1, 0.42f, &n1, 0.5f);
		Point ps2 = tris[1]->Sample(u1, 0.42f, &n2, 0.5f);
		TASK_CHECK(Near(n1.LengthSquared(), 1.f));
		TASK_CHECK(Near(n2.LengthSquared(), 1.f));
		TASK_CHECK(Near(ps.z, 0.f) && Near(ps2.z, 1.f));
	}
	delete light;
	return 0;
}
static int den_area_weighted_selection() {
	// Layer A grows from area 1 to area 4, layer B keeps area 1.
	vector<Point> P = {
		Point(-1, -1, 0), Point(1, -1, 0), Point(0, 1, 0),
		Point(-1, -1, 1), Point(1, -1, 1), Point(0, 1, 1) };
	vector<Point> P1 = {
		Point(-2, -2, 0), Point(2, -2, 0), Point(0, 2, 0),
		Point(-1, -1, 1), Point(1, -1, 1), Point(0, 1, 1) };
	vector<int> ix = { 0, 1, 2, 3, 4, 5 };
	Reference<Shape> mesh = MakeMesh(ix, P, &P1);
	Light *light = MakeAreaLight(mesh);
	const int N = 400;
	for (int k = 0; k < 2; ++k) {
		float time = k == 0 ? 0.f : 1.f;
		int onA = 0;
		for (int i = 0; i < N; ++i) {
			// Drive the ShapeSet through the area light's shape via
			// Sample_L; classify by which layer plane was sampled.
			Vector wi;
			float pdf;
			VisibilityTester vis;
			Point ref(0, 0, 5);
			light->Sample_L(ref, Normal(0, 0, 1),
				i / float(N), 0.37f, &wi, &pdf, &vis, time);
			TASK_CHECK(pdf >= 0.f);
			// Shadow segment endpoint identifies the layer plane.
			if (vis.r(1.f).z < 0.5f) ++onA;
		}
		float fraction = onA / float(N);
		// At t=0 areas are 1 and 1 (50%), at t=1 they are 4 and 1
		// (80% on layer A).
		float expect = k == 0 ? 0.5f : 0.8f;
		TASK_CHECK(std::fabs(fraction - expect) < 0.08f);
	}
	delete light;
	return 0;
}
static int den_zero_area_and_determinism() {
	vector<Point> P = { Point(-1, 0, 0), Point(1, 0, 0),
	                    Point(0, 1, 0) };
	vector<Point> P1 = { Point(-1, 0, 0), Point(1, 0, 0),
	                     Point(0, -1, 0) };
	vector<int> ix = { 0, 1, 2 };
	Reference<Shape> mesh = MakeMesh(ix, P, &P1);
	Light *light = MakeAreaLight(mesh);
	Point ref(0, 0, 3);
	Vector wi(0, 0, -1);
	TASK_CHECK(Near(light->Pdf(ref, wi, 0.5f), 0.f));
	TASK_CHECK(Near(light->Pdf(ref, Normal(0, 0, 1), wi, 0.5f),
		0.f));
	Vector swi;
	float pdf;
	VisibilityTester vis;
	Spectrum L = light->Sample_L(ref, Normal(0, 0, 1),
		0.4f, 0.6f, &swi, &pdf, &vis, 0.5f);
	TASK_CHECK(std::isfinite(pdf) && Near(pdf, 0.f));
	TASK_CHECK(!L.IsNaN() && !std::isnan(swi.x));
	// Determinism at a healthy time, interleaved with other times.
	Normal n1, n2, n3;
	vector<Reference<Shape> > tris;
	mesh->Refine(tris);
	Point a = tris[0]->Sample(0.17f, 0.83f, &n1, 0.23f);
	tris[0]->Sample(0.91f, 0.05f, &n2, 0.77f);
	Point b = tris[0]->Sample(0.17f, 0.83f, &n3, 0.23f);
	TASK_CHECK(NearPoint(a, b));
	TASK_CHECK(Near(n1.x, n3.x) && Near(n1.y, n3.y) &&
	           Near(n1.z, n3.z));
	delete light;
	return 0;
}

// ----------------------------- transport ----------------------------
namespace {
struct TransportRig {
	Reference<Shape> lightMesh;
	Reference<Shape> receiverMesh;
	Light *light;
	Reference<Material> matte;
};
// Light triangle faces -z above a matte receiver at z=0; the eye
// looks down onto the same side the emitter faces.  The light
// translates in x, so direct illumination changes with shutter time.
TransportRig BuildRig(bool collapseAtHalf) {
	TransportRig rig;
	// Winding chosen so the geometric normal points toward -z.
	vector<Point> LP = { Point(-1, -1, 3), Point(0, 1, 3),
	                    Point(1, -1, 3) };
	vector<Point> LP1;
	if (collapseAtHalf)
		// At t=0.5 the middle vertex reaches y=-1, the base edge
		// line, so the triangle has exactly zero area then.
		LP1 = { Point(3, -1, 3), Point(4, -3, 3),
		        Point(5, -1, 3) };
	else
		LP1 = { Point(3, -1, 3), Point(4, 1, 3),
		        Point(5, -1, 3) };
	vector<int> lix = { 0, 1, 2 };
	rig.lightMesh = MakeMesh(lix, LP, &LP1);
	// Large receiver triangle in z=0 facing +z toward the light.
	vector<Point> RP = { Point(-8, -8, 0), Point(0, 8, 0),
	                    Point(8, -8, 0) };
	vector<int> rix = { 0, 1, 2 };
	rig.receiverMesh = MakeMesh(rix, RP, NULL);
	rig.matte = MakeMatte();
	rig.light = MakeAreaLight(rig.lightMesh);
	return rig;
}
Scene *BuildTransportScene(TransportRig &rig,
		const string &strategy, const string &accel,
		bool pathIntegrator = false) {
	vector<Reference<Primitive> > objects;
	objects.push_back(new GeometricPrimitive(rig.lightMesh,
		rig.matte, static_cast<AreaLight *>(rig.light)));
	objects.push_back(new GeometricPrimitive(rig.receiverMesh,
		rig.matte, NULL));
	vector<Light *> lights;
	lights.push_back(rig.light);
	ParamSet ap;
	Primitive *acc = accel == "kdtree"
		? CreateKDTreeAccelerator(objects, ap)
		: CreateGridAccelerator(objects, ap);
	ParamSet ip;
	ip.AddString("strategy", &strategy);
	SurfaceIntegrator *integ = pathIntegrator
		? CreatePathIntegrator(ip)
		: CreateDirectIntegrator(ip);
	return new Scene(NULL, integ, new TaskEmptyVolume(),
		new TaskFixedSampler(), acc, lights, NULL);
}
} // namespace
static int tr_direct_strategies_and_path() {
	const string strategies[3] = { "all", "one", "weighted" };
	const char *accels[2] = { "grid", "kdtree" };
	Point eye(0, -5, 2);
	Vector dir = Normalize(Vector(0, 2.5, -2));
	for (int usePath = 0; usePath < 2; ++usePath) {
		for (int si = 0; si < 3; ++si) {
			TransportRig rig = BuildRig(false);
			Scene *scene = BuildTransportScene(rig, strategies[si],
				accels[si % 2], usePath == 1);
			Spectrum L0 = RenderTime(scene, eye, dir, 0.f);
			Spectrum Lh = RenderTime(scene, eye, dir, 0.5f);
			Spectrum L1 = RenderTime(scene, eye, dir, 1.f);
			Spectrum Lh2 = RenderTime(scene, eye, dir, 0.5f);
			TASK_CHECK(L0.y() >= 0.f && L1.y() >= 0.f &&
			           Lh.y() >= 0.f);
			TASK_CHECK(std::isfinite(L0.y()) &&
			           std::isfinite(Lh.y()) &&
			           std::isfinite(L1.y()));
			// Same samples and time must give the identical result.
			TASK_CHECK(Near(Lh.y(), Lh2.y(), 1e-6f));
			// The light sweeps in x, so the two endpoints differ.
			TASK_CHECK(std::fabs(L1.y() - L0.y()) > 1e-4f);
			delete scene;
		}
	}
	return 0;
}
static int tr_zero_area_transport() {
	TransportRig rig = BuildRig(true);
	Scene *direct = BuildTransportScene(rig, "all", "grid", false);
	Point eye(0, -5, 2);
	Vector dir = Normalize(Vector(0, 2.5, -2));
	Spectrum Lhalf = RenderTime(direct, eye, dir, 0.5f);
	Spectrum Lzero = RenderTime(direct, eye, dir, 0.f);
	TASK_CHECK(Near(Lhalf.y(), 0.f));
	TASK_CHECK(Lzero.y() > 0.f);
	delete direct;
	TransportRig rig2 = BuildRig(true);
	Scene *path = BuildTransportScene(rig2, "all", "kdtree", true);
	Spectrum Phalf = RenderTime(path, eye, dir, 0.5f);
	Spectrum Pzero = RenderTime(path, eye, dir, 0.f);
	TASK_CHECK(Near(Phalf.y(), 0.f));
	TASK_CHECK(Pzero.y() > 0.f);
	delete path;
	return 0;
}
static int tr_mis_branches_same_time() {
	TransportRig rig = BuildRig(false);
	Scene *scene = BuildTransportScene(rig, "all", "grid", false);
	// Receiver hit point for a ray from the eye.
	Point eye(0, -5, 2);
	Vector dir = Normalize(Vector(0, 2.5, -2));
	Ray view(eye, dir, 0.f, INF_F, 0.5f);
	Intersection is;
	TASK_CHECK(scene->Intersect(view, &is));
	Point p = is.dg.p;
	Normal n = is.dg.nn;
	Light *light = rig.light;
	// Light-sampling MIS branch
	Vector wi;
	float lightPdf;
	VisibilityTester vis;
	Spectrum Li = light->Sample_L(p, n, 0.31f, 0.67f, &wi,
		&lightPdf, &vis, 0.5f);
	TASK_CHECK(lightPdf > 0.f && Li.y() > 0.f);
	TASK_CHECK(Near(vis.r.time, 0.5f));
	TASK_CHECK(vis.Unoccluded(scene));
	// BSDF-sampling MIS branch: look the direction back up at the
	// same shutter time and hit the actual emissive geometry.
	float reversePdf = light->Pdf(p, n, wi, 0.5f);
	TASK_CHECK(Near(reversePdf, lightPdf, 2e-4f));
	RayDifferential probe(p, wi);
	probe.time = 0.5f;
	probe.rx.time = probe.ry.time = 0.5f;
	Intersection lightHit;
	TASK_CHECK(scene->Intersect(probe, &lightHit));
	TASK_CHECK(lightHit.primitive->GetAreaLight() == light);
	Spectrum Le = lightHit.Le(-wi);
	TASK_CHECK(Le.y() > 0.f);
	// Independently recompute the density from the time-0.5 area.
	vector<Reference<Shape> > tris;
	rig.lightMesh->Refine(tris);
	float area = tris[0]->Area(0.5f);
	float d2 = DistanceSquared(p, lightHit.dg.p);
	float cosine = AbsDot(lightHit.dg.nn, -wi);
	float expect = d2 / (area * cosine);
	TASK_CHECK(Near(reversePdf, expect, 2e-4f));
	// A probe at time zero must not report the time-0.5 result.
	RayDifferential probe0(Ray(p, wi, 0.f, INF_F, 0.f));
	Intersection hit0;
	bool hitAtZero = scene->Intersect(probe0, &hit0);
	if (hitAtZero && hit0.primitive->GetAreaLight() == light) {
		float d20 = DistanceSquared(p, hit0.dg.p);
		TASK_CHECK(!Near(d20, d2, 1e-3f) ||
		           Near(light->Pdf(p, n, wi, 0.f),
		                light->Pdf(p, n, wi, 0.5f)));
	}
	delete scene;
	return 0;
}
static int tr_shadow_time_occlusion() {
	// The light starts behind a static occluder and sweeps out of
	// it; visibility must be evaluated at the ray's shutter time.
	TransportRig rig = BuildRig(false);
	vector<Point> OP = { Point(-2, -2, 2.2f), Point(2, -2, 2.2f),
	                    Point(0, 2, 2.2f) };
	vector<int> oix = { 0, 1, 2 };
	Reference<Shape> occluder = MakeMesh(oix, OP, NULL);
	vector<Reference<Primitive> > objects;
	objects.push_back(new GeometricPrimitive(rig.lightMesh,
		rig.matte, static_cast<AreaLight *>(rig.light)));
	objects.push_back(new GeometricPrimitive(rig.receiverMesh,
		rig.matte, NULL));
	objects.push_back(new GeometricPrimitive(occluder, rig.matte,
		NULL));
	vector<Light *> lights;
	lights.push_back(rig.light);
	ParamSet ap;
	Primitive *acc = CreateGridAccelerator(objects, ap);
	Scene *scene = new Scene(NULL, CreateDirectIntegrator(ParamSet()),
		new TaskEmptyVolume(), new TaskFixedSampler(), acc, lights,
		NULL);
	Point p(0, 0, 0);
	Normal n(0, 0, 1);
	Vector wi0, wi1;
	float pdf0, pdf1;
	VisibilityTester v0, v1;
	rig.light->Sample_L(p, n, 0.5f, 0.5f, &wi0, &pdf0, &v0, 0.f);
	rig.light->Sample_L(p, n, 0.5f, 0.5f, &wi1, &pdf1, &v1, 1.f);
	TASK_CHECK(pdf0 > 0.f && pdf1 > 0.f);
	TASK_CHECK(!v0.Unoccluded(scene));
	TASK_CHECK(v1.Unoccluded(scene));
	delete scene;
	return 0;
}
static int tr_camera_shutter_time() {
	// A real PerspectiveCamera must turn the sample's shutter phase
	// into ray.time, and Scene::Li must render the moving emitter at
	// that instant.
	TransportRig rig = BuildRig(false);
	vector<Reference<Primitive> > objects;
	objects.push_back(new GeometricPrimitive(rig.lightMesh,
		rig.matte, static_cast<AreaLight *>(rig.light)));
	objects.push_back(new GeometricPrimitive(rig.receiverMesh,
		rig.matte, NULL));
	vector<Light *> lights;
	lights.push_back(rig.light);
	ParamSet ap;
	Primitive *acc = CreateGridAccelerator(objects, ap);
	ParamSet ip;
	string strategy = "all";
	ip.AddString("strategy", &strategy);
	Scene *scene = new Scene(NULL, CreateDirectIntegrator(ip),
		new TaskEmptyVolume(), new TaskFixedSampler(), acc, lights,
		NULL);
	Transform world2cam = LookAt(Point(0, -5, 2), Point(0, 0, 1),
		Vector(0, 0, 1));
	Film *film = new TaskFilm(8, 8);
	ParamSet cp;
	float sopen = 0.f, sclose = 1.f;
	cp.AddFloat("shutteropen", &sopen);
	cp.AddFloat("shutterclose", &sclose);
	Camera *camera = CreatePerspectiveCamera(cp, world2cam, film);
	Sample *s = MakeFilledSample(scene);
	const float phases[3] = { 0.f, 0.5f, 1.f };
	float values[3];
	for (int k = 0; k < 3; ++k) {
		s->time = phases[k];
		s->imageX = 4.f; s->imageY = 4.f;
		Ray ray;
		float weight = camera->GenerateRay(*s, &ray);
		TASK_CHECK(weight > 0.f);
		TASK_CHECK(Near(ray.time, phases[k]));
		RayDifferential rd(ray);
		float alpha = 0.f;
		Spectrum L = scene->Li(rd, s, &alpha);
		TASK_CHECK(std::isfinite(L.y()));
		values[k] = L.y();
		BSDF::FreeAll();
		// Re-rendering the identical camera ray is deterministic.
		Spectrum again = scene->Li(rd, s, &alpha);
		TASK_CHECK(Near(again.y(), L.y(), 1e-6f));
		BSDF::FreeAll();
	}
	// The emitter sweeps in x, so the two shutter endpoints differ.
	TASK_CHECK(std::fabs(values[2] - values[0]) > 1e-5f);
	// The camera maps the shutter phase linearly; the mesh itself
	// performs the endpoint clamping (covered by interpolated_hit).
	s->time = 1.7f;
	Ray clamped;
	camera->GenerateRay(*s, &clamped);
	TASK_CHECK(Near(clamped.time, 1.7f));
	delete s;
	delete scene;
	delete camera;
	return 0;
}

// --------------------------- registration ---------------------------
const struct task_case task_cases[] = {
	{ "geometry", "interpolated_hit", geo_interpolated_hit },
	{ "geometry", "area_motion", geo_area_motion },
	{ "geometry", "zero_area_instant", geo_zero_area_instant },
	{ "geometry", "invalid_specs", geo_invalid_specs },
	{ "geometry", "swept_bounds", geo_swept_bounds_and_accelerators },
	{ "geometry", "scale_reflection", geo_scale_and_reflection },
	{ "geometry", "interleaved_shared_mesh", geo_interleaved_shared_mesh },
	{ "geometry", "static_compatibility", geo_static_compatibility },
	{ "density", "triangle_pdf", den_triangle_pdf_consistency },
	{ "density", "two_layer_shape_set", den_two_layer_shape_set },
	{ "density", "area_weighted_selection", den_area_weighted_selection },
	{ "density", "zero_area_determinism", den_zero_area_and_determinism },
	{ "transport", "strategies_and_path", tr_direct_strategies_and_path },
	{ "transport", "zero_area_transport", tr_zero_area_transport },
	{ "transport", "mis_branches_same_time", tr_mis_branches_same_time },
	{ "transport", "shadow_time_occlusion", tr_shadow_time_occlusion },
	{ "transport", "camera_shutter_time", tr_camera_shutter_time },
};
const size_t task_case_count =
	sizeof(task_cases) / sizeof(task_cases[0]);
