#include "Renderer/ControlVariate.hpp"
#include "RendererInternal.hpp"
#include "Color.hpp"
#include "Defaults.hpp"
#include "Hittables/Sphere.hpp"
#include "Hittables/Triangle.hpp"
#include "Materials/Emissive.hpp"
#include "Materials/Lambertian.hpp"
#include "ONB.hpp"
#include "Scene/Scene.hpp"
#include "SkyTypes.hpp"
#include "Utilities.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <stdexcept>

namespace
{
	constexpr double	SH_PI = 3.14159265358979323846;
	constexpr double	HIT_IDENTITY_EPSILON = 1.0e-7;

	std::uint64_t	splitMix64(std::uint64_t value)
	{
		value += 0x9e3779b97f4a7c15ull;
		value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ull;
		value = (value ^ (value >> 27)) * 0x94d049bb133111ebull;
		return (value ^ (value >> 31));
	}

	double	unitUniform(std::uint64_t hash)
	{
		return (static_cast<double>(hash >> 11) * (1.0 / 9007199254740992.0));
	}

	bool	sameHitDistance(double a, double b)
	{
		const double scale = std::max(1.0, std::fabs(a));

		return (std::fabs(a - b) <= HIT_IDENTITY_EPSILON * scale);
	}

	Color	sumWeighted(const ControlVariate::ModelCoefficients& coefficients, const std::array<double, ControlVariate::SH_COEFFICIENT_COUNT>& weights)
	{
		Color	result(0.0, 0.0, 0.0);

		for (std::size_t i = 0; i < ControlVariate::SH_COEFFICIENT_COUNT; i++)
		{
			result += coefficients[i] * weights[i];
		}
		return (result);
	}

	// Gauss-Jordan solve of A x = b for one channel of the 9x9 ridge system.
	bool	solveChannel(
		std::array<std::array<double, ControlVariate::SH_COEFFICIENT_COUNT>, ControlVariate::SH_COEFFICIENT_COUNT> matrix,
		std::array<double, ControlVariate::SH_COEFFICIENT_COUNT> rightHandSide,
		std::array<double, ControlVariate::SH_COEFFICIENT_COUNT>& solution
	)
	{
		for (std::size_t column = 0; column < ControlVariate::SH_COEFFICIENT_COUNT; column++)
		{
			std::size_t pivot = column;
			for (std::size_t row = column + 1; row < ControlVariate::SH_COEFFICIENT_COUNT; row++)
			{
				if (std::fabs(matrix[row][column]) > std::fabs(matrix[pivot][column]))
				{
					pivot = row;
				}
			}
			if (!std::isfinite(matrix[pivot][column]) || std::fabs(matrix[pivot][column]) < 1.0e-15)
			{
				return (false);
			}
			if (pivot != column)
			{
				for (std::size_t k = 0; k < ControlVariate::SH_COEFFICIENT_COUNT; k++)
				{
					std::swap(matrix[column][k], matrix[pivot][k]);
				}
				std::swap(rightHandSide[column], rightHandSide[pivot]);
			}

			const double pivotValue = matrix[column][column];
			for (std::size_t k = 0; k < ControlVariate::SH_COEFFICIENT_COUNT; k++)
			{
				matrix[column][k] /= pivotValue;
			}
			rightHandSide[column] /= pivotValue;

			for (std::size_t row = 0; row < ControlVariate::SH_COEFFICIENT_COUNT; row++)
			{
				if (row == column)
				{
					continue;
				}
				const double factor = matrix[row][column];
				if (factor == 0.0)
				{
					continue;
				}
				for (std::size_t k = 0; k < ControlVariate::SH_COEFFICIENT_COUNT; k++)
				{
					matrix[row][k] -= factor * matrix[column][k];
				}
				rightHandSide[row] -= factor * rightHandSide[column];
			}
		}
		for (std::size_t i = 0; i < ControlVariate::SH_COEFFICIENT_COUNT; i++)
		{
			solution[i] = rightHandSide[i];
			if (!std::isfinite(solution[i]))
			{
				return (false);
			}
		}
		return (true);
	}
}

