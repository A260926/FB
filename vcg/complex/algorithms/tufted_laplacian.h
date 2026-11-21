/****************************************************************************
* VCGLib                                                            o o     *
* Visual and Computer Graphics Library                            o     o   *
*                                                                _   O  _   *
* Copyright(C) 2024                                                \/)\/    *
* Visual Computing Lab                                            /\/|      *
* ISTI - Italian National Research Council                           |      *
*                                                                    \      *
* All rights reserved.                                                      *
*                                                                           *
* This program is free software; you can redistribute it and/or modify      *
* it under the terms of the GNU General Public License as published by      *
* the Free Software Foundation; either version 2 of the License, or         *
* (at your option) any later version.                                       *
*                                                                           *
****************************************************************************/

#ifndef __VCGLIB_TUFTED_LAPLACIAN
#define __VCGLIB_TUFTED_LAPLACIAN

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <limits>
#include <map>
#include <queue>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <vcg/complex/allocate.h>
#include <vcg/complex/base.h>
#include <Eigen/Sparse>

namespace vcg {
namespace tri {

namespace detail {
static const double kTuftedPi = 3.14159265358979323846;
}

/**
 * @brief Owning tufted intrinsic-Delaunay Laplacian operator.
 *
 * The operator builds an independent intrinsic *tufted cover* of a compact
 * triangle mesh that may be non manifold: every input triangle is duplicated
 * into a front and a back logical copy, and for every undirected input edge the
 * incident faces are ring-stitched into two-sided cover edges. The resulting
 * cover is a closed (every halfedge has exactly one mate) triangle surface
 * that may still have several face fans meeting at a vertex.
 *
 * Intrinsic Delaunay edge flips are then performed on this cover. Edge flips
 * keep edge identity, halfedge pairing and the mapping back to the original
 * vertices: parallel cover edges with identical endpoints stay distinct, and
 * intrinsic edge lengths are never recomputed from coordinates (so a
 * degenerate self loop would never silently acquire a zero length). The final
 * cotangent stiffness matrix and barycentric mass matrix are gathered back
 * into the original vertex space.
 *
 * The constructor takes a const mesh: the source mesh, its attributes and its
 * display coordinates are never modified, and the operator keeps its own copy
 * of every piece of data it needs.
 */
template <class MeshType>
class TuftedOperator
{
public:
	typedef typename MeshType::VertexType VertexType;
	typedef typename MeshType::FaceType   FaceType;
	typedef typename MeshType::ScalarType MeshScalar;

	typedef Eigen::SparseMatrix<double>        SparseMatrixType;
	typedef Eigen::Matrix<double, Eigen::Dynamic, 1> VectorType;

	/**
	 * @brief Snapshot of the current cover state, exposed for independent
	 * verification (two faces per edge, triangle inequalities, angle sums,
	 * total area, ...).
	 */
	struct Diagnostics
	{
		/// Number of intrinsic flips actually performed.
		std::size_t flipCount = 0;
		/// Smallest final per-edge cotangent weight (must be non negative).
		double minEdgeWeight = 0.0;
		/// Number of cover triangles (always 2 * input face count).
		std::size_t coverFaceCount = 0;
		/// Number of cover halfedges (always 6 * input face count).
		std::size_t coverHalfedgeCount = 0;
		/// Number of cover edges (always 3 * input face count).
		std::size_t coverEdgeCount = 0;

		/// Cover vertex -> original vertex index.
		std::vector<int> vertexMap;
		/// For each cover edge: the two paired halfedges (opposite directions).
		std::vector<std::pair<int, int> > edgeHalfedges;
		/// For each cover edge: the two cover faces incident to it.
		std::vector<std::pair<int, int> > edgeFaces;
		/// For each cover edge: its current endpoints (original vertex ids).
		std::vector<std::pair<int, int> > edgeEndpoints;
		/// For each cover edge: its intrinsic length.
		std::vector<double> edgeLengths;
		/// For each cover face: its three corner vertices (original vertex ids).
		std::vector<std::array<int, 3> > faceVertices;
		/// For each cover vertex: sum of incident interior angles.
		std::vector<double> vertexAngleSums;
		/// Total area of the cover (twice the original area).
		double coverArea = 0.0;
	};

