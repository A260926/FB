#include "Renderer/ControlVariate.hpp"

#include "RendererInternal.hpp"

#include "Defaults.hpp"
#include "Hittables/Hittable.hpp"
#include "Hittables/Sphere.hpp"
#include "Hittables/Triangle.hpp"
#include "Materials/Material.hpp"
#include "ONB.hpp"
#include "Scene/Scene.hpp"
#include "SkyTypes.hpp"
#include "Utilities.hpp"

#include <algorithm>
#include <cmath>
#include <functional>
#include <stdexcept>
#include <string>

namespace ControlVariate
{
	namespace
	{
		constexpr double	SH_Y00 = 0.28209479177387814;	// sqrt(1 / 4pi)
		constexpr double	SH_Y1 = 0.4886025119029199;	// sqrt(3 / 4pi)
		constexpr double	SH_Y2M2 = 1.0925484305920792;	// sqrt(15 / 4pi)
		constexpr double	SH_Y20 = 0.31539156525252005;	// sqrt(5 / 16pi)
		constexpr double	SH_Y22 = 0.5462742152960396;	// sqrt(15 / 16pi)

		constexpr std::uint64_t	SPLIT_MIX_INCREMENT = 0x9e3779b97f4a7c15ull;
		constexpr std::uint64_t	SPLIT_MIX_MULTIPLIER_1 = 0xbf58476d1ce4e5b9ull;
		constexpr std::uint64_t	SPLIT_MIX_MULTIPLIER_2 = 0x94d049bb133111ebull;

		std::uint64_t	splitMix64(std::uint64_t value)
		{
			value += SPLIT_MIX_INCREMENT;
			value = (value ^ (value >> 30)) * SPLIT_MIX_MULTIPLIER_1;
			value = (value ^ (value >> 27)) * SPLIT_MIX_MULTIPLIER_2;
			return (value ^ (value >> 31));
		}

		double	unitFromHash(std::uint64_t hash)
		{
			return (static_cast<double>(hash >> 11) * (1.0 / 9007199254740992.0));
		}

		Vector3	safeNormalize(const Vector3& vector, const Vector3& fallback)
		{
			const double lengthSquared = Utilities::vectorLengthSquared(vector);

			if (!std::isfinite(lengthSquared) || lengthSquared <= 1e-24)
			{
				return (fallback);
			}
			return (vector / std::sqrt(lengthSquared));
		}

		// Deterministic cosine-weighted direction. Depends only on the seed, the
		// epoch, the sample id and the depth along the path, never on the model.
		Vector3	cosineDirection(
			std::uint32_t seed,
			std::uint64_t epoch,
			std::uint64_t sampleId,
			int pathDepth,
			const Vector3& normal
		)
		{
			std::uint64_t hash = splitMix64(SPLIT_MIX_INCREMENT ^ (static_cast<std::uint64_t>(seed) + 1ull));
			hash = splitMix64(hash ^ ((epoch + 1ull) * SPLIT_MIX_MULTIPLIER_1));
			hash = splitMix64(hash ^ ((sampleId + 1ull) * SPLIT_MIX_MULTIPLIER_2));
			hash = splitMix64(hash ^ ((static_cast<std::uint64_t>(pathDepth) + 1ull) * 0xda942042e4dd58b5ull));

			const double u1 = unitFromHash(hash);
			const double u2 = unitFromHash(splitMix64(hash ^ 0xd1b54a32d192ed03ull));

			const double phi = 2.0 * D_PI * u1;
			const double radius = std::sqrt(u2);
			const double height = std::sqrt(std::max(0.0, 1.0 - u2));

			const ONB basis(normal);
			const Vector3 local(
				radius * std::cos(phi),
				radius * std::sin(phi),
				height
			);

			return (safeNormalize(basis.local(local), normal));
		}

		Matrix	regularizedMatrix(const Matrix& normalMatrix, double ridge)
		{
			Matrix result = normalMatrix;

			for (int i = 0; i < SH_COEFFICIENT_COUNT; i++)
			{
				result[i][i] += ridge;
			}
			return (result);
		}

