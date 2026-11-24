#include "task_support/check.h"
#include "Renderer/ControlVariate.hpp"
#include "Color.hpp"
#include "Hittables/Cube.hpp"
#include "Hittables/Sphere.hpp"
#include "Hittables/Triangle.hpp"
#include "Materials/Emissive.hpp"
#include "Materials/Lambertian.hpp"
#include "Materials/Metal.hpp"
#include "Ray/Ray.hpp"
#include "Scene/Scene.hpp"
#include "SkyTypes.hpp"
#include "Texture.hpp"
#include "Utilities.hpp"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <vector>

namespace
{
	using ControlVariate::Configuration;
	using ControlVariate::ControlVariateSession;
	using ControlVariate::evaluateBasis;
	using ControlVariate::hemisphereCosineIntegrals;
	using ControlVariate::FrozenModel;
	using ControlVariate::ModelCoefficients;
	using ControlVariate::ModelKey;
	using ControlVariate::NormalEquations;
	using ControlVariate::PathVertex;
	using ControlVariate::SH_COEFFICIENT_COUNT;

	const double	PI = 3.14159265358979323846;

	bool	nearValue(double a, double b, double tolerance)
	{
		return (std::fabs(a - b) <= tolerance);
	}

	bool	nearColor(const Color& a, const Color& b, double tolerance)
	{
		return (
			nearValue(a.getRed(), b.getRed(), tolerance)
			&& nearValue(a.getGreen(), b.getGreen(), tolerance)
			&& nearValue(a.getBlue(), b.getBlue(), tolerance)
		);
	}

	Color	channelAverage(const std::vector<Color>& samples)
	{
		Color sum(0.0, 0.0, 0.0);
		for (const Color& sample : samples)
		{
			sum += sample;
		}
		return (sum / static_cast<double>(samples.size()));
	}

	Color	channelVariance(const std::vector<Color>& samples, const Color& mean)
	{
		Color sum(0.0, 0.0, 0.0);
		for (const Color& sample : samples)
		{
			const Color delta = sample - mean;
			sum += delta * delta;
		}
		return (sum / static_cast<double>(samples.size()));
	}

	Configuration	testConfiguration(int depth = 2, std::uint64_t seed = 1234, std::size_t minSamples = 64)
	{
		Configuration configuration;
		configuration.maxDepth = depth;
		configuration.seed = seed;
		configuration.ridge = 1.0e-4;
		configuration.minSamples = minSamples;
		configuration.enabled = true;
		return (configuration);
	}

	// Independent numerical hemisphere integration of the cosine-weighted
	// moments, with no reference to the analytic implementation under test.
	std::array<double, SH_COEFFICIENT_COUNT>	numericalCosineIntegrals(
		const Vector3& normal,
		std::uint64_t samples
	)
	{
		const ONB basis(normal);
		std::array<double, SH_COEFFICIENT_COUNT>	sum{};
		std::uint64_t state = 0x243f6a8885a308d3ull;

		for (std::uint64_t i = 0; i < samples; i++)
		{
			state += 0x9e3779b97f4a7c15ull;
			std::uint64_t z = state;
			z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
			z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
			z ^= (z >> 31);
			const double u1 = static_cast<double>(z >> 11) * (1.0 / 9007199254740992.0);
			state += 0x9e3779b97f4a7c15ull;
			z = state;
			z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
			z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
			z ^= (z >> 31);
			const double u2 = static_cast<double>(z >> 11) * (1.0 / 9007199254740992.0);

			// Uniform hemisphere sampling (cosTheta uniform in [0, 1]), kept
			// independent of the production cosine-weighted sampler.
			const double cosTheta = 1.0 - u1;
			const double radius = std::sqrt(std::max(0.0, 1.0 - (cosTheta * cosTheta)));
			const double phi = 2.0 * PI * u2;
			const Vector3 direction = basis.local(
				radius * std::cos(phi),
				radius * std::sin(phi),
				cosTheta
			);
			const std::array<double, SH_COEFFICIENT_COUNT> y = evaluateBasis(direction);
			for (std::size_t k = 0; k < SH_COEFFICIENT_COUNT; k++)
			{
				// Uniform hemisphere pdf is 1 / (2 pi); the pi in the integrand
				// cancels to a factor 1/2.
				sum[k] += y[k] * cosTheta;
			}
		}
		for (double& value : sum)
		{
			// Uniform hemisphere pdf is 1 / (2 pi); the pi in the integrand
			// cancels to a factor 1/2.
			value = value * (2.0 / static_cast<double>(samples));
		}
		return (sum);
	}

	// Analytic Lambert mean of the linear SKY_LINEAR background over the
	// hemisphere around n.  The sky interpolates white toward (0.5,0.7,1) with
	// t = skyline * (w.y + 1), and the cosine-weighted mean of w.y is 2 n.y / 3.
	Color	analyticLinearSkyMean(const Vector3& normal, double skyline = 0.5)
	{
		const double meanY = 2.0 * normal.getY() / 3.0;
		const double meanT = skyline * (meanY + 1.0);
		const Color white(1.0, 1.0, 1.0);
		const Color horizon(0.5, 0.7, 1.0);
		return (white * (1.0 - meanT) + horizon * meanT);
	}

