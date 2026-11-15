
/*
    deforming_light.cpp -- example renderer for a deforming triangle
    mesh area light.  A single shared Scene is rendered at three
    shutter times; only ray.time changes between frames, so the
    output shows the same moving emissive geometry seen at different
    instants without rebuilding any mesh or acceleration structure.

    Usage: deforming_light <output-directory>
    Writes <output-directory>/motion.ppm (binary P6, three frames
    side by side).
 */
#include "core/scene.h"
#include "core/film.h"
#include "core/volume.h"
#include "core/sampling.h"
#include "core/texture.h"
#include "task_support/native.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>

using std::map;
using std::string;

static const int W = 96;
static const int H = 72;
static const int FRAMES = 3;

static Reference<Material> MakeMatte() {
	ParamSet lp;
	map<string, Reference<Texture<float> > > ft;
	map<string, Reference<Texture<Spectrum> > > st;
	TextureParams tp(lp, lp, ft, st);
	return CreateMatteMaterial(Transform(), tp);
}
static Reference<Shape> MakeMesh(const int *ix, int nix,
		const Point *P, int np, const Point *P1) {
	ParamSet p;
	p.AddInt("indices", ix, nix);
	p.AddPoint("P", P, np);
	if (P1) p.AddPoint("P1", P1, np);
	return CreateTriangleMeshShape(Transform(), false, p);
}
static unsigned char ToByte(float v) {
	v = Clamp(v, 0.f, 1.f);
	v = std::pow(v, 1.f / 2.2f);
	int q = int(v * 255.f + 0.5f);
	return (unsigned char)Clamp(q, 0, 255);
}
int main(int argc, char **argv) {
	if (argc < 2) {
		std::fprintf(stderr, "usage: %s <output-directory>\n",
			argv[0]);
		return 1;
	}
	// Deforming emitter: a triangle facing -z that sweeps along +x.
	Point LP[3] = { Point(-1, -1, 3), Point(0, 1, 3),
	                Point(1, -1, 3) };
	Point LP1[3] = { Point(3, -1, 3), Point(4, 1, 3),
	                 Point(5, -1, 3) };
	int lix[3] = { 0, 1, 2 };
	Reference<Shape> lightMesh = MakeMesh(lix, 3, LP, 3, LP1);
	// Static matte receiver in z=0, normal +z toward the emitter.
	Point RP[3] = { Point(-8, -8, 0), Point(0, 8, 0),
	                Point(8, -8, 0) };
	int rix[3] = { 0, 1, 2 };
	Reference<Shape> receiver = MakeMesh(rix, 3, RP, 3, NULL);
	Reference<Material> matte = MakeMatte();
	ParamSet lightParams;
	Light *light = CreateAreaLight(Transform(), lightParams,
		lightMesh);
	vector<Reference<Primitive> > objects;
	objects.push_back(new GeometricPrimitive(lightMesh, matte,
		static_cast<AreaLight *>(light)));
	objects.push_back(new GeometricPrimitive(receiver, matte, NULL));
	vector<Light *> lights;
	lights.push_back(light);
	ParamSet accelParams;
	Primitive *accel = CreateGridAccelerator(objects, accelParams);
	ParamSet integParams;
	string strategy = "all";
	integParams.AddString("strategy", &strategy);
	Scene *scene = new Scene(NULL,
		CreateDirectIntegrator(integParams),
		new TaskEmptyVolume(), new TaskFixedSampler(),
		accel, lights, NULL);
	// One sample layout, filled once; every frame reuses it so only
	// the shutter time differs between renders.
	Sample *sample = new Sample(scene->surfaceIntegrator,
		scene->volumeIntegrator, scene);
	for (u_int i = 0; i < sample->n1D.size(); ++i)
		for (u_int j = 0; j < sample->n1D[i]; ++j)
			sample->oneD[i][j] = 0.5f;
	for (u_int i = 0; i < sample->n2D.size(); ++i)
		for (u_int j = 0; j < 2 * sample->n2D[i]; ++j)
			sample->twoD[i][j] = 0.5f;
	// Simple perspective camera basis.
	Point eye(2, -6, 4);
	Point target(2, 0, 1);
	Vector forward = Normalize(target - eye);
	Vector right = Normalize(Cross(forward, Vector(0, 0, 1)));
	Vector up = Cross(right, forward);
	const float tanHalfFov = std::tan(0.5f * 0.9f);
	unsigned char *image = new unsigned char[3 * W * H * FRAMES];
	const float times[FRAMES] = { 0.f, 0.5f, 1.f };
	for (int frame = 0; frame < FRAMES; ++frame) {
		for (int y = 0; y < H; ++y) {
			for (int x = 0; x < W; ++x) {
				float sx = 2.f * (x + 0.5f) / W - 1.f;
				float sy = 1.f - 2.f * (y + 0.5f) / H;
				Vector d = Normalize(forward +
					sx * tanHalfFov * right +
					sy * tanHalfFov * up);
				RayDifferential ray(Ray(eye, d, 0.f, 1e30f,
					times[frame]));
				float alpha = 0.f;
				Spectrum L = scene->Li(ray, sample, &alpha);
				float xyz[3];
				L.XYZ(xyz);
				// Same XYZ->sRGB weights as film/image.cpp.
				float rgb[3] = {
					3.240479f * xyz[0] - 1.537150f * xyz[1] -
						0.498535f * xyz[2],
					-0.969256f * xyz[0] + 1.875991f * xyz[1] +
						0.041556f * xyz[2],
					0.055648f * xyz[0] - 0.204043f * xyz[1] +
						1.057311f * xyz[2] };
				int px = frame * W + x;
				unsigned char *out =
					image + 3 * (y * W * FRAMES + px);
				out[0] = ToByte(rgb[0]);
				out[1] = ToByte(rgb[1]);
				out[2] = ToByte(rgb[2]);
				BSDF::FreeAll();
			}
		}
	}
	string outName = string(argv[1]) + "/motion.ppm";
	FILE *fp = std::fopen(outName.c_str(), "wb");
	if (!fp) {
		std::fprintf(stderr, "cannot open %s\n", outName.c_str());
		return 1;
	}
	std::fprintf(fp, "P6\n%d %d\n255\n", W * FRAMES, H);
	std::fwrite(image, 1, 3 * W * H * FRAMES, fp);
	std::fclose(fp);
	delete[] image;
	delete sample;
	delete scene;
	std::printf("wrote %s (three shutter times 0, 0.5, 1)\n",
		outName.c_str());
	return 0;
}
