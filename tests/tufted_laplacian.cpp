/****************************************************************************
* Tests for vcg::tri::TuftedOperator and the tufted Harmonic entry point.
*
* Registered through task_support/check.h in four groups:
*   cover     - tufted cover stitching, checked with an independent walk
*   intrinsic - intrinsic Delaunay flips (parallel edges, self loops, ...)
*   operator  - independently reassembled matrices and global invariants
*   harmonic  - production harmonic entry point, constraints and reuse
****************************************************************************/

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <vector>

#include "task_support/check.h"
#include "task_support/mesh.hpp"
#include <vcg/complex/algorithms/harmonic.h>
#include <vcg/complex/algorithms/tufted_laplacian.h>

namespace {

using Op = vcg::tri::TuftedOperator<TMesh>;
using Diag = Op::Diagnostics;

constexpr double kPi = 3.14159265358979323846;

void buildNoTopo(TMesh& m, const std::vector<vcg::Point3d>& p,
                 const std::vector<std::array<int, 3> >& f)
{
	vcg::tri::Allocator<TMesh>::AddVertices(m, p.size());
	vcg::tri::Allocator<TMesh>::AddFaces(m, f.size());
	for (size_t i = 0; i < p.size(); ++i) m.vert[i].P() = p[i];
	for (size_t i = 0; i < f.size(); ++i)
		for (int k = 0; k < 3; ++k) m.face[i].V(k) = &m.vert[f[i][k]];
}

double originalArea(const TMesh& m)
{
	double a = 0.0;
	for (size_t f = 0; f < m.face.size(); ++f)
	{
		const auto p0 = m.face[f].cV(0)->P();
		const auto p1 = m.face[f].cV(1)->P();
		const auto p2 = m.face[f].cV(2)->P();
		a += 0.5 * ((p1 - p0) ^ (p2 - p0)).Norm();
	}
	return a;
}

// Edges of a cover face (with multiplicity), recovered from the diagnostics
// edge-face incidence.
std::vector<int> faceEdges(const Diag& d, int face)
{
	std::vector<int> out;
	for (int e = 0; e < static_cast<int>(d.edgeFaces.size()); ++e)
	{
		if (d.edgeFaces[e].first == face) out.push_back(e);
		if (d.edgeFaces[e].second == face) out.push_back(e);
	}
	return out;
}

// For every corner of a cover face, returns the edge id opposite that corner
// (consumed with multiplicity, so self adjacencies are handled).
std::array<int, 3> cornerOppositeEdges(const Diag& d, int face)
{
	const auto corners = d.faceVertices[face];
	std::vector<int> pool = faceEdges(d, face);
	std::array<int, 3> out = { -1, -1, -1 };
	for (int k = 0; k < 3; ++k)
	{
		const int x = corners[(k + 1) % 3];
		const int y = corners[(k + 2) % 3];
		for (size_t i = 0; i < pool.size(); ++i)
		{
			const auto ep = d.edgeEndpoints[pool[i]];
			const bool match = ((ep.first == x && ep.second == y) ||
			                    (ep.first == y && ep.second == x));
			if (match)
			{
				out[k] = pool[i];
				pool.erase(pool.begin() + static_cast<ptrdiff_t>(i));
				break;
			}
		}
	}
	return out;
}

double angleFromLengths(double adjacentA, double adjacentB, double opposite)
{
	const double c = (adjacentA * adjacentA + adjacentB * adjacentB - opposite * opposite)
	                 / (2.0 * adjacentA * adjacentB);
	return std::acos(std::max(-1.0, std::min(1.0, c)));
}

double heron(double a, double b, double c)
{
	const double s = 0.5 * (a + b + c);
	const double v = s * (s - a) * (s - b) * (s - c);
	return std::sqrt(v > 0.0 ? v : 0.0);
}

// Independent reassembly of the gathered Laplacian and mass, using only the
// exposed diagnostics (edge lengths, edge-face incidence, vertex mapping).
bool reassemble(const Diag& d, int nVerts,
                Eigen::MatrixXd& Lout, Eigen::VectorXd& Mout)
{
	Lout = Eigen::MatrixXd::Zero(nVerts, nVerts);
	Mout = Eigen::VectorXd::Zero(nVerts);
	for (int f = 0; f < static_cast<int>(d.faceVertices.size()); ++f)
	{
		const std::array<int, 3> es = cornerOppositeEdges(d, f);
		for (int k = 0; k < 3; ++k) if (es[k] < 0) return false;
		const double l0 = d.edgeLengths[es[0]];
		const double l1 = d.edgeLengths[es[1]];
		const double l2 = d.edgeLengths[es[2]];
		const double area = heron(l0, l1, l2);
		for (int k = 0; k < 3; ++k)
			Mout[d.faceVertices[f][k]] += area / 3.0;
		for (int k = 0; k < 3; ++k)
		{
			const int e = es[k];
			const int p = d.edgeEndpoints[e].first;
			const int q = d.edgeEndpoints[e].second;
			if (p == q) continue;          // self loops carry zero stiffness
			const double adjacentA = d.edgeLengths[es[(k + 1) % 3]];
			const double adjacentB = d.edgeLengths[es[(k + 2) % 3]];
			const double cot = (adjacentA * adjacentA + adjacentB * adjacentB
			                    - d.edgeLengths[e] * d.edgeLengths[e]) / (4.0 * area);
			const double w = 0.5 * cot;
			Lout(p, q) -= w;
			Lout(q, p) -= w;
			Lout(p, p) += w;
			Lout(q, q) += w;
		}
	}
	Lout *= 0.5;                        // tuft average of the two copies
	Mout *= 0.5;
	return true;
}

// Every cover edge must have two incident faces and two distinct halfedges.
// After intrinsic flips a single face may own both sides of an edge (a self
// adjacent face around an intrinsic self loop), so the two incident faces are
// only required to exist, not to differ.
bool checkCoverStitching(const Diag& d)
{
	if (d.coverFaceCount != d.faceVertices.size()) return false;
	if (3 * d.coverFaceCount != d.coverHalfedgeCount) return false;
	if (2 * d.coverEdgeCount != d.coverHalfedgeCount) return false;

	std::vector<int> faceDegree(d.coverFaceCount, 0);
	for (int e = 0; e < static_cast<int>(d.edgeFaces.size()); ++e)
	{
		const int f1 = d.edgeFaces[e].first;
		const int f2 = d.edgeFaces[e].second;
		if (f1 < 0 || f2 < 0) return false;
		++faceDegree[f1];
		++faceDegree[f2];
		const int h1 = d.edgeHalfedges[e].first;
		const int h2 = d.edgeHalfedges[e].second;
		if (h1 == h2) return false;
		if (d.edgeEndpoints[e].first < 0 || d.edgeEndpoints[e].second < 0)
			return false;
		if (!(d.edgeLengths[e] > 0.0)) return false;
	}
	for (size_t f = 0; f < faceDegree.size(); ++f)
		if (faceDegree[f] != 3) return false;
	return true;
}

// Final Delaunay condition recomputed from intrinsic lengths alone.
bool checkDelaunay(const Diag& d)
{
	for (int e = 0; e < static_cast<int>(d.edgeFaces.size()); ++e)
	{
		double sum = 0.0;
		for (int side = 0; side < 2; ++side)
		{
			const int f = side == 0 ? d.edgeFaces[e].first : d.edgeFaces[e].second;
			const std::array<int, 3> es = cornerOppositeEdges(d, f);
			int idx = -1;
			for (int k = 0; k < 3; ++k) if (es[k] == e) idx = k;
			if (idx < 0) return false;
			const double opp = d.edgeLengths[es[idx]];
			const double a = d.edgeLengths[es[(idx + 1) % 3]];
			const double b = d.edgeLengths[es[(idx + 2) % 3]];
			sum += angleFromLengths(a, b, opp);
			// Strict triangle inequality on every cover triangle.
			if (!(a + b > opp + 1e-10) || !(a + opp > b + 1e-10)
			    || !(b + opp > a + 1e-10)) return false;
		}
		if (sum > kPi + 1e-7) return false;
	}
	return true;
}

int countEdgesBetween(const Diag& d, int a, int b)
{
	int n = 0;
	for (size_t e = 0; e < d.edgeEndpoints.size(); ++e)
		if (d.edgeEndpoints[e].first == a && d.edgeEndpoints[e].second == b)
			++n;
	return n;
}

// ---------------------------------------------------------------- cover ----

int coverThreeFacesEdge()
{
	TMesh m;
	task_mesh(m, { {0,0,0}, {1,0,0}, {.3,1,0}, {.3,0,1}, {.3,-1,0} },
	          { {0,1,2}, {0,1,3}, {0,1,4} });
	Op op(m);
	const Diag d = op.diagnostics();
	TASK_CHECK(d.coverFaceCount == 6u);
	TASK_CHECK(d.coverHalfedgeCount == 18u);
	TASK_CHECK(d.coverEdgeCount == 9u);
	TASK_CHECK(checkCoverStitching(d));
	// The three-face ring becomes three distinct parallel cover edges.
	TASK_CHECK(countEdgesBetween(d, 0, 1) == 3);
	TASK_CHECK(d.vertexMap.size() == 5u);
	// Source mesh untouched.
	TASK_CHECK(m.face.size() == 3 && m.vert.size() == 5);
	return 0;
}

int coverSameWinding()
{
	// Inconsistent winding on the shared edge is explicitly allowed.
	TMesh m;
	task_mesh(m, { {0,0,0}, {1,0,0}, {0,1,0}, {1,1,0} },
	          { {0,1,2}, {1,0,3} });
	Op op(m);
	const Diag d = op.diagnostics();
	TASK_CHECK(d.coverFaceCount == 4u);
	TASK_CHECK(checkCoverStitching(d));
	TASK_CHECK(countEdgesBetween(d, 0, 1) == 2);
	TASK_CHECK(checkDelaunay(d));
	return 0;
}

int coverPointTouchFans()
{
	// Two fans meeting at a single vertex: one component, but no edge coupling
	// between the two sets of tip vertices.
	TMesh m;
	task_mesh(m, { {0,0,0}, {1,0,0}, {0,1,0}, {-1,0,1}, {0,1,1} },
	          { {0,1,2}, {0,3,4} });
	Op op(m);
	const Diag d = op.diagnostics();
	TASK_CHECK(checkCoverStitching(d));
	TASK_CHECK(checkDelaunay(d));
	Eigen::SparseMatrix<double> L = op.laplacian();
	for (int k = 0; k < L.outerSize(); ++k)
		for (Eigen::SparseMatrix<double>::InnerIterator it(L, k); it; ++it)
		{
			const int r = it.row(), c = it.col();
			const bool sideA = (r == 1 || r == 2 || c == 1 || c == 2);
			const bool sideB = (r == 3 || r == 4 || c == 3 || c == 4);
			TASK_CHECK(!(sideA && sideB));
		}
	return 0;
}

int coverObtuseSingle()
{
	// Equilateral single face: no flip is needed, so the initial front/back
	// pairing of every cover edge can be checked directly.
	{
		TMesh m;
		task_mesh(m, { {0,0,0}, {1,0,0}, {.5, 0.8660254037844386,0} },
		          { {0,1,2} });
		Op op(m);
		const Diag d = op.diagnostics();
		TASK_CHECK(op.flipCount() == 0u);
		TASK_CHECK(d.coverFaceCount == 2u);
		TASK_CHECK(checkCoverStitching(d));
		for (size_t e = 0; e < d.edgeFaces.size(); ++e)
		{
			// Every cover edge joins the front and the back copy.
			TASK_CHECK(d.edgeFaces[e].first + d.edgeFaces[e].second == 1);
			TASK_CHECK(std::abs(d.edgeLengths[e]) > 0.0);
		}
	}

	// Obtuse single face: the flow flips, yet all global invariants survive.
	TMesh m;
	task_mesh(m, { {0,0,0}, {2,0,0}, {.1,.01,0} }, { {0,1,2} });
	Op op(m);
	const Diag d = op.diagnostics();
	TASK_CHECK(d.coverFaceCount == 2u);
	TASK_CHECK(checkCoverStitching(d));
	TASK_CHECK(checkDelaunay(d));
	// Angle sums at the cover vertices are preserved by intrinsic flips and
	// equal twice the original planar angles.
	double originalAngles[3] = { 0,0,0 };
	{
		const auto p0 = m.vert[0].P(), p1 = m.vert[1].P(), p2 = m.vert[2].P();
		originalAngles[0] = vcg::Angle(p1 - p0, p2 - p0);
		originalAngles[1] = vcg::Angle(p0 - p1, p2 - p1);
		originalAngles[2] = vcg::Angle(p0 - p2, p1 - p2);
	}
	for (int v = 0; v < 3; ++v)
		TASK_CHECK(std::abs(d.vertexAngleSums[v] - 2.0 * originalAngles[v]) < 1e-8);
	TASK_CHECK(std::abs(d.coverArea - 2.0 * originalArea(m)) < 1e-10);
	return 0;
}

// ------------------------------------------------------------- intrinsic ----

int coverCoincidentNotWelded()
{
	// Spatially coincident but distinct vertices must keep separate identities:
	// the operator must never weld them.
	TMesh m;
	task_mesh(m,
	          { {0,0,0}, {1,0,0}, {0,1,0}, {0,0,0}, {1,0,0}, {1,1,0} },
	          { {0,1,2}, {3,4,5} });
	Op op(m);
	const Diag d = op.diagnostics();
	TASK_CHECK(d.vertexMap.size() == 6u);
	for (size_t v = 0; v < d.vertexMap.size(); ++v)
		TASK_CHECK(d.vertexMap[v] == static_cast<int>(v));
	TASK_CHECK(checkCoverStitching(d));
	const Eigen::SparseMatrix<double> L = op.laplacian();
	TASK_CHECK(L.rows() == 6 && L.cols() == 6);
	// The two components share coordinates but stay algebraically independent.
	for (int r = 0; r < 3; ++r)
		for (int c = 3; c < 6; ++c)
			TASK_CHECK(L.coeff(r, c) == 0.0 && L.coeff(c, r) == 0.0);
	return 0;
}

int intrinsicParallelEdges()
{
	// Two skinny triangles sharing edge (0,1) on opposite sides: both parallel
	// cover edges of the ring must flip and must never be merged.
	TMesh m;
	task_mesh(m, { {0,0,0}, {1,0,0}, {.5,.01,0}, {.5,-.01,0} },
	          { {0,1,2}, {0,1,3} });
	Op op(m);
	const Diag d = op.diagnostics();
	TASK_CHECK(op.flipCount() >= 2u);
	TASK_CHECK(d.coverEdgeCount == 6u);
	TASK_CHECK(checkCoverStitching(d));
	TASK_CHECK(checkDelaunay(d));
	TASK_CHECK(countEdgesBetween(d, 2, 3) == 2);
	TASK_CHECK(op.minEdgeWeight() >= -1e-10);
	return 0;
}

int intrinsicConsecutiveFlips()
{
	TMesh m;
	task_mesh(m, { {0,0,0}, {1,0,0}, {2,0,0}, {.5,.02,0}, {1.5,.02,0} },
	          { {0,1,3}, {1,2,4}, {1,4,3} });
	Op op(m);
	const Diag d = op.diagnostics();
	TASK_CHECK(op.flipCount() >= 2u);
	TASK_CHECK(checkCoverStitching(d));
	TASK_CHECK(checkDelaunay(d));
	// Edge identities preserved: counts never change under flips.
	TASK_CHECK(d.coverFaceCount == 6u && d.coverEdgeCount == 9u);
	for (size_t e = 0; e < d.edgeLengths.size(); ++e)
		TASK_CHECK(std::isfinite(d.edgeLengths[e]) && d.edgeLengths[e] > 0.0);
	return 0;
}

int intrinsicCocircularKept()
{
	// Rectangle diagonal: opposite angles sum exactly to pi, so it is kept.
	TMesh m;
	task_mesh(m, { {0,0,0}, {2,0,0}, {2,1,0}, {0,1,0} },
	          { {0,1,2}, {0,2,3} });
	Op op(m);
	TASK_CHECK(op.flipCount() == 0u);
	const Diag d = op.diagnostics();
	TASK_CHECK(checkDelaunay(d));
	TASK_CHECK(op.minEdgeWeight() >= -1e-10);
	return 0;
}

int intrinsicSelfLoop()
{
	TMesh m;
	task_mesh(m, { {0,0,0}, {2,0,0}, {.1,.01,0} }, { {0,1,2} });
	Op op(m);
	const Diag d = op.diagnostics();
	TASK_CHECK(op.flipCount() >= 1u);
	int selfLoops = 0;
	for (size_t e = 0; e < d.edgeEndpoints.size(); ++e)
		if (d.edgeEndpoints[e].first == d.edgeEndpoints[e].second)
		{
			++selfLoops;
			// A self loop keeps a positive intrinsic length, never zero.
			TASK_CHECK(d.edgeLengths[e] > 1e-12);
		}
	TASK_CHECK(selfLoops >= 1);
	TASK_CHECK(checkDelaunay(d));
	return 0;
}

// -------------------------------------------------------------- operator ----

int operatorReassemblyAndSigns()
{
	TMesh m;
	task_mesh(m, { {0,0,0}, {1,0,0}, {.3,1,0}, {.3,0,1}, {.3,-1,0} },
	          { {0,1,2}, {0,1,3}, {0,1,4} });
	Op op(m);
	const Diag d = op.diagnostics();
	Eigen::MatrixXd Lref;
	Eigen::VectorXd Mref;
	TASK_CHECK(reassemble(d, m.VN(), Lref, Mref));
	const Eigen::MatrixXd L = Eigen::MatrixXd(op.laplacian());
	const Eigen::VectorXd M = op.mass();
	TASK_CHECK(L.rows() == m.VN() && L.cols() == m.VN());
	TASK_CHECK((L - Lref).array().abs().maxCoeff() < 1e-9);
	TASK_CHECK((M - Mref).array().abs().maxCoeff() < 1e-10);
	// Sign convention and zero row sums.
	for (int r = 0; r < m.VN(); ++r)
	{
		TASK_CHECK(std::abs(L.row(r).sum()) < 1e-10);
		TASK_CHECK(L(r, r) >= -1e-12);
		for (int c = 0; c < m.VN(); ++c)
			if (r != c) TASK_CHECK(L(r, c) <= 1e-12);
	}
	TASK_CHECK((L - L.transpose()).array().abs().maxCoeff() < 1e-12);
	return 0;
}

int operatorMassAndComponents()
{
	// Two disjoint components, one of them a point-touch fan pair.
	TMesh m;
	task_mesh(m, { {0,0,0}, {1,0,0}, {0,1,0}, {4,0,0}, {5,0,0}, {4,1,0} },
	          { {0,1,2}, {3,4,5} });
	Op op(m);
	const Eigen::VectorXd M = op.mass();
	// Each component has the same area; its gathered mass equals that area.
	TASK_CHECK(std::abs(M.segment(0, 3).sum() - originalArea(m) / 2.0) < 1e-12);
	TASK_CHECK(std::abs(M.segment(3, 3).sum() - originalArea(m) / 2.0) < 1e-12);
	TASK_CHECK(std::abs(M.sum() - originalArea(m)) < 1e-10);
	// Laplacian blocks are independent.
	const Eigen::MatrixXd L = Eigen::MatrixXd(op.laplacian());
	for (int r = 0; r < 3; ++r)
		for (int c = 3; c < 6; ++c)
			TASK_CHECK(L(r, c) == 0.0 && L(c, r) == 0.0);
	return 0;
}

int operatorScaling()
{
	TMesh m;
	task_mesh(m, { {0,0,0}, {1,0,0}, {.2,.7,0}, {.8,.3,0} },
	          { {0,1,2}, {0,3,1} });
	Op op(m);
	TMesh scaled;
	std::vector<vcg::Point3d> pp;
	for (size_t i = 0; i < m.vert.size(); ++i) pp.push_back(m.vert[i].P() * 3.0);
	task_mesh(scaled, pp, { {0,1,2}, {0,3,1} });
	Op ops(scaled);
	const Eigen::MatrixXd L = Eigen::MatrixXd(op.laplacian());
	const Eigen::MatrixXd Ls = Eigen::MatrixXd(ops.laplacian());
	const Eigen::VectorXd M = op.mass();
	const Eigen::VectorXd Ms = ops.mass();
	TASK_CHECK((L - Ls).array().abs().maxCoeff() < 1e-9);
	TASK_CHECK((Ms - 9.0 * M).array().abs().maxCoeff() < 1e-9);
	return 0;
}

int operatorSourceImmunity()
{
	TMesh m;
	task_mesh(m, { {0,0,0}, {1,0,0}, {.3,1,0}, {.3,0,1} },
	          { {0,1,2}, {0,1,3} });
	Op op(m);
	const Eigen::MatrixXd L0 = Eigen::MatrixXd(op.laplacian());
	const Eigen::VectorXd M0 = op.mass();
	const auto p0 = m.vert[0].P();
	for (size_t i = 0; i < m.vert.size(); ++i) m.vert[i].P() *= 5.0;
	const Eigen::MatrixXd L1 = Eigen::MatrixXd(op.laplacian());
	const Eigen::VectorXd M1 = op.mass();
	TASK_CHECK((L0 - L1).array().abs().maxCoeff() == 0.0);
	TASK_CHECK((M0 - M1).array().abs().maxCoeff() == 0.0);
	TASK_CHECK((m.vert[0].P() - p0 * 5.0).Norm() < 1e-12);
	return 0;
}

int operatorInvalidInputs()
{
	{
		TMesh m;
		buildNoTopo(m, { {0,0,0}, {1,0,0}, {0,1,0}, {7,7,7} }, { {0,1,2} });
		bool threw = false;
		try { Op op(m); } catch (const std::invalid_argument&) { threw = true; }
		TASK_CHECK(threw);
	}
	{
		TMesh m;
		buildNoTopo(m, { {0,0,0}, {1,0,0}, {2,0,0} }, { {0,1,2} });
		bool threw = false;
		try { Op op(m); } catch (const std::invalid_argument&) { threw = true; }
		TASK_CHECK(threw);
	}
	{
		TMesh m;
		buildNoTopo(m, { {0,0,0}, {1,0,0}, {0,1,0} }, { {0,0,1} });
		bool threw = false;
		try { Op op(m); } catch (const std::invalid_argument&) { threw = true; }
		TASK_CHECK(threw);
	}
	{
		TMesh m;
		buildNoTopo(m, { {0,0,0}, {1,0,0},
		                 {std::numeric_limits<double>::quiet_NaN(),1,0} },
		            { {0,1,2} });
		bool threw = false;
		try { Op op(m); } catch (const std::invalid_argument&) { threw = true; }
		TASK_CHECK(threw);
	}
	{
		TMesh m;
		bool threw = false;
		try { Op op(m); } catch (const std::invalid_argument&) { threw = true; }
		TASK_CHECK(threw);
	}
	{
		// A deleted element makes the mesh non compact; the precondition
		// exception must surface as std::invalid_argument.
		TMesh m;
		buildNoTopo(m, { {0,0,0}, {1,0,0}, {0,1,0} }, { {0,1,2} });
		vcg::tri::Allocator<TMesh>::DeleteFace(m, m.face[0]);
		bool threw = false;
		try { Op op(m); } catch (const std::invalid_argument&) { threw = true; }
		TASK_CHECK(threw);
	}
	return 0;
}

// --------------------------------------------------------------- harmonic ----

int harmonicNonManifold()
{
	TMesh m;
	task_mesh(m, { {0,0,0}, {1,0,0}, {.3,1,0}, {.3,0,1}, {.3,-1,0} },
	          { {0,1,2}, {0,1,3}, {0,1,4} });
	Op op(m);
	using H = vcg::tri::Harmonic<TMesh, double>;
	H::ConstraintVec cs;
	cs.push_back({ &m.vert[0], 0.0 });
	cs.push_back({ &m.vert[1], 1.0 });
	auto field = vcg::tri::Allocator<TMesh>::GetPerVertexAttribute<double>(m, "field");
	TASK_CHECK(H::ComputeScalarField(m, cs, field, op));
	const Eigen::SparseMatrix<double> L = op.laplacian();
	Eigen::VectorXd x(m.VN());
	for (int i = 0; i < m.VN(); ++i) x[i] = field[i];
	TASK_CHECK(field[0] == 0.0 && field[1] == 1.0);
	for (int i = 0; i < m.VN(); ++i)
		TASK_CHECK(field[i] >= -1e-10 && field[i] <= 1.0 + 1e-10);
	const Eigen::VectorXd res = L * x;
	for (int i = 2; i < m.VN(); ++i)
		TASK_CHECK(std::abs(res[i]) < 1e-9);
	return 0;
}

int harmonicConstraintErrors()
{
	TMesh m;
	task_mesh(m, { {0,0,0}, {1,0,0}, {0,1,0}, {4,0,0}, {5,0,0}, {4,1,0} },
	          { {0,1,2}, {3,4,5} });
	Op op(m);
	using H = vcg::tri::Harmonic<TMesh, double>;
	auto field = vcg::tri::Allocator<TMesh>::GetPerVertexAttribute<double>(m, "field");

	// Missing anchor on the second component.
	{
		H::ConstraintVec cs;
		cs.push_back({ &m.vert[0], 0.0 });
		cs.push_back({ &m.vert[1], 1.0 });
		for (int i = 0; i < m.VN(); ++i) field[i] = -12345.0;
		bool threw = false;
		try { H::ComputeScalarField(m, cs, field, op); }
		catch (const std::invalid_argument&) { threw = true; }
		TASK_CHECK(threw);
		for (int i = 0; i < m.VN(); ++i) TASK_CHECK(field[i] == -12345.0);
	}
	// Conflicting duplicate constraints.
	{
		H::ConstraintVec cs;
		cs.push_back({ &m.vert[0], 0.0 });
		cs.push_back({ &m.vert[0], 7.0 });
		cs.push_back({ &m.vert[3], 2.0 });
		cs.push_back({ &m.vert[4], 3.0 });
		for (int i = 0; i < m.VN(); ++i) field[i] = -98765.0;
		bool threw = false;
		try { H::ComputeScalarField(m, cs, field, op); }
		catch (const std::invalid_argument&) { threw = true; }
		TASK_CHECK(threw);
		for (int i = 0; i < m.VN(); ++i) TASK_CHECK(field[i] == -98765.0);
	}
	// Repeated but consistent constraints are accepted.
	{
		H::ConstraintVec cs;
		cs.push_back({ &m.vert[0], 0.0 });
		cs.push_back({ &m.vert[0], 0.0 });
		cs.push_back({ &m.vert[1], 1.0 });
		cs.push_back({ &m.vert[3], 4.0 });
		cs.push_back({ &m.vert[4], 5.0 });
		TASK_CHECK(H::ComputeScalarField(m, cs, field, op));
		TASK_CHECK(field[0] == 0.0 && field[3] == 4.0);
	}
	return 0;
}

int harmonicReuseABA()
{
	TMesh m;
	task_mesh(m, { {0,0,0}, {1,0,0}, {.3,1,0}, {.3,0,1}, {.3,-1,0} },
	          { {0,1,2}, {0,1,3}, {0,1,4} });
	Op op(m);
	using H = vcg::tri::Harmonic<TMesh, double>;
	auto field = vcg::tri::Allocator<TMesh>::GetPerVertexAttribute<double>(m, "field");

	H::ConstraintVec A;
	A.push_back({ &m.vert[0], 0.0 });
	A.push_back({ &m.vert[1], 1.0 });
	TASK_CHECK(H::ComputeScalarField(m, A, field, op));
	Eigen::VectorXd xa(m.VN());
	for (int i = 0; i < m.VN(); ++i) xa[i] = field[i];

	H::ConstraintVec B;
	B.push_back({ &m.vert[0], 2.0 });
	B.push_back({ &m.vert[2], -3.0 });
	TASK_CHECK(H::ComputeScalarField(m, B, field, op));
	TASK_CHECK(field[0] == 2.0 && field[2] == -3.0);

	// Back to A on the same operator: identical result, then mutate the source
	// mesh coordinates: the owned operator must not change.
	TASK_CHECK(H::ComputeScalarField(m, A, field, op));
	for (int i = 0; i < m.VN(); ++i)
		TASK_CHECK(std::abs(field[i] - xa[i]) < 1e-11);
	for (size_t i = 0; i < m.vert.size(); ++i) m.vert[i].P() *= 2.5;
	TASK_CHECK(H::ComputeScalarField(m, A, field, op));
	for (int i = 0; i < m.VN(); ++i)
		TASK_CHECK(std::abs(field[i] - xa[i]) < 1e-11);
	return 0;
}

int harmonicLegacyEntry()
{
	// The original four parameter entry keeps its spatial cotangent behavior.
	TMesh disk;
	task_mesh(disk, { {0,0,0}, {1,0,0}, {1,1,0}, {0,1,0}, {.3,.4,0} },
	          { {0,1,4}, {1,2,4}, {2,3,4}, {3,0,4} });
	using H = vcg::tri::Harmonic<TMesh, double>;
	H::ConstraintVec constraints;
	for (int i = 0; i < 4; ++i)
		constraints.push_back({ &disk.vert[i], disk.vert[i].P()[0] });
	auto values = vcg::tri::Allocator<TMesh>::GetPerVertexAttribute<double>(disk, "field");
	TASK_CHECK(H::ComputeScalarField(disk, constraints, values));
	TASK_CHECK(std::abs(values[4] - .3) < 1e-6);
	// Biharmonic entry remains available.
	TASK_CHECK(H::ComputeScalarField(disk, constraints, values, true));
	TASK_CHECK(std::isfinite(values[4]));
	return 0;
}

} // anonymous namespace