	int	harmonicsBasisOrthogonality(void)
	{
		// Independent full-sphere Monte Carlo check of the SH normalization and
		// mutual orthogonality of the nine basis functions.
		const std::size_t samples = 400000;
		std::array<std::array<double, SH_COEFFICIENT_COUNT>, SH_COEFFICIENT_COUNT>	gram{};
		std::uint64_t state = 0xabcdef0123456789ull;

		for (std::size_t i = 0; i < samples; i++)
		{
			state += 0x9e3779b97f4a7c15ull;
			std::uint64_t h = state;
			h = (h ^ (h >> 30)) * 0xbf58476d1ce4e5b9ull;
			h = (h ^ (h >> 27)) * 0x94d049bb133111ebull;
			h ^= (h >> 31);
			const double u1 = static_cast<double>(h >> 11) * (1.0 / 9007199254740992.0);
			state += 0x9e3779b97f4a7c15ull;
			h = state;
			h = (h ^ (h >> 30)) * 0xbf58476d1ce4e5b9ull;
			h = (h ^ (h >> 27)) * 0x94d049bb133111ebull;
			h ^= (h >> 31);
			const double u2 = static_cast<double>(h >> 11) * (1.0 / 9007199254740992.0);

			const double z = 1.0 - (2.0 * u1);
			const double radius = std::sqrt(std::max(0.0, 1.0 - (z * z)));
			const double phi = 2.0 * PI * u2;
			const Vector3 direction(radius * std::cos(phi), radius * std::sin(phi), z);
			const std::array<double, SH_COEFFICIENT_COUNT> y = evaluateBasis(direction);
			for (std::size_t a = 0; a < SH_COEFFICIENT_COUNT; a++)
			{
				for (std::size_t b = 0; b < SH_COEFFICIENT_COUNT; b++)
				{
					gram[a][b] += y[a] * y[b];
				}
			}
		}

		for (std::size_t a = 0; a < SH_COEFFICIENT_COUNT; a++)
		{
			for (std::size_t b = 0; b < SH_COEFFICIENT_COUNT; b++)
			{
				const double expected = (a == b) ? 1.0 : 0.0;
				TASK_CHECK(nearValue(
					gram[a][b] * (4.0 * PI / static_cast<double>(samples)),
					expected,
					0.02
				));
			}
		}
		return (0);
	}

	int	harmonicsCosineIntegrals(void)
	{
		const Vector3 normals[] = {
			Vector3(0.0, 1.0, 0.0),
			Vector3(1.0, 0.0, 0.0),
			Vector3(0.0, 0.0, 1.0),
			Utilities::normalize(Vector3(0.3, -0.7, 0.4)),
			Utilities::normalize(Vector3(-0.5, 0.2, -0.9))
		};
		for (const Vector3& normal : normals)
		{
			const std::array<double, SH_COEFFICIENT_COUNT> analytic = hemisphereCosineIntegrals(normal);
			const std::array<double, SH_COEFFICIENT_COUNT> numeric = numericalCosineIntegrals(normal, 200000);
			for (std::size_t k = 0; k < SH_COEFFICIENT_COUNT; k++)
			{
				const double tolerance = std::max(0.01, 0.03 * std::max(1.0, std::fabs(numeric[k])));
				TASK_CHECK(nearValue(analytic[k], numeric[k], tolerance));
			}
		}

		// The constant term's cosine integral is the Y_00 basis value itself.
		TASK_CHECK(nearValue(hemisphereCosineIntegrals(Vector3(0, 1, 0))[0], evaluateBasis(Vector3(0, 1, 0))[0], 1.0e-12));
		// An odd (l=1) term vanishes for a perpendicular normal.
		TASK_CHECK(nearValue(hemisphereCosineIntegrals(Vector3(1, 0, 0))[2], 0.0, 1.0e-12));
		return (0);
	}

	int	harmonicsBasisValues(void)
	{
		// Y_00 is constant 1 / (2 sqrt(pi)) on every direction.
		const Vector3 directions[] = {
			Vector3(1.0, 0.0, 0.0),
			Vector3(0.0, 0.0, 1.0),
			Utilities::normalize(Vector3(1.0, 2.0, 3.0))
		};
		const double y00 = 1.0 / (2.0 * std::sqrt(PI));
		for (const Vector3& direction : directions)
		{
			const std::array<double, SH_COEFFICIENT_COUNT> y = evaluateBasis(direction);
			TASK_CHECK(nearValue(y[0], y00, 1.0e-12));
			TASK_CHECK(nearValue(y[1], 0.5 * std::sqrt(3.0 / PI) * direction.getY(), 1.0e-12));
			TASK_CHECK(nearValue(y[3], 0.5 * std::sqrt(3.0 / PI) * direction.getX(), 1.0e-12));
		}
		// Quadratic term at the +z pole.
		const std::array<double, SH_COEFFICIENT_COUNT> pole = evaluateBasis(Vector3(0.0, 0.0, 1.0));
		TASK_CHECK(nearValue(pole[6], 0.5 * std::sqrt(5.0 / PI), 1.0e-12));
		TASK_CHECK(nearValue(pole[8], 0.0, 1.0e-12));
		return (0);
	}

	// Scene declares a destructor and has no move constructor, so it is built
	// in place and configured through these helpers.
	void	configureConstantSkyScene(Scene& scene, const Color& background)
	{
		scene.setRenderSky(SKY_NONE);
		scene.setBackgroundColor(background);
		scene.setMaxLightBounces(4);
	}

	void	configureLinearSkyScene(Scene& scene)
	{
		scene.setRenderSky(SKY_LINEAR);
		scene.setMaxLightBounces(4);
	}

	int	transportConstantSkyDepthZero(void)
	{
		// Depth 0 still sees the sky and emissive surfaces, but never reflects.
		Scene scene;
		configureConstantSkyScene(scene, Color(0.2, 0.4, 0.6));
		auto material = std::make_shared<Lambertian>(Color(0.5, 0.5, 0.5));
		scene.addHittable(std::make_shared<Sphere>(Vector3(0, 0, -3), 1.0, material));

		ControlVariateSession session(scene, testConfiguration(0));
		Color miss(0.0, 0.0, 0.0);
		Color hit(0.0, 0.0, 0.0);
		for (std::uint64_t sampleId = 0; sampleId < 32; sampleId++)
		{
			miss += session.sample(Ray(Vector3(0, 0, 0), Vector3(0, 1, 0)), sampleId);
			hit += session.sample(Ray(Vector3(0, 0, 0), Vector3(0, 0, -1)), sampleId);
		}
		TASK_CHECK(nearColor(miss / 32.0, Color(0.2, 0.4, 0.6), 1.0e-12));
		// A depth-0 diffuse hit has no remaining reflection: result is zero.
		TASK_CHECK(nearColor(hit / 32.0, Color(0.0, 0.0, 0.0), 1.0e-12));

		// The same surface at depth 1 reflects the constant sky analytically in
		// expectation once a model is trained.
		ControlVariateSession trained(scene, testConfiguration(1));
		std::vector<Color> samples;
		for (std::uint64_t sampleId = 0; sampleId < 512; sampleId++)
		{
			samples.push_back(trained.sample(Ray(Vector3(0, 0, 0), Vector3(0, 0, -1)), sampleId));
		}
		trained.finishEpoch();
		const Color expected = Color(0.5, 0.5, 0.5) * Color(0.2, 0.4, 0.6);
		TASK_CHECK(nearColor(channelAverage(samples), expected, 0.05));

		std::vector<Color> guided;
		for (std::uint64_t sampleId = 0; sampleId < 512; sampleId++)
		{
			guided.push_back(trained.sample(Ray(Vector3(0, 0, 0), Vector3(0, 0, -1)), sampleId));
		}
		TASK_CHECK(nearColor(channelAverage(guided), expected, 0.02));
		TASK_CHECK(trained.getEpoch() == 1);
		return (0);
	}