		// Solves A x = b with partial-pivoting Gauss-Jordan elimination. A is
		// symmetric positive definite (ridge > 0), so a solution always exists.
		std::array<double, SH_COEFFICIENT_COUNT>	solveSystem(
			const Matrix& matrix,
			const std::array<double, SH_COEFFICIENT_COUNT>& rhs
		)
		{
			const int n = SH_COEFFICIENT_COUNT;
			Matrix a = matrix;
			std::array<double, SH_COEFFICIENT_COUNT> b = rhs;

			for (int column = 0; column < n; column++)
			{
				int pivotRow = column;
				double pivotMagnitude = std::fabs(a[column][column]);

				for (int row = column + 1; row < n; row++)
				{
					const double magnitude = std::fabs(a[row][column]);
					if (magnitude > pivotMagnitude)
					{
						pivotMagnitude = magnitude;
						pivotRow = row;
					}
				}
				if (pivotMagnitude <= 1e-18)
				{
					continue;
				}
				if (pivotRow != column)
				{
					std::swap(a[pivotRow], a[column]);
					std::swap(b[pivotRow], b[column]);
				}

				const double pivot = a[column][column];
				for (int entry = column; entry < n; entry++)
				{
					a[column][entry] /= pivot;
				}
				b[column] /= pivot;

				for (int row = 0; row < n; row++)
				{
					if (row == column)
					{
						continue;
					}
					const double factor = a[row][column];
					if (factor == 0.0)
					{
						continue;
					}
					for (int entry = column; entry < n; entry++)
					{
						a[row][entry] -= factor * a[column][entry];
					}
					b[row] -= factor * b[column];
				}
			}

			return (b);
		}

		bool	isSupportedHittable(const Hittable* hittable)
		{
			return (
				dynamic_cast<const Sphere*>(hittable) != nullptr
				|| dynamic_cast<const Triangle*>(hittable) != nullptr
			);
		}
	}

	ShVector	shBasis(const Vector3& direction)
	{
		ShVector basis{};
		const double lengthSquared = Utilities::vectorLengthSquared(direction);

		if (!std::isfinite(lengthSquared) || lengthSquared <= 1e-24)
		{
			return (basis);
		}

		const Vector3 unit = direction / std::sqrt(lengthSquared);
		const double x = unit.getX();
		const double y = unit.getY();
		const double z = unit.getZ();

		basis[0] = SH_Y00;
		basis[1] = SH_Y1 * y;
		basis[2] = SH_Y1 * z;
		basis[3] = SH_Y1 * x;
		basis[4] = SH_Y2M2 * x * y;
		basis[5] = SH_Y2M2 * y * z;
		basis[6] = SH_Y20 * (3.0 * z * z - 1.0);
		basis[7] = SH_Y2M2 * x * z;
		basis[8] = SH_Y22 * (x * x - y * y);

		return (basis);
	}

	ShVector	shCosineIntegral(const Vector3& normal)
	{
		const ShVector directionBasis = shBasis(normal);
		ShVector integral{};

		// Convolution with the zonal cosine kernel (n.w)_+/pi is diagonal in
		// l, with alpha_l = 2 * integral_0^1 mu * P_l(mu) dmu:
		// alpha_0 = 1, alpha_1 = 2/3, alpha_2 = 1/4.
		integral[0] = directionBasis[0];
		for (int i = 1; i <= 3; i++)
		{
			integral[i] = (2.0 / 3.0) * directionBasis[i];
		}
		for (int i = 4; i <= 8; i++)
		{
			integral[i] = 0.25 * directionBasis[i];
		}
		return (integral);
	}

	Color	evaluateSh(const ShCoefficients& coefficients, const ShVector& basis)
	{
		double red = 0.0;
		double green = 0.0;
		double blue = 0.0;

		for (int i = 0; i < SH_COEFFICIENT_COUNT; i++)
		{
			red += coefficients[i].getRed() * basis[i];
			green += coefficients[i].getGreen() * basis[i];
			blue += coefficients[i].getBlue() * basis[i];
		}
		return (Color(red, green, blue));
	}

	bool	ModelKey::operator<(const ModelKey& other) const
	{
		if (std::less<const Hittable*>()(this->object, other.object))
		{
			return (true);
		}
		if (std::less<const Hittable*>()(other.object, this->object))
		{
			return (false);
		}
		if (this->backFace != other.backFace)
		{
			return (this->backFace < other.backFace);
		}
		return (this->remainingDepth < other.remainingDepth);
	}