namespace ControlVariate
{
	std::array<double, SH_COEFFICIENT_COUNT>	evaluateBasis(const Vector3& direction)
	{
		const double x = direction.getX();
		const double y = direction.getY();
		const double z = direction.getZ();
		const double sqrtPi = std::sqrt(SH_PI);
		const double k1 = 0.5 * std::sqrt(3.0 / SH_PI);
		const double k2 = 0.5 * std::sqrt(15.0 / SH_PI);
		const double k4 = 0.25 * std::sqrt(5.0 / SH_PI);
		std::array<double, SH_COEFFICIENT_COUNT>	basis{};

		basis[0] = 1.0 / (2.0 * sqrtPi);
		basis[1] = k1 * y;
		basis[2] = k1 * z;
		basis[3] = k1 * x;
		basis[4] = k2 * (x * y);
		basis[5] = k2 * (y * z);
		basis[6] = k4 * ((3.0 * z * z) - 1.0);
		basis[7] = k2 * (x * z);
		basis[8] = 0.5 * k2 * ((x * x) - (y * y));
		return (basis);
	}

	// Analytic values of H_i(n) = integral over the hemisphere around n of
	// Y_i(w) (n.w) / pi dw.  They follow from the cosine-weighted hemisphere
	// moments E[1] = 1, E[w_i] = 2 n_i / 3 and
	// E[w_i w_j] = (delta_ij + n_i n_j) / 4.
	std::array<double, SH_COEFFICIENT_COUNT>	hemisphereCosineIntegrals(const Vector3& normal)
	{
		const double nx = normal.getX();
		const double ny = normal.getY();
		const double nz = normal.getZ();
		const double sqrtPi = std::sqrt(SH_PI);
		const double k1 = 0.5 * std::sqrt(3.0 / SH_PI);
		const double k2 = 0.5 * std::sqrt(15.0 / SH_PI);
		const double k4 = 0.25 * std::sqrt(5.0 / SH_PI);
		std::array<double, SH_COEFFICIENT_COUNT>	integrals{};

		integrals[0] = 1.0 / (2.0 * sqrtPi);
		integrals[1] = k1 * (2.0 / 3.0) * ny;
		integrals[2] = k1 * (2.0 / 3.0) * nz;
		integrals[3] = k1 * (2.0 / 3.0) * nx;
		integrals[4] = k2 * 0.25 * nx * ny;
		integrals[5] = k2 * 0.25 * ny * nz;
		integrals[6] = k4 * ((3.0 * nz * nz) - 1.0) * 0.25;
		integrals[7] = k2 * 0.25 * nx * nz;
		integrals[8] = k2 * 0.125 * (nx * nx - ny * ny);
		return (integrals);
	}

	bool	ModelKey::operator<(const ModelKey& other) const
	{
		if (this->objectIndex != other.objectIndex)
		{
			return (this->objectIndex < other.objectIndex);
		}
		if (this->frontFace != other.frontFace)
		{
			return (this->frontFace < other.frontFace);
		}
		return (this->depth < other.depth);
	}

	bool	ModelKey::operator==(const ModelKey& other) const
	{
		return (
			this->object == other.object
			&& this->objectIndex == other.objectIndex
			&& this->frontFace == other.frontFace
			&& this->depth == other.depth
		);
	}

	struct	ControlVariateSession::EpochSlot
	{
		NormalEquations	equations;

		// Running per-sample averages of Y Y^T and Y Li.
		void	accumulate(
			const std::array<double, SH_COEFFICIENT_COUNT>& basis,
			const Color& label
		)
		{
			this->equations.sampleCount++;
			const double blend = 1.0 / static_cast<double>(this->equations.sampleCount);

			for (std::size_t i = 0; i < SH_COEFFICIENT_COUNT; i++)
			{
				for (std::size_t j = 0; j < SH_COEFFICIENT_COUNT; j++)
				{
					const double moment = basis[i] * basis[j];
					this->equations.matrix[i][j] += (moment - this->equations.matrix[i][j]) * blend;
				}
				const Color rhsMoment(
					basis[i] * label.getRed(),
					basis[i] * label.getGreen(),
					basis[i] * label.getBlue()
				);
				this->equations.rhs[i] += (rhsMoment - this->equations.rhs[i]) * blend;
			}
		}
	};

	ControlVariateSession::ControlVariateSession(const Scene& scene, const Configuration& configuration)
		: _scene(scene), _configuration(configuration), _epoch(0), _enabled(configuration.enabled)
	{
		if (configuration.maxDepth < 0 || configuration.maxDepth > MAX_SUPPORTED_DEPTH)
		{
			throw std::invalid_argument("ControlVariate maxDepth must be between 0 and 12.");
		}
		if (!std::isfinite(configuration.ridge) || configuration.ridge <= 0.0)
		{
			throw std::invalid_argument("ControlVariate ridge must be finite and positive.");
		}
		if (configuration.minSamples < MIN_REQUIRED_SAMPLES)
		{
			throw std::invalid_argument("ControlVariate minSamples must be at least 9.");
		}
		this->_seed = configuration.seed;
		this->validateScene();
	}

