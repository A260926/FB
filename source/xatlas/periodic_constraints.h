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

// Periodic (integer lattice seam) parameterization.
//
// This header contains the public entry point types for computing a single
// least squares UV solution shared by every chart of an atlas, while
// enforcing exact face-corner relations U_b = Q * U_a + k with signed
// permutation matrices Q and integer translations k.
//
// The generic constraint elimination / reduced sparse normal equation engine
// lives in namespace xatlas::internal::periodic and has no dependency on the
// atlas internals; the production entry point ComputePeriodicUv is declared
// alongside the rest of the public API in xatlas.h and implemented in
// xatlas.cpp.

#pragma once
#ifndef XATLAS_PERIODIC_CONSTRAINTS_H
#define XATLAS_PERIODIC_CONSTRAINTS_H

#include <stdint.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <unordered_map>
#include <vector>

namespace xatlas {

struct Atlas;

// A face corner: meshIndex selects an AddMesh call, faceIndex is the original
// triangle number within that mesh and corner is 0, 1 or 2.
struct PeriodicAngle
{
	uint32_t meshIndex = 0;
	uint32_t faceIndex = 0;
	uint32_t corner = 0;
};

// Exact relation between two face corners: U_b = Q * U_a + k.
// Q is a 2x2 signed permutation matrix (row major), i.e. quarter turn
// rotations and reflections. k is a signed integer 2D translation.
// Relations are applied in the given coordinates exactly; no integer
// translation is searched for automatically.
struct PeriodicRelation
{
	PeriodicAngle a;
	PeriodicAngle b;
	int32_t q[4] = {1, 0, 0, 1};
	int32_t k[2] = {0, 0};
};

// A face corner pinned to an absolute, fixed UV value.
struct PeriodicPin
{
	PeriodicAngle angle;
	double uv[2] = {0.0, 0.0};
};

enum class PeriodicError
{
	Success,
	InvalidState, // Null atlas, no charts computed, UV meshes / n-gons / ignored faces present.
	InvalidAngle, // A relation or pin references a mesh, face or corner that doesn't exist.
	InvalidTransform, // Q is not a signed permutation matrix or an input value is invalid.
	NonFiniteGeometry, // Input positions or pin values are not finite.
	DegenerateGeometry, // A triangle has zero area or a zero length edge.
	InconsistentRelation, // A closed loop of relations cannot be satisfied.
	ConflictingPin, // Pins constrain the same parameter unknown to different values.
	RankDeficient, // The constrained least squares problem is not uniquely determined.
	SolveFailed // Numerical solve failed or residual tolerance was not met.
};

const char *StringForEnum(PeriodicError error);

// Per face corner output.
struct PeriodicAngleResult
{
	int32_t unknownIndex; // Parameter unknown index, shared by corners unified within a chart. -1 if invalid.
	int32_t chartIndex; // Global chart index the corner was parameterized through. -1 if invalid.
	double uv[2]; // Absolute double precision UV, may be outside [0, 1].
};

struct PeriodicMeshResult
{
	uint32_t meshIndex;
	uint32_t faceCount;
	PeriodicAngleResult *angles; // faceCount * 3 entries, indexed faceIndex * 3 + corner.
};

// Owns the periodic parameterization output. Allocated independently of the
// atlas, so it remains valid after the atlas is destroyed. Free with
// DestroyPeriodicResult.
struct PeriodicResult
{
	uint32_t meshCount;
	uint32_t faceCount; // Total number of triangles across all meshes.
	uint32_t unknownCount; // Number of distinct parameter unknowns.
	uint32_t freeDimension; // 0, 1 or 2: dimension of the constraint null space (0/1/2D free subspaces).
	double relativeResidual; // Relative normal equation residual of the reduced free-variable system.
	PeriodicMeshResult *meshes; // meshCount entries.
};

// Compute a periodic parameterization. The atlas must contain only triangle
// meshes added with AddMesh (no AddUvMesh, no ignored faces, no n-gons) and
// ComputeCharts must have been called. The relation and pin arrays are only
// read during the call and may be freed afterwards. On failure *resultOut is
// left untouched and any previously packed atlas / periodic result is
// unchanged. On success a new PeriodicResult is allocated and assigned to
// *resultOut. Repeated calls always re-solve from the original geometry and
// constraints.
PeriodicError ComputePeriodicUv(Atlas *atlas, const PeriodicRelation *relations, uint32_t relationCount, const PeriodicPin *pins, uint32_t pinCount, PeriodicResult **resultOut);

void DestroyPeriodicResult(PeriodicResult *result);

namespace internal {
namespace periodic {

// Generic engine input: everything is expressed in parameter unknown indices,
// each unknown is a 2D double vector.
struct EngineRelation
{
	int32_t varA;
	int32_t varB;
	double q[4]; // Row major signed permutation.
	double k[2];
};

struct EnginePin
{
	int32_t var;
	double uv[2];
};

// Per triangle least squares term. Local complex coordinates use z0 = 0,
// z1 on the real axis and z2 in the orthonormal face basis. weight is
// 1 / (2 * 3D triangle area).
struct EngineTriangle
{
	int32_t v[3];
	double z1[2]; // Complex z1, imaginary part normally 0.
	double z2[2]; // Complex z2.
	double weight;
};

// Runs the full constraint elimination and reduced sparse least squares
// solve. uvOut must hold unknownCount * 2 doubles. Constraints are satisfied
// exactly (to numerical tolerance); the free variables of any compatible
// closed loop null space take the minimum norm value. Returns a PeriodicError
// on inconsistent constraints, conflicting pins or remaining rank deficiency.
// freeDimensionOut receives 0, 1 or 2 and relativeResidualOut the relative
// normal equation residual of the reduced system.
PeriodicError solve(uint32_t unknownCount, const EngineRelation *relations, uint32_t relationCount, const EnginePin *pins, uint32_t pinCount, const EngineTriangle *triangles, uint32_t triangleCount, double *uvOut, uint32_t *freeDimensionOut, double *relativeResidualOut);

namespace {

static const double kConstraintTolerance = 1e-8;
static const double kSolveTolerance = 1e-12;

struct Mat2
{
	double m[4]; // Row major.
};

static Mat2 identity()
{
	Mat2 r = {{1.0, 0.0, 0.0, 1.0}};
	return r;
}

static Mat2 multiply(const Mat2 &a, const Mat2 &b)
{
	Mat2 r;
	for (int i = 0; i < 2; i++)
		for (int j = 0; j < 2; j++)
			r.m[i * 2 + j] = a.m[i * 2 + 0] * b.m[0 * 2 + j] + a.m[i * 2 + 1] * b.m[1 * 2 + j];
	return r;
}

static Mat2 invert(const Mat2 &a)
{
	const double det = a.m[0] * a.m[3] - a.m[1] * a.m[2];
	Mat2 r;
	const double invDet = 1.0 / det;
	r.m[0] = a.m[3] * invDet;
	r.m[1] = -a.m[1] * invDet;
	r.m[2] = -a.m[2] * invDet;
	r.m[3] = a.m[0] * invDet;
	return r;
}

static void multiplyVec(const Mat2 &a, const double v[2], double out[2])
{
	const double x = a.m[0] * v[0] + a.m[1] * v[1];
	const double y = a.m[2] * v[0] + a.m[3] * v[1];
	out[0] = x;
	out[1] = y;
}

static bool isFinite(double v)
{
	return std::isfinite(v);
}

// Signed permutation matrices have exactly one nonzero entry (+/-1) per row
// and column.
static bool isSignedPermutation(const double q[4])
{
	for (int i = 0; i < 4; i++)
		if (!isFinite(q[i]))
			return false;
	int rowNonZero[2] = {0, 0}, colNonZero[2] = {0, 0};
	for (int i = 0; i < 2; i++) {
		for (int j = 0; j < 2; j++) {
			const double v = q[i * 2 + j];
			if (v != 0.0) {
				if (std::fabs(v) != 1.0)
					return false;
				rowNonZero[i]++;
				colNonZero[j]++;
			}
		}
	}
	return rowNonZero[0] == 1 && rowNonZero[1] == 1 && colNonZero[0] == 1 && colNonZero[1] == 1;
}

struct LoopConstraint
{
	uint32_t root;
	Mat2 q;
	double k[2];
};

struct ScalarConstraint
{
	double normal[2];
	double rhs;
};

// Per component representative frame. w = F^T * U_rep, w0 holds the fixed
// particular coordinates (0 for free axes), freeAxis lists independent
// reduced unknowns.
struct RootFrame
{
	Mat2 frame; // Columns are orthonormal frame axes.
	double w0[2];
	int freeAxis[2];
	uint32_t freeCount;
};

struct BasisEntry
{
	int32_t col;
	double g[2];
};

// Complex arithmetic on plain 2D vectors: x is the real part, y the
// imaginary part.
static void complexMul(const double a[2], const double b[2], double out[2])
{
	const double x = a[0] * b[0] - a[1] * b[1];
	const double y = a[0] * b[1] + a[1] * b[0];
	out[0] = x;
	out[1] = y;
}

// Solve a 2x2 system. Returns false if singular.
static bool solve2x2(const Mat2 &a, const double b[2], double x[2])
{
	const double det = a.m[0] * a.m[3] - a.m[1] * a.m[2];
	if (std::fabs(det) < 1e-30)
		return false;
	x[0] = (b[0] * a.m[3] - a.m[1] * b[1]) / det;
	x[1] = (a.m[0] * b[1] - b[0] * a.m[2]) / det;
	return true;
}

struct SparseMatrix
{
	std::vector<int> rowPtr;
	std::vector<int> colIdx;
	std::vector<double> values;

