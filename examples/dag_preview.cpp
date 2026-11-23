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
//
// Headless CPU preview of a sparse volume, using the dag_rasterizer module.
//
// This example deliberately uses no window, SDL, OpenGL or GPU: it builds a small volume,
// renders it with renderDag(), and writes the result to .task-build/dag.ppm. The scene is a
// material-4 wall full of square holes, with a two-material (2/3) striped structure behind
// it, so the rendered image shows both the holes and the differently coloured background.
//
// Build (from the repository root), or just run `bash task_support/run.sh target`:
//
//   c++ -std=c++20 -O2 -pthread -DCUBIQUITY_USE_POOLSTL -Isrc/library -I. \
//       examples/dag_preview.cpp src/library/*.cpp -o .task-build/example
//   .task-build/example
//
#include "dag_rasterizer.h"
#include "storage.h"

#include <cmath>
#include <cstdio>
#include <limits>
#include <string>
#include <vector>

using namespace Cubiquity;

namespace
{
	// Material ids used by the preview scene.
	constexpr MaterialId WallMaterial = 4;
	constexpr MaterialId StripeMaterialA = 2;
	constexpr MaterialId StripeMaterialB = 3;

	// Builds the preview volume: an occluding wall with square holes at z ~ 0, and a tall
	// striped backdrop at z ~ 24..32. Voxel centres are at integer positions, so a voxel at
	// integer q covers the solid interval [q - 0.5, q + 0.5].
	void buildScene(Volume& volume)
	{
		// The wall occupies two voxels of thickness and is 72 x 72 voxels across. Square
		// holes are cut on a 12-voxel pitch (6 solid, 6 empty) in both x and y.
		for (int z = -2; z <= 1; ++z)
		{
			for (int y = -4; y < 68; ++y)
			{
				for (int x = -4; x < 68; ++x)
				{
					const bool hole = (((x + 4) % 12) < 6) && (((y + 4) % 12) < 6);
					if (!hole)
					{
						volume.setVoxel(x, y, z, WallMaterial);
					}
				}
			}
		}

		// The backdrop is a set of 5-voxel vertical stripes alternating two materials. It is
		// large enough that every hole shows background rather than a miss.
		for (int z = 24; z < 32; ++z)
		{
			for (int y = -8; y < 80; ++y)
			{
				for (int x = -8; x < 80; ++x)
				{
					const MaterialId material = ((((x + 8) / 5) % 2) != 0) ? StripeMaterialA : StripeMaterialB;
					volume.setVoxel(x, y, z, material);
				}
			}
		}

		volume.bake();
	}

	// A simple colour ramp so the PPM is readable: each material gets a fixed RGB colour and
	// depth only modulates brightness (nearer is brighter).
	struct Rgb { unsigned char r, g, b; };

	Rgb materialColour(MaterialId material)
	{
		switch (material)
		{
		case WallMaterial:    return Rgb{ 210, 110,  80 }; // warm foreground
		case StripeMaterialA: return Rgb{  70, 150, 220 }; // blue stripe
		case StripeMaterialB: return Rgb{  70, 210, 150 }; // green stripe
		default:              return Rgb{   0,   0,   0 };
		}
	}

	void writePpm(const std::string& filename, const RasterResult& result, double nearDepth, double farDepth)
	{
		FILE* file = std::fopen(filename.c_str(), "wb");
		if (!file)
		{
			std::fprintf(stderr, "dag_preview: could not open '%s' for writing\n", filename.c_str());
			return;
		}

		std::fprintf(file, "P6\n%d %d\n255\n", result.width, result.height);
		for (int y = 0; y < result.height; ++y)
		{
			for (int x = 0; x < result.width; ++x)
			{
				Rgb colour = materialColour(result.materialAt(x, y));
				const double depth = result.depthAt(x, y);
				if (std::isinf(depth))
				{
					colour = Rgb{ 12, 12, 16 }; // miss: near-black background
				}
				else if (farDepth > nearDepth)
				{
					// Shade 0.45..1.0 by depth (nearer = brighter), purely cosmetic.
					const double t = (depth - nearDepth) / (farDepth - nearDepth);
					const double shade = 1.0 - 0.55 * std::min(std::max(t, 0.0), 1.0);
					colour.r = static_cast<unsigned char>(colour.r * shade);
					colour.g = static_cast<unsigned char>(colour.g * shade);
					colour.b = static_cast<unsigned char>(colour.b * shade);
				}
				const unsigned char pixel[3] = { colour.r, colour.g, colour.b };
				std::fwrite(pixel, 1, 3, file);
			}
		}
		std::fclose(file);
	}
}

int main()
{
	Volume volume;
	buildScene(volume);

	RasterCamera camera;
	camera.eye = vec3d(31.5, 31.5, -64.0);
	camera.right = vec3d(1.0, 0.0, 0.0);
	camera.up = vec3d(0.0, 1.0, 0.0);
	camera.forward = vec3d(0.0, 0.0, 1.0);
	camera.width = 96;
	camera.height = 72;
	camera.fovY = 0.7;
	camera.nearPlane = 1.0;
	camera.farPlane = 200.0;

	const RasterResult withOcclusion = renderDag(volume, camera, true);
	const RasterResult withoutOcclusion = renderDag(volume, camera, false);

	// Both paths must agree pixel for pixel; the occlusion path only saves work.
	int mismatches = 0;
	double nearest = std::numeric_limits<double>::infinity();
	double farthest = -std::numeric_limits<double>::infinity();
	for (int y = 0; y < camera.height; ++y)
	{
		for (int x = 0; x < camera.width; ++x)
		{
			if (withOcclusion.depthAt(x, y) != withoutOcclusion.depthAt(x, y) ||
				withOcclusion.materialAt(x, y) != withoutOcclusion.materialAt(x, y))
			{
				++mismatches;
			}
			const double depth = withOcclusion.depthAt(x, y);
			if (!std::isinf(depth))
			{
				nearest = std::min(nearest, depth);
				farthest = std::max(farthest, depth);
			}
		}
	}

	const std::string filename = ".task-build/dag.ppm";
	writePpm(filename, withOcclusion, nearest, farthest);

	std::printf("dag_preview: wrote %s (%dx%d)\n", filename.c_str(), camera.width, camera.height);
	std::printf("  pixels: %d, mismatches between occlusion modes: %d\n", camera.width * camera.height, mismatches);
	std::printf("  depth range: %.3f .. %.3f\n", nearest, farthest);
	std::printf("  occlusion on : nodeVisits=%llu leafBoxes=%llu hizReads=%llu hizUpdates=%llu hizRejected=%llu\n",
		static_cast<unsigned long long>(withOcclusion.stats.nodeVisits),
		static_cast<unsigned long long>(withOcclusion.stats.leafBoxes),
		static_cast<unsigned long long>(withOcclusion.stats.hizReads),
		static_cast<unsigned long long>(withOcclusion.stats.hizUpdates),
		static_cast<unsigned long long>(withOcclusion.stats.hizRejected));
	std::printf("  occlusion off: nodeVisits=%llu leafBoxes=%llu\n",
		static_cast<unsigned long long>(withoutOcclusion.stats.nodeVisits),
		static_cast<unsigned long long>(withoutOcclusion.stats.leafBoxes));

	return mismatches == 0 ? 0 : 1;
}