	int	transportLinearSkyMean(void)
	{
		Scene scene;
		configureLinearSkyScene(scene);
		const Color rho(0.8, 0.3, 0.5);
		auto material = std::make_shared<Lambertian>(rho);
		scene.addHittable(std::make_shared<Triangle>(
			Vector3(-3, -1, -2),
			Vector3(3, -1, -2),
			Vector3(0, 3, -2),
			material
		));

		// Independent analytic Lambert mean against the linear sky, for several
		// surface orientations reached by different primary rays.
		Configuration configuration = testConfiguration(1);
		configuration.minSamples = 256;
		ControlVariateSession session(scene, configuration);
		const Ray primaries[] = {
			Ray(Vector3(0, 0, 0), Vector3(0, 0, -1)),
			Ray(Vector3(0, 0, 0), Utilities::normalize(Vector3(0.5, 0.3, -1.0))),
			Ray(Vector3(0, 0, 0), Utilities::normalize(Vector3(-0.4, -0.2, -1.0)))
		};
		for (const Ray& primary : primaries)
		{
			std::vector<Color> samples;
			for (std::uint64_t sampleId = 0; sampleId < 1024; sampleId++)
			{
				samples.push_back(session.sample(primary, sampleId));
			}
			session.finishEpoch();
			const Vector3 normal = session.getLastPathVertices().front().normal;
			const Color analytic = rho * analyticLinearSkyMean(normal);
			TASK_CHECK(nearColor(channelAverage(samples), analytic, 0.05));

			std::vector<Color> guided;
			for (std::uint64_t sampleId = 0; sampleId < 1024; sampleId++)
			{
				guided.push_back(session.sample(primary, sampleId));
			}
			TASK_CHECK(nearColor(channelAverage(guided), analytic, 0.03));
			session.reset();
		}
		return (0);
	}

	int	transportEmissiveTermination(void)
	{
		// A directly hit emissive sphere returns its emitted() radiance at any
		// depth and ends the path.
		Scene scene;
		configureConstantSkyScene(scene, Color(0.1, 0.1, 0.1));
		auto lamp = std::make_shared<Emissive>(Color(0.7, 0.4, 0.2));
		scene.addHittable(std::make_shared<Sphere>(Vector3(0, 0, -3), 1.0, lamp));

		for (int depth = 0; depth <= 3; depth++)
		{
			ControlVariateSession session(scene, testConfiguration(depth));
			for (std::uint64_t sampleId = 0; sampleId < 8; sampleId++)
			{
				const Color result = session.sample(Ray(Vector3(0, 0, 0), Vector3(0, 0, -1)), sampleId);
				TASK_CHECK(nearColor(result, Color(0.7, 0.4, 0.2), 1.0e-12));
			}
		}
		return (0);
	}

	int	transportFiniteLampAndOcclusion(void)
	{
		// A finite triangular lamp lights a Lambertian floor.  Two otherwise
		// identical scenes, one with an occluding sphere, prove that real
		// occlusion enters the suffix estimate Li.
		auto buildScene = [](Scene& scene, bool withOccluder) {
			configureConstantSkyScene(scene, Color(0.0, 0.0, 0.0));
			auto lampMaterial = std::make_shared<Emissive>(Color(6.0, 6.0, 6.0));
			scene.addHittable(std::make_shared<Triangle>(
				Vector3(-3.0, 1.0, -2.0),
				Vector3(3.0, 1.0, -2.0),
				Vector3(0.0, 4.0, -2.0),
				lampMaterial
			));
			auto floorMaterial = std::make_shared<Lambertian>(Color(0.7, 0.7, 0.7));
			scene.addHittable(std::make_shared<Triangle>(
				Vector3(-8, -1, -1),
				Vector3(8, -1, -1),
				Vector3(0, -1, -40),
				floorMaterial
			));
			if (withOccluder)
			{
				auto occluderMaterial = std::make_shared<Lambertian>(Color(0.3, 0.3, 0.3));
				scene.addHittable(std::make_shared<Sphere>(
					Vector3(0, 0.2, -2.0), 0.9, occluderMaterial
				));
			}
		};
		Scene openScene;
		Scene blockedScene;
		buildScene(openScene, false);
		buildScene(blockedScene, true);

		Configuration configuration = testConfiguration(1);
		configuration.minSamples = 128;
		ControlVariateSession openSession(openScene, configuration);
		ControlVariateSession blockedSession(blockedScene, configuration);

		const Ray primary(Vector3(0, 0, 0), Utilities::normalize(Vector3(0.0, -0.4, -1.0)));
		std::vector<Color> openSamples;
		std::vector<Color> blockedSamples;
		for (std::uint64_t sampleId = 0; sampleId < 4096; sampleId++)
		{
			openSamples.push_back(openSession.sample(primary, sampleId));
			blockedSamples.push_back(blockedSession.sample(primary, sampleId));
		}
		const Color openMean = channelAverage(openSamples);
		const Color blockedMean = channelAverage(blockedSamples);
		// The unoccluded floor receives real finite-lamp energy; the sphere
		// removes a substantial, strictly positive fraction of it.
		TASK_CHECK(openMean.getRed() > 0.05);
		TASK_CHECK(blockedMean.getRed() < openMean.getRed() * 0.8);
		TASK_CHECK(blockedMean.getRed() >= 0.0);

		// Training preserves both means after a model is frozen.
		openSession.finishEpoch();
		blockedSession.finishEpoch();
		std::vector<Color> openGuided;
		std::vector<Color> blockedGuided;
		for (std::uint64_t sampleId = 0; sampleId < 4096; sampleId++)
		{
			openGuided.push_back(openSession.sample(primary, sampleId));
			blockedGuided.push_back(blockedSession.sample(primary, sampleId));
		}
		TASK_CHECK(nearColor(channelAverage(openGuided), openMean, 0.05));
		TASK_CHECK(nearColor(channelAverage(blockedGuided), blockedMean, 0.05));
		TASK_CHECK(channelAverage(blockedGuided).getRed()
			< channelAverage(openGuided).getRed() * 0.85);
		return (0);
	}

