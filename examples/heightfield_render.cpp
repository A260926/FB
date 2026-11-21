// Height field rendering example.
//
// Renders three scenes with the CpuRenderer and writes binary PPM images:
//   before.ppm   - the original height field snapshot
//   after.ppm    - a new snapshot after lifting a centre vertex
//   retained.ppm - a freshly built primitive still holding the old snapshot
//
// Usage: heightfield_render <output directory>

#include "src/render.h"

#include <cmath>
#include <cstdio>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

namespace
{
	float FieldHeight(float x, float z)
	{
		return 0.35f*std::sin(1.3f*x) + 0.25f*std::cos(1.1f*z) + 0.1f*x*z*0.15f;
	}

	// Quaternion that rotates the default camera forward (-Z) onto target-from.
	Quat LookAtQuat(const Vec3& from, const Vec3& target)
	{
		const Vec3 forward(0.0f, 0.0f, -1.0f);
		Vec3 dir = Normalize(target - from);

		const float dot = Clamp(Dot(forward, dir), -1.0f, 1.0f);
		if (dot > 0.99999f)
			return Quat();
		if (dot < -0.99999f)
			return Quat(Vec3(1,0,0), kPi);

		const Vec3 axis = Normalize(Cross(forward, dir));
		const float angle = std::acos(dot);
		return Quat(axis, angle);
	}

	void WritePPM(const std::string& path, int width, int height, const Color* pixels)
	{
		std::ofstream file(path.c_str(), std::ios::binary);

		file << "P6\n" << width << " " << height << "\n255\n";
		for (int i=0; i < width*height; ++i)
		{
			const Color& c = pixels[i];
			const unsigned char rgb[3] =
			{
				(unsigned char)Clamp(int(c.x*255.0f), 0, 255),
				(unsigned char)Clamp(int(c.y*255.0f), 0, 255),
				(unsigned char)Clamp(int(c.z*255.0f), 0, 255),
			};
			file.write(reinterpret_cast<const char*>(rgb), 3);
		}
	}

	Primitive MakeFieldPrimitive(const HeightFieldSnapshot& snapshot)
	{
		Primitive primitive;
		primitive.type = eHeightField;
		primitive.heightfield.snapshot =
			std::make_shared<HeightFieldSnapshot>(snapshot);
		primitive.startTransform = Transform();
		primitive.endTransform = Transform();
		primitive.material.color = Vec3(0.35f, 0.55f, 0.25f);
		primitive.material.metallic = 0.0f;
		primitive.material.specular = 0.2f;
		primitive.material.roughness = 0.7f;
		primitive.lightSamples = 0;
		return primitive;
	}

	Primitive MakeSphereLight()
	{
		Primitive light;
		light.type = eSphere;
		light.sphere.radius = 0.6f;
		const Transform transform(Vec3(0.0f, 5.0f, 0.0f));
		light.startTransform = transform;
		light.endTransform = transform;
		light.material.emission = Vec3(6.0f, 5.5f, 4.5f);
		light.lightSamples = 4;
		return light;
	}

	void RenderScene(const Scene& scene, const std::string& path)
	{
		const int width = 96;
		const int height = 96;

		Camera camera;
		camera.position = Vec3(7.5f, 5.5f, 9.0f);
		camera.rotation = LookAtQuat(camera.position, Vec3(3.0f, 0.0f, 3.0f));
		camera.fov = DegToRad(45.0f);

		Options options;
		options.width = width;
		options.height = height;
		options.mode = ePathTrace;
		options.maxDepth = 3;
		options.maxSamples = 1;
		options.clamp = 20.0f;
		options.filter = Filter(eFilterBox, 0.0f);

		std::vector<Color> pixels(width*height, Color(0.0f));
		std::unique_ptr<Renderer> renderer(CreateCpuRenderer(&scene));
		renderer->Render(camera, options, pixels.data());

		WritePPM(path, width, height, pixels.data());
		std::printf("wrote %s (%dx%d)\n", path.c_str(), width, height);
	}
}

int main(int argc, char** argv)
{
	if (argc < 2)
	{
		std::fprintf(stderr, "usage: %s <output directory>\n", argv[0]);
		return 1;
	}

	const std::string outputDir = argv[1];

	const int n = 64;
	std::vector<float> heights((n+1)*(n+1));
	for (int z=0; z <= n; ++z)
		for (int x=0; x <= n; ++x)
			heights[z*(n+1)+x] = FieldHeight(6.0f*x/n, 6.0f*z/n);

	HeightField field(n, 6.0f, 6.0f, heights.data());

	// retain the original snapshot independently of later edits
	const HeightFieldSnapshot beforeSnapshot = field.Snapshot();

	// edit: lift the centre vertex well above the original surface bounds
	HeightUpdateStats stats;
	const HeightEdit lift(n/2, n/2, 2.5f);
	if (!field.Update(&lift, 1, stats))
	{
		std::fprintf(stderr, "height field edit failed\n");
		return 2;
	}
	std::printf("edit leaves=%d nodes=%d version=%d\n",
		stats.leavesRecomputed, stats.nodesRecomputed, field.Version());

	const HeightFieldSnapshot afterSnapshot = field.Snapshot();

	// before: original geometry
	{
		Scene scene;
		scene.AddPrimitive(MakeFieldPrimitive(beforeSnapshot));
		scene.AddPrimitive(MakeSphereLight());
		scene.Build();
		RenderScene(scene, outputDir + "/before.ppm");
		delete[] scene.bvh.nodes;
		scene.bvh.nodes = NULL;
	}

	// after: edited geometry
	{
		Scene scene;
		scene.AddPrimitive(MakeFieldPrimitive(afterSnapshot));
		scene.AddPrimitive(MakeSphereLight());
		scene.Build();
		RenderScene(scene, outputDir + "/after.ppm");
		delete[] scene.bvh.nodes;
		scene.bvh.nodes = NULL;
	}

	// retained: a brand new primitive still pins the old snapshot version
	{
		Scene scene;
		scene.AddPrimitive(MakeFieldPrimitive(beforeSnapshot));
		scene.AddPrimitive(MakeSphereLight());
		scene.Build();
		RenderScene(scene, outputDir + "/retained.ppm");
		delete[] scene.bvh.nodes;
		scene.bvh.nodes = NULL;
	}

	std::printf("snapshots: before v%d, after v%d, retained stays v%d\n",
		beforeSnapshot.Version(), afterSnapshot.Version(), beforeSnapshot.Version());
	return 0;
}
