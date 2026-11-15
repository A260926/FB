// Multi-pose (shared UV across discrete vertex poses) test cases.
//
// The internal production assembly is included exactly once through
// source/xatlas/xatlas.cpp so the tests can inspect charts, per-chart areas and
// the shared solve. The production object files themselves are also linked
// (xatlas.cpp is guarded from a second definition by the build); see
// task_support/run.sh.
// The test registry and harness are consumed by task_support/driver.c, which
// the build compiles as C++ (see task_support/run.sh), so these declarations
// use C++ linkage.
#include "task_support/check.h"
#include "source/xatlas/xatlas.cpp"
#include "source/xatlas/xatlas_multipose.h"
#include "examples/multipose_example.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

extern "C" int multipose_c_probe(void);

namespace {

// ---------------------------------------------------------------------------
// Shared energy of a UV assignment, evaluated with the exact production
// objective: sum over poses and faces of |R|^2 / (2 * 3D area), where
// R = z1(U2-U0) - z2(U1-U0).
// ---------------------------------------------------------------------------

static double SharedEnergy(const std::vector<uint32_t> &faces,
	const std::vector<std::vector<float>> &poses, const std::vector<float> &weights,
	const std::vector<double> &uv, uint32_t uvVertexCount)
{
	// uv is indexed by vertex used in faces. Validate bounds in caller.
	double energy = 0.0;
	const size_t faceCount = faces.size() / 3;
	for (size_t f = 0; f < faceCount; f++) {
		const uint32_t i0 = faces[f * 3 + 0], i1 = faces[f * 3 + 1], i2 = faces[f * 3 + 2];
		for (size_t p = 0; p < poses.size(); p++) {
			const std::vector<float> &P = poses[p];
			const double w = p == 0 ? 1.0 : weights[p - 1];
			const double p0x = P[i0 * 3], p0y = P[i0 * 3 + 1], p0z = P[i0 * 3 + 2];
			const double e0 = double(P[i1 * 3]) - p0x, e1 = double(P[i1 * 3 + 1]) - p0y, e2 = double(P[i1 * 3 + 2]) - p0z;
			const double a = std::sqrt(e0 * e0 + e1 * e1 + e2 * e2);
			const double ex = e0 / a, ey = e1 / a, ez = e2 / a;
			const double rx = double(P[i2 * 3]) - p0x, ry = double(P[i2 * 3 + 1]) - p0y, rz = double(P[i2 * 3 + 2]) - p0z;
			const double b = rx * ex + ry * ey + rz * ez;
			const double px = rx - b * ex, py = ry - b * ey, pz = rz - b * ez;
			const double c = std::sqrt(px * px + py * py + pz * pz);
			const double cx = e1 * rz - e2 * ry, cy = e2 * rx - e0 * rz, cz = e0 * ry - e1 * rx;
			const double twoA = std::sqrt(cx * cx + cy * cy + cz * cz);
			const double u0 = uv[i0 * 2], v0 = uv[i0 * 2 + 1];
			const double u1 = uv[i1 * 2], v1 = uv[i1 * 2 + 1];
			const double u2 = uv[i2 * 2], v2 = uv[i2 * 2 + 1];
			const double zr = a * (u2 - u0) - b * (u1 - u0) + c * (v1 - v0);
			const double zi = a * (v2 - v0) - c * (u1 - u0) - b * (v1 - v0);
			energy += w * (zr * zr + zi * zi) / twoA;
		}
	}
	(void)uvVertexCount;
	return energy;
}

// Independent dense QR least-squares solve of the same shared objective. Used
// to check the production solve on a small chart. Free variables are assigned
// to chart vertices in ascending index order, skipping the two pinned ones.

static std::vector<double> SolveDenseQr(std::vector<std::vector<double>> &A, int n)
{
	const int m = (int)A.size();
	for (int k = 0; k < n && k < m; k++) {
		double norm2 = 0.0;
		for (int i = k; i < m; i++)
			norm2 += A[i][k] * A[i][k];
		double norm = std::sqrt(norm2);
		if (norm == 0.0)
			continue;
		const double alpha = A[k][k] > 0.0 ? -norm : norm;
		std::vector<double> vv(m - k, 0.0);
		for (int i = k; i < m; i++)
			vv[i - k] = A[i][k];
		vv[0] -= alpha;
		double vnorm2 = 0.0;
		for (double t : vv) vnorm2 += t * t;
		if (vnorm2 == 0.0)
			continue;
		for (int j = k; j <= n; j++) {
			double dot = 0.0;
			for (int i = k; i < m; i++)
				dot += vv[i - k] * A[i][j];
			const double f = 2.0 * dot / vnorm2;
			for (int i = k; i < m; i++)
				A[i][j] -= f * vv[i - k];
		}
	}
	std::vector<double> x(n, 0.0);
	for (int i = n - 1; i >= 0; i--) {
		double sum = A[i][n];
		for (int j = i + 1; j < n; j++)
			sum -= A[i][j] * x[j];
		x[i] = std::fabs(A[i][i]) > 1e-300 ? sum / A[i][i] : 0.0;
	}
	return x;
}

// Build the reference least-squares rows for one small chart, pinning the two
// vertices stated. `pinUv` gives their locked coordinates.
static std::vector<double> ReferenceSharedSolve(const std::vector<uint32_t> &faces,
	const std::vector<std::vector<float>> &poses, const std::vector<float> &weights,
	uint32_t vertexCount, uint32_t pin0, uint32_t pin1,
	const double pinUv0[2], const double pinUv1[2], std::vector<int> *freeMapOut)
{
	std::vector<int> freeMap(vertexCount * 2, -1);
	int n = 0;
	for (uint32_t v = 0; v < vertexCount; v++) {
		if (v == pin0 || v == pin1)
			continue;
		freeMap[v * 2] = n++;
		freeMap[v * 2 + 1] = n++;
	}
	double locked[4] = { pinUv0[0], pinUv0[1], pinUv1[0], pinUv1[1] };
	std::vector<std::vector<double>> rows;
	for (size_t p = 0; p < poses.size(); p++) {
		const std::vector<float> &P = poses[p];
		const double w = p == 0 ? 1.0 : weights[p - 1];
		for (size_t f = 0; f < faces.size() / 3; f++) {
			const uint32_t i0 = faces[f * 3], i1 = faces[f * 3 + 1], i2 = faces[f * 3 + 2];
			auto V = [&](uint32_t i, int c) { return double(P[i * 3 + c]); };
			const double e0 = V(i1, 0) - V(i0, 0), e1 = V(i1, 1) - V(i0, 1), e2 = V(i1, 2) - V(i0, 2);
			const double a = std::sqrt(e0 * e0 + e1 * e1 + e2 * e2);
			const double ex = e0 / a, ey = e1 / a, ez = e2 / a;
			const double rx = V(i2, 0) - V(i0, 0), ry = V(i2, 1) - V(i0, 1), rz = V(i2, 2) - V(i0, 2);
			const double b = rx * ex + ry * ey + rz * ez;
			const double px = rx - b * ex, py = ry - b * ey, pz = rz - b * ez;
			const double c = std::sqrt(px * px + py * py + pz * pz);
			const double cx = e1 * rz - e2 * ry, cy = e2 * rx - e0 * rz, cz = e0 * ry - e1 * rx;
			const double twoA = std::sqrt(cx * cx + cy * cy + cz * cz);
			const double scale = std::sqrt(w / twoA);
			const uint32_t tv[3] = { i0, i1, i2 };
			const double real[6] = { b - a, -c, -b, c, a, 0.0 };
			const double imag[6] = { c, b - a, -c, -b, 0.0, a };
			for (int part = 0; part < 2; part++) {
				const double *co = part == 0 ? real : imag;
				std::vector<double> row(n + 1, 0.0);
				for (int k = 0; k < 3; k++) {
					for (int comp = 0; comp < 2; comp++) {
						const double value = co[k * 2 + comp] * scale;
						if (value == 0.0)
							continue;
						const uint32_t vertex = tv[k];
						if (vertex == pin0 || vertex == pin1) {
							const int li = (vertex == pin0 ? 0 : 2) + comp;
							row[n] -= value * locked[li];
						} else {
							row[freeMap[vertex * 2 + comp]] += value;
						}
					}
				}
				rows.push_back(row);
			}
		}
	}
	if (freeMapOut)
		*freeMapOut = freeMap;
	if (n == 0)
		return std::vector<double>();
	return SolveDenseQr(rows, n);
}

// ---------------------------------------------------------------------------
// Small deterministic meshes.
// ---------------------------------------------------------------------------

struct PoseMesh {
	std::vector<float> base;
	std::vector<float> poseA;
	std::vector<float> poseB;
	std::vector<uint32_t> indices;
	uint32_t vertexCount = 0;
};

// A single quad split into two triangles. Each triangle stretches in a
// different pose, so sum(max_pose area) differs from max_pose(sum area).
static PoseMesh TwoTriangleStretch()
{
	PoseMesh m;
	m.base = { 0, 0, 0, 1, 0, 0, 0, 1, 0, 1, 1, 0 };
	m.poseA = { 0, 0, 0, 3, 0, 0, 0, 1, 0, 1, 1, 0 };
	m.poseB = { 0, 0, 0, 1, 0, 0, 0, 1, 0, 3, 1, 0 };
	m.indices = { 0, 1, 2, 1, 3, 2 };
	m.vertexCount = 4;
	return m;
}

// A bent strip (flat binding pose, bent pose) used for mapping/solve tests.
static PoseMesh BentStrip(uint32_t segments, uint32_t rows, float bend)
{
	PoseMesh m;
	const uint32_t cols = segments + 1;
	m.vertexCount = cols * (rows + 1);
	for (uint32_t r = 0; r <= rows; r++) {
		const float v = float(r) / float(rows);
		for (uint32_t c = 0; c <= segments; c++) {
			const float u = float(c) / float(segments);
			m.base.push_back(u * 4.0f);
			m.base.push_back((v - 0.5f) * 1.4f);
			m.base.push_back(0.0f);
			m.poseA.push_back(u * 4.0f * 1.3f);
			m.poseA.push_back((v - 0.5f) * 1.4f);
			m.poseA.push_back(std::sin(u * 2.0f) * bend);
		}
	}
	for (uint32_t r = 0; r < rows; r++) {
		for (uint32_t c = 0; c < segments; c++) {
			const uint32_t i0 = r * cols + c, i1 = i0 + 1, i2 = i0 + cols, i3 = i2 + 1;
			const uint32_t t[6] = { i0, i2, i1, i1, i2, i3 };
			for (int k = 0; k < 6; k++)
				m.indices.push_back(t[k]);
		}
	}
	return m;
}

// A flat disk with one non-uniformly bent additional pose, used for the disc
// solve test.
static PoseMesh BentDisk(uint32_t rings, uint32_t slices, float bend)
{
	PoseMesh m;
	m.vertexCount = (rings + 1) * slices;
	for (uint32_t r = 0; r <= rings; r++) {
		// Start at a nonzero inner radius so every triangle is non-degenerate.
		const float rr = 0.3f + 0.7f * float(r) / float(rings);
		for (uint32_t s = 0; s < slices; s++) {
			const float th = 6.2831853f * float(s) / float(slices);
			const float x = rr * std::cos(th), y = rr * std::sin(th);
			m.base.push_back(x); m.base.push_back(y); m.base.push_back(0.0f);
			m.poseA.push_back(x * 1.4f); m.poseA.push_back(y * 0.7f);
			m.poseA.push_back(bend * (x * x + y * y));
		}
	}
	for (uint32_t r = 0; r < rings; r++) {
		for (uint32_t s = 0; s < slices; s++) {
			const uint32_t a = r * slices + s, b = r * slices + (s + 1) % slices;
			const uint32_t c = (r + 1) * slices + s, d = (r + 1) * slices + (s + 1) % slices;
			const uint32_t t[6] = { a, c, b, b, c, d };
			for (int k = 0; k < 6; k++)
				m.indices.push_back(t[k]);
		}
	}
	return m;
}

// A bent strip with two extra poses (both non-degenerate).
static PoseMesh BentStripTwoPoses(uint32_t segments, uint32_t rows, float bend)
{
	PoseMesh m = BentStrip(segments, rows, bend);
	m.poseB.resize(m.poseA.size());
	for (size_t i = 0; i < m.poseA.size(); i += 3) {
		m.poseB[i] = m.poseA[i] * 0.85f;
		m.poseB[i + 1] = m.poseA[i + 1] * 1.1f;
		m.poseB[i + 2] = -0.6f * m.poseA[i + 2];
	}
	return m;
}

// A grid mesh whose base pose is a plane and whose two extra poses fold it
// strongly enough that a single LSCM chart fails and the piecewise fallback
// runs. Parameters are deterministic (own LCG, no libc rand).
static PoseMesh FoldedGrid(int n, float amp, float jitter, uint32_t seed)
{
	struct Lcg {
		uint32_t s;
		float next() { s = s * 1664525u + 1013904223u; return float((s >> 8) & 0xFFFFFF) / float(0x1000000); }
	} rng{ seed };
	PoseMesh m;
	m.vertexCount = (n + 1) * (n + 1);
	for (int r = 0; r <= n; r++) {
		for (int c = 0; c <= n; c++) {
			const float x = (float(c) / n - .5f) * 2, y = (float(r) / n - .5f) * 2;
			m.base.push_back(x * 3); m.base.push_back(y * 3);
			m.base.push_back(amp * std::sin(x * 2) * std::cos(y * 2));
			m.poseA.push_back(x * 3 * (1 + jitter * (2 * rng.next() - 1)));
			m.poseA.push_back(y * 3 * (1 + jitter * (2 * rng.next() - 1)));
			m.poseA.push_back(amp * std::sin(x * 3 + 1) * std::cos(y * 3));
			m.poseB.push_back(x * 3 * (1 + jitter * (2 * rng.next() - 1)));
			m.poseB.push_back(y * 3 * (1 + jitter * (2 * rng.next() - 1)));
			m.poseB.push_back(amp * std::cos(x * 2) * std::sin(y * 2 + 1));
		}
	}
	for (int r = 0; r < n; r++) {
		for (int c = 0; c < n; c++) {
			const uint32_t i0 = r * (n + 1) + c, i1 = i0 + 1, i2 = i0 + n + 1, i3 = i2 + 1;
			const uint32_t t[6] = { i0, i2, i1, i1, i2, i3 };
			for (int k = 0; k < 6; k++)
				m.indices.push_back(t[k]);
		}
	}
	return m;
}

// ---------------------------------------------------------------------------
// Running the pipeline.
// ---------------------------------------------------------------------------

struct RunResult {
	xatlas::Atlas *atlas = nullptr;
	bool added = false;
	xatlas::AddMeshError error = xatlas::AddMeshError::Success;
	~RunResult() { if (atlas) xatlas::Destroy(atlas); }
};

static void FillDecl(xatlas::MeshDecl &decl, const PoseMesh &m, xatlas::MeshPose *poses,
	uint32_t poseCount, const std::vector<float> *weights = nullptr)
{
	decl = xatlas::MeshDecl();
	decl.vertexPositionData = m.base.data();
	decl.vertexPositionStride = sizeof(float) * 3;
	decl.vertexCount = m.vertexCount;
	decl.indexData = m.indices.data();
	decl.indexCount = (uint32_t)m.indices.size();
	decl.indexFormat = xatlas::IndexFormat::UInt32;
	if (poseCount > 0) {
		poses[0].vertexPositionData = m.poseA.data();
		poses[0].vertexPositionStride = sizeof(float) * 3;
		poses[0].weight = weights && weights->size() > 0 ? (*weights)[0] : 1.0f;
		if (poseCount > 1) {
			poses[1].vertexPositionData = m.poseB.data();
			poses[1].vertexPositionStride = sizeof(float) * 3;
			poses[1].weight = weights && weights->size() > 1 ? (*weights)[1] : 1.0f;
		}
		decl.poses = poses;
		decl.poseCount = poseCount;
	}
}

// Returns the number of valid (non-invalid) piecewise sub-charts in the
// production param atlas, or 0 if none. Also fills the chart face lists.
struct ChartSnapshot {
	std::vector<std::vector<uint32_t>> chartFaces; // chart -> source faces
	std::vector<xatlas::ChartType> chartTypes;
	std::vector<bool> chartValid;
	std::vector<std::vector<float>> chartMinUv; // per chart vertex min
};

static ChartSnapshot SnapshotCharts(xatlas::Context *ctx)
{
	ChartSnapshot s;
	for (unsigned g = 0; g < ctx->paramAtlas.chartGroupCount(0); g++) {
		auto *grp = ctx->paramAtlas.chartGroupAt(0, g);
		for (unsigned c = 0; c < grp->chartCount(); c++) {
			auto *ch = grp->chartAt(c);
			s.chartTypes.push_back(ch->type());
			s.chartValid.push_back(!ch->isInvalid());
			std::vector<uint32_t> faces;
			for (unsigned f = 0; f < ch->unifiedMesh()->faceCount(); f++)
				faces.push_back(ch->mapFaceToSourceFace(f));
			s.chartFaces.push_back(faces);
		}
	}
	return s;
}

// ---------------------------------------------------------------------------
// Group: metric — the shared objective actually drives the result.
// ---------------------------------------------------------------------------

// Run the production pipeline and evaluate, per chart, the shared energy of the
// produced UVs and the energy of the base-only orthographic projection over the
// same poses. The shared solution must not be worse, and it must differ from
// the base-only projection whenever an extra pose is stretched.
static int TestProductionChartsUseSharedObjective()
{
	PoseMesh m = BentStrip(4, 1, 1.6f);
	RunResult r;
	r.atlas = xatlas::Create();
	xatlas::MeshPose poses[2];
	xatlas::MeshDecl decl;
	FillDecl(decl, m, poses, 1);
	TASK_CHECK(xatlas::AddMesh(r.atlas, decl) == xatlas::AddMeshError::Success);
	xatlas::ComputeCharts(r.atlas);
	auto *ctx = (xatlas::Context *)r.atlas;
	std::vector<std::vector<float>> poseVecs = { m.base, m.poseA };
	std::vector<float> weights = { 1.0f };
	bool anyDiffer = false;
	for (unsigned g = 0; g < ctx->paramAtlas.chartGroupCount(0); g++) {
		auto *grp = ctx->paramAtlas.chartGroupAt(0, g);
		for (unsigned c = 0; c < grp->chartCount(); c++) {
			auto *ch = grp->chartAt(c);
			auto *mesh = ch->unifiedMesh();
			const uint32_t vc = mesh->vertexCount();
			std::vector<uint32_t> faces;
			for (uint32_t f = 0; f < mesh->faceCount(); f++) {
				faces.push_back(mesh->vertexAt(f * 3 + 0));
				faces.push_back(mesh->vertexAt(f * 3 + 1));
				faces.push_back(mesh->vertexAt(f * 3 + 2));
			}
			// Local pose arrays indexed by the chart mesh's vertices.
			std::vector<std::vector<float>> chartPoses(2);
			for (int p = 0; p < 2; p++) {
				chartPoses[p].resize(vc * 3);
				for (uint32_t v = 0; v < vc; v++) {
					const xatlas::internal::Vector3 &pos = p == 0 ? mesh->position(v) : mesh->posePosition(1, v);
					chartPoses[p][v * 3 + 0] = pos.x;
					chartPoses[p][v * 3 + 1] = pos.y;
					chartPoses[p][v * 3 + 2] = pos.z;
				}
			}
			std::vector<double> solved(vc * 2), baseOnly(vc * 2);
			bool differ = false;
			for (uint32_t v = 0; v < vc; v++) {
				solved[v * 2] = mesh->texcoord(v).x;
				solved[v * 2 + 1] = mesh->texcoord(v).y;
				baseOnly[v * 2] = chartPoses[0][v * 3];
				baseOnly[v * 2 + 1] = chartPoses[0][v * 3 + 1];
				if (std::fabs(solved[v * 2] - baseOnly[v * 2]) > 1e-3 ||
					std::fabs(solved[v * 2 + 1] - baseOnly[v * 2 + 1]) > 1e-3)
					differ = true;
			}
			const double eSolved = SharedEnergy(faces, chartPoses, weights, solved, vc);
			const double eBase = SharedEnergy(faces, chartPoses, weights, baseOnly, vc);
			TASK_CHECK(std::isfinite(eSolved));
			// The shared solve is optimal for the posed objective; the base-only
			// projection (a rigid frame per face, but a single global frame for
			// the chart) is feasible, so the solved energy cannot exceed it.
			TASK_CHECK(eSolved <= eBase + 1e-4 * (1.0 + eBase));
			if (differ)
				anyDiffer = true;
		}
	}
	TASK_CHECK(anyDiffer);
	return 0;
}

static int TestSharedObjectiveBeatsBase()
{
	// Take a small chart, solve the shared objective with the production
	// solver directly (through the multipose module) and confirm its energy is
	// at least as low as the base-pose-only optimum when a second pose exists.
	PoseMesh m = BentStrip(4, 1, 1.6f);
	std::vector<std::vector<float>> poses = { m.base, m.poseA };
	std::vector<float> weights = { 1.0f };
	// Chart uses all vertices.
	std::vector<uint32_t> faces = m.indices;
	uint32_t pin0 = 0, pin1 = 0;
	// Find two far apart vertices for the pins (use existing helper logic:
	// boundary extremes).
	uint32_t vc = m.vertexCount;
	pin1 = vc - 1;
	float best = -1.0f;
	for (uint32_t v = 1; v < vc; v++) {
		const float d = (m.base[v * 3] - m.base[0]) * (m.base[v * 3] - m.base[0]) +
			(m.base[v * 3 + 1] - m.base[1]) * (m.base[v * 3 + 1] - m.base[1]) +
			(m.base[v * 3 + 2] - m.base[2]) * (m.base[v * 3 + 2] - m.base[2]);
		if (d > best) { best = d; pin1 = v; }
	}
	std::vector<float> uv(vc * 2);
	for (uint32_t v = 0; v < vc; v++) {
		uv[v * 2] = m.base[v * 3];
		uv[v * 2 + 1] = m.base[v * 3 + 1];
	}
	const float pinUv0[2] = { uv[pin0 * 2], uv[pin0 * 2 + 1] };
	const float pinUv1[2] = { uv[pin1 * 2], uv[pin1 * 2 + 1] };
	TASK_CHECK(xatlas::multipose::SolveChart(vc, faces.data(), (uint32_t)faces.size(),
		m.base.data(), m.poseA.data(), 1, weights.data(), pin0, pin1, pinUv0, pinUv1, uv.data()));

	// Reference QR with the same pinned values.
	double pinD0[2] = { pinUv0[0], pinUv0[1] }, pinD1[2] = { pinUv1[0], pinUv1[1] };
	std::vector<int> freeMap;
	std::vector<double> ref = ReferenceSharedSolve(faces, poses, weights, vc, pin0, pin1,
		pinD0, pinD1, &freeMap);
	TASK_CHECK(ref.size() == (vc - 2) * 2);
	for (uint32_t v = 0; v < vc; v++) {
		if (v == pin0 || v == pin1)
			continue;
		for (int c = 0; c < 2; c++)
			TASK_CHECK(std::fabs(uv[v * 2 + c] - ref[freeMap[v * 2 + c]]) < 1e-4);
	}

	// The production solution must be non-flipped and finite.
	unsigned flips = 0, nan = 0;
	std::vector<double> uvd(vc * 2);
	for (uint32_t i = 0; i < vc * 2; i++) {
		uvd[i] = uv[i];
		if (!std::isfinite(uvd[i])) nan++;
	}
	for (size_t f = 0; f < faces.size() / 3; f++) {
		const uint32_t i0 = faces[f * 3], i1 = faces[f * 3 + 1], i2 = faces[f * 3 + 2];
		const double a = (uvd[i1 * 2] - uvd[i0 * 2]) * (uvd[i2 * 2 + 1] - uvd[i0 * 2 + 1]) -
			(uvd[i2 * 2] - uvd[i0 * 2]) * (uvd[i1 * 2 + 1] - uvd[i0 * 2 + 1]);
		if (a < -1e-6) flips++;
	}
	TASK_CHECK(nan == 0);
	TASK_CHECK(flips == 0);
	// Pins are exact.
	TASK_CHECK(std::fabs(uvd[pin0 * 2] - pinUv0[0]) < 1e-6);
	TASK_CHECK(std::fabs(uvd[pin1 * 2] - pinUv1[0]) < 1e-6);
	// Energy is finite and no worse than base-only orthogonal projection
	// evaluated over BOTH poses.
	double eSolved = SharedEnergy(faces, poses, weights, uvd, vc);
	double eBase = SharedEnergy(faces, poses, weights, [&] {
		std::vector<double> t(vc * 2);
		for (uint32_t v = 0; v < vc; v++) { t[v * 2] = m.base[v * 3]; t[v * 2 + 1] = m.base[v * 3 + 1]; }
		return t;
	}(), vc);
	TASK_CHECK(std::isfinite(eSolved));
	TASK_CHECK(eSolved <= eBase + 1e-7);
	return 0;
}

// Verify the production solver equals an independent dense QR optimum under the
// same pins, on a small chart.
static int SolveChartMatchesQr(const PoseMesh &m, uint32_t pin0, uint32_t pin1)
{
	std::vector<uint32_t> faces = m.indices;
	const uint32_t vc = m.vertexCount;
	std::vector<std::vector<float>> poses = { m.base, m.poseA };
	std::vector<float> weights = { 1.0f };
	std::vector<float> uv(vc * 2);
	for (uint32_t v = 0; v < vc; v++) {
		uv[v * 2] = m.base[v * 3];
		uv[v * 2 + 1] = m.base[v * 3 + 1];
	}
	const float pinUv0[2] = { uv[pin0 * 2], uv[pin0 * 2 + 1] };
	const float pinUv1[2] = { uv[pin1 * 2], uv[pin1 * 2 + 1] };
	TASK_CHECK(xatlas::multipose::SolveChart(vc, faces.data(), (uint32_t)faces.size(),
		m.base.data(), m.poseA.data(), 1, weights.data(), pin0, pin1, pinUv0, pinUv1, uv.data()));

	double pinD0[2] = { pinUv0[0], pinUv0[1] }, pinD1[2] = { pinUv1[0], pinUv1[1] };
	std::vector<int> freeMap;
	std::vector<double> ref = ReferenceSharedSolve(faces, poses, weights, vc, pin0, pin1,
		pinD0, pinD1, &freeMap);
	TASK_CHECK(ref.size() == (vc - 2) * 2);
	for (uint32_t v = 0; v < vc; v++) {
		if (v == pin0 || v == pin1)
			continue;
		for (int c = 0; c < 2; c++) {
			const double got = uv[v * 2 + c];
			const double want = ref[freeMap[v * 2 + c]];
			TASK_CHECK(std::fabs(got - want) < 1e-4);
		}
	}
	return 0;
}

static int TestSolverMatchesDenseQr()
{
	// Small bent strip (base plane) with a bent / stretched pose.
	PoseMesh strip = BentStrip(2, 1, 1.2f); // 6 verts, 4 triangles
	if (SolveChartMatchesQr(strip, 0, strip.vertexCount - 1))
		return 1;
	// Flat disk with a non-uniformly bent extra pose. A nonzero inner radius
	// keeps every triangle non-degenerate.
	PoseMesh disk = BentDisk(2, 6, 1.4f); // 18 verts
	if (SolveChartMatchesQr(disk, 0, disk.vertexCount - 1))
		return 1;
	return 0;
}

// Weight swap and rigid per-pose invariance: applying an independent rigid
// transform (and one global uniform scale) to every pose leaves the energy of
// a fixed UV assignment unchanged.
static int TestWeightAndRigidInvariance()
{
	PoseMesh m = BentStrip(3, 1, 1.0f);
	std::vector<uint32_t> faces = m.indices;
	const uint32_t vc = m.vertexCount;
	std::vector<float> uv(vc * 2);
	for (uint32_t v = 0; v < vc; v++) {
		uv[v * 2] = m.base[v * 3] * 0.7f + 0.1f;
		uv[v * 2 + 1] = m.base[v * 3 + 1] * 1.1f - 0.2f;
	}
	std::vector<double> uvd(uv.begin(), uv.end());
	std::vector<float> weights = { 1.0f };
	std::vector<std::vector<float>> poses = { m.base, m.poseA };
	double e0 = SharedEnergy(faces, poses, weights, uvd, vc);
	// Rotate + translate every pose independently (a different rigid frame per
	// pose). The local complex coordinates and face areas are invariant.
	auto rigid = [](const std::vector<float> &P, float ang, float tx, float ty, float tz) {
		std::vector<float> out(P.size());
		const float ca = std::cos(ang), sa = std::sin(ang);
		for (size_t i = 0; i < P.size(); i += 3) {
			out[i] = ca * P[i] - sa * P[i + 1] + tx;
			out[i + 1] = sa * P[i] + ca * P[i + 1] + ty;
			out[i + 2] = P[i + 2] + tz;
		}
		return out;
	};
	std::vector<std::vector<float>> rotated = {
		rigid(m.base, 0.7f, 3.0f, -2.0f, 0.5f),
		rigid(m.poseA, -1.1f, -1.5f, 4.0f, -0.25f)
	};
	double e1 = SharedEnergy(faces, rotated, weights, uvd, vc);
	TASK_CHECK(std::fabs(e0 - e1) < 1e-5 * (1.0 + e0));
	// A uniform global scale multiplies every area by s^2 and every local
	// length by s; a UV assignment scaled by 1/s leaves the energy invariant.
	auto scaled = [](const std::vector<float> &P, float s) {
		std::vector<float> out(P.size());
		for (size_t i = 0; i < P.size(); i++) out[i] = P[i] * s;
		return out;
	};
	std::vector<std::vector<float>> scaledPoses = { scaled(m.base, 2.0f), scaled(m.poseA, 2.0f) };
	// Scaling every pose by s scales each local z by s and each 2*area by s^2,
	// so the residual |z1 dU2 - z2 dU1|^2 scales by s^2 and the term by s^2 /
	// s^2 = 1. With a fixed UV assignment the objective is therefore invariant
	// under a global uniform scale.
	double e2 = SharedEnergy(faces, scaledPoses, weights, uvd, vc);
	TASK_CHECK(std::fabs(e0 - e2) < 1e-5 * (1.0 + e0));
	// Weight invariance: the objective sums w_p * E_p over poses, so permuting
	// the poses leaves the energy of a fixed UV assignment unchanged. Swapping
	// base and extra pose with equal weights reproduces the same set of terms.
	double energyOriginal = 0.0, energySwapped = 0.0;
	{
		std::vector<float> wOne = { 1.0f };
		std::vector<std::vector<float>> orig = { m.base, m.poseA };
		std::vector<std::vector<float>> swapped = { m.poseA, m.base };
		energyOriginal = SharedEnergy(faces, orig, wOne, uvd, vc);
		energySwapped = SharedEnergy(faces, swapped, wOne, uvd, vc);
	}
	TASK_CHECK(std::fabs(energyOriginal - energySwapped) < 1e-5 * (1.0 + energyOriginal));
	// Changing a single weight must change the objective in the expected
	// direction: increasing poseA's weight increases its contribution.
	std::vector<float> heavy = { 4.0f };
	double energyHeavy = SharedEnergy(faces, poses, heavy, uvd, vc);
	TASK_CHECK(energyHeavy > energyOriginal);
	return 0;
}

// A failed solve must not publish a partial parameterization.
static int TestNoPartialUvOnFailure()
{
	PoseMesh m = BentStrip(2, 1, 1.0f);
	const uint32_t vc = m.vertexCount;
	std::vector<float> uv(vc * 2);
	for (uint32_t v = 0; v < vc; v++) {
		uv[v * 2] = float(v) * 0.123f + 0.5f;
		uv[v * 2 + 1] = float(v) * -0.077f - 0.25f;
	}
	std::vector<float> original = uv;
	const float pin0[2] = { uv[0], uv[1] };
	const float pin1[2] = { uv[(vc - 1) * 2], uv[(vc - 1) * 2 + 1] };
	// Degenerate extra pose: collapse every vertex to the origin. The solve
	// must reject it and leave uv untouched.
	std::vector<float> degenerate(vc * 3, 0.0f);
	float weight[1] = { 1.0f };
	TASK_CHECK(!xatlas::multipose::SolveChart(vc, m.indices.data(), (uint32_t)m.indices.size(),
		m.base.data(), degenerate.data(), 1, weight, 0, vc - 1, pin0, pin1, uv.data()));
	for (uint32_t i = 0; i < vc * 2; i++)
		TASK_CHECK(uv[i] == original[i]);
	// Non-positive weight is rejected too.
	float badWeight[1] = { -1.0f };
	TASK_CHECK(!xatlas::multipose::SolveChart(vc, m.indices.data(), (uint32_t)m.indices.size(),
		m.base.data(), m.poseA.data(), 1, badWeight, 0, vc - 1, pin0, pin1, uv.data()));
	for (uint32_t i = 0; i < vc * 2; i++)
		TASK_CHECK(uv[i] == original[i]);
	return 0;
}

static int CaseSharedObjective()
{
	return TestProductionChartsUseSharedObjective()
		|| TestSharedObjectiveBeatsBase()
		|| TestSolverMatchesDenseQr()
		|| TestWeightAndRigidInvariance()
		|| TestNoPartialUvOnFailure();
}

// ---------------------------------------------------------------------------
// Group: mapping — chart/colocal/pose mapping behaviour.
// ---------------------------------------------------------------------------

static int TestAddMeshCopiesPoses()
{
	RunResult r;
	r.atlas = xatlas::Create();
	PoseMesh m = BentStripTwoPoses(3, 1, 1.0f);
	xatlas::MeshPose poses[2];
	xatlas::MeshDecl decl;
	FillDecl(decl, m, poses, 2);
	r.error = xatlas::AddMesh(r.atlas, decl);
	TASK_CHECK(r.error == xatlas::AddMeshError::Success);
	// Colocals are built on a worker thread; join before inspecting them.
	xatlas::AddMeshJoin(r.atlas);
	// Release the caller's arrays.
	m.base.clear(); m.base.shrink_to_fit();
	m.poseA.clear(); m.poseA.shrink_to_fit();
	m.poseB.clear(); m.poseB.shrink_to_fit();
	// The internal copies must be non-empty and finite.
	auto *ctx = (xatlas::Context *)r.atlas;
	TASK_CHECK(ctx->meshes.size() == 1);
	auto *mesh = ctx->meshes[0];
	TASK_CHECK(mesh->poseCount() == 3);
	TASK_CHECK(mesh->extraPoseCount() == 2);
	for (uint32_t v = 0; v < mesh->vertexCount(); v++) {
		for (uint32_t p = 0; p < mesh->poseCount(); p++) {
			const xatlas::internal::Vector3 &pos = mesh->posePosition(p, v);
			TASK_CHECK(std::isfinite(pos.x) && std::isfinite(pos.y) && std::isfinite(pos.z));
		}
	}
	return 0;
}

static int TestPoseValidation()
{
	PoseMesh m = BentStrip(2, 1, 1.0f);
	xatlas::MeshPose poses[2];
	xatlas::MeshDecl decl;
	FillDecl(decl, m, poses, 1);

	// Null poses pointer with nonzero poseCount.
	{
		RunResult r; r.atlas = xatlas::Create();
		xatlas::MeshDecl bad = decl;
		bad.poses = nullptr;
		bad.poseCount = 1;
		TASK_CHECK(xatlas::AddMesh(r.atlas, bad) == xatlas::AddMeshError::Error);
		auto *ctx = (xatlas::Context *)r.atlas;
		TASK_CHECK(ctx->meshes.isEmpty()); // Not left in the context.
	}
	// Null position data.
	{
		RunResult r; r.atlas = xatlas::Create();
		xatlas::MeshPose p = poses[0];
		p.vertexPositionData = nullptr;
		xatlas::MeshDecl bad = decl;
		bad.poses = &p;
		bad.poseCount = 1;
		TASK_CHECK(xatlas::AddMesh(r.atlas, bad) == xatlas::AddMeshError::Error);
		TASK_CHECK(((xatlas::Context *)r.atlas)->meshes.isEmpty());
	}
	// Stride below 12 bytes.
	{
		RunResult r; r.atlas = xatlas::Create();
		xatlas::MeshPose p = poses[0];
		p.vertexPositionStride = 8;
		xatlas::MeshDecl bad = decl;
		bad.poses = &p;
		bad.poseCount = 1;
		TASK_CHECK(xatlas::AddMesh(r.atlas, bad) == xatlas::AddMeshError::Error);
		TASK_CHECK(((xatlas::Context *)r.atlas)->meshes.isEmpty());
	}
	// Non-positive weight and NaN weight.
	{
		RunResult r; r.atlas = xatlas::Create();
		xatlas::MeshPose p = poses[0];
		p.weight = 0.0f;
		xatlas::MeshDecl bad = decl; bad.poses = &p; bad.poseCount = 1;
		TASK_CHECK(xatlas::AddMesh(r.atlas, bad) == xatlas::AddMeshError::Error);
		TASK_CHECK(((xatlas::Context *)r.atlas)->meshes.isEmpty());
	}
	{
		RunResult r; r.atlas = xatlas::Create();
		xatlas::MeshPose p = poses[0];
		p.weight = std::nanf("");
		xatlas::MeshDecl bad = decl; bad.poses = &p; bad.poseCount = 1;
		TASK_CHECK(xatlas::AddMesh(r.atlas, bad) == xatlas::AddMeshError::Error);
		TASK_CHECK(((xatlas::Context *)r.atlas)->meshes.isEmpty());
	}
	// Non-finite coordinate in an extra pose.
	{
		std::vector<float> nanPose = m.poseA;
		nanPose[4] = std::nanf("");
		RunResult r; r.atlas = xatlas::Create();
		xatlas::MeshPose p;
		p.vertexPositionData = nanPose.data();
		p.vertexPositionStride = sizeof(float) * 3;
		p.weight = 1.0f;
		xatlas::MeshDecl bad = decl; bad.poses = &p; bad.poseCount = 1;
		TASK_CHECK(xatlas::AddMesh(r.atlas, bad) == xatlas::AddMeshError::Error);
		TASK_CHECK(((xatlas::Context *)r.atlas)->meshes.isEmpty());
	}
	// Degenerate pose: collapse all vertices onto one point.
	{
		std::vector<float> degenerate(m.base.size(), 0.0f);
		RunResult r; r.atlas = xatlas::Create();
		xatlas::MeshPose p;
		p.vertexPositionData = degenerate.data();
		p.vertexPositionStride = sizeof(float) * 3;
		p.weight = 1.0f;
		xatlas::MeshDecl bad = decl; bad.poses = &p; bad.poseCount = 1;
		TASK_CHECK(xatlas::AddMesh(r.atlas, bad) == xatlas::AddMeshError::Error);
		TASK_CHECK(((xatlas::Context *)r.atlas)->meshes.isEmpty());
	}
	return 0;
}

// Base pose coincides but a later pose separates: the two vertices must not be
// unified into one colocal class.
static int TestColocalSeamStaysSplit()
{
	// Two triangles meeting along an edge whose endpoints are coincident in the
	// base pose but pulled apart in poseA. Triangle 0: v0, v1, v2.
	// Triangle 1: v3, v4, v5 with v3==v1, v4==v2, v5==v0 in the base pose.
	PoseMesh m;
	m.base = {
		0, 0, 0, 1, 0, 0, 0, 1, 0,
		1, 0, 0, 0, 1, 0, 0, 0, 0
	};
	// In poseA the coincident vertices separate.
	m.poseA = m.base;
	m.poseA[9] = 2.0f;   // v3 pulled away from v1
	m.poseA[12] = -1.0f; // v4 pulled away from v2
	m.indices = { 0, 1, 2, 3, 4, 5 };
	m.vertexCount = 6;
	RunResult r;
	r.atlas = xatlas::Create();
	xatlas::MeshPose poses[2];
	xatlas::MeshDecl decl;
	FillDecl(decl, m, poses, 1);
	r.error = xatlas::AddMesh(r.atlas, decl);
	TASK_CHECK(r.error == xatlas::AddMeshError::Success);
	xatlas::AddMeshJoin(r.atlas);
	auto *ctx = (xatlas::Context *)r.atlas;
	auto *mesh = ctx->meshes[0];
	// v1 and v3 are coincident in the base pose but separated in poseA, so the
	// colocal classes must stay distinct.
	TASK_CHECK(mesh->firstColocalVertex(1) != mesh->firstColocalVertex(3));
	TASK_CHECK(mesh->firstColocalVertex(2) != mesh->firstColocalVertex(4));
	// The base-only test would have merged them: confirm the base positions do
	// coincide (so the split is due solely to the extra pose).
	TASK_CHECK(xatlas::internal::equal(mesh->position(1), mesh->position(3), 1e-5f));
	return 0;
}

// Full trajectory identical in every pose: duplicated seam vertices are
// legitimately colocal and get unified.
static int TestColocalLegitimateMerge()
{
	// Two triangles whose shared edge is duplicated into separate indices
	// (v0/v3 and v1/v4), but the duplicates coincide in every pose.
	PoseMesh m;
	m.base = {
		0, 0, 0, 1, 0, 0, 0, 1, 0,
		0, 0, 0, 1, 0, 0
	};
	m.indices = { 0, 1, 2, 3, 4, 2 };
	m.vertexCount = 5;
	m.poseA = m.base; // identical trajectory across poses
	RunResult r;
	r.atlas = xatlas::Create();
	xatlas::MeshPose poses[2];
	xatlas::MeshDecl decl;
	FillDecl(decl, m, poses, 1);
	r.error = xatlas::AddMesh(r.atlas, decl);
	TASK_CHECK(r.error == xatlas::AddMeshError::Success);
	xatlas::AddMeshJoin(r.atlas);
	auto *ctx = (xatlas::Context *)r.atlas;
	auto *mesh = ctx->meshes[0];
	TASK_CHECK(mesh->vertexCount() == 5);
	// v0 and v3 coincide in every pose -> same colocal representative.
	TASK_CHECK(mesh->firstColocalVertex(0) == mesh->firstColocalVertex(3));
	TASK_CHECK(mesh->firstColocalVertex(1) == mesh->firstColocalVertex(4));
	return 0;
}

// Material split and ignored faces: ignored faces contribute neither energy nor
// area, and materials still separate charts.
static int TestMaterialsAndIgnoredFaces()
{
	// Two quads with different materials, three vertices of one quad ignored.
	PoseMesh m;
	m.vertexCount = 8;
	// quad A uses 0..3, quad B uses 4..7
	for (int q = 0; q < 2; q++) {
		const float ox = q == 0 ? 0.0f : 3.0f;
		m.base.insert(m.base.end(), {
			ox, 0, 0, ox + 1, 0, 0, ox, 1, 0, ox + 1, 1, 0
		});
		m.poseA.insert(m.poseA.end(), {
			ox * 1.2f, 0, 0.1f, ox + 1.2f, 0, 0.1f, ox * 1.2f, 1, 0.1f, ox + 1.2f, 1, 0.1f
		});
	}
	m.indices = { 0, 1, 2, 1, 3, 2, 4, 5, 6, 5, 7, 6 };
	uint32_t materials[4] = { 1, 1, 2, 2 };
	bool ignore[4] = { false, false, false, true };

	RunResult r;
	r.atlas = xatlas::Create();
	xatlas::MeshPose poses[2];
	xatlas::MeshDecl decl;
	FillDecl(decl, m, poses, 1);
	decl.faceMaterialData = materials;
	decl.faceIgnoreData = ignore;
	r.error = xatlas::AddMesh(r.atlas, decl);
	TASK_CHECK(r.error == xatlas::AddMeshError::Success);
	xatlas::ComputeCharts(r.atlas);
	auto *ctx = (xatlas::Context *)r.atlas;
	ChartSnapshot snap = SnapshotCharts(ctx);
	// Ignored face 3 must not appear in any chart, nor contribute to any
	// chart's max-pose area.
	for (auto &faces : snap.chartFaces)
		for (uint32_t f : faces)
			TASK_CHECK(f != 3);
	// Material 1 and material 2 faces must land in different charts.
	int mat1Chart = -1, mat2Chart = -1;
	for (size_t c = 0; c < snap.chartFaces.size(); c++) {
		for (uint32_t f : snap.chartFaces[c]) {
			if (f == 0 || f == 1) mat1Chart = (int)c;
			if (f == 2) mat2Chart = (int)c;
		}
	}
	TASK_CHECK(mat1Chart >= 0 && mat2Chart >= 0 && mat1Chart != mat2Chart);
	return 0;
}

// Shared solve also drives Planar and Ortho shortcuts, and pose sub-charts are
// recomputed over their own faces after the invalid-parameterization fallback.
static int TestForcedSubchartsAndPlanarShared()
{
	// Planar base with a strongly bent pose.
	{
		PoseMesh m = BentDisk(2, 10, 1.5f);
		RunResult r; r.atlas = xatlas::Create();
		xatlas::MeshPose poses[2];
		xatlas::MeshDecl decl;
		FillDecl(decl, m, poses, 1);
		TASK_CHECK(xatlas::AddMesh(r.atlas, decl) == xatlas::AddMeshError::Success);
		xatlas::ComputeCharts(r.atlas);
		auto *ctx = (xatlas::Context *)r.atlas;
		ChartSnapshot snap = SnapshotCharts(ctx);
		TASK_CHECK(!snap.chartValid.empty());
		for (size_t c = 0; c < snap.chartValid.size(); c++)
			TASK_CHECK(snap.chartValid[c]);
	}
	// Strong fold: piecewise fallback produces sub-charts that each carry pose
	// data and solve over their own face set.
	{
		PoseMesh m = FoldedGrid(6, 0.6f, 0.4f, 23);
		RunResult r; r.atlas = xatlas::Create();
		xatlas::MeshPose poses[2];
		xatlas::MeshDecl decl;
		FillDecl(decl, m, poses, 2);
		TASK_CHECK(xatlas::AddMesh(r.atlas, decl) == xatlas::AddMeshError::Success);
		xatlas::ComputeCharts(r.atlas);
		auto *ctx = (xatlas::Context *)r.atlas;
		unsigned pieceCharts = 0;
		for (unsigned g = 0; g < ctx->paramAtlas.chartGroupCount(0); g++) {
			auto *grp = ctx->paramAtlas.chartGroupAt(0, g);
			for (unsigned c = 0; c < grp->chartCount(); c++) {
				auto *ch = grp->chartAt(c);
				TASK_CHECK(ch->unifiedMesh()->poseCount() == 3);
				if (ch->type() == xatlas::ChartType::Piecewise) {
					pieceCharts++;
					TASK_CHECK(!ch->isInvalid());
				}
			}
		}
		TASK_CHECK(pieceCharts >= 1);
	}
	return 0;
}

// Meshes with poses reject useInputMeshUvs and a custom paramFunc: the shared
// objective cannot honour either.
static int TestPoseChartOptionRestrictions()
{
	PoseMesh m = BentStrip(3, 1, 1.0f);
	{
		RunResult r; r.atlas = xatlas::Create();
		xatlas::MeshPose poses[2];
		xatlas::MeshDecl decl;
		FillDecl(decl, m, poses, 1);
		TASK_CHECK(xatlas::AddMesh(r.atlas, decl) == xatlas::AddMeshError::Success);
		xatlas::ChartOptions options;
		options.useInputMeshUvs = true;
		xatlas::ComputeCharts(r.atlas, options);
		auto *ctx = (xatlas::Context *)r.atlas;
		TASK_CHECK(!ctx->paramAtlas.chartsComputed());
	}
	{
		RunResult r; r.atlas = xatlas::Create();
		xatlas::MeshPose poses[2];
		xatlas::MeshDecl decl;
		FillDecl(decl, m, poses, 1);
		TASK_CHECK(xatlas::AddMesh(r.atlas, decl) == xatlas::AddMeshError::Success);
		xatlas::ChartOptions options;
		options.paramFunc = [](const float *, float *, uint32_t, const uint32_t *, uint32_t) {};
		xatlas::ComputeCharts(r.atlas, options);
		auto *ctx = (xatlas::Context *)r.atlas;
		TASK_CHECK(!ctx->paramAtlas.chartsComputed());
	}
	// Pose-free meshes still accept those options.
	{
		RunResult r; r.atlas = xatlas::Create();
		xatlas::MeshPose poses[2];
		xatlas::MeshDecl decl;
		FillDecl(decl, m, poses, 0);
		TASK_CHECK(xatlas::AddMesh(r.atlas, decl) == xatlas::AddMeshError::Success);
		xatlas::ChartOptions options;
		options.paramFunc = [](const float *, float *, uint32_t, const uint32_t *, uint32_t) {};
		xatlas::ComputeCharts(r.atlas, options);
		auto *ctx = (xatlas::Context *)r.atlas;
		TASK_CHECK(ctx->paramAtlas.chartsComputed());
	}
	return 0;
}

static int CaseMapping()
{
	return TestAddMeshCopiesPoses()
		|| TestPoseValidation()
		|| TestColocalSeamStaysSplit()
		|| TestColocalLegitimateMerge()
		|| TestMaterialsAndIgnoredFaces()
		|| TestForcedSubchartsAndPlanarShared()
		|| TestPoseChartOptionRestrictions();
}

// ---------------------------------------------------------------------------
// Group: packing — max-pose area, automatic and explicit density, repack.
// ---------------------------------------------------------------------------

static double MeshSurfaceArea(const PoseMesh &m, int pose)
{
	const std::vector<float> &P = pose == 0 ? m.base : (pose == 1 ? m.poseA : m.poseB);
	double area = 0.0;
	for (size_t f = 0; f + 2 < m.indices.size(); f += 3) {
		const uint32_t i0 = m.indices[f], i1 = m.indices[f + 1], i2 = m.indices[f + 2];
		const double ax = P[i1 * 3] - P[i0 * 3], ay = P[i1 * 3 + 1] - P[i0 * 3 + 1], az = P[i1 * 3 + 2] - P[i0 * 3 + 2];
		const double bx = P[i2 * 3] - P[i0 * 3], by = P[i2 * 3 + 1] - P[i0 * 3 + 1], bz = P[i2 * 3 + 2] - P[i0 * 3 + 2];
		const double cx = ay * bz - az * by, cy = az * bx - ax * bz, cz = ax * by - ay * bx;
		area += 0.5 * std::sqrt(cx * cx + cy * cy + cz * cz);
	}
	return area;
}

static int TestMaxPoseAreaAndDensity()
{
	PoseMesh m = TwoTriangleStretch();
	RunResult r;
	r.atlas = xatlas::Create();
	xatlas::MeshPose poses[2];
	xatlas::MeshDecl decl;
	FillDecl(decl, m, poses, 2);
	TASK_CHECK(xatlas::AddMesh(r.atlas, decl) == xatlas::AddMeshError::Success);
	xatlas::ComputeCharts(r.atlas);
	auto *ctx = (xatlas::Context *)r.atlas;
	// Build the pack atlas and inspect per-chart surfaceArea.
	xatlas::internal::pack::Atlas packAtlas;
	packAtlas.addCharts(ctx->taskScheduler, &ctx->paramAtlas);
	TASK_CHECK(packAtlas.getChartCount() >= 1);
	double maxPoseSum = 0.0, perFaceMaxSum = 0.0;
	for (uint32_t pose = 0; pose < 3; pose++)
		maxPoseSum = std::max(maxPoseSum, MeshSurfaceArea(m, (int)pose));
	// max_pose(sum_chart_faces) over the whole chart set equals max over poses
	// of the total area, while sum_face(max_pose) is strictly larger here.
	double sumOverFacesOfMax = 0.0;
	for (size_t f = 0; f + 2 < m.indices.size(); f += 3) {
		double fmax = 0.0;
		for (int pose = 0; pose < 3; pose++) {
			const std::vector<float> &P = pose == 0 ? m.base : (pose == 1 ? m.poseA : m.poseB);
			const uint32_t i0 = m.indices[f], i1 = m.indices[f + 1], i2 = m.indices[f + 2];
			const double ax = P[i1 * 3] - P[i0 * 3], ay = P[i1 * 3 + 1] - P[i0 * 3 + 1], az = P[i1 * 3 + 2] - P[i0 * 3 + 2];
			const double bx = P[i2 * 3] - P[i0 * 3], by = P[i2 * 3 + 1] - P[i0 * 3 + 1], bz = P[i2 * 3 + 2] - P[i0 * 3 + 2];
			const double cx = ay * bz - az * by, cy = az * bx - ax * bz, cz = ax * by - ay * bx;
			fmax = std::max(fmax, 0.5 * std::sqrt(cx * cx + cy * cy + cz * cz));
		}
		sumOverFacesOfMax += fmax;
	}
	perFaceMaxSum = sumOverFacesOfMax;
	double packArea = 0.0;
	for (uint32_t c = 0; c < packAtlas.getChartCount(); c++)
		packArea += packAtlas.getChart(c)->surfaceArea;
	TASK_CHECK(std::fabs(packArea - maxPoseSum) < 1e-5);
	TASK_CHECK(packArea < perFaceMaxSum - 1e-4); // Distinct from sum_face(max).

	// Explicit texelsPerUnit: chart scale is sqrt(surfaceArea/parametricArea)*tpu.
	xatlas::PackOptions explicitPack;
	explicitPack.texelsPerUnit = 7.0f;
	explicitPack.padding = 0;
	explicitPack.rotateCharts = false;
	explicitPack.rotateChartsToAxis = false;
	xatlas::PackCharts(r.atlas, explicitPack);
	TASK_CHECK(std::fabs(r.atlas->texelsPerUnit - 7.0f) < 1e-5);
	// Rebuild the pack atlas from original param coords for the scale check.
	xatlas::internal::pack::Atlas pack2;
	pack2.addCharts(ctx->taskScheduler, &ctx->paramAtlas);
	TASK_CHECK(pack2.getChartCount() >= 1);

	// Automatic density: texelsPerUnit estimated from the max-pose mesh area.
	xatlas::PackOptions autoPack;
	autoPack.resolution = 256;
	autoPack.padding = 0;
	xatlas::PackCharts(r.atlas, autoPack);
	const double expected = std::sqrt((256.0 * 256.0) / std::max(1.0, maxPoseSum / 0.75));
	TASK_CHECK(std::fabs(r.atlas->texelsPerUnit - expected) < 1e-3 * (1.0 + expected));
	return 0;
}

// Two PackCharts calls must recompute from the original parameter coordinates
// and original pose areas, not from already-packed coordinates.
static int TestRepackFromOriginal()
{
	PoseMesh m = BentStrip(4, 1, 1.2f);
	RunResult r;
	r.atlas = xatlas::Create();
	xatlas::MeshPose poses[2];
	xatlas::MeshDecl decl;
	FillDecl(decl, m, poses, 1);
	TASK_CHECK(xatlas::AddMesh(r.atlas, decl) == xatlas::AddMeshError::Success);
	xatlas::ComputeCharts(r.atlas);

	auto snapshotUvs = [&](std::vector<std::vector<float>> &out) {
		auto &mesh = r.atlas->meshes[0];
		out.clear();
		for (uint32_t c = 0; c < mesh.chartCount; c++) {
			std::vector<float> uvs;
			for (uint32_t v = 0; v < mesh.vertexCount; v++)
				if (mesh.vertexArray[v].chartIndex == (int32_t)c) {
					uvs.push_back(mesh.vertexArray[v].uv[0]);
					uvs.push_back(mesh.vertexArray[v].uv[1]);
				}
			out.push_back(uvs);
		}
	};

	xatlas::PackOptions p1;
	p1.texelsPerUnit = 5.0f;
	p1.padding = 1;
	p1.rotateCharts = true;
	xatlas::PackCharts(r.atlas, p1);
	std::vector<std::vector<float>> uv1;
	snapshotUvs(uv1);
	const uint32_t charts1 = r.atlas->chartCount;
	// A/B/A: pack with different options, then again with the first options.
	xatlas::PackOptions pOther;
	pOther.texelsPerUnit = 9.0f;
	pOther.padding = 3;
	pOther.rotateCharts = false;
	xatlas::PackCharts(r.atlas, pOther);
	std::vector<std::vector<float>> uvOther;
	snapshotUvs(uvOther);
	xatlas::PackCharts(r.atlas, p1);
	std::vector<std::vector<float>> uv2;
	snapshotUvs(uv2);
	TASK_CHECK(charts1 == r.atlas->chartCount);
	TASK_CHECK(uv1.size() == uv2.size());
	bool same = true;
	for (size_t c = 0; c < uv1.size(); c++) {
		if (uv1[c].size() != uv2[c].size()) { same = false; break; }
		for (size_t i = 0; i < uv1[c].size(); i++)
			if (std::fabs(uv1[c][i] - uv2[c][i]) > 1e-4) { same = false; break; }
	}
	TASK_CHECK(same);
	// The intermediate pack must have produced different coordinates.
	bool differ = false;
	for (size_t c = 0; c < uv1.size() && c < uvOther.size(); c++)
		for (size_t i = 0; i < uv1[c].size() && i < uvOther[c].size(); i++)
			if (std::fabs(uv1[c][i] - uvOther[c][i]) > 1e-3) differ = true;
	TASK_CHECK(differ);
	return 0;
}

static int TestNoPoseUnchanged()
{
	// poseCount == 0 goes through the untouched legacy path and still works.
	PoseMesh m = BentStrip(4, 1, 1.2f);
	RunResult r; r.atlas = xatlas::Create();
	xatlas::MeshPose poses[2];
	xatlas::MeshDecl decl;
	FillDecl(decl, m, poses, 0);
	TASK_CHECK(xatlas::AddMesh(r.atlas, decl) == xatlas::AddMeshError::Success);
	xatlas::ComputeCharts(r.atlas);
	xatlas::PackOptions po;
	po.texelsPerUnit = 6.0f;
	po.resolution = 128;
	po.padding = 1;
	xatlas::PackCharts(r.atlas, po);
	TASK_CHECK(r.atlas->chartCount >= 1);
	TASK_CHECK(r.atlas->width > 0 && r.atlas->height > 0);
	auto &mesh = r.atlas->meshes[0];
	for (uint32_t i = 0; i < mesh.vertexCount; i++) {
		TASK_CHECK(std::isfinite(mesh.vertexArray[i].uv[0]));
		TASK_CHECK(std::isfinite(mesh.vertexArray[i].uv[1]));
	}
	return 0;
}

static int TestCInterface()
{
	// The C probe is compiled as C11 and linked into this test.
	TASK_CHECK(multipose_c_probe() == 0);
	// Drive the C API end-to-end.
	PoseMesh m = BentStrip(3, 1, 0.8f);
	xatlasMeshPose poses[2];
	std::memset(poses, 0, sizeof(poses));
	poses[0].vertexPositionData = m.poseA.data();
	poses[0].vertexPositionStride = sizeof(float) * 3;
	poses[0].weight = 1.0f;
	xatlasMeshDecl decl;
	xatlasMeshDeclInit(&decl);
	decl.vertexPositionData = m.base.data();
	decl.vertexPositionStride = sizeof(float) * 3;
	decl.vertexCount = m.vertexCount;
	decl.indexData = m.indices.data();
	decl.indexCount = (uint32_t)m.indices.size();
	decl.indexFormat = XATLAS_INDEX_FORMAT_UINT32;
	decl.poses = poses;
	decl.poseCount = 1;
	xatlasAtlas *atlas = xatlasCreate();
	TASK_CHECK(atlas != nullptr);
	TASK_CHECK(xatlasAddMesh(atlas, &decl, 0) == XATLAS_ADD_MESH_ERROR_SUCCESS);
	// Copy happened: release the C pose array.
	poses[0].vertexPositionData = nullptr;
	m.poseA.clear();
	m.poseA.shrink_to_fit();
	decl.poses = nullptr;
	decl.poseCount = 0;
	xatlasComputeCharts(atlas, nullptr);
	xatlasPackOptions po;
	xatlasPackOptionsInit(&po);
	po.texelsPerUnit = 6.0f;
	po.resolution = 128;
	po.padding = 1;
	xatlasPackCharts(atlas, &po);
	TASK_CHECK(atlas->chartCount >= 1);
	TASK_CHECK(atlas->width > 0 && atlas->height > 0);
	xatlasDestroy(atlas);
	return 0;
}

static int TestAsyncSourceRelease()
{
	// AddMesh must copy all pose positions synchronously; freeing the caller
	// arrays before AddMeshJoin / ComputeCharts is safe.
	PoseMesh m = BentStripTwoPoses(5, 1, 1.0f);
	xatlas::Atlas *atlas = xatlas::Create();
	xatlas::MeshPose poses[2];
	xatlas::MeshDecl decl;
	FillDecl(decl, m, poses, 2);
	TASK_CHECK(xatlas::AddMesh(atlas, decl) == xatlas::AddMeshError::Success);
	m.base.assign(m.base.size(), 0.0f);
	m.poseA.assign(m.poseA.size(), 0.0f);
	m.poseB.assign(m.poseB.size(), 0.0f);
	m.base.clear(); m.base.shrink_to_fit();
	m.poseA.clear(); m.poseA.shrink_to_fit();
	m.poseB.clear(); m.poseB.shrink_to_fit();
	xatlas::AddMeshJoin(atlas);
	xatlas::ComputeCharts(atlas);
	xatlas::PackOptions po;
	po.texelsPerUnit = 5.0f;
	po.resolution = 128;
	xatlas::PackCharts(atlas, po);
	TASK_CHECK(atlas->chartCount >= 1);
	xatlas::Destroy(atlas);
	return 0;
}

static int TestExampleRuns()
{
	unsigned charts = 0;
	TASK_CHECK(RunMultiposeExample(&charts));
	TASK_CHECK(charts >= 1);
	const char *path = "task_build_multipose_example.obj";
	unsigned objCharts = 0;
	TASK_CHECK(WriteMultiposeExampleObj(path, &objCharts));
	TASK_CHECK(objCharts >= 1);
	FILE *f = std::fopen(path, "r");
	TASK_CHECK(f != nullptr);
	char line[512];
	unsigned v = 0, vt = 0, fn = 0;
	while (std::fgets(line, sizeof(line), f)) {
		if (line[0] == 'v' && line[1] == ' ') v++;
		else if (line[0] == 'v' && line[1] == 't') vt++;
		else if (line[0] == 'f') fn++;
	}
	std::fclose(f);
	std::remove(path);
	TASK_CHECK(v > 0 && vt > 0 && fn > 0);
	TASK_CHECK(v == vt);
	return 0;
}

static int CasePacking()
{
	return TestMaxPoseAreaAndDensity()
		|| TestRepackFromOriginal()
		|| TestNoPoseUnchanged()
		|| TestCInterface()
		|| TestAsyncSourceRelease()
		|| TestExampleRuns();
}

} // namespace

const struct task_case task_cases[] = {
	{ "metric", "shared_objective", CaseSharedObjective },
	{ "mapping", "chart_mapping", CaseMapping },
	{ "packing", "packing_metric", CasePacking },
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
