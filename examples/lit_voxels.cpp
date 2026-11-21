/*******************************************************************************
* The MIT License (MIT)
*
* Copyright (c) 2015 David Williams and Matthew Williams
*
* Permission is hereby granted, free of charge, to any person obtaining a copy
* of this software and associated documentation files (the "Software"), to deal
* in the Software without restriction, including without limitation the rights
* to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
* copies of the Software, and to permit persons to whom the Software is
* furnished to do so, subject to the following conditions:
*
* The above copyright notice and this permission notice shall be included in all
* copies or substantial portions of the Software.
*
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
* IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
* FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
* AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
* LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
* OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
* SOFTWARE.
*******************************************************************************/

// A self contained demonstration of LightVolume and extractLitCubicMesh.
//
// The scene is a 32x32x32 room with a stone floor and a roof. Two lamps of
// different colours sit on the floor. The example lights the scene, turns one
// lamp off (and back on), then opens a hole in the roof, re-extracting the lit
// mesh after each edit. The final mesh is written to .task-build/lit.ply as an
// ASCII PLY file with per-vertex normals and colours.
//
// Colour mapping: a vertex carries four light channels in [0, 1] (sky, red,
// green, blue). The PLY colour is
//     red   = clamp(light.red   + 0.10 * light.sky)
//     green = clamp(light.green + 0.10 * light.sky)
//     blue  = clamp(light.blue  + 0.10 * light.sky)
// scaled to 0..255. See README.rst for details.

#include "PolyVox/LightVolume.h"
#include "PolyVox/LitCubicSurfaceExtractor.h"
#include "PolyVox/RawVolume.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

using namespace PolyVox;

namespace
{
	const int32_t RegionSize = 32;

	// Material indices used by the scene.
	const uint8_t MaterialAir = 0;
	const uint8_t MaterialRedLamp = 1;
	const uint8_t MaterialBlueLamp = 2;
	const uint8_t MaterialStone = 3;

	std::array<LightMaterial, 256> makeMaterials()
	{
		std::array<LightMaterial, 256> materials = {};
		// Material 0 is forced to air by LightVolume regardless of this entry.
		materials[MaterialRedLamp] = LightMaterial(true, 15, 0, 0);
		materials[MaterialBlueLamp] = LightMaterial(true, 0, 6, 15);
		materials[MaterialStone] = LightMaterial(true, 0, 0, 0);
		return materials;
	}

	Region sceneRegion()
	{
		return Region(0, 0, 0, RegionSize - 1, RegionSize - 1, RegionSize - 1);
	}

	void buildScene(RawVolume<uint8_t>& volume)
	{
		for (int32_t z = 0; z < RegionSize; ++z)
		{
			for (int32_t y = 0; y < RegionSize; ++y)
			{
				for (int32_t x = 0; x < RegionSize; ++x)
				{
					uint8_t material = MaterialAir;
					if (y == 0)
					{
						material = MaterialStone; // Floor
					}
					if (y == 24)
					{
						material = MaterialStone; // Closed roof
					}
					volume.setVoxel(x, y, z, material);
				}
			}
		}

		// Two lamps of different colours, each a 2x2x2 block.
		for (int32_t dz = 0; dz < 2; ++dz)
		{
			for (int32_t dy = 0; dy < 2; ++dy)
			{
				for (int32_t dx = 0; dx < 2; ++dx)
				{
					volume.setVoxel(8 + dx, 1 + dy, 8 + dz, MaterialRedLamp);
					volume.setVoxel(22 + dx, 1 + dy, 22 + dz, MaterialBlueLamp);
				}
			}
		}
	}

	void faceColor(float sky, float red, float green, float blue, uint8_t out[3])
	{
		const float ambient = 0.10f * sky;
		out[0] = static_cast<uint8_t>(std::lround(std::min(1.0f, red + ambient) * 255.0f));
		out[1] = static_cast<uint8_t>(std::lround(std::min(1.0f, green + ambient) * 255.0f));
		out[2] = static_cast<uint8_t>(std::lround(std::min(1.0f, blue + ambient) * 255.0f));
	}

	void report(const char* stage, const Mesh<LitCubicVertex<uint8_t> >& mesh)
	{
		std::cout << stage << ": vertices=" << mesh.getNoOfVertices()
			<< " triangles=" << (mesh.getNoOfIndices() / 3) << std::endl;
	}