	ControlVariateSession::~ControlVariateSession(void) = default;

	void	ControlVariateSession::validateScene(void)
	{
		const SkyTypes skyType = this->_scene.getRenderSky();

		if (skyType != SKY_NONE && skyType != SKY_LINEAR)
		{
			throw std::invalid_argument("ControlVariate only supports SKY_NONE and SKY_LINEAR skies.");
		}
		if (this->_scene.hasEnvironmentMap())
		{
			throw std::invalid_argument("ControlVariate does not support environment maps.");
		}

		const auto& hittables = this->_scene.getHittables();
		for (const std::shared_ptr<Hittable>& hittable : hittables)
		{
			if (!hittable)
			{
				throw std::invalid_argument("ControlVariate does not support empty scene objects.");
			}
			const Sphere* sphere = dynamic_cast<const Sphere*>(hittable.get());
			const Triangle* triangle = dynamic_cast<const Triangle*>(hittable.get());
			if (sphere == nullptr && triangle == nullptr)
			{
				throw std::invalid_argument("ControlVariate only supports standalone Sphere and Triangle geometry.");
			}
			Material* material = hittable->getMaterial();
			if (material == nullptr)
			{
				throw std::invalid_argument("ControlVariate requires every object to carry a material.");
			}
			const MaterialType type = material->getType();
			if (type != LAMBERTIAN && type != EMISSIVE)
			{
				throw std::invalid_argument("ControlVariate only supports Lambertian and Emissive materials.");
			}

			// No public texture query exists on Material; a textured material
			// changes colorAt() relative to its constant base color.
			const Color baseColor = material->getColor();
			const double probeUV[4][2] = {{0.25, 0.75}, {0.7, 0.15}, {0.45, 0.55}, {0.9, 0.8}};
			for (const auto& uv : probeUV)
			{
				HitRecord probe;
				probe.u = uv[0];
				probe.v = uv[1];
				const Color sampled = material->colorAt(probe);
				if (
					std::fabs(sampled.getRed() - baseColor.getRed()) > 1.0e-12
					|| std::fabs(sampled.getGreen() - baseColor.getGreen()) > 1.0e-12
					|| std::fabs(sampled.getBlue() - baseColor.getBlue()) > 1.0e-12
				)
				{
					throw std::invalid_argument("ControlVariate only supports untextured materials.");
				}
			}
			this->_supportedObjects.push_back(hittable);
		}
	}

	const Configuration&	ControlVariateSession::getConfiguration(void) const
	{
		return (this->_configuration);
	}

	std::size_t	ControlVariateSession::getEpoch(void) const
	{
		return (this->_epoch);
	}

	bool	ControlVariateSession::isEnabled(void) const
	{
		return (this->_enabled);
	}

	const std::map<ModelKey, FrozenModel>&	ControlVariateSession::getModels(void) const
	{
		return (this->_models);
	}

	std::size_t	ControlVariateSession::getEpochSampleCount(const ModelKey& key) const
	{
		const auto slot = this->_epochSlots.find(key);
		if (slot == this->_epochSlots.end())
		{
			return (0);
		}
		return (slot->second.equations.sampleCount);
	}

	const NormalEquations*	ControlVariateSession::getEpochEquations(const ModelKey& key) const
	{
		const auto slot = this->_epochSlots.find(key);
		if (slot == this->_epochSlots.end())
		{
			return (nullptr);
		}
		return (&slot->second.equations);
	}

	std::vector<ModelKey>	ControlVariateSession::getEpochKeys(void) const
	{
		std::vector<ModelKey>	keys;
		keys.reserve(this->_epochSlots.size());
		for (const auto& entry : this->_epochSlots)
		{
			keys.push_back(entry.first);
		}
		return (keys);
	}

	const std::vector<PathVertex>&	ControlVariateSession::getLastPathVertices(void) const
	{
		return (this->_pathVertices);
	}

	Vector3	ControlVariateSession::cosineDirection(
		const Vector3& normal,
		std::uint64_t sampleId,
		int depth
	) const
	{
		std::uint64_t hash = this->_seed;
		hash = splitMix64(hash + 0x123456789abcdef0ull + this->_epoch);
		hash = splitMix64(hash + sampleId);
		hash = splitMix64(hash + (static_cast<std::uint64_t>(depth) * 0x9e3779b97f4a7c15ull));
		const double u1 = unitUniform(splitMix64(hash));
		const double u2 = unitUniform(splitMix64(hash + 1ull));
		const double radius = std::sqrt(std::clamp(u1, 0.0, 1.0));
		const double phi = 2.0 * SH_PI * u2;
		const double z = std::sqrt(std::max(0.0, 1.0 - (radius * radius)));
		const ONB basis(normal);

		return (basis.local(radius * std::cos(phi), radius * std::sin(phi), z));
	}

