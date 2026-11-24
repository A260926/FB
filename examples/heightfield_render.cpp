// Height field example: renders the same editable surface three times.
//
//   before.ppm   - the surface as originally built
//   after.ppm    - after one vertex is lifted into a tall spike
//   retained.ppm - a snapshot taken before the edit, rendered afterwards to
//                  prove old snapshots keep the old surface alive
//
// Usage: heightfield_render <output directory>
//
// Everything uses the in-repo CpuRenderer and a hand-written PPM writer: no
// window, GPU or third-party downloads are involved.

#include "src/render.h"
#include "src/disney.h"

#include <cstdio>
#include <cmath>
#include <vector>
#include <string>
#include <memory>

namespace {

const int kN = 64;                 // cells per side (power of two)
const float kWidth = 8.0f;
const float kDepth = 8.0f;

std::vector<float> BuildHeights()
{
	std::vector<float> h((kN + 1)*(kN + 1));
	for (int z = 0; z <= kN; ++z)
	{
		for (int x = 0; x <= kN; ++x)
		{
			const float u = float(x) / kN;
			const float v = float(z) / kN;
			const float y = 0.35f*std::sin(u*kPi*3.0f)*std::cos(v*kPi*2.0f)
			              + 0.15f*u - 0.1f*v;
			h[z*(kN + 1) + x] = y;
		}
	}
	return h;
}

void WritePPM(const char* path, const std::vector<Color>& pixels, int width, int height)
{
	FILE* f = std::fopen(path, "wb");
	if (!f)
	{
		std::printf("could not open %s\n", path);
		return;
	}
	std::fprintf(f, "P6\n%d %d\n255\n", width, height);
	for (int i = 0; i < width*height; ++i)
	{
		// simple filmic-ish clamp plus sRGB gamma for display
		auto encode = [](float c)
		{
			c = std::min(1.0f, std::max(0.0f, c));
			c = std::pow(c, 1.0f/2.2f);
			return (unsigned char)(c*255.0f + 0.5f);
		};
		const unsigned char rgb[3] = { encode(pixels[i].x), encode(pixels[i].y), encode(pixels[i].z) };
		std::fwrite(rgb, 1, 3, f);
	}
	std::fclose(f);
}

void RenderScene(const char* outputPath, const HeightField::Snapshot& snapshot, int seed)
{
	Scene scene;

	// the editable height field surface
	Primitive field;
	SetHeightField(field, snapshot);
	field.startTransform = field.endTransform = Transform(Vec3(-kWidth*0.5f, 0.0f, -kDepth*0.5f));
	field.material.color = Vec3(0.75f, 0.72f, 0.65f);
	field.material.specular = 0.05f;
	field.material.roughness = 0.8f;
	scene.AddPrimitive(field);

	// spherical area light floating above the field
	Primitive light;
	light.type = eSphere;
	light.sphere.radius = 0.6f;
	light.startTransform = light.endTransform = Transform(Vec3(0.0f, 4.5f, 0.0f));
	light.material.emission = Vec3(6.0f, 5.5f, 4.5f);
	light.lightSamples = 2;
	scene.AddPrimitive(light);

	// dark floor plane beneath everything
	Primitive plane;
	plane.type = ePlane;
	plane.plane.plane[0] = 0.0f;
	plane.plane.plane[1] = 1.0f;
	plane.plane.plane[2] = 0.0f;
	plane.plane.plane[3] = 1.5f;
	plane.material.color = Vec3(0.25f, 0.27f, 0.3f);
	plane.material.specular = 0.0f;
	scene.AddPrimitive(plane);

	scene.Build();

	Camera camera;
	camera.position = Vec3(0.0f, 2.6f, 7.5f);
	camera.rotation = Quat();
	camera.fov = DegToRad(45.0f);

	Options options{};
	options.width = options.height = 80;
	options.mode = ePathTrace;
	options.maxDepth = 4;
	options.maxSamples = 1;
	options.clamp = 20.0f;
	options.filter = Filter(eFilterBox, 0.0f);

	std::vector<Color> pixels(options.width*options.height, Color(0.0f));
	std::unique_ptr<Renderer> renderer(CreateCpuRenderer(&scene));
	renderer->Render(camera, options, pixels.data());
	WritePPM(outputPath, pixels, options.width, options.height);

	std::printf("wrote %s (seed=%d, version=%d)\n", outputPath, seed, snapshot.Version());

	delete[] scene.bvh.nodes;
	scene.bvh.nodes = NULL;
}

} // namespace

int main(int argc, char** argv)
{
	if (argc < 2)
	{
		std::printf("usage: %s <output directory>\n", argv[0]);
		return 1;
	}

	const std::string dir = argv[1];

	HeightField field;
	const std::vector<float> heights = BuildHeights();
	if (!field.Initialize(kN, kWidth, kDepth, heights.data(), (int)heights.size()))
	{
		std::printf("height field initialization failed\n");
		return 2;
	}

	// pin the original generation before any editing happens
	const HeightField::Snapshot retained = field.TakeSnapshot();
	RenderScene((dir + "/before.ppm").c_str(), field.TakeSnapshot(), 1);

	// lift the central vertex far above its original bounds
	hf::Edit edit{ kN/2, kN/2, 3.2f };
	hf::EditStats stats;
	if (!field.Update(&edit, 1, &stats))
	{
		std::printf("height field edit failed\n");
		return 3;
	}
	std::printf("edit leaves=%d hierarchyNodes=%d version=%d\n",
	            stats.leavesRecomputed, stats.nodesRecomputed, field.Version());

	RenderScene((dir + "/after.ppm").c_str(), field.TakeSnapshot(), 2);

	// the retained snapshot still describes the pre-edit surface
	RenderScene((dir + "/retained.ppm").c_str(), retained, 3);

	return 0;
}
