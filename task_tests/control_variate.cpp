// Control-variate tests: harmonics, transport, epochs and variance.
//
// Built by task_support/run.sh target, which provides task_check and drives
// the registered task_cases. No main() is defined here.
//
// Path-vertex ordering: Session appends a vertex only after its continuation
// returns, so lastPath() is ordered deepest-first; lastPath().back() is the
// primary vertex and lastPath()[k].li equals lastPath()[k-1].estimate.
#include "task_support/check.h"

#include "Renderer/ControlVariate.hpp"

#include "Hittables/Rectangle.hpp"
#include "Hittables/Sphere.hpp"
#include "Hittables/Triangle.hpp"
#include "Materials/Emissive.hpp"
#include "Materials/Lambertian.hpp"
#include "Materials/Metal.hpp"
#include "Scene/Scene.hpp"
#include "SkyTypes.hpp"
#include "Texture.hpp"
#include "Transform.hpp"
#include "Utilities.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <memory>
#include <vector>

namespace
{
	constexpr double	TEST_PI = 3.14159265358979323846;
	constexpr double	SH_Y00 = 0.28209479177387814;
	constexpr double	SH_Y1 = 0.4886025119029199;
	constexpr double	SH_Y2M2 = 1.0925484305920792;
	constexpr double	SH_Y20 = 0.31539156525252005;
	constexpr double	SH_Y22 = 0.5462742152960396;

	bool	near(double a, double b, double tolerance = 1e-9)
	{
		return (std::fabs(a - b) <= tolerance * std::max(1.0, std::max(std::fabs(a), std::fabs(b))));
	}

	bool	nearColor(const Color& a, const Color& b, double tolerance = 1e-9)
	{
		return (
			near(a.getRed(), b.getRed(), tolerance)
			&& near(a.getGreen(), b.getGreen(), tolerance)
			&& near(a.getBlue(), b.getBlue(), tolerance)
		);
	}

	bool	nearVector(const Vector3& a, const Vector3& b, double tolerance = 1e-9)
	{
		return (
			near(a.getX(), b.getX(), tolerance)
			&& near(a.getY(), b.getY(), tolerance)
			&& near(a.getZ(), b.getZ(), tolerance)
		);
	}

	// --- Independent reference SH (normalized convention). ------------------
	// Index order follows the (l, m) layout documented in ControlVariate.hpp:
	//   0: Y_0^0, 1: Y_1^-1, 2: Y_1^0, 3: Y_1^1, 4: Y_2^-2, 5: Y_2^-1,
	//   6: Y_2^0, 7: Y_2^1, 8: Y_2^2
	double	referenceY(int index, const Vector3& d)
	{
		const double x = d.getX();
		const double y = d.getY();
		const double z = d.getZ();

		switch (index)
		{
			case 0: return (SH_Y00);
			case 1: return (SH_Y1 * y);
			case 2: return (SH_Y1 * z);
			case 3: return (SH_Y1 * x);
			case 4: return (SH_Y2M2 * x * y);
			case 5: return (SH_Y2M2 * y * z);
			case 6: return (SH_Y20 * (3.0 * z * z - 1.0));
			case 7: return (SH_Y2M2 * x * z);
			case 8: return (SH_Y22 * (x * x - y * y));
			default: return (0.0);
		}
	}

	std::vector<Vector3>	sphereQuadrature(std::size_t count)
	{
		std::vector<Vector3> directions;
		directions.reserve(count);
		const double goldenAngle = TEST_PI * (3.0 - std::sqrt(5.0));

		for (std::size_t i = 0; i < count; i++)
		{
			const double z = 1.0 - 2.0 * (static_cast<double>(i) + 0.5) / static_cast<double>(count);
			const double radius = std::sqrt(std::max(0.0, 1.0 - z * z));
			const double phi = goldenAngle * static_cast<double>(i);
			directions.emplace_back(radius * std::cos(phi), radius * std::sin(phi), z);
		}
		return (directions);
	}

	// Uniform-sphere quadrature of integral over H(n) of Y(w)(n.w)/pi dw.
	double	integrateCosineSphere(
		const std::vector<Vector3>& directions,
		const Vector3& normal,
		int index
	)
	{
		const double weight = 4.0 * TEST_PI / static_cast<double>(directions.size());
		double sum = 0.0;

		for (const Vector3& direction : directions)
		{
			const double cosine = Utilities::dot(direction, normal);
			if (cosine > 0.0)
			{
				sum += referenceY(index, direction) * cosine;
			}
		}
		return (sum * weight / TEST_PI);
	}

	Vector3	localHemisphereDirection(double u1, double u2, const Vector3& normal)
	{
		const double phi = 2.0 * TEST_PI * u1;
		const double radius = std::sqrt(u2);
		const double height = std::sqrt(std::max(0.0, 1.0 - u2));
		const Vector3 helper = std::fabs(normal.getX()) > 0.9
			? Vector3(0.0, 1.0, 0.0) : Vector3(1.0, 0.0, 0.0);
		const Vector3 tangent = Utilities::normalize(Utilities::cross(helper, normal));
		const Vector3 bitangent = Utilities::cross(normal, tangent);
		return (
			radius * std::cos(phi) * tangent
			+ radius * std::sin(phi) * bitangent
			+ height * normal
		);
	}

	ControlVariate::Config	validConfig(int maxDepth = 4)
	{
		ControlVariate::Config config;

		config.maxDepth = maxDepth;
		config.seed = 987654321u;
		config.ridge = 1e-4;
		config.minSamples = 16;
		config.enabled = true;
		return (config);
	}