const struct task_case task_cases[] = {
	{ "cover", "three_faces_edge", coverThreeFacesEdge },
	{ "cover", "same_winding", coverSameWinding },
	{ "cover", "point_touch_fans", coverPointTouchFans },
	{ "cover", "obtuse_single", coverObtuseSingle },
	{ "cover", "coincident_not_welded", coverCoincidentNotWelded },

	{ "intrinsic", "parallel_edges", intrinsicParallelEdges },
	{ "intrinsic", "consecutive_flips", intrinsicConsecutiveFlips },
	{ "intrinsic", "cocircular_kept", intrinsicCocircularKept },
	{ "intrinsic", "self_loop", intrinsicSelfLoop },

	{ "operator", "reassembly_and_signs", operatorReassemblyAndSigns },
	{ "operator", "mass_and_components", operatorMassAndComponents },
	{ "operator", "scaling", operatorScaling },
	{ "operator", "source_immunity", operatorSourceImmunity },
	{ "operator", "invalid_inputs", operatorInvalidInputs },

	{ "harmonic", "non_manifold", harmonicNonManifold },
	{ "harmonic", "constraint_errors", harmonicConstraintErrors },
	{ "harmonic", "reuse_aba", harmonicReuseABA },
	{ "harmonic", "legacy_entry", harmonicLegacyEntry },
};
const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