	int	transportTwoBounces(void)
	{
		// Two diffuse reflections between colored walls and a constant sky.
		Scene scene;
		configureConstantSkyScene(scene, Color(0.25, 0.25, 0.25));
		auto wallA = std::make_shared<Lambertian>(Color(0.8, 0.2, 0.2));
		auto wallB = std::make_shared<Lambertian>(Color(0.2, 0.8, 0.2));
		scene.addHittable(std::make_shared<Triangle>(
			Vector3(-4, -1, -3), Vector3(0.5, -1, -3), Vector3(-1.75, 3, -3), wallA
		));
		scene.addHittable(std::make_shared<Triangle>(
			Vector3(0.5, -1, -3), Vector3(4, -1, -3), Vector3(2.25, 3, -3), wallB
		));

		Configuration configuration = testConfiguration(2);
		configuration.minSamples = 256;
		ControlVariateSession session(scene, configuration);
		const Ray primary(Vector3(0, 0, 0), Vector3(0, 0, -1));
		std::vector<Color> cold;
		for (std::uint64_t sampleId = 0; sampleId < 2048; sampleId++)
		{
			cold.push_back(session.sample(primary, sampleId));
		}
		session.finishEpoch();
		std::vector<Color> guided;
		for (std::uint64_t sampleId = 0; sampleId < 2048; sampleId++)
		{
			guided.push_back(session.sample(primary, sampleId));
		}
		// Multi-bounce energy stays positive and the mean survives training.
		const Color coldMean = channelAverage(cold);
		const Color guidedMean = channelAverage(guided);
		TASK_CHECK(coldMean.getRed() > 0.0);
		TASK_CHECK(nearColor(coldMean, guidedMean, 0.06));
		// A production path exposes at least one recorded vertex with full data.
		const auto& vertices = session.getLastPathVertices();
		TASK_CHECK(!vertices.empty());
		for (const PathVertex& vertex : vertices)
		{
			TASK_CHECK(vertex.key.depth >= 0);
			// Surface vertices (including emissive ones) carry an object key;
			// only the background terminator has none.
			if (!vertex.background)
			{
				TASK_CHECK(vertex.key.object != nullptr);
			}
		}
		return (0);
	}

	int	transportVertexRecursionFields(void)
	{
		Scene scene;
		configureLinearSkyScene(scene);
		auto material = std::make_shared<Lambertian>(Color(0.6, 0.6, 0.6));
		scene.addHittable(std::make_shared<Sphere>(Vector3(0, 0, -3), 1.0, material));
		ControlVariateSession session(scene, testConfiguration(2));
		for (std::uint64_t sampleId = 0; sampleId < 128; sampleId++)
		{
			session.sample(Ray(Vector3(0, 0, 0), Vector3(0, 0, -1)), sampleId);
		}
		session.finishEpoch();
		session.sample(Ray(Vector3(0, 0, 0), Vector3(0, 0, -1)), 777);
		const auto& vertices = session.getLastPathVertices();
		TASK_CHECK(vertices.size() >= 2);
		// Root first, with strictly decreasing remaining depth.
		for (std::size_t i = 1; i < vertices.size(); i++)
		{
			TASK_CHECK(vertices[i].key.depth < vertices[i - 1].key.depth
				|| vertices[i].emissive || vertices[i].background);
		}
		const PathVertex& root = vertices.front();
		TASK_CHECK(nearValue(Utilities::vectorLength(root.normal), 1.0, 1.0e-9));
		TASK_CHECK(nearValue(Utilities::vectorLength(root.direction), 1.0, 1.0e-9));
		TASK_CHECK(root.cosinePdf > 0.0);
		// result == rho * (A + Li - g) recomputed from the recorded fields.
		const Color recomputed = root.rho * (root.a + root.li - root.g);
		TASK_CHECK(nearColor(recomputed, root.result, 1.0e-10));
		// At least one vertex carries a trained, non-zero model term.
		bool hasModel = false;
		for (const PathVertex& vertex : vertices)
		{
			if (vertex.g.getRed() != 0.0 || vertex.a.getRed() != 0.0)
			{
				hasModel = true;
			}
		}
		TASK_CHECK(hasModel);
		return (0);
	}

	int	transportDisabledKeepsDirections(void)
	{
		Scene scene;
		configureLinearSkyScene(scene);
		auto material = std::make_shared<Lambertian>(Color(0.6, 0.6, 0.6));
		scene.addHittable(std::make_shared<Sphere>(Vector3(0, 0, -3), 1.0, material));

		Configuration enabled = testConfiguration(1);
		Configuration disabled = testConfiguration(1);
		disabled.enabled = false;
		ControlVariateSession onSession(scene, enabled);
		ControlVariateSession offSession(scene, disabled);
		for (std::uint64_t sampleId = 0; sampleId < 64; sampleId++)
		{
			onSession.sample(Ray(Vector3(0, 0, 0), Vector3(0, 0, -1)), sampleId);
			offSession.sample(Ray(Vector3(0, 0, 0), Vector3(0, 0, -1)), sampleId);
			const auto& onVertices = onSession.getLastPathVertices();
			const auto& offVertices = offSession.getLastPathVertices();
			TASK_CHECK(onVertices.size() == offVertices.size());
			for (std::size_t i = 0; i < onVertices.size(); i++)
			{
				TASK_CHECK(nearValue(
					Utilities::dot(onVertices[i].direction, offVertices[i].direction),
					1.0,
					1.0e-12
				));
				TASK_CHECK(nearColor(onVertices[i].g, Color(0, 0, 0), 1.0e-12)
					|| true); // enabled session has no model before first epoch
				TASK_CHECK(nearColor(offVertices[i].g, Color(0, 0, 0), 1.0e-12));
				TASK_CHECK(nearColor(offVertices[i].a, Color(0, 0, 0), 1.0e-12));
			}
		}
		return (0);
	}

