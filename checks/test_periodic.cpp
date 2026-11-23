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

// Tests for the periodic parameterization entry point. The small mesh cases
// carry an independent dense constraint elimination and dense KKT least
// squares reference implementation; the large mesh case exercises the
// sparse path.

#include "source/xatlas/xatlas.h"
#include "task_support/check.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

// ---------------------------------------------------------------------------
// Independent dense reference machinery.
// ---------------------------------------------------------------------------

struct DenseMatrix
{
	int n = 0;
	std::vector<double> a;

	DenseMatrix() {}
	DenseMatrix(int n_) : n(n_), a(n_ * n_, 0.0) {}

	double &at(int row, int col) { return a[row * n + col]; }
	double at(int row, int col) const { return a[row * n + col]; }
};

// General Gaussian elimination with partial pivoting. Solves A x = b,
// returns false if singular.
static bool denseSolve(DenseMatrix a, std::vector<double> b, std::vector<double> &x)
{
	const int n = a.n;
	x.assign(n, 0.0);
	for (int col = 0; col < n; col++) {
		int pivot = col;
		for (int row = col + 1; row < n; row++) {
			if (std::fabs(a.at(row, col)) > std::fabs(a.at(pivot, col)))
				pivot = row;
		}
		if (std::fabs(a.at(pivot, col)) < 1e-12)
			return false;
		if (pivot != col) {
			for (int j = 0; j < n; j++)
				std::swap(a.at(col, j), a.at(pivot, j));
			std::swap(b[col], b[pivot]);
		}
		for (int row = 0; row < n; row++) {
			if (row == col)
				continue;
			const double factor = a.at(row, col) / a.at(col, col);
			for (int j = col; j < n; j++)
				a.at(row, j) -= factor * a.at(col, j);
			b[row] -= factor * b[col];
		}
	}
	for (int i = 0; i < n; i++)
		x[i] = b[i] / a.at(i, i);
	return true;
}

struct RefTriangle
{
	int v[3];
	double z1[2], z2[2];
	double weight;
};

struct RefConstraint
{
	// coeff[var] is a 2x2 row major matrix, rhs a 2-vector.
	std::unordered_map<int, std::vector<double> > coeff;
	double rhs[2];
};

// Dense reference least squares with exact constraints:
//   minimize 1/2 u^T H u subject to C u = d
// solved as the KKT system [H C^T; C 0] [u; lambda] = [0; d].
static bool denseReferenceSolve(int unknownCount, const std::vector<RefTriangle> &triangles, const std::vector<RefConstraint> &constraints, std::vector<double> &uvOut)
{
	const int n2 = unknownCount * 2;
	DenseMatrix h(n2);
	for (size_t t = 0; t < triangles.size(); t++) {
		const RefTriangle &tri = triangles[t];
		const double coef[3][2] = {
			{-tri.z1[0] + tri.z2[0], -tri.z1[1] + tri.z2[1]}, // -z1 + z2 on U0
			{-tri.z2[0], -tri.z2[1]}, // -z2 on U1
			{tri.z1[0], tri.z1[1]} // z1 on U2
		};
		for (int i = 0; i < 3; i++) {
			for (int j = 0; j < 3; j++) {
				// J^T J block for complex coefficient c_i, c_j.
				const double cr = coef[i][0], ci = coef[i][1];
				const double dr = coef[j][0], di = coef[j][1];
				const double xx = cr * dr + ci * di;
				const double xy = -cr * di + ci * dr;
				const int vi = tri.v[i], vj = tri.v[j];
				h.at(vi * 2 + 0, vj * 2 + 0) += tri.weight * xx;
				h.at(vi * 2 + 0, vj * 2 + 1) += tri.weight * xy;
				h.at(vi * 2 + 1, vj * 2 + 0) += tri.weight * -xy;
				h.at(vi * 2 + 1, vj * 2 + 1) += tri.weight * xx;
			}
		}
	}
	const int m2 = (int)constraints.size() * 2;
	const int kktSize = n2 + m2;
	DenseMatrix kkt(kktSize);
	std::vector<double> rhs(kktSize, 0.0);
	for (int i = 0; i < n2; i++)
		for (int j = 0; j < n2; j++)
			kkt.at(i, j) = h.at(i, j);
	for (size_t c = 0; c < constraints.size(); c++) {
		const RefConstraint &con = constraints[c];
		for (std::unordered_map<int, std::vector<double> >::const_iterator it = con.coeff.begin(); it != con.coeff.end(); ++it) {
			const int var = it->first;
			const double *q = it->second.data();
			for (int row = 0; row < 2; row++) {
				for (int col = 0; col < 2; col++) {
					kkt.at(n2 + (int)c * 2 + row, var * 2 + col) = q[row * 2 + col];
					kkt.at(var * 2 + col, n2 + (int)c * 2 + row) = q[row * 2 + col];
				}
			}
		}
		rhs[n2 + (int)c * 2 + 0] = con.rhs[0];
		rhs[n2 + (int)c * 2 + 1] = con.rhs[1];
	}
	std::vector<double> solution;
	if (!denseSolve(kkt, rhs, solution))
		return false;
	uvOut.assign(n2, 0.0);
	for (int i = 0; i < n2; i++)
		uvOut[i] = solution[i];
	return true;
}

// Rank of a dense matrix via Gaussian elimination on a copy.
static int denseRank(const DenseMatrix &input)
{
	DenseMatrix a = input;
	const int n = a.n;
	int rank = 0;
	for (int col = 0; col < n; col++) {
		int pivot = -1;
		for (int row = rank; row < n; row++) {
			if (std::fabs(a.at(row, col)) > 1e-10) {
				pivot = row;
				break;
			}
		}
		if (pivot < 0)
			continue;
		for (int j = 0; j < n; j++)
			std::swap(a.at(rank, j), a.at(pivot, j));
		for (int row = rank + 1; row < n; row++) {
			const double factor = a.at(row, col) / a.at(rank, col);
			for (int j = col; j < n; j++)
				a.at(row, j) -= factor * a.at(rank, j);
		}
		rank++;
	}
	return rank;
}

// Builds the reference triangle list from input positions and the unknown
// assignment exposed by a periodic result.
static void buildRefTriangles(const std::vector<float> &positions, const std::vector<uint32_t> &indices, const xatlas::PeriodicResult *result, int meshIndex, std::vector<RefTriangle> &out)
{
	const xatlas::PeriodicAngleResult *angles = result->meshes[meshIndex].angles;
	const uint32_t faceCount = (uint32_t)(indices.size() / 3);
	for (uint32_t face = 0; face < faceCount; face++) {
		RefTriangle tri;
		for (int c = 0; c < 3; c++)
			tri.v[c] = angles[face * 3 + c].unknownIndex;
		const float *p0 = &positions[indices[face * 3 + 0] * 3];
		const float *p1 = &positions[indices[face * 3 + 1] * 3];
		const float *p2 = &positions[indices[face * 3 + 2] * 3];
		double e1[3] = {p1[0] - p0[0], p1[1] - p0[1], p1[2] - p0[2]};
		double e2[3] = {p2[0] - p0[0], p2[1] - p0[1], p2[2] - p0[2]};
		double cross[3] = {e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]};
		double crossLen = std::sqrt(cross[0] * cross[0] + cross[1] * cross[1] + cross[2] * cross[2]);
		const double area = crossLen * 0.5;
		double n[3] = {cross[0] / crossLen, cross[1] / crossLen, cross[2] / crossLen};
		double edge1 = std::sqrt(e1[0] * e1[0] + e1[1] * e1[1] + e1[2] * e1[2]);
		double ax[3] = {e1[0] / edge1, e1[1] / edge1, e1[2] / edge1};
		double ay[3] = {n[1] * ax[2] - n[2] * ax[1], n[2] * ax[0] - n[0] * ax[2], n[0] * ax[1] - n[1] * ax[0]};
		tri.z1[0] = edge1;
		tri.z1[1] = 0.0;
		tri.z2[0] = e2[0] * ax[0] + e2[1] * ax[1] + e2[2] * ax[2];
		tri.z2[1] = e2[0] * ay[0] + e2[1] * ay[1] + e2[2] * ay[2];
		tri.weight = 1.0 / (2.0 * area);
		out.push_back(tri);
	}
}

static void relationConstraint(const xatlas::PeriodicResult *result, const xatlas::PeriodicRelation &rel, RefConstraint &out)
{
	static const double identity[4] = {1, 0, 0, 1};
	const int va = result->meshes[rel.a.meshIndex].angles[rel.a.faceIndex * 3 + rel.a.corner].unknownIndex;
	const int vb = result->meshes[rel.b.meshIndex].angles[rel.b.faceIndex * 3 + rel.b.corner].unknownIndex;
	// U_b - Q U_a = k.
	out.coeff.clear();
	out.coeff[vb] = std::vector<double>(identity, identity + 4);
	std::vector<double> negQ(4);
	for (int i = 0; i < 4; i++)
		negQ[i] = -(double)rel.q[i];
	out.coeff[va] = negQ;
	out.rhs[0] = rel.k[0];
	out.rhs[1] = rel.k[1];
}

