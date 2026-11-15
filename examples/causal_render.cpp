// Causal (finite light speed) rendering example.
//
// An emissive quad slides horizontally above a ground receiver while a thin
// static wall stands between them.  At the camera observation time the light
// has already crossed to the far side of the wall, but photons reaching the
// ground were emitted earlier - while the light was still on the near side -
// so the far side of the receiver stays dark.  That delayed visibility change
// only appears when light travels at a finite speed.
//
// Usage: causal_render <output directory>   (writes <dir>/causal.ppm)

#include "src/render.h"
#include "src/causal.h"
#include "src/util.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <cmath>

namespace
{
	Mesh* MakeQuad(const Vec3& p0, const Vec3& p1, const Vec3& p2, const Vec3& p3)
	{
		Mesh* m = new Mesh();
		m->positions.push_back(p0);
		m->positions.push_back(p1);
		m->positions.push_back(p2);
		m->positions.push_back(p3);
		m->indices.push_back(0); m->indices.push_back(1); m->indices.push_back(2);
		m->indices.push_back(2); m->indices.push_back(3); m->indices.push_back(0);
		m->CalculateNormals();
		m->RebuildBVH();
		return m;
	}

	void WritePPM(const char* path, const Color* pixels, int width, int height)
	{
		FILE* f = std::fopen(path, "wb");
		if (!f)
		{
			std::fprintf(stderr, "failed to open %s\n", path);
			std::exit(1);
		}

		std::fprintf(f, "P6\n%d %d\n255\n", width, height);

		for (int i = 0; i < width*height; ++i)
		{
			unsigned char rgb[3];
			for (int k = 0; k < 3; ++k)
			{
				float v = pixels[i][k];
				v = Clamp(v, 0.0f, 1.0f);
				v = powf(v, 1.0f / 2.2f);
				rgb[k] = (unsigned char)(v*255.0f + 0.5f);
			}
			std::fwrite(rgb, 1, 3, f);
		}

		std::fclose(f);
	}
}

int main(int argc, char** argv)
{
	if (argc < 2)
	{
		std::fprintf(stderr, "usage: %s <output directory>\n", argv[0]);
		return 1;
	}

	const float c = 6.0f;

	Scene scene;
	scene.sky.horizon = scene.sky.zenith = Vec3(0.0f);

	// static ground receiver
	Mesh* ground = MakeQuad(Vec3(-6, 0, -6), Vec3(6, 0, -6),
	                        Vec3(6, 0, 6), Vec3(-6, 0, 6));

	// static occluder wall at x=0
	Mesh* wall = MakeQuad(Vec3(0, 0, -1.5f), Vec3(0, 0, 1.5f),
	                      Vec3(0, 1.6f, 1.5f), Vec3(0, 1.6f, -1.5f));

	// deforming emissive quad: slides in x between t=0 and t=1
	const float half = 0.35f;
	const float y = 2.0f;
	const float x0 = -0.7f;
	const float x1 = 0.7f;

	Mesh* light = new Mesh();
	light->positions.push_back(Vec3(x0-half, y, -half));
	light->positions.push_back(Vec3(x0+half, y, -half));
	light->positions.push_back(Vec3(x0+half, y, half));
	light->positions.push_back(Vec3(x0-half, y, half));
	light->endPositions.push_back(Vec3(x1-half, y, -half));
	light->endPositions.push_back(Vec3(x1+half, y, -half));
	light->endPositions.push_back(Vec3(x1+half, y, half));
	light->endPositions.push_back(Vec3(x1-half, y, half));
	light->indices.push_back(0); light->indices.push_back(1); light->indices.push_back(2);
	light->indices.push_back(2); light->indices.push_back(3); light->indices.push_back(0);
	light->CalculateNormals();
	light->RebuildBVH();

	scene.meshes.push_back(ground);
	scene.meshes.push_back(wall);
	scene.meshes.push_back(light);

	Primitive groundPrim;
	groundPrim.type = eMesh;
	groundPrim.mesh = GeometryFromMesh(ground);
	groundPrim.material.color = Vec3(0.8f);
	scene.AddPrimitive(groundPrim);

	Primitive wallPrim;
	wallPrim.type = eMesh;
	wallPrim.mesh = GeometryFromMesh(wall);
	wallPrim.material.color = Vec3(0.15f);
	scene.AddPrimitive(wallPrim);

	Primitive lightPrim;
	lightPrim.type = eMesh;
	lightPrim.mesh = GeometryFromMesh(light);
	lightPrim.material.emission = Vec3(8.0f);
	lightPrim.lightSamples = 4;
	scene.AddPrimitive(lightPrim);

	scene.EnableCausal(c);
	scene.Build();

	if (!scene.causal || !scene.causal->valid)
	{
		std::fprintf(stderr, "causal scene failed to build\n");
		return 1;
	}

	const int width = 128;
	const int height = 96;

	Options options;
	options.mode = ePathTrace;
	options.width = width;
	options.height = height;
	options.maxDepth = 4;
	options.maxSamples = 1;
	options.clamp = 100.0f;
	options.filter = Filter(eFilterBox, 0.5f);

	Camera camera;
	camera.position = Vec3(0.0f, 3.2f, 4.2f);
	const Vec3 target(0.0f, 0.4f, 0.0f);
	const Vec3 forward = Normalize(target - camera.position);
	const Vec3 axis = SafeNormalize(Cross(Vec3(0, 0, -1), forward));
	const float angle = acosf(Clamp(Dot(Vec3(0, 0, -1), forward), -1.0f, 1.0f));
	camera.rotation = Quat(axis, angle);
	camera.fov = DegToRad(50.0f);

	// single observation time: the light is past the wall, but its photons
	// reaching the ground predate the crossing
	camera.shutterStart = camera.shutterEnd = 0.8f;

	Color* pixels = new Color[width*height];
	for (int i = 0; i < width*height; ++i)
		pixels[i] = Color(0.0f);

	Renderer* renderer = CreateCpuRenderer(&scene);

	// several independent passes, averaged when writing the image
	const int passes = 8;
	for (int k = 0; k < passes; ++k)
		renderer->Render(camera, options, pixels);

	const std::string out = std::string(argv[1]) + "/causal.ppm";

	for (int i = 0; i < width*height; ++i)
		pixels[i] = pixels[i] * (1.0f / passes);

	WritePPM(out.c_str(), pixels, width, height);

	int lit = 0;
	for (int i = 0; i < width*height; ++i)
	{
		if (!std::isfinite(pixels[i].x) || !std::isfinite(pixels[i].y) || !std::isfinite(pixels[i].z))
		{
			std::fprintf(stderr, "non-finite pixel\n");
			return 1;
		}
		if (pixels[i].x > 0.0f || pixels[i].y > 0.0f || pixels[i].z > 0.0f)
			++lit;
	}

	std::printf("CAUSAL_EXAMPLE pixels=%d lit=%d lightSpeed=%.3f observeTime=%.3f\n",
	            width*height, lit, c, camera.shutterStart);

	delete renderer;
	delete[] pixels;
	scene.Clear();

	return lit > 0 ? 0 : 1;
}