	struct	TransportScene
	{
		std::shared_ptr<Lambertian>	material;
		std::shared_ptr<Emissive>	lampMaterial;
		std::shared_ptr<Hittable>	sphereA;
		std::shared_ptr<Hittable>	sphereB;
		std::shared_ptr<Hittable>	lampTriangle;
	};

	// Fills 'scene' with two Lambertian spheres sharing one material plus a
	// finite emissive triangle. Returns the identity handles used by the keys.
	TransportScene	buildTransportScene(Scene& scene, double background = 0.0)
	{
		TransportScene result;

		scene.setRenderSky(SKY_NONE);
		scene.setBackgroundColor(Color(background, background, background));
		scene.setMaxLightBounces(4);

		result.material = std::make_shared<Lambertian>(Color(0.6, 0.5, 0.4));
		result.lampMaterial = std::make_shared<Emissive>(Color(6.0, 5.0, 3.0));

		result.sphereA = std::make_shared<Sphere>(Vector3(-1.0, 0.0, 0.0), 0.9, result.material);
		result.sphereB = std::make_shared<Sphere>(Vector3(1.0, 0.0, 0.0), 0.9, result.material);
		result.lampTriangle = std::make_shared<Triangle>(
			Vector3(-2.5, 2.0, 2.5),
			Vector3(2.5, 2.0, 2.5),
			Vector3(0.0, 2.0, -2.5),
			result.lampMaterial
		);

		scene.addHittable(result.sphereA);
		scene.addHittable(result.sphereB);
		scene.addHittable(result.lampTriangle);
		scene.updateLights();
		return (result);
	}

	// Single Lambertian floor under the linear sky, geometric normal +y.
	void	buildLinearSkyFloorScene(Scene& scene)
	{
		scene.setRenderSky(SKY_LINEAR);
		scene.setBackgroundColor(Color(0.0, 0.0, 0.0));
		scene.setMaxLightBounces(2);
		scene.addHittable(std::make_shared<Triangle>(
			Vector3(-40.0, 0.0, -40.0),
			Vector3(-40.0, 0.0, 40.0),
			Vector3(40.0, 0.0, 0.0),
			std::make_shared<Lambertian>(Color(0.6, 0.6, 0.6))
		));
	}

	// A floor with a finite lamp straight above and one Lambertian sphere that
	// can catch a second bounce. Optionally places a dark occluder between the
	// floor origin and the lamp.
	void	buildLampScene(Scene& scene, bool withOccluder)
	{
		scene.setRenderSky(SKY_NONE);
		scene.setBackgroundColor(Color(0.0, 0.0, 0.0));
		scene.setMaxLightBounces(3);
		scene.addHittable(std::make_shared<Triangle>(
			Vector3(-10.0, 0.0, -10.0),
			Vector3(-10.0, 0.0, 10.0),
			Vector3(10.0, 0.0, 0.0),
			std::make_shared<Lambertian>(Color(0.8, 0.8, 0.8))
		));
		scene.addHittable(std::make_shared<Triangle>(
			Vector3(-2.0, 3.0, 2.0),
			Vector3(2.0, 3.0, 2.0),
			Vector3(0.0, 3.0, -2.0),
			std::make_shared<Emissive>(Color(8.0, 8.0, 8.0))
		));
		scene.addHittable(std::make_shared<Sphere>(
			Vector3(2.0, 1.0, 0.0), 1.0,
			std::make_shared<Lambertian>(Color(0.5, 0.5, 0.5))
		));
		if (withOccluder)
		{
			scene.addHittable(std::make_shared<Sphere>(
				Vector3(0.0, 1.5, 0.0), 0.6,
				std::make_shared<Lambertian>(Color(0.05, 0.05, 0.05))
			));
		}
	}

	// --- Case group: harmonics ---------------------------------------------

	int	harmonicBasisMatchesReference(void)
	{
		const std::vector<Vector3> directions = {
			Vector3(0.0, 0.0, 1.0),
			Vector3(1.0, 0.0, 0.0),
			Vector3(0.0, 1.0, 0.0),
			Vector3(0.36, -0.48, 0.80),
			Utilities::normalize(Vector3(-0.2, 0.7, 0.3)),
			Utilities::normalize(Vector3(0.9, -0.1, -0.4))
		};

		for (const Vector3& direction : directions)
		{
			const ControlVariate::ShVector basis = ControlVariate::shBasis(direction);
			for (int i = 0; i < ControlVariate::SH_COEFFICIENT_COUNT; i++)
			{
				TASK_CHECK(near(basis[i], referenceY(i, Utilities::normalize(direction)), 1e-12));
			}
		}

		// Orthonormality under a fine Fibonacci sphere quadrature.
		const std::size_t sampleCount = 400000;
		const std::vector<Vector3> quadrature = sphereQuadrature(sampleCount);
		double gram[9][9] = {};
		const double weight = 4.0 * TEST_PI / static_cast<double>(sampleCount);

		for (const Vector3& direction : quadrature)
		{
			const ControlVariate::ShVector basis = ControlVariate::shBasis(direction);
			for (int i = 0; i < 9; i++)
			{
				for (int j = 0; j < 9; j++)
				{
					gram[i][j] += basis[i] * basis[j] * weight;
				}
			}
		}
		for (int i = 0; i < 9; i++)
		{
			for (int j = 0; j < 9; j++)
			{
				TASK_CHECK(near(gram[i][j], i == j ? 1.0 : 0.0, 3e-3));
			}
		}
		return (0);
	}