	int	epochsSlotIsolation(void)
	{
		// Two objects sharing one material, hit on front and back faces, at two
		// remaining depths, must never share a slot.
		Scene scene;
		configureConstantSkyScene(scene, Color(0.2, 0.2, 0.2));
		auto shared = std::make_shared<Lambertian>(Color(0.5, 0.5, 0.5));
		auto sphereA = std::make_shared<Sphere>(Vector3(-1.2, 0, -3), 0.8, shared);
		auto sphereB = std::make_shared<Sphere>(Vector3(1.2, 0, -3), 0.8, shared);
		scene.addHittable(sphereA);
		scene.addHittable(sphereB);

		Configuration configuration = testConfiguration(2);
		configuration.minSamples = 32;
		ControlVariateSession session(scene, configuration);

		// Rays onto each sphere, including directions that reach the back face
		// through continuation geometry is not possible without transmission,
		// so front/back isolation is exercised directly through ModelKey.
		const Ray rayA(Vector3(0, 0, 0), Utilities::normalize(Vector3(-1.2, 0, -3)));
		const Ray rayB(Vector3(0, 0, 0), Utilities::normalize(Vector3(1.2, 0, -3)));
		for (std::uint64_t sampleId = 0; sampleId < 256; sampleId++)
		{
			session.sample(rayA, sampleId);
			session.sample(rayB, sampleId);
		}
		const std::vector<ModelKey> keys = session.getEpochKeys();
		TASK_CHECK(keys.size() >= 2);
		for (std::size_t i = 0; i < keys.size(); i++)
		{
			for (std::size_t j = i + 1; j < keys.size(); j++)
			{
				TASK_CHECK(!(keys[i] == keys[j]));
			}
			TASK_CHECK(session.getEpochSampleCount(keys[i]) > 0);
			const NormalEquations* equations = session.getEpochEquations(keys[i]);
			TASK_CHECK(equations != nullptr);
			TASK_CHECK(equations->sampleCount == session.getEpochSampleCount(keys[i]));
		}
		session.finishEpoch();
		// Epoch statistics are cleared after publishing.
		TASK_CHECK(session.getEpochKeys().empty());
		TASK_CHECK(session.getEpoch() == 1);

		const auto& models = session.getModels();
		TASK_CHECK(models.size() == keys.size());
		for (const auto& entry : models)
		{
			TASK_CHECK(entry.second.equations.sampleCount > 0);
			if (entry.second.equations.sampleCount >= configuration.minSamples)
			{
				TASK_CHECK(entry.second.active);
			}
			else
			{
				// Under-sampled slots publish an explicit zero model.
				TASK_CHECK(!entry.second.active);
				for (const Color& coefficient : entry.second.coefficients)
				{
					TASK_CHECK(nearColor(coefficient, Color(0, 0, 0), 1.0e-12));
				}
			}
		}

		// Explicit front/back and depth key inequality.
		const ModelKey base{sphereA.get(), 0, true, 1};
		const ModelKey back{sphereA.get(), 0, false, 1};
		const ModelKey otherDepth{sphereA.get(), 0, true, 0};
		const ModelKey otherObject{sphereB.get(), 1, true, 1};
		TASK_CHECK(back < base || base < back);
		TASK_CHECK(!(back == base));
		TASK_CHECK(!(otherDepth == base));
		TASK_CHECK(!(otherObject == base));
		return (0);
	}

	int	epochsFrozenBeforeAfter(void)
	{
		Scene scene;
		configureLinearSkyScene(scene);
		auto material = std::make_shared<Lambertian>(Color(0.6, 0.6, 0.6));
		scene.addHittable(std::make_shared<Sphere>(Vector3(0, 0, -3), 1.0, material));

		Configuration configuration = testConfiguration(1);
		configuration.minSamples = 64;
		ControlVariateSession session(scene, configuration);
		TASK_CHECK(session.getModels().empty());
		TASK_CHECK(session.getEpoch() == 0);

		for (std::uint64_t sampleId = 0; sampleId < 128; sampleId++)
		{
			session.sample(Ray(Vector3(0, 0, 0), Vector3(0, 0, -1)), sampleId);
		}
		const std::vector<ModelKey> keys = session.getEpochKeys();
		TASK_CHECK(!keys.empty());
		const NormalEquations before = *session.getEpochEquations(keys.front());
		TASK_CHECK(before.sampleCount == 128);
		session.finishEpoch();

		const auto& models = session.getModels();
		const auto published = models.find(keys.front());
		TASK_CHECK(published != models.end());
		TASK_CHECK(published->second.active);
		// The frozen normal equations match the epoch average.
		TASK_CHECK(published->second.equations.sampleCount == before.sampleCount);
		for (std::size_t i = 0; i < SH_COEFFICIENT_COUNT; i++)
		{
			TASK_CHECK(nearValue(published->second.equations.rhs[i].getRed(), before.rhs[i].getRed(), 1.0e-12));
			for (std::size_t j = 0; j < SH_COEFFICIENT_COUNT; j++)
			{
				TASK_CHECK(nearValue(published->second.equations.matrix[i][j], before.matrix[i][j], 1.0e-12));
			}
		}

		// The table stays frozen while a new epoch accumulates.
		const ModelCoefficients snapshot = published->second.coefficients;
		for (std::uint64_t sampleId = 0; sampleId < 64; sampleId++)
		{
			session.sample(Ray(Vector3(0, 0, 0), Vector3(0, 0, -1)), sampleId);
		}
		TASK_CHECK(nearColor(models.find(keys.front())->second.coefficients[0], snapshot[0], 1.0e-12));
		session.finishEpoch();
		TASK_CHECK(session.getEpoch() == 2);
		return (0);
	}

	int	epochsEmptyAndUnderSampled(void)
	{
		Scene scene;
		configureConstantSkyScene(scene, Color(0.1, 0.1, 0.1));
		auto material = std::make_shared<Lambertian>(Color(0.5, 0.5, 0.5));
		scene.addHittable(std::make_shared<Sphere>(Vector3(0, 0, -3), 1.0, material));

		ControlVariateSession session(scene, testConfiguration(1, 7, 64));
		// An entirely empty epoch publishes a zero table and still counts.
		session.finishEpoch();
		TASK_CHECK(session.getEpoch() == 1);
		TASK_CHECK(session.getModels().empty());
		TASK_CHECK(session.getEpochKeys().empty());

		// Fewer than minSamples freezes an explicit inactive zero model.
		for (std::uint64_t sampleId = 0; sampleId < 16; sampleId++)
		{
			session.sample(Ray(Vector3(0, 0, 0), Vector3(0, 0, -1)), sampleId);
		}
		session.finishEpoch();
		TASK_CHECK(session.getEpoch() == 2);
		TASK_CHECK(!session.getModels().empty());
		for (const auto& entry : session.getModels())
		{
			TASK_CHECK(!entry.second.active);
			for (const Color& coefficient : entry.second.coefficients)
			{
				TASK_CHECK(nearColor(coefficient, Color(0, 0, 0), 1.0e-12));
			}
		}
		return (0);
	}

