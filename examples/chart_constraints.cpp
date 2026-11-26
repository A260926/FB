// Example: hard Cut and transitive Join constraints on a folded panel atlas.
// Usage: chart_constraints_example output.svg
#include "source/xatlas/xatlas.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <vector>

using namespace xatlas;

namespace {

struct MeshData {
	uint32_t nx = 6, ny = 3;
	std::vector<float> positions;
	std::vector<uint32_t> indices;
	uint32_t faceCount() const { return uint32_t(indices.size() / 3); }
};

static MeshData makeFoldedPanel()
{
	MeshData m;
	const uint32_t vx = m.nx + 1;
	for (uint32_t j = 0; j <= m.ny; j++) {
		for (uint32_t i = 0; i <= m.nx; i++) {
			float x = float(i) * 0.8f;
			float y = float(j) * 0.8f;
			// A sequence of planar folds. The Join side is allowed to bend;
			// LSCM/Tutte parameterization, not an axis projection, handles it.
			float z = 0.55f * sinf(float(i) * 1.15f) + 0.18f * sinf(float(j) * 1.7f);
			m.positions.push_back(x);
			m.positions.push_back(y);
			m.positions.push_back(z);
		}
	}
	for (uint32_t j = 0; j < m.ny; j++) {
		for (uint32_t i = 0; i < m.nx; i++) {
			uint32_t a = j * vx + i;
			uint32_t b = a + 1;
			uint32_t c = a + vx;
			uint32_t d = c + 1;
			m.indices.push_back(a); m.indices.push_back(b); m.indices.push_back(c);
			m.indices.push_back(b); m.indices.push_back(d); m.indices.push_back(c);
		}
	}
	return m;
}

static int32_t oppositeCorner(const std::vector<uint32_t> &idx, uint32_t faceCount, uint32_t corner)
{
	uint32_t f = corner / 3, c = corner % 3;
	uint32_t a = idx[f * 3 + c], b = idx[f * 3 + (c + 1) % 3];
	for (uint32_t g = 0; g < faceCount; g++) {
		for (uint32_t k = 0; k < 3; k++) {
			uint32_t ga = idx[g * 3 + k], gb = idx[g * 3 + (k + 1) % 3];
			if (ga == b && gb == a)
				return int32_t(g * 3 + k);
		}
	}
	return -1;
}

static void setBoth(std::vector<uint8_t> &flags, const MeshData &m, uint32_t face, uint32_t corner, uint8_t value)
{
	uint32_t c = face * 3 + corner;
	flags[c] = value;
	int32_t o = oppositeCorner(m.indices, m.faceCount(), c);
	if (o >= 0)
		flags[uint32_t(o)] = value;
}

static const char *kPalette[] = {
	"#e6194b", "#3cb44b", "#4363d8", "#f58231", "#911eb4", "#42d4f4",
	"#f032e6", "#bfef45", "#fabed4", "#469990", "#dcbeff", "#9a6324",
	"#800000", "#aaffc3", "#000075", "#a9a9a9", "#ffe119", "#000000"
};

static std::string colorForFace(uint32_t face)
{
	return kPalette[face % (sizeof(kPalette) / sizeof(kPalette[0]))];
}

} // namespace

