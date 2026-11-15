/***************************************************************************************************
* Cubiquity - A micro-voxel engine for games and other interactive applications                    *
*                                                                                                  *
* Written in 2026 by David Williams                                                                *
*                                                                                                  *
* To the extent possible under law, the author(s) have dedicated all copyright and related and     *
* neighboring rights to this software to the public domain worldwide. This software is distributed *
* without any warranty.                                                                            *
*                                                                                                  *
* You should have received a copy of the CC0 Public Domain Dedication along with this software.    *
* If not, see http://creativecommons.org/publicdomain/zero/1.0/.                                   *
***************************************************************************************************/
#ifndef CUBIQUITY_BEAM_VOLUME_H
#define CUBIQUITY_BEAM_VOLUME_H

#include "base.h"
#include "geometry.h"
#include "storage.h"

#include <array>
#include <cstddef>
#include <memory>
#include <stdexcept>

namespace Cubiquity
{
	//! Optical description of a single material.
	/*!
		The medium is constant inside each unit voxel. Extinction is the absorption coefficient
		(per unit length) and emission is the emitted radiance density (also per unit length).
		There is no scattering, reflection or interpolation, and no windowing is applied.
	*/
	struct OpticalMaterial
	{
		double extinction = 0.0;
		vec3d  emission = vec3d(0.0, 0.0, 0.0);
	};

	//! Result of a beam query, expressed as an average over the transverse footprint.
	/*!
		'radiance' is the zero-incident average premultiplied emission and 'transmittance'
		is the average transmittance. For a background B the displayed pixel is
		radiance + transmittance * B.
	*/
	struct BeamResult
	{
		vec3d  radiance     = vec3d(0.0, 0.0, 0.0);
		double transmittance = 1.0;
	};

	//! Optional counters describing the work performed by a single query.
	struct BeamStats
	{
		std::size_t sourceVisits   = 0;
		std::size_t transferVisits = 0;
	};

	//! Statistics describing the persistent optical representation.
	struct BeamPrepareStats
	{
		std::size_t sourceStates  = 0;
		std::size_t transferNodes = 0;
	};

	using PrepareStats = BeamPrepareStats;

	//! Orthographic emission/absorption preview built on top of a Volume.
	/*!
		A BeamVolume does not modify or extend the source Volume. It builds a persistent,
		hierarchical column-transfer representation directly on the volume's NodeStore, and
		supports integer rectangular footprints and depth windows. The source Volume must
		outlive the BeamVolume and must not be modified while the BeamVolume exists.

		Material zero is required to be perfectly transparent (zero extinction and zero
		emission). All other materials must be finite and each coefficient must lie in
		[0, 1e6].
	*/
	class BeamVolume
	{
	public:
		// This is public only so that the implementation can name it; it is otherwise opaque.
		struct Impl;

		BeamVolume(const Volume& volume,
			const std::array<OpticalMaterial, Internals::MaterialCount>& palette);

		// Copyable and movable (the prepared representation is immutable and may be shared).
		BeamVolume(const BeamVolume&) = default;
		BeamVolume& operator=(const BeamVolume&) = default;
		BeamVolume(BeamVolume&&) noexcept = default;
		BeamVolume& operator=(BeamVolume&&) noexcept = default;
		~BeamVolume() = default;

		//! Validate a whole palette and publish its optical representation.
		/*!
			The new representation is published only if every material is valid. On failure
			the previous representation (and the previous statistics) are retained. Setting
			the palette which is already active does not rebuild the optical graph.

			\return true on success, false if the palette was rejected.
		*/
		bool setPalette(const std::array<OpticalMaterial, Internals::MaterialCount>& palette);

		//! Query an integer rectangular footprint through a depth window.
		/*!
			\param axis 0, 1 or 2. The two transverse axes are the remaining coordinate
			axes in increasing order.
			\param reverse false walks from low depth to high depth, true walks the other way.
			\param lo,hi Transverse voxel indices as a half-open rectangle. The two entries
			correspond to the two transverse axes in increasing order.
			\param depthLo,depthHi Half-open depth window in voxel indices.
			\param stats Optional counters, reset to zero before the query.

			All endpoints must lie in [-2^31, 2^31] and lo<=hi, depthLo<=depthHi. Invalid
			parameters throw std::invalid_argument. An empty footprint or depth window
			returns zero radiance and unit transmittance.
		*/
		BeamResult query(int axis, bool reverse,
			const std::array<i64, 2>& lo, const std::array<i64, 2>& hi,
			i64 depthLo, i64 depthHi, BeamStats* stats = nullptr) const;

		//! Statistics from the most recent successful preparation.
		BeamPrepareStats prepareStats() const;

		//! The currently active palette.
		const std::array<OpticalMaterial, Internals::MaterialCount>& palette() const { return mPalette; }

	private:
		const Volume* mVolume = nullptr;
		std::array<OpticalMaterial, Internals::MaterialCount> mPalette{};
		std::shared_ptr<Impl> mRep;
	};
}

#endif // CUBIQUITY_BEAM_VOLUME_H