	/**
	 * @brief Builds the tufted cover and runs the intrinsic Delaunay flow.
	 * @throws std::invalid_argument on any illegal input.
	 * @throws std::runtime_error if the intrinsic flow stalls before reaching
	 *         a Delaunay configuration (negative weights are never clamped).
	 */
	explicit TuftedOperator(const MeshType& m)
	{
		validate(m);
		buildCover(m);
		runIntrinsicDelaunay();
	}

	/// Number of vertices of the source mesh (and of the produced matrices).
	int vertexCount() const { return nVerts_; }

	/// Number of faces of the source mesh.
	int faceCount() const { return nFaces_; }

	/// Number of intrinsic flips performed during construction.
	std::size_t flipCount() const { return flipCount_; }

	/**
	 * @brief Cotangent stiffness matrix in the original vertex space.
	 *
	 * Standard per-triangle cotangent stiffness on the current intrinsic
	 * cover, gathered through the vertex mapping and divided by two (the two
	 * tuft copies). Sign convention: non negative diagonal, non positive
	 * off diagonal, zero row sums.
	 */
	SparseMatrixType laplacian() const
	{
		SparseMatrixType coverL = assembleCoverLaplacian();
		SparseMatrixType L(nVerts_, nVerts_);
		// The cover vertex space is one-to-one with the original one, but the
		// gathering goes through the explicit mapping as required.
		gatherMatrix(coverL, L);
		return L;
	}

	/**
	 * @brief Barycentric (area thirds) diagonal mass in the original vertex
	 * space, divided by two. The mass of every input connected component
	 * equals its original 3D area.
	 */
	VectorType mass() const
	{
		VectorType coverM = VectorType::Zero(nCoverVerts_);
		for (int f = 0; f < nCoverFaces_; ++f)
		{
			const double a = faceArea(f);
			for (int k = 0; k < 3; ++k)
				coverM[origin_[faceHalfedge(f, k)]] += a / 3.0;
		}
		VectorType M = VectorType::Zero(nVerts_);
		for (int cv = 0; cv < nCoverVerts_; ++cv)
			M[vertexMap_[cv]] += 0.5 * coverM[cv];
		return M;
	}

	/**
	 * @brief Recomputes a full diagnostics snapshot of the current cover.
	 */
	Diagnostics diagnostics() const
	{
		Diagnostics d;
		d.flipCount = flipCount_;
		d.coverFaceCount = static_cast<std::size_t>(nCoverFaces_);
		d.coverHalfedgeCount = static_cast<std::size_t>(nCoverHalfedges_);
		d.coverEdgeCount = static_cast<std::size_t>(nCoverEdges_);
		d.vertexMap = vertexMap_;
		d.edgeLengths = edgeLength_;
		d.vertexAngleSums.assign(nCoverVerts_, 0.0);

		d.edgeHalfedges.resize(nCoverEdges_);
		d.edgeFaces.resize(nCoverEdges_);
		d.edgeEndpoints.resize(nCoverEdges_);
		for (int h = 0; h < nCoverHalfedges_; ++h)
		{
			const int e = halfedgeEdge_[h];
			if (h > mate_[h]) continue;     // write every edge exactly once
			const int oa = origin_[h];
			const int ob = origin_[mate_[h]];
			d.edgeHalfedges[e] = std::make_pair(h, mate_[h]);
			d.edgeFaces[e] = std::make_pair(halfedgeFace_[h], halfedgeFace_[mate_[h]]);
			// Canonical undirected ordering (self loops keep equal endpoints).
			d.edgeEndpoints[e] = std::make_pair(std::min(oa, ob), std::max(oa, ob));
		}

		d.faceVertices.resize(nCoverFaces_);
		for (int f = 0; f < nCoverFaces_; ++f)
		{
			const int h0 = faceHalfedge(f, 0);
			d.faceVertices[f] = {{ origin_[h0], origin_[next_[h0]], origin_[prev_[h0]] }};
		}

		for (int h = 0; h < nCoverHalfedges_; ++h)
			d.vertexAngleSums[origin_[h]] += angleAt(h);

		d.coverArea = 0.0;
		for (int f = 0; f < nCoverFaces_; ++f)
			d.coverArea += faceArea(f);

		d.minEdgeWeight = computeMinEdgeWeight();
		return d;
	}

