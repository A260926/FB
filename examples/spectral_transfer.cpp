
/*
    pbrt source code Copyright(c) 1998-2010 Matt Pharr and Greg Humphreys.

    This file is part of pbrt.

    pbrt is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

 */

// examples/spectral_transfer.cpp*
//
// Renders a real scene through the spectralmix transfer coating:
// a ground triangle carries a discrete channel operator that moves
// incident blue energy into the green channel, lit by an area
// light.  The image is evaluated through the normal Scene::Li path
// integrator path and written as a binary PPM to
// <argv[1]>/spectral.ppm.
#include "task_support/native.h"
#include "spectral_transfer.h"
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

const int XRES = 96;
const int YRES = 72;

Spectrum MakeSpectrum(float a, float b, float c) {
	float v[COLOR_SAMPLES] = { a, b, c };
	return Spectrum(v);
}

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

Reference<Material> MakeCoating() {
	// Row-major transfer matrix; each column sums to 0.8, so the
	// coating conserves energy and absorbs the remaining 0.2.
	// Row i, column j: incident channel j to outgoing channel i.
	float c[COLOR_SAMPLES*COLOR_SAMPLES] = {
		0.5f, 0.1f, 0.0f,
		0.2f, 0.5f, 0.6f,
		0.1f, 0.2f, 0.2f
	};
	ParamSet mp;
	mp.AddFloat("transfer", c, COLOR_SAMPLES*COLOR_SAMPLES);
	Spectrum kd(0.f);
	mp.AddSpectrum("Kd", &kd, 1);
	std::map<string, Reference<Texture<float> > > ft;
	std::map<string, Reference<Texture<Spectrum> > > st;
	TextureParams tp(mp, mp, ft, st);
	return CreateSpectralMixMaterial(Transform(), tp);
}

// Convert a rendering Spectrum to display RGB, matching the
// pipeline used by film/image.cpp.
void SpectrumToRGB(const Spectrum &s, float rgb[3]) {
	float xyz[3];
	s.XYZ(xyz);
	const float rWeight[3] = { 3.240479f, -1.537150f, -0.498535f };
	const float gWeight[3] = { -0.969256f, 1.875991f, 0.041556f };
	const float bWeight[3] = { 0.055648f, -0.204043f, 1.057311f };
	rgb[0] = rWeight[0]*xyz[0] + rWeight[1]*xyz[1] + rWeight[2]*xyz[2];
	rgb[1] = gWeight[0]*xyz[0] + gWeight[1]*xyz[1] + gWeight[2]*xyz[2];
	rgb[2] = bWeight[0]*xyz[0] + bWeight[1]*xyz[1] + bWeight[2]*xyz[2];
}

} // namespace

int main(int argc, char *argv[]) {
	if (argc < 2) {
		std::fprintf(stderr, "usage: %s <output-directory>\n", argv[0]);
		return 1;
	}
	// Build the scene geometry: a coated ground and an area light.
	Reference<Material> coating = MakeCoating();
	if (!coating) {
		std::fprintf(stderr, "spectralmix factory rejected the "
			"coating parameters\n");
		return 1;
	}
	Reference<Shape> ground = MakeTriangle(
		Point(-8, -8, 0), Point(8, -8, 0), Point(0, 8, 0));
	// Winding gives the emitter a normal facing -z towards the ground.
	Reference<Shape> lightShape = MakeTriangle(
		Point(-1.5f, -1.5f, 3.f), Point(0.f, 1.5f, 3.f),
		Point(1.5f, -1.5f, 3.f));
	ParamSet lightParams;
	Spectrum Le = MakeSpectrum(1.2f, 1.0f, 0.8f);
	lightParams.AddSpectrum("L", &Le, 1);
	AreaLight *areaLight = (AreaLight *)
		CreateAreaLight(Transform(), lightParams, lightShape);

	vector<Reference<Primitive> > prims;
	prims.push_back(new GeometricPrimitive(ground, coating, NULL));
	prims.push_back(new GeometricPrimitive(lightShape, coating,
		areaLight));
	Primitive *accel = CreateGridAccelerator(prims, ParamSet());
	vector<Light *> lights;
	lights.push_back(areaLight);

	ParamSet integratorParams;
	int maxDepth = 5;
	integratorParams.AddInt("maxdepth", &maxDepth, 1);
	Scene scene(NULL, CreatePathIntegrator(integratorParams),
		new TaskEmptyVolume(), new TaskFixedSampler(),
		accel, lights, NULL);
	Sample sample(scene.surfaceIntegrator,
		scene.volumeIntegrator, &scene);
	for (u_int i = 0; i < sample.n1D.size(); ++i)
		for (u_int j = 0; j < sample.n1D[i]; ++j)
			sample.oneD[i][j] = 0.5f;
	for (u_int i = 0; i < sample.n2D.size(); ++i)
		for (u_int j = 0; j < 2*sample.n2D[i]; ++j)
			sample.twoD[i][j] = 0.5f;

	// Pinhole camera frame, looking from (0,-4,2.5) at (0,0,0.5).
	Point eye(0.f, -4.f, 2.5f);
	Point target(0.f, 0.f, 0.5f);
	Vector forward = Normalize(target - eye);
	Vector worldUp(0.f, 0.f, 1.f);
	Vector right = Normalize(Cross(forward, worldUp));
	Vector up = Cross(right, forward);
	const float fov = Radians(60.f);
	const float halfH = tanf(fov * 0.5f);
	const float aspect = float(XRES) / float(YRES);

	std::string outName = argv[1];
	outName += "/spectral.ppm";
	FILE *out = std::fopen(outName.c_str(), "wb");
	if (!out) {
		std::fprintf(stderr, "cannot open %s\n", outName.c_str());
		return 1;
	}
	std::fprintf(out, "P6\n%d %d\n255\n", XRES, YRES);
	unsigned char row[XRES*3];
	for (int y = 0; y < YRES; ++y) {
		for (int x = 0; x < XRES; ++x) {
			float u = ((x + 0.5f) / XRES) * 2.f - 1.f;
			float v = 1.f - ((y + 0.5f) / YRES) * 2.f;
			Vector dir = Normalize(forward +
				right * (u * halfH * aspect) +
				up * (v * halfH));
			RayDifferential ray(eye, dir);
			sample.imageX = float(x);
			sample.imageY = float(y);
			sample.lensU = sample.lensV = 0.5f;
			sample.time = 0.5f;
			Spectrum L = scene.Li(ray, &sample, NULL);
			BSDF::FreeAll();
			float rgb[3];
			SpectrumToRGB(L, rgb);
			for (int k = 0; k < 3; ++k) {
				rgb[k] = Clamp(rgb[k], 0.f, 1.f);
				// Gamma encode for the PPM display value.
				float g = powf(rgb[k], 1.f / 2.2f);
				row[3*x + k] = (unsigned char)(g * 255.f + 0.5f);
			}
		}
		std::fwrite(row, 1, sizeof(row), out);
	}
	std::fclose(out);
	std::printf("wrote %s (%dx%d)\n", outName.c_str(), XRES, YRES);
	return 0;
}