	int	harmonicCosineIntegralQuadrature(void)
	{
		// Independent sphere quadrature of the analytic cosine integral.
		const std::vector<Vector3> quadrature = sphereQuadrature(400000);
		const Vector3 normals[3] = {
			Vector3(0.0, 1.0, 0.0),
			Utilities::normalize(Vector3(0.3, -0.2, 0.85)),
			Utilities::normalize(Vector3(-0.7, 0.1, 0.2))
		};

		for (const Vector3& normal : normals)
		{
			const ControlVariate::ShVector analytic = ControlVariate::shCosineIntegral(normal);
			for (int i = 0; i < 9; i++)
			{
				const double reference = integrateCosineSphere(quadrature, normal, i);
				TASK_CHECK(near(analytic[i], reference, 8e-3));
			}
		}

		// Independent cosine-weighted Monte Carlo: A(n) = E_{w ~ cos}[Y(w)].
		const std::size_t sampleCount = 300000;
		std::uint64_t state = 0x123456789abcdefull;
		const Vector3 mcNormal = Utilities::normalize(Vector3(0.25, 0.6, -0.35));
		double sums[9] = {};

		for (std::size_t i = 0; i < sampleCount; i++)
		{
			state = state * 6364136223846793005ull + 1442695040888963407ull;
			const double u1 = static_cast<double>((state >> 11) & ((1ull << 53) - 1)) / static_cast<double>(1ull << 53);
			state = state * 6364136223846793005ull + 1442695040888963407ull;
			const double u2 = static_cast<double>((state >> 11) & ((1ull << 53) - 1)) / static_cast<double>(1ull << 53);
			const Vector3 direction = localHemisphereDirection(u1, u2, mcNormal);
			for (int index = 0; index < 9; index++)
			{
				sums[index] += referenceY(index, direction);
			}
		}
		const ControlVariate::ShVector analytic = ControlVariate::shCosineIntegral(mcNormal);
		for (int index = 0; index < 9; index++)
		{
			TASK_CHECK(near(sums[index] / static_cast<double>(sampleCount), analytic[index], 8e-3));
		}

		// evaluateSh is linear and reconstructs constant radiance from l = 0.
		ControlVariate::ShCoefficients constant{};
		constant[0] = Color(2.0, 3.0, 4.0);
		const ControlVariate::ShVector basis =
			ControlVariate::shBasis(Utilities::normalize(Vector3(0.2, -0.1, 0.9)));
		TASK_CHECK(nearColor(
			ControlVariate::evaluateSh(constant, basis),
			Color(2.0 * SH_Y00, 3.0 * SH_Y00, 4.0 * SH_Y00),
			1e-12
		));
		return (0);
	}

	// --- Case group: transport ---------------------------------------------

	int	transportSkyAndDepthZero(void)
	{
		Scene scene;
		TransportScene fixture = buildTransportScene(scene, 0.2);

		// Depth 0: an emissive surface is still visible, a miss still shows the
		// sky, but a Lambertian surface stops (no reflection).
		ControlVariate::Config config = validConfig(0);
		config.enabled = false;
		ControlVariate::Session session(scene, config);

		const Ray towardLamp(Vector3(0.0, 2.5, 0.0), Vector3(0.0, -1.0, 0.0));
		TASK_CHECK(nearColor(session.sample(towardLamp, 0), Color(6.0, 5.0, 3.0), 1e-9));

		const Ray skyUp(Vector3(-50.0, -100.0, -50.0), Vector3(0.0, 1.0, 0.0));
		const Ray skyDown(Vector3(50.0, 100.0, 50.0), Vector3(0.0, -1.0, 0.0));
		TASK_CHECK(nearColor(session.sample(skyUp, 1), Color(0.2, 0.2, 0.2), 1e-12));
		TASK_CHECK(nearColor(session.sample(skyDown, 2), Color(0.2, 0.2, 0.2), 1e-12));

		const Ray ontoSphere(Vector3(-1.0, -3.0, 0.0), Vector3(0.0, 1.0, 0.0));
		TASK_CHECK(nearColor(session.sample(ontoSphere, 3), Color(0.0, 0.0, 0.0), 1e-12));

		// Same with an enabled but empty model: zero model, no reflection.
		ControlVariate::Session enabledSession(scene, validConfig(0));
		TASK_CHECK(nearColor(enabledSession.sample(ontoSphere, 3), Color(0.0, 0.0, 0.0), 1e-12));

		// The sky is an unoccluded infinite source, so an emissive surface at
		// depth 0 still returns its emitted radiance unchanged.
		(void)fixture;
		return (0);
	}

