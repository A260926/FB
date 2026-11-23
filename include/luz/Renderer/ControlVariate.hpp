#pragma once

#include "Color.hpp"
#include "Ray/Ray.hpp"
#include "Vector3.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <vector>

class Scene;
class Hittable;

// Radiance control variate for the diffuse preview.
//
// A session is owned by the caller and bound to a read-only Scene. It traces
// cosine-weighted diffuse paths exactly like the production preview (the same
// _checkHits and the real materials, no NEE, no Russian roulette, no radiance
// clamping), but it learns a low-order spherical-harmonic model of the
// incoming radiance per (object, geometric side, remaining depth). The next
// epoch evaluates that model analytically over the sampled hemisphere and only
// lets Monte Carlo estimate the signed residual.
//
// Spherical-harmonic convention
// -----------------------------
// Real, orthonormal SH with respect to the usual solid-angle measure on the
// unit sphere. Coefficients are ordered as
//
//   0: Y_0^0
//   1: Y_1^-1  (y)
//   2: Y_1^0   (z)
//   3: Y_1^1   (x)
//   4: Y_2^-2  (xy)
//   5: Y_2^-1  (yz)
//   6: Y_2^0
//   7: Y_2^1   (xz)
//   8: Y_2^2   (x^2 - y^2)
//
// with the standard normalization
//
//   Y_0^0  = 0.2820947918
//   Y_1    = 0.4886025119 * (component)
//   Y_2^-2 = 1.0925484306 xy
//   Y_2^-1 = 1.0925484306 yz
//   Y_2^0  = 0.3153915653 (3z^2 - 1)
//   Y_2^1  = 1.0925484306 xz
//   Y_2^2  = 0.5462742153 (x^2 - y^2)
//
// Every direction is a world-space unit vector.
namespace ControlVariate
{
	constexpr int SH_COEFFICIENT_COUNT = 9;

	using ShVector = std::array<double, SH_COEFFICIENT_COUNT>;
	using ShCoefficients = std::array<Color, SH_COEFFICIENT_COUNT>;
	using Matrix = std::array<std::array<double, SH_COEFFICIENT_COUNT>, SH_COEFFICIENT_COUNT>;

	// Evaluates the nine real SH basis functions at 'direction' (normalized
	// internally). A zero direction yields an all-zero vector.
	ShVector	shBasis(const Vector3& direction);

	// Returns alpha_l * Y_l^m(n) for each basis index, that is the exact value
	// of the cosine-weighted hemisphere integral
	//
	//   integral over H(n) of Y_l^m(w) * (n.w) / pi dw
	//
	// with alpha_0 = 1, alpha_1 = 2/3, alpha_2 = 1/4.
	ShVector	shCosineIntegral(const Vector3& normal);

	// Evaluates sum_k coefficients[k] * basis[k] per RGB channel.
	Color	evaluateSh(const ShCoefficients& coefficients, const ShVector& basis);

	struct ModelKey
	{
		const Hittable*	object = nullptr;
		bool			backFace = false;
		int				remainingDepth = 0;

		bool	operator<(const ModelKey& other) const;
		bool	operator==(const ModelKey& other) const;
		bool	operator!=(const ModelKey& other) const;
	};

	ModelKey	makeModelKey(const Hittable* object, bool backFace, int remainingDepth);

	struct ModelEntry
	{
		ModelKey		key;
		ShCoefficients	coefficients{};
		std::uint64_t	trainingSamples = 0;
	};

	// Snapshot of one key's accumulated normal equations for the current epoch.
	// 'normalMatrix' is the average of phi phi^T, 'rhs' the average of phi * Li
	// and 'regularizedMatrix' = normalMatrix + ridge * I.
	struct NormalEquationEntry
	{
		ModelKey		key;
		std::uint64_t	sampleCount = 0;
		Matrix			normalMatrix{};
		Matrix			regularizedMatrix{};
		ShCoefficients	rhs{};
	};

	// Read-only record of one reflective vertex of the most recent production
	// path. 'li' is the complete estimate returned by the next real ray, which
	// is also the training label; 'g' is the frozen model at 'direction', 'a'
	// is its analytic cosine integral at 'normal', and 'estimate' is the value
	// returned by this vertex.
	struct PathVertex
	{
		ModelKey	key;
		Vector3		normal;
		Vector3		direction;
		Color		li;
		Color		g;
		Color		a;
		Color		rho;
		Color		estimate;
	};

	struct Config
	{
		int				maxDepth = 6;	// 0..12 remaining diffuse bounces
		std::uint32_t	seed = 0;
		double			ridge = 1e-4;	// finite, > 0
		int				minSamples = 9;	// >= 9
		bool			enabled = true;
	};

	class Session
	{
		public:
			// Throws std::invalid_argument for an invalid configuration, or for
			// a scene that uses unsupported geometry, materials or a sky mode
			// other than SKY_NONE / SKY_LINEAR.
			Session(const Scene& scene, const Config& config);
			~Session(void) = default;

			Session(const Session&) = delete;
			Session&	operator=(const Session&) = delete;

			// Traces one production path for the given sampleId. sampleId must
			// be unique within the current epoch; it seeds the direction stream
			// together with the configured seed, the epoch and the path depth.
			// Returns linear RGB, which may be negative.
			Color	sample(const Ray& ray, std::uint64_t sampleId);

			// Publishes the model fitted from this epoch's statistics, clears the
			// per-epoch statistics and advances the epoch counter. An epoch with
			// no recorded samples publishes the zero model.
			std::uint64_t	finishEpoch(void);

			// Clears every model, every trained sample and every diagnostic.
			void	reset(void);

			std::uint64_t	epoch(void) const { return this->_epoch; }
			std::size_t		epochSampleCount(void) const { return this->_epochSamples; }
			std::size_t		epochVertexCount(void) const;
			const Config&	config(void) const { return this->_config; }

			std::vector<ModelEntry>				modelEntries(void) const;
			const ModelEntry*					findModelEntry(const ModelKey& key) const;
			std::vector<NormalEquationEntry>	trainingEquations(void) const;
			std::vector<NormalEquationEntry>	lastFittedEquations(void) const;
			const std::vector<PathVertex>&		lastPath(void) const { return this->_lastPath; }

		private:
			struct Accumulator
			{
				std::uint64_t	count = 0;
				Matrix			sumPhiPhi{};
				ShCoefficients	sumPhiL{};
			};

			Color	trace(
				const Ray& ray,
				int remainingDepth,
				std::uint64_t sampleId,
				int pathDepth,
				std::vector<PathVertex>& vertices
			);
			Color	skyRadiance(const Ray& ray) const;
			Color	modelValue(const ModelKey& key, const ShVector& basis) const;
			Color	analyticMean(const ModelKey& key, const ShVector& integral) const;

			const Scene*					_scene = nullptr;
			Config							_config;
			std::uint64_t					_epoch = 0;
			std::size_t						_epochSamples = 0;
			std::map<ModelKey, ModelEntry>	_model;
			std::map<ModelKey, Accumulator>	_stats;
			std::vector<NormalEquationEntry>	_lastFit;
			std::vector<PathVertex>			_lastPath;
	};
}
