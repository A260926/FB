/*
MIT License

Copyright (c) 2018-2020 Jonathan Young

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
*/

// Periodic UV example.
//
// Builds a folded band: two planar quads meeting at a 90 degree crease. The
// quads use different materials, so xatlas turns them into separate charts.
// The two seam endpoints are then tied together with two different lattice
// transforms: a quarter turn plus an integer translation at one end and a
// reflection plus an integer translation at the other. The shared periodic
// solve produces absolute UVs that satisfy both seams exactly.
//
// The output OBJ contains one vt line per distinct parameter unknown: face
// corners that share an unknown reference the same texture coordinate, which
// makes the unique parameter path explicit.

#include "source/xatlas/xatlas.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

static bool check(bool condition, const char *message)
{
	if (!condition) {
		std::fprintf(stderr, "periodic_uv: %s\n", message);
		std::exit(2);
	}
	return condition;
}

int main(int argc, char **argv)
{
	if (argc < 2) {
		std::fprintf(stderr, "usage: %s output.obj\n", argv[0]);
		return 1;
	}
	// Six positions, four triangles. Quad A lies in the z = 0 plane, quad B
	// is folded 90 degrees around the edge v1-v2 and lies in the x = 1 plane.
	const float positions[] = {
		0.0f, 0.0f, 0.0f, // v0
		1.0f, 0.0f, 0.0f, // v1 (crease)
		1.0f, 1.0f, 0.0f, // v2 (crease)
		0.0f, 1.0f, 0.0f, // v3
		1.0f, 0.0f, 1.0f, // v4
		1.0f, 1.0f, 1.0f  // v5
	};
	const unsigned int indices[] = {
		0, 1, 2, 0, 2, 3, // quad A, material 0
		1, 4, 5, 1, 5, 2  // quad B, material 1
	};
	const unsigned int materials[] = {0, 0, 1, 1};
	xatlas::MeshDecl decl;
	decl.vertexPositionData = positions;
	decl.vertexCount = 6;
	decl.vertexPositionStride = sizeof(float) * 3;
	decl.indexData = indices;
	decl.indexCount = 12;
	decl.indexFormat = xatlas::IndexFormat::UInt32;
	decl.faceMaterialData = materials;
	xatlas::Atlas *atlas = xatlas::Create();
	check(xatlas::AddMesh(atlas, decl) == xatlas::AddMeshError::Success, "AddMesh failed");
	xatlas::ComputeCharts(atlas);
	// Face layout:
	//   face 0 (v0,v1,v2): corner 1 is v1 in chart A
	//   face 1 (v0,v2,v3): corner 1 is v2 in chart A
	//   face 2 (v1,v4,v5): corner 0 is v1 in chart B
	//   face 3 (v1,v5,v2): corner 2 is v2 in chart B
	xatlas::PeriodicRelation relations[2];
	// Seam endpoint v1: chart A corner (1, 0) maps to chart B corner (2, 0),
	// i.e. U_b = R90 * U_a + (2, -1), R90(x, y) = (-y, x).
	relations[0].a.meshIndex = 0; relations[0].a.faceIndex = 0; relations[0].a.corner = 1;
	relations[0].b.meshIndex = 0; relations[0].b.faceIndex = 2; relations[0].b.corner = 0;
	relations[0].q[0] = 0; relations[0].q[1] = -1;
	relations[0].q[2] = 1; relations[0].q[3] = 0;
	relations[0].k[0] = 2; relations[0].k[1] = -1;
	// Seam endpoint v2: chart A corner (1, 1) maps to chart B corner (2, 1),
	// i.e. U_b = F * U_a + (1, 2), F(x, y) = (x, -y).
	relations[1].a.meshIndex = 0; relations[1].a.faceIndex = 1; relations[1].a.corner = 1; // face 1 corner 1 is v2
	relations[1].b.meshIndex = 0; relations[1].b.faceIndex = 3; relations[1].b.corner = 2; // face 3 corner 2 is v2
	relations[1].q[0] = 1; relations[1].q[1] = 0;
	relations[1].q[2] = 0; relations[1].q[3] = -1;
	relations[1].k[0] = 1; relations[1].k[1] = 2;
	// Pin two corners of chart A, fixing its frame completely.
	xatlas::PeriodicPin pins[2];
	pins[0].angle.meshIndex = 0; pins[0].angle.faceIndex = 0; pins[0].angle.corner = 0; // v0
	pins[0].uv[0] = 0.0; pins[0].uv[1] = 0.0;
	pins[1].angle.meshIndex = 0; pins[1].angle.faceIndex = 1; pins[1].angle.corner = 2; // v3
	pins[1].uv[0] = 0.0; pins[1].uv[1] = 1.0;
	xatlas::PeriodicResult *result = nullptr;
	const xatlas::PeriodicError error = xatlas::ComputePeriodicUv(atlas, relations, 2, pins, 2, &result);
	if (error != xatlas::PeriodicError::Success) {
		std::fprintf(stderr, "periodic_uv: ComputePeriodicUv failed: %s\n", xatlas::StringForEnum(error));
		xatlas::Destroy(atlas);
		return 3;
	}
	check(result->meshCount == 1 && result->faceCount == 4, "unexpected result size");
	// The material split produces distinct charts for the two quads.
	const xatlas::PeriodicAngleResult *angles = result->meshes[0].angles;
	check(angles[0].chartIndex != angles[2 * 3].chartIndex, "expected separate charts from the material split");
	for (int r = 0; r < 2; r++) {
		const xatlas::PeriodicRelation &rel = relations[r];
		const double *ua = angles[rel.a.faceIndex * 3 + rel.a.corner].uv;
		const double *ub = angles[rel.b.faceIndex * 3 + rel.b.corner].uv;
		const double expected[2] = {
			rel.q[0] * ua[0] + rel.q[1] * ua[1] + rel.k[0],
			rel.q[2] * ua[0] + rel.q[3] * ua[1] + rel.k[1]
		};
		check(std::fabs(ub[0] - expected[0]) <= 1e-8 && std::fabs(ub[1] - expected[1]) <= 1e-8, "seam relation not satisfied");
	}
	// One vt per parameter unknown, so corners sharing an unknown share a vt.
	std::vector<double> vtX(result->unknownCount, 0.0), vtY(result->unknownCount, 0.0);
	std::vector<int> written(result->unknownCount, 0);
	for (uint32_t f = 0; f < result->faceCount; f++) {
		for (int c = 0; c < 3; c++) {
			const int32_t unknown = angles[f * 3 + c].unknownIndex;
			check(unknown >= 0, "invalid unknown index");
			if (!written[unknown]) {
				vtX[unknown] = angles[f * 3 + c].uv[0];
				vtY[unknown] = angles[f * 3 + c].uv[1];
				written[unknown] = 1;
			}
		}
	}
	FILE *file = fopen(argv[1], "w");
	check(file != nullptr, "could not open output file");
	std::fprintf(file, "# periodic parameterization example, %u parameter unknowns\n", result->unknownCount);
	for (int i = 0; i < 6; i++)
		std::fprintf(file, "v %g %g %g\n", positions[i * 3], positions[i * 3 + 1], positions[i * 3 + 2]);
	for (uint32_t u = 0; u < result->unknownCount; u++)
		std::fprintf(file, "vt %.10g %.10g\n", vtX[u], vtY[u]);
	std::fprintf(file, "s off\n");
	for (uint32_t f = 0; f < result->faceCount; f++) {
		std::fprintf(file, "f");
		for (int c = 0; c < 3; c++) {
			const uint32_t sourceVertex = indices[f * 3 + c];
			const int32_t unknown = angles[f * 3 + c].unknownIndex;
			std::fprintf(file, " %u/%d", sourceVertex + 1, unknown + 1); // OBJ is 1 indexed.
		}
		std::fprintf(file, "\n");
	}
	fclose(file);
	std::printf("periodic_uv: wrote %s (%u unknowns, free dimension %u)\n", argv[1], result->unknownCount, result->freeDimension);
	// The result outlives the atlas.
	xatlas::Destroy(atlas);
	check(angles[0].unknownIndex >= 0 && std::isfinite(angles[0].uv[0]), "result did not outlive the atlas");
	xatlas::DestroyPeriodicResult(result);
	return 0;
}