static double maxRelationResidual(const xatlas::PeriodicResult *result, const std::vector<xatlas::PeriodicRelation> &relations)
{
	double worst = 0.0;
	for (size_t r = 0; r < relations.size(); r++) {
		const xatlas::PeriodicRelation &rel = relations[r];
		const double *ua = result->meshes[rel.a.meshIndex].angles[rel.a.faceIndex * 3 + rel.a.corner].uv;
		const double *ub = result->meshes[rel.b.meshIndex].angles[rel.b.faceIndex * 3 + rel.b.corner].uv;
		const double ex = rel.q[0] * ua[0] + rel.q[1] * ua[1] + rel.k[0];
		const double ey = rel.q[2] * ua[0] + rel.q[3] * ua[1] + rel.k[1];
		worst = std::max(worst, std::fabs(ub[0] - ex));
		worst = std::max(worst, std::fabs(ub[1] - ey));
	}
	return worst;
}

static double maxPinResidual(const xatlas::PeriodicResult *result, const std::vector<xatlas::PeriodicPin> &pins)
{
	double worst = 0.0;
	for (size_t p = 0; p < pins.size(); p++) {
		const xatlas::PeriodicPin &pin = pins[p];
		const double *uv = result->meshes[pin.angle.meshIndex].angles[pin.angle.faceIndex * 3 + pin.angle.corner].uv;
		worst = std::max(worst, std::fabs(uv[0] - pin.uv[0]));
		worst = std::max(worst, std::fabs(uv[1] - pin.uv[1]));
	}
	return worst;
}

// ---------------------------------------------------------------------------
// Mesh construction helpers.
// ---------------------------------------------------------------------------

struct TestMesh
{
	std::vector<float> positions;
	std::vector<uint32_t> indices;
	std::vector<uint32_t> materials;
};

// Two planar quads in different materials, four triangles, matching the
// baseline layout.
static TestMesh makeFoldedBand()
{
	TestMesh mesh;
	const float p[] = {0, 0, 0, 1, 0, 0, 0, 1, 0, 1, 1, 0, 0, 2, 1, 1, 2, 1};
	mesh.positions.assign(p, p + 18);
	const uint32_t ix[] = {0, 1, 3, 0, 3, 2, 2, 3, 5, 2, 5, 4};
	mesh.indices.assign(ix, ix + 12);
	const uint32_t mat[] = {1, 1, 2, 2};
	mesh.materials.assign(mat, mat + 4);
	return mesh;
}

// A curved pyramid patch: four triangles around a raised apex. The apex has
// positive intrinsic curvature, so the conformal least squares energy is
// strictly positive on the constrained space (no exact developable null
// mode), giving a well conditioned unique minimum.
static TestMesh makeCurvedPatch()
{
	TestMesh mesh;
	const float p[] = {
		-1.0f, -1.0f, 0.0f, // v0 base corner
		1.0f, -1.0f, 0.0f, // v1 base corner
		1.0f, 1.0f, 0.0f, // v2 base corner
		-1.0f, 1.0f, 0.0f, // v3 base corner
		0.0f, 0.0f, 1.0f // v4 raised apex
	};
	mesh.positions.assign(p, p + 15);
	const uint32_t ix[] = {
		0, 1, 4, // mat 0
		1, 2, 4, // mat 0
		2, 3, 4, // mat 1
		3, 0, 4 // mat 1
	};
	mesh.indices.assign(ix, ix + 12);
	const uint32_t mat[] = {0, 0, 1, 1};
	mesh.materials.assign(mat, mat + 4);
	return mesh;
}

static xatlas::Atlas *makeAtlas(const TestMesh &mesh)
{
	xatlas::MeshDecl decl;
	decl.vertexPositionData = mesh.positions.data();
	decl.vertexCount = (uint32_t)(mesh.positions.size() / 3);
	decl.vertexPositionStride = sizeof(float) * 3;
	decl.indexData = mesh.indices.data();
	decl.indexCount = (uint32_t)mesh.indices.size();
	decl.indexFormat = xatlas::IndexFormat::UInt32;
	decl.faceMaterialData = mesh.materials.data();
	xatlas::Atlas *atlas = xatlas::Create();
	if (xatlas::AddMesh(atlas, decl) != xatlas::AddMeshError::Success) {
		xatlas::Destroy(atlas);
		return nullptr;
	}
	xatlas::ComputeCharts(atlas);
	return atlas;
}

// A planar grid split into two materials down the middle.
static TestMesh makeGrid(uint32_t cells)
{
	TestMesh mesh;
	const uint32_t stride = cells + 1;
	for (uint32_t y = 0; y <= cells; y++) {
		for (uint32_t x = 0; x <= cells; x++) {
			mesh.positions.push_back((float)x);
			mesh.positions.push_back((float)y);
			mesh.positions.push_back(0.0f);
		}
	}
	for (uint32_t y = 0; y < cells; y++) {
		for (uint32_t x = 0; x < cells; x++) {
			const uint32_t v00 = y * stride + x;
			const uint32_t v10 = v00 + 1;
			const uint32_t v01 = v00 + stride;
			const uint32_t v11 = v01 + 1;
			mesh.indices.push_back(v00);
			mesh.indices.push_back(v10);
			mesh.indices.push_back(v11);
			mesh.indices.push_back(v00);
			mesh.indices.push_back(v11);
			mesh.indices.push_back(v01);
			const uint32_t material = x < cells / 2 ? 0 : 1;
			mesh.materials.push_back(material);
			mesh.materials.push_back(material);
		}
	}
	return mesh;
}

// Finds a face corner (face, corner) that references the given vertex.
static bool findCorner(const TestMesh &mesh, uint32_t vertex, xatlas::PeriodicAngle &angle)
{
	for (uint32_t face = 0; face < mesh.indices.size() / 3; face++) {
		for (uint32_t corner = 0; corner < 3; corner++) {
			if (mesh.indices[face * 3 + corner] == vertex) {
				angle.meshIndex = 0;
				angle.faceIndex = face;
				angle.corner = corner;
				return true;
			}
		}
	}
	return false;
}

// ---------------------------------------------------------------------------
// mapping group.
// ---------------------------------------------------------------------------

static int testMaterialSplitCharts()
{
	TestMesh mesh = makeFoldedBand();
	xatlas::Atlas *atlas = makeAtlas(mesh);
	TASK_CHECK(atlas != nullptr);
	// Two pins per material chart: each disconnected component needs two
	// anchors for a unique minimum, otherwise its free rotation and scale
	// collapse onto the single pin.
	xatlas::PeriodicPin pins[4];
	TASK_CHECK(findCorner(mesh, 0, pins[0].angle));
	pins[0].uv[0] = 0.0; pins[0].uv[1] = 0.0;
	TASK_CHECK(findCorner(mesh, 1, pins[1].angle));
	pins[1].uv[0] = 1.0; pins[1].uv[1] = 0.0;
	TASK_CHECK(findCorner(mesh, 4, pins[2].angle));
	pins[2].uv[0] = 3.0; pins[2].uv[1] = 4.0;
	TASK_CHECK(findCorner(mesh, 5, pins[3].angle));
	pins[3].uv[0] = 4.0; pins[3].uv[1] = 4.0;
	xatlas::PeriodicResult *result = nullptr;
	TASK_CHECK(xatlas::ComputePeriodicUv(atlas, nullptr, 0, pins, 4, &result) == xatlas::PeriodicError::Success);
	TASK_CHECK(result->meshCount == 1 && result->faceCount == 4);
	const xatlas::PeriodicAngleResult *angles = result->meshes[0].angles;
	// Faces 0,1 are material 1 and faces 2,3 material 2: distinct charts.
	TASK_CHECK(angles[0 * 3].chartIndex != angles[2 * 3].chartIndex);
	// Two corners of the same source vertex inside one chart share an unknown.
	TASK_CHECK(mesh.indices[0 * 3 + 2] == mesh.indices[1 * 3 + 1]); // vertex 3
	TASK_CHECK(angles[0 * 3 + 2].unknownIndex == angles[1 * 3 + 1].unknownIndex);
	// Corners of a shared position split across charts have distinct unknowns.
	bool foundSplit = false;
	for (uint32_t f1 = 0; f1 < 2 && !foundSplit; f1++) {
		for (uint32_t c1 = 0; c1 < 3 && !foundSplit; c1++) {
			for (uint32_t f2 = 2; f2 < 4 && !foundSplit; f2++) {
				for (uint32_t c2 = 0; c2 < 3; c2++) {
					const uint32_t x1 = mesh.indices[f1 * 3 + c1];
					const uint32_t x2 = mesh.indices[f2 * 3 + c2];
					if (x1 == x2) {
						TASK_CHECK(angles[f1 * 3 + c1].unknownIndex != angles[f2 * 3 + c2].unknownIndex);
						TASK_CHECK(angles[f1 * 3 + c1].chartIndex != angles[f2 * 3 + c2].chartIndex);
						foundSplit = true;
					}
				}
			}
		}
	}
	TASK_CHECK(foundSplit);
	TASK_CHECK(maxPinResidual(result, std::vector<xatlas::PeriodicPin>(pins, pins + 4)) <= 1e-8);
	xatlas::DestroyPeriodicResult(result);
	xatlas::Destroy(atlas);
	return 0;
}

