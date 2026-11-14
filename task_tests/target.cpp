/*
 * Task tests for the rectangular LSQR LSCM path.
 *
 * This file includes the full xatlas implementation once (no second linked copy) so it can
 * drive the internal OpenNL assembly and the internal LSCM entry point. The small rectangular
 * systems are cross-checked against an independent, self-contained Householder QR solve and
 * independently recomputed residuals/indicators - the normal-equation CG path is never used
 * as the reference.
 */
#include "registry.hpp"
#include "../source/xatlas/xatlas.cpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <utility>
#include <vector>

using namespace xatlas::internal;
using namespace xatlas::internal::opennl;

namespace {

typedef std::vector<std::vector<double>> DenseMat;

static bool finiteDouble(double v)
{
	return std::isfinite(v);
}

static bool finiteFloat(float v)
{
	return std::isfinite(v);
}

/* ------------------------------------------------------------------ */
/* Independent dense reference: Householder QR least squares.         */
/* No normal equations are formed. Rank-deficient pivots yield a      */
/* valid stationary (not necessarily minimum-norm) solution.          */
/* ------------------------------------------------------------------ */
static void householderLeastSquares(const DenseMat &A, const std::vector<double> &rhs, std::vector<double> &x)
{
	const uint32_t m = (uint32_t)A.size();
	const uint32_t n = (uint32_t)A[0].size();
	DenseMat R = A;
	std::vector<double> b = rhs;
	for (uint32_t k = 0; k < std::min(m, n); k++) {
		double norm = 0.0;
		for (uint32_t i = k; i < m; i++)
			norm += R[i][k] * R[i][k];
		norm = sqrt(norm);
		if (norm == 0.0)
			continue;
		std::vector<double> v(m - k, 0.0);
		for (uint32_t i = k; i < m; i++)
			v[i - k] = R[i][k];
		const double sigma = (v[0] >= 0.0 ? norm : -norm);
		v[0] += sigma;
		double vtv = 0.0;
		for (double vi : v)
			vtv += vi * vi;
		if (vtv == 0.0)
			continue;
		const double beta = 2.0 / vtv;
		for (uint32_t j = k; j < n; j++) {
			double dot = 0.0;
			for (uint32_t i = k; i < m; i++)
				dot += v[i - k] * R[i][j];
			const double s = beta * dot;
			for (uint32_t i = k; i < m; i++)
				R[i][j] -= s * v[i - k];
		}
		double dotb = 0.0;
		for (uint32_t i = k; i < m; i++)
			dotb += v[i - k] * b[i];
		const double sb = beta * dotb;
		for (uint32_t i = k; i < m; i++)
			b[i] -= sb * v[i - k];
	}
	x.assign(n, 0.0);
	double maxPivot = 0.0;
	for (uint32_t k = 0; k < std::min(m, n); k++)
		maxPivot = std::max(maxPivot, fabs(R[k][k]));
	const double pivotEps = (maxPivot > 0.0 ? maxPivot : 1.0) * 1e-12;
	for (int k = (int)std::min(m, n) - 1; k >= 0; k--) {
		if (fabs(R[k][k]) <= pivotEps) {
			x[k] = 0.0; // Rank-deficient column: any stationary value is acceptable, choose zero.
			continue;
		}
		double sum = b[k];
		for (uint32_t j = (uint32_t)k + 1; j < n; j++)
			sum -= R[k][j] * x[j];
		x[k] = sum / R[k][k];
	}
}

static double frobeniusNorm(const DenseMat &A)
{
	double s = 0.0;
	for (const auto &row : A)
		for (double v : row)
			s += v * v;
	return sqrt(s);
}

static double vecNorm(const std::vector<double> &v)
{
	double s = 0.0;
	for (double x : v)
		s += x * x;
	return sqrt(s);
}

static std::vector<double> denseMatVec(const DenseMat &A, const std::vector<double> &x)
{
	std::vector<double> y(A.size(), 0.0);
	for (uint32_t i = 0; i < A.size(); i++) {
		double s = 0.0;
		for (uint32_t j = 0; j < A[i].size(); j++)
			s += A[i][j] * x[j];
		y[i] = s;
	}
	return y;
}

static std::vector<double> denseMatTVec(const DenseMat &A, const std::vector<double> &y)
{
	std::vector<double> x(A[0].size(), 0.0);
	for (uint32_t i = 0; i < A.size(); i++)
		for (uint32_t j = 0; j < A[i].size(); j++)
			x[j] += A[i][j] * y[i];
	return x;
}

/* Recompute the LSQR stopping indicators from scratch on the true residual r = A*x - b. */
static void recomputeIndicators(const DenseMat &A, const std::vector<double> &x, const std::vector<double> &b,
                                double &eta, double &rho, double &rnorm)
{
	const std::vector<double> Ax = denseMatVec(A, x);
	std::vector<double> r(A.size());
	for (uint32_t i = 0; i < A.size(); i++)
		r[i] = Ax[i] - b[i];
	rnorm = vecNorm(r);
	const double atrNorm = vecNorm(denseMatTVec(A, r));
	const double xnorm = vecNorm(x);
	const double bnorm = vecNorm(b);
	const double frob = frobeniusNorm(A);
	const double etaDen = frob * rnorm;
	eta = (etaDen > 0.0) ? atrNorm / etaDen : (atrNorm == 0.0 ? 0.0 : std::numeric_limits<double>::infinity());
	const double rhoDen = frob * xnorm + bnorm;
	rho = (rhoDen > 0.0) ? rnorm / rhoDen : (rnorm == 0.0 ? 0.0 : std::numeric_limits<double>::infinity());
}

/* ------------------------------------------------------------------ */
/* OpenNL assembly helpers.                                            */
/* ------------------------------------------------------------------ */
struct BuiltSystem
{
	NLContext *context;
	std::vector<uint32_t> freeVars;  // Original variable indices that are free.
	std::vector<uint32_t> lockedVars;
};

// Build an LSQR context. Each dense row lists (varIndex, coefficient); locked variables keep fixed values.
static BuiltSystem buildLsqrContext(uint32_t nbVariables,
                                    const std::vector<std::vector<std::pair<uint32_t, double>>> &rows,
                                    const std::vector<uint32_t> &locked, const std::vector<double> &lockedValues,
                                    double threshold, int maxIterations)
{
	BuiltSystem built;
	NLContext *c = nlNewContext();
	c->use_lsqr = true;
	c->threshold = threshold;
	nlSolverParameteri(c, NL_NB_VARIABLES, (int)nbVariables);
	nlSolverParameteri(c, NL_MAX_ITERATIONS, maxIterations);
	nlBegin(c, NL_SYSTEM);
	for (uint32_t i = 0; i < nbVariables; i++)
		nlSetVariable(c, i, 0.0);
	for (uint32_t k = 0; k < locked.size(); k++) {
		nlSetVariable(c, locked[k], lockedValues[k]);
		nlLockVariable(c, locked[k]);
	}
	nlBegin(c, NL_MATRIX);
	for (const auto &row : rows) {
		nlBegin(c, NL_ROW);
		for (const auto &coeff : row)
			nlCoefficient(c, coeff.first, coeff.second);
		nlEnd(c, NL_ROW);
	}
	nlEnd(c, NL_MATRIX);
	nlEnd(c, NL_SYSTEM);
	built.context = c;
	built.lockedVars = locked;
	for (uint32_t i = 0; i < nbVariables; i++) {
		bool isLocked = false;
		for (uint32_t lv : locked)
			if (lv == i) isLocked = true;
		if (!isLocked)
			built.freeVars.push_back(i);
	}
	return built;
}

// Pull the retained rectangular system back out of the context as independent dense storage.
static void extractDenseSystem(NLContext *c, DenseMat &A, std::vector<double> &b)
{
	const uint32_t m = c->lsqr_m;
	const uint32_t n = c->n;
	A.assign(m, std::vector<double>(n, 0.0));
	b.assign(m, 0.0);
	for (uint32_t i = 0; i < m; i++) {
		const NLLsqrRow *row = &c->lsqr_rows[i];
		b[i] = row->rhs;
		for (uint32_t j = 0; j < row->size; j++) {
			// A merged row never carries the same free column twice.
			for (uint32_t q = 0; q < j; q++)
				if (row->coeff[q].index == row->coeff[j].index)
					throw std::runtime_error("duplicate free column survived in retained row");
			A[i][row->coeff[j].index] = row->coeff[j].value;
		}
	}
}

static std::vector<double> lsqrSolution(NLContext *c, const std::vector<uint32_t> &freeVars)
{
	std::vector<double> x(freeVars.size());
	for (uint32_t k = 0; k < freeVars.size(); k++)
		x[k] = nlGetVariable(c, freeVars[k]);
	return x;
}

/* Relative difference between two vectors, robust when the reference is near zero. */
static double relativeDifference(const std::vector<double> &a, const std::vector<double> &b)
{
	double num = 0.0, den = 0.0;
	for (uint32_t i = 0; i < a.size(); i++) {
		num += (a[i] - b[i]) * (a[i] - b[i]);
		den += b[i] * b[i];
	}
	num = sqrt(num);
	den = sqrt(den);
	return den > 0.0 ? num / den : num;
}

/* ------------------------------------------------------------------ */
/* Internal LSCM mesh construction, mirroring the LSCM assembly so     */
/* the retained rows can be inspected directly.                        */
/* ------------------------------------------------------------------ */
struct LscmAssembly
{
	NLContext *context;
	uint32_t lockedVertex0;
	uint32_t lockedVertex1;
};

// Replays computeLeastSquaresConformalMap's relation assembly into an LSQR context, without solving.
static LscmAssembly assembleLscmContext(Mesh *mesh)
{
	LscmAssembly out;
	out.context = nullptr;
	uint32_t lockedVertex0, lockedVertex1;
	if (!param::findApproximateDiameterVertices(mesh, &lockedVertex0, &lockedVertex1))
		return out;
	out.lockedVertex0 = lockedVertex0;
	out.lockedVertex1 = lockedVertex1;
	const uint32_t vertexCount = mesh->vertexCount();
	NLContext *context = nlNewContext();
	context->use_lsqr = true;
	nlSolverParameteri(context, NL_NB_VARIABLES, (int)(2 * vertexCount));
	nlSolverParameteri(context, NL_MAX_ITERATIONS, (int)(5 * vertexCount));
	nlBegin(context, NL_SYSTEM);
	ArrayView<Vector2> texcoords = mesh->texcoords();
	for (uint32_t i = 0; i < vertexCount; i++) {
		nlSetVariable(context, 2 * i, (double)texcoords[i].x);
		nlSetVariable(context, 2 * i + 1, (double)texcoords[i].y);
		if (i == lockedVertex0 || i == lockedVertex1) {
			nlLockVariable(context, 2 * i);
			nlLockVariable(context, 2 * i + 1);
		}
	}
	nlBegin(context, NL_MATRIX);
	const uint32_t faceCount = mesh->faceCount();
	ConstArrayView<Vector3> positions = mesh->positions();
	ConstArrayView<uint32_t> indices = mesh->indices();
	for (uint32_t f = 0; f < faceCount; f++) {
		const uint32_t v0 = indices[f * 3 + 0];
		const uint32_t v1 = indices[f * 3 + 1];
		const uint32_t v2 = indices[f * 3 + 2];
		if (!param::setup_abf_relations(context, (int)v0, (int)v1, (int)v2, positions[v0], positions[v1], positions[v2])) {
			Vector2 z0, z1, z2;
			param::projectTriangle(positions[v0], positions[v1], positions[v2], &z0, &z1, &z2);
			double a = z1.x - z0.x;
			double b = z1.y - z0.y;
			double c = z2.x - z0.x;
			double d = z2.y - z0.y;
			uint32_t u0_id = 2 * v0, v0_id = 2 * v0 + 1;
			uint32_t u1_id = 2 * v1, v1_id = 2 * v1 + 1;
			uint32_t u2_id = 2 * v2, v2_id = 2 * v2 + 1;
			nlBegin(context, NL_ROW);
			nlCoefficient(context, u0_id, -a + c);
			nlCoefficient(context, v0_id, b - d);
			nlCoefficient(context, u1_id, -c);
			nlCoefficient(context, v1_id, d);
			nlCoefficient(context, u2_id, a);
			nlEnd(context, NL_ROW);
			nlBegin(context, NL_ROW);
			nlCoefficient(context, u0_id, -b + d);
			nlCoefficient(context, v0_id, -a + c);
			nlCoefficient(context, u1_id, -d);
			nlCoefficient(context, v1_id, -c);
			nlCoefficient(context, v2_id, a);
			nlEnd(context, NL_ROW);
		}
	}
	nlEnd(context, NL_MATRIX);
	nlEnd(context, NL_SYSTEM);
	out.context = context;
	return out;
}

// Meshes are allocated through xatlas's tagged allocator, so they must be destroyed the same way.
static Mesh *newTestMesh(uint32_t approxVertexCount, uint32_t approxFaceCount)
{
	void *mem = xatlas::internal::Realloc(nullptr, sizeof(Mesh), xatlas::internal::MemTag::Default, __FILE__, __LINE__);
	return new (mem) Mesh(1e-6f, approxVertexCount, approxFaceCount);
}

static void destroyMesh(Mesh *mesh)
{
	mesh->~Mesh();
	xatlas::internal::Realloc(mesh, 0, xatlas::internal::MemTag::Default, __FILE__, __LINE__);
}

// Builds an open triangle sheet. Unequal column spacing + curvature gives the long, non-planar strip.
static Mesh *buildCurvedStrip(uint32_t columns, uint32_t rows)
{
	Mesh *mesh = newTestMesh(columns * rows, (columns - 1) * (rows - 1) * 2);
	double x = 0.0;
	for (uint32_t cx = 0; cx < columns; cx++) {
		const double step = 1.0 + 0.6 * (double)cx; // Unequal strip widths.
		if (cx != 0)
			x += step;
		for (uint32_t cy = 0; cy < rows; cy++) {
			const double y = (double)cy;
			const double z = 0.5 * sin(0.7 * x) + 0.25 * y * y; // Curved, non-planar.
			mesh->addVertex(Vector3((float)x, (float)y, (float)z), Vector3(0.0f), Vector2((float)x, (float)y));
		}
	}
	for (uint32_t cx = 0; cx + 1 < columns; cx++) {
		for (uint32_t cy = 0; cy + 1 < rows; cy++) {
			const uint32_t a = cx * rows + cy;
			const uint32_t b = (cx + 1) * rows + cy;
			const uint32_t c = (cx + 1) * rows + (cy + 1);
			const uint32_t d = cx * rows + (cy + 1);
			uint32_t tri0[3] = { a, b, c };
			uint32_t tri1[3] = { a, c, d };
			mesh->addFace(tri0);
			mesh->addFace(tri1);
		}
	}
	mesh->createColocals();
	mesh->createBoundaries();
	return mesh;
}

// Open patch with a genuine interior vertex (grid >= 3x3) and a dome shape.
static Mesh *buildInteriorPatch(uint32_t columns, uint32_t rows)
{
	Mesh *mesh = newTestMesh(columns * rows, (columns - 1) * (rows - 1) * 2);
	for (uint32_t cx = 0; cx < columns; cx++) {
		for (uint32_t cy = 0; cy < rows; cy++) {
			const double x = (double)cx - (double)(columns - 1) * 0.5;
			const double y = (double)cy - (double)(rows - 1) * 0.5;
			const double z = 0.8 * exp(-(x * x + y * y) * 0.25); // Smooth non-planar dome.
			mesh->addVertex(Vector3((float)x, (float)y, (float)z), Vector3(0.0f), Vector2((float)cx, (float)cy));
		}
	}
	for (uint32_t cx = 0; cx + 1 < columns; cx++) {
		for (uint32_t cy = 0; cy + 1 < rows; cy++) {
			const uint32_t a = cx * rows + cy;
			const uint32_t b = (cx + 1) * rows + cy;
			const uint32_t c = (cx + 1) * rows + (cy + 1);
			const uint32_t d = cx * rows + (cy + 1);
			uint32_t tri0[3] = { a, b, c };
			uint32_t tri1[3] = { a, c, d };
			mesh->addFace(tri0);
			mesh->addFace(tri1);
		}
	}
	mesh->createColocals();
	mesh->createBoundaries();
	return mesh;
}

// Verifies an internal-mesh LSCM run: finite UVs, positive area, preserved locks, low conformal energy.
static void verifyInternalLscm(Case &q, Mesh *mesh, const char *tag)
{
	const uint32_t vertexCount = mesh->vertexCount();
	std::vector<Vector2> originalUv(vertexCount);
	for (uint32_t i = 0; i < vertexCount; i++)
		originalUv[i] = mesh->texcoord(i);

	LscmAssembly assembly = assembleLscmContext(mesh);
	q.check(assembly.context != nullptr, tag);
	NLContext *probe = assembly.context;
	const uint32_t locked0 = assembly.lockedVertex0;
	const uint32_t locked1 = assembly.lockedVertex1;

	const bool ok = param::computeLeastSquaresConformalMap(mesh, true);
	q.check(ok, "internal LSCM LSQR solve succeeded");

	// Re-solve the independently assembled probe context and inspect its own status.
	const bool probeOk = nlSolve(probe);
	q.check(probeOk, "retained LSCM rectangular system converges");
	q.check(probe->lsqr_status == Converged, "probe reports Converged");

	// Every output UV is finite and the patch has positive parametric area.
	double area = 0.0;
	for (uint32_t i = 0; i < vertexCount; i++)
		q.check(finiteFloat(mesh->texcoord(i).x) && finiteFloat(mesh->texcoord(i).y), "finite LSCM UVs");
	for (uint32_t f = 0; f < mesh->faceCount(); f++)
		area += fabs((double)mesh->computeFaceParametricArea(f));
	q.check(area > 0.0, "positive parametric area");

	// Locked vertices keep their exact input values.
	q.check(mesh->texcoord(locked0).x == originalUv[locked0].x && mesh->texcoord(locked0).y == originalUv[locked0].y, "locked vertex 0 preserved");
	q.check(mesh->texcoord(locked1).x == originalUv[locked1].x && mesh->texcoord(locked1).y == originalUv[locked1].y, "locked vertex 1 preserved");

	// Independently reconstruct A, b and evaluate the raw LSCM relation energy at the produced solution.
	DenseMat A;
	std::vector<double> b;
	extractDenseSystem(probe, A, b);
	q.check(A.size() == (size_t)probe->lsqr_m && A[0].size() == (size_t)probe->n, "dense system dimensions");
	std::vector<double> x(probe->n, 0.0);
	for (uint32_t original = 0; original < 2 * vertexCount; original++) {
		const uint32_t freeIndex = probe->variable_index[original];
		if (freeIndex != UINT32_MAX)
			x[freeIndex] = (original % 2 == 0) ? (double)mesh->texcoord(original / 2).x : (double)mesh->texcoord(original / 2).y;
	}
	double eta, rho, rnorm;
	recomputeIndicators(A, x, b, eta, rho, rnorm);
	q.check(finiteDouble(eta) && finiteDouble(rho), "recomputed LSCM indicators finite");
	// LSCM rows are assembled from float-rounded geometry, so the residual floor is around 1e-4..1e-3.
	// The stationarity indicator eta is the meaningful smallness test here; rho only needs to stay small.
	q.check(eta <= 1e-3, "LSCM solution is stationary (small eta)");
	q.check(rho <= 1e-2, "raw LSCM relations have small scaled backward error");
	nlDeleteContext(probe);
}

/* ------------------------------------------------------------------ */
/* Public pipeline helpers (C API, with a struct layout check).        */
/* ------------------------------------------------------------------ */
#if XATLAS_C_API
static int g_paramFuncCalls = 0;
static void testParameterizeFunc(const float *positions, float *texcoords, uint32_t vertexCount,
                                 const uint32_t *indices, uint32_t indexCount)
{
	(void)indices;
	(void)indexCount;
	g_paramFuncCalls++;
	for (uint32_t i = 0; i < vertexCount; i++) {
		// Deterministic, non-trivial custom parameterization so the callback is observably used.
		texcoords[i * 2 + 0] = positions[i * 3 + 0] + 0.5f * positions[i * 3 + 2];
		texcoords[i * 2 + 1] = positions[i * 3 + 1] - 0.5f * positions[i * 3 + 2];
	}
}

struct PublicMesh
{
	std::vector<float> positions;
	std::vector<uint32_t> indices;
};

static PublicMesh makeStripGeometry(uint32_t columns, uint32_t rows)
{
	PublicMesh out;
	for (uint32_t cx = 0; cx < columns; cx++) {
		for (uint32_t cy = 0; cy < rows; cy++) {
			const float x = (float)cx * 1.3f;
			const float y = (float)cy;
			const float z = 0.4f * sinf(0.8f * (float)cx);
			out.positions.push_back(x);
			out.positions.push_back(y);
			out.positions.push_back(z);
		}
	}
	for (uint32_t cx = 0; cx + 1 < columns; cx++) {
		for (uint32_t cy = 0; cy + 1 < rows; cy++) {
			const uint32_t a = cx * rows + cy;
			const uint32_t b = (cx + 1) * rows + cy;
			const uint32_t c = (cx + 1) * rows + (cy + 1);
			const uint32_t d = cx * rows + (cy + 1);
			out.indices.insert(out.indices.end(), { a, b, c, a, c, d });
		}
	}
	return out;
}

// Strongly curved open sheet so the chart cannot stay planar/orthographic and goes through LSCM.
static PublicMesh makeDomeGeometry(uint32_t columns, uint32_t rows)
{
	PublicMesh out;
	for (uint32_t cx = 0; cx < columns; cx++) {
		for (uint32_t cy = 0; cy < rows; cy++) {
			const float x = (float)cx - (float)(columns - 1) * 0.5f;
			const float y = (float)cy - (float)(rows - 1) * 0.5f;
			const float z = 2.5f * sinf(0.9f * x) * cosf(0.8f * y) + 1.5f * expf(-0.15f * (x * x + y * y));
			out.positions.push_back(x);
			out.positions.push_back(y);
			out.positions.push_back(z);
		}
	}
	for (uint32_t cx = 0; cx + 1 < columns; cx++) {
		for (uint32_t cy = 0; cy + 1 < rows; cy++) {
			const uint32_t a = cx * rows + cy;
			const uint32_t b = (cx + 1) * rows + cy;
			const uint32_t c = (cx + 1) * rows + (cy + 1);
			const uint32_t d = cx * rows + (cy + 1);
			out.indices.insert(out.indices.end(), { a, b, c, a, c, d });
		}
	}
	return out;
}

static void runPublicPipeline(Case &q, const PublicMesh &geom, bool useLsqr, bool useParamFunc, int expectedChartsAtLeast)
{
	xatlasAtlas *atlas = xatlasCreate();
	q.check(atlas != nullptr, "atlas created");
	xatlasMeshDecl decl;
	xatlasMeshDeclInit(&decl);
	decl.vertexCount = (uint32_t)(geom.positions.size() / 3);
	decl.vertexPositionData = geom.positions.data();
	decl.vertexPositionStride = sizeof(float) * 3;
	decl.indexCount = (uint32_t)geom.indices.size();
	decl.indexData = geom.indices.data();
	decl.indexFormat = XATLAS_INDEX_FORMAT_UINT32;
	const xatlasAddMeshError addErr = xatlasAddMesh(atlas, &decl, 0);
	q.check(addErr == XATLAS_ADD_MESH_ERROR_SUCCESS, "AddMesh succeeded");

	xatlasChartOptions options;
	xatlasChartOptionsInit(&options);
	options.useLsqr = useLsqr;
	if (useParamFunc) {
		g_paramFuncCalls = 0;
		options.paramFunc = testParameterizeFunc;
	}
	xatlasComputeCharts(atlas, &options);
	xatlasPackOptions packOptions;
	xatlasPackOptionsInit(&packOptions);
	xatlasPackCharts(atlas, &packOptions);

	q.check(atlas->chartCount >= (uint32_t)expectedChartsAtLeast, "charts produced");
	q.check(atlas->meshCount == 1, "one output mesh");
	const xatlasMesh &outMesh = atlas->meshes[0];
	q.check(outMesh.indexCount == (uint32_t)geom.indices.size(), "index count preserved");
	q.check(outMesh.vertexCount > 0, "output vertices produced");
	for (uint32_t i = 0; i < outMesh.vertexCount; i++) {
		q.check(finiteFloat(outMesh.vertexArray[i].uv[0]) && finiteFloat(outMesh.vertexArray[i].uv[1]), "finite public UVs");
		q.check(outMesh.vertexArray[i].atlasIndex >= 0, "vertex assigned to an atlas");
	}
	if (useParamFunc)
		q.check(g_paramFuncCalls > 0, "paramFunc took precedence over internal LSCM");
	else
		q.check(g_paramFuncCalls == 0, "no paramFunc invocation when unset");
	xatlasDestroy(atlas);
}
#endif // XATLAS_C_API

/* ================================================================== */
/* Group 1: rectangular-ls                                            */
/* ================================================================== */
static int testRectFullRank(Case &q)
{
	// Two free vars, one locked constant: x0+x1=3, x0-x1=-1 -> x0=1, x1=2.
	std::vector<std::vector<std::pair<uint32_t, double>>> rows = {
		{ {0, 1.0}, {1, 1.0}, {2, -3.0} },
		{ {0, 1.0}, {1, -1.0}, {2, 1.0} }
	};
	BuiltSystem built = buildLsqrContext(3, rows, { 2 }, { 1.0 }, 1e-12, 100);
	NLContext *c = built.context;
	q.check(c->M == nullptr, "no normal-equation matrix allocated on LSQR path");
	const bool ok = nlSolve(c);
	q.check(ok, "full rank solve succeeded");
	q.check(c->lsqr_status == Converged, "Converged status");
	DenseMat A;
	std::vector<double> b;
	extractDenseSystem(c, A, b);
	std::vector<double> qrX;
	householderLeastSquares(A, b, qrX);
	const std::vector<double> lsqrX = lsqrSolution(c, built.freeVars);
	q.check(relativeDifference(lsqrX, qrX) < 1e-8, "matches independent Householder QR");
	q.check(fabs(lsqrX[0] - 1.0) < 1e-8 && fabs(lsqrX[1] - 2.0) < 1e-8, "expected analytic values");
	nlDeleteContext(c);
	return 0;
}

static int testRectInconsistent(Case &q)
{
	// Single free var with conflicting equations x0=0 and x0=1. LS solution is x0=0.5 with nonzero residual.
	std::vector<std::vector<std::pair<uint32_t, double>>> rows = {
		{ {0, 1.0} },
		{ {0, 1.0}, {1, -1.0} }
	};
	BuiltSystem built = buildLsqrContext(2, rows, { 1 }, { 1.0 }, 1e-12, 100);
	NLContext *c = built.context;
	const bool ok = nlSolve(c);
	q.check(ok, "inconsistent system terminates successfully at a stationary point");
	q.check(c->lsqr_status == Converged, "stationarity (eta) ends incompatible LS");
	const std::vector<double> x = lsqrSolution(c, built.freeVars);
	q.check(fabs(x[0] - 0.5) < 1e-9, "least squares value 0.5");
	DenseMat A;
	std::vector<double> b;
	extractDenseSystem(c, A, b);
	double eta, rho, rnorm;
	recomputeIndicators(A, x, b, eta, rho, rnorm);
	q.check(rnorm > 0.1, "nonzero residual retained for incompatible system");
	q.check(eta <= 1e-10, "independently recomputed stationarity indicator small");
	nlDeleteContext(c);
	return 0;
}

static int testRectDuplicateCoeffs(Case &q)
{
	// Repeated free coefficients in one row must merge: (x0 + x0 + 2*x0) = 4 -> 4*x0 = 4 -> x0 = 1.
	std::vector<std::vector<std::pair<uint32_t, double>>> rows = {
		{ {0, 1.0}, {0, 1.0}, {0, 2.0}, {1, -4.0} }
	};
	BuiltSystem built = buildLsqrContext(2, rows, { 1 }, { 1.0 }, 1e-12, 100);
	NLContext *c = built.context;
	q.check(nlSolve(c), "duplicate-coefficient solve succeeded");
	DenseMat A;
	std::vector<double> b;
	extractDenseSystem(c, A, b); // Throws if a duplicate column survived.
	q.check(A[0][0] == 4.0, "duplicate free coefficients merged");
	q.check(fabs(lsqrSolution(c, built.freeVars)[0] - 1.0) < 1e-9, "merged row solves to 1");
	nlDeleteContext(c);
	return 0;
}

static int testRectMultipleLocks(Case &q)
{
	// x1=2, x2=3 locked, x3=1 a locked constant. Relations x0+x1+x2-10*x3=0 and x0-x1-3*x3=0 -> x0=5.
	std::vector<std::vector<std::pair<uint32_t, double>>> rows = {
		{ {0, 1.0}, {1, 1.0}, {2, 1.0}, {3, -10.0} },
		{ {0, 1.0}, {1, -1.0}, {3, -3.0} }
	};
	BuiltSystem built = buildLsqrContext(4, rows, { 1, 2, 3 }, { 2.0, 3.0, 1.0 }, 1e-12, 100);
	NLContext *c = built.context;
	q.check(nlSolve(c), "multiple-lock solve succeeded");
	DenseMat A;
	std::vector<double> b;
	extractDenseSystem(c, A, b);
	std::vector<double> qrX;
	householderLeastSquares(A, b, qrX);
	q.check(relativeDifference(lsqrSolution(c, built.freeVars), qrX) < 1e-8, "matches QR with multiple locks");
	q.check(fabs(lsqrSolution(c, built.freeVars)[0] - 5.0) < 1e-8, "locked constants moved into rhs");
	q.check(nlGetVariable(c, 1) == 2.0 && nlGetVariable(c, 2) == 3.0 && nlGetVariable(c, 3) == 1.0, "locked values retained exactly");
	nlDeleteContext(c);
	return 0;
}

static int testRectRankDeficient(Case &q)
{
	// Two free vars, one relation x0+x1=1. Rank deficient: any finite stationary point is accepted.
	std::vector<std::vector<std::pair<uint32_t, double>>> rows = {
		{ {0, 1.0}, {1, 1.0}, {2, -1.0} }
	};
	BuiltSystem built = buildLsqrContext(3, rows, { 2 }, { 1.0 }, 1e-10, 100);
	NLContext *c = built.context;
	const bool ok = nlSolve(c);
	q.check(ok, "rank-deficient system accepted at a stationary point");
	const std::vector<double> x = lsqrSolution(c, built.freeVars);
	for (double v : x)
		q.check(finiteDouble(v), "finite stationary solution for rank-deficient system");
	DenseMat A;
	std::vector<double> b;
	extractDenseSystem(c, A, b);
	double eta, rho, rnorm;
	recomputeIndicators(A, x, b, eta, rho, rnorm);
	// The single relation is consistent, so the stationary point fits it: the scaled backward error is tiny.
	// eta alone can be O(1) at a floating (not exactly zero) residual, so success is judged by either indicator.
	q.check(rnorm < 1e-8, "rank-deficient stationary point fits the relation");
	q.check(std::min(eta, rho) <= 1e-8, "rank-deficient result passes a stopping indicator");
	nlDeleteContext(c);
	return 0;
}

static int testRectZeroColumn(Case &q)
{
	// x0 never appears (zero column); x1=3. x0 stays at its zero start, x1 solves to 3.
	std::vector<std::vector<std::pair<uint32_t, double>>> rows = {
		{ {1, 1.0}, {2, -3.0} }
	};
	BuiltSystem built = buildLsqrContext(3, rows, { 2 }, { 1.0 }, 1e-12, 100);
	NLContext *c = built.context;
	q.check(nlSolve(c), "zero-column solve succeeded");
	const std::vector<double> x = lsqrSolution(c, built.freeVars);
	q.check(x[0] == 0.0, "zero column leaves variable at zero");
	q.check(fabs(x[1] - 3.0) < 1e-9, "active column solves correctly");
	nlDeleteContext(c);
	return 0;
}

static int testRectAllLocked(Case &q)
{
	// Every variable locked; rows contain only locked coefficients and must still count in the residual.
	std::vector<std::vector<std::pair<uint32_t, double>>> rows = {
		{ {0, 2.0} },
		{ {0, -1.0} }
	};
	BuiltSystem built = buildLsqrContext(1, rows, { 0 }, { 7.0 }, 1e-12, 100);
	NLContext *c = built.context;
	q.check(c->n == 0, "no free variables after full locking");
	q.check(c->lsqr_m == 2, "locked-only rows are retained");
	const bool ok = nlSolve(c);
	q.check(ok, "all-locked system succeeds");
	q.check(nlGetVariable(c, 0) == 7.0, "locked value untouched");
	DenseMat A;
	std::vector<double> b;
	extractDenseSystem(c, A, b);
	// Rows have no free coefficients; rhs encodes -a_l * v_l, so residuals are -rhs and generally nonzero.
	double residualSum = 0.0;
	for (uint32_t i = 0; i < b.size(); i++)
		residualSum += b[i] * b[i];
	q.check(residualSum > 0.0, "locked-only rows contribute to the residual");
	nlDeleteContext(c);
	return 0;
}

static int testRectExtremeScale(Case &q)
{
	// Column coefficients 16 orders apart, both consistent with x0=1.
	std::vector<std::vector<std::pair<uint32_t, double>>> rows = {
		{ {0, 1e8}, {1, -1e8} },
		{ {0, 1e-8}, {1, -1e-8} }
	};
	BuiltSystem built = buildLsqrContext(2, rows, { 1 }, { 1.0 }, 1e-12, 200);
	NLContext *c = built.context;
	q.check(nlSolve(c), "extreme-scale solve succeeded");
	DenseMat A;
	std::vector<double> b;
	extractDenseSystem(c, A, b);
	std::vector<double> qrX;
	householderLeastSquares(A, b, qrX);
	q.check(relativeDifference(lsqrSolution(c, built.freeVars), qrX) < 1e-6, "extreme scale matches QR without normal equations");
	q.check(fabs(lsqrSolution(c, built.freeVars)[0] - 1.0) < 1e-6, "extreme-scale value correct");
	nlDeleteContext(c);
	return 0;
}

static int testRectLockOnlyAndZeroRows(Case &q)
{
	// var0 free; var1=1 a locked constant encoding rhs; var2=0 a locked variable.
	//  row0: x0 - 2*var1 = 0            -> rhs 2, so x0 = 2.
	//  row1: 5*var2 = 0 (locked only)   -> zero residual because var2 is zero.
	//  row2: completely empty (no coeff)-> zero row, zero residual.
	std::vector<std::vector<std::pair<uint32_t, double>>> rows = {
		{ {0, 1.0}, {1, -2.0} },
		{ {2, 5.0} },
		{}
	};
	BuiltSystem built = buildLsqrContext(3, rows, { 1, 2 }, { 1.0, 0.0 }, 1e-12, 100);
	NLContext *c = built.context;
	q.check(c->lsqr_m == 3, "all rows retained including locked-only and empty rows");
	q.check(c->lsqr_rows[1].size == 0 && c->lsqr_rows[2].size == 0, "locked-only and zero rows carry no free coefficients");
	q.check(nlSolve(c), "mixed free/locked-only/zero system solves");
	DenseMat A;
	std::vector<double> b;
	extractDenseSystem(c, A, b);
	const std::vector<double> x = lsqrSolution(c, built.freeVars);
	q.check(fabs(x[0] - 2.0) < 1e-9, "free variable correct");
	double eta, rho, rnorm;
	recomputeIndicators(A, x, b, eta, rho, rnorm);
	q.check(rnorm < 1e-9, "zero-valued locked-only row and empty row contribute zero residual");
	nlDeleteContext(c);
	return 0;
}

/* ================================================================== */
/* Group 2: stopping                                                  */
/* ================================================================== */
static int testStopEtaStationarity(Case &q)
{
	// Incompatible system: eta (not rho) must reach tolerance at the nonzero least-squares residual.
	std::vector<std::vector<std::pair<uint32_t, double>>> rows = {
		{ {0, 1e6} },
		{ {0, 1e6}, {1, -1.0} }
	};
	BuiltSystem built = buildLsqrContext(2, rows, { 1 }, { 1.0 }, 1e-10, 200);
	NLContext *c = built.context;
	const bool ok = nlSolve(c);
	q.check(ok, "incompatible LS converges via stationarity");
	const std::vector<double> x = lsqrSolution(c, built.freeVars);
	DenseMat A;
	std::vector<double> b;
	extractDenseSystem(c, A, b);
	double eta, rho, rnorm;
	recomputeIndicators(A, x, b, eta, rho, rnorm);
	q.check(eta <= 1e-8, "eta independently small");
	q.check(c->error <= 1e-8 && fabs(c->error - std::min(eta, rho)) < 1e-12, "error reports recomputed min(eta,rho)");
	q.check(c->used_iterations >= 1 && c->used_iterations <= 200, "used_iterations is the bidiagonalization step count");
	nlDeleteContext(c);
	return 0;
}

static int testStopRhoConsistent(Case &q)
{
	// Consistent system terminates near machine precision through rho.
	std::vector<std::vector<std::pair<uint32_t, double>>> rows = {
		{ {0, 1.0}, {1, 1.0}, {2, -3.0} },
		{ {0, 1.0}, {1, -1.0}, {2, 1.0} }
	};
	BuiltSystem built = buildLsqrContext(3, rows, { 2 }, { 1.0 }, 1e-12, 100);
	NLContext *c = built.context;
	q.check(nlSolve(c), "consistent system converged");
	const std::vector<double> x = lsqrSolution(c, built.freeVars);
	DenseMat A;
	std::vector<double> b;
	extractDenseSystem(c, A, b);
	double eta, rho, rnorm;
	recomputeIndicators(A, x, b, eta, rho, rnorm);
	q.check(rho <= 1e-10, "rho independently near machine precision");
	q.check(fabs(c->error - std::min(eta, rho)) < 1e-12, "error is recomputed min indicator");
	nlDeleteContext(c);
	return 0;
}

static int testStopIterationLimit(Case &q)
{
	// max_iterations = 0 on a system not solved by the zero initial guess -> IterationLimit, false.
	std::vector<std::vector<std::pair<uint32_t, double>>> rows = {
		{ {0, 1.0}, {1, 1.0}, {2, -3.0} },
		{ {0, 1.0}, {1, -1.0}, {2, 1.0} }
	};
	BuiltSystem built = buildLsqrContext(3, rows, { 2 }, { 1.0 }, 1e-12, 0);
	NLContext *c = built.context;
	const bool ok = nlSolve(c);
	q.check(!ok, "exhausted iteration budget returns false");
	q.check(c->lsqr_status == IterationLimit, "IterationLimit status");
	q.check(c->used_iterations == 0, "no bidiagonalization steps taken");
	q.check(c->error > 1e-3, "unconverged error remains large");
	// Free variables are not written back on failure.
	q.check(nlGetVariable(c, 0) == 0.0 && nlGetVariable(c, 1) == 0.0, "no half-finished writeback on failure");
	nlDeleteContext(c);
	return 0;
}

static int testStopInvalidInput(Case &q)
{
	// Non-positive tolerance.
	BuiltSystem zeroTol = buildLsqrContext(1, { { {0, 1.0} } }, {}, {}, 0.0, 10);
	q.check(!nlSolve(zeroTol.context), "zero tolerance rejected");
	q.check(zeroTol.context->lsqr_status == InvalidInput, "zero tolerance -> InvalidInput");
	nlDeleteContext(zeroTol.context);
	BuiltSystem negTol = buildLsqrContext(1, { { {0, 1.0} } }, {}, {}, -1e-6, 10);
	q.check(!nlSolve(negTol.context), "negative tolerance rejected");
	q.check(negTol.context->lsqr_status == InvalidInput, "negative tolerance -> InvalidInput");
	nlDeleteContext(negTol.context);
	// Non-finite retained data.
	BuiltSystem nan = buildLsqrContext(1, { { {0, 1.0}, {1, -1.0} } }, { 1 }, { 1.0 }, 1e-10, 10);
	nan.context->lsqr_rows[0].rhs = std::numeric_limits<double>::quiet_NaN();
	q.check(!nlSolve(nan.context), "non-finite input rejected");
	q.check(nan.context->lsqr_status == InvalidInput, "non-finite data -> InvalidInput");
	nlDeleteContext(nan.context);
	return 0;
}

static int testStopBreakdown(Case &q)
{
	// Incompatible system with a tolerance below the machine-precision stationarity floor: stagnation -> Breakdown.
	std::vector<std::vector<std::pair<uint32_t, double>>> rows = {
		{ {0, 1e10} },
		{ {0, 1e10}, {1, -1.0} }
	};
	BuiltSystem built = buildLsqrContext(2, rows, { 1 }, { 1.0 }, 1e-18, 200);
	NLContext *c = built.context;
	const bool ok = nlSolve(c);
	q.check(!ok, "numerical stagnation returns false");
	q.check(c->lsqr_status == Breakdown, "Breakdown status without meeting stationarity");
	q.check(finiteDouble(c->error), "breakdown still reports a finite error");
	nlDeleteContext(c);
	return 0;
}

static int testStopZeroOverZero(Case &q)
{
	// Homogeneous zero-rhs system starting from zero: zero-over-zero indicators are defined as zero.
	BuiltSystem built = buildLsqrContext(2, { { {0, 1.0} }, { {1, 1.0} } }, {}, {}, 1e-12, 100);
	NLContext *c = built.context;
	const bool ok = nlSolve(c);
	q.check(ok, "zero rhs system succeeds immediately");
	q.check(c->lsqr_status == Converged, "Converged on zero initial residual");
	q.check(c->used_iterations == 0, "no iterations needed for zero rhs");
	q.check(c->error == 0.0, "zero-over-zero indicator defined as zero");
	const std::vector<double> x = lsqrSolution(c, built.freeVars);
	q.check(x[0] == 0.0 && x[1] == 0.0, "zero solution retained");
	nlDeleteContext(c);
	return 0;
}

static int testStopFreshStartAndLocks(Case &q)
{
	// Every solve starts at the zero free vector; seed a nonzero initial value and confirm it is ignored,
	// while locked values survive a forced-failure run untouched.
	NLContext *c = nlNewContext();
	c->use_lsqr = true;
	c->threshold = 1e-12;
	nlSolverParameteri(c, NL_NB_VARIABLES, 3);
	nlSolverParameteri(c, NL_MAX_ITERATIONS, 0); // Force failure before any update.
	nlBegin(c, NL_SYSTEM);
	nlSetVariable(c, 0, 9.0);  // Free seed that must be reset to zero, never carried into the solve.
	nlSetVariable(c, 1, -4.0); // Free seed.
	nlSetVariable(c, 2, 1.5);
	nlLockVariable(c, 2);
	nlBegin(c, NL_MATRIX);
	nlBegin(c, NL_ROW);
	nlCoefficient(c, 0, 1.0);
	nlCoefficient(c, 1, 1.0);
	nlCoefficient(c, 2, -3.0);
	nlEnd(c, NL_ROW);
	nlBegin(c, NL_ROW);
	nlCoefficient(c, 0, 1.0);
	nlCoefficient(c, 1, -1.0);
	nlCoefficient(c, 2, 1.0);
	nlEnd(c, NL_ROW);
	nlEnd(c, NL_MATRIX);
	nlEnd(c, NL_SYSTEM);
	const bool ok = nlSolve(c);
	q.check(!ok, "forced failure");
	q.check(c->lsqr_status == IterationLimit, "iteration limit on fresh-start check");
	q.check(nlGetVariable(c, 0) == 9.0 && nlGetVariable(c, 1) == -4.0, "free seed left intact when solve fails (no writeback)");
	q.check(nlGetVariable(c, 2) == 1.5, "locked value preserved through failure");
	nlDeleteContext(c);
	return 0;
}

/* ================================================================== */
/* Group 3: lscm-integration                                          */
/* ================================================================== */
static int testLscmCurvedStrip(Case &q)
{
	Mesh *mesh = buildCurvedStrip(7, 3);
	q.check(mesh->vertexCount() == 21 && mesh->faceCount() == 24, "curved strip dimensions");
	verifyInternalLscm(q, mesh, "curved strip assembly");
	destroyMesh(mesh);
	return 0;
}

static int testLscmInteriorPatch(Case &q)
{
	Mesh *mesh = buildInteriorPatch(4, 4);
	q.check(mesh->vertexCount() == 16 && mesh->faceCount() == 18, "interior patch dimensions");
	// The center vertex (column 1, row 1 of a 4x4 grid) is genuinely interior.
	q.check(!mesh->isBoundaryVertex(5), "interior vertex present");
	verifyInternalLscm(q, mesh, "interior patch assembly");
	destroyMesh(mesh);
	return 0;
}

static int testLscmRerunFixedChart(Case &q)
{
	// Re-run the same fixed chart through both solver entry points; both must produce finite, stationary UVs.
	Mesh *meshA = buildCurvedStrip(5, 3);
	q.check(param::computeLeastSquaresConformalMap(meshA, true), "LSQR rerun of fixed chart");
	std::vector<Vector2> lsqrUv(meshA->vertexCount());
	for (uint32_t i = 0; i < meshA->vertexCount(); i++) {
		q.check(finiteFloat(meshA->texcoord(i).x), "LSQR rerun finite");
		lsqrUv[i] = meshA->texcoord(i);
	}
	Mesh *meshB = buildCurvedStrip(5, 3);
	q.check(param::computeLeastSquaresConformalMap(meshB, false), "default CG rerun of fixed chart still works");
	for (uint32_t i = 0; i < meshB->vertexCount(); i++)
		q.check(finiteFloat(meshB->texcoord(i).x), "CG rerun finite");
	// The two solvers agree on the same chart up to a loose tolerance.
	double maxDiff = 0.0;
	for (uint32_t i = 0; i < meshA->vertexCount(); i++) {
		maxDiff = std::max(maxDiff, (double)fabs(lsqrUv[i].x - meshB->texcoord(i).x));
		maxDiff = std::max(maxDiff, (double)fabs(lsqrUv[i].y - meshB->texcoord(i).y));
	}
	q.check(maxDiff < 1e-3, "LSQR and CG agree on the fixed chart");
	destroyMesh(meshA);
	destroyMesh(meshB);
	return 0;
}

#if XATLAS_C_API
static int testPublicLsqrPipeline(Case &q)
{
	static_assert(sizeof(xatlas::ChartOptions) == sizeof(xatlasChartOptions), "C API chart options layout");
	xatlasChartOptions options;
	xatlasChartOptionsInit(&options);
	q.check(options.useLsqr == false, "C API default has LSQR off");
	options.useLsqr = true;
	q.check(options.useLsqr == true, "C API can enable LSQR");
	xatlas::ChartOptions cppOptions;
	q.check(cppOptions.useLsqr == false, "C++ default has LSQR off");
	g_paramFuncCalls = 0;
	runPublicPipeline(q, makeStripGeometry(8, 3), true, false, 1);
	return 0;
}

static int testPublicCgPipeline(Case &q)
{
	g_paramFuncCalls = 0;
	runPublicPipeline(q, makeStripGeometry(8, 3), false, false, 1);
	return 0;
}

static int testPublicParamFuncPrecedence(Case &q)
{
	// A strongly curved dome forces LSCM charts, where paramFunc is actually consulted.
	g_paramFuncCalls = 0;
	runPublicPipeline(q, makeDomeGeometry(10, 8), true, true, 1); // LSQR requested, but paramFunc must win.
	q.check(g_paramFuncCalls > 0, "paramFunc invoked despite useLsqr being set");
	return 0;
}

static int testPublicAbaRerunIsolation(Case &q)
{
	// A/B/A: LSQR -> CG -> LSQR on one atlas, then a second atlas with a different mesh; no state leaks.
	for (int pass = 0; pass < 3; pass++) {
		PublicMesh geom = makeStripGeometry(pass == 1 ? 6 : 8, 3);
		xatlasAtlas *atlas = xatlasCreate();
		xatlasMeshDecl decl;
		xatlasMeshDeclInit(&decl);
		decl.vertexCount = (uint32_t)(geom.positions.size() / 3);
		decl.vertexPositionData = geom.positions.data();
		decl.vertexPositionStride = sizeof(float) * 3;
		decl.indexCount = (uint32_t)geom.indices.size();
		decl.indexData = geom.indices.data();
		decl.indexFormat = XATLAS_INDEX_FORMAT_UINT32;
		q.check(xatlasAddMesh(atlas, &decl, 0) == XATLAS_ADD_MESH_ERROR_SUCCESS, "rerun AddMesh");
		xatlasChartOptions options;
		xatlasChartOptionsInit(&options);
		options.useLsqr = (pass != 1); // A: LSQR, B: CG, A: LSQR.
		xatlasComputeCharts(atlas, &options);
		xatlasPackOptions packOptions;
		xatlasPackOptionsInit(&packOptions);
		xatlasPackCharts(atlas, &packOptions);
		q.check(atlas->chartCount >= 1, "rerun produces charts");
		q.check(atlas->meshes[0].vertexCount > 0, "rerun produces vertices");
		for (uint32_t i = 0; i < atlas->meshes[0].vertexCount; i++)
			q.check(finiteFloat(atlas->meshes[0].vertexArray[i].uv[0]), "rerun UVs finite");
		xatlasDestroy(atlas);
	}
	// A fresh, differently-sized atlas on its own context must not inherit rows or locks.
	PublicMesh other = makeStripGeometry(4, 4);
	xatlasAtlas *atlas = xatlasCreate();
	xatlasMeshDecl decl;
	xatlasMeshDeclInit(&decl);
	decl.vertexCount = (uint32_t)(other.positions.size() / 3);
	decl.vertexPositionData = other.positions.data();
	decl.vertexPositionStride = sizeof(float) * 3;
	decl.indexCount = (uint32_t)other.indices.size();
	decl.indexData = other.indices.data();
	decl.indexFormat = XATLAS_INDEX_FORMAT_UINT32;
	q.check(xatlasAddMesh(atlas, &decl, 0) == XATLAS_ADD_MESH_ERROR_SUCCESS, "isolated AddMesh");
	xatlasChartOptions options;
	xatlasChartOptionsInit(&options);
	options.useLsqr = true;
	xatlasComputeCharts(atlas, &options);
	xatlasPackOptions packOptions;
	xatlasPackOptionsInit(&packOptions);
	xatlasPackCharts(atlas, &packOptions);
	q.check(atlas->chartCount >= 1, "isolated atlas charts");
	q.check(atlas->meshes[0].indexCount == (uint32_t)other.indices.size(), "isolated atlas keeps its own indices");
	xatlasDestroy(atlas);
	return 0;
}
#endif // XATLAS_C_API

} // namespace

