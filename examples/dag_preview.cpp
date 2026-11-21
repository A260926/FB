/***************************************************************************************************
* Cubiquity - A micro-voxel engine for games and other interactive applications                    *
*                                                                                                  *
* Written in 2026 by David Williams                                                                *
*                                                                                                  *
* To the extent possible under law, the author(s) have dedicated all copyright and related and     *
* neighboring rights to this software under the public domain worldwide. This software is          *
* distributed without any warranty.                                                                *
*                                                                                                  *
* You should have received a copy of the CC0 Public Domain Dedication along with this software.    *
* If not, see http://creativecommons.org/publicdomain/zero/1.0/.                                   *
***************************************************************************************************/
// Headless preview of a compressed voxel DAG: renders a scene containing an
// occluder wall with holes and a differently-coloured striped structure behind
// it, using the CPU DAG rasteriser (no window, GPU, SDL or OpenGL), and writes
// the result as a binary PPM image to .task-build/dag.ppm (run from the
// repository root, e.g. via 'bash task_support/run.sh target').
//
// See examples/README.md for a description of the camera, depth and statistics.

#include "dag_rasterizer.h"
#include "storage.h"

#include <cstdio>

using namespace Cubiquity;

namespace
{
	// Fixed palette mapping materials to RGB colours.
	void materialColor(MaterialId material, unsigned char& r, unsigned char& g, unsigned char& b)
	{
		switch (material)
		{
		case 0:  r = 16;  g = 16;  b = 24;  break; // Empty space (dark sky).
		case 2:  r = 204; g = 68;  b = 68;  break; // Occluder wall (red).
		case 5:  r = 68;  g = 204; b = 221; break; // Rear structure, stripe A (cyan).
		case 6:  r = 240; g = 220; b = 90;  break; // Rear structure, stripe B (yellow).
		default: r = 255; g = 0;   b = 255; break; // Unused materials (magenta).
		}
	}

	void printStats(const char* label, const DagRasterResult& result)
	{
		unsigned hits = 0;
		for (size_t i = 0; i < result.depth.size(); ++i) {
			if (result.material[i] != 0) { ++hits; }
		}
		std::printf("%s nodeVisits=%llu leafBoxes=%llu hizReads=%llu hizUpdates=%llu hizRejected=%llu hits=%u/%zu\n",
			label,
			static_cast<unsigned long long>(result.nodeVisits),
			static_cast<unsigned long long>(result.leafBoxes),
			static_cast<unsigned long long>(result.hizReads),
			static_cast<unsigned long long>(result.hizUpdates),
			static_cast<unsigned long long>(result.hizRejected),
			hits, result.depth.size());
	}
}

int main()
{
	// Scene: an occluder wall (material 2) with a grid of 6x6 holes punched
	// through it, and a rear structure of vertical stripes (materials 5 and 6)
	// which is only visible through the holes.
	Volume volume;
	for (int z = 24; z <= 27; ++z) {
		for (int y = 0; y < 48; ++y) {
			for (int x = 0; x < 64; ++x) {
				const bool hole = (x % 16 >= 5 && x % 16 < 11) && (y % 12 >= 3 && y % 12 < 9);
				if (!hole) { volume.setVoxel(x, y, z, 2); }
			}
		}
	}
	for (int z = 40; z <= 45; ++z) {
		for (int y = 0; y < 48; ++y) {
			for (int x = 0; x < 64; ++x) {
				volume.setVoxel(x, y, z, ((x / 8) % 2 == 0) ? 5 : 6);
			}
		}
	}
	volume.bake();

	// Camera: 64x48 pixels, looking along +z at the wall.
	RasterCamera camera;
	camera.eye     = vec3d(31.5, 23.5, -30.0);
	camera.right   = vec3d(1.0, 0.0, 0.0);
	camera.up      = vec3d(0.0, 1.0, 0.0);
	camera.forward = vec3d(0.0, 0.0, 1.0);
	camera.width   = 64;
	camera.height  = 48;
	camera.fovY    = 0.6;
	camera.near    = 1.0;
	camera.far     = 300.0;

	const DagRasterResult result = renderDag(volume, camera, /*occlusion=*/true);
	printStats("dag_preview occlusion=on : ", result);

	// For comparison, the same frame with occlusion rejection switched off.
	const DagRasterResult resultNoOcclusion = renderDag(volume, camera, /*occlusion=*/false);
	printStats("dag_preview occlusion=off: ", resultNoOcclusion);

	// Write the occluded render as a binary PPM.
	const char* filename = ".task-build/dag.ppm";
	FILE* file = std::fopen(filename, "wb");
	if (!file)
	{
		std::fprintf(stderr, "Failed to open '%s' for writing\n", filename);
		return 1;
	}
	std::fprintf(file, "P6\n%d %d\n255\n", camera.width, camera.height);
	for (int y = 0; y < camera.height; ++y)
	{
		for (int x = 0; x < camera.width; ++x)
		{
			unsigned char rgb[3];
			materialColor(result.materialAt(x, y), rgb[0], rgb[1], rgb[2]);
			std::fwrite(rgb, 1, 3, file);
		}
	}
	std::fclose(file);
	std::printf("dag_preview wrote %s (%dx%d)\n", filename, camera.width, camera.height);
	return 0;
}
