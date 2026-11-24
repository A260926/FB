#pragma once

#include "Color.hpp"
#include "Ray/Ray.hpp"
#include "Vector3.hpp"
#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <vector>

class	Scene;
struct	HitRecord;
class	Hittable;

// Diffuse-preview radiance control variate.
//
// A ControlVariateSession is owned by the caller and bound to a read-only
// Scene.  Every non-emissive diffuse hit keeps its full Monte Carlo suffix
// estimate Li, while a low-order spherical-harmonic model g(w) of the
// incident radiance is integrated analytically:
//
//     estimate = rho * (A(n) + Li - g(w))
//
// where w is the cosine-weighted continuation direction, rho is the surface
// albedo, and A(n) = integral over the hemisphere of g(w) (n.w) / pi dw.
//
// Depth means the number of diffuse reflections still available: at depth 0
// an emissive surface or the sky is still visible, but nothing reflects.
// Only cosine continuation is used (no NEE, no russian roulette, no luminance
// clipping).  Emissive geometry terminates through the material's emitted().
namespace ControlVariate
{
	constexpr std::size_t	SH_COEFFICIENT_COUNT = 9;
	constexpr int			MAX_SUPPORTED_DEPTH = 12;
	constexpr std::size_t	MIN_REQUIRED_SAMPLES = 9;

	// Real orthonormal spherical harmonics, l <= 2, evaluated on a world-space
	// unit direction w = (x, y, z).  The convention is the standard real SH
	// ordering Y_00, Y_1-1, Y_10, Y_11, Y_2-2, Y_2-1, Y_20, Y_21, Y_22 with
	// Condon-Shortley phase:
	//
	//   Y_00  = 1 / (2 sqrt(pi))
	//   Y_1-1 = sqrt(3/pi) y / 2          Y_10 = sqrt(3/pi) z / 2
	//   Y_11  = sqrt(3/pi) x / 2
	//   Y_2-2 = sqrt(15/pi) xy / 2        Y_2-1 = sqrt(15/pi) yz / 2
	//   Y_20  = sqrt(5/pi) (3z^2 - 1) / 4
	//   Y_21  = sqrt(15/pi) xz / 2        Y_22 = sqrt(15/pi) (x^2 - y^2) / 4
	std::array<double, SH_COEFFICIENT_COUNT>	evaluateBasis(const Vector3& direction);

	// Analytic hemisphere cosine integrals of the nine basis functions:
	// H_i(n) = integral over the hemisphere around n of Y_i(w) (n.w) / pi dw.
	std::array<double, SH_COEFFICIENT_COUNT>	hemisphereCosineIntegrals(const Vector3& normal);

	struct	Configuration
	{
		int			maxDepth = 2;
		std::uint64_t	seed = 0;
		double		ridge = 1.0e-4;
		std::size_t	minSamples = MIN_REQUIRED_SAMPLES;
		bool		enabled = true;
	};

	// Identity of a model slot: the actual object hit, which geometric side of
	// it was hit, and the remaining diffuse depth at the hit.  Objects sharing a
	// material never share a slot.
	struct	ModelKey
	{
		const void*	object = nullptr;
		std::size_t	objectIndex = 0;
		bool		frontFace = true;
		int			depth = 0;

		bool	operator<(const ModelKey& other) const;
		bool	operator==(const ModelKey& other) const;
	};

	// One RGB coefficient per basis function of a frozen model.
	using ModelCoefficients = std::array<Color, SH_COEFFICIENT_COUNT>;

	// Ridge-regression normal equations accumulated during an epoch, averaged
	// over the samples of the slot: matrix = mean(Y Y^T), rhs = mean(Y Li),
	// each rhs entry carrying the full RGB label.  The published model solves
	// (matrix + ridge * I) c = rhs independently per channel.
	struct	NormalEquations
	{
		std::array<std::array<double, SH_COEFFICIENT_COUNT>, SH_COEFFICIENT_COUNT>	matrix{};
		std::array<Color, SH_COEFFICIENT_COUNT>	rhs{};
		std::size_t	sampleCount = 0;
	};

	struct	FrozenModel
	{
		ModelKey			key;
		ModelCoefficients	coefficients{};
		bool				active = false; // false: zero model (never enough samples)
		NormalEquations		equations;      // averaged equations that produced it
	};

	// Read-only record of one vertex of a production path, so the recursion can
	// be re-derived independently.  Li is the complete suffix estimate before
	// the local albedo, g the frozen model evaluated at the continuation
	// direction, A the analytic hemisphere integral, rho the local albedo, and
	// result the value returned for this vertex.
	struct	PathVertex
	{
		ModelKey	key;
		Vector3		position;
		Vector3		normal;
		Vector3		direction;
		Color		li;
		Color		g;
		Color		a;
		Color		rho;
		Color		result;
		double		cosinePdf = 0.0;
		bool		emissive = false;
		bool		background = false;
	};

	class	ControlVariateSession
	{
		public:
			ControlVariateSession(const Scene& scene, const Configuration& configuration);
			~ControlVariateSession(void);

			// Runs one path starting from 'ray' (typically a camera ray) and
			// returns linear RGB.  sampleId must be unique within an epoch; the
			// random stream is derived from the configured seed, the current
			// epoch, sampleId and the path depth.
			Color	sample(const Ray& ray, std::uint64_t sampleId);

			// Freezes the epoch's normal equations into the next model table,
			// clears the epoch statistics and advances the epoch counter.  An
			// epoch without samples publishes zero models.
			void	finishEpoch(void);

			// Clears every model and every epoch statistic.
			void	reset(void);

			const Configuration&	getConfiguration(void) const;
			std::size_t			getEpoch(void) const;
			bool				isEnabled(void) const;

			// Frozen model table, valid for the current epoch (empty before the
			// first finishEpoch).  Inactive entries are explicit zero models.
			const std::map<ModelKey, FrozenModel>&	getModels(void) const;

			// Current-epoch diagnostics: per-slot sample counts and the running
			// averaged normal equations.  Reading these never consumes random
			// numbers and never mutates the session.
			std::size_t			getEpochSampleCount(const ModelKey& key) const;
			const NormalEquations*	getEpochEquations(const ModelKey& key) const;
			std::vector<ModelKey>	getEpochKeys(void) const;

			// Vertices of the most recent production path, root first.
			const std::vector<PathVertex>&	getLastPathVertices(void) const;

			// Deterministic cosine-weighted hemisphere direction around
			// 'normal' for (epoch, sampleId, depth).  Shared by the enabled and
			// disabled paths so toggling the control variate never changes the
			// sampled directions.
			Vector3	cosineDirection(
				const Vector3& normal,
				std::uint64_t sampleId,
				int depth
			) const;

		private:
			struct	EpochSlot;

			const Scene&	_scene;
			Configuration	_configuration;
			std::uint64_t	_seed;
			std::size_t		_epoch;
			bool			_enabled;
			std::map<ModelKey, FrozenModel>	_models;
			std::map<ModelKey, EpochSlot>	_epochSlots;
			mutable std::vector<PathVertex>	_pathVertices;
			std::vector<std::shared_ptr<class Hittable>>	_supportedObjects;

			Color	estimate(
				const Ray& ray,
				std::uint64_t sampleId,
				int depth,
				bool recordVertices
			);
			Color	backgroundRadiance(const Ray& ray) const;
			const FrozenModel*	findModel(const ModelKey& key) const;
			EpochSlot&	epochSlot(const ModelKey& key);
			void	clearEpoch(void);
			void	validateScene(void);
	};
}
