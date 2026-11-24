#include "Renderer/ControlVariate.hpp"
#include "Color.hpp"
#include "Hittables/Sphere.hpp"
#include "Hittables/Triangle.hpp"
#include "Materials/Emissive.hpp"
#include "Materials/Lambertian.hpp"
#include "Ray/Ray.hpp"
#include "Scene/Scene.hpp"
#include "SkyTypes.hpp"
#include "Utilities.hpp"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
	using ControlVariate::Configuration;
	using ControlVariate::ControlVariateSession;

	constexpr std::size_t	IMAGE_WIDTH = 64;
	constexpr std::size_t	IMAGE_HEIGHT = 48;
	constexpr std::size_t	SAMPLES_PER_PIXEL = 256;
	constexpr std::size_t	EPOCH_COUNT = 3;

	Ray	cameraRay(std::size_t x, std::size_t y)
	{
		const double aspect = static_cast<double>(IMAGE_WIDTH) / static_cast<double>(IMAGE_HEIGHT);
		const double verticalFov = 1.05; // ~60 degrees
		const double halfHeight = std::tan(verticalFov / 2.0);
		const double halfWidth = halfHeight * aspect;
		const double u = (static_cast<double>(x) + 0.5) / static_cast<double>(IMAGE_WIDTH);
		const double v = (static_cast<double>(y) + 0.5) / static_cast<double>(IMAGE_HEIGHT);
		const Vector3 direction = Utilities::normalize(Vector3(
			(2.0 * u - 1.0) * halfWidth,
			(1.0 - (2.0 * v)) * halfHeight - 0.25,
			-1.0
		));
		return (Ray(Vector3(0.0, 0.0, 0.0), direction));
	}

	void	writePPM(const std::string& fileName, const std::vector<Color>& pixels)
	{
		std::ofstream stream(fileName, std::ios::binary);
		if (!stream)
		{
			throw std::runtime_error("Could not open output PPM: " + fileName);
		}
		stream << "P6\n" << IMAGE_WIDTH << " " << IMAGE_HEIGHT << "\n255\n";
		for (const Color& pixel : pixels)
		{
			auto encode = [](double value) {
				const double gammaCorrected = std::pow(std::max(0.0, value), 1.0 / 2.2);
				return (static_cast<unsigned char>(
					std::clamp(255.0 * gammaCorrected, 0.0, 255.0)
				));
			};
			const unsigned char rgb[3] = {
				encode(pixel.getRed()),
				encode(pixel.getGreen()),
				encode(pixel.getBlue())
			};
			stream.write(reinterpret_cast<const char*>(rgb), 3);
		}
	}

	void	buildScene(Scene& scene)
	{
		scene.setRenderSky(SKY_LINEAR);
		scene.setMaxLightBounces(4);

		auto floorMaterial = std::make_shared<Lambertian>(Color(0.75, 0.75, 0.7));
		scene.addHittable(std::make_shared<Triangle>(
			Vector3(-8, -1, -1),
			Vector3(8, -1, -1),
			Vector3(0, -1, -40),
			floorMaterial
		));
		auto sphereMaterial = std::make_shared<Lambertian>(Color(0.85, 0.35, 0.25));
		scene.addHittable(std::make_shared<Sphere>(
			Vector3(-0.9, -0.45, -3.2), 0.55, sphereMaterial
		));
		auto otherMaterial = std::make_shared<Lambertian>(Color(0.3, 0.55, 0.9));
		scene.addHittable(std::make_shared<Sphere>(
			Vector3(1.0, -0.55, -3.8), 0.45, otherMaterial
		));
		auto lampMaterial = std::make_shared<Emissive>(Color(2.5, 2.2, 1.8));
		scene.addHittable(std::make_shared<Triangle>(
			Vector3(-0.7, 1.4, -3.0),
			Vector3(0.7, 1.4, -3.0),
			Vector3(0.0, 2.2, -3.0),
			lampMaterial
		));
	}
}

int	main(int argc, char** argv)
{
	const std::string outputPath = argc > 1 ? argv[1] : "control_variate.ppm";
	Scene scene;
	buildScene(scene);
	// The session keeps using the scene's ordinary acceleration structure.
	scene.updateAccelerationStructure();

	Configuration configuration;
	configuration.maxDepth = 3;
	configuration.seed = 20261124;
	configuration.ridge = 1.0e-4;
	configuration.minSamples = 64;
	configuration.enabled = true;
	ControlVariateSession session(scene, configuration);

	std::vector<Color> pixels(IMAGE_WIDTH * IMAGE_HEIGHT);
	for (std::size_t epoch = 0; epoch < EPOCH_COUNT; epoch++)
	{
		for (std::size_t y = 0; y < IMAGE_HEIGHT; y++)
		{
			for (std::size_t x = 0; x < IMAGE_WIDTH; x++)
			{
				Color accumulator(0.0, 0.0, 0.0);
				for (std::size_t sample = 0; sample < SAMPLES_PER_PIXEL; sample++)
				{
					const std::uint64_t sampleId =
						((static_cast<std::uint64_t>(y) * IMAGE_WIDTH) + x) * SAMPLES_PER_PIXEL
						+ sample;
					accumulator += session.sample(cameraRay(x, y), sampleId);
				}
				pixels[y * IMAGE_WIDTH + x] = accumulator / static_cast<double>(SAMPLES_PER_PIXEL);
			}
		}

		const std::size_t activeModels = std::count_if(
			session.getModels().begin(),
			session.getModels().end(),
			[](const auto& entry) { return (entry.second.active); }
		);
		std::cout << "epoch " << epoch
			<< " samples=" << (IMAGE_WIDTH * IMAGE_HEIGHT * SAMPLES_PER_PIXEL)
			<< " frozen_models=" << session.getModels().size()
			<< " active=" << activeModels << "\n";
		session.finishEpoch();
	}

	writePPM(outputPath, pixels);
	std::cout << "wrote " << outputPath
		<< " (" << IMAGE_WIDTH << "x" << IMAGE_HEIGHT
		<< ", final epoch=" << session.getEpoch() << ")\n";
	return (0);
}
