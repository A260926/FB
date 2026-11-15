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
#include "beam_render.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <limits>
#include <stdexcept>

namespace Cubiquity
{
	BeamImage renderBeamImage(const BeamVolume& beam, const BeamView& view, BeamStats* stats)
	{
		if (stats) { *stats = BeamStats{}; }

		if (view.axis < 0 || view.axis > 2)
		{
			throw std::invalid_argument("renderBeamImage: axis must be 0, 1 or 2");
		}
		if (view.width < 0 || view.height < 0)
		{
			throw std::invalid_argument("renderBeamImage: negative image dimensions");
		}
		if (view.pixel[0] <= 0 || view.pixel[1] <= 0)
		{
			throw std::invalid_argument("renderBeamImage: pixel footprint must be positive");
		}
		if (view.depthLo > view.depthHi)
		{
			throw std::invalid_argument("renderBeamImage: invalid depth window");
		}

		const i64 limit = i64(1) << 31;

		// Bound the coordinate arithmetic so that origin + index * pixel cannot overflow i64.
		// Queries never accept endpoints outside [-2^31, 2^31] anyway.
		if (view.origin[0] < -limit || view.origin[0] > limit ||
			view.origin[1] < -limit || view.origin[1] > limit)
		{
			throw std::invalid_argument("renderBeamImage: origin outside [-2^31, 2^31]");
		}
		if (view.pixel[0] > limit || view.pixel[1] > limit)
		{
			throw std::invalid_argument("renderBeamImage: pixel footprint is too large");
		}

		BeamImage image;
		image.width = view.width;
		image.height = view.height;
		const std::size_t pixelCount = static_cast<std::size_t>(view.width) * static_cast<std::size_t>(view.height);
		if (pixelCount > image.pixels.max_size())
		{
			throw std::invalid_argument("renderBeamImage: image is too large");
		}
		image.pixels.assign(pixelCount, vec3d(0.0, 0.0, 0.0));

		for (int py = 0; py < view.height; ++py)
		{
			// Guard the coordinate arithmetic before handing it to the query.
			const i64 base1 = view.origin[1] + static_cast<i64>(py) * view.pixel[1];
			const i64 top1 = base1 + view.pixel[1];
			if (base1 < -limit || top1 > limit)
			{
				throw std::invalid_argument("renderBeamImage: pixel footprint outside [-2^31, 2^31]");
			}

			for (int px = 0; px < view.width; ++px)
			{
				const i64 base0 = view.origin[0] + static_cast<i64>(px) * view.pixel[0];
				const i64 top0 = base0 + view.pixel[0];
				if (base0 < -limit || top0 > limit)
				{
					throw std::invalid_argument("renderBeamImage: pixel footprint outside [-2^31, 2^31]");
				}

				const std::array<i64, 2> lo{ {base0, base1} };
				const std::array<i64, 2> hi{ {top0, top1} };

				// query() resets the counters it is given, so use a per-pixel value and
				// accumulate here if the caller wants totals for the whole image.
				BeamStats pixelStats;
				const BeamResult result = beam.query(view.axis, view.reverse, lo, hi,
					view.depthLo, view.depthHi, &pixelStats);

				if (stats)
				{
					stats->sourceVisits += pixelStats.sourceVisits;
					stats->transferVisits += pixelStats.transferVisits;
				}

				const vec3d color = result.radiance + result.transmittance * view.background;
				image.pixels[static_cast<std::size_t>(py) * view.width + px] = color;
			}
		}

		return image;
	}

	std::uint8_t encodeLinearChannel(double value)
	{
		if (!(value > 0.0)) { return 0; } // Also maps NaN to zero.
		const double clamped = std::min(value, 1.0);
		return static_cast<std::uint8_t>(std::lround(clamped * 255.0));
	}

	std::array<std::uint8_t, 3> encodeLinearRGB(const vec3d& color)
	{
		return { encodeLinearChannel(color.x), encodeLinearChannel(color.y), encodeLinearChannel(color.z) };
	}

	bool writePPM(const std::string& filename, const BeamImage& image)
	{
		if (image.width < 0 || image.height < 0) return false;
		if (image.pixels.size() != static_cast<std::size_t>(image.width) * static_cast<std::size_t>(image.height))
		{
			return false;
		}

		std::ofstream file(filename, std::ios::out | std::ios::binary);
		if (!file.is_open()) return false;

		file << "P6\n" << image.width << " " << image.height << "\n255\n";
		for (const vec3d& pixel : image.pixels)
		{
			const auto rgb = encodeLinearRGB(pixel);
			file.write(reinterpret_cast<const char*>(rgb.data()), 3);
		}

		file.flush();
		return file.good();
	}
}