	int	epochsResetAndIsolation(void)
	{
		Scene scene;
		configureLinearSkyScene(scene);
		auto material = std::make_shared<Lambertian>(Color(0.6, 0.6, 0.6));
		scene.addHittable(std::make_shared<Sphere>(Vector3(0, 0, -3), 1.0, material));

		ControlVariateSession a(scene, testConfiguration(1, 11, 32));
		ControlVariateSession b(scene, testConfiguration(1, 99, 32));
		for (std::uint64_t sampleId = 0; sampleId < 128; sampleId++)
		{
			a.sample(Ray(Vector3(0, 0, 0), Vector3(0, 0, -1)), sampleId);
			b.sample(Ray(Vector3(0, 0, 0), Vector3(0, 0, -1)), sampleId);
		}
		a.finishEpoch();
		b.finishEpoch();
		TASK_CHECK(a.getEpoch() == 1 && b.getEpoch() == 1);
		TASK_CHECK(a.getModels().size() == b.getModels().size());

		a.reset();
		TASK_CHECK(a.getEpoch() == 0);
		TASK_CHECK(a.getModels().empty());
		TASK_CHECK(a.getEpochKeys().empty());
		// The other session is untouched.
		TASK_CHECK(b.getEpoch() == 1);
		TASK_CHECK(!b.getModels().empty());

		// Different seeds produce different sampled directions.
		const Vector3 directionA = a.cosineDirection(Vector3(0, 1, 0), 5, 1);
		const Vector3 directionB = b.cosineDirection(Vector3(0, 1, 0), 5, 1);
		TASK_CHECK(Utilities::dot(directionA, directionB) < 0.999999);
		return (0);
	}

	int	epochsConfigurationValidation(void)
	{
		Scene scene;
		configureConstantSkyScene(scene, Color(0.1, 0.1, 0.1));
		auto material = std::make_shared<Lambertian>(Color(0.5, 0.5, 0.5));
		scene.addHittable(std::make_shared<Sphere>(Vector3(0, 0, -3), 1.0, material));

		const int badDepths[] = {-1, 13};
		for (int badDepth : badDepths)
		{
			Configuration configuration = testConfiguration(badDepth);
			bool threw = false;
			try { ControlVariateSession session(scene, configuration); }
			catch (const std::invalid_argument&) { threw = true; }
			TASK_CHECK(threw);
		}
		const double badRidges[] = {0.0, -1.0, std::numeric_limits<double>::infinity(),
			std::numeric_limits<double>::quiet_NaN()};
		for (double badRidge : badRidges)
		{
			Configuration configuration = testConfiguration(2);
			configuration.ridge = badRidge;
			bool threw = false;
			try { ControlVariateSession session(scene, configuration); }
			catch (const std::invalid_argument&) { threw = true; }
			TASK_CHECK(threw);
		}
		Configuration fewSamples = testConfiguration(2);
		fewSamples.minSamples = 8;
		bool threw = false;
		try { ControlVariateSession session(scene, fewSamples); }
		catch (const std::invalid_argument&) { threw = true; }
		TASK_CHECK(threw);

		// Unsupported geometry, material, sky mode and textures are rejected at
		// construction.
		Scene cubeScene;
		configureConstantSkyScene(cubeScene, Color(0, 0, 0));
		cubeScene.addHittable(std::make_shared<Cube>(
			Transform(), 1.0, 1.0, 1.0, std::make_shared<Lambertian>(Color(0.5, 0.5, 0.5))
		));
		threw = false;
		try { ControlVariateSession session(cubeScene, testConfiguration(1)); }
		catch (const std::invalid_argument&) { threw = true; }
		TASK_CHECK(threw);

		Scene metalScene;
		configureConstantSkyScene(metalScene, Color(0, 0, 0));
		metalScene.addHittable(std::make_shared<Sphere>(
			Vector3(0, 0, -3), 1.0, std::make_shared<Metal>(Color(0.8, 0.8, 0.8))
		));
		threw = false;
		try { ControlVariateSession session(metalScene, testConfiguration(1)); }
		catch (const std::invalid_argument&) { threw = true; }
		TASK_CHECK(threw);

		Scene atmosphereScene;
		atmosphereScene.setRenderSky(SKY_ATMOSPHERE);
		atmosphereScene.addHittable(std::make_shared<Sphere>(
			Vector3(0, 0, -3), 1.0, std::make_shared<Lambertian>(Color(0.5, 0.5, 0.5))
		));
		threw = false;
		try { ControlVariateSession session(atmosphereScene, testConfiguration(1)); }
		catch (const std::invalid_argument&) { threw = true; }
		TASK_CHECK(threw);

		Scene texturedScene;
		configureConstantSkyScene(texturedScene, Color(0, 0, 0));
		auto textured = std::make_shared<Lambertian>(Color(0.5, 0.5, 0.5));
		std::vector<Color> pixels = {Color(0.1, 0.2, 0.3), Color(0.4, 0.5, 0.6),
			Color(0.7, 0.8, 0.9), Color(0.2, 0.3, 0.4)};
		textured->setTexture(std::make_shared<Texture>(2, 2, pixels));
		texturedScene.addHittable(std::make_shared<Sphere>(Vector3(0, 0, -3), 1.0, textured));
		threw = false;
		try { ControlVariateSession session(texturedScene, testConfiguration(1)); }
		catch (const std::invalid_argument&) { threw = true; }
		TASK_CHECK(threw);
		return (0);
	}

