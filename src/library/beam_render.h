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
#ifndef CUBIQUITY_BEAM_RENDER_H
#define CUBIQUITY_BEAM_RENDER_H

#include "base.h"
#include "beam_volume.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace Cubiquity
{
	//! Description of an orthographic integer-pixel view onto a BeamVolume.
	/*!
		Each image pixel corresponds to exactly one integer rectangular footprint in the transverse
		plane. Footprints are given in voxel units. Pixel (px, py) covers the transverse half-open
		rectangle [origin[0] + px * pixel[0], + pixel[0]) x [origin[1] + py * pixel[1], + pixel[1]),
		where the two entries are ordered by the remaining coordinate axes (see BeamVolume::query).

		The image is written as linear RGB. A background colour may optionally be supplied.
	*/
	struct BeamView
	{
		int axis = 2;                      //!< Depth axis: 0, 1 or 2.
		bool reverse = false;              //!< Walk from low depth to high depth (false) or backwards.
		std::array<i64, 2> origin{ {0, 0} }; //!< Transverse coordinate of the lower corner of pixel (0, 0).
		std::array<i64, 2> pixel{ {1, 1} };  //!< Integer footprint extent of each pixel, in voxels.
		int width = 0;                     //!< Image width in pixels.
		int height = 0;                    //!< Image height in pixels.
		i64 depthLo = -(i64(1) << 31);     //!< Low end of the depth window (inclusive).
		i64 depthHi = (i64(1) << 31);      //!< High end of the depth window (exclusive).
		vec3d background = vec3d(0.0, 0.0, 0.0); //!< Optional background radiance B.
	};

	//! A linear RGB image. Rows are stored consecutively, starting with the first row.
	struct BeamImage
	{
		int width = 0;
		int height = 0;
		std::vector<vec3d> pixels;
	};

	//! Render an image by issuing one integer-footprint beam query per pixel.
	/*!
		If 'stats' is non-null it is reset to zero before the render and accumulates the visits
		performed by every query. Invalid views throw std::invalid_argument.
	*/
	BeamImage renderBeamImage(const BeamVolume& beam, const BeamView& view, BeamStats* stats = nullptr);

	//! Convert a linear channel to an 8-bit sRGB-free byte using clamping to [0, 1].
	std::uint8_t encodeLinearChannel(double value);

	//! Encode a linear RGB pixel to 8-bit bytes with clamping.
	std::array<std::uint8_t, 3> encodeLinearRGB(const vec3d& color);

	//! Write a binary PPM (P6) file using clamped linear RGB encoding. Returns true on success.
	bool writePPM(const std::string& filename, const BeamImage& image);
}

#endif // CUBIQUITY_BEAM_RENDER_H