	bool	ModelKey::operator==(const ModelKey& other) const
	{
		return (
			this->object == other.object
			&& this->backFace == other.backFace
			&& this->remainingDepth == other.remainingDepth
		);
	}

	bool	ModelKey::operator!=(const ModelKey& other) const
	{
		return (!(*this == other));
	}

	ModelKey	makeModelKey(const Hittable* object, bool backFace, int remainingDepth)
	{
		ModelKey key;

		key.object = object;
		key.backFace = backFace;
		key.remainingDepth = remainingDepth;
		return (key);
	}

	Session::Session(const Scene& scene, const Config& config)
		: _scene(&scene), _config(config)
	{
		if (config.maxDepth < 0 || config.maxDepth > 12)
		{
			throw std::invalid_argument("ControlVariate::Config.maxDepth must be in [0, 12].");
		}
		if (!std::isfinite(config.ridge) || config.ridge <= 0.0)
		{
			throw std::invalid_argument("ControlVariate::Config.ridge must be finite and positive.");
		}
		if (config.minSamples < SH_COEFFICIENT_COUNT)
		{
			throw std::invalid_argument("ControlVariate::Config.minSamples must be at least 9.");
		}

		const SkyTypes sky = scene.getRenderSky();
		if (sky != SKY_NONE && sky != SKY_LINEAR)
		{
			throw std::invalid_argument(
				"ControlVariate sessions only support SKY_NONE and SKY_LINEAR."
			);
		}

		for (const std::shared_ptr<Hittable>& hittable : scene.getHittables())
		{
			if (!hittable)
			{
				throw std::invalid_argument("ControlVariate scenes cannot contain null hittables.");
			}
			if (!isSupportedHittable(hittable.get()))
			{
				throw std::invalid_argument(
					"ControlVariate sessions only support standalone Sphere and Triangle geometry."
				);
			}

			const Material* material = hittable->getMaterial();
			if (material == nullptr)
			{
				throw std::invalid_argument("ControlVariate geometry must have a material.");
			}
			const MaterialType type = material->getType();
			if (type != LAMBERTIAN && type != EMISSIVE)
			{
				throw std::invalid_argument(
					"ControlVariate sessions only support Lambertian and Emissive materials."
				);
			}
			if (material->hasTexture())
			{
				throw std::invalid_argument(
					"ControlVariate sessions do not support textured materials."
				);
			}
		}
	}

	Color	Session::skyRadiance(const Ray& ray) const
	{
		const Scene& scene = *this->_scene;

		if (scene.getRenderSky() == SKY_LINEAR)
		{
			const Vector3 direction = safeNormalize(ray.getDirection(), Vector3(0.0, 1.0, 0.0));
			const double skyLine = scene.getSkyline();
			const double blend = skyLine * (direction.getY() + 1.0);

			return (
				Color(1.0, 1.0, 1.0) * (1.0 - blend)
				+ Color(0.5, 0.7, 1.0) * blend
			);
		}
		return (scene.getBackgroundColor());
	}

	Color	Session::modelValue(const ModelKey& key, const ShVector& basis) const
	{
		const auto iterator = this->_model.find(key);
		if (iterator == this->_model.end())
		{
			return (Color(0.0, 0.0, 0.0));
		}
		return (evaluateSh(iterator->second.coefficients, basis));
	}

	Color	Session::analyticMean(const ModelKey& key, const ShVector& integral) const
	{
		const auto iterator = this->_model.find(key);
		if (iterator == this->_model.end())
		{
			return (Color(0.0, 0.0, 0.0));
		}
		return (evaluateSh(iterator->second.coefficients, integral));
	}