	/**
	 * @brief Smallest final per-edge cotangent weight. It is never clamped:
	 * a negative value means the intrinsic flow failed and construction
	 * throws instead.
	 */
	double minEdgeWeight() const { return computeMinEdgeWeight(); }

private:

	// ---- cover data -------------------------------------------------------
	// Halfedges are the primary representation: each cover face is a directed
	// cycle of three halfedges linked by next_/prev_. Every halfedge has
	// exactly one mate_ on the other side of its cover edge.
	int nVerts_ = 0;
	int nFaces_ = 0;
	int nCoverVerts_ = 0;
	int nCoverFaces_ = 0;
	int nCoverHalfedges_ = 0;
	int nCoverEdges_ = 0;

	std::vector<int>    origin_;          // cover vertex at the tail
	std::vector<int>    next_;            // next halfedge around the face
	std::vector<int>    prev_;            // previous halfedge around the face
	std::vector<int>    mate_;            // glued opposite halfedge
	std::vector<int>    halfedgeFace_;    // cover face owning the halfedge
	std::vector<int>    faceStart_;       // one halfedge belonging to each face
	std::vector<int>    halfedgeEdge_;    // cover edge id
	std::vector<double> edgeLength_;      // intrinsic length per edge id
	std::vector<int>    vertexMap_;       // cover vertex -> original vertex

	std::size_t flipCount_ = 0;

	// ---- validation -------------------------------------------------------

	void validate(const MeshType& m)
	{
		// Translate VCG precondition exceptions into the documented
		// std::invalid_argument contract for illegal input.
		try
		{
			RequireCompactness(m);
		}
		catch (const std::exception& e)
		{
			throw std::invalid_argument(std::string("TuftedOperator: ") + e.what());
		}
		nVerts_ = static_cast<int>(m.vert.size());
		nFaces_ = static_cast<int>(m.face.size());
		if (nVerts_ <= 0 || nFaces_ <= 0)
			throw std::invalid_argument("TuftedOperator: mesh must contain vertices and faces");

		std::vector<char> referenced(nVerts_, 0);
		for (int i = 0; i < nVerts_; ++i)
		{
			const auto& p = m.vert[i].P();
			for (int k = 0; k < 3; ++k)
			{
				if (!std::isfinite(static_cast<double>(p[k])))
					throw std::invalid_argument("TuftedOperator: non finite vertex coordinate");
			}
		}

		for (int f = 0; f < nFaces_; ++f)
		{
			const FaceType& face = m.face[f];
			if (face.VN() != 3)
				throw std::invalid_argument("TuftedOperator: only triangular faces are supported");
			int idx[3];
			for (int k = 0; k < 3; ++k)
			{
				const VertexType* vp = face.cV(k);
				if (vp == nullptr)
					throw std::invalid_argument("TuftedOperator: face has a null vertex reference");
				const int vi = tri::Index(m, vp);
				if (vi < 0 || vi >= nVerts_)
					throw std::invalid_argument("TuftedOperator: face references vertex outside the mesh");
				idx[k] = vi;
				referenced[vi] = 1;
			}
			if (idx[0] == idx[1] || idx[1] == idx[2] || idx[0] == idx[2])
				throw std::invalid_argument("TuftedOperator: face must have three distinct vertices");

			const auto p0 = m.vert[idx[0]].P();
			const auto p1 = m.vert[idx[1]].P();
			const auto p2 = m.vert[idx[2]].P();
			const double area2 = static_cast<double>(((p1 - p0) ^ (p2 - p0)).SquaredNorm());
			const double scale = 1.0 + static_cast<double>((p1 - p0).SquaredNorm())
			                       + static_cast<double>((p2 - p0).SquaredNorm());
			if (!(area2 > 1e-30 * scale * scale))
				throw std::invalid_argument("TuftedOperator: degenerate zero area face");
		}

		for (int i = 0; i < nVerts_; ++i)
			if (!referenced[i])
				throw std::invalid_argument("TuftedOperator: unreferenced (isolated) vertex");
	}

	// ---- cover construction ----------------------------------------------