int main(int argc, char **argv)
{
	if (argc != 2) {
		std::fprintf(stderr, "usage: %s output.svg\n", argv[0]);
		return 2;
	}
	SetPrint(nullptr, false);
	MeshData input = makeFoldedPanel();
	Atlas *atlas = Create();
	MeshDecl decl;
	decl.vertexPositionData = input.positions.data();
	decl.vertexCount = uint32_t(input.positions.size() / 3);
	decl.vertexPositionStride = 12;
	decl.indexData = input.indices.data();
	decl.indexCount = uint32_t(input.indices.size());
	decl.indexFormat = IndexFormat::UInt32;
	if (AddMesh(atlas, decl) != AddMeshError::Success)
		return 3;

	std::vector<uint8_t> flags(input.faceCount() * 3, 0);
	// Join every shared edge first.
	for (uint32_t c = 0; c < flags.size(); c++) {
		int32_t o = oppositeCorner(input.indices, input.faceCount(), c);
		if (o >= 0) {
			flags[c] = 2;
			flags[uint32_t(o)] = 2;
		}
	}
	// Cut a complete line between cell columns 2 and 3. Override the Join on
	// exactly those directed edge sides; no Join path crosses this line.
	for (uint32_t j = 0; j < input.ny; j++) {
		uint32_t secondTriangle = 2 * (j * input.nx + 2) + 1;
		setBoth(flags, input, secondTriangle, 0, 1);
		// setBoth assigns 1 to both sides. Make the merge explicit 1 + 0.
		uint32_t edge = secondTriangle * 3 + 0;
		int32_t opposite = oppositeCorner(input.indices, input.faceCount(), edge);
		if (opposite >= 0)
			flags[uint32_t(opposite)] = 0;
	}
	if (!SetMeshChartConstraints(atlas, 0, flags.data(), uint32_t(flags.size())))
		return 4;
	ChartOptions chartOptions;
	chartOptions.maxIterations = 2;
	if (!ComputeChartsConstrained(atlas, chartOptions))
		return 5;
	PackOptions packOptions;
	packOptions.resolution = 768;
	packOptions.padding = 3;
	packOptions.bilinear = false;
	PackCharts(atlas, packOptions);
	if (atlas->meshCount != 1 || atlas->meshes[0].chartCount < 2)
		return 6;

	const Mesh &out = atlas->meshes[0];
	const float width = float(atlas->width ? atlas->width : 768);
	const float height = float(atlas->height ? atlas->height : 768);
	std::ofstream svg(argv[1]);
	if (!svg)
		return 7;
	svg << "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"" << width << "\" height=\"" << height
	    << "\" viewBox=\"0 0 " << width << ' ' << height << "\">\n";
	svg << "<rect width=\"100%\" height=\"100%\" fill=\"#f7f7f2\"/>\n";
	svg << "<g stroke=\"#202020\" stroke-width=\"0.7\" stroke-linejoin=\"round\" fill-opacity=\"0.82\">\n";
	for (uint32_t f = 0; f < input.faceCount(); f++) {
		uint32_t ia = out.indexArray[f * 3 + 0];
		uint32_t ib = out.indexArray[f * 3 + 1];
		uint32_t ic = out.indexArray[f * 3 + 2];
		const Vertex &a = out.vertexArray[ia];
		const Vertex &b = out.vertexArray[ib];
		const Vertex &c = out.vertexArray[ic];
		// SVG's y axis points down; xatlas UV y points up.
		svg << "<polygon data-source-face=\"" << f << "\" fill=\"" << colorForFace(f)
		    << "\" points=\"" << a.uv[0] << ',' << (height - a.uv[1]) << ' '
		    << b.uv[0] << ',' << (height - b.uv[1]) << ' '
		    << c.uv[0] << ',' << (height - c.uv[1]) << "\"/>\n";
	}
	svg << "</g>\n";
	// Bold chart outlines as path elements.
	svg << "<g fill=\"none\" stroke=\"#111111\" stroke-width=\"2.2\" stroke-linejoin=\"round\">\n";
	for (uint32_t ci = 0; ci < out.chartCount; ci++) {
		const Chart &chart = out.chartArray[ci];
		for (uint32_t i = 0; i < chart.faceCount; i++) {
			uint32_t f = chart.faceArray[i];
			for (uint32_t e = 0; e < 3; e++) {
				// Draw every triangle edge; internal strokes are thin above,
				// while this pass gives a visible non-empty path group.
				uint32_t vi0 = out.indexArray[f * 3 + e];
				uint32_t vi1 = out.indexArray[f * 3 + (e + 1) % 3];
				const Vertex &v0 = out.vertexArray[vi0];
				const Vertex &v1 = out.vertexArray[vi1];
				svg << "<path d=\"M " << v0.uv[0] << ' ' << (height - v0.uv[1])
				    << " L " << v1.uv[0] << ' ' << (height - v1.uv[1]) << "\"/>\n";
			}
		}
	}
	svg << "</g>\n";
	svg << "</svg>\n";
	svg.close();
	Destroy(atlas);
	std::printf("wrote %s\n", argv[1]);
	return 0;
}