static int testColocalIdentity()
{
	// Eight vertices: v4..v7 duplicate the positions of v0..v3. Same material
	// inside each quad, different material across quads.
	TestMesh mesh;
	const float p[] = {
		0, 0, 0, 1, 0, 0, 0, 1, 0, 1, 1, 0,
		0, 0, 0, 1, 0, 0, 0, 1, 0, 1, 1, 0
	};
	mesh.positions.assign(p, p + 24);
	const uint32_t ix[] = {0, 1, 3, 0, 3, 2, 4, 5, 7, 4, 7, 6};
	mesh.indices.assign(ix, ix + 12);
	const uint32_t mat[] = {0, 0, 1, 1};
	mesh.materials.assign(mat, mat + 4);
	xatlas::Atlas *atlas = makeAtlas(mesh);
	TASK_CHECK(atlas != nullptr);
	xatlas::PeriodicPin pin;
	TASK_CHECK(findCorner(mesh, 0, pin.angle));
	pin.uv[0] = 1.0; pin.uv[1] = 2.0;
	xatlas::PeriodicResult *result = nullptr;
	TASK_CHECK(xatlas::ComputePeriodicUv(atlas, nullptr, 0, &pin, 1, &result) == xatlas::PeriodicError::Success);
	const xatlas::PeriodicAngleResult *angles = result->meshes[0].angles;
	// v0 and v4 are colocal but distinct source vertices. Inside chart A the
	// duplicated corner used by its two triangles unifies; across the material
	// boundary they stay separate unknowns.
	int unknownV0 = -1, unknownV4 = -1;
	for (uint32_t face = 0; face < 4; face++) {
		for (uint32_t c = 0; c < 3; c++) {
			const uint32_t xref = mesh.indices[face * 3 + c];
			if (xref == 0)
				unknownV0 = angles[face * 3 + c].unknownIndex;
			if (xref == 4)
				unknownV4 = angles[face * 3 + c].unknownIndex;
		}
	}
	TASK_CHECK(unknownV0 >= 0 && unknownV4 >= 0);
	TASK_CHECK(unknownV0 != unknownV4);
	// Welding by source xref alone would have merged them; the chart path
	// keeps the two chart identities apart.
	TASK_CHECK(angles[0 * 3].chartIndex != angles[2 * 3].chartIndex);
	xatlas::DestroyPeriodicResult(result);
	xatlas::Destroy(atlas);
	return 0;
}

static int testAngleAndInputValidation()
{
	TestMesh mesh = makeFoldedBand();
	xatlas::Atlas *atlas = makeAtlas(mesh);
	TASK_CHECK(atlas != nullptr);
	xatlas::PeriodicResult *result = nullptr;
	xatlas::PeriodicRelation bad;
	bad = xatlas::PeriodicRelation();
	bad.a.meshIndex = 99;
	TASK_CHECK(xatlas::ComputePeriodicUv(atlas, &bad, 1, nullptr, 0, &result) == xatlas::PeriodicError::InvalidAngle);
	bad = xatlas::PeriodicRelation();
	bad.a.faceIndex = 99;
	TASK_CHECK(xatlas::ComputePeriodicUv(atlas, &bad, 1, nullptr, 0, &result) == xatlas::PeriodicError::InvalidAngle);
	bad = xatlas::PeriodicRelation();
	bad.a.corner = 3;
	TASK_CHECK(xatlas::ComputePeriodicUv(atlas, &bad, 1, nullptr, 0, &result) == xatlas::PeriodicError::InvalidAngle);
	// Q must be a signed permutation.
	bad = xatlas::PeriodicRelation();
	bad.b.faceIndex = 1;
	bad.q[0] = 2; bad.q[1] = 0; bad.q[2] = 0; bad.q[3] = 1;
	TASK_CHECK(xatlas::ComputePeriodicUv(atlas, &bad, 1, nullptr, 0, &result) == xatlas::PeriodicError::InvalidTransform);
	// Non finite pin.
	xatlas::PeriodicPin pin;
	TASK_CHECK(findCorner(mesh, 0, pin.angle));
	pin.uv[0] = std::numeric_limits<double>::infinity();
	pin.uv[1] = 0.0;
	TASK_CHECK(xatlas::ComputePeriodicUv(atlas, nullptr, 0, &pin, 1, &result) == xatlas::PeriodicError::NonFiniteGeometry);
	TASK_CHECK(result == nullptr);
	xatlas::Destroy(atlas);
	return 0;
}

static void dummyParamFunc(const float *, float *texcoords, uint32_t vertexCount, const uint32_t *, uint32_t)
{
	for (uint32_t i = 0; i < vertexCount; i++) {
		texcoords[i * 2] = 0.0f;
		texcoords[i * 2 + 1] = 0.0f;
	}
}

static int testGeometryAndStateValidation()
{
	TestMesh mesh = makeCurvedPatch();
	// A custom parameterization function is not supported by the periodic path.
	xatlas::MeshDecl decl;
	decl.vertexPositionData = mesh.positions.data();
	decl.vertexCount = (uint32_t)(mesh.positions.size() / 3);
	decl.vertexPositionStride = sizeof(float) * 3;
	decl.indexData = mesh.indices.data();
	decl.indexCount = (uint32_t)mesh.indices.size();
	decl.indexFormat = xatlas::IndexFormat::UInt32;
	decl.faceMaterialData = mesh.materials.data();
	xatlas::Atlas *atlas = xatlas::Create();
	TASK_CHECK(atlas != nullptr);
	TASK_CHECK(xatlas::AddMesh(atlas, decl) == xatlas::AddMeshError::Success);
	xatlas::ChartOptions options;
	options.paramFunc = dummyParamFunc;
	xatlas::ComputeCharts(atlas, options);
	xatlas::PeriodicPin pins[2];
	TASK_CHECK(findCorner(mesh, 0, pins[0].angle));
	pins[0].uv[0] = 0.0; pins[0].uv[1] = 0.0;
	TASK_CHECK(findCorner(mesh, 1, pins[1].angle));
	pins[1].uv[0] = 1.0; pins[1].uv[1] = 0.0;
	xatlas::PeriodicResult *result = nullptr;
	TASK_CHECK(xatlas::ComputePeriodicUv(atlas, nullptr, 0, pins, 2, &result) == xatlas::PeriodicError::InvalidState);
	TASK_CHECK(result == nullptr);
	xatlas::Destroy(atlas);
	// Calling before ComputeCharts is an invalid state as well.
	atlas = xatlas::Create();
	TASK_CHECK(xatlas::AddMesh(atlas, decl) == xatlas::AddMeshError::Success);
	TASK_CHECK(xatlas::ComputePeriodicUv(atlas, nullptr, 0, nullptr, 0, &result) == xatlas::PeriodicError::InvalidState);
	TASK_CHECK(result == nullptr);
	xatlas::Destroy(atlas);
	TASK_CHECK(xatlas::ComputePeriodicUv(nullptr, nullptr, 0, nullptr, 0, &result) == xatlas::PeriodicError::InvalidState);
	// The engine rejects degenerate triangle terms with a specific reason.
	xatlas::internal::periodic::EngineTriangle degenerate;
	degenerate.v[0] = 0; degenerate.v[1] = 1; degenerate.v[2] = 2;
	degenerate.z1[0] = 1.0; degenerate.z1[1] = 0.0;
	degenerate.z2[0] = 0.0; degenerate.z2[1] = 1.0;
	degenerate.weight = 0.0;
	double uv[6] = {0};
	uint32_t freeDimension = 0;
	double relativeResidual = 0.0;
	TASK_CHECK(xatlas::internal::periodic::solve(3, nullptr, 0, nullptr, 0, &degenerate, 1, uv, &freeDimension, &relativeResidual) == xatlas::PeriodicError::DegenerateGeometry);
	// A successful curved patch solve reports a small reduced residual.
	xatlas::Atlas *goodAtlas = makeAtlas(mesh);
	TASK_CHECK(goodAtlas != nullptr);
	xatlas::PeriodicResult *goodResult = nullptr;
	TASK_CHECK(xatlas::ComputePeriodicUv(goodAtlas, nullptr, 0, pins, 2, &goodResult) == xatlas::PeriodicError::Success);
	TASK_CHECK(goodResult->relativeResidual <= 1e-8);
	TASK_CHECK(std::isfinite(goodResult->relativeResidual));
	xatlas::DestroyPeriodicResult(goodResult);
	xatlas::Destroy(goodAtlas);
	return 0;
}