	void buildCover(const MeshType& m)
	{
		nCoverVerts_ = nVerts_;          // one cover vertex per original vertex
		nCoverFaces_ = 2 * nFaces_;      // front and back copy of each triangle
		nCoverHalfedges_ = 3 * nCoverFaces_;
		nCoverEdges_ = 3 * nFaces_;      // each pair of halfedges is one edge

		origin_.resize(nCoverHalfedges_);
		next_.resize(nCoverHalfedges_);
		prev_.resize(nCoverHalfedges_);
		mate_.assign(nCoverHalfedges_, -1);
		halfedgeFace_.resize(nCoverHalfedges_);
		faceStart_.resize(nCoverFaces_);
		halfedgeEdge_.assign(nCoverHalfedges_, -1);
		edgeLength_.assign(nCoverEdges_, 0.0);
		vertexMap_.resize(nCoverVerts_);
		for (int v = 0; v < nCoverVerts_; ++v)
			vertexMap_[v] = v;

		// Original face corners (original vertex ids).
		std::vector<std::array<int, 3> > fv(nFaces_);
		for (int f = 0; f < nFaces_; ++f)
			for (int k = 0; k < 3; ++k)
				fv[f][k] = tri::Index(m, m.face[f].cV(k));

		// Build the two oriented cycles for every input triangle.
		// Cover face 2*f     : (v0, v1, v2)  (front copy)
		// Cover face 2*f + 1 : (v0, v2, v1)  (back copy, reversed winding)
		for (int f = 0; f < nFaces_; ++f)
		{
			const int front = 2 * f;
			const int back = front + 1;
			const int frontV[3] = { fv[f][0], fv[f][1], fv[f][2] };
			const int backV[3]  = { fv[f][0], fv[f][2], fv[f][1] };
			initFaceCycle(front, frontV);
			initFaceCycle(back, backV);
		}

		// Collect every halfedge on every undirected original edge.
		// Key: (small endpoint, big endpoint); entries: (source face, edge).
		std::map<std::pair<int, int>, std::vector<std::pair<int, int> > > rings;
		for (int f = 0; f < nFaces_; ++f)
		{
			for (int e = 0; e < 3; ++e)
			{
				int a = fv[f][e];
				int b = fv[f][(e + 1) % 3];
				if (a > b) std::swap(a, b);
				rings[std::make_pair(a, b)].push_back(std::make_pair(f, e));
			}
		}

		for (std::map<std::pair<int, int>, std::vector<std::pair<int, int> > >::iterator
		     it = rings.begin(); it != rings.end(); ++it)
		{
			std::vector<std::pair<int, int> >& entries = it->second;
			std::sort(entries.begin(), entries.end());
			const int a = it->first.first;
			const int b = it->first.second;
			const std::size_t k = entries.size();

			for (std::size_t i = 0; i < k; ++i)
			{
				// Halfedge of face i directed small -> big ...
				const int hSmallToBig = directedHalfedge(entries[i].first, entries[i].second,
				                                          fv, a, b);
				// ... paired with the reverse copy of the next face in the
				// sorted ring (last entry wraps back to the first).
				const std::pair<int, int>& nextEntry = entries[(i + 1) % k];
				const int hBigToSmall = directedHalfedge(nextEntry.first, nextEntry.second,
				                                          fv, b, a);
				mate_[hSmallToBig] = hBigToSmall;
				mate_[hBigToSmall] = hSmallToBig;
			}
		}

		// Assign edge ids and initial intrinsic lengths (3D edge lengths).
		int edgeCount = 0;
		for (int h = 0; h < nCoverHalfedges_; ++h)
		{
			if (h > mate_[h]) continue;
			halfedgeEdge_[h] = edgeCount;
			halfedgeEdge_[mate_[h]] = edgeCount;
			const int va = origin_[h];
			const int vb = origin_[mate_[h]];
			const double len = static_cast<double>(
				(m.vert[vertexMap_[va]].P() - m.vert[vertexMap_[vb]].P()).Norm());
			edgeLength_[edgeCount] = len;
			++edgeCount;
		}
		if (edgeCount != nCoverEdges_)
			throw std::logic_error("TuftedOperator: internal cover edge count mismatch");
	}

	/// Fills the halfedge cycle of one cover face with corner vertex ids.
	void initFaceCycle(int face, const int verts[3])
	{
		const int base = 3 * face;
		for (int k = 0; k < 3; ++k)
		{
			const int h = base + k;
			origin_[h] = verts[k];
			next_[h] = base + (k + 1) % 3;
			prev_[h] = base + (k + 2) % 3;
			halfedgeFace_[h] = face;
		}
		faceStart_[face] = base;
	}