	Color	ControlVariateSession::backgroundRadiance(const Ray& ray) const
	{
		Scene& mutableScene = const_cast<Scene&>(this->_scene);

		if (this->_scene.getRenderSky() == SKY_LINEAR)
		{
			Ray skyRay = ray;
			return (Renderer::internal::_calculateSkyInterpolation(mutableScene, skyRay));
		}
		return (this->_scene.getBackgroundColor());
	}

	const FrozenModel*	ControlVariateSession::findModel(const ModelKey& key) const
	{
		const auto model = this->_models.find(key);
		if (model == this->_models.end())
		{
			return (nullptr);
		}
		return (&model->second);
	}

	ControlVariateSession::EpochSlot&	ControlVariateSession::epochSlot(const ModelKey& key)
	{
		return (this->_epochSlots[key]);
	}

	Color	ControlVariateSession::sample(const Ray& ray, std::uint64_t sampleId)
	{
		this->_pathVertices.clear();
		const Color result = this->estimate(ray, sampleId, this->_configuration.maxDepth, true);
		std::reverse(this->_pathVertices.begin(), this->_pathVertices.end());
		return (result);
	}

	Color	ControlVariateSession::estimate(
		const Ray& ray,
		std::uint64_t sampleId,
		int depth,
		bool recordVertices
	)
	{
		Ray currentRay = ray;
		HitRecord hit;
		PathVertex vertex;

		if (!Renderer::internal::_checkHits(const_cast<Scene&>(this->_scene), currentRay, hit))
		{
			const Color sky = this->backgroundRadiance(currentRay);
			if (recordVertices)
			{
				vertex.background = true;
				vertex.direction = currentRay.getDirection();
				vertex.li = sky;
				vertex.result = sky;
				this->_pathVertices.push_back(vertex);
			}
			return (sky);
		}

		// Resolve the actual object identity; a Material pointer is never enough.
		std::size_t objectIndex = 0;
		const Hittable* object = nullptr;
		double bestDifference = 0.0;
		for (std::size_t i = 0; i < this->_supportedObjects.size(); i++)
		{
			Ray probeRay = ray;
			HitRecord probe;
			if (
				this->_supportedObjects[i]->hit(probeRay, probe, T_MIN, hit.t0 + T_MIN)
				&& sameHitDistance(probe.t0, hit.t0)
			)
			{
				const double difference = std::fabs(probe.t0 - hit.t0);
				if (object == nullptr || difference < bestDifference)
				{
					objectIndex = i;
					object = this->_supportedObjects[i].get();
					bestDifference = difference;
				}
			}
		}
		if (object == nullptr)
		{
			throw std::runtime_error("ControlVariate could not resolve the hit object identity.");
		}

		const ModelKey key{object, objectIndex, hit.frontFace, depth};
		vertex.key = key;
		vertex.position = hit.position;

		Material* material = hit.material;
		if (material->getType() == EMISSIVE)
		{
			// Emissive geometry terminates the path through the material.
			const Color emitted = material->emitted();
			vertex.emissive = true;
			vertex.normal = hit.geometricNormal;
			vertex.li = emitted;
			vertex.result = emitted;
			if (recordVertices)
			{
				this->_pathVertices.push_back(vertex);
			}
			return (emitted);
		}

		// Geometric normal on the incoming-light side, as a unit vector.
		Vector3 normal = hit.geometricNormal;
		if (Utilities::vectorLengthSquared(normal) <= 1.0e-24)
		{
			normal = hit.normal;
		}
		normal = Utilities::normalize(normal);
		const Color rho = material->colorAt(hit);
		vertex.normal = normal;
		vertex.rho = rho;

		Color li(0.0, 0.0, 0.0);
		Color g(0.0, 0.0, 0.0);
		Color a(0.0, 0.0, 0.0);
		Vector3 direction(0.0, 0.0, 0.0);
		double cosinePdf = 0.0;

		if (depth > 0)
		{
			direction = this->cosineDirection(normal, sampleId, depth);
			cosinePdf = std::max(0.0, Utilities::dot(normal, direction)) / SH_PI;
			Ray nextRay = Ray::fromNormalizedDirection(hit.position, direction);

			// Complete suffix estimate at remaining depth - 1: occlusion, later
			// emission, the environment and further bounces all enter through
			// the real production hit test and materials.
			li = this->estimate(nextRay, sampleId, depth - 1, recordVertices);

			const std::array<double, SH_COEFFICIENT_COUNT> basis = evaluateBasis(direction);
			EpochSlot& slot = this->epochSlot(key);
			// Labels are the full Li: no local rho and no camera throughput.
			slot.accumulate(basis, li);

			const FrozenModel* model = this->_enabled ? this->findModel(key) : nullptr;
			if (model != nullptr && model->active)
			{
				g = sumWeighted(model->coefficients, basis);
				const std::array<double, SH_COEFFICIENT_COUNT> integrals = hemisphereCosineIntegrals(normal);
				a = sumWeighted(model->coefficients, integrals);
			}
		}

		// rho * (A + Li - g).  Negative residuals and negative intermediate
		// radiance pass through untouched.
		const Color result = rho * (a + li - g);

		vertex.direction = direction;
		vertex.li = li;
		vertex.g = g;
		vertex.a = a;
		vertex.result = result;
		vertex.cosinePdf = cosinePdf;
		if (recordVertices)
		{
			this->_pathVertices.push_back(vertex);
		}
		return (result);
	}

