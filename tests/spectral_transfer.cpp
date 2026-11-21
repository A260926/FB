
/*
    pbrt source code Copyright(c) 1998-2010 Matt Pharr and Greg Humphreys.

    This file is part of pbrt.

    pbrt is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

 */

// tests/spectral_transfer.cpp*
//
// Task tests for the discrete-channel spectral transfer operator,
// the spectralmix material and the operator-aware transport paths.
// The file only registers task cases; main() lives in
// task_support/driver.c.
#include "task_support/check.h"
#include "task_support/native.h"
#include "spectral_transfer.h"
#include "material.h"
#include "reflection.h"
#include "transport.h"
#include "scene.h"
#include "paramset.h"
#include <cmath>
#include <map>
#include <string>

namespace {

const float EPS = 2e-5f;

Spectrum MakeSpectrum(float a, float b, float c) {
	float v[COLOR_SAMPLES] = { a, b, c };
	return Spectrum(v);
}

SpectralTransfer MakeTransfer(float m00, float m01, float m02,
		float m10, float m11, float m12,
		float m20, float m21, float m22) {
	float m[COLOR_SAMPLES*COLOR_SAMPLES] = {
		m00, m01, m02, m10, m11, m12, m20, m21, m22
	};
	return SpectralTransfer(m);
}

bool Near(float a, float b, float eps = EPS) {
	return std::fabs(a - b) <= eps;
}

bool NearSpectrum(const Spectrum &a, const Spectrum &b,
		float eps = EPS) {
	for (int i = 0; i < COLOR_SAMPLES; ++i)
		if (!Near(a[i], b[i], eps)) return false;
	return true;
}

bool NearTransfer(const SpectralTransfer &a, const SpectralTransfer &b,
		float eps = EPS) {
	for (int i = 0; i < COLOR_SAMPLES; ++i)
		for (int j = 0; j < COLOR_SAMPLES; ++j)
			if (!Near(a(i, j), b(i, j), eps)) return false;
	return true;
}

// ----------------------------------------------------------------
// algebra group
// ----------------------------------------------------------------
int algebra_direction_and_product_order() {
	// Hand-built asymmetric matrices with distinct entries
	SpectralTransfer A = MakeTransfer(
		0.1f, 0.2f, 0.3f,
		0.4f, 0.5f, 0.6f,
		0.7f, 0.8f, 0.9f);
	SpectralTransfer B = MakeTransfer(
		0.2f, 0.0f, 0.5f,
		0.1f, 0.3f, 0.0f,
		0.4f, 0.0f, 0.2f);
	Spectrum v = MakeSpectrum(1.f, 2.f, 3.f);
	// A * v computed by hand:
	// row0: .1 + .4 + .9 = 1.4
	// row1: .4 + 1.0 + 1.8 = 3.2
	// row2: .7 + 1.6 + 2.7 = 5.0
	Spectrum Av = A * v;
	TASK_CHECK(Near(Av[0], 1.4f));
	TASK_CHECK(Near(Av[1], 3.2f));
	TASK_CHECK(Near(Av[2], 5.0f));
	// Matrix composition order, entry (0,0) computed by hand:
	// (A*B)(0,0) = .1*.2 + .2*.1 + .3*.4 = .16
	// (B*A)(0,0) = .2*.1 + 0*.4 + .5*.7 = .37
	SpectralTransfer AB = A * B;
	SpectralTransfer BA = B * A;
	TASK_CHECK(Near(AB(0, 0), 0.16f));
	TASK_CHECK(Near(BA(0, 0), 0.37f));
	TASK_CHECK(!NearTransfer(AB, BA, 1e-6f));
	// Composition action order: (A*B) v == A (B v)
	Spectrum lhs = (A * B) * v;
	Spectrum rhs = A * (B * v);
	TASK_CHECK(NearSpectrum(lhs, rhs));
	// Addition and scalar scaling
	SpectralTransfer ApB = A + B;
	for (int i = 0; i < COLOR_SAMPLES; ++i)
		for (int j = 0; j < COLOR_SAMPLES; ++j)
			TASK_CHECK(Near(ApB(i, j), A(i, j) + B(i, j)));
	SpectralTransfer sA = A * 2.5f;
	for (int i = 0; i < COLOR_SAMPLES; ++i)
		for (int j = 0; j < COLOR_SAMPLES; ++j)
			TASK_CHECK(Near(sA(i, j), A(i, j) * 2.5f));
	return 0;
}

int algebra_diagonal_adaptation() {
	Spectrum d = MakeSpectrum(0.3f, 0.6f, 0.9f);
	SpectralTransfer D(d);
	for (int i = 0; i < COLOR_SAMPLES; ++i)
		for (int j = 0; j < COLOR_SAMPLES; ++j)
			TASK_CHECK(Near(D(i, j), i == j ? d[i] : 0.f));
	// Diagonal operator action matches per-channel Spectrum multiply
	Spectrum v = MakeSpectrum(2.f, 0.5f, 4.f);
	Spectrum got = D * v;
	TASK_CHECK(NearSpectrum(got,
		MakeSpectrum(0.6f, 0.3f, 3.6f)));
	// Diagonal() round trips, identity and zero predicates
	TASK_CHECK(NearSpectrum(D.Diagonal(), d));
	SpectralTransfer I = SpectralTransfer::Identity();
	TASK_CHECK(NearSpectrum(I * v, v));
	SpectralTransfer Z;
	TASK_CHECK(Z.IsZero());
	TASK_CHECK(!D.IsZero());
	return 0;
}

int algebra_pure_offdiagonal_not_zero() {
	// All non-diagonal, no zero rows: blue -> green transfer
	SpectralTransfer C = MakeTransfer(
		0.f, 0.f, 0.f,
		0.f, 0.f, 0.8f,
		0.f, 0.f, 0.f);
	TASK_CHECK(!C.IsZero());
	Spectrum blue = MakeSpectrum(0.f, 0.f, 1.f);
	Spectrum out = C * blue;
	TASK_CHECK(NearSpectrum(out, MakeSpectrum(0.f, 0.8f, 0.f)));
	// The diagonal part is an actual zero spectrum
	TASK_CHECK(C.Diagonal().Black());
	return 0;
}

int algebra_parameter_rejection() {
	std::map<string, Reference<Texture<float> > > ft;
	std::map<string, Reference<Texture<Spectrum> > > st;
	// Wrong element count
	{
		ParamSet mp;
		float bad[8] = { 0.f };
		mp.AddFloat("transfer", bad, 8);
		TextureParams tp(mp, mp, ft, st);
		Material *m = CreateSpectralMixMaterial(Transform(), tp);
		TASK_CHECK(m == NULL);
	}
	// Negative coefficient
	{
		ParamSet mp;
		float c[9] = {
			0.5f, 0.f, 0.f,
			0.f, -0.1f, 0.f,
			0.f, 0.f, 0.5f };
		mp.AddFloat("transfer", c, 9);
		TextureParams tp(mp, mp, ft, st);
		Material *m = CreateSpectralMixMaterial(Transform(), tp);
		TASK_CHECK(m == NULL);
	}
	// Non-finite coefficient
	{
		ParamSet mp;
		float c[9] = {
			INFINITY, 0.f, 0.f,
			0.f, 0.5f, 0.f,
			0.f, 0.f, 0.5f };
		mp.AddFloat("transfer", c, 9);
		TextureParams tp(mp, mp, ft, st);
		Material *m = CreateSpectralMixMaterial(Transform(), tp);
		TASK_CHECK(m == NULL);
	}
	// Column sum plus Kd exceeds one
	{
		ParamSet mp;
		float c[9] = {
			0.6f, 0.f, 0.f,
			0.4f, 0.5f, 0.f,
			0.f, 0.f, 0.5f };
		mp.AddFloat("transfer", c, 9);
		Spectrum kd = MakeSpectrum(0.1f, 0.6f, 0.f);
		mp.AddSpectrum("Kd", &kd, 1);
		TextureParams tp(mp, mp, ft, st);
		Material *m = CreateSpectralMixMaterial(Transform(), tp);
		TASK_CHECK(m == NULL);
	}
	// Negative Kd
	{
		ParamSet mp;
		float c[9] = { 0.f };
		mp.AddFloat("transfer", c, 9);
		Spectrum kd = MakeSpectrum(-0.1f, 0.f, 0.f);
		mp.AddSpectrum("Kd", &kd, 1);
		TextureParams tp(mp, mp, ft, st);
		Material *m = CreateSpectralMixMaterial(Transform(), tp);
		TASK_CHECK(m == NULL);
	}
	// Valid: column sums plus Kd exactly one is accepted
	{
		ParamSet mp;
		float c[9] = {
			0.6f, 0.f, 0.f,
			0.4f, 0.5f, 0.f,
			0.f, 0.f, 0.5f };
		mp.AddFloat("transfer", c, 9);
		Spectrum kd = MakeSpectrum(0.f, 0.5f, 0.5f);
		mp.AddSpectrum("Kd", &kd, 1);
		TextureParams tp(mp, mp, ft, st);
		Material *m = CreateSpectralMixMaterial(Transform(), tp);
		TASK_CHECK(m != NULL);
		delete m;
	}
	return 0;
}

// ----------------------------------------------------------------
// Shared scene/BSDF construction helpers
// ----------------------------------------------------------------
Reference<Shape> MakeTriangle(const Point &p0, const Point &p1,
		const Point &p2) {
	ParamSet p;
	int indices[] = { 0, 1, 2 };
	Point pts[] = { p0, p1, p2 };
	float uv[] = { 0, 0, 1, 0, 1, 1 };
	p.AddInt("indices", indices, 3);
	p.AddPoint("P", pts, 3);
	p.AddFloat("uv", uv, 6);
	return CreateTriangleMeshShape(Transform(), false, p);
}

Reference<Material> MakeSpectralMix(const SpectralTransfer &C,
		const Spectrum &kd) {
	ParamSet mp;
	mp.AddFloat("transfer", C.Rows(),
		COLOR_SAMPLES*COLOR_SAMPLES);
	mp.AddSpectrum("Kd", &kd, 1);
	std::map<string, Reference<Texture<float> > > ft;
	std::map<string, Reference<Texture<Spectrum> > > st;
	TextureParams tp(mp, mp, ft, st);
	return CreateSpectralMixMaterial(Transform(), tp);
}

Reference<Material> MakeDefaultMatte() {
	ParamSet empty;
	std::map<string, Reference<Texture<float> > > ft;
	std::map<string, Reference<Texture<Spectrum> > > st;
	TextureParams tp(empty, empty, ft, st);
	return CreateMatteMaterial(Transform(), tp);
}

// Build a fresh accelerator over one z=const plane triangle.
Primitive *MakePlaneAccel(Reference<Material> mat,
		AreaLight *light = NULL, float z = 0.f) {
	Reference<Shape> tri = MakeTriangle(
		Point(-2, -2, z), Point(2, -2, z), Point(0, 2, z));
	vector<Reference<Primitive> > prims;
	prims.push_back(new GeometricPrimitive(tri, mat, light));
	return CreateGridAccelerator(prims, ParamSet());
}

// Hit the z=const plane straight on and return its evaluated BSDF.
BSDF *GetPlaneBSDF(Reference<Primitive> prim, float z) {
	RayDifferential ray(Point(0.f, 0.f, z + 1.f),
		Vector(0.f, 0.f, -1.f));
	Intersection isect;
	if (!prim->Intersect(ray, &isect))
		return NULL;
	return isect.GetBSDF(ray);
}

void FillSample(Sample *sample) {
	for (u_int i = 0; i < sample->n1D.size(); ++i)
		for (u_int j = 0; j < sample->n1D[i]; ++j)
			sample->oneD[i][j] = 0.5f;
	for (u_int i = 0; i < sample->n2D.size(); ++i)
		for (u_int j = 0; j < 2*sample->n2D[i]; ++j)
			sample->twoD[i][j] = 0.5f;
}

// ----------------------------------------------------------------
// mixture group
// ----------------------------------------------------------------
int mixture_component_selection() {
	Reference<Material> matte = MakeDefaultMatte();
	Reference<Primitive> prim = MakePlaneAccel(matte);
	BSDF *base = GetPlaneBSDF(prim, 0.f);
	TASK_CHECK(base != NULL);
	const DifferentialGeometry dg = base->dgShading;

	Spectrum kd = MakeSpectrum(0.1f, 0.2f, 0.f);
	SpectralTransfer C = MakeTransfer(
		0.05f, 0.1f, 0.f,
		0.2f, 0.3f, 0.1f,
		0.f, 0.1f, 0.4f);
	Spectrum R = MakeSpectrum(0.3f, 0.4f, 0.5f);
	BSDF *mix = BSDF_ALLOC(BSDF)(dg, Normal(0, 0, 1));
	mix->Add(BSDF_ALLOC(Lambertian)(kd));
	mix->Add(BSDF_ALLOC(TransferBxDF)(C));
	mix->Add(BSDF_ALLOC(SpecularReflection)(R,
		BSDF_ALLOC(FresnelNoOp)()));

	Vector wo(0.f, 0.f, 1.f);
	const float u1 = 0.3f, u2 = 0.7f;
	Vector expectedDiffuse = CosineSampleHemisphere(u1, u2);

	// Component 0: ordinary Lambertian lobe
	{
		Vector wi;
		float pdf = 0.f;
		BxDFType flags = BxDFType(0);
		SpectralTransfer F = mix->Sample_fTransfer(wo, &wi,
			u1, u2, 0.0f, &pdf, BSDF_ALL, &flags);
		TASK_CHECK(flags == (BSDF_REFLECTION | BSDF_DIFFUSE));
		TASK_CHECK(Near(wi.x, expectedDiffuse.x));
		TASK_CHECK(Near(wi.y, expectedDiffuse.y));
		TASK_CHECK(Near(wi.z, expectedDiffuse.z));
		// Mixture pdf: two cosine lobes over three components
		float expectedPdf =
			2.f * fabsf(expectedDiffuse.z) * INV_PI / 3.f;
		TASK_CHECK(Near(pdf, expectedPdf));
		TASK_CHECK(Near(mix->Pdf(wo, wi), expectedPdf));
		// Full operator: both diffuse lobes summed
		SpectralTransfer expected =
			SpectralTransfer(kd * INV_PI) + C * INV_PI;
		TASK_CHECK(NearTransfer(F, expected));
	}
	// Component 1: transfer lobe, same cosine direction and pdf
	{
		Vector wi;
		float pdf = 0.f;
		BxDFType flags = BxDFType(0);
		SpectralTransfer F = mix->Sample_fTransfer(wo, &wi,
			u1, u2, 0.4f, &pdf, BSDF_ALL, &flags);
		TASK_CHECK(flags == (BSDF_REFLECTION | BSDF_DIFFUSE));
		TASK_CHECK(Near(wi.x, expectedDiffuse.x));
		TASK_CHECK(Near(wi.y, expectedDiffuse.y));
		TASK_CHECK(Near(wi.z, expectedDiffuse.z));
		float expectedPdf =
			2.f * fabsf(expectedDiffuse.z) * INV_PI / 3.f;
		TASK_CHECK(Near(pdf, expectedPdf));
		SpectralTransfer expected =
			SpectralTransfer(kd * INV_PI) + C * INV_PI;
		TASK_CHECK(NearTransfer(F, expected));
	}
	// Component 2: specular lobe keeps the discrete rule
	{
		Vector wi;
		float pdf = 0.f;
		BxDFType flags = BxDFType(0);
		SpectralTransfer F = mix->Sample_fTransfer(wo, &wi,
			u1, u2, 0.8f, &pdf, BSDF_ALL, &flags);
		TASK_CHECK(flags == (BSDF_REFLECTION | BSDF_SPECULAR));
		TASK_CHECK(Near(wi.x, 0.f) && Near(wi.y, 0.f) &&
			Near(wi.z, 1.f));
		TASK_CHECK(Near(pdf, 1.f / 3.f));
		TASK_CHECK(NearTransfer(F, SpectralTransfer(R)));
	}
	BSDF::FreeAll();
	return 0;
}

int mixture_spectrum_query_is_diagonal() {
	Reference<Material> matte = MakeDefaultMatte();
	Reference<Primitive> prim = MakePlaneAccel(matte);
	BSDF *base = GetPlaneBSDF(prim, 0.f);
	TASK_CHECK(base != NULL);

	// Pure off-diagonal transfer lobe
	SpectralTransfer C = MakeTransfer(
		0.f, 0.f, 0.f,
		0.f, 0.f, 0.8f,
		0.f, 0.f, 0.f);
	BSDF *only = BSDF_ALLOC(BSDF)(base->dgShading, Normal(0, 0, 1));
	only->Add(BSDF_ALLOC(TransferBxDF)(C));
	Vector wo(0, 0, 1), wi(0.2f, 0.1f, 0.9f);
	// Old Spectrum query is the diagonal part: exactly zero here...
	Spectrum f = only->f(wo, wi);
	TASK_CHECK(f.Black());
	// ...but the operator response is non-zero, so not absorption
	SpectralTransfer ft2 = only->fTransfer(wo, wi);
	TASK_CHECK(!ft2.IsZero());
	TASK_CHECK(NearTransfer(ft2, C * INV_PI));

	// Automatic random-sample form returns F / pdf: for the lone
	// cosine-sampled transfer lobe that is C / cos(theta).
	Vector swi;
	BxDFType flags = BxDFType(0);
	SpectralTransfer autoF = only->Sample_fTransfer(wo, &swi,
		BSDF_ALL, &flags);
	TASK_CHECK(flags == (BSDF_REFLECTION | BSDF_DIFFUSE));
	TASK_CHECK(NearTransfer(autoF, C * (1.f / fabsf(swi.z))));

	// Lambertian automatic form: R / cos(theta)
	BSDF *lam = BSDF_ALLOC(BSDF)(base->dgShading, Normal(0, 0, 1));
	Spectrum kd = MakeSpectrum(0.25f, 0.5f, 0.75f);
	lam->Add(BSDF_ALLOC(Lambertian)(kd));
	Vector lwi;
	SpectralTransfer lamAuto = lam->Sample_fTransfer(wo, &lwi);
	TASK_CHECK(NearTransfer(lamAuto,
		SpectralTransfer(kd * (1.f / fabsf(lwi.z)))));
	BSDF::FreeAll();
	return 0;
}

int mixture_material_bsdf_lobes() {
	// The real spectralmix material produces one BSDF mixing the
	// transfer lobe and a Lambertian Kd lobe.
	SpectralTransfer C = MakeTransfer(
		0.2f, 0.f, 0.f,
		0.f, 0.3f, 0.4f,
		0.f, 0.f, 0.1f);
	Spectrum kd = MakeSpectrum(0.1f, 0.2f, 0.3f);
	Reference<Material> mat = MakeSpectralMix(C, kd);
	Reference<Primitive> prim = MakePlaneAccel(mat);
	BSDF *bsdf = GetPlaneBSDF(prim, 0.f);
	TASK_CHECK(bsdf != NULL);
	TASK_CHECK(bsdf->NumComponents() == 2);
	TASK_CHECK(bsdf->NumComponents(BSDF_ALL_REFLECTION) == 2);
	Vector wo(0, 0, 1), wi(0, 0, 1);
	SpectralTransfer expected =
		SpectralTransfer(kd * INV_PI) + C * INV_PI;
	TASK_CHECK(NearTransfer(bsdf->fTransfer(wo, wi), expected));
	// Spectrum view sees only the diagonal components
	TASK_CHECK(NearSpectrum(bsdf->f(wo, wi), expected.Diagonal()));
	BSDF::FreeAll();
	return 0;
}

// ----------------------------------------------------------------
// Transport test scaffolding: custom lights
// ----------------------------------------------------------------
class TaskDistantLight : public Light {
public:
	TaskDistantLight(const Vector &dir, const Spectrum &L)
		: Light(Transform()), lightDir(Normalize(dir)), L(L) { }
	bool IsDeltaLight() const { return true; }
	Spectrum Power(const Scene *) const { return L; }
	Spectrum Sample_L(const Point &, Vector *wi,
			VisibilityTester *vis) const {
		*wi = lightDir;
		vis->SetRay(Point(), *wi);
		return L;
	}
	Spectrum Sample_L(const Point &p, float, float, Vector *wi,
			float *pdf, VisibilityTester *vis) const {
		*wi = lightDir;
		*pdf = 1.f;
		vis->SetRay(p, *wi);
		return L;
	}
	Spectrum Sample_L(const Scene *, float, float, float, float,
			Ray *ray, float *pdf) const {
		ray->o = Point();
		ray->d = lightDir;
		*pdf = 1.f;
		return L;
	}
	float Pdf(const Point &, const Vector &) const { return 0.f; }
private:
	Vector lightDir;
	Spectrum L;
};

class TaskEnvLight : public Light {
public:
	TaskEnvLight(const Spectrum &L) : Light(Transform()), L(L) { }
	bool IsDeltaLight() const { return false; }
	Spectrum Power(const Scene *) const { return L; }
	Spectrum Le(const RayDifferential &) const { return L; }
	Spectrum Sample_L(const Point &p, Vector *wi,
			VisibilityTester *vis) const {
		*wi = Vector(0, 0, 1);
		vis->SetRay(p, *wi);
		return L;
	}
	Spectrum Sample_L(const Point &p, const Normal &n, float u1,
			float u2, Vector *wi, float *pdf,
			VisibilityTester *vis) const {
		float x, y, z;
		ConcentricSampleDisk(u1, u2, &x, &y);
		z = sqrtf(max(0.f, 1.f - x*x - y*y));
		Vector v1, v2;
		CoordinateSystem(Normalize(Vector(n)), &v1, &v2);
		*wi = Vector(v1.x*x + v2.x*y + n.x*z,
		             v1.y*x + v2.y*y + n.y*z,
		             v1.z*x + v2.z*y + n.z*z);
		*pdf = fabsf(wi->z) * INV_TWOPI;
		vis->SetRay(p, *wi);
		return L;
	}
	Spectrum Sample_L(const Point &p, float u1, float u2, Vector *wi,
			float *pdf, VisibilityTester *vis) const {
		*wi = UniformSampleSphere(u1, u2);
		*pdf = UniformSpherePdf();
		vis->SetRay(p, *wi);
		return L;
	}
	Spectrum Sample_L(const Scene *, float, float, float, float,
			Ray *ray, float *pdf) const {
		ray->o = Point();
		ray->d = Vector(0, 0, 1);
		*pdf = 1.f;
		return L;
	}
	float Pdf(const Point &, const Vector &) const {
		return 1.f / (4.f * M_PI);
	}
	float Pdf(const Point &, const Normal &n, const Vector &wi) const {
		return AbsDot(n, wi) * INV_TWOPI;
	}
private:
	Spectrum L;
};

// Volume integrator that applies a non-uniform channel attenuation
// only to upward rays leaving the floor region, i.e. the single
// segment between the two coatings.  Everywhere else it is unit.
class TaskSlabVolume : public VolumeIntegrator {
public:
	TaskSlabVolume(const Spectrum &tau) : tau(tau) { }
	Spectrum Li(const Scene *, const RayDifferential &,
			const Sample *, float *) const { return Spectrum(0.f); }
	Spectrum Transmittance(const Scene *, const Ray &ray,
			const Sample *, float *) const {
		if (ray.o.z < 0.5f && ray.d.z > 0.6f)
			return tau;
		return Spectrum(1.f);
	}
private:
	Spectrum tau;
};

// Inverse of pbrt's ConcentricSampleDisk: unit-disk point (dx,dy)
// back to the unit-square inputs (u1,u2) that produce it.
void RefConcentricInverse(float dx, float dy, float *u1, float *u2) {
	float r = sqrtf(dx*dx + dy*dy);
	if (r == 0.f) { *u1 = *u2 = 0.5f; return; }
	float theta = atan2f(dy, dx) / (M_PI * 0.25f);
	if (theta < 0.f) theta += 8.f;
	float sx, sy;
	if (theta <= 1.f || theta >= 7.f) {
		if (theta >= 7.f) theta -= 8.f;
		sx = r;
		sy = r * theta;
	} else if (theta <= 3.f) {
		sx = r * (2.f - theta);
		sy = r;
	} else if (theta <= 5.f) {
		sx = -r;
		sy = r * (4.f - theta);
	} else {
		sx = r * (theta - 6.f);
		sy = -r;
	}
	*u1 = (sx + 1.f) * 0.5f;
	*u2 = (sy + 1.f) * 0.5f;
}

// Independent reference geometry helpers (not calling pbrt's Pdf).
// Matches Triangle::Sample's barycentric convention:
// UniformSampleTriangle gives (u, v) and the point is
// u*p0 + v*p1 + (1-u-v)*p2.
void RefTriangleSample(const Point p[3], float u1, float u2,
		Point *ps, Normal *ns) {
	float su1 = sqrtf(u1);
	float u = 1.f - su1;
	float v = u2 * su1;
	*ps = u*p[0] + v*p[1] + (1.f - u - v)*p[2];
	*ns = Normalize(Normal(Cross(p[1]-p[0], p[2]-p[0])));
}

bool RefRayTriangle(const Point &o, const Vector &d,
		const Point p[3], float *tHit) {
	Vector e1 = p[1] - p[0], e2 = p[2] - p[0];
	Vector s1 = Cross(d, e2);
	float divisor = Dot(s1, e1);
	if (divisor == 0.f) return false;
	float inv = 1.f / divisor;
	Vector s = o - p[0];
	float b1 = Dot(s, s1) * inv;
	if (b1 < 0.f || b1 > 1.f) return false;
	Vector s2 = Cross(s, e1);
	float b2 = Dot(d, s2) * inv;
	if (b2 < 0.f || b1 + b2 > 1.f) return false;
	float t = Dot(e2, s2) * inv;
	if (t < 1e-6f) return false;
	*tHit = t;
	return true;
}

float PowerWeight(float fp, float gp) {
	float f = fp, g = gp;
	return f*f / (f*f + g*g);
}

// ----------------------------------------------------------------
// transport group
// ----------------------------------------------------------------
// Pure off-diagonal transfer lit by a delta light: direct lighting
// through all three direct-lighting strategies.
int transport_delta_offdiagonal() {
	SpectralTransfer C = MakeTransfer(
		0.f, 0.f, 0.f,
		0.f, 0.f, 0.8f,
		0.f, 0.f, 0.f);
	Reference<Material> mat = MakeSpectralMix(C, Spectrum(0.f));
	const char *strategyNames[] = { "all", "one", "weighted" };
	Spectrum results[3];
	for (int k = 0; k < 3; ++k) {
		Primitive *accel = MakePlaneAccel(mat);
		vector<Light *> lights;
		lights.push_back(new TaskDistantLight(
			Vector(0.f, 0.f, 1.f), MakeSpectrum(0.f, 0.f, 1.f)));
		ParamSet ip;
		string strategy(strategyNames[k]);
		ip.AddString("strategy", &strategy, 1);
		Scene scene(NULL, CreateDirectIntegrator(ip),
			new TaskEmptyVolume(), new TaskFixedSampler(),
			accel, lights, NULL);
		Sample sample(scene.surfaceIntegrator,
			scene.volumeIntegrator, &scene);
		FillSample(&sample);
		RayDifferential ray(Point(0, 0, 1), Vector(0, 0, -1));
		results[k] = scene.Li(ray, &sample, NULL);
		BSDF::FreeAll();
	}
	// Delta light, wi = +z: L = (C/pi) * Li * cos / 1
	Spectrum expected = C * INV_PI *
		MakeSpectrum(0.f, 0.f, 1.f);
	for (int k = 0; k < 3; ++k)
		TASK_CHECK(NearSpectrum(results[k], expected));
	// Pure red illumination through the same coating transfers to
	// zero: a real path whose transfer result is black even though
	// the lobe operator is non-zero.
	{
		Primitive *accel = MakePlaneAccel(mat);
		vector<Light *> lights;
		lights.push_back(new TaskDistantLight(
			Vector(0.f, 0.f, 1.f), MakeSpectrum(1.f, 0.f, 0.f)));
		ParamSet ip;
		string one = "one";
		ip.AddString("strategy", &one, 1);
		Scene scene(NULL, CreateDirectIntegrator(ip),
			new TaskEmptyVolume(), new TaskFixedSampler(),
			accel, lights, NULL);
		Sample sample(scene.surfaceIntegrator,
			scene.volumeIntegrator, &scene);
		FillSample(&sample);
		RayDifferential ray(Point(0, 0, 1), Vector(0, 0, -1));
		Spectrum zeroed = scene.Li(ray, &sample, NULL);
		TASK_CHECK(zeroed.Black());
		BSDF::FreeAll();
	}
	return 0;
}

// Old material path keeps its original Lambertian value.
int transport_old_matte_path() {
	ParamSet mp;
	Spectrum kd = MakeSpectrum(0.2f, 0.4f, 0.1f);
	mp.AddSpectrum("Kd", &kd, 1);
	std::map<string, Reference<Texture<float> > > ft;
	std::map<string, Reference<Texture<Spectrum> > > st;
	Reference<Material> matte = CreateMatteMaterial(Transform(),
		TextureParams(mp, mp, ft, st));
	Primitive *accel = MakePlaneAccel(matte);
	vector<Light *> lights;
	lights.push_back(new TaskDistantLight(
		Vector(0.f, 0.f, 1.f), Spectrum(1.f)));
	ParamSet ip;
	string one = "one";
	ip.AddString("strategy", &one, 1);
	Scene scene(NULL, CreateDirectIntegrator(ip),
		new TaskEmptyVolume(), new TaskFixedSampler(),
		accel, lights, NULL);
	Sample sample(scene.surfaceIntegrator,
		scene.volumeIntegrator, &scene);
	FillSample(&sample);
	RayDifferential ray(Point(0, 0, 1), Vector(0, 0, -1));
	Spectrum L = scene.Li(ray, &sample, NULL);
	// L = R/pi * 1 * cos(=1) / lightPdf(=1)
	TASK_CHECK(NearSpectrum(L, kd * INV_PI));
	BSDF::FreeAll();
	return 0;
}

// Two distinct transfers in one real path plus a non-uniform
// channel attenuation, an area light and both MIS branches.
//
// Room corner: the camera looks at the floor (coating C1 at z=0).
// The deterministic first bounce reaches the back wall (coating C2
// at y=2); the wall's direct lighting sees the ceiling emitter.
// An absorbing shelf blocks every direct line from the floor to
// the emitter, so the only light transport is the two-transfer
// chain.  The slab volume attenuates the floor->wall segment.
int transport_two_transfers_chain() {
	// C1: pure off-diagonal blue -> green.  C2: asymmetric matrix.
	SpectralTransfer C1 = MakeTransfer(
		0.f, 0.f, 0.f,
		0.f, 0.f, 0.8f,
		0.f, 0.f, 0.f);
	SpectralTransfer C2 = MakeTransfer(
		0.1f, 0.2f, 0.f,
		0.3f, 0.4f, 0.1f,
		0.2f, 0.1f, 0.3f);
	Reference<Material> mat1 = MakeSpectralMix(C1, Spectrum(0.f));
	Reference<Material> mat2 = MakeSpectralMix(C2, Spectrum(0.f));
	Reference<Material> black = MakeSpectralMix(
		SpectralTransfer(), Spectrum(0.f));

	Reference<Shape> floor = MakeTriangle(
		Point(-2, -2, 0), Point(2, -2, 0), Point(0, 2, 0));
	// Winding gives the wall a -y normal facing the room.
	Reference<Shape> wall = MakeTriangle(
		Point(-2, 2, 0), Point(2, 2, 0), Point(0, 2, 3));
	// Narrow ceiling emitter near the wall; winding gives its
	// normal a -z orientation facing the room.
	Reference<Shape> emitterShape = MakeTriangle(
		Point(-1.5f, 1.4f, 3.f), Point(0.f, 1.7f, 3.f),
		Point(1.5f, 1.4f, 3.f));
	// Absorbing baffle between the floor and the emitter.  Its apex
	// (y = 1.3) sits below the 45-degree bounce (y = 1.5 at z = 1.5)
	// but above every direct floor->emitter crossing (y <= 1.275).
	Reference<Shape> shelf = MakeTriangle(
		Point(-6, -3, 1.5f), Point(6, -3, 1.5f),
		Point(0, 1.3f, 1.5f));

	ParamSet lightParamSet;
	Spectrum Le = MakeSpectrum(0.5f, 0.7f, 1.1f);
	lightParamSet.AddSpectrum("L", &Le, 1);
	AreaLight *areaLight = (AreaLight *)
		CreateAreaLight(Transform(), lightParamSet, emitterShape);

	vector<Reference<Primitive> > prims;
	prims.push_back(new GeometricPrimitive(floor, mat1, NULL));
	prims.push_back(new GeometricPrimitive(wall, mat2, NULL));
	prims.push_back(new GeometricPrimitive(shelf, black, NULL));
	prims.push_back(new GeometricPrimitive(emitterShape, black,
		areaLight));
	Primitive *accel = CreateGridAccelerator(prims, ParamSet());
	vector<Light *> lights;
	lights.push_back(areaLight);

	Spectrum tau = MakeSpectrum(0.9f, 0.7f, 0.5f);
	ParamSet ip;
	int maxDepth = 5;
	ip.AddInt("maxdepth", &maxDepth, 1);
	Scene scene(NULL, CreatePathIntegrator(ip),
		new TaskSlabVolume(tau), new TaskFixedSampler(),
		accel, lights, NULL);
	Sample sample(scene.surfaceIntegrator,
		scene.volumeIntegrator, &scene);
	FillSample(&sample);
	// PathIntegrator requests per depth: 2D offsets
	// {lightPos, bsdfDir, outgoingDir}; outgoing is index 3*d+2.
	// Bounce directions are chosen in world space and converted
	// through the actual shading frames, so the test does not
	// assume the triangles' tangent axes are axis aligned.
	const float a = 1.f / sqrtf(2.f);
	const float b = 2.f / sqrtf(5.f);
	const float c = 1.f / sqrtf(5.f);
	Vector worldWi0(0.f, a, a);
	Vector worldWi1(0.f, -c, b);
	{
		// Probe the floor frame and invert the cosine map.
		RayDifferential probe(Point(0.f, -2.f, 1.f),
			Normalize(Vector(0.f, 2.f, -1.f)));
		Intersection pi;
		TASK_CHECK(accel->Intersect(probe, &pi));
		BSDF *fb = pi.GetBSDF(probe);
		Vector local0 = fb->WorldToLocal(worldWi0);
		float u1, u2;
		RefConcentricInverse(local0.x, local0.y, &u1, &u2);
		float vx, vy;
		ConcentricSampleDisk(u1, u2, &vx, &vy);
		TASK_CHECK(Near(vx, local0.x, 1e-5f));
		TASK_CHECK(Near(vy, local0.y, 1e-5f));
		sample.twoD[2][0] = u1;
		sample.twoD[2][1] = u2;
		BSDF::FreeAll();
	}
	{
		// Probe the wall frame at v1 and invert the cosine map.
		RayDifferential probe(Point(0, 0, 0), worldWi0);
		Intersection pi;
		TASK_CHECK(accel->Intersect(probe, &pi));
		BSDF *wb = pi.GetBSDF(probe);
		Vector local1 = wb->WorldToLocal(worldWi1);
		float u1, u2;
		RefConcentricInverse(local1.x, local1.y, &u1, &u2);
		float vx, vy;
		ConcentricSampleDisk(u1, u2, &vx, &vy);
		TASK_CHECK(Near(vx, local1.x, 1e-5f));
		TASK_CHECK(Near(vy, local1.y, 1e-5f));
		// Outgoing path direction at depth 1 (slot 3d+2 = 5).
		sample.twoD[5][0] = u1;
		sample.twoD[5][1] = u2;
		// EstimateDirect's BSDF-sampling MIS branch at depth 1
		// uses slot 3d+1 = 4; point it at the same oblique
		// direction so that branch deterministically reaches the
		// emitter as well.
		sample.twoD[4][0] = u1;
		sample.twoD[4][1] = u2;
		BSDF::FreeAll();
	}
	// Camera below the baffle looking at the floor origin.
	RayDifferential ray(Point(0.f, -2.f, 1.f),
		Normalize(Vector(0.f, 2.f, -1.f)));
	Spectrum L = scene.Li(ray, &sample, NULL);
	BSDF::FreeAll();

	// ---- Independent reference computation ----
	// Bounce points: v0 = (0,0,0); the floor->wall ray dir is
	// (0,a,a), reaching y=2 at v1 = (0,2,2).
	Point v0(0, 0, 0), v1(0, 2, 2);
	Point floorTri[3] = { Point(-2,-2,0), Point(2,-2,0),
		Point(0,2,0) };
	Point wallTri[3] = { Point(-2,2,0), Point(2,2,0),
		Point(0,2,3) };
	float tHit;
	Vector d01(0.f, a, a);
	TASK_CHECK(RefRayTriangle(Point(0,-2,1.f),
		Normalize(Vector(0,2,-1.f)), floorTri, &tHit));
	TASK_CHECK(RefRayTriangle(v0, d01, wallTri, &tHit));
	TASK_CHECK(Near(v0.y + d01.y*tHit, v1.y));
	TASK_CHECK(Near(d01.z*tHit, v1.z));
	// The baffle blocks every direct floor->emitter line; the ray
	// toward each emitter vertex crosses the baffle plane first.
	Point shelfTri[3] = { Point(-6,-3,1.5f), Point(6,-3,1.5f),
		Point(0,1.3f,1.5f) };
	Point emitTri[3] = { Point(-1.5f,1.4f,3.f), Point(0,1.7f,3.f),
		Point(1.5f,1.4f,3.f) };
	for (int k = 0; k < 3; ++k) {
		Vector dk = Normalize(emitTri[k] - v0);
		TASK_CHECK(RefRayTriangle(v0, dk, shelfTri, &tHit));
	}
	// The floor->wall segment clears the baffle (y = 1.5 at the
	// baffle plane, above its y = 1.3 apex).
	TASK_CHECK(!RefRayTriangle(v0, d01, shelfTri, &tHit));

	// A lone cosine-sampled lobe gives scattering weight
	// (C/pi) cos / (cos/pi) = C.  The wall's own C2 is applied as
	// the local F = C2/pi inside EstimateDirect, so the throughput
	// arriving at v1 is only T = C1 * diag(tau).
	SpectralTransfer T = C1 * SpectralTransfer(tau);

	// Direct lighting at v1 against the triangular area emitter.
	float triArea = 0.5f *
		Cross(emitTri[1]-emitTri[0], emitTri[2]-emitTri[0]).Length();
	TASK_CHECK(Near(triArea, 0.45f));
	Spectrum ref(0.f);
	// Light-sampling MIS branch with the fixed (0.5, 0.5) sample.
	{
		Point ps;
		Normal ns;
		RefTriangleSample(emitTri, 0.5f, 0.5f, &ps, &ns);
		Vector wiL = Normalize(ps - v1);
		float dist2 = DistanceSquared(v1, ps);
		float cosAtWall = AbsDot(Normal(0, -1, 0), wiL);
		float cosAtLight = fabsf(Dot(ns, -wiL));
		float lightPdf = dist2 / (cosAtLight * triArea);
		float bsdfPdf = cosAtWall * INV_PI;
		TASK_CHECK(RefRayTriangle(v1, wiL, emitTri, &tHit));
		float w = PowerWeight(lightPdf, bsdfPdf);
		SpectralTransfer F = C2 * INV_PI;
		Spectrum term = F * Le;
		term = term * (cosAtWall * w / lightPdf);
		ref += T * term;
	}
	// BSDF-sampling MIS branch with the fixed oblique direction:
	// world (0,-c,b), c = 1/sqrt(5), hitting the emitter at
	// (0,1.5,3).
	{
		const float c = 1.f / sqrtf(5.f);
		Vector wi1(0.f, -c, b);
		TASK_CHECK(Near(wi1.Length(), 1.f));
		float cosAtWall = AbsDot(Normal(0, -1, 0), wi1);
		TASK_CHECK(Near(cosAtWall, c));
		float bpdf = cosAtWall * INV_PI;
		TASK_CHECK(RefRayTriangle(v1, wi1, emitTri, &tHit));
		Point hit = v1 + wi1 * tHit;
		float dist2 = DistanceSquared(v1, hit);
		Normal emitNormal(0, 0, -1);
		float cosAtLight = fabsf(Dot(emitNormal, -wi1));
		float lpdf = dist2 / (cosAtLight * triArea);
		float w = PowerWeight(bpdf, lpdf);
		SpectralTransfer F = C2 * INV_PI;
		Spectrum term = F * Le;
		term = term * (cosAtWall * w / bpdf);
		ref += T * term;
	}
	TASK_CHECK(NearSpectrum(L, ref, EPS));
	// Channel attribution: C1 is the last operator on the eye side
	// and its only non-zero row is the green row, so all surviving
	// energy leaves in the green channel; the red and blue outputs
	// are exactly zero even though the path carried real energy.
	TASK_CHECK(Near(L[0], 0.f));
	TASK_CHECK(Near(L[2], 0.f));
	TASK_CHECK(L[1] > 0.f);
	return 0;
}

// Environment light contribution through a transfer coating.
int transport_environment() {
	SpectralTransfer C1 = MakeTransfer(
		0.f, 0.f, 0.f,
		0.f, 0.f, 0.8f,
		0.f, 0.f, 0.f);
	Reference<Material> mat = MakeSpectralMix(C1, Spectrum(0.f));
	Primitive *accel = MakePlaneAccel(mat);
	Spectrum Lenv = MakeSpectrum(0.3f, 0.6f, 0.9f);
	vector<Light *> lights;
	lights.push_back(new TaskEnvLight(Lenv));
	ParamSet ip;
	int maxDepth = 5;
	ip.AddInt("maxdepth", &maxDepth, 1);
	Scene scene(NULL, CreatePathIntegrator(ip),
		new TaskEmptyVolume(), new TaskFixedSampler(),
		accel, lights, NULL);
	Sample sample(scene.surfaceIntegrator,
		scene.volumeIntegrator, &scene);
	FillSample(&sample);
	RayDifferential ray(Point(0, 0, 1), Vector(0, 0, -1));
	Spectrum L = scene.Li(ray, &sample, NULL);
	BSDF::FreeAll();

	// All 0.5 samples drive both branches straight up (cos = 1).
	// Light pdf = 1/(2 pi), bsdf pdf = 1/pi, F = C1/pi.
	SpectralTransfer F = C1 * INV_PI;
	float lp = INV_TWOPI, bp = INV_PI;
	float wL = PowerWeight(lp, bp);
	float wB = PowerWeight(bp, lp);
	Spectrum expected = F * Lenv * (wL * (1.f / lp) + wB * (1.f / bp));
	TASK_CHECK(NearSpectrum(L, expected, EPS));
	// Only the green output channel can be populated.
	TASK_CHECK(Near(L[0], 0.f));
	TASK_CHECK(Near(L[2], 0.f));
	TASK_CHECK(L[1] > 0.f);
	return 0;
}

} // namespace

// External linkage so task_support/driver.c can find the registry.
const task_case task_cases[] = {
	{ "algebra", "direction_and_product_order", algebra_direction_and_product_order },
	{ "algebra", "diagonal_adaptation", algebra_diagonal_adaptation },
	{ "algebra", "pure_offdiagonal_not_zero", algebra_pure_offdiagonal_not_zero },
	{ "algebra", "parameter_rejection", algebra_parameter_rejection },
	{ "mixture", "component_selection", mixture_component_selection },
	{ "mixture", "spectrum_query_is_diagonal", mixture_spectrum_query_is_diagonal },
	{ "mixture", "material_bsdf_lobes", mixture_material_bsdf_lobes },
	{ "transport", "delta_offdiagonal", transport_delta_offdiagonal },
	{ "transport", "old_matte_path", transport_old_matte_path },
	{ "transport", "two_transfers_chain", transport_two_transfers_chain },
	{ "transport", "environment", transport_environment },
};
const size_t task_case_count =
	sizeof(task_cases) / sizeof(task_cases[0]);