	/**
	 * @brief Returns the cover halfedge of source face f / source edge e that
	 * is directed from vertex @p from to vertex @p to (the two must be the
	 * edge endpoints). One of the two tuft copies always contains it.
	 */
	int directedHalfedge(int f, int e,
	                     const std::vector<std::array<int, 3> >& fv,
	                     int from, int to) const
	{
		const int frontBase = 6 * f;            // cover face 2f, local edges 0..2
		const int backBase = 6 * f + 3;         // cover face 2f+1
		if (fv[f][e] == from && fv[f][(e + 1) % 3] == to)
			return frontBase + e;               // front copy keeps the direction
		// The back copy reverses every source edge: in the reversed triangle
		// (v0, v2, v1) source edge e lives at local edge 2-e.
		const int backLocal = 2 - e;
		return backBase + backLocal;
	}

	// ---- intrinsic geometry ----------------------------------------------

	/// Local-th halfedge of a cover face (valid after flips too).
	int faceHalfedge(int face, int local) const
	{
		int h = faceStart_[face];
		for (int k = 0; k < local; ++k) h = next_[h];
		return h;
	}

	/// Interior angle at the origin of halfedge h in its cover triangle.
	double angleAt(int h) const
	{
		const int hNext = next_[h];
		const int hPrev = prev_[h];
		const double lA = edgeLength_[halfedgeEdge_[h]];      // origin -> next
		const double lB = edgeLength_[halfedgeEdge_[hPrev]];  // prev -> origin
		const double lC = edgeLength_[halfedgeEdge_[hNext]];  // next -> prev
		const double cosAng = (lA * lA + lB * lB - lC * lC) / (2.0 * lA * lB);
		return std::acos(std::max(-1.0, std::min(1.0, cosAng)));
	}

	/// Area of a cover triangle from its intrinsic lengths (Heron).
	double faceArea(int face) const
	{
		const int h0 = faceHalfedge(face, 0);
		const int h1 = next_[h0];
		const int h2 = next_[h1];
		const double l0 = edgeLength_[halfedgeEdge_[h0]];
		const double l1 = edgeLength_[halfedgeEdge_[h1]];
		const double l2 = edgeLength_[halfedgeEdge_[h2]];
		const double s = 0.5 * (l0 + l1 + l2);
		const double val = s * (s - l0) * (s - l1) * (s - l2);
		return std::sqrt(val > 0.0 ? val : 0.0);
	}

	/// Angle opposite a cover edge, summed over its two incident triangles.
	double edgeOppositeAngleSum(int h) const
	{
		return angleAt(prev_[h]) + angleAt(prev_[mate_[h]]);
	}

	/**
	 * @brief Intrinsic Delaunay edge flip on the cover.
	 *
	 * Edge rotation keeps the edge identity (h stays paired with mate_[h])
	 * and only rewires the four outer halfedges. The new diagonal length
	 * comes from unfolding the two triangles into the plane.
	 *
	 * @return false when the flip would create an invalid intrinsic triangle.
	 */
	static bool triangleValid(double x, double y, double z, double tol)
	{
		return (x + y > z + tol) && (x + z > y + tol) && (y + z > x + tol);
	}