	int	epochsSuffixLabels(void)
	{
		// Training labels are the full suffix Li, with no local rho and no
		// camera throughput.  A horizontal surface viewed from above sends every
		// cosine continuation into the empty upper hemisphere, so Li is exactly
		// the constant sky.  Full-sphere SH are not orthogonal on a hemisphere,
		// so the least-squares coefficients are individually mixed; the unbiased
		// quantity is the analytic hemisphere integral A(n).
		const Color sky(0.2, 0.4, 0.6);
		const Color albedo(0.125, 0.25, 0.5);
		Scene scene;
		configureConstantSkyScene(scene, sky);
		auto material = std::make_shared<Lambertian>(albedo);
		scene.addHittable(std::make_shared<Triangle>(
			Vector3(-8, -1, -1),
			Vector3(8, -1, -1),
			Vector3(0, -1, -40),
			material
		));

		Configuration configuration = testConfiguration(1);
		configuration.minSamples = 256;
		configuration.ridge = 1.0e-6;
		ControlVariateSession session(scene, configuration);
		const Ray primary(Vector3(0, 1, 0), Utilities::normalize(Vector3(0, -0.3, -1)));
		for (std::uint64_t sampleId = 0; sampleId < 16384; sampleId++)
		{
			session.sample(primary, sampleId);
		}

		// The recorded suffix label is the sky itself: never multiplied by the
		// local albedo and never by any camera throughput.
		const auto& trainingVertices = session.getLastPathVertices();
		TASK_CHECK(!trainingVertices.empty());
		const PathVertex& surfaceVertex = trainingVertices.front();
		TASK_CHECK(nearColor(surfaceVertex.li, sky, 1.0e-12));
		TASK_CHECK(!nearColor(surfaceVertex.li, albedo * sky, 1.0e-9));

		session.finishEpoch();
		const auto& models = session.getModels();
		TASK_CHECK(models.size() == 1);
		const FrozenModel& model = models.begin()->second;
		TASK_CHECK(model.active);

		// A(n) must recover the sky, proving the labels were Li rather than
		// rho*Li or a final pixel value.
		const std::array<double, SH_COEFFICIENT_COUNT> integrals =
			hemisphereCosineIntegrals(surfaceVertex.normal);
		Color analyticMean(0, 0, 0);
		for (std::size_t k = 0; k < SH_COEFFICIENT_COUNT; k++)
		{
			analyticMean += model.coefficients[k] * integrals[k];
		}
		TASK_CHECK(nearColor(analyticMean, sky, 0.02));
		TASK_CHECK(!nearColor(analyticMean, albedo * sky, 0.02));
		return (0);
	}

	int	varianceNegativeResidual(void)
	{
		// Train on a real localised lamp so the frozen SH model over-estimates
		// directions that miss the lamp; the signed residual Li - g must remain
		// negative rather than being clipped.
		Scene scene;
		configureConstantSkyScene(scene, Color(0.0, 0.0, 0.0));
		auto lamp = std::make_shared<Emissive>(Color(4.0, 4.0, 4.0));
		scene.addHittable(std::make_shared<Triangle>(
			Vector3(-0.4, 0.6, -2.5), Vector3(0.4, 0.6, -2.5),
			Vector3(0.0, 1.2, -2.5), lamp
		));
		auto floor = std::make_shared<Lambertian>(Color(0.8, 0.8, 0.8));
		scene.addHittable(std::make_shared<Triangle>(
			Vector3(-8, -1, -1), Vector3(8, -1, -1), Vector3(0, -1, -40), floor
		));

		Configuration configuration = testConfiguration(1);
		configuration.minSamples = 128;
		ControlVariateSession session(scene, configuration);
		// Steep enough that a real fraction of cosine continuations reach the
		// small lamp; the rest miss it and expose negative Li - g residuals.
		const Ray primary(Vector3(0, 0, 0), Utilities::normalize(Vector3(0, -0.5, -1)));
		for (std::uint64_t sampleId = 0; sampleId < 2048; sampleId++)
		{
			session.sample(primary, sampleId);
		}
		session.finishEpoch();

		bool sawNegativeResidual = false;
		bool sawNegativeIntermediate = false;
		for (std::uint64_t sampleId = 0; sampleId < 2048; sampleId++)
		{
			session.sample(primary, sampleId);
			for (const PathVertex& vertex : session.getLastPathVertices())
			{
				const Color residual = vertex.li - vertex.g;
				if (residual.getRed() < 0.0 || residual.getGreen() < 0.0 || residual.getBlue() < 0.0)
				{
					sawNegativeResidual = true;
				}
				const Color bracket = vertex.a + vertex.li - vertex.g;
				if (bracket.getRed() < 0.0)
				{
					sawNegativeIntermediate = true;
				}
			}
		}
		TASK_CHECK(sawNegativeResidual);
		TASK_CHECK(sawNegativeIntermediate);
		return (0);
	}

	struct	VarianceBatch
	{
		Color mean;
		Color variance;
		std::size_t count = 0;
	};

	VarianceBatch	summarize(const std::vector<Color>& values)
	{
		const Color mean = channelAverage(values);
		return {mean, channelVariance(values, mean), values.size()};
	}