	void multiply(const std::vector<double> &x, std::vector<double> &out) const
	{
		const size_t n = rowPtr.size() - 1;
		for (size_t row = 0; row < n; row++) {
			double sum = 0.0;
			for (int e = rowPtr[row]; e < rowPtr[row + 1]; e++)
				sum += values[e] * x[colIdx[e]];
			out[row] = sum;
		}
	}
};

// Jacobi preconditioned conjugate gradient for an SPD system. Returns the
// final relative residual.
static double conjugateGradient(const SparseMatrix &h, const std::vector<double> &rhs, std::vector<double> &x, uint32_t maxIterations)
{
	const size_t n = rhs.size();
	x.assign(n, 0.0);
	std::vector<double> diag(n, 0.0), r(n), p(n), ap(n), z(n);
	for (size_t row = 0; row < n; row++) {
		for (int e = h.rowPtr[row]; e < h.rowPtr[row + 1]; e++) {
			if (h.colIdx[e] == (int)row) {
				diag[row] = h.values[e];
				break;
			}
		}
	}
	double rhsNorm2 = 0.0;
	for (size_t i = 0; i < n; i++)
		rhsNorm2 += rhs[i] * rhs[i];
	h.multiply(x, r);
	for (size_t i = 0; i < n; i++)
		r[i] = rhs[i] - r[i];
	double residualScale = std::max(std::sqrt(rhsNorm2), 1e-30);
	for (size_t i = 0; i < n; i++)
		z[i] = r[i] / diag[i];
	p = z;
	double rz = 0.0;
	for (size_t i = 0; i < n; i++)
		rz += r[i] * z[i];
	for (uint32_t iteration = 0; iteration < maxIterations; iteration++) {
		h.multiply(p, ap);
		double pap = 0.0;
		for (size_t i = 0; i < n; i++)
			pap += p[i] * ap[i];
		if (pap <= 0.0 || !std::isfinite(pap))
			return std::numeric_limits<double>::infinity();
		const double alpha = rz / pap;
		for (size_t i = 0; i < n; i++) {
			x[i] += alpha * p[i];
			r[i] -= alpha * ap[i];
		}
		double residualNorm2 = 0.0;
		for (size_t i = 0; i < n; i++)
			residualNorm2 += r[i] * r[i];
		if (std::sqrt(residualNorm2) / residualScale <= kSolveTolerance)
			break;
		for (size_t i = 0; i < n; i++)
			z[i] = r[i] / diag[i];
		double rzNew = 0.0;
		for (size_t i = 0; i < n; i++)
			rzNew += r[i] * z[i];
		const double beta = rzNew / rz;
		for (size_t i = 0; i < n; i++)
			p[i] = z[i] + beta * p[i];
		rz = rzNew;
	}
	std::vector<double> check(n, 0.0);
	h.multiply(x, check);
	double residual2 = 0.0;
	for (size_t i = 0; i < n; i++) {
		const double d = rhs[i] - check[i];
		residual2 += d * d;
	}
	return std::sqrt(residual2) / residualScale;
}

} // namespace

inline PeriodicError solve(uint32_t unknownCount, const EngineRelation *relations, uint32_t relationCount, const EnginePin *pins, uint32_t pinCount, const EngineTriangle *triangles, uint32_t triangleCount, double *uvOut, uint32_t *freeDimensionOut, double *relativeResidualOut)
{
	if (unknownCount == 0)
		return PeriodicError::RankDeficient;
	const uint32_t n = unknownCount;
	// U_var = transform[var] * U_rep + offset[var].
	std::vector<Mat2> transform(n);
	std::vector<std::vector<double> > offset(n);
	std::vector<uint32_t> rep(n), groupSize(n);
	std::vector<std::vector<uint32_t> > members(n);
	for (uint32_t i = 0; i < n; i++) {
		transform[i] = identity();
		offset[i].assign(2, 0.0);
		rep[i] = i;
		groupSize[i] = 1;
		members[i].push_back(i);
	}
	std::vector<LoopConstraint> loops;
	// Eliminate relations.
	for (uint32_t r = 0; r < relationCount; r++) {
		const EngineRelation &rel = relations[r];
		if (rel.varA < 0 || rel.varB < 0 || (uint32_t)rel.varA >= n || (uint32_t)rel.varB >= n)
			return PeriodicError::InvalidAngle;
		if (!isSignedPermutation(rel.q) || !isFinite(rel.k[0]) || !isFinite(rel.k[1]))
			return PeriodicError::InvalidTransform;
		Mat2 qr;
		std::memcpy(qr.m, rel.q, sizeof(qr.m));
		const uint32_t a = (uint32_t)rel.varA, b = (uint32_t)rel.varB;
		const uint32_t ra = rep[a], rb = rep[b];
		// U_rb = qb^-1 * (qr * (qa * U_ra + ka) + kr - kb)
		Mat2 qqa = multiply(qr, transform[a]);
		double qka[2];
		multiplyVec(qr, &offset[a][0], qka);
		const double rhsInner[2] = {qka[0] + rel.k[0] - offset[b][0], qka[1] + rel.k[1] - offset[b][1]};
		Mat2 qbInv = invert(transform[b]);
		Mat2 qc = multiply(qbInv, qqa);
		double kc[2];
		multiplyVec(qbInv, rhsInner, kc);
		if (ra == rb) {
			// Closed loop: U_r = qc * U_r + kc.
			LoopConstraint loop;
			loop.root = ra;
			loop.q = qc;
			loop.k[0] = kc[0];
			loop.k[1] = kc[1];
			loops.push_back(loop);
			continue;
		}
		// Union by size, keeping the explicit relation direction correct.
		uint32_t keepRoot, attachRoot;
		Mat2 attachQ;
		double attachK[2];
		if (groupSize[ra] >= groupSize[rb]) {
			keepRoot = ra;
			attachRoot = rb;
			attachQ = qc; // U_rb = qc * U_ra + kc
			attachK[0] = kc[0];
			attachK[1] = kc[1];
		} else {
			// Invert: U_ra = qc^-1 * (U_rb - kc).
			keepRoot = rb;
			attachRoot = ra;
			attachQ = invert(qc);
			double negKc[2] = {-kc[0], -kc[1]};
			multiplyVec(attachQ, negKc, attachK);
		}
		std::vector<uint32_t> &attachMembers = members[attachRoot];
		for (size_t i = 0; i < attachMembers.size(); i++) {
			const uint32_t v = attachMembers[i];
			// U_v = qv * U_attach + kv = qv * (attachQ * U_keep + attachK) + kv
			double qvK[2];
			multiplyVec(transform[v], attachK, qvK);
			offset[v][0] = qvK[0] + offset[v][0];
			offset[v][1] = qvK[1] + offset[v][1];
			transform[v] = multiply(transform[v], attachQ);
			rep[v] = keepRoot;
			members[keepRoot].push_back(v);
		}
		groupSize[keepRoot] += groupSize[attachRoot];
		groupSize[attachRoot] = 0;
		std::vector<uint32_t>().swap(attachMembers);
	}
	// Validate pins up front and collect per-root scalar constraints.
	std::vector<std::vector<ScalarConstraint> > rootConstraints(n);
	for (uint32_t p = 0; p < pinCount; p++) {
		const EnginePin &pin = pins[p];
		if (pin.var < 0 || (uint32_t)pin.var >= n)
			return PeriodicError::InvalidAngle;
		if (!isFinite(pin.uv[0]) || !isFinite(pin.uv[1]))
			return PeriodicError::NonFiniteGeometry;
		const uint32_t v = (uint32_t)pin.var;
		const uint32_t root = rep[v];
		// U_rep = qv^-1 * (uv - kv).
		Mat2 qInv = invert(transform[v]);
		const double rel[2] = {pin.uv[0] - offset[v][0], pin.uv[1] - offset[v][1]};
		double repUv[2];
		multiplyVec(qInv, rel, repUv);
		ScalarConstraint cx = {{1.0, 0.0}, repUv[0]};
		ScalarConstraint cy = {{0.0, 1.0}, repUv[1]};
		rootConstraints[root].push_back(cx);
		rootConstraints[root].push_back(cy);
	}
	// Turn every closed loop into scalar constraints on its representative.
	// Each loop is kept separate: two non-parallel reflection lines, for
	// example, jointly fix the representative to a point.
	for (size_t i = 0; i < loops.size(); i++) {
		const uint32_t root = loops[i].root;
		const Mat2 &qacc = loops[i].q;
		const double (&kacc)[2] = loops[i].k;
		const double trace = qacc.m[0] + qacc.m[3];
		const double det = qacc.m[0] * qacc.m[3] - qacc.m[1] * qacc.m[2];
		const double scale = std::max(1.0, std::fabs(kacc[0]) + std::fabs(kacc[1]));
		if (std::fabs(trace - 2.0) < 1e-12 && std::fabs(det - 1.0) < 1e-12) {
			// Identity loop: only compatible with a zero translation.
			if (std::fabs(kacc[0]) > kConstraintTolerance * scale || std::fabs(kacc[1]) > kConstraintTolerance * scale)
				return PeriodicError::InconsistentRelation;
			continue;
		}
		if (std::fabs(det - 1.0) < 1e-12) {
			// Rotation loop: (I - Q) * U = k has a unique point solution.
			Mat2 iMinusQ = {{1.0 - qacc.m[0], -qacc.m[1], -qacc.m[2], 1.0 - qacc.m[3]}};
			double point[2];
			if (!solve2x2(iMinusQ, kacc, point))
				return PeriodicError::InconsistentRelation;
			rootConstraints[root].push_back(ScalarConstraint{{1.0, 0.0}, point[0]});
			rootConstraints[root].push_back(ScalarConstraint{{0.0, 1.0}, point[1]});
			continue;
		}
		if (std::fabs(det + 1.0) < 1e-12) {
			// Reflection loop: the representative is restricted to a line.
			// Null direction d of (I - Q).
			Mat2 m = {{1.0 - qacc.m[0], -qacc.m[1], -qacc.m[2], 1.0 - qacc.m[3]}};
			double d[2] = {m.m[3], -m.m[2]};
			if (std::fabs(d[0]) + std::fabs(d[1]) < 1e-20) {
				d[0] = -m.m[1];
				d[1] = m.m[0];
			}
			const double dLen = std::sqrt(d[0] * d[0] + d[1] * d[1]);
			d[0] /= dLen;
			d[1] /= dLen;
			// k must be orthogonal to the free line direction.
			if (std::fabs(d[0] * kacc[0] + d[1] * kacc[1]) > kConstraintTolerance * scale)
				return PeriodicError::InconsistentRelation;
			const double normal[2] = {-d[1], d[0]};
			const double point[2] = {0.5 * kacc[0], 0.5 * kacc[1]};
			const double rhs = normal[0] * point[0] + normal[1] * point[1];
			rootConstraints[root].push_back(ScalarConstraint{{normal[0], normal[1]}, rhs});
			continue;
		}
		return PeriodicError::InconsistentRelation;
	}
	// Build an orthonormal frame and particular solution for every root.
	std::vector<RootFrame> frames(n);
	std::vector<uint32_t> reducedIndex(n * 2, UINT32_MAX);
	uint32_t reducedCount = 0;
	uint32_t maxFreeCount = 0;
	for (uint32_t root = 0; root < n; root++) {
		if (rep[root] != root)
			continue;
		RootFrame &frame = frames[root];
		frame.w0[0] = frame.w0[1] = 0.0;
		frame.freeCount = 0;
		frame.frame = identity();
		std::vector<ScalarConstraint> &rows = rootConstraints[root];
		// Gaussian elimination on at most two unit-norm scalar rows.
		uint32_t rank = 0;
		double basisNormal[2] = {0.0, 0.0};
		double basisRhs = 0.0;
		for (size_t i = 0; i < rows.size(); i++) {
			double normal[2] = {rows[i].normal[0], rows[i].normal[1]};
			double rhs = rows[i].rhs;
			const double nLen = std::sqrt(normal[0] * normal[0] + normal[1] * normal[1]);
			if (nLen < 1e-30)
				return PeriodicError::InvalidTransform;
			normal[0] /= nLen;
			normal[1] /= nLen;
			rhs /= nLen;
			if (rank == 0) {
				basisNormal[0] = normal[0];
				basisNormal[1] = normal[1];
				basisRhs = rhs;
				rank = 1;
				continue;
			}
			if (rank == 1) {
				const double cross = basisNormal[0] * normal[1] - basisNormal[1] * normal[0];
				const double parallel = basisNormal[0] * normal[0] + basisNormal[1] * normal[1];
				const double rhsScale = std::max(1.0, std::fabs(rhs) + std::fabs(basisRhs));
				if (std::fabs(cross) < 1e-10) {
					// Parallel normals must agree on the right hand side.
					if (std::fabs((parallel > 0.0 ? rhs - basisRhs : rhs + basisRhs)) > kConstraintTolerance * rhsScale)
						return PeriodicError::ConflictingPin;
					continue;
				}
				// Independent row: solve for the representative directly.
				Mat2 sys = {{basisNormal[0], basisNormal[1], normal[0], normal[1]}};
				double solution[2];
				if (!solve2x2(sys, (double[2]){basisRhs, rhs}, solution))
					return PeriodicError::ConflictingPin;
				frame.w0[0] = solution[0];
				frame.w0[1] = solution[1];
				rank = 2;
				continue;
			}
			// rank == 2: any further constraint only needs a conflict check.
			const double value = normal[0] * frame.w0[0] + normal[1] * frame.w0[1];
			const double rhsScale = std::max(1.0, std::fabs(rhs) + std::fabs(value));
			if (std::fabs(value - rhs) > kConstraintTolerance * rhsScale)
				return PeriodicError::ConflictingPin;
		}
		if (rank == 1) {
			// Frame: x axis is the free direction, y axis the constrained normal.
			const double freeDir[2] = {-basisNormal[1], basisNormal[0]};
			frame.frame.m[0] = freeDir[0];
			frame.frame.m[2] = freeDir[1];
			frame.frame.m[1] = basisNormal[0];
			frame.frame.m[3] = basisNormal[1];
			frame.w0[0] = 0.0;
			frame.w0[1] = basisRhs;
			frame.freeAxis[frame.freeCount++] = 0;
		} else if (rank == 0) {
			frame.freeAxis[frame.freeCount++] = 0;
			frame.freeAxis[frame.freeCount++] = 1;
		}
		for (uint32_t f = 0; f < frame.freeCount; f++) {
			const uint32_t axis = (uint32_t)frame.freeAxis[f];
			reducedIndex[root * 2 + axis] = reducedCount++;
		}
		maxFreeCount = std::max(maxFreeCount, frame.freeCount);
	}
	const uint32_t freeDimension = maxFreeCount;
	// Particular solution b_v and sparse reduced basis columns g_v for every
	// unknown. A variable only depends on the free axes of its own component,
	// so each basis holds at most two entries.
	std::vector<std::vector<double> > particular(n);
	std::vector<std::vector<BasisEntry> > reducedBasis(n);
	for (uint32_t v = 0; v < n; v++) {
		const uint32_t root = rep[v];
		const RootFrame &frame = frames[root];
		double repParticular[2];
		multiplyVec(frame.frame, frame.w0, repParticular);
		double b[2];
		multiplyVec(transform[v], repParticular, b);
		particular[v].resize(2);
		particular[v][0] = b[0] + offset[v][0];
		particular[v][1] = b[1] + offset[v][1];
		for (uint32_t f = 0; f < frame.freeCount; f++) {
			const uint32_t axis = (uint32_t)frame.freeAxis[f];
			const uint32_t col = reducedIndex[root * 2 + axis];
			const double frameColumn[2] = {frame.frame.m[axis], frame.frame.m[2 + axis]};
			BasisEntry entry;
			entry.col = (int32_t)col;
			multiplyVec(transform[v], frameColumn, entry.g);
			reducedBasis[v].push_back(entry);
		}
	}
	// Assemble the reduced sparse normal equations from the triangle terms.
	std::vector<std::unordered_map<int, double> > scatter(reducedCount);
	std::vector<double> rhs(reducedCount, 0.0);
	for (uint32_t t = 0; t < triangleCount; t++) {
		const EngineTriangle &tri = triangles[t];
		for (int c = 0; c < 3; c++) {
			if (tri.v[c] < 0 || (uint32_t)tri.v[c] >= n)
				return PeriodicError::InvalidAngle;
		}
		if (!(tri.weight > 0.0) || !isFinite(tri.z1[0]) || !isFinite(tri.z2[0]) || !isFinite(tri.z2[1]))
			return PeriodicError::DegenerateGeometry;
		// Reduced columns touched by this triangle.
		int localCols[6];
		uint32_t localCount = 0;
		for (int c = 0; c < 3; c++) {
			const uint32_t v = (uint32_t)tri.v[c];
			for (size_t e = 0; e < reducedBasis[v].size(); e++) {
				const int col = reducedBasis[v][e].col;
				bool found = false;
				for (uint32_t j = 0; j < localCount; j++)
					found = found || localCols[j] == col;
				if (!found)
					localCols[localCount++] = col;
			}
		}
		const double *b[3] = {&particular[(uint32_t)tri.v[0]][0], &particular[(uint32_t)tri.v[1]][0], &particular[(uint32_t)tri.v[2]][0]};
		// r0 = z1 * (b2 - b0) - z2 * (b1 - b0).
		double d20[2] = {b[2][0] - b[0][0], b[2][1] - b[0][1]};
		double d10[2] = {b[1][0] - b[0][0], b[1][1] - b[0][1]};
		double term1[2], term2[2], r0[2];
		complexMul(tri.z1, d20, term1);
		complexMul(tri.z2, d10, term2);
		r0[0] = term1[0] - term2[0];
		r0[1] = term1[1] - term2[1];
		// Look up the basis column of a triangle vertex for a local column.
		struct BasisLookup
		{
			const std::vector<BasisEntry> *entries;
			const double *get(int col) const
			{
				static const double zero[2] = {0.0, 0.0};
				for (size_t i = 0; i < entries->size(); i++) {
					if ((*entries)[i].col == col)
						return &(*entries)[i].g[0];
				}
				return zero;
			}
		};
		BasisLookup lookup[3] = {{&reducedBasis[(uint32_t)tri.v[0]]}, {&reducedBasis[(uint32_t)tri.v[1]]}, {&reducedBasis[(uint32_t)tri.v[2]]}};
		std::vector<std::vector<double> > rColumns(localCount, std::vector<double>(2, 0.0));
		for (uint32_t l = 0; l < localCount; l++) {
			const int col = localCols[l];
			const double *g0 = lookup[0].get(col);
			const double *g1 = lookup[1].get(col);
			const double *g2 = lookup[2].get(col);
			double g20[2] = {g2[0] - g0[0], g2[1] - g0[1]};
			double g10[2] = {g1[0] - g0[0], g1[1] - g0[1]};
			double rt1[2], rt2[2];
			complexMul(tri.z1, g20, rt1);
			complexMul(tri.z2, g10, rt2);
			rColumns[l][0] = rt1[0] - rt2[0];
			rColumns[l][1] = rt1[1] - rt2[1];
		}
		const double w = tri.weight;
		for (uint32_t l = 0; l < localCount; l++) {
			for (uint32_t l2 = 0; l2 < localCount; l2++) {
				const double dot = rColumns[l][0] * rColumns[l2][0] + rColumns[l][1] * rColumns[l2][1];
				scatter[localCols[l]][localCols[l2]] += w * dot;
			}
			const double dotR = rColumns[l][0] * r0[0] + rColumns[l][1] * r0[1];
			rhs[localCols[l]] -= w * dotR;
		}
	}
	// Convert to CSR.
	SparseMatrix matrix;
	matrix.rowPtr.resize(reducedCount + 1, 0);
	for (uint32_t row = 0; row < reducedCount; row++) {
		std::vector<std::pair<int, double> > entries(scatter[row].begin(), scatter[row].end());
		std::sort(entries.begin(), entries.end());
		for (size_t e = 0; e < entries.size(); e++) {
			matrix.colIdx.push_back(entries[e].first);
			matrix.values.push_back(entries[e].second);
		}
		matrix.rowPtr[row + 1] = (int)matrix.colIdx.size();
	}
	// Partition exact null columns (compatible loop free axes that coincide
	// with genuine energy null directions, e.g. global translation) from the
	// positive definite active block. Null variables take their minimum norm
	// value of zero.
	double diagonalScale = 1.0;
	for (uint32_t row = 0; row < reducedCount; row++) {
		for (int e = matrix.rowPtr[row]; e < matrix.rowPtr[row + 1]; e++) {
			if (matrix.colIdx[e] == (int)row)
				diagonalScale = std::max(diagonalScale, std::fabs(matrix.values[e]));
		}
	}
	const double nullThreshold = 1e-12 * diagonalScale;
	double rhsScale = 1.0;
	for (size_t i = 0; i < rhs.size(); i++)
		rhsScale = std::max(rhsScale, std::fabs(rhs[i]));
	std::vector<int> activeIndex(reducedCount, -1);
	std::vector<uint32_t> activeColumns;
	for (uint32_t row = 0; row < reducedCount; row++) {
		double diagonal = 0.0;
		double rowNorm = 0.0;
		for (int e = matrix.rowPtr[row]; e < matrix.rowPtr[row + 1]; e++) {
			rowNorm += matrix.values[e] * matrix.values[e];
			if (matrix.colIdx[e] == (int)row)
				diagonal = matrix.values[e];
		}
		if (std::fabs(diagonal) <= nullThreshold) {
			// A null column must have an all-zero row to be a true null
			// direction; otherwise the remaining system is rank deficient.
			if (std::sqrt(rowNorm) > nullThreshold || std::fabs(rhs[row]) > kConstraintTolerance * rhsScale)
				return PeriodicError::RankDeficient;
			continue;
		}
		if (!(diagonal > 0.0))
			return PeriodicError::RankDeficient;
		activeIndex[row] = (int)activeColumns.size();
		activeColumns.push_back(row);
	}
	// If the constraints fixed every coordinate there is nothing to solve.
	std::vector<double> reducedSolution(reducedCount, 0.0);
	double relativeResidual = 0.0;
	if (!activeColumns.empty()) {
		SparseMatrix activeMatrix;
		std::vector<double> activeRhs(activeColumns.size(), 0.0);
		activeMatrix.rowPtr.resize(activeColumns.size() + 1, 0);
		for (size_t ar = 0; ar < activeColumns.size(); ar++) {
			const uint32_t row = activeColumns[ar];
			activeRhs[ar] = rhs[row];
			for (int e = matrix.rowPtr[row]; e < matrix.rowPtr[row + 1]; e++) {
				const int mapped = activeIndex[matrix.colIdx[e]];
				if (mapped >= 0) {
					activeMatrix.colIdx.push_back(mapped);
					activeMatrix.values.push_back(matrix.values[e]);
				}
			}
			activeMatrix.rowPtr[ar + 1] = (int)activeMatrix.colIdx.size();
		}
		std::vector<double> activeSolution;
		const uint32_t maxIterations = std::max((uint32_t)1000, (uint32_t)activeColumns.size() * 10u);
		relativeResidual = conjugateGradient(activeMatrix, activeRhs, activeSolution, maxIterations);
		if (!std::isfinite(relativeResidual))
			return PeriodicError::RankDeficient;
		if (relativeResidual > 1e-8)
			return PeriodicError::SolveFailed;
		for (size_t i = 0; i < activeColumns.size(); i++)
			reducedSolution[activeColumns[i]] = activeSolution[i];
	}
	// Back substitute every unknown.
	for (uint32_t v = 0; v < n; v++) {
		double uv[2] = {particular[v][0], particular[v][1]};
		for (size_t e = 0; e < reducedBasis[v].size(); e++) {
			const int32_t col = reducedBasis[v][e].col;
			uv[0] += reducedBasis[v][e].g[0] * reducedSolution[col];
			uv[1] += reducedBasis[v][e].g[1] * reducedSolution[col];
		}
		if (!isFinite(uv[0]) || !isFinite(uv[1]))
			return PeriodicError::SolveFailed;
		uvOut[v * 2 + 0] = uv[0];
		uvOut[v * 2 + 1] = uv[1];
	}
	// Verify every relation exactly.
	for (uint32_t r = 0; r < relationCount; r++) {
		const EngineRelation &rel = relations[r];
		const double *ua = &uvOut[(uint32_t)rel.varA * 2];
		const double *ub = &uvOut[(uint32_t)rel.varB * 2];
		double expected[2];
		multiplyVec(Mat2{{rel.q[0], rel.q[1], rel.q[2], rel.q[3]}}, ua, expected);
		expected[0] += rel.k[0];
		expected[1] += rel.k[1];
		if (std::fabs(ub[0] - expected[0]) > kConstraintTolerance || std::fabs(ub[1] - expected[1]) > kConstraintTolerance)
			return PeriodicError::SolveFailed;
	}
	// Verify every pin exactly.
	for (uint32_t p = 0; p < pinCount; p++) {
		const EnginePin &pin = pins[p];
		const double *uv = &uvOut[(uint32_t)pin.var * 2];
		if (std::fabs(uv[0] - pin.uv[0]) > kConstraintTolerance || std::fabs(uv[1] - pin.uv[1]) > kConstraintTolerance)
			return PeriodicError::SolveFailed;
	}
	if (freeDimensionOut)
		*freeDimensionOut = freeDimension;
	if (relativeResidualOut)
		*relativeResidualOut = relativeResidual;
	return PeriodicError::Success;
}


} // namespace periodic
} // namespace internal

} // namespace xatlas

#endif // XATLAS_PERIODIC_CONSTRAINTS_H