	bool flipEdge(int h)
	{
		const int g = mate_[h];
		// h runs a -> b; the opposite tips of the two triangles are c and d.
		const int c = origin_[prev_[h]];   // tip of the first triangle
		const int d = origin_[prev_[g]];   // tip of the second triangle
		// Note: c == d is legal on the tufted cover: the rotated edge becomes
		// an intrinsic self loop. Its length is the unfolded distance between
		// two distinct intrinsic copies, never zero, and the edge identity is
		// preserved. Parallel edges are never merged either.

		const int nH = next_[h];            // b -> c
		const int pH = prev_[h];            // c -> a
		const int nG = next_[g];            // a -> d
		const int pG = prev_[g];            // d -> b

		// Unfold both triangles along (a,b) and measure (c,d) intrinsically.
		const double lab = edgeLength_[halfedgeEdge_[h]];
		const double lac = edgeLength_[halfedgeEdge_[pH]];
		const double lbc = edgeLength_[halfedgeEdge_[nH]];
		const double lad = edgeLength_[halfedgeEdge_[nG]];
		const double lbd = edgeLength_[halfedgeEdge_[pG]];
		if (!(lab > 0.0 && lac > 0.0 && lbc > 0.0 && lad > 0.0 && lbd > 0.0))
			return false;

		const double cosAlpha = (lab * lab + lac * lac - lbc * lbc) / (2.0 * lab * lac);
		const double cosBeta  = (lab * lab + lad * lad - lbd * lbd) / (2.0 * lab * lad);
		const double sinAlpha = std::sqrt(std::max(0.0, 1.0 - cosAlpha * cosAlpha));
		const double sinBeta  = std::sqrt(std::max(0.0, 1.0 - cosBeta * cosBeta));
		const double cosSum = cosAlpha * cosBeta - sinAlpha * sinBeta;
		const double newLenSq = lac * lac + lad * lad - 2.0 * lac * lad * cosSum;
		if (!(newLenSq > 0.0))
			return false;
		const double newLen = std::sqrt(newLenSq);

		// Validate the rotation intrinsically: both new triangles must keep
		// strict triangle inequalities. This also admits self loops (c == d):
		// their intrinsic length is positive even though both endpoints map
		// back to the same cover vertex.
		const double tol = 1e-12 * std::max(std::max(lac, lad),
		                                    std::max(lbc, lbd));
		if (!triangleValid(lac, lad, newLen, tol) ||
		    !triangleValid(lbc, lbd, newLen, tol))
			return false;

		// Rotate the edge: triangles become (a,d,c) and (b,c,d).
		// Outer halfedges keep their identity and their mate pairing; only the
		// face cycles and the two diagonal endpoints change.
		const int faceF = halfedgeFace_[h];
		const int faceG = halfedgeFace_[g];

		next_[nG] = h;  prev_[h]  = nG;     // cycle nG -> h -> pH -> nG
		next_[h]  = pH; prev_[pH] = h;
		next_[pH] = nG; prev_[nG] = pH;

		next_[nH] = g;  prev_[g]  = nH;     // cycle nH -> g -> pG -> nH
		next_[g]  = pG; prev_[pG] = g;
		next_[pG] = nH; prev_[nH] = pG;

		origin_[h] = d;                     // h is now d -> c
		origin_[g] = c;                     // g is now c -> d

		halfedgeFace_[h] = faceF;
		halfedgeFace_[nG] = faceF;
		halfedgeFace_[pH] = faceF;
		halfedgeFace_[g] = faceG;
		halfedgeFace_[nH] = faceG;
		halfedgeFace_[pG] = faceG;
		faceStart_[faceF] = h;
		faceStart_[faceG] = g;

		// The edge identity (and its pairing) survives; only its intrinsic
		// length changes. Parallel cover edges are never merged.
		edgeLength_[halfedgeEdge_[h]] = newLen;
		return true;
	}

	void runIntrinsicDelaunay()
	{
		const double angleTol = 1e-10;
		std::queue<int> work;
		std::vector<char> queued(nCoverEdges_, 0);
		for (int h = 0; h < nCoverHalfedges_; ++h)
		{
			if (h < mate_[h])
			{
				work.push(h);
				queued[halfedgeEdge_[h]] = 1;
			}
		}

		const std::size_t flipCap = 1000u * static_cast<std::size_t>(nCoverEdges_ + 1);

		while (!work.empty())
		{
			const int h = work.front();
			work.pop();
			queued[halfedgeEdge_[h]] = 0;

			if (edgeOppositeAngleSum(h) <= detail::kTuftedPi + angleTol)
				continue;                   // Delaunay (co-circular edges stay)

			const int g = mate_[h];
			const int nH = next_[h], pH = prev_[h];
			const int nG = next_[g], pG = prev_[g];
			if (!flipEdge(h))
			{
				// A genuinely non flippable edge that still violates Delaunay
				// is an explicit failure: weights are never clamped.
				throw std::runtime_error(
					"TuftedOperator: intrinsic Delaunay flow stalled on a non flippable edge");
			}
			++flipCount_;
			if (flipCount_ > flipCap)
				throw std::runtime_error(
					"TuftedOperator: intrinsic Delaunay flip flow exceeded its iteration cap");

			// The rotated edge and the four outer edges must be rechecked.
			int candidates[5] = { h, nH, pH, nG, pG };
			for (int i = 0; i < 5; ++i)
			{
				const int e = halfedgeEdge_[candidates[i]];
				if (!queued[e])
				{
					queued[e] = 1;
					work.push(candidates[i]);
				}
			}
		}

		// Explicit failure instead of clamping negative weights away.
		for (int h = 0; h < nCoverHalfedges_; ++h)
		{
			if (h > mate_[h]) continue;
			if (edgeOppositeAngleSum(h) > detail::kTuftedPi + 1e-8)
				throw std::runtime_error(
					"TuftedOperator: intrinsic Delaunay flow stalled on a non Delaunay edge");
		}
		if (computeMinEdgeWeight() < -1e-9)
			throw std::runtime_error(
				"TuftedOperator: negative cotangent weight after intrinsic Delaunay flow");
	}