	void	ControlVariateSession::clearEpoch(void)
	{
		this->_epochSlots.clear();
	}

	void	ControlVariateSession::finishEpoch(void)
	{
		std::map<ModelKey, FrozenModel> nextModels;

		for (const auto& entry : this->_epochSlots)
		{
			const ModelKey& key = entry.first;
			const NormalEquations& equations = entry.second.equations;
			FrozenModel model;
			model.key = key;
			model.equations = equations;

			if (equations.sampleCount >= this->_configuration.minSamples)
			{
				std::array<std::array<double, SH_COEFFICIENT_COUNT>, SH_COEFFICIENT_COUNT> system{};
				for (std::size_t i = 0; i < SH_COEFFICIENT_COUNT; i++)
				{
					for (std::size_t j = 0; j < SH_COEFFICIENT_COUNT; j++)
					{
						system[i][j] = equations.matrix[i][j];
					}
					system[i][i] += this->_configuration.ridge;
				}

				bool solved = true;
				ModelCoefficients coefficients{};
				const double	channelValues[3][SH_COEFFICIENT_COUNT] = {
					{
						equations.rhs[0].getRed(), equations.rhs[1].getRed(), equations.rhs[2].getRed(),
						equations.rhs[3].getRed(), equations.rhs[4].getRed(), equations.rhs[5].getRed(),
						equations.rhs[6].getRed(), equations.rhs[7].getRed(), equations.rhs[8].getRed()
					},
					{
						equations.rhs[0].getGreen(), equations.rhs[1].getGreen(), equations.rhs[2].getGreen(),
						equations.rhs[3].getGreen(), equations.rhs[4].getGreen(), equations.rhs[5].getGreen(),
						equations.rhs[6].getGreen(), equations.rhs[7].getGreen(), equations.rhs[8].getGreen()
					},
					{
						equations.rhs[0].getBlue(), equations.rhs[1].getBlue(), equations.rhs[2].getBlue(),
						equations.rhs[3].getBlue(), equations.rhs[4].getBlue(), equations.rhs[5].getBlue(),
						equations.rhs[6].getBlue(), equations.rhs[7].getBlue(), equations.rhs[8].getBlue()
					}
				};
				for (std::size_t channel = 0; channel < 3; channel++)
				{
					std::array<double, SH_COEFFICIENT_COUNT> rhs{};
					std::array<double, SH_COEFFICIENT_COUNT> solution{};
					for (std::size_t i = 0; i < SH_COEFFICIENT_COUNT; i++)
					{
						rhs[i] = channelValues[channel][i];
					}
					if (!solveChannel(system, rhs, solution))
					{
						solved = false;
						break;
					}
					for (std::size_t i = 0; i < SH_COEFFICIENT_COUNT; i++)
					{
						if (channel == 0)
						{
							coefficients[i].setRed(solution[i]);
						}
						else if (channel == 1)
						{
							coefficients[i].setGreen(solution[i]);
						}
						else
						{
							coefficients[i].setBlue(solution[i]);
						}
					}
				}
				if (solved)
				{
					model.coefficients = coefficients;
					model.active = true;
				}
			}

			// Slots below minSamples (or a failed solve) publish an explicit
			// zero model.
			nextModels.emplace(key, model);
		}

		this->_models.swap(nextModels);
		this->clearEpoch();
		this->_epoch++;
	}

	void	ControlVariateSession::reset(void)
	{
		this->_models.clear();
		this->_epochSlots.clear();
		this->_pathVertices.clear();
		this->_epoch = 0;
	}
}