static int testTwoChartRotationTranslation()
{
	TestMesh mesh = makeCurvedPatch();
	xatlas::Atlas *atlas = makeAtlas(mesh);
	TASK_CHECK(atlas != nullptr);
	// Pyramid face layout:
	//   face 0 (v0,v1,v4) mat 0, face 1 (v1,v2,v4) mat 0
	//   face 2 (v2,v3,v4) mat 1, face 3 (v3,v0,v4) mat 1
	std::vector<xatlas::PeriodicRelation> relations(2);
	// Shared base vertex v2: R90 plus an integer translation.
	relations[0].a.meshIndex = 0; relations[0].a.faceIndex = 1; relations[0].a.corner = 1; // v2, chart mat 0
	relations[0].b.meshIndex = 0; relations[0].b.faceIndex = 2; relations[0].b.corner = 0; // v2, chart mat 1
	relations[0].q[0] = 0; relations[0].q[1] = -1;
	relations[0].q[2] = 1; relations[0].q[3] = 0;
	relations[0].k[0] = 2; relations[0].k[1] = -3;
	// Shared apex v4: a reflection seam with integer translation.
	relations[1].a.meshIndex = 0; relations[1].a.faceIndex = 0; relations[1].a.corner = 2; // v4, chart mat 0
	relations[1].b.meshIndex = 0; relations[1].b.faceIndex = 3; relations[1].b.corner = 2; // v4, chart mat 1
	relations[1].q[0] = 1; relations[1].q[1] = 0;
	relations[1].q[2] = 0; relations[1].q[3] = -1;
	relations[1].k[0] = -1; relations[1].k[1] = 4;
	xatlas::PeriodicPin pins[3];
	TASK_CHECK(findCorner(mesh, 0, pins[0].angle));
	pins[0].uv[0] = 0.0; pins[0].uv[1] = 0.0;
	TASK_CHECK(findCorner(mesh, 1, pins[1].angle));
	pins[1].uv[0] = 2.0; pins[1].uv[1] = 0.0;
	TASK_CHECK(findCorner(mesh, 3, pins[2].angle));
	pins[2].uv[0] = 5.0; pins[2].uv[1] = -2.0;
	xatlas::PeriodicResult *result = nullptr;
	TASK_CHECK(xatlas::ComputePeriodicUv(atlas, relations.data(), 2, pins, 3, &result) == xatlas::PeriodicError::Success);
	// The material split really produced separate chart identities.
	TASK_CHECK(result->meshes[0].angles[0].chartIndex != result->meshes[0].angles[2 * 3].chartIndex);
	TASK_CHECK(maxRelationResidual(result, relations) <= 1e-8);
	TASK_CHECK(maxPinResidual(result, std::vector<xatlas::PeriodicPin>(pins, pins + 3)) <= 1e-8);
	// Independent dense reference.
	std::vector<RefTriangle> refTriangles;
	buildRefTriangles(mesh.positions, mesh.indices, result, 0, refTriangles);
	std::vector<RefConstraint> refConstraints;
	for (size_t r = 0; r < relations.size(); r++) {
		RefConstraint con;
		relationConstraint(result, relations[r], con);
		refConstraints.push_back(con);
	}
	for (int p = 0; p < 3; p++) {
		static const double identity[4] = {1, 0, 0, 1};
		RefConstraint con;
		const int var = result->meshes[0].angles[pins[p].angle.faceIndex * 3 + pins[p].angle.corner].unknownIndex;
		con.coeff[var] = std::vector<double>(identity, identity + 4);
		con.rhs[0] = pins[p].uv[0];
		con.rhs[1] = pins[p].uv[1];
		refConstraints.push_back(con);
	}
	std::vector<double> refUv;
	TASK_CHECK(denseReferenceSolve((int)result->unknownCount, refTriangles, refConstraints, refUv));
	// Two independent numerical methods (sparse reduced CG vs dense KKT);
	// agreement to 1e-7 on O(1..10) UVs proves they solve the same problem.
	for (uint32_t face = 0; face < 4; face++) {
		for (uint32_t c = 0; c < 3; c++) {
			const int unknown = result->meshes[0].angles[face * 3 + c].unknownIndex;
			const double *uv = result->meshes[0].angles[face * 3 + c].uv;
			TASK_CHECK(std::fabs(uv[0] - refUv[unknown * 2]) <= 1e-7);
			TASK_CHECK(std::fabs(uv[1] - refUv[unknown * 2 + 1]) <= 1e-7);
		}
	}
	xatlas::DestroyPeriodicResult(result);
	xatlas::Destroy(atlas);
	return 0;
}

// Engine-only synthetic systems for closed loop behavior.
static xatlas::internal::periodic::EngineTriangle makeEngineTriangle()
{
	xatlas::internal::periodic::EngineTriangle tri;
	tri.v[0] = 0; tri.v[1] = 1; tri.v[2] = 2;
	tri.z1[0] = 1.0; tri.z1[1] = 0.0;
	tri.z2[0] = 0.0; tri.z2[1] = 1.0;
	tri.weight = 1.0;
	return tri;
}

static RefTriangle makeRefTriangle()
{
	RefTriangle tri;
	tri.v[0] = 0; tri.v[1] = 1; tri.v[2] = 2;
	tri.z1[0] = 1.0; tri.z1[1] = 0.0;
	tri.z2[0] = 0.0; tri.z2[1] = 1.0;
	tri.weight = 1.0;
	return tri;
}

static RefConstraint engineRelationConstraint(const xatlas::internal::periodic::EngineRelation &rel)
{
	static const double identity[4] = {1, 0, 0, 1};
	RefConstraint con;
	con.coeff[(int)rel.varB] = std::vector<double>(identity, identity + 4);
	std::vector<double> negQ(4);
	for (int i = 0; i < 4; i++)
		negQ[i] = -rel.q[i];
	con.coeff[(int)rel.varA] = negQ;
	con.rhs[0] = rel.k[0];
	con.rhs[1] = rel.k[1];
	return con;
}

static int testReflectionLoopLineFreedom()
{
	const uint32_t n = 3;
	xatlas::internal::periodic::EngineTriangle tri = makeEngineTriangle();
	std::vector<xatlas::internal::periodic::EngineRelation> relations(3);
	// F = diag(1, -1). One reflection plus two identity relations closes a
	// loop whose accumulated transform is F. The total translation is
	// k = (0, 2), with F k = -k, so the loop is compatible and pins U0 to
	// the line y = 1, leaving a one dimensional free subspace.
	relations[0].varA = 0; relations[0].varB = 1;
	relations[0].q[0] = 1; relations[0].q[1] = 0; relations[0].q[2] = 0; relations[0].q[3] = -1;
	relations[0].k[0] = 0; relations[0].k[1] = 2;
	relations[1].varA = 1; relations[1].varB = 2;
	relations[1].q[0] = 1; relations[1].q[1] = 0; relations[1].q[2] = 0; relations[1].q[3] = 1;
	relations[1].k[0] = 0; relations[1].k[1] = 0;
	relations[2].varA = 2; relations[2].varB = 0;
	relations[2].q[0] = 1; relations[2].q[1] = 0; relations[2].q[2] = 0; relations[2].q[3] = 1;
	relations[2].k[0] = 0; relations[2].k[1] = 0;
	std::vector<double> uv(n * 2, 0.0);
	uint32_t freeDimension = 0;
	double residual = 0.0;
	TASK_CHECK(xatlas::internal::periodic::solve(n, relations.data(), 3, nullptr, 0, &tri, 1, uv.data(), &freeDimension, &residual) == xatlas::PeriodicError::Success);
	TASK_CHECK(freeDimension == 1);
	// Every unknown is restricted to the line y = 1.
	for (uint32_t v = 0; v < n; v++)
		TASK_CHECK(std::fabs(uv[v * 2 + 1] - 1.0) <= 1e-8);
	// Dense nullity of the constraint matrix is one.
	DenseMatrix c(n * 2);
	for (int r = 0; r < 3; r++) {
		const int va = relations[r].varA, vb = relations[r].varB;
		c.at(r * 2 + 0, vb * 2 + 0) += 1.0;
		c.at(r * 2 + 1, vb * 2 + 1) += 1.0;
		c.at(r * 2 + 0, va * 2 + 0) += -(double)relations[r].q[0];
		c.at(r * 2 + 0, va * 2 + 1) += -(double)relations[r].q[1];
		c.at(r * 2 + 1, va * 2 + 0) += -(double)relations[r].q[2];
		c.at(r * 2 + 1, va * 2 + 1) += -(double)relations[r].q[3];
	}
	TASK_CHECK(denseRank(c) == (int)(n * 2) - 1);
	// The line is a true energy null direction: with all unknowns on y = 1
	// and linked by the relations, every triangle term vanishes for any free
	// coordinate. The reduced minimum norm choice is therefore t = 0.
	TASK_CHECK(std::fabs(uv[0 * 2 + 0] - 0.0) <= 1e-8);
	TASK_CHECK(std::fabs(uv[1 * 2 + 0] - 0.0) <= 1e-8);
	TASK_CHECK(std::fabs(uv[2 * 2 + 0] - 0.0) <= 1e-8);
	// A pin on the line makes the minimum unique; a pin off the line conflicts.
	std::vector<xatlas::internal::periodic::EnginePin> pins(1);
	pins[0].var = 0;
	pins[0].uv[0] = 7.0; pins[0].uv[1] = 1.0;
	std::vector<double> pinnedUv(n * 2, 0.0);
	TASK_CHECK(xatlas::internal::periodic::solve(n, relations.data(), 3, pins.data(), 1, &tri, 1, pinnedUv.data(), &freeDimension, &residual) == xatlas::PeriodicError::Success);
	TASK_CHECK(std::fabs(pinnedUv[0] - 7.0) <= 1e-8 && std::fabs(pinnedUv[1] - 1.0) <= 1e-8);
	pins[0].uv[0] = 7.0; pins[0].uv[1] = 0.0;
	TASK_CHECK(xatlas::internal::periodic::solve(n, relations.data(), 3, pins.data(), 1, &tri, 1, pinnedUv.data(), &freeDimension, &residual) == xatlas::PeriodicError::ConflictingPin);
	return 0;
}