	Color	Session::trace(
		const Ray& ray,
		int remainingDepth,
		std::uint64_t sampleId,
		int pathDepth,
		std::vector<PathVertex>& vertices
	)
	{
		Scene& scene = const_cast<Scene&>(*this->_scene);
		Ray currentRay = ray;
		HitRecord hitRecord;

		if (!Renderer::internal::_checkHits(scene, currentRay, hitRecord))
		{
			return (this->skyRadiance(currentRay));
		}

		Material* material = hitRecord.material;
		if (material == nullptr)
		{
			return (Color(0.0, 0.0, 0.0));
		}

		// An emissive surface terminates the path exactly like the production
		// preview's emitted() termination, at any remaining depth.
		if (material->getType() == EMISSIVE)
		{
			return (material->emitted());
		}

		if (remainingDepth <= 0)
		{
			return (Color(0.0, 0.0, 0.0));
		}

		Vector3 normal = safeNormalize(hitRecord.geometricNormal, Vector3(0.0, 1.0, 0.0));
		if (Utilities::dot(normal, currentRay.getDirection()) >= 0.0)
		{
			normal = normal * -1.0;
		}

		const Color rho = material->colorAt(hitRecord);
		const Vector3 direction = cosineDirection(
			this->_config.seed,
			this->_epoch,
			sampleId,
			pathDepth,
			normal
		);

		const Ray scattered = Ray::fromNormalizedDirection(hitRecord.position, direction);
		const Color li = this->trace(scattered, remainingDepth - 1, sampleId, pathDepth + 1, vertices);

		const ModelKey key = makeModelKey(
			hitRecord.hittable,
			!hitRecord.geometricFrontFace,
			remainingDepth
		);

		Color g(0.0, 0.0, 0.0);
		Color a(0.0, 0.0, 0.0);
		Color estimate;

		if (this->_config.enabled)
		{
			const ShVector basis = shBasis(direction);
			const ShVector integral = shCosineIntegral(normal);
			g = this->modelValue(key, basis);
			a = this->analyticMean(key, integral);
			estimate = Color(
				rho.getRed() * (a.getRed() + li.getRed() - g.getRed()),
				rho.getGreen() * (a.getGreen() + li.getGreen() - g.getGreen()),
				rho.getBlue() * (a.getBlue() + li.getBlue() - g.getBlue())
			);
		}
		else
		{
			estimate = rho * li;
		}

		if (this->_config.enabled)
		{
			Accumulator& accumulator = this->_stats[key];
			const ShVector basis = shBasis(direction);

			accumulator.count += 1;
			for (int i = 0; i < SH_COEFFICIENT_COUNT; i++)
			{
				for (int j = 0; j < SH_COEFFICIENT_COUNT; j++)
				{
					accumulator.sumPhiPhi[i][j] += basis[i] * basis[j];
				}
				accumulator.sumPhiL[i] += Color(
					basis[i] * li.getRed(),
					basis[i] * li.getGreen(),
					basis[i] * li.getBlue()
				);
			}
		}

		PathVertex vertex;
		vertex.key = key;
		vertex.normal = normal;
		vertex.direction = direction;
		vertex.li = li;
		vertex.g = g;
		vertex.a = a;
		vertex.rho = rho;
		vertex.estimate = estimate;
		vertices.push_back(vertex);

		return (estimate);
	}

	Color	Session::sample(const Ray& ray, std::uint64_t sampleId)
	{
		this->_lastPath.clear();
		this->_epochSamples += 1;

		return (this->trace(ray, this->_config.maxDepth, sampleId, 0, this->_lastPath));
	}

	std::size_t	Session::epochVertexCount(void) const
	{
		std::size_t total = 0;

		for (const auto& entry : this->_stats)
		{
			total += static_cast<std::size_t>(entry.second.count);
		}
		return (total);
	}