	int	transportConstantSkyLambertianMean(void)
	{
		// A single convex Lambertian sphere under a constant SKY_NONE colour.
		// A cosine bounce off a convex sphere can never re-hit it, so every
		// path at depth >= 1 returns rho * sky exactly.
		Scene scene;

		scene.setRenderSky(SKY_NONE);
		scene.setBackgroundColor(Color(0.25, 0.5, 0.75));
		scene.setMaxLightBounces(3);
		scene.addHittable(std::make_shared<Sphere>(
			Vector3(0.0, 0.0, 0.0), 1.0,
			std::make_shared<Lambertian>(Color(0.8, 0.5, 0.2))
		));

		ControlVariate::Config config = validConfig(3);
		config.enabled = false;
		ControlVariate::Session session(scene, config);

		const Ray ray(Vector3(0.0, 0.0, 3.0), Vector3(0.0, 0.0, -1.0));
		const Color expected(0.8 * 0.25, 0.5 * 0.5, 0.2 * 0.75);
		for (std::uint64_t sampleId = 0; sampleId < 2048; sampleId++)
		{
			TASK_CHECK(nearColor(session.sample(ray, sampleId), expected, 1e-12));
		}

		// Analytic Lambertian mean under the linear sky. The sky is
		// L(w) = (1-t)*(1,1,1) + t*(0.5,0.7,1) with t = skyline*(w.y + 1).
		// Over the cosine hemisphere around n = +y, E[w.y] = 2/3, hence
		// E[t] = skyline * 5/3. The trained SH model must integrate to that.
		Scene linear;
		buildLinearSkyFloorScene(linear);
		ControlVariate::Session meanSession(linear, validConfig(2));
		const Ray probe(Vector3(0.0, 2.0, 3.0), Utilities::normalize(Vector3(0.0, -2.0, -3.0)));

		for (std::uint64_t sampleId = 0; sampleId < 8192; sampleId++)
		{
			meanSession.sample(probe, sampleId);
		}
		meanSession.finishEpoch();

		const ControlVariate::ModelEntry* entry = meanSession.findModelEntry(
			ControlVariate::makeModelKey(linear.getHittables()[0].get(), false, 2)
		);
		TASK_CHECK(entry != nullptr);
		const double skyline = linear.getSkyline();
		const double expectedT = skyline * (2.0 / 3.0 + 1.0);
		const Color expectedMean(
			1.0 - expectedT + 0.5 * expectedT,
			1.0 - expectedT + 0.7 * expectedT,
			1.0 - expectedT + 1.0 * expectedT
		);
		const Color analyticMean = ControlVariate::evaluateSh(
			entry->coefficients,
			ControlVariate::shCosineIntegral(Vector3(0.0, 1.0, 0.0))
		);
		TASK_CHECK(nearColor(analyticMean, expectedMean, 3e-2));
		return (0);
	}

	int	transportFiniteLampOcclusionAndTwoBounces(void)
	{
		// Occlusion must enter Li: an occluder between the floor point and the
		// finite lamp lowers the mean radiance at identical seeds.
		Scene openScene;
		Scene blockedScene;
		buildLampScene(openScene, false);
		buildLampScene(blockedScene, true);

		ControlVariate::Config config = validConfig(3);
		config.enabled = false;
		ControlVariate::Session openSession(openScene, config);
		ControlVariate::Session blockedSession(blockedScene, config);

		const Ray probe(Vector3(0.0, 2.5, 2.5), Utilities::normalize(Vector3(0.0, -2.5, -2.5)));
		constexpr std::size_t samples = 8192;
		double openSum = 0.0;
		double blockedSum = 0.0;
		for (std::size_t i = 0; i < samples; i++)
		{
			openSum += Utilities::luminance(openSession.sample(probe, static_cast<std::uint64_t>(i)));
			blockedSum += Utilities::luminance(blockedSession.sample(probe, static_cast<std::uint64_t>(i)));
		}
		TASK_CHECK(openSum > 0.0);
		TASK_CHECK(blockedSum < openSum * 0.97);

		// Two reflections: the parent's label is exactly the child's estimate
		// (the complete suffix), and with the model disabled every vertex
		// returns rho * Li.
		ControlVariate::Session pathSession(openScene, config);
		bool foundTwoVertices = false;
		for (std::uint64_t sampleId = 0; sampleId < 8192 && !foundTwoVertices; sampleId++)
		{
			pathSession.sample(probe, sampleId);
			const std::vector<ControlVariate::PathVertex>& path = pathSession.lastPath();

			TASK_CHECK(!path.empty());
			for (const ControlVariate::PathVertex& vertex : path)
			{
				TASK_CHECK(nearColor(vertex.g, Color(0.0, 0.0, 0.0), 1e-12));
				TASK_CHECK(nearColor(vertex.a, Color(0.0, 0.0, 0.0), 1e-12));
				TASK_CHECK(nearColor(vertex.estimate, vertex.rho * vertex.li, 1e-12));
				TASK_CHECK(near(Utilities::vectorLength(vertex.normal), 1.0, 1e-9));
				TASK_CHECK(near(Utilities::vectorLength(vertex.direction), 1.0, 1e-9));
				TASK_CHECK(Utilities::dot(vertex.normal, vertex.direction) >= -1e-9);
			}
			if (path.size() >= 2)
			{
				// Deepest-first ordering: path[0] is the child.
				TASK_CHECK(nearColor(path[1].li, path[0].estimate, 1e-12));
				TASK_CHECK(path[0].key != path[1].key);
				foundTwoVertices = true;
			}
		}
		TASK_CHECK(foundTwoVertices);
		return (0);
	}

	// --- Case group: epochs ------------------------------------------------

	int	epochKeysDoNotMerge(void)
	{
		Scene scene;
		TransportScene fixture = buildTransportScene(scene);
		ControlVariate::Session session(scene, validConfig(2));

		const ControlVariate::ModelKey keyA =
			ControlVariate::makeModelKey(fixture.sphereA.get(), false, 2);
		const ControlVariate::ModelKey keyB =
			ControlVariate::makeModelKey(fixture.sphereB.get(), false, 2);

		// Two objects sharing one material instance must not collapse.
		TASK_CHECK(keyA != keyB);
		TASK_CHECK(keyA < keyB || keyB < keyA);
		// Side and remaining depth are part of the key.
		TASK_CHECK(keyA != ControlVariate::makeModelKey(fixture.sphereA.get(), true, 2));
		TASK_CHECK(keyA != ControlVariate::makeModelKey(fixture.sphereA.get(), false, 1));
		TASK_CHECK(keyA != ControlVariate::makeModelKey(fixture.sphereA.get(), false, 3));

		// Keys observed by real paths use the genuine object identity.
		session.sample(Ray(Vector3(-1.0, 0.0, 3.0), Vector3(0.0, 0.0, -1.0)), 0);
		const ControlVariate::PathVertex& primary = session.lastPath().back();
		TASK_CHECK(primary.key.object == fixture.sphereA.get());
		TASK_CHECK(primary.key.object != fixture.sphereB.get());
		TASK_CHECK(primary.key.remainingDepth == 2);
		return (0);
	}