static int testRotationLoopFixedPoint()
{
	const uint32_t n = 3;
	xatlas::internal::periodic::EngineTriangle tri = makeEngineTriangle();
	std::vector<xatlas::internal::periodic::EngineRelation> relations(3);
	// R90 twice is -I, so the loop fixes U0 to a single point.
	relations[0].varA = 0; relations[0].varB = 1;
	relations[0].q[0] = 0; relations[0].q[1] = -1; relations[0].q[2] = 1; relations[0].q[3] = 0;
	relations[0].k[0] = 1; relations[0].k[1] = 0;
	relations[1].varA = 1; relations[1].varB = 0;
	relations[1].q[0] = 0; relations[1].q[1] = -1; relations[1].q[2] = 1; relations[1].q[3] = 0;
	relations[1].k[0] = 0; relations[1].k[1] = 0;
	// Pull the third unknown into the same component so the whole problem has
	// a single, fully fixed frame (free dimension 0).
	relations[2].varA = 2; relations[2].varB = 1;
	relations[2].q[0] = 1; relations[2].q[1] = 0; relations[2].q[2] = 0; relations[2].q[3] = 1;
	relations[2].k[0] = 0; relations[2].k[1] = 0;
	std::vector<double> uv(n * 2, 0.0);
	uint32_t freeDimension = 99;
	double residual = 0.0;
	TASK_CHECK(xatlas::internal::periodic::solve(n, relations.data(), 3, nullptr, 0, &tri, 1, uv.data(), &freeDimension, &residual) == xatlas::PeriodicError::Success);
	TASK_CHECK(freeDimension == 0);
	// 2 U0 = R90 (1, 0) = (0, 1).
	TASK_CHECK(std::fabs(uv[0] - 0.0) <= 1e-8);
	TASK_CHECK(std::fabs(uv[1] - 0.5) <= 1e-8);
	// And the relation to U1 holds exactly.
	const double expectedX = 0.0 * uv[0] + -1.0 * uv[1] + 1.0;
	const double expectedY = 1.0 * uv[0] + 0.0 * uv[1] + 0.0;
	TASK_CHECK(std::fabs(uv[2] - expectedX) <= 1e-8 && std::fabs(uv[3] - expectedY) <= 1e-8);
	// U2 is welded to U1 by the identity relation.
	TASK_CHECK(std::fabs(uv[4] - uv[2]) <= 1e-8 && std::fabs(uv[5] - uv[3]) <= 1e-8);
	DenseMatrix c(n * 2);
	for (int r = 0; r < 3; r++) {
		const int va = relations[r].varA, vb = relations[r].varB;
		c.at(r * 2 + 0, vb * 2 + 0) += 1.0;
		c.at(r * 2 + 1, vb * 2 + 1) += 1.0;
		c.at(r * 2 + 0, va * 2 + 0) += -(double)relations[r].q[0];
		c.at(r * 2 + 0, va * 2 + 1) += -(double)relations[r].q[1];
		c.at(r * 2 + 1, va * 2 + 0) += -(double)relations[r].q[2];
		c.at(r * 2 + 1, va * 2 + 1) += -(double)relations[r].q[3];
	}
	TASK_CHECK(denseRank(c) == (int)(n * 2));
	// Cross check the fixed point against the dense KKT reference.
	std::vector<RefTriangle> refTriangles(1, makeRefTriangle());
	std::vector<RefConstraint> refConstraints;
	for (size_t r = 0; r < relations.size(); r++)
		refConstraints.push_back(engineRelationConstraint(relations[r]));
	std::vector<double> refUv;
	TASK_CHECK(denseReferenceSolve((int)n, refTriangles, refConstraints, refUv));
	for (uint32_t v = 0; v < n; v++) {
		TASK_CHECK(std::fabs(uv[v * 2] - refUv[v * 2]) <= 1e-8);
		TASK_CHECK(std::fabs(uv[v * 2 + 1] - refUv[v * 2 + 1]) <= 1e-8);
	}
	return 0;
}

static int testInconsistentTranslationLoop()
{
	const uint32_t n = 3;
	xatlas::internal::periodic::EngineTriangle tri = makeEngineTriangle();
	std::vector<xatlas::internal::periodic::EngineRelation> relations(2);
	relations[0].varA = 0; relations[0].varB = 1;
	relations[0].q[0] = 1; relations[0].q[1] = 0; relations[0].q[2] = 0; relations[0].q[3] = 1;
	relations[0].k[0] = 1; relations[0].k[1] = 0;
	relations[1].varA = 1; relations[1].varB = 0;
	relations[1].q[0] = 1; relations[1].q[1] = 0; relations[1].q[2] = 0; relations[1].q[3] = 1;
	relations[1].k[0] = 1; relations[1].k[1] = 0;
	std::vector<double> uv(n * 2, 0.0);
	uint32_t freeDimension = 0;
	double residual = 0.0;
	TASK_CHECK(xatlas::internal::periodic::solve(n, relations.data(), 2, nullptr, 0, &tri, 1, uv.data(), &freeDimension, &residual) == xatlas::PeriodicError::InconsistentRelation);
	return 0;
}

static int testConflictingPins()
{
	const uint32_t n = 3;
	xatlas::internal::periodic::EngineTriangle tri = makeEngineTriangle();
	std::vector<xatlas::internal::periodic::EnginePin> pins(2);
	pins[0].var = 0; pins[0].uv[0] = 1.0; pins[0].uv[1] = 2.0;
	pins[1].var = 0; pins[1].uv[0] = 1.0; pins[1].uv[1] = 3.0;
	std::vector<double> uv(n * 2, 0.0);
	uint32_t freeDimension = 0;
	double residual = 0.0;
	TASK_CHECK(xatlas::internal::periodic::solve(n, nullptr, 0, pins.data(), 2, &tri, 1, uv.data(), &freeDimension, &residual) == xatlas::PeriodicError::ConflictingPin);
	// The same values twice are fine.
	pins[1].uv[1] = 2.0;
	TASK_CHECK(xatlas::internal::periodic::solve(n, nullptr, 0, pins.data(), 2, &tri, 1, uv.data(), &freeDimension, &residual) == xatlas::PeriodicError::Success);
	TASK_CHECK(std::fabs(uv[0] - 1.0) <= 1e-8 && std::fabs(uv[1] - 2.0) <= 1e-8);
	return 0;
}

// ---------------------------------------------------------------------------
// solve group.
// ---------------------------------------------------------------------------

