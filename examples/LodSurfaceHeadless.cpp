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
* IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
* FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
* COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
* IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
* CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
*******************************************************************************/

// Headless demonstration of PolyVox::LodSurface. It needs no Qt, display or
// GPU: a scalar field is built once, an initial layout is extracted, then a
// local level-of-detail switch and a batch of density edits are applied, each
// time writing the resulting mesh to a Wavefront OBJ file. Run it from the
// repository root (or wherever you want the two .obj files to appear).

#include "PolyVox/LodSurface.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using namespace PolyVox;

namespace
{
	std::vector<float> buildField(const Region& region)
	{
		// Two separated Gaussian blobs above a negative background, so the zero
		// level forms two closed shells. The non-integer centres keep the level
		// from passing exactly through a lattice point.
		std::vector<float> values;
		values.reserve(static_cast<size_t>(region.getWidthInVoxels())
			* region.getHeightInVoxels() * region.getDepthInVoxels());
		for (int32_t z = region.getLowerZ(); z <= region.getUpperZ(); ++z)
		for (int32_t y = region.getLowerY(); y <= region.getUpperY(); ++y)
		for (int32_t x = region.getLowerX(); x <= region.getUpperX(); ++x)
		{
			const float ax = static_cast<float>(x) - 9.3f;
			const float ay = static_cast<float>(y) - 10.7f;
			const float az = static_cast<float>(z) - 10.1f;
			const float bx = static_cast<float>(x) - 22.4f;
			const float by = static_cast<float>(y) - 21.6f;
			const float bz = static_cast<float>(z) - 23.2f;
			const float blobA = std::exp(-(ax * ax + ay * ay + az * az) / 28.0f);
			const float blobB = std::exp(-(bx * bx + by * by + bz * bz) / 20.0f);
			values.push_back(blobA + blobB - 0.5f);
		}
		return values;
	}

	void writeObj(const std::string& path, const Mesh<Vertex<float> >& mesh)
	{
		std::FILE* file = std::fopen(path.c_str(), "w");
		if (file == NULL) return;
		std::fprintf(file, "# PolyVox::LodSurface export\n");
		for (uint32_t i = 0; i < mesh.getNoOfVertices(); ++i)
		{
			const Vertex<float>& v = mesh.getVertex(i);
			std::fprintf(file, "v %f %f %f\n",
				v.position.getX(), v.position.getY(), v.position.getZ());
			std::fprintf(file, "vn %f %f %f\n",
				v.normal.getX(), v.normal.getY(), v.normal.getZ());
		}
		for (size_t i = 0; i + 2 < mesh.getNoOfIndices(); i += 3)
		{
			// OBJ indices are 1-based; position and normal share an index.
			const uint32_t a = mesh.getIndex(static_cast<uint32_t>(i)) + 1;
			const uint32_t b = mesh.getIndex(static_cast<uint32_t>(i + 1)) + 1;
			const uint32_t c = mesh.getIndex(static_cast<uint32_t>(i + 2)) + 1;
			std::fprintf(file, "f %u//%u %u//%u %u//%u\n", a, a, b, b, c, c);
		}
		std::fclose(file);
	}

	void report(const char* label, const LodSurface& surface)
	{
		const LodSurfaceStats stats = surface.lastStats();
		const Mesh<Vertex<float> > mesh = surface.mesh();
		std::printf("%s: vertices=%u triangles=%zu rebuiltBlocks=%llu "
			"sourceReads=%llu\n", label, mesh.getNoOfVertices(),
			mesh.getNoOfIndices() / 3,
			static_cast<unsigned long long>(stats.rebuiltBlocks),
			static_cast<unsigned long long>(stats.sourceReads));
	}

	// Eight octants covering [0,32)^3; the corner octant is fine (step 1) and
	// meets coarse (step 2) octants across two and three faces.
	std::vector<LodBlock> mixedLodLayout()
	{
		std::vector<LodBlock> blocks;
		int32_t id = 1;
		for (int bz = 0; bz < 2; ++bz)
		for (int by = 0; by < 2; ++by)
		for (int bx = 0; bx < 2; ++bx)
		{
			const bool fineCorner = (bx == 0 && by == 0 && bz == 0);
			const int step = fineCorner ? 1 : 2;
			blocks.push_back(LodBlock(id++,
				Vector3DInt32(bx * 16, by * 16, bz * 16),
				fineCorner ? 16 : 8, step));
		}
		return blocks;
	}
}

int main()
{
	// One layer of source samples outside the block box for central-difference
	// normals and the transition closure.
	const Region samples(-1, -1, -1, 33, 33, 33);
	LodSurface surface(samples, buildField(samples));

	// Initial uniform fine layout.
	std::vector<LodBlock> uniform;
	uniform.push_back(LodBlock(1, Vector3DInt32(0, 0, 0), 32, 1));
	surface.setLayout(uniform);
	writeObj(".task-build/lod_surface_uniform.obj", surface.mesh());
	report("uniform fine layout", surface);

	// Switch one octant to a coarser resolution; only the blocks touching the
	// new seams are re-extracted and the mesh stays watertight.
	surface.setLayout(mixedLodLayout());
	writeObj(".task-build/lod_surface_lod_switch.obj", surface.mesh());
	report("after local LOD switch", surface);

	// Apply a batch of density edits (the repeated position takes the last
	// value). Only blocks reading these samples are rebuilt.
	std::vector<SampleEdit> edits;
	edits.push_back(SampleEdit(Vector3DInt32(8, 10, 10), 120.0f));
	edits.push_back(SampleEdit(Vector3DInt32(8, 10, 10), -120.0f));
	edits.push_back(SampleEdit(Vector3DInt32(20, 14, 18), 90.0f));
	surface.updateSamples(edits);
	writeObj(".task-build/lod_surface_edited.obj", surface.mesh());
	report("after density edits", surface);

	std::printf("Wrote .task-build/lod_surface_uniform.obj, "
		".task-build/lod_surface_lod_switch.obj and .task-build/lod_surface_edited.obj\n");
	return 0;
}
