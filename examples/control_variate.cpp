// Control-variate diffuse preview example.
//
// Trains a ControlVariate::Session over a couple of epochs on a small scene
// (Lambertian floor, a finite emissive triangle lamp and a Lambertian occluder
// sphere) under the linear sky, then renders a tiny PPM with the trained model.
//
// Build (through task_support/run.sh target) or manually:
//   c++ -std=c++20 -I. -Iinclude/luz examples/control_variate.cpp \
//       <luz_core objects> -o control_variate
#include "Renderer/ControlVariate.hpp"

#include "Hittables/Sphere.hpp"
#include "Hittables/Triangle.hpp"
#include "Materials/Emissive.hpp"
#include "Materials/Lambertian.hpp"
#include "Scene/Scene.hpp"
#include "SkyTypes.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace
{
	constexpr int	IMAGE_WIDTH = 96;
	constexpr int	IMAGE_HEIGHT = 60;
	constexpr int	SAMPLES_PER_EPOCH = 256;
	constexpr int	TRAINING_EPOCHS = 4;

	void	buildScene(Scene& scene)
	{
		scene.setRenderSky(SKY_LINEAR);
		scene.setBackgroundColor(Color(0.02, 0.02, 0.03));
		scene.setMaxLightBounces(4);

		auto floorMaterial = std::make_shared<Lambertian>(Color(0.72, 0.70, 0.66));
		auto wallMaterial = std::make_shared<Lambertian>(Color(0.55, 0.28, 0.22));
		auto occluderMaterial = std::make_shared<Lambertian>(Color(0.20, 0.32, 0.48));
		auto lampMaterial = std::make_shared<Emissive>(Color(9.0, 7.2, 4.8));

		// Floor: a large triangle whose geometric normal points up (+y).
		scene.addHittable(std::make_shared<Triangle>(
			Vector3(-40.0, 0.0, -40.0),
			Vector3(0.0, 0.0, 40.0),
			Vector3(40.0, 0.0, -40.0),
			floorMaterial
		));

		// Back wall, geometric normal facing the camera side.
		scene.addHittable(std::make_shared<Triangle>(
			Vector3(-12.0, 0.0, -6.0),
			Vector3(12.0, 0.0, -6.0),
			Vector3(12.0, 14.0, -6.0),
			wallMaterial
		));
		scene.addHittable(std::make_shared<Triangle>(
			Vector3(-12.0, 0.0, -6.0),
			Vector3(12.0, 14.0, -6.0),
			Vector3(-12.0, 14.0, -6.0),
			wallMaterial
		));

		// Lambertian occluder.
		scene.addHittable(std::make_shared<Sphere>(
			Vector3(1.4, 1.3, 0.5),
			1.3,
			occluderMaterial
		));

		// Finite emissive lamp: a triangle hanging above the scene, facing down.
		scene.addHittable(std::make_shared<Triangle>(
			Vector3(-1.6, 4.6, 1.2),
			Vector3(-1.6, 4.6, -1.2),
			Vector3(1.8, 4.6, 0.0),
			lampMaterial
		));

		scene.updateLights();
	}

	void	writePpm(const std::string& path, const std::vector<Color>& pixels)
	{
		std::ofstream output(path, std::ios::binary);

		output << "P6\n" << IMAGE_WIDTH << " " << IMAGE_HEIGHT << "\n255\n";
		for (const Color& pixel : pixels)
		{
			auto encode = [](double value)
			{
				const double mapped = 1.0 - std::exp(-std::max(0.0, value));
				return (static_cast<unsigned char>(std::lround(std::min(1.0, mapped) * 255.0)));
			};
			const unsigned char bytes[3] = {
				encode(pixel.getRed()),
				encode(pixel.getGreen()),
				encode(pixel.getBlue())
			};
			output.write(reinterpret_cast<const char*>(bytes), 3);
		}
	}

	Ray	cameraRay(int x, int y)
	{
		const double u = (static_cast<double>(x) + 0.5) / IMAGE_WIDTH;
		const double v = (static_cast<double>(y) + 0.5) / IMAGE_HEIGHT;
		const Vector3 target(
			-6.0 + 13.0 * u,
			4.2 - 4.4 * v,
			6.5
		);
		return (Ray(Vector3(0.0, 1.9, 9.5), target - Vector3(0.0, 1.9, 9.5)));
	}
}

int	main(int argc, char** argv)
{
	const std::string outputPath = argc > 1 ? argv[1] : "control_variate.ppm";
	Scene scene;
	buildScene(scene);

	ControlVariate::Config config;
	config.maxDepth = 4;
	config.seed = 12345;
	config.ridge = 1e-4;
	config.minSamples = 16;
	config.enabled = true;

	ControlVariate::Session session(scene, config);
	std::size_t trainedVertices = 0;

	// Training pass: a deterministic sweep of primary rays per epoch.
	for (int epoch = 0; epoch < TRAINING_EPOCHS; epoch++)
	{
		std::uint64_t sampleId = 0;
		for (int sample = 0; sample < SAMPLES_PER_EPOCH; sample++)
		{
			const int x = (sample * 7) % IMAGE_WIDTH;
			const int y = (sample * 13) % IMAGE_HEIGHT;
			session.sample(cameraRay(x, y), sampleId++);
		}
		trainedVertices += session.epochVertexCount();
		session.finishEpoch();
	}

	std::cout
		<< "ControlVariate example: epoch=" << session.epoch()
		<< " models=" << session.modelEntries().size()
		<< " trainedVertices=" << trainedVertices
		<< std::endl;

	// Render pass with the frozen model from the last finished epoch.
	std::vector<Color> pixels;
	pixels.reserve(static_cast<std::size_t>(IMAGE_WIDTH) * IMAGE_HEIGHT);

	for (int y = 0; y < IMAGE_HEIGHT; y++)
	{
		for (int x = 0; x < IMAGE_WIDTH; x++)
		{
			Color accumulated(0.0, 0.0, 0.0);
			constexpr int RENDER_SAMPLES = 4;

			for (int sample = 0; sample < RENDER_SAMPLES; sample++)
			{
				const std::uint64_t sampleId =
					static_cast<std::uint64_t>(y) * IMAGE_WIDTH * RENDER_SAMPLES
					+ static_cast<std::uint64_t>(x) * RENDER_SAMPLES
					+ static_cast<std::uint64_t>(sample);
				accumulated += session.sample(cameraRay(x, y), sampleId);
			}
			pixels.push_back(accumulated / static_cast<double>(RENDER_SAMPLES));
		}
	}

	writePpm(outputPath, pixels);
	std::cout << "Wrote " << outputPath << " (" << IMAGE_WIDTH << "x" << IMAGE_HEIGHT << ")" << std::endl;
	return (0);
}
