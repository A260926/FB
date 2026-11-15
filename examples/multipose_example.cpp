// Windowless multi-pose example. See multipose_example.h.
//
// The mesh is a small strip that is bent in its binding pose and deformed in
// two additional poses (one stretched along its length, one bent the other
// way). All three poses share a single baked UV set. The example runs the
// AddMesh -> ComputeCharts -> PackCharts sequence and writes an OBJ file with
// the source vertex positions and the resulting UVs.

#include "multipose_example.h"
#include "source/xatlas/xatlas.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {

// Builds a curved strip along +x. `bend` controls how much it curves upward,
// `stretch` scales the length, and `twist` offsets the curvature sign.
struct StripMesh
{
	std::vector<float> base; // xyz per vertex
	std::vector<float> poseA;
	std::vector<float> poseB;
	std::vector<uint32_t> indices;
	uint32_t vertexCount = 0;
};

static StripMesh BuildStrip(uint32_t segments, uint32_t rows)
{
	StripMesh mesh;
	const uint32_t cols = segments + 1;
	mesh.vertexCount = cols * (rows + 1);
	const float width = 1.2f;
	for (uint32_t r = 0; r <= rows; r++) {
		const float v = float(r) / float(rows);
		for (uint32_t c = 0; c <= segments; c++) {
			const float u = float(c) / float(segments);
			const float x = u * 4.0f;
			const float y = (v - 0.5f) * width;
			mesh.base.push_back(x);
			mesh.base.push_back(y);
			mesh.base.push_back(std::sin(u * 1.6f) * 0.6f);
			// Pose A: stretched along x and bent more.
			mesh.poseA.push_back(x * 1.5f);
			mesh.poseA.push_back(y);
			mesh.poseA.push_back(std::sin(u * 2.4f) * 1.1f);
			// Pose B: compressed and bent the other way.
			mesh.poseB.push_back(x * 0.7f);
			mesh.poseB.push_back(y * 0.9f);
			mesh.poseB.push_back(-std::sin(u * 1.2f) * 0.9f);
		}
	}
	for (uint32_t r = 0; r < rows; r++) {
		for (uint32_t c = 0; c < segments; c++) {
			const uint32_t i0 = r * cols + c;
			const uint32_t i1 = i0 + 1;
			const uint32_t i2 = i0 + cols;
			const uint32_t i3 = i2 + 1;
			mesh.indices.push_back(i0);
			mesh.indices.push_back(i2);
			mesh.indices.push_back(i1);
			mesh.indices.push_back(i1);
			mesh.indices.push_back(i2);
			mesh.indices.push_back(i3);
		}
	}
	return mesh;
}

static bool ChartAndPack(const StripMesh &strip, std::vector<xatlas::Vertex> *outVertices,
	std::vector<uint32_t> *outIndices, unsigned int *outChartCount, unsigned int *outWidth,
	unsigned int *outHeight)
{
	xatlas::Atlas *atlas = xatlas::Create();
	xatlas::MeshDecl decl;
	decl.vertexPositionData = strip.base.data();
	decl.vertexPositionStride = sizeof(float) * 3;
	decl.vertexCount = strip.vertexCount;
	decl.indexData = strip.indices.data();
	decl.indexCount = (uint32_t)strip.indices.size();
	decl.indexFormat = xatlas::IndexFormat::UInt32;

	xatlas::MeshPose poses[2];
	poses[0].vertexPositionData = strip.poseA.data();
	poses[0].vertexPositionStride = sizeof(float) * 3;
	poses[0].weight = 1.0f;
	poses[1].vertexPositionData = strip.poseB.data();
	poses[1].vertexPositionStride = sizeof(float) * 3;
	poses[1].weight = 1.0f;
	decl.poses = poses;
	decl.poseCount = 2;

	xatlas::AddMeshError error = xatlas::AddMesh(atlas, decl);
	if (error != xatlas::AddMeshError::Success) {
		std::fprintf(stderr, "AddMesh failed: %s\n", xatlas::StringForEnum(error));
		xatlas::Destroy(atlas);
		return false;
	}
	// AddMesh copied all position and pose data, so the source arrays can be
	// released here.
	xatlas::ComputeCharts(atlas);
	// Atlas::chartCount is only populated by PackCharts; chart creation is
	// verified by packing below.
	xatlas::PackOptions packOptions;
	packOptions.texelsPerUnit = 8.0f;
	packOptions.resolution = 256;
	packOptions.padding = 2;
	packOptions.bruteForce = true;
	xatlas::PackCharts(atlas, packOptions);
	if (atlas->meshCount != 1 || atlas->width == 0 || atlas->height == 0) {
		std::fprintf(stderr, "PackCharts failed\n");
		xatlas::Destroy(atlas);
		return false;
	}
	const xatlas::Mesh &mesh = atlas->meshes[0];
	outVertices->assign(mesh.vertexArray, mesh.vertexArray + mesh.vertexCount);
	outIndices->assign(mesh.indexArray, mesh.indexArray + mesh.indexCount);
	if (outChartCount)
		*outChartCount = atlas->chartCount;
	if (outWidth)
		*outWidth = atlas->width;
	if (outHeight)
		*outHeight = atlas->height;
	xatlas::Destroy(atlas);
	return true;
}

} // namespace

bool WriteMultiposeExampleObj(const char *filename, unsigned int *outChartCount)
{
	if (!filename)
		return false;
	const StripMesh strip = BuildStrip(10, 2);
	std::vector<xatlas::Vertex> vertices;
	std::vector<uint32_t> indices;
	unsigned int width = 0, height = 0;
	if (!ChartAndPack(strip, &vertices, &indices, outChartCount, &width, &height))
		return false;
	FILE *file = std::fopen(filename, "w");
	if (!file)
		return false;
	std::fprintf(file, "# xatlas multi-pose example\n");
	std::fprintf(file, "# %u charts, atlas %ux%u\n", outChartCount ? *outChartCount : 0, width, height);
	// Source vertex positions, one per output vertex.
	for (size_t i = 0; i < vertices.size(); i++) {
		const uint32_t xref = vertices[i].xref;
		std::fprintf(file, "v %g %g %g\n", strip.base[xref * 3 + 0], strip.base[xref * 3 + 1], strip.base[xref * 3 + 2]);
	}
	// UVs, in atlas texel units normalized by the atlas size.
	for (size_t i = 0; i < vertices.size(); i++) {
		const float u = width > 0 ? vertices[i].uv[0] / float(width) : 0.0f;
		const float v = height > 0 ? vertices[i].uv[1] / float(height) : 0.0f;
		std::fprintf(file, "vt %g %g\n", u, v);
	}
	std::fprintf(file, "o multipose\n");
	for (size_t i = 0; i + 2 < indices.size(); i += 3) {
		std::fprintf(file, "f %u/%u %u/%u %u/%u\n",
			indices[i + 0] + 1, indices[i + 0] + 1,
			indices[i + 1] + 1, indices[i + 1] + 1,
			indices[i + 2] + 1, indices[i + 2] + 1);
	}
	std::fclose(file);
	return true;
}

bool RunMultiposeExample(unsigned int *outChartCount)
{
	const StripMesh strip = BuildStrip(10, 2);
	std::vector<xatlas::Vertex> vertices;
	std::vector<uint32_t> indices;
	unsigned int width = 0, height = 0;
	return ChartAndPack(strip, &vertices, &indices, outChartCount, &width, &height);
}