	bool writePly(const std::string& path, const Mesh<LitCubicVertex<uint8_t> >& mesh)
	{
		std::ofstream out(path.c_str(), std::ios::out | std::ios::trunc);
		if (!out)
		{
			std::cerr << "Unable to open " << path << " for writing." << std::endl;
			return false;
		}

		out << "ply\n";
		out << "format ascii 1.0\n";
		out << "comment PolyVox lit cubic mesh\n";
		out << "element vertex " << mesh.getNoOfVertices() << "\n";
		out << "property float x\n";
		out << "property float y\n";
		out << "property float z\n";
		out << "property float nx\n";
		out << "property float ny\n";
		out << "property float nz\n";
		out << "property uchar red\n";
		out << "property uchar green\n";
		out << "property uchar blue\n";
		out << "element face " << (mesh.getNoOfIndices() / 3) << "\n";
		out << "property list uchar int vertex_indices\n";
		out << "end_header\n";

		for (uint32_t i = 0; i < mesh.getNoOfVertices(); ++i)
		{
			const LitCubicVertex<uint8_t>& vertex = mesh.getVertex(i);
			uint8_t color[3];
			faceColor(vertex.light[0], vertex.light[1], vertex.light[2], vertex.light[3], color);
			out << vertex.position.getX() << " " << vertex.position.getY() << " " << vertex.position.getZ() << " "
				<< vertex.normal.getX() << " " << vertex.normal.getY() << " " << vertex.normal.getZ() << " "
				<< static_cast<int>(color[0]) << " " << static_cast<int>(color[1]) << " " << static_cast<int>(color[2]) << "\n";
		}

		for (size_t i = 0; i < mesh.getNoOfIndices(); i += 3)
		{
			out << "3 " << mesh.getIndex(i) << " " << mesh.getIndex(i + 1) << " " << mesh.getIndex(i + 2) << "\n";
		}

		return true;
	}
}

int main()
{
	Region region = sceneRegion();
	RawVolume<uint8_t> volume(region);
	buildScene(volume);

	LightVolume<RawVolume<uint8_t> > light(&volume, region, makeMaterials());

	// The constructor scans the full volume and produces the initial lit mesh.
	Mesh<LitCubicVertex<uint8_t> > mesh = extractLitCubicMesh(&volume, light, region);
	report("initial (two lamps, roof closed)", mesh);
	if (mesh.isEmpty())
	{
		std::cerr << "Initial lit mesh is empty." << std::endl;
		return 1;
	}

	// Turn the blue lamp off.
	std::vector<LightEdit> lampOff;
	for (int32_t dz = 0; dz < 2; ++dz)
	{
		for (int32_t dy = 0; dy < 2; ++dy)
		{
			for (int32_t dx = 0; dx < 2; ++dx)
			{
				lampOff.push_back(LightEdit(22 + dx, 1 + dy, 22 + dz, MaterialAir));
			}
		}
	}
	light.apply(lampOff);
	mesh = extractLitCubicMesh(&volume, light, region);
	report("blue lamp off", mesh);

	// Turn the blue lamp back on.
	std::vector<LightEdit> lampOn;
	for (int32_t dz = 0; dz < 2; ++dz)
	{
		for (int32_t dy = 0; dy < 2; ++dy)
		{
			for (int32_t dx = 0; dx < 2; ++dx)
			{
				lampOn.push_back(LightEdit(22 + dx, 1 + dy, 22 + dz, MaterialBlueLamp));
			}
		}
	}
	light.apply(lampOn);
	mesh = extractLitCubicMesh(&volume, light, region);
	report("blue lamp back on", mesh);

	// Open a 4x4 hole in the roof, letting skylight into the room.
	std::vector<LightEdit> hole;
	for (int32_t z = 14; z < 18; ++z)
	{
		for (int32_t x = 14; x < 18; ++x)
		{
			hole.push_back(LightEdit(x, 24, z, MaterialAir));
		}
	}
	const std::vector<Vector3DInt32> changed = light.apply(hole);
	std::cout << "roof hole: changed light at " << changed.size() << " voxels, material reads="
		<< light.materialReads() << ", queue pops=" << light.queuePops() << std::endl;

	mesh = extractLitCubicMesh(&volume, light, region);
	report("roof hole open (final)", mesh);
	if (mesh.isEmpty())
	{
		std::cerr << "Final lit mesh is empty." << std::endl;
		return 1;
	}

	const std::string path = ".task-build/lit.ply";
	if (!writePly(path, mesh))
	{
		return 1;
	}

	std::cout << "Wrote " << path << " (" << mesh.getNoOfVertices() << " vertices, "
		<< (mesh.getNoOfIndices() / 3) << " triangles)." << std::endl;
	return 0;
}