	// ---- matrix assembly --------------------------------------------------

	/// Cotangent of the angle opposite edge (origin(h), next(h)).
	double cotangentOpposite(int h) const
	{
		const int hNext = next_[h];
		const int hPrev = prev_[h];
		const double lOpp = edgeLength_[halfedgeEdge_[h]];
		const double l1 = edgeLength_[halfedgeEdge_[hNext]];
		const double l2 = edgeLength_[halfedgeEdge_[hPrev]];
		const double area = faceArea(halfedgeFace_[h]);
		if (!(area > 0.0))
			throw std::runtime_error("TuftedOperator: degenerate cover triangle while assembling");
		return (l1 * l1 + l2 * l2 - lOpp * lOpp) / (4.0 * area);
	}

	SparseMatrixType assembleCoverLaplacian() const
	{
		std::vector<Eigen::Triplet<double> > trips;
		trips.reserve(static_cast<std::size_t>(12) * nCoverFaces_);
		for (int f = 0; f < nCoverFaces_; ++f)
		{
			const int h0 = faceHalfedge(f, 0);
			const int h1 = next_[h0];
			const int h2 = next_[h1];
			const int v[3] = { origin_[h0], origin_[h1], origin_[h2] };
			const double cot[3] = { cotangentOpposite(h0),
			                        cotangentOpposite(h1),
			                        cotangentOpposite(h2) };
			// Edge (v[i], v[i+1]) faces the angle at v[i+2], whose
			// cotangent is cotangentOpposite(h_i), i.e. cot[i].
			for (int i = 0; i < 3; ++i)
			{
				const int j = (i + 1) % 3;
				const double w = 0.5 * cot[i];
				trips.emplace_back(v[i], v[j], -w);
				trips.emplace_back(v[j], v[i], -w);
				trips.emplace_back(v[i], v[i], w);
				trips.emplace_back(v[j], v[j], w);
			}
		}
		SparseMatrixType L(nCoverVerts_, nCoverVerts_);
		L.setFromTriplets(trips.begin(), trips.end());
		return L;
	}

	/// Gathers a cover matrix into the original vertex space and divides by 2.
	void gatherMatrix(const SparseMatrixType& coverL, SparseMatrixType& L) const
	{
		std::vector<Eigen::Triplet<double> > trips;
		trips.reserve(static_cast<std::size_t>(coverL.nonZeros()));
		for (int k = 0; k < coverL.outerSize(); ++k)
		{
			for (SparseMatrixType::InnerIterator it(coverL, k); it; ++it)
			{
				trips.emplace_back(vertexMap_[it.row()],
				                   vertexMap_[it.col()],
				                   0.5 * it.value());
			}
		}
		L.setFromTriplets(trips.begin(), trips.end());
	}

	double computeMinEdgeWeight() const
	{
		double minW = std::numeric_limits<double>::infinity();
		for (int h = 0; h < nCoverHalfedges_; ++h)
		{
			if (h > mate_[h]) continue;
			// Per-edge weight after gathering: each side contributes cot/2,
			// the tuft average divides once more by two.
			const double w = 0.25 * (cotangentOpposite(h) + cotangentOpposite(mate_[h]));
			minW = std::min(minW, w);
		}
		return minW;
	}
};

} // namespace tri
} // namespace vcg

#endif // __VCGLIB_TUFTED_LAPLACIAN
