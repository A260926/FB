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
// Renders an orthographic emission/absorption preview of a two-layer scene whose layers are
// laterally offset from one another. The two layers have equal mean density, so a random
// super-sampling renderer would produce a similar image for both the aligned and staggered
// variants; the beam renderer instead respects column correlation.
//
// Usage: beam-example [output.ppm]
#include "beam_render.h"
#include "beam_volume.h"
#include "storage.h"

#include <cmath>
#include <cstdio>
#include <string>

using namespace Cubiquity;

namespace
{
	constexpr u32 Red = 1;
	constexpr u32 Blue = 2;

	std::array<OpticalMaterial, Internals::MaterialCount> makePalette()
	{
		std::array<OpticalMaterial, Internals::MaterialCount> palette{};
		for (auto& material : palette)
		{
			material.extinction = 0.0;
			material.emission = vec3d(0.0, 0.0, 0.0);
		}

		// Material zero is already fully transparent.
		palette[Red].extinction = std::log(2.0) * 2.0;
		palette[Red].emission = vec3d(2.5, 0.15, 0.1);

		palette[Blue].extinction = std::log(2.0) * 2.0;
		palette[Blue].emission = vec3d(0.1, 0.2, 2.5);

		return palette;
	}

	// Two layers, each a grid of cells. The second layer is offset by half a cell in the
	// transverse plane so that its columns only partly line up with those of the first layer.
	void fillScene(Volume& volume, bool staggered)
	{
		const int cells = 8;
		const int cellSize = 4;

		for (int dz = 0; dz < 2; ++dz)
		{
			const i32 z = dz * cellSize;

			for (int cy = 0; cy < cells; ++cy)
			{
				for (int cx = 0; cx < cells; ++cx)
				{
					const bool check = ((cx + cy + dz) & 1) != 0;
					if (!check) continue;

					const i64 offset = staggered ? (dz == 0 ? 0 : cellSize / 2) : 0;
					const i32 x0 = static_cast<i32>(cx * cellSize + offset);
					const i32 y0 = static_cast<i32>(cy * cellSize + offset);

					for (i32 y = y0; y < y0 + cellSize / 2; ++y)
					{
						for (i32 x = x0; x < x0 + cellSize / 2; ++x)
						{
							volume.setVoxel(x, y, z, dz == 0 ? Red : Blue);
						}
					}
				}
			}
		}
	}
}

int main(int argc, char** argv)
{
	const std::string output = (argc > 1) ? argv[1] : ".task-build/beam.ppm";

	const auto palette = makePalette();

	// Render the staggered scene, which is the one which demonstrates column correlation.
	Volume volume;
	fillScene(volume, true);

	BeamVolume beam(volume, palette);

	BeamView view;
	view.axis = 2;
	view.reverse = false;
	view.origin = { {0, 0} };
	view.pixel = { {1, 1} };
	view.width = 48;
	view.height = 48;
	view.depthLo = 0;
	view.depthHi = 2 * 4;
	view.background = vec3d(0.02, 0.03, 0.05);

	BeamStats stats;
	const BeamImage image = renderBeamImage(beam, view, &stats);

	if (!writePPM(output, image))
	{
		std::fprintf(stderr, "Failed to write %s\n", output.c_str());
		return 1;
	}

	const auto prepare = beam.prepareStats();
	std::printf("BEAM_OK %dx%d %s prepare(source=%zu transfer=%zu) query(source=%zu transfer=%zu)\n",
		image.width, image.height, output.c_str(),
		prepare.sourceStates, prepare.transferNodes,
		stats.sourceVisits, stats.transferVisits);
	return 0;
}
