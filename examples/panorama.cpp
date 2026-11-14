// Generates a small equirectangular panorama using Rasterizer's panorama
// entry point.  Usage: panorama <output.ppm>
//
// The scene contains a large object behind the camera that straddles the
// longitude seam (so it appears on both the left and the right edge of the
// image), plus two overlapping objects in front of the camera that exercise
// radial depth ordering through the pixel shader.

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "Rasterizer.h"

using namespace swr;

namespace {

const int kWidth = 192;
const int kHeight = 96;
const float kNear = 0.05f;
const float kFar = 1000.0f;

std::vector<unsigned char> g_color; // RGB8
std::vector<float> g_depth;

// Colour shader: interpolates the per-vertex RGB colour and keeps the
// nearest radial distance per pixel.  This is the same place the regular
// pipeline lets the shader do depth testing and discard.
struct DemoShader : PixelShaderBase<DemoShader> {
	static const int AVarCount = 3;
	static const int PVarCount = 0;
	static const bool InterpolateZ = true;
	static const bool InterpolateW = true;

	static void drawPixel(const PixelData &p)
	{
		if (p.x < 0 || p.x >= kWidth || p.y < 0 || p.y >= kHeight)
			return;

		const size_t index = (size_t)p.y * kWidth + p.x;
		if (p.z >= g_depth[index])
			return;

		g_depth[index] = p.z;

		unsigned char *out = &g_color[index * 3];
		for (int c = 0; c < 3; ++c) {
			float v = p.avar[c];
			if (v < 0.0f) v = 0.0f;
			if (v > 1.0f) v = 1.0f;
			out[c] = (unsigned char)(v * 255.0f + 0.5f);
		}
	}
};

struct Mesh {
	std::vector<RasterizerVertex> vertices;
	std::vector<int> indices;

	// Adds a triangle with a solid colour.
	void add(const float a[3], const float b[3], const float c[3],
		float r, float g, float bl)
	{
		const int base = (int)vertices.size();
		const float *points[3] = { a, b, c };
		for (int i = 0; i < 3; ++i) {
			RasterizerVertex v;
			v.x = points[i][0];
			v.y = points[i][1];
			v.z = points[i][2];
			v.w = 1.0f;
			for (int k = 0; k < MaxAVars; ++k) v.avar[k] = 0.0f;
			for (int k = 0; k < MaxPVars; ++k) v.pvar[k] = 0.0f;
			v.avar[0] = r;
			v.avar[1] = g;
			v.avar[2] = bl;
			vertices.push_back(v);
		}
		indices.push_back(base);
		indices.push_back(base + 1);
		indices.push_back(base + 2);
	}

	// Adds a planar quad as two triangles.
	void quad(float x0, float y0, float z0,
		float x1, float y1, float z1,
		float x2, float y2, float z2,
		float x3, float y3, float z3,
		float r, float g, float b)
	{
		const float p0[3] = { x0, y0, z0 };
		const float p1[3] = { x1, y1, z1 };
		const float p2[3] = { x2, y2, z2 };
		const float p3[3] = { x3, y3, z3 };
		add(p0, p1, p2, r, g, b);
		add(p0, p2, p3, r, g, b);
	}
};

} // namespace

int main(int argc, char **argv)
{
	if (argc != 2) {
		std::fprintf(stderr, "usage: %s <output.ppm>\n", argv[0]);
		return 2;
	}

	g_color.assign((size_t)kWidth * kHeight * 3, 0);
	g_depth.assign((size_t)kWidth * kHeight, 3.0e38f);

	Mesh mesh;

	// A large wall behind the camera at z = -5.  Its two triangles straddle
	// the longitude seam: the left and right image edges both see it.
	mesh.quad(
		-3.5f, -2.5f, -5.0f,
		 3.5f, -2.5f, -5.0f,
		 3.5f,  2.5f, -5.0f,
		-3.5f,  2.5f, -5.0f,
		0.12f, 0.35f, 0.85f);

	// A mid-distance plane in front of the camera, submitted before the
	// nearer object so that the shader's depth test decides the winner.
	mesh.quad(
		-1.8f, -1.0f, 4.2f,
		 1.8f, -1.0f, 4.2f,
		 1.8f,  1.0f, 4.2f,
		-1.8f,  1.0f, 4.2f,
		0.8f, 0.2f, 0.2f);

	// The nearer occluder, a small diamond-ish pair of triangles.
	mesh.quad(
		-0.8f, -0.6f, 2.0f,
		 0.9f, -0.6f, 2.0f,
		 0.5f,  1.0f, 2.0f,
		-1.0f,  1.0f, 2.0f,
		0.95f, 0.85f, 0.15f);

	Rasterizer rasterizer;
	rasterizer.setScissorRect(0, 0, kWidth, kHeight);
	rasterizer.setPixelShader<DemoShader>();

	PanoramaStats stats;
	rasterizer.drawPanoramaTriangleList(&mesh.vertices[0], &mesh.indices[0],
		mesh.indices.size(), kWidth, kHeight, kNear, kFar, &stats);

	std::FILE *out = std::fopen(argv[1], "wb");
	if (!out) {
		std::fprintf(stderr, "cannot open %s\n", argv[1]);
		return 3;
	}
	std::fprintf(out, "P3\n%d %d\n255\n", kWidth, kHeight);
	for (int y = 0; y < kHeight; ++y) {
		for (int x = 0; x < kWidth; ++x) {
			const size_t index = (size_t)y * kWidth + x;
			const unsigned char *c = &g_color[index * 3];
			std::fprintf(out, "%d %d %d\n", c[0], c[1], c[2]);
		}
	}
	std::fclose(out);

	std::printf("panorama %dx%d fragments=%lu bin_refs=%lu tests=%lu\n",
		kWidth, kHeight,
		(unsigned long)stats.emitted_fragments,
		(unsigned long)stats.bin_references,
		(unsigned long)stats.candidate_tests);

	return 0;
}