	std::uint64_t	Session::finishEpoch(void)
	{
		this->_lastFit.clear();
		this->_model.clear();

		for (const auto& entry : this->_stats)
		{
			const ModelKey& key = entry.first;
			const Accumulator& accumulator = entry.second;

			if (accumulator.count == 0)
			{
				continue;
			}

			const double inverseCount = 1.0 / static_cast<double>(accumulator.count);
			NormalEquationEntry equation;
			equation.key = key;
			equation.sampleCount = accumulator.count;

			for (int i = 0; i < SH_COEFFICIENT_COUNT; i++)
			{
				for (int j = 0; j < SH_COEFFICIENT_COUNT; j++)
				{
					equation.normalMatrix[i][j] = accumulator.sumPhiPhi[i][j] * inverseCount;
					equation.regularizedMatrix[i][j] = equation.normalMatrix[i][j];
				}
				equation.regularizedMatrix[i][i] += this->_config.ridge;
				equation.rhs[i] = Color(
					accumulator.sumPhiL[i].getRed() * inverseCount,
					accumulator.sumPhiL[i].getGreen() * inverseCount,
					accumulator.sumPhiL[i].getBlue() * inverseCount
				);
			}
			this->_lastFit.push_back(equation);

			ModelEntry model;
			model.key = key;
			model.trainingSamples = accumulator.count;

			if (accumulator.count >= static_cast<std::uint64_t>(this->_config.minSamples))
			{
				std::array<double, SH_COEFFICIENT_COUNT> rhsRed{};
				std::array<double, SH_COEFFICIENT_COUNT> rhsGreen{};
				std::array<double, SH_COEFFICIENT_COUNT> rhsBlue{};

				for (int i = 0; i < SH_COEFFICIENT_COUNT; i++)
				{
					rhsRed[i] = equation.rhs[i].getRed();
					rhsGreen[i] = equation.rhs[i].getGreen();
					rhsBlue[i] = equation.rhs[i].getBlue();
				}

				const std::array<double, SH_COEFFICIENT_COUNT> solvedRed =
					solveSystem(equation.regularizedMatrix, rhsRed);
				const std::array<double, SH_COEFFICIENT_COUNT> solvedGreen =
					solveSystem(equation.regularizedMatrix, rhsGreen);
				const std::array<double, SH_COEFFICIENT_COUNT> solvedBlue =
					solveSystem(equation.regularizedMatrix, rhsBlue);

				for (int i = 0; i < SH_COEFFICIENT_COUNT; i++)
				{
					model.coefficients[i] = Color(solvedRed[i], solvedGreen[i], solvedBlue[i]);
				}
			}
			this->_model[key] = model;
		}

		this->_stats.clear();
		this->_epochSamples = 0;
		this->_epoch += 1;

		return (this->_epoch);
	}

	void	Session::reset(void)
	{
		this->_epoch = 0;
		this->_epochSamples = 0;
		this->_model.clear();
		this->_stats.clear();
		this->_lastFit.clear();
		this->_lastPath.clear();
	}

	std::vector<ModelEntry>	Session::modelEntries(void) const
	{
		std::vector<ModelEntry> entries;

		entries.reserve(this->_model.size());
		for (const auto& entry : this->_model)
		{
			entries.push_back(entry.second);
		}
		return (entries);
	}

	const ModelEntry*	Session::findModelEntry(const ModelKey& key) const
	{
		const auto iterator = this->_model.find(key);
		if (iterator == this->_model.end())
		{
			return (nullptr);
		}
		return (&iterator->second);
	}

	std::vector<NormalEquationEntry>	Session::trainingEquations(void) const
	{
		std::vector<NormalEquationEntry> equations;

		equations.reserve(this->_stats.size());
		for (const auto& entry : this->_stats)
		{
			const Accumulator& accumulator = entry.second;
			if (accumulator.count == 0)
			{
				continue;
			}

			const double inverseCount = 1.0 / static_cast<double>(accumulator.count);
			NormalEquationEntry equation;
			equation.key = entry.first;
			equation.sampleCount = accumulator.count;

			for (int i = 0; i < SH_COEFFICIENT_COUNT; i++)
			{
				for (int j = 0; j < SH_COEFFICIENT_COUNT; j++)
				{
					equation.normalMatrix[i][j] = accumulator.sumPhiPhi[i][j] * inverseCount;
				}
				equation.rhs[i] = Color(
					accumulator.sumPhiL[i].getRed() * inverseCount,
					accumulator.sumPhiL[i].getGreen() * inverseCount,
					accumulator.sumPhiL[i].getBlue() * inverseCount
				);
			}
			equation.regularizedMatrix = regularizedMatrix(
				equation.normalMatrix,
				this->_config.ridge
			);
			equations.push_back(equation);
		}
		return (equations);
	}

	std::vector<NormalEquationEntry>	Session::lastFittedEquations(void) const
	{
		return (this->_lastFit);
	}
}