static int testDenseLeastSquaresSmall()
{
	TestMesh mesh = makeCurvedPatch();
	xatlas::Atlas *atlas = makeAtlas(mesh);
	TASK_CHECK(atlas != nullptr);
	std::vector<xatlas::PeriodicRelation> relations(1);
	// Base vertex v2 on the material boundary gets an R270 seam.
	relations[0].a.meshIndex = 0; relations[0].a.faceIndex = 1; relations[0].a.corner = 1; // v2, chart mat 0
	relations[0].b.meshIndex = 0; relations[0].b.faceIndex = 2; relations[0].b.corner = 0; // v2, chart mat 1
	relations[0].q[0] = 0; relations[0].q[1] = 1; relations[0].q[2] = -1; relations[0].q[3] = 0; // R270
	relations[0].k[0] = 4; relations[0].k[1] = -2;
	// Three non-collinear base anchors fix the global frame.
	xatlas::PeriodicPin pins[3];
	TASK_CHECK(findCorner(mesh, 0, pins[0].angle));
	pins[0].uv[0] = -1.5; pins[0].uv[1] = 2.5;
	TASK_CHECK(findCorner(mesh, 1, pins[1].angle));
	pins[1].uv[0] = 3.0; pins[1].uv[1] = 3.0;
	TASK_CHECK(findCorner(mesh, 3, pins[2].angle));
	pins[2].uv[0] = -2.0; pins[2].uv[1] = 6.0;
	xatlas::PeriodicResult *result = nullptr;
	TASK_CHECK(xatlas::ComputePeriodicUv(atlas, relations.data(), 1, pins, 3, &result) == xatlas::PeriodicError::Success);
	std::vector<RefTriangle> refTriangles;
	buildRefTriangles(mesh.positions, mesh.indices, result, 0, refTriangles);
	std::vector<RefConstraint> refConstraints;
	RefConstraint seam;
	relationConstraint(result, relations[0], seam);
	refConstraints.push_back(seam);
	static const double identity[4] = {1, 0, 0, 1};
	for (int p = 0; p < 3; p++) {
		RefConstraint con;
		const int var = result->meshes[0].angles[pins[p].angle.faceIndex * 3 + pins[p].angle.corner].unknownIndex;
		con.coeff[var] = std::vector<double>(identity, identity + 4);
		con.rhs[0] = pins[p].uv[0];
		con.rhs[1] = pins[p].uv[1];
		refConstraints.push_back(con);
	}
	std::vector<double> refUv;
	TASK_CHECK(denseReferenceSolve((int)result->unknownCount, refTriangles, refConstraints, refUv));
	const xatlas::PeriodicAngleResult *angles = result->meshes[0].angles;
	for (uint32_t face = 0; face < 4; face++) {
		for (uint32_t c = 0; c < 3; c++) {
			const int unknown = angles[face * 3 + c].unknownIndex;
			// Independent sparse reduced CG vs dense KKT: agreement to a
			// scale-relative 1e-7 bound (the exact constraints themselves are
			// still checked at 1e-8 elsewhere).
			const double tol = 1e-7 * std::max(1.0, std::max(std::fabs(refUv[unknown * 2]), std::fabs(refUv[unknown * 2 + 1])));
			TASK_CHECK(std::fabs(angles[face * 3 + c].uv[0] - refUv[unknown * 2]) <= tol);
			TASK_CHECK(std::fabs(angles[face * 3 + c].uv[1] - refUv[unknown * 2 + 1]) <= tol);
		}
	}
	// The dense KKT solution is itself a stationary point: perturbing any
	// single unknown cannot lower the energy below the solved value.
	auto energy = [&](const std::vector<double> &u) {
		double total = 0.0;
		for (size_t t = 0; t < refTriangles.size(); t++) {
			const RefTriangle &tri = refTriangles[t];
			const double *u0 = &u[tri.v[0] * 2], *u1 = &u[tri.v[1] * 2], *u2 = &u[tri.v[2] * 2];
			double dx[2] = {u2[0] - u0[0], u2[1] - u0[1]};
			double dy[2] = {u1[0] - u0[0], u1[1] - u0[1]};
			double a[2], b[2], r[2];
			a[0] = tri.z1[0] * dx[0] - tri.z1[1] * dx[1];
			a[1] = tri.z1[0] * dx[1] + tri.z1[1] * dx[0];
			b[0] = tri.z2[0] * dy[0] - tri.z2[1] * dy[1];
			b[1] = tri.z2[0] * dy[1] + tri.z2[1] * dy[0];
			r[0] = a[0] - b[0]; r[1] = a[1] - b[1];
			total += tri.weight * (r[0] * r[0] + r[1] * r[1]);
		}
		return total;
	};
	std::vector<double> solved(refUv.size(), 0.0);
	for (uint32_t face = 0; face < 4; face++)
		for (uint32_t c = 0; c < 3; c++) {
			const int unknown = angles[face * 3 + c].unknownIndex;
			solved[unknown * 2] = angles[face * 3 + c].uv[0];
			solved[unknown * 2 + 1] = angles[face * 3 + c].uv[1];
		}
	const double baseEnergy = energy(solved);
	for (int unknown = 0; unknown < (int)result->unknownCount; unknown++) {
		for (int axis = 0; axis < 2; axis++) {
			std::vector<double> perturbed = solved;
			perturbed[unknown * 2 + axis] += 1e-5;
			TASK_CHECK(energy(perturbed) >= baseEnergy - 1e-10);
		}
	}
	TASK_CHECK(maxRelationResidual(result, relations) <= 1e-8);
	xatlas::DestroyPeriodicResult(result);
	xatlas::Destroy(atlas);
	return 0;
}

// Sparse full-space gradient H u, accumulated per triangle.
static void sparseGradient(int unknownCount, const std::vector<RefTriangle> &triangles, const std::vector<double> &uv, std::vector<double> &grad)
{
	grad.assign(unknownCount * 2, 0.0);
	for (size_t t = 0; t < triangles.size(); t++) {
		const RefTriangle &tri = triangles[t];
		const double *u0 = &uv[tri.v[0] * 2], *u1 = &uv[tri.v[1] * 2], *u2 = &uv[tri.v[2] * 2];
		double dx[2] = {u2[0] - u0[0], u2[1] - u0[1]};
		double dy[2] = {u1[0] - u0[0], u1[1] - u0[1]};
		double a[2], b[2], r[2];
		a[0] = tri.z1[0] * dx[0] - tri.z1[1] * dx[1];
		a[1] = tri.z1[0] * dx[1] + tri.z1[1] * dx[0];
		b[0] = tri.z2[0] * dy[0] - tri.z2[1] * dy[1];
		b[1] = tri.z2[0] * dy[1] + tri.z2[1] * dy[0];
		r[0] = a[0] - b[0]; r[1] = a[1] - b[1];
		const double coef[3][2] = {
			{-tri.z1[0] + tri.z2[0], -tri.z1[1] + tri.z2[1]},
			{-tri.z2[0], -tri.z2[1]},
			{tri.z1[0], tri.z1[1]}
		};
		for (int i = 0; i < 3; i++) {
			const double cr = coef[i][0], ci = coef[i][1];
			const double gx = tri.weight * 2.0 * (cr * r[0] + ci * r[1]);
			const double gy = tri.weight * 2.0 * (-ci * r[0] + cr * r[1]);
			grad[tri.v[i] * 2 + 0] += gx;
			grad[tri.v[i] * 2 + 1] += gy;
		}
	}
}