	int	epochFrozenModelAndEmpty(void)
	{
		Scene scene;
		TransportScene fixture = buildTransportScene(scene, 0.2);
		ControlVariate::Config config = validConfig(2);
		config.minSamples = 9;
		ControlVariate::Session session(scene, config);

		TASK_CHECK(session.epoch() == 0);
		TASK_CHECK(session.modelEntries().empty());

		// Empty epochs publish a zero model and advance the counter.
		TASK_CHECK(session.finishEpoch() == 1);
		TASK_CHECK(session.modelEntries().empty());
		TASK_CHECK(session.finishEpoch() == 2);
		TASK_CHECK(session.epoch() == 2);

		// Train for real; the model is only published at finishEpoch.
		ControlVariate::Session trained(scene, config);
		const Ray probe(Vector3(-1.0, 0.0, 3.0), Vector3(0.0, 0.0, -1.0));
		for (std::uint64_t i = 0; i < 64; i++)
		{
			trained.sample(probe, i);
		}
		TASK_CHECK(trained.modelEntries().empty());
		const std::vector<ControlVariate::NormalEquationEntry> before = trained.trainingEquations();
		TASK_CHECK(!before.empty());

		TASK_CHECK(trained.finishEpoch() == 1);
		TASK_CHECK(trained.trainingEquations().empty());
		TASK_CHECK(!trained.modelEntries().empty());

		// The published equations match the pre-finish snapshot, and the
		// regularized matrix is the average normal matrix plus ridge * I.
		const std::vector<ControlVariate::NormalEquationEntry> fitted = trained.lastFittedEquations();
		TASK_CHECK(fitted.size() == before.size());
		for (std::size_t e = 0; e < fitted.size(); e++)
		{
			TASK_CHECK(fitted[e].key == before[e].key);
			TASK_CHECK(fitted[e].sampleCount == before[e].sampleCount);
			for (int a = 0; a < 9; a++)
			{
				TASK_CHECK(nearColor(fitted[e].rhs[a], before[e].rhs[a], 1e-12));
				for (int b = 0; b < 9; b++)
				{
					const double expected = before[e].normalMatrix[a][b]
						+ (a == b ? config.ridge : 0.0);
					TASK_CHECK(near(fitted[e].regularizedMatrix[a][b], expected, 1e-12));
				}
			}
			TASK_CHECK(fitted[e].normalMatrix[0][0] > 0.0);
		}

		// The model stays frozen while sampling the next epoch.
		const std::vector<ControlVariate::ModelEntry> frozen = trained.modelEntries();
		for (std::uint64_t i = 0; i < 32; i++)
		{
			trained.sample(probe, i);
		}
		const std::vector<ControlVariate::ModelEntry> still = trained.modelEntries();
		TASK_CHECK(still.size() == frozen.size());
		for (std::size_t i = 0; i < frozen.size(); i++)
		{
			TASK_CHECK(frozen[i].key == still[i].key);
			for (int c = 0; c < 9; c++)
			{
				TASK_CHECK(nearColor(frozen[i].coefficients[c], still[i].coefficients[c], 1e-15));
			}
		}

		// Fewer than minSamples records -> zero model for that key.
		ControlVariate::Config sparseConfig = validConfig(2);
		sparseConfig.minSamples = 9;
		ControlVariate::Session sparse(scene, sparseConfig);
		for (std::uint64_t i = 0; i < 5; i++)
		{
			sparse.sample(probe, i);
		}
		sparse.finishEpoch();
		const ControlVariate::ModelEntry* sparseEntry = sparse.findModelEntry(
			ControlVariate::makeModelKey(fixture.sphereA.get(), false, 2)
		);
		TASK_CHECK(sparseEntry != nullptr);
		TASK_CHECK(sparseEntry->trainingSamples == 5);
		for (int c = 0; c < 9; c++)
		{
			TASK_CHECK(nearColor(sparseEntry->coefficients[c], Color(0.0, 0.0, 0.0), 1e-12));
		}
		return (0);
	}

	int	epochFullSuffixLabelAndReset(void)
	{
		Scene scene;

		scene.setRenderSky(SKY_NONE);
		scene.setBackgroundColor(Color(0.3, 0.4, 0.5));
		scene.setMaxLightBounces(2);
		scene.addHittable(std::make_shared<Sphere>(
			Vector3(0.0, 0.0, 0.0), 1.0,
			std::make_shared<Lambertian>(Color(0.9, 0.8, 0.7))
		));

		ControlVariate::Config config = validConfig(2);
		config.enabled = false;
		ControlVariate::Session session(scene, config);
		ControlVariate::Session isolated(scene, config);

		session.sample(Ray(Vector3(0.0, 0.0, 3.0), Vector3(0.0, 0.0, -1.0)), 0);
		const std::vector<ControlVariate::PathVertex>& path = session.lastPath();
		TASK_CHECK(path.size() == 1);

		// The label is the complete suffix Li (the sky colour), not rho * sky
		// and not the returned pixel.
		const ControlVariate::PathVertex& vertex = path.front();
		TASK_CHECK(nearColor(vertex.li, Color(0.3, 0.4, 0.5), 1e-10));
		TASK_CHECK(nearColor(vertex.estimate, Color(0.9 * 0.3, 0.8 * 0.4, 0.7 * 0.5), 1e-10));

		// A second session is unaffected.
		TASK_CHECK(isolated.epoch() == 0);
		TASK_CHECK(isolated.modelEntries().empty());
		TASK_CHECK(isolated.lastPath().empty());

		session.finishEpoch();
		TASK_CHECK(session.epoch() == 1);
		session.reset();
		TASK_CHECK(session.epoch() == 0);
		TASK_CHECK(session.epochSampleCount() == 0);
		TASK_CHECK(session.modelEntries().empty());
		TASK_CHECK(session.trainingEquations().empty());
		TASK_CHECK(session.lastFittedEquations().empty());
		TASK_CHECK(session.lastPath().empty());
		return (0);
	}