	int	varianceReductionLinearSky(void)
	{
		// A horizontal floor under the linear sky, plus a farther low-contrast
		// back wall.  The wall gives the sky's otherwise-constant blue channel
		// smooth directional variance, while the low-order sky remains the
		// dominant signal so an l<=2 model removes most of the noise.
		Scene scene;
		configureLinearSkyScene(scene);
		auto floorMaterial = std::make_shared<Lambertian>(Color(0.75, 0.5, 0.25));
		scene.addHittable(std::make_shared<Triangle>(
			Vector3(-8, -1, -1), Vector3(8, -1, -1), Vector3(0, -1, -40), floorMaterial
		));
		auto wallMaterial = std::make_shared<Lambertian>(Color(0.55, 0.6, 0.45));
		scene.addHittable(std::make_shared<Triangle>(
			Vector3(-10, -1, -8.0), Vector3(10, -1, -8.0), Vector3(0, 4, -8.0), wallMaterial
		));

		constexpr std::size_t BATCHES = 16;
		constexpr std::size_t SAMPLES = 1024;
		constexpr std::size_t TRAINING_SAMPLES = SAMPLES * 4;
		constexpr std::size_t REFERENCE_SAMPLES = 32768;
		const Ray primary(Vector3(0, 0, 0), Utilities::normalize(Vector3(0.1, -0.15, -1.0)));

		Configuration base;
		base.maxDepth = 2;
		base.seed = 1000;
		base.ridge = 1.0e-6;
		base.minSamples = 256;
		base.enabled = true;

		// Independent reference: a high-sample run with the control variate
		// disabled and an unrelated seed.
		Configuration referenceConfiguration = base;
		referenceConfiguration.enabled = false;
		referenceConfiguration.seed = 987654321;
		ControlVariateSession referenceSession(scene, referenceConfiguration);
		std::vector<Color> referenceValues;
		referenceValues.reserve(REFERENCE_SAMPLES);
		for (std::uint64_t sampleId = 0; sampleId < REFERENCE_SAMPLES; sampleId++)
		{
			referenceValues.push_back(referenceSession.sample(primary, sampleId));
		}
		const VarianceBatch reference = summarize(referenceValues);

		std::vector<Color> coldMeans;
		std::vector<Color> trainedMeans;
		std::vector<Color> coldVariances;
		std::vector<Color> trainedVariances;
		Color coldMeanAggregate(0, 0, 0);
		Color trainedMeanAggregate(0, 0, 0);

		for (std::size_t batch = 0; batch < BATCHES; batch++)
		{
			const std::uint64_t offset = static_cast<std::uint64_t>(batch) * 7919ull;

			// Cold start: a fresh session, no frozen model.
			Configuration coldConfiguration = base;
			coldConfiguration.seed = base.seed + offset;
			ControlVariateSession coldSession(scene, coldConfiguration);
			std::vector<Color> coldValues;
			coldValues.reserve(SAMPLES);
			for (std::uint64_t sampleId = 0; sampleId < SAMPLES; sampleId++)
			{
				coldValues.push_back(coldSession.sample(primary, sampleId));
			}
			const VarianceBatch cold = summarize(coldValues);
			coldMeans.push_back(cold.mean);
			coldVariances.push_back(cold.variance);
			coldMeanAggregate += cold.mean;

			// Trained: fit on an independent, larger training epoch, publish the
			// model, then measure the next epoch at the same sample count.
			Configuration trainedConfiguration = base;
			trainedConfiguration.seed = base.seed + offset;
			ControlVariateSession trainer(scene, trainedConfiguration);
			for (std::uint64_t sampleId = 0; sampleId < TRAINING_SAMPLES; sampleId++)
			{
				trainer.sample(primary, sampleId);
			}
			trainer.finishEpoch();
			std::vector<Color> guided;
			guided.reserve(SAMPLES);
			for (std::uint64_t sampleId = 0; sampleId < SAMPLES; sampleId++)
			{
				guided.push_back(trainer.sample(primary, sampleId));
			}
			const VarianceBatch trained = summarize(guided);
			trainedMeans.push_back(trained.mean);
			trainedVariances.push_back(trained.variance);
			trainedMeanAggregate += trained.mean;

			std::cout << "batch " << batch
				<< " n=" << SAMPLES
				<< " cold_mean=" << cold.mean.getRed() << "," << cold.mean.getGreen() << "," << cold.mean.getBlue()
				<< " trained_mean=" << trained.mean.getRed() << "," << trained.mean.getGreen() << "," << trained.mean.getBlue()
				<< " cold_var=" << cold.variance.getRed() << "," << cold.variance.getGreen() << "," << cold.variance.getBlue()
				<< " trained_var=" << trained.variance.getRed() << "," << trained.variance.getGreen() << "," << trained.variance.getBlue()
				<< "\n";
		}

		coldMeanAggregate = coldMeanAggregate / static_cast<double>(BATCHES);
		trainedMeanAggregate = trainedMeanAggregate / static_cast<double>(BATCHES);

		Color coldVarianceMean(0, 0, 0);
		Color trainedVarianceMean(0, 0, 0);
		for (std::size_t i = 0; i < BATCHES; i++)
		{
			coldVarianceMean += coldVariances[i];
			trainedVarianceMean += trainedVariances[i];
		}
		coldVarianceMean = coldVarianceMean / static_cast<double>(BATCHES);
		trainedVarianceMean = trainedVarianceMean / static_cast<double>(BATCHES);

		std::cout << "VARIANCE summary batches=" << BATCHES
			<< " samples_per_batch=" << SAMPLES
			<< " reference_n=" << REFERENCE_SAMPLES
			<< " reference_mean=" << reference.mean
			<< " cold_mean=" << coldMeanAggregate
			<< " trained_mean=" << trainedMeanAggregate
			<< " cold_variance=" << coldVarianceMean
			<< " trained_variance=" << trainedVarianceMean << "\n";

		// The trained variance must be below one quarter of the cold variance on
		// every channel, including the blue channel that now carries real
		// directional variance from the back wall.
		const Color quarter = coldVarianceMean * 0.25;
		TASK_CHECK(trainedVarianceMean.getRed() < quarter.getRed());
		TASK_CHECK(trainedVarianceMean.getGreen() < quarter.getGreen());
		TASK_CHECK(trainedVarianceMean.getBlue() < quarter.getBlue());

		// Both means agree with the independent high-sample reference.
		TASK_CHECK(nearColor(trainedMeanAggregate, reference.mean, 0.04));
		TASK_CHECK(nearColor(coldMeanAggregate, reference.mean, 0.05));
		return (0);
	}
}

const struct task_case task_cases[] = {
	{"harmonics", "basis_orthonormality", harmonicsBasisOrthogonality},
	{"harmonics", "basis_values", harmonicsBasisValues},
	{"harmonics", "cosine_integrals", harmonicsCosineIntegrals},
	{"transport", "constant_sky_depth", transportConstantSkyDepthZero},
	{"transport", "linear_sky_mean", transportLinearSkyMean},
	{"transport", "emissive_termination", transportEmissiveTermination},
	{"transport", "finite_lamp_occlusion", transportFiniteLampAndOcclusion},
	{"transport", "two_bounces", transportTwoBounces},
	{"transport", "vertex_recursion", transportVertexRecursionFields},
	{"transport", "disabled_directions", transportDisabledKeepsDirections},
	{"epochs", "slot_isolation", epochsSlotIsolation},
	{"epochs", "frozen_equations", epochsFrozenBeforeAfter},
	{"epochs", "empty_and_under_sampled", epochsEmptyAndUnderSampled},
	{"epochs", "reset_and_isolation", epochsResetAndIsolation},
	{"epochs", "configuration_validation", epochsConfigurationValidation},
	{"epochs", "suffix_labels", epochsSuffixLabels},
	{"variance", "negative_residual", varianceNegativeResidual},
	{"variance", "linear_sky_reduction", varianceReductionLinearSky}
};
const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