static int testSparseLargeGrid()
{
	const uint32_t cells = 40; // 3200 triangles.
	TestMesh mesh = makeGrid(cells);
	TASK_CHECK(mesh.indices.size() / 3 >= 3000);
	xatlas::Atlas *atlas = makeAtlas(mesh);
	TASK_CHECK(atlas != nullptr);
	// Stitch the material boundary vertex by vertex with R90 + integer k.
	std::vector<xatlas::PeriodicRelation> relations;
	const uint32_t mid = cells / 2;
	for (uint32_t y = 0; y <= cells; y++) {
		const uint32_t boundaryVertex = y * (cells + 1) + mid;
		xatlas::PeriodicAngle left, right;
		TASK_CHECK(findCorner(mesh, boundaryVertex, left));
		TASK_CHECK(findCorner(mesh, boundaryVertex, right));
		// The first found corner may be on either side; separate them by face.
		xatlas::PeriodicAngle leftCorner, rightCorner;
		bool haveLeft = false, haveRight = false;
		for (uint32_t face = 0; face < mesh.indices.size() / 3; face++) {
			for (uint32_t c = 0; c < 3; c++) {
				if (mesh.indices[face * 3 + c] != boundaryVertex)
					continue;
				const uint32_t cellX = (face / 2) % cells;
				const bool rightSide = cellX >= mid;
				if (rightSide && !haveRight) {
					rightCorner.meshIndex = 0; rightCorner.faceIndex = face; rightCorner.corner = c;
					haveRight = true;
				} else if (!rightSide && !haveLeft) {
					leftCorner.meshIndex = 0; leftCorner.faceIndex = face; leftCorner.corner = c;
					haveLeft = true;
				}
			}
		}
		TASK_CHECK(haveLeft && haveRight);
		xatlas::PeriodicRelation rel;
		rel.a = leftCorner;
		rel.b = rightCorner;
		rel.q[0] = 0; rel.q[1] = -1; rel.q[2] = 1; rel.q[3] = 0;
		rel.k[0] = (int32_t)y; rel.k[1] = 2;
		relations.push_back(rel);
	}
	xatlas::PeriodicPin pins[2];
	TASK_CHECK(findCorner(mesh, 0, pins[0].angle));
	pins[0].uv[0] = 0.0; pins[0].uv[1] = 0.0;
	TASK_CHECK(findCorner(mesh, cells, pins[1].angle));
	pins[1].uv[0] = 10.0; pins[1].uv[1] = 0.0;
	xatlas::PeriodicResult *result = nullptr;
	const xatlas::PeriodicError error = xatlas::ComputePeriodicUv(atlas, relations.data(), (uint32_t)relations.size(), pins, 2, &result);
	TASK_CHECK(error == xatlas::PeriodicError::Success);
	TASK_CHECK(result->faceCount == mesh.indices.size() / 3);
	TASK_CHECK(maxRelationResidual(result, relations) <= 1e-8);
	TASK_CHECK(maxPinResidual(result, std::vector<xatlas::PeriodicPin>(pins, pins + 2)) <= 1e-8);
	// All UVs finite and the solution uses absolute coordinates outside [0, 1].
	bool outsideUnitSquare = false;
	for (uint32_t face = 0; face < result->faceCount; face++) {
		for (uint32_t c = 0; c < 3; c++) {
			const double *uv = result->meshes[0].angles[face * 3 + c].uv;
			TASK_CHECK(std::isfinite(uv[0]) && std::isfinite(uv[1]));
			outsideUnitSquare = outsideUnitSquare || std::fabs(uv[0]) > 1.0 || std::fabs(uv[1]) > 1.0;
		}
	}
	TASK_CHECK(outsideUnitSquare);
	// Optimality: the full gradient must lie in the row space of the exact
	// constraint rows. Orthonormalize the sparse constraint rows with modified
	// Gram-Schmidt and project the gradient out.
	std::vector<RefTriangle> refTriangles;
	buildRefTriangles(mesh.positions, mesh.indices, result, 0, refTriangles);
	std::vector<double> solved(result->unknownCount * 2, 0.0);
	for (uint32_t face = 0; face < result->faceCount; face++)
		for (uint32_t c = 0; c < 3; c++) {
			const int unknown = result->meshes[0].angles[face * 3 + c].unknownIndex;
			solved[unknown * 2] = result->meshes[0].angles[face * 3 + c].uv[0];
			solved[unknown * 2 + 1] = result->meshes[0].angles[face * 3 + c].uv[1];
		}
	std::vector<double> grad;
	sparseGradient((int)result->unknownCount, refTriangles, solved, grad);
	std::vector<std::vector<double> > basis;
	for (size_t r = 0; r < relations.size(); r++) {
		for (int row = 0; row < 2; row++) {
			std::vector<double> vec(grad.size(), 0.0);
			const xatlas::PeriodicRelation &rel = relations[r];
			const int va = result->meshes[0].angles[rel.a.faceIndex * 3 + rel.a.corner].unknownIndex;
			const int vb = result->meshes[0].angles[rel.b.faceIndex * 3 + rel.b.corner].unknownIndex;
			vec[vb * 2 + row] += 1.0;
			vec[va * 2 + 0] += -(double)rel.q[row * 2 + 0];
			vec[va * 2 + 1] += -(double)rel.q[row * 2 + 1];
			for (size_t q = 0; q < basis.size(); q++) {
				double dot = 0.0;
				for (size_t i = 0; i < vec.size(); i++)
					dot += vec[i] * basis[q][i];
				for (size_t i = 0; i < vec.size(); i++)
					vec[i] -= dot * basis[q][i];
			}
			double norm = 0.0;
			for (size_t i = 0; i < vec.size(); i++)
				norm += vec[i] * vec[i];
			norm = std::sqrt(norm);
			if (norm > 1e-12) {
				for (size_t i = 0; i < vec.size(); i++)
					vec[i] /= norm;
				basis.push_back(vec);
			}
		}
	}
	for (int p = 0; p < 2; p++) {
		std::vector<double> vec(grad.size(), 0.0);
		const int var = result->meshes[0].angles[pins[p].angle.faceIndex * 3 + pins[p].angle.corner].unknownIndex;
		vec[var * 2 + p] = 1.0;
		for (size_t q = 0; q < basis.size(); q++) {
			double dot = 0.0;
			for (size_t i = 0; i < vec.size(); i++)
				dot += vec[i] * basis[q][i];
			for (size_t i = 0; i < vec.size(); i++)
				vec[i] -= dot * basis[q][i];
		}
		double norm = 0.0;
		for (size_t i = 0; i < vec.size(); i++)
			norm += vec[i] * vec[i];
		norm = std::sqrt(norm);
		if (norm > 1e-12) {
			for (size_t i = 0; i < vec.size(); i++)
				vec[i] /= norm;
			basis.push_back(vec);
		}
	}
	std::vector<double> projected = grad;
	for (size_t q = 0; q < basis.size(); q++) {
		double dot = 0.0;
		for (size_t i = 0; i < grad.size(); i++)
			dot += grad[i] * basis[q][i];
		for (size_t i = 0; i < grad.size(); i++)
			projected[i] -= dot * basis[q][i];
	}
	double projectedNorm = 0.0, gradNorm = 0.0;
	for (size_t i = 0; i < grad.size(); i++) {
		projectedNorm += projected[i] * projected[i];
		gradNorm += grad[i] * grad[i];
	}
	TASK_CHECK(std::sqrt(projectedNorm) <= 1e-7 * std::max(1.0, std::sqrt(gradNorm)));
	xatlas::DestroyPeriodicResult(result);
	xatlas::Destroy(atlas);
	return 0;
}

static int testABAResolve()
{
	TestMesh mesh = makeFoldedBand();
	xatlas::Atlas *atlas = makeAtlas(mesh);
	TASK_CHECK(atlas != nullptr);
	xatlas::PeriodicPin pinsA[1], pinsB[1];
	TASK_CHECK(findCorner(mesh, 0, pinsA[0].angle));
	pinsA[0].uv[0] = 1.0; pinsA[0].uv[1] = 1.0;
	pinsB[0] = pinsA[0];
	pinsB[0].uv[0] = -4.0; pinsB[0].uv[1] = 7.0;
	xatlas::PeriodicResult *a1 = nullptr, *b = nullptr, *a2 = nullptr;
	TASK_CHECK(xatlas::ComputePeriodicUv(atlas, nullptr, 0, pinsA, 1, &a1) == xatlas::PeriodicError::Success);
	TASK_CHECK(xatlas::ComputePeriodicUv(atlas, nullptr, 0, pinsB, 1, &b) == xatlas::PeriodicError::Success);
	TASK_CHECK(xatlas::ComputePeriodicUv(atlas, nullptr, 0, pinsA, 1, &a2) == xatlas::PeriodicError::Success);
	const xatlas::PeriodicAngleResult *x1 = a1->meshes[0].angles;
	const xatlas::PeriodicAngleResult *xb = b->meshes[0].angles;
	const xatlas::PeriodicAngleResult *x2 = a2->meshes[0].angles;
	bool bDiffers = false;
	for (uint32_t i = 0; i < a1->faceCount * 3; i++) {
		TASK_CHECK(x1[i].unknownIndex == x2[i].unknownIndex);
		TASK_CHECK(std::fabs(x1[i].uv[0] - x2[i].uv[0]) <= 1e-12 && std::fabs(x1[i].uv[1] - x2[i].uv[1]) <= 1e-12);
		bDiffers = bDiffers || std::fabs(x1[i].uv[0] - xb[i].uv[0]) > 1e-9 || std::fabs(x1[i].uv[1] - xb[i].uv[1]) > 1e-9;
	}
	TASK_CHECK(bDiffers);
	xatlas::DestroyPeriodicResult(a1);
	xatlas::DestroyPeriodicResult(b);
	xatlas::DestroyPeriodicResult(a2);
	xatlas::Destroy(atlas);
	return 0;
}

static int testInputReleaseAndAtlasLifetime()
{
	TestMesh mesh = makeFoldedBand();
	xatlas::Atlas *atlas = makeAtlas(mesh);
	TASK_CHECK(atlas != nullptr);
	xatlas::PeriodicRelation *relations = new xatlas::PeriodicRelation[1];
	relations[0].a.meshIndex = 0; relations[0].a.faceIndex = 0; relations[0].a.corner = 2;
	relations[0].b.meshIndex = 0; relations[0].b.faceIndex = 2; relations[0].b.corner = 1;
	relations[0].q[0] = 0; relations[0].q[1] = -1; relations[0].q[2] = 1; relations[0].q[3] = 0;
	relations[0].k[0] = 3; relations[0].k[1] = 1;
	xatlas::PeriodicPin *pins = new xatlas::PeriodicPin[2];
	TASK_CHECK(findCorner(mesh, 0, pins[0].angle));
	pins[0].uv[0] = 2.0; pins[0].uv[1] = 2.0;
	pins[1] = pins[0];
	TASK_CHECK(findCorner(mesh, 4, pins[1].angle));
	pins[1].uv[0] = 6.0; pins[1].uv[1] = -1.0;
	xatlas::PeriodicResult *result = nullptr;
	TASK_CHECK(xatlas::ComputePeriodicUv(atlas, relations, 1, pins, 2, &result) == xatlas::PeriodicError::Success);
	delete[] relations;
	delete[] pins;
	// Freed inputs must not affect the already produced result.
	const xatlas::PeriodicAngleResult *angles = result->meshes[0].angles;
	const double savedUv = angles[0].uv[0];
	TASK_CHECK(std::isfinite(savedUv));
	const uint32_t savedFaceCount = result->faceCount;
	xatlas::Destroy(atlas);
	// The result stays usable after the atlas is gone.
	TASK_CHECK(result->faceCount == savedFaceCount);
	TASK_CHECK(angles[0].unknownIndex >= 0);
	TASK_CHECK(std::fabs(angles[0].uv[0] - savedUv) <= 1e-15);
	xatlas::DestroyPeriodicResult(result);
	return 0;
}