void register_task_tests(Registry &registry)
{
	registry.add("rectangular-ls", "full-rank-vs-householder-qr", testRectFullRank);
	registry.add("rectangular-ls", "incompatible-stationary", testRectInconsistent);
	registry.add("rectangular-ls", "duplicate-coefficients-merge", testRectDuplicateCoeffs);
	registry.add("rectangular-ls", "multiple-locks", testRectMultipleLocks);
	registry.add("rectangular-ls", "rank-deficient-stationary", testRectRankDeficient);
	registry.add("rectangular-ls", "zero-column", testRectZeroColumn);
	registry.add("rectangular-ls", "all-locked-rows-residual", testRectAllLocked);
	registry.add("rectangular-ls", "extreme-column-scale", testRectExtremeScale);
	registry.add("rectangular-ls", "locked-only-and-zero-free-rows", testRectLockOnlyAndZeroRows);

	registry.add("stopping", "eta-incompatible", testStopEtaStationarity);
	registry.add("stopping", "rho-consistent", testStopRhoConsistent);
	registry.add("stopping", "iteration-limit", testStopIterationLimit);
	registry.add("stopping", "invalid-input", testStopInvalidInput);
	registry.add("stopping", "breakdown-stagnation", testStopBreakdown);
	registry.add("stopping", "zero-over-zero", testStopZeroOverZero);
	registry.add("stopping", "fresh-start-and-locked-values", testStopFreshStartAndLocks);

	registry.add("lscm-integration", "curved-strip", testLscmCurvedStrip);
	registry.add("lscm-integration", "interior-vertex-patch", testLscmInteriorPatch);
	registry.add("lscm-integration", "rerun-fixed-chart-both-solvers", testLscmRerunFixedChart);
#if XATLAS_C_API
	registry.add("lscm-integration", "public-pipeline-lsqr", testPublicLsqrPipeline);
	registry.add("lscm-integration", "public-pipeline-cg-off", testPublicCgPipeline);
	registry.add("lscm-integration", "paramfunc-takes-precedence", testPublicParamFuncPrecedence);
	registry.add("lscm-integration", "aba-rerun-and-isolation", testPublicAbaRerunIsolation);
#endif
}