	int	epochSharedMaterialAndSides(void)
	{
		Scene scene;

		scene.setRenderSky(SKY_NONE);
		scene.setBackgroundColor(Color(0.1, 0.1, 0.1));
		scene.setMaxLightBounces(1);
		auto material = std::make_shared<Lambertian>(Color(0.5, 0.5, 0.5));
		auto triangle = std::make_shared<Triangle>(
			Vector3(-1.0, -1.0, 0.0),
			Vector3(1.0, -1.0, 0.0),
			Vector3(0.0, 1.0, 0.0),
			material
		);
		scene.addHittable(triangle);

		ControlVariate::Session session(scene, validConfig(1));

		session.sample(Ray(Vector3(0.0, 0.0, 1.0), Vector3(0.0, 0.0, -1.0)), 0);
		const ControlVariate::ModelKey frontKey = session.lastPath().back().key;
		session.sample(Ray(Vector3(0.0, 0.0, -1.0), Vector3(0.0, 0.0, 1.0)), 1);
		const ControlVariate::ModelKey backKey = session.lastPath().back().key;

		TASK_CHECK(frontKey.object == triangle.get());
		TASK_CHECK(backKey.object == triangle.get());
		TASK_CHECK(frontKey.backFace != backKey.backFace);
		TASK_CHECK(frontKey != backKey);

		// Different remaining depth on the same object/side stays distinct.
		ControlVariate::Session deeper(scene, validConfig(2));
		deeper.sample(Ray(Vector3(0.0, 0.0, 1.0), Vector3(0.0, 0.0, -1.0)), 0);
		const ControlVariate::ModelKey depth2Key = deeper.lastPath().back().key;
		TASK_CHECK(depth2Key.object == frontKey.object);
		TASK_CHECK(depth2Key.backFace == frontKey.backFace);
		TASK_CHECK(depth2Key.remainingDepth == 2);
		TASK_CHECK(depth2Key != frontKey);
		return (0);
	}

	int	epochNegativeResidualPreserved(void)
	{
		// A small bright lamp plus a dim sky is poorly represented by a
		// second-order SH model, so the trained model locally overestimates and
		// the residual Li - g takes both signs. Negative residuals must survive
		// into the estimate (no clamping), and the training label stays raw Li.
		Scene scene;

		scene.setRenderSky(SKY_NONE);
		scene.setBackgroundColor(Color(0.05, 0.1, 0.15));
		scene.setMaxLightBounces(2);
		auto floorMaterial = std::make_shared<Lambertian>(Color(0.8, 0.8, 0.8));
		auto lampMaterial = std::make_shared<Emissive>(Color(20.0, 20.0, 20.0));
		scene.addHittable(std::make_shared<Triangle>(
			Vector3(-20.0, 0.0, -20.0),
			Vector3(-20.0, 0.0, 20.0),
			Vector3(20.0, 0.0, 0.0),
			floorMaterial
		));
		scene.addHittable(std::make_shared<Triangle>(
			Vector3(-0.35, 3.0, 0.35),
			Vector3(0.35, 3.0, 0.35),
			Vector3(0.0, 3.0, -0.35),
			lampMaterial
		));

		ControlVariate::Config config = validConfig(2);
		config.minSamples = 32;
		config.ridge = 1e-6;
		ControlVariate::Session session(scene, config);

		const Ray probe(Vector3(0.0, 2.0, 2.0), Utilities::normalize(Vector3(0.0, -2.0, -2.0)));
		for (std::uint64_t i = 0; i < 20000; i++)
		{
			session.sample(probe, i);
		}
		session.finishEpoch();

		bool sawPositive = false;
		bool sawNegative = false;
		bool sawLampLabel = false;
		for (std::uint64_t i = 0; i < 20000; i++)
		{
			session.sample(probe, static_cast<std::uint64_t>(100000 + i));
			for (const ControlVariate::PathVertex& vertex : session.lastPath())
			{
				const double residualRed = vertex.li.getRed() - vertex.g.getRed();
				sawPositive = sawPositive || residualRed > 1e-6;
				sawNegative = sawNegative || residualRed < -1e-6;

				// Exact signed recursion, never clamped.
				TASK_CHECK(near(
					vertex.estimate.getRed(),
					vertex.rho.getRed() * (vertex.a.getRed() + residualRed),
					1e-12
				));
				if (residualRed < -1e-6)
				{
					TASK_CHECK(vertex.estimate.getRed() < vertex.rho.getRed() * vertex.a.getRed());
				}
				if (nearColor(vertex.li, Color(20.0, 20.0, 20.0), 1e-12))
				{
					sawLampLabel = true;
				}
			}
		}
		TASK_CHECK(sawPositive);
		TASK_CHECK(sawNegative);
		TASK_CHECK(sawLampLabel);
		return (0);
	}