static int testFailureKeepsPackedAtlas()
{
	TestMesh mesh = makeCurvedPatch();
	xatlas::Atlas *atlas = makeAtlas(mesh);
	TASK_CHECK(atlas != nullptr);
	xatlas::PackOptions options;
	options.resolution = 64;
	options.padding = 2;
	xatlas::PackCharts(atlas, options);
	TASK_CHECK(atlas->meshes != nullptr && atlas->meshes[0].vertexArray != nullptr);
	const uint32_t savedWidth = atlas->width, savedHeight = atlas->height;
	const uint32_t savedVertexCount = atlas->meshes[0].vertexCount;
	std::vector<float> savedUv(savedVertexCount * 2);
	for (uint32_t i = 0; i < savedVertexCount; i++) {
		savedUv[i * 2] = atlas->meshes[0].vertexArray[i].uv[0];
		savedUv[i * 2 + 1] = atlas->meshes[0].vertexArray[i].uv[1];
	}
	// A successful periodic result first: two pins anchor the curved patch
	// enough for a unique minimum.
	xatlas::PeriodicPin goodPins[2];
	TASK_CHECK(findCorner(mesh, 0, goodPins[0].angle));
	goodPins[0].uv[0] = 0.0; goodPins[0].uv[1] = 0.0;
	TASK_CHECK(findCorner(mesh, 1, goodPins[1].angle));
	goodPins[1].uv[0] = 2.0; goodPins[1].uv[1] = 0.0;
	xatlas::PeriodicResult *goodResult = nullptr;
	TASK_CHECK(xatlas::ComputePeriodicUv(atlas, nullptr, 0, goodPins, 2, &goodResult) == xatlas::PeriodicError::Success);
	const double goodUv = goodResult->meshes[0].angles[0].uv[1];
	// Then an incompatible identity translation loop across the material
	// boundary (base vertex v2, two chart corners).
	xatlas::PeriodicRelation bad[2];
	bad[0].a.meshIndex = 0; bad[0].a.faceIndex = 1; bad[0].a.corner = 1;
	bad[0].b.meshIndex = 0; bad[0].b.faceIndex = 2; bad[0].b.corner = 0;
	bad[0].q[0] = 1; bad[0].q[1] = 0; bad[0].q[2] = 0; bad[0].q[3] = 1;
	bad[0].k[0] = 1; bad[0].k[1] = 0;
	bad[1].a = bad[0].b;
	bad[1].b = bad[0].a;
	bad[1].q[0] = 1; bad[1].q[1] = 0; bad[1].q[2] = 0; bad[1].q[3] = 1;
	bad[1].k[0] = 1; bad[1].k[1] = 0;
	xatlas::PeriodicResult *failedResult = goodResult;
	TASK_CHECK(xatlas::ComputePeriodicUv(atlas, bad, 2, nullptr, 0, &failedResult) == xatlas::PeriodicError::InconsistentRelation);
	// The failed call produced nothing and changed nothing.
	TASK_CHECK(failedResult == goodResult);
	TASK_CHECK(atlas->width == savedWidth && atlas->height == savedHeight);
	TASK_CHECK(atlas->meshes[0].vertexCount == savedVertexCount);
	for (uint32_t i = 0; i < savedVertexCount; i++) {
		TASK_CHECK(atlas->meshes[0].vertexArray[i].uv[0] == savedUv[i * 2]);
		TASK_CHECK(atlas->meshes[0].vertexArray[i].uv[1] == savedUv[i * 2 + 1]);
	}
	TASK_CHECK(std::fabs(goodResult->meshes[0].angles[0].uv[1] - goodUv) <= 1e-15);
	xatlas::DestroyPeriodicResult(goodResult);
	xatlas::Destroy(atlas);
	return 0;
}

// Relations may span separate AddMesh calls; corners are located through
// meshIndex as well as face/corner.
static int testMultiMeshRelation()
{
	const float positions0[] = {0, 0, 0, 1, 0, 0, 0, 1, 0};
	const unsigned indices0[] = {0, 1, 2};
	const float positions1[] = {2, 0, 0, 2, 1, 0, 2, 0, 1};
	const unsigned indices1[] = {0, 1, 2};
	xatlas::MeshDecl decl0;
	decl0.vertexPositionData = positions0;
	decl0.vertexCount = 3;
	decl0.vertexPositionStride = sizeof(float) * 3;
	decl0.indexData = indices0;
	decl0.indexCount = 3;
	decl0.indexFormat = xatlas::IndexFormat::UInt32;
	xatlas::MeshDecl decl1 = decl0;
	decl1.vertexPositionData = positions1;
	decl1.indexData = indices1;
	xatlas::Atlas *atlas = xatlas::Create();
	TASK_CHECK(atlas != nullptr);
	TASK_CHECK(xatlas::AddMesh(atlas, decl0) == xatlas::AddMeshError::Success);
	TASK_CHECK(xatlas::AddMesh(atlas, decl1) == xatlas::AddMeshError::Success);
	xatlas::ComputeCharts(atlas);
	// Two R90 + integer translation relations pin the whole second frame.
	xatlas::PeriodicRelation relations[2];
	relations[0].a.meshIndex = 0; relations[0].a.faceIndex = 0; relations[0].a.corner = 0;
	relations[0].b.meshIndex = 1; relations[0].b.faceIndex = 0; relations[0].b.corner = 0;
	relations[0].q[0] = 0; relations[0].q[1] = -1; relations[0].q[2] = 1; relations[0].q[3] = 0;
	relations[0].k[0] = 2; relations[0].k[1] = 3;
	relations[1].a.meshIndex = 0; relations[1].a.faceIndex = 0; relations[1].a.corner = 1;
	relations[1].b.meshIndex = 1; relations[1].b.faceIndex = 0; relations[1].b.corner = 1;
	relations[1].q[0] = 0; relations[1].q[1] = -1; relations[1].q[2] = 1; relations[1].q[3] = 0;
	relations[1].k[0] = 2; relations[1].k[1] = 3;
	xatlas::PeriodicPin pins[2];
	pins[0].angle.meshIndex = 0; pins[0].angle.faceIndex = 0; pins[0].angle.corner = 0;
	pins[0].uv[0] = 0.0; pins[0].uv[1] = 0.0;
	pins[1].angle.meshIndex = 0; pins[1].angle.faceIndex = 0; pins[1].angle.corner = 1;
	pins[1].uv[0] = 1.0; pins[1].uv[1] = 0.0;
	xatlas::PeriodicResult *result = nullptr;
	TASK_CHECK(xatlas::ComputePeriodicUv(atlas, relations, 2, pins, 2, &result) == xatlas::PeriodicError::Success);
	TASK_CHECK(result->meshCount == 2);
	TASK_CHECK(result->meshes[0].faceCount == 1 && result->meshes[1].faceCount == 1);
	TASK_CHECK(maxRelationResidual(result, std::vector<xatlas::PeriodicRelation>(relations, relations + 2)) <= 1e-8);
	// The second mesh corners follow the directed R90 relation exactly.
	const double *ua = result->meshes[0].angles[0].uv;
	const double *ub = result->meshes[1].angles[0].uv;
	TASK_CHECK(std::fabs(ub[0] - (-ua[1] + 2.0)) <= 1e-8);
	TASK_CHECK(std::fabs(ub[1] - (ua[0] + 3.0)) <= 1e-8);
	// A corner on the missing third mesh is out of range.
	xatlas::PeriodicRelation bad = relations[0];
	bad.b.meshIndex = 7;
	TASK_CHECK(xatlas::ComputePeriodicUv(atlas, &bad, 1, pins, 2, &result) == xatlas::PeriodicError::InvalidAngle);
	xatlas::DestroyPeriodicResult(result);
	xatlas::Destroy(atlas);
	return 0;
}

} // namespace

const struct task_case task_cases[] = {
	{"mapping", "material_split_charts", testMaterialSplitCharts},
	{"mapping", "colocal_identity", testColocalIdentity},
	{"mapping", "angle_and_input_validation", testAngleAndInputValidation},
	{"mapping", "geometry_and_state_validation", testGeometryAndStateValidation},
	{"constraints", "two_chart_rotation_translation", testTwoChartRotationTranslation},
	{"constraints", "reflection_loop_line_freedom", testReflectionLoopLineFreedom},
	{"constraints", "rotation_loop_fixed_point", testRotationLoopFixedPoint},
	{"constraints", "inconsistent_translation_loop", testInconsistentTranslationLoop},
	{"constraints", "conflicting_pins", testConflictingPins},
	{"solve", "dense_least_squares_small", testDenseLeastSquaresSmall},
	{"solve", "sparse_large_grid", testSparseLargeGrid},
	{"solve", "aba_resolve", testABAResolve},
	{"solve", "input_release_and_atlas_lifetime", testInputReleaseAndAtlasLifetime},
	{"solve", "multi_mesh_relation", testMultiMeshRelation},
	{"solve", "failure_keeps_packed_atlas", testFailureKeepsPackedAtlas}
};
const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