	int	epochIsolationAndConfigValidation(void)
	{
		Scene scene;
		buildTransportScene(scene);
		ControlVariate::Config config = validConfig(2);

		ControlVariate::Session first(scene, config);
		ControlVariate::Session second(scene, config);

		for (std::uint64_t i = 0; i < 20; i++)
		{
			first.sample(Ray(Vector3(-1.0, 0.0, 3.0), Vector3(0.0, 0.0, -1.0)), i);
		}
		first.finishEpoch();
		TASK_CHECK(first.epoch() == 1);
		TASK_CHECK(second.epoch() == 0);
		TASK_CHECK(!first.modelEntries().empty());
		TASK_CHECK(second.modelEntries().empty());

		auto expectThrow = [](const Scene& target, ControlVariate::Config bad)
		{
			try
			{
				ControlVariate::Session session(target, bad);
				(void)session;
			}
			catch (const std::invalid_argument&)
			{
				return (true);
			}
			return (false);
		};

		TASK_CHECK(expectThrow(scene, validConfig(13)));
		TASK_CHECK(expectThrow(scene, validConfig(-1)));

		ControlVariate::Config zeroRidge = validConfig();
		zeroRidge.ridge = 0.0;
		TASK_CHECK(expectThrow(scene, zeroRidge));

		ControlVariate::Config negativeRidge = validConfig();
		negativeRidge.ridge = -1.0;
		TASK_CHECK(expectThrow(scene, negativeRidge));

		ControlVariate::Config infiniteRidge = validConfig();
		infiniteRidge.ridge = std::numeric_limits<double>::infinity();
		TASK_CHECK(expectThrow(scene, infiniteRidge));

		ControlVariate::Config nanRidge = validConfig();
		nanRidge.ridge = std::numeric_limits<double>::quiet_NaN();
		TASK_CHECK(expectThrow(scene, nanRidge));

		ControlVariate::Config smallSamples = validConfig();
		smallSamples.minSamples = 8;
		TASK_CHECK(expectThrow(scene, smallSamples));

		// Unsupported sky.
		Scene atmosphereScene;
		buildTransportScene(atmosphereScene);
		atmosphereScene.setRenderSky(SKY_ATMOSPHERE);
		TASK_CHECK(expectThrow(atmosphereScene, validConfig()));

		// Unsupported material.
		Scene metalScene;
		buildTransportScene(metalScene);
		metalScene.addHittable(std::make_shared<Sphere>(
			Vector3(5.0, 5.0, 5.0), 1.0, std::make_shared<Metal>(Color(1.0, 1.0, 1.0))
		));
		TASK_CHECK(expectThrow(metalScene, validConfig()));

		// Unsupported geometry.
		Scene rectangleScene;
		buildTransportScene(rectangleScene);
		rectangleScene.addHittable(std::make_shared<Rectangle>(
			Transform(), 1.0, 1.0, std::make_shared<Lambertian>(Color(0.5, 0.5, 0.5))
		));
		TASK_CHECK(expectThrow(rectangleScene, validConfig()));

		// Textured Lambertian.
		Scene texturedScene;
		buildTransportScene(texturedScene);
		auto texturedMaterial = std::make_shared<Lambertian>(Color(0.5, 0.5, 0.5));
		texturedMaterial->setTexture(
			std::make_shared<Texture>(1, 1, std::vector<Color>{Color(1.0, 1.0, 1.0)})
		);
		texturedScene.addHittable(std::make_shared<Sphere>(
			Vector3(5.0, 5.0, 5.0), 1.0, texturedMaterial
		));
		TASK_CHECK(expectThrow(texturedScene, validConfig()));
		return (0);
	}

	// --- Case group: variance ----------------------------------------------

	ControlVariate::Config	linearSkyConfig(int maxDepth, bool enabled, std::uint32_t seed, int minSamples)
	{
		ControlVariate::Config config = validConfig(maxDepth);
		config.enabled = enabled;
		config.seed = seed;
		config.minSamples = minSamples;
		config.ridge = 1e-6;
		return (config);
	}

	int	varianceTrainedBeatsColdStart(void)
	{
		Scene scene;
		buildLinearSkyFloorScene(scene);
		const Ray probe(Vector3(0.0, 2.0, 3.0), Utilities::normalize(Vector3(0.0, -2.0, -3.0)));

		ControlVariate::Config coldConfig = linearSkyConfig(2, false, 424242u, 32);
		ControlVariate::Config trainConfig = linearSkyConfig(2, true, 424242u, 32);
		constexpr std::size_t TRAINING_SAMPLES = 4096;

		// Independent reference mean from a large cold-start run.
		constexpr std::size_t REFERENCE_COUNT = 200000;
		double referenceSum = 0.0;
		{
			ControlVariate::Session reference(scene, coldConfig);
			for (std::size_t i = 0; i < REFERENCE_COUNT; i++)
			{
				referenceSum += Utilities::luminance(
					reference.sample(probe, static_cast<std::uint64_t>(i))
				);
			}
		}
		const double referenceMean = referenceSum / static_cast<double>(REFERENCE_COUNT);

		// At least 16 fixed-seed batches with identical sample counts. Each
		// trained batch re-fits the same deterministic model.
		constexpr std::size_t BATCH_COUNT = 16;
		constexpr std::size_t BATCH_SAMPLES = 512;
		std::vector<double> coldMeans;
		std::vector<double> trainedMeans;

		for (std::size_t batch = 0; batch < BATCH_COUNT; batch++)
		{
			const std::uint64_t base = 2000000 + batch * BATCH_SAMPLES;

			ControlVariate::Session cold(scene, coldConfig);
			double coldSum = 0.0;
			for (std::size_t i = 0; i < BATCH_SAMPLES; i++)
			{
				coldSum += Utilities::luminance(cold.sample(probe, base + i));
			}
			coldMeans.push_back(coldSum / static_cast<double>(BATCH_SAMPLES));

			ControlVariate::Session trained(scene, trainConfig);
			for (std::uint64_t i = 0; i < TRAINING_SAMPLES; i++)
			{
				trained.sample(probe, i);
			}
			trained.finishEpoch();
			double trainedSum = 0.0;
			for (std::size_t i = 0; i < BATCH_SAMPLES; i++)
			{
				trainedSum += Utilities::luminance(trained.sample(probe, base + i));
			}
			trainedMeans.push_back(trainedSum / static_cast<double>(BATCH_SAMPLES));
		}

		auto meanAndVariance = [](const std::vector<double>& values)
		{
			double mean = 0.0;
			for (double value : values)
			{
				mean += value;
			}
			mean /= static_cast<double>(values.size());
			double variance = 0.0;
			for (double value : values)
			{
				const double delta = value - mean;
				variance += delta * delta;
			}
			return (std::pair<double, double>{ mean, variance / static_cast<double>(values.size()) });
		};

		const auto cold = meanAndVariance(coldMeans);
		const auto trained = meanAndVariance(trainedMeans);

		std::cout
			<< "VARIANCE batch_samples=" << BATCH_SAMPLES
			<< " batches=" << BATCH_COUNT
			<< " cold_mean=" << cold.first
			<< " cold_var=" << cold.second
			<< " trained_mean=" << trained.first
			<< " trained_var=" << trained.second
			<< " reference_mean=" << referenceMean
			<< std::endl;

		TASK_CHECK(cold.second > 0.0);
		TASK_CHECK(trained.second < cold.second / 4.0);
		TASK_CHECK(std::fabs(trained.first - referenceMean) < 0.1 * std::max(1.0, std::fabs(referenceMean)));
		TASK_CHECK(std::fabs(cold.first - referenceMean) < 0.1 * std::max(1.0, std::fabs(referenceMean)));
		return (0);
	}

	int	varianceDeterministicStreams(void)
	{
		// Same seed, epoch, sampleId and depth must yield identical cosine
		// directions regardless of the control variate being enabled.
		Scene scene;

		scene.setRenderSky(SKY_NONE);
		scene.setBackgroundColor(Color(0.2, 0.2, 0.2));
		scene.setMaxLightBounces(2);
		scene.addHittable(std::make_shared<Sphere>(
			Vector3(0.0, 0.0, 0.0), 1.0, std::make_shared<Lambertian>(Color(0.5, 0.5, 0.5))
		));

		ControlVariate::Config onConfig = validConfig(2);
		onConfig.seed = 7u;
		ControlVariate::Config offConfig = onConfig;
		offConfig.enabled = false;

		ControlVariate::Session on(scene, onConfig);
		ControlVariate::Session off(scene, offConfig);

		const Ray ray(Vector3(0.0, 0.0, 3.0), Vector3(0.0, 0.0, -1.0));
		for (std::uint64_t sampleId = 0; sampleId < 10; sampleId++)
		{
			on.sample(ray, sampleId);
			off.sample(ray, sampleId);
			const std::vector<ControlVariate::PathVertex>& onPath = on.lastPath();
			const std::vector<ControlVariate::PathVertex>& offPath = off.lastPath();
			TASK_CHECK(onPath.size() == offPath.size());
			for (std::size_t i = 0; i < onPath.size(); i++)
			{
				TASK_CHECK(nearVector(onPath[i].direction, offPath[i].direction, 1e-15));
				TASK_CHECK(nearColor(onPath[i].li, offPath[i].li, 1e-15));
				TASK_CHECK(nearColor(onPath[i].estimate, offPath[i].estimate, 1e-15));
			}
		}

		// Reading diagnostics consumes no randomness: a fresh identical
		// session reproduces exactly the same sample.
		ControlVariate::Session replay(scene, offConfig);
		const Color firstRun = off.sample(ray, 999);
		for (int i = 0; i < 5; i++)
		{
			(void)on.lastPath();
			(void)on.modelEntries();
			(void)on.trainingEquations();
		}
		const Color replayRun = replay.sample(ray, 999);
		TASK_CHECK(nearColor(firstRun, replayRun, 1e-15));
		return (0);
	}
}

extern const struct task_case task_cases[] = {
	{"harmonics", "basis_matches_reference", harmonicBasisMatchesReference},
	{"harmonics", "cosine_integral_quadrature", harmonicCosineIntegralQuadrature},
	{"transport", "sky_and_depth_zero", transportSkyAndDepthZero},
	{"transport", "constant_sky_lambertian_mean", transportConstantSkyLambertianMean},
	{"transport", "finite_lamp_occlusion_two_bounces", transportFiniteLampOcclusionAndTwoBounces},
	{"epochs", "keys_do_not_merge", epochKeysDoNotMerge},
	{"epochs", "frozen_model_and_empty", epochFrozenModelAndEmpty},
	{"epochs", "full_suffix_label_and_reset", epochFullSuffixLabelAndReset},
	{"epochs", "shared_material_and_sides", epochSharedMaterialAndSides},
	{"epochs", "negative_residual_preserved", epochNegativeResidualPreserved},
	{"epochs", "isolation_and_config_validation", epochIsolationAndConfigValidation},
	{"variance", "trained_beats_cold_start", varianceTrainedBeatsColdStart},
	{"variance", "deterministic_streams", varianceDeterministicStreams}
};

extern const std::size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
