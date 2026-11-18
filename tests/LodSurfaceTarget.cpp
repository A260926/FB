/*******************************************************************************
* The MIT License (MIT)
*
* Copyright (c) 2015 David Williams and Matthew Williams
*
* Permission is hereby granted, free of charge, to any person obtaining a copy
* of this software and associated documentation files (the "Software"), to deal
* in the Software without restriction, including without limitation the rights
* to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
* copies of the Software, and to permit persons to whom the Software is
* furnished to do so, subject to the following conditions:
*
* The above copyright notice and this permission notice shall be included in all
* copies or substantial portions of the Software.
*
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
* IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
* FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
* COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
* IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
* CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
*******************************************************************************/

// Target tests for PolyVox::LodSurface. Registered through the task_support
// driver (no main() here): the three groups are "seams", "edits" and "reuse".
// Every check goes through TASK_CHECK and inspects actual mesh topology,
// winding, boundary edges, seam vertices and source-read statistics.

#include "task_support/check.h"

#include "PolyVox/LodSurface.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
#include <set>
#include <tuple>
#include <vector>

using namespace PolyVox;

namespace
{
	const int kBox = 16; // block box is [0,16)^3 in every test

	std::vector<float> planeField(const Region& r)
	{
		// Tilted plane with irrational-looking coefficients so the level never
		// passes exactly through a lattice point.
		std::vector<float> v;
		v.reserve(static_cast<size_t>(r.getWidthInVoxels())
			* r.getHeightInVoxels() * r.getDepthInVoxels());
		for (int32_t z = r.getLowerZ(); z <= r.getUpperZ(); ++z)
		for (int32_t y = r.getLowerY(); y <= r.getUpperY(); ++y)
		for (int32_t x = r.getLowerX(); x <= r.getUpperX(); ++x)
		{
			v.push_back(0.70710678f * static_cast<float>(x)
				+ 0.41421356f * static_cast<float>(y)
				+ 0.33333334f * static_cast<float>(z) - 9.5f);
		}
		return v;
	}

	std::vector<float> sphereField(const Region& r,
		float cx, float cy, float cz, float radiusSquared = 27.3f)
	{
		// Closed shell; a non-integer squared radius keeps the level off every
		// lattice point.
		std::vector<float> v;
		v.reserve(static_cast<size_t>(r.getWidthInVoxels())
			* r.getHeightInVoxels() * r.getDepthInVoxels());
		for (int32_t z = r.getLowerZ(); z <= r.getUpperZ(); ++z)
		for (int32_t y = r.getLowerY(); y <= r.getUpperY(); ++y)
		for (int32_t x = r.getLowerX(); x <= r.getUpperX(); ++x)
		{
			const float dx = static_cast<float>(x) - cx;
			const float dy = static_cast<float>(y) - cy;
			const float dz = static_cast<float>(z) - cz;
			v.push_back(radiusSquared - (dx * dx + dy * dy + dz * dz));
		}
		return v;
	}

	// Centre of the 16x8x8 two-block box for an interface normal along axis.
	Vector3DFloat twoBlockCentre(int axis)
	{
		float c[3] = { 4.0f, 4.0f, 4.0f };
		c[axis] = 8.0f;
		return Vector3DFloat(c[0], c[1], c[2]);
	}

	// Two physical 8^3 blocks tiling a 16x8x8 box along one axis: a fine
	// step-1 block (8 cells) and a coarse step-2 block (4 cells). sign > 0 puts
	// the fine block on the low side of the interface, sign < 0 on the high
	// side.
	std::vector<LodBlock> twoBlockLayout(int axis, int sign)
	{
		int fineLo[3] = { 0, 0, 0 };
		int coarseLo[3] = { 0, 0, 0 };
		if (sign > 0)
		{
			fineLo[axis] = 0;
			coarseLo[axis] = 8;
		}
		else
		{
			coarseLo[axis] = 0;
			fineLo[axis] = 8;
		}
		std::vector<LodBlock> lay;
		lay.push_back(LodBlock(1,
			Vector3DInt32(fineLo[0], fineLo[1], fineLo[2]), 8, 1));
		lay.push_back(LodBlock(2,
			Vector3DInt32(coarseLo[0], coarseLo[1], coarseLo[2]), 4, 2));
		return lay;
	}

	// Eight octants: one fine corner block touching three coarse blocks, the
	// remaining octants coarse. The fine block simultaneously meets coarse
	// neighbours across two and three interface planes at the corner.
	std::vector<LodBlock> octantLayout()
	{
		const int o[8][3] =
		{
			{ 0, 0, 0 }, { 8, 0, 0 }, { 0, 8, 0 }, { 8, 8, 0 },
			{ 0, 0, 8 }, { 8, 0, 8 }, { 0, 8, 8 }, { 8, 8, 8 }
		};
		std::vector<LodBlock> lay;
		for (int i = 0; i < 8; ++i)
		{
			const int step = (i == 0) ? 1 : 2;
			lay.push_back(LodBlock(i + 1,
				Vector3DInt32(o[i][0], o[i][1], o[i][2]),
				step == 1 ? 8 : 4, step));
		}
		return lay;
	}

	struct MeshAudit
	{
		size_t triangles;
		uint32_t vertices;
		int unpairedEdges;
		int duplicateEdges;
		int degenerateTriangles;
		int nonUnitNormals;
		int nonFiniteNormals;
		int duplicatedPositions;
	};

	typedef std::tuple<float, float, float> PosKey;

	PosKey positionKey(const Vector3DFloat& p)
	{
		return std::make_tuple(p.getX(), p.getY(), p.getZ());
	}

	MeshAudit audit(const Mesh<Vertex<float> >& m)
	{
		MeshAudit a;
		a.triangles = m.getNoOfIndices() / 3;
		a.vertices = m.getNoOfVertices();
		a.unpairedEdges = 0;
		a.duplicateEdges = 0;
		a.degenerateTriangles = 0;
		a.nonUnitNormals = 0;
		a.nonFiniteNormals = 0;
		a.duplicatedPositions = 0;

		std::map<std::pair<uint32_t, uint32_t>, int> edges;
		for (size_t i = 0; i + 2 < m.getNoOfIndices(); i += 3)
		{
			const uint32_t t[3] =
			{
				m.getIndex(static_cast<uint32_t>(i)),
				m.getIndex(static_cast<uint32_t>(i + 1)),
				m.getIndex(static_cast<uint32_t>(i + 2))
			};
			if (t[0] == t[1] || t[1] == t[2] || t[0] == t[2])
				++a.degenerateTriangles;
			for (int k = 0; k < 3; ++k)
				++edges[std::make_pair(t[k], t[(k + 1) % 3])];
		}

		for (std::map<std::pair<uint32_t, uint32_t>, int>::const_iterator it =
			edges.begin(); it != edges.end(); ++it)
		{
			if (it->second > 1) ++a.duplicateEdges;
			const std::pair<uint32_t, uint32_t> rev =
				std::make_pair(it->first.second, it->first.first);
			std::map<std::pair<uint32_t, uint32_t>, int>::const_iterator jt =
				edges.find(rev);
			if (jt == edges.end() || jt->second != 1) ++a.unpairedEdges;
		}

		std::set<PosKey> positions;
		for (uint32_t i = 0; i < m.getNoOfVertices(); ++i)
		{
			const Vertex<float>& v = m.getVertex(i);
			if (!std::isfinite(v.normal.getX()) || !std::isfinite(v.normal.getY())
				|| !std::isfinite(v.normal.getZ()))
			{
				++a.nonFiniteNormals;
			}
			const float len = v.normal.length();
			if (len > 1.0e-9f && std::fabs(len - 1.0f) > 1.0e-4f)
				++a.nonUnitNormals;
			if (!positions.insert(positionKey(v.position)).second)
				++a.duplicatedPositions;
		}
		return a;
	}

	bool onBoxBoundary(const Vector3DFloat& p,
		const float lo[3], const float hi[3])
	{
		const float eps = 1.0e-5f;
		for (int axis = 0; axis < 3; ++axis)
		{
			const float c = axis == 0 ? p.getX()
				: (axis == 1 ? p.getY() : p.getZ());
			if (std::fabs(c - lo[axis]) < eps || std::fabs(c - hi[axis]) < eps)
				return true;
		}
		return false;
	}

	// Every unpaired (boundary) edge must lie on the layout's outer box; an
	// interior unpaired edge would be a seam crack.
	int interiorBoundaryEdges(const Mesh<Vertex<float> >& m,
		const float lo[3], const float hi[3])
	{
		std::map<std::pair<uint32_t, uint32_t>, int> edges;
		for (size_t i = 0; i + 2 < m.getNoOfIndices(); i += 3)
		{
			const uint32_t t[3] =
			{
				m.getIndex(static_cast<uint32_t>(i)),
				m.getIndex(static_cast<uint32_t>(i + 1)),
				m.getIndex(static_cast<uint32_t>(i + 2))
			};
			for (int k = 0; k < 3; ++k)
				++edges[std::make_pair(t[k], t[(k + 1) % 3])];
		}
		int interior = 0;
		for (std::map<std::pair<uint32_t, uint32_t>, int>::const_iterator it =
			edges.begin(); it != edges.end(); ++it)
		{
			const std::pair<uint32_t, uint32_t> rev =
				std::make_pair(it->first.second, it->first.first);
			if (edges.count(rev)) continue;
			const Vertex<float>& a = m.getVertex(it->first.first);
			const Vertex<float>& b = m.getVertex(it->first.second);
			if (!onBoxBoundary(a.position, lo, hi)
				|| !onBoxBoundary(b.position, lo, hi))
				++interior;
		}
		return interior;
	}

	// Canonical oriented triangles expressed through vertex positions, so two
	// resources with different vertex numbering can be compared geometrically.
	std::set<std::tuple<PosKey, PosKey, PosKey> >
		directedGeometry(const Mesh<Vertex<float> >& m)
	{
		std::set<std::tuple<PosKey, PosKey, PosKey> > out;
		for (size_t i = 0; i + 2 < m.getNoOfIndices(); i += 3)
		{
			const PosKey a = positionKey(m.getVertex(m.getIndex(
				static_cast<uint32_t>(i))).position);
			const PosKey b = positionKey(m.getVertex(m.getIndex(
				static_cast<uint32_t>(i + 1))).position);
			const PosKey c = positionKey(m.getVertex(m.getIndex(
				static_cast<uint32_t>(i + 2))).position);
			out.insert(std::make_tuple(a, b, c));
		}
		return out;
	}

	bool sameDirectedGeometry(const Mesh<Vertex<float> >& a,
		const Mesh<Vertex<float> >& b)
	{
		return directedGeometry(a) == directedGeometry(b);
	}

	// Largest normal difference at positions common to both meshes; returns a
	// negative value when there is no common position.
	float maxSharedNormalChange(const Mesh<Vertex<float> >& before,
		const Mesh<Vertex<float> >& after)
	{
		std::map<PosKey, Vector3DFloat> normals;
		for (uint32_t i = 0; i < before.getNoOfVertices(); ++i)
			normals[positionKey(before.getVertex(i).position)] =
				before.getVertex(i).normal;
		float worst = -1.0f;
		for (uint32_t i = 0; i < after.getNoOfVertices(); ++i)
		{
			const PosKey key = positionKey(after.getVertex(i).position);
			std::map<PosKey, Vector3DFloat>::const_iterator it = normals.find(key);
			if (it == normals.end()) continue;
			const float change =
				(it->second - after.getVertex(i).normal).length();
			if (change > worst) worst = change;
		}
		return worst;
	}
}

////////////////////////////////////////////////////////////////////////////////
// seams group: analytic plane and off-centre sphere across all six interface
// orientations, plus a fine block meeting coarse blocks on two and three faces.
////////////////////////////////////////////////////////////////////////////////

static int seams_six_orientations_plane()
{
	Region r(-1, -1, -1, kBox + 1, kBox + 1, kBox + 1);
	const std::vector<float> values = planeField(r);

	for (int axis = 0; axis < 3; ++axis)
	for (int signIdx = 0; signIdx < 2; ++signIdx)
	{
		const int sign = signIdx == 0 ? 1 : -1;
		LodSurface surface(r, values);
		surface.setLayout(twoBlockLayout(axis, sign));
		const Mesh<Vertex<float> > m = surface.mesh();

		TASK_CHECK(m.getNoOfVertices() > 0);
		TASK_CHECK(m.getNoOfIndices() >= 3);
		TASK_CHECK(m.getOffset() == Vector3DInt32(0, 0, 0));
		const MeshAudit a = audit(m);
		TASK_CHECK(a.degenerateTriangles == 0);
		TASK_CHECK(a.duplicateEdges == 0);
		TASK_CHECK(a.duplicatedPositions == 0);
		TASK_CHECK(a.nonFiniteNormals == 0);
		TASK_CHECK(a.nonUnitNormals == 0);
		// Plane exits the domain: the only open edges may be on the outer box.
		// The rotated box spans [0,16) on the interface axis and [0,8) on the
		// other two.
		float lo[3] = { 0.0f, 0.0f, 0.0f };
		float hi[3] = { 8.0f, 8.0f, 8.0f };
		hi[axis] = static_cast<float>(kBox);
		TASK_CHECK(interiorBoundaryEdges(m, lo, hi) == 0);
	}
	return 0;
}

static int seams_six_orientations_sphere()
{
	Region r(-1, -1, -1, kBox + 1, kBox + 1, kBox + 1);

	for (int axis = 0; axis < 3; ++axis)
	for (int signIdx = 0; signIdx < 2; ++signIdx)
	{
		const int sign = signIdx == 0 ? 1 : -1;
		// A small shell centered in the (possibly rotated) 16x8x8 box, so it
		// stays enclosed by the domain for every interface orientation.
		const Vector3DFloat centre = twoBlockCentre(axis);
		const std::vector<float> values = sphereField(r,
			centre.getX(), centre.getY(), centre.getZ(), 4.7f);

		LodSurface surface(r, values);
		surface.setLayout(twoBlockLayout(axis, sign));
		const Mesh<Vertex<float> > m = surface.mesh();

		TASK_CHECK(m.getNoOfVertices() > 0);
		const MeshAudit a = audit(m);
		TASK_CHECK(a.degenerateTriangles == 0);
		TASK_CHECK(a.duplicateEdges == 0);
		TASK_CHECK(a.duplicatedPositions == 0);
		// Closed shell inside the domain: a closed, oriented two-manifold.
		TASK_CHECK(a.unpairedEdges == 0);
		TASK_CHECK(a.nonFiniteNormals == 0);
		TASK_CHECK(a.nonUnitNormals == 0);
	}
	return 0;
}

static int seams_multi_face_corner()
{
	Region r(-2, -2, -2, kBox + 2, kBox + 2, kBox + 2);
	const std::vector<float> values = sphereField(r, 8.0f, 8.0f, 8.0f);

	LodSurface surface(r, values);
	surface.setLayout(octantLayout());
	const Mesh<Vertex<float> > m = surface.mesh();

	const MeshAudit a = audit(m);
	TASK_CHECK(m.getNoOfVertices() > 0);
	TASK_CHECK(a.degenerateTriangles == 0);
	TASK_CHECK(a.duplicateEdges == 0);
	TASK_CHECK(a.duplicatedPositions == 0);
	TASK_CHECK(a.unpairedEdges == 0); // three-way corner leaves no crack

	// Outward winding: every geometric face normal points away from the
	// sphere centre.
	int wrongWinding = 0;
	const Vector3DFloat centre(8.0f, 8.0f, 8.0f);
	for (size_t i = 0; i + 2 < m.getNoOfIndices(); i += 3)
	{
		const Vertex<float>& v0 = m.getVertex(m.getIndex(
			static_cast<uint32_t>(i)));
		const Vertex<float>& v1 = m.getVertex(m.getIndex(
			static_cast<uint32_t>(i + 1)));
		const Vertex<float>& v2 = m.getVertex(m.getIndex(
			static_cast<uint32_t>(i + 2)));
		const Vector3DFloat faceNormal =
			(v1.position - v0.position).cross(v2.position - v0.position);
		const Vector3DFloat mid = (v0.position + v1.position + v2.position)
			* (1.0f / 3.0f);
		if (faceNormal.dot(mid - centre) <= 0.0f) ++wrongWinding;
	}
	TASK_CHECK(wrongWinding == 0);

	// Every block got exactly one revision from the initial build.
	for (int id = 1; id <= 8; ++id)
		TASK_CHECK(surface.blockRevision(id) == 1);
	return 0;
}

static int seams_coarse_fine_interface_vertices()
{
	Region r(-1, -1, -1, kBox + 1, kBox + 1, kBox + 1);
	// Small enclosed shell centred on the interface plane x = 8 so the seam
	// definitely carries vertices.
	const std::vector<float> values = sphereField(r, 8.0f, 4.0f, 4.0f, 4.7f);

	LodSurface surface(r, values);
	surface.setLayout(twoBlockLayout(0, 1)); // interface plane x = 8
	const Mesh<Vertex<float> > m = surface.mesh();

	// Vertices generated on the interface plane must be shared (one position,
	// one normal). audit() already proves no position is duplicated; here we
	// additionally confirm the interface actually carries vertices and that
	// each has a finite, unit normal.
	int onInterface = 0;
	for (uint32_t i = 0; i < m.getNoOfVertices(); ++i)
	{
		const Vertex<float>& v = m.getVertex(i);
		if (std::fabs(v.position.getX() - 8.0f) < 1.0e-5f)
		{
			++onInterface;
			TASK_CHECK(std::isfinite(v.normal.length()));
			TASK_CHECK(std::fabs(v.normal.length() - 1.0f) < 1.0e-4f);
		}
	}
	TASK_CHECK(onInterface > 0);
	const MeshAudit a = audit(m);
	TASK_CHECK(a.unpairedEdges == 0);
	return 0;
}

// A monotone three-level chain (native steps 1, then 2, then 4) with a closed
// shell crossing both interfaces. The transition subdivision must cascade
// (4 -> 2 -> 1) and leave a single closed, oriented two-manifold.
static int seams_three_level_chain()
{
	Region r(-1, -1, -1, 33, 33, 33);
	std::vector<float> values;
	for (int32_t z = -1; z <= 33; ++z)
	for (int32_t y = -1; y <= 33; ++y)
	for (int32_t x = -1; x <= 33; ++x)
	{
		const float dx = static_cast<float>(x) - 12.3f;
		const float dy = static_cast<float>(y) - 16.7f;
		const float dz = static_cast<float>(z) - 15.9f;
		values.push_back(45.0f - (dx * dx + dy * dy + dz * dz));
	}

	std::vector<LodBlock> layout;
	int32_t id = 1;
	for (int by = 0; by < 4; ++by)
	for (int bz = 0; bz < 4; ++bz)
		layout.push_back(LodBlock(id++,
			Vector3DInt32(0, by * 8, bz * 8), 8, 1));   // slab x[0,8)
	for (int by = 0; by < 4; ++by)
	for (int bz = 0; bz < 4; ++bz)
		layout.push_back(LodBlock(id++,
			Vector3DInt32(8, by * 8, bz * 8), 4, 2));   // slab x[8,16)
	for (int by = 0; by < 2; ++by)
	for (int bz = 0; bz < 2; ++bz)
		layout.push_back(LodBlock(id++,
			Vector3DInt32(16, by * 16, bz * 16), 4, 4)); // slab x[16,32)

	LodSurface surface(r, values);
	surface.setLayout(layout);
	const Mesh<Vertex<float> > m = surface.mesh();

	TASK_CHECK(m.getNoOfVertices() > 0);
	const MeshAudit a = audit(m);
	TASK_CHECK(a.degenerateTriangles == 0);
	TASK_CHECK(a.duplicateEdges == 0);
	TASK_CHECK(a.duplicatedPositions == 0);
	TASK_CHECK(a.unpairedEdges == 0); // fully enclosed closed shell
	TASK_CHECK(a.nonFiniteNormals == 0);
	return 0;
}

////////////////////////////////////////////////////////////////////////////////
// edits group: density and normal changes, cross-block edits, invalid batches,
// and interleaved operation of two resources.
////////////////////////////////////////////////////////////////////////////////

static int edits_local_propagation_and_normals()
{
	Region r(-2, -2, -2, kBox + 2, kBox + 2, kBox + 2);
	const std::vector<float> values = sphereField(r, 8.0f, 8.0f, 8.0f);

	LodSurface surface(r, values);
	surface.setLayout(octantLayout());
	const Mesh<Vertex<float> > before = surface.mesh();
	const uint32_t farRevision = surface.blockRevision(8);
	const uint64_t fullReads = surface.lastStats().sourceReads;

	// A sign flip next to the shell: the surface positions and the
	// interpolated central-difference normals in the neighbourhood must move.
	std::vector<SampleEdit> batch;
	batch.push_back(SampleEdit(Vector3DInt32(5, 5, 5), -50.0f));
	batch.push_back(SampleEdit(Vector3DInt32(5, 5, 5), 60.0f)); // duplicate: last wins
	surface.updateSamples(batch);

	const LodSurfaceStats stats = surface.lastStats();
	TASK_CHECK(stats.rebuiltBlocks >= 1);
	TASK_CHECK(stats.rebuiltBlocks < 8);
	TASK_CHECK(stats.sourceReads < fullReads);
	TASK_CHECK(surface.blockRevision(8) == farRevision);

	const Mesh<Vertex<float> > after = surface.mesh();
	TASK_CHECK(after.getNoOfIndices() != before.getNoOfIndices()
		|| !sameDirectedGeometry(before, after));
	// A sign flip must move both positions and the interpolated normals.
	TASK_CHECK(maxSharedNormalChange(before, after) > 1.0e-3f);
	const MeshAudit a = audit(after);
	TASK_CHECK(a.degenerateTriangles == 0);
	TASK_CHECK(a.duplicatedPositions == 0);
	TASK_CHECK(a.unpairedEdges == 0);
	TASK_CHECK(a.nonFiniteNormals == 0);

	// Empty edit performs no rebuild and no reads.
	surface.updateSamples(std::vector<SampleEdit>());
	TASK_CHECK(surface.lastStats().rebuiltBlocks == 0);
	TASK_CHECK(surface.lastStats().sourceReads == 0);
	TASK_CHECK(surface.blockRevision(8) == farRevision);
	return 0;
}

static int edits_cross_block_and_rollback()
{
	Region r(-1, -1, -1, kBox + 1, kBox + 1, kBox + 1);
	// Small shell centred in the 16x8x8 box (interface y = 8), fully enclosed.
	const std::vector<float> values = sphereField(r, 8.0f, 4.0f, 4.0f, 4.7f);

	LodSurface surface(r, values);
	surface.setLayout(twoBlockLayout(1, 1)); // interface y = 8
	const Mesh<Vertex<float> > before = surface.mesh();

	// Two edits on opposite sides of the interface (fine side y<8, coarse y>8).
	std::vector<SampleEdit> batch;
	batch.push_back(SampleEdit(Vector3DInt32(4, 4, 4), 40.0f));
	batch.push_back(SampleEdit(Vector3DInt32(4, 12, 4), -40.0f));
	surface.updateSamples(batch);

	const Mesh<Vertex<float> > after = surface.mesh();
	TASK_CHECK(!sameDirectedGeometry(before, after));
	const MeshAudit a = audit(after);
	// After the edit the shell may touch the outer box, but a seam crack would
	// appear as an unpaired edge in the interior, which must stay impossible.
	float lo[3] = { 0.0f, 0.0f, 0.0f };
	float hi[3] = { 8.0f, 8.0f, 8.0f };
	hi[1] = static_cast<float>(kBox); // interface axis is y
	TASK_CHECK(interiorBoundaryEdges(after, lo, hi) == 0);
	TASK_CHECK(a.degenerateTriangles == 0);
	TASK_CHECK(a.duplicatedPositions == 0);

	// Invalid batches must leave the whole resource intact.
	const Mesh<Vertex<float> > snapshot = surface.mesh();
	const LodSurfaceStats goodStats = surface.lastStats();

	bool threwBounds = false;
	try
	{
		std::vector<SampleEdit> bad;
		bad.push_back(SampleEdit(Vector3DInt32(999, 0, 0), 1.0f));
		surface.updateSamples(bad);
	}
	catch (const std::invalid_argument&) { threwBounds = true; }
	TASK_CHECK(threwBounds);

	bool threwFinite = false;
	try
	{
		std::vector<SampleEdit> bad;
		bad.push_back(SampleEdit(Vector3DInt32(1, 1, 1),
			std::numeric_limits<float>::infinity()));
		surface.updateSamples(bad);
	}
	catch (const std::invalid_argument&) { threwFinite = true; }
	TASK_CHECK(threwFinite);

	TASK_CHECK(sameDirectedGeometry(snapshot, surface.mesh()));
	TASK_CHECK(surface.lastStats().sourceReads == goodStats.sourceReads);
	TASK_CHECK(surface.lastStats().rebuiltBlocks == goodStats.rebuiltBlocks);
	return 0;
}

static int edits_two_interleaved_resources()
{
	Region r(-2, -2, -2, kBox + 2, kBox + 2, kBox + 2);
	const std::vector<float> values = sphereField(r, 8.0f, 8.0f, 8.0f);

	LodSurface a(r, values);
	LodSurface b(r, values);
	a.setLayout(octantLayout());
	b.setLayout(octantLayout());
	TASK_CHECK(sameDirectedGeometry(a.mesh(), b.mesh()));

	// Near-surface sign flip so both vertex positions and normals move.
	const SampleEdit nearSurface(Vector3DInt32(4, 8, 8), -11.0f);
	a.updateSamples(std::vector<SampleEdit>{ nearSurface });
	// b is untouched and must still match the original shared geometry.
	LodSurface reference(r, values);
	reference.setLayout(octantLayout());
	TASK_CHECK(sameDirectedGeometry(b.mesh(), reference.mesh()));
	TASK_CHECK(!sameDirectedGeometry(a.mesh(), b.mesh()));

	// Applying the same edit to b converges the two resources again.
	b.updateSamples(std::vector<SampleEdit>{ nearSurface });
	TASK_CHECK(sameDirectedGeometry(a.mesh(), b.mesh()));
	return 0;
}

////////////////////////////////////////////////////////////////////////////////
// reuse group: incremental rebuild selection, ordering independence, LOD
// switching equivalence and real source-read accounting.
////////////////////////////////////////////////////////////////////////////////

static int reuse_identical_and_reordered_layout()
{
	Region r(-2, -2, -2, kBox + 2, kBox + 2, kBox + 2);
	const std::vector<float> values = sphereField(r, 8.0f, 8.0f, 8.0f);

	LodSurface surface(r, values);
	const std::vector<LodBlock> layout = octantLayout();
	surface.setLayout(layout);
	const Mesh<Vertex<float> > first = surface.mesh();
	const uint32_t revision = surface.blockRevision(3);

	// Same content in reverse order: geometry unchanged, no block rebuilt.
	std::vector<LodBlock> reordered(layout.rbegin(), layout.rend());
	surface.setLayout(reordered);
	TASK_CHECK(surface.lastStats().rebuiltBlocks == 0);
	TASK_CHECK(surface.lastStats().sourceReads == 0);
	TASK_CHECK(surface.blockRevision(3) == revision);
	TASK_CHECK(sameDirectedGeometry(first, surface.mesh()));

	// Setting the very same vector again is likewise a zero rebuild.
	surface.setLayout(layout);
	TASK_CHECK(surface.lastStats().rebuiltBlocks == 0);
	TASK_CHECK(surface.blockRevision(3) == revision);
	return 0;
}

static int reuse_local_lod_switch_equivalence()
{
	Region r(-2, -2, -2, kBox + 2, kBox + 2, kBox + 2);
	const std::vector<float> values = sphereField(r, 8.0f, 8.0f, 8.0f);

	// Resource one: all-fine, then switch one octant to coarse.
	LodSurface switched(r, values);
	std::vector<LodBlock> allFine;
	allFine.push_back(LodBlock(1, Vector3DInt32(0, 0, 0), 16, 1));
	switched.setLayout(allFine);
	switched.setLayout(octantLayout());

	// Resource two: built directly with the switched layout.
	LodSurface fresh(r, values);
	fresh.setLayout(octantLayout());

	TASK_CHECK(sameDirectedGeometry(switched.mesh(), fresh.mesh()));

	// The switch rebuilt only blocks participating in the new seams; it never
	// forced every block down to the finest layer.
	TASK_CHECK(switched.lastStats().rebuiltBlocks > 0);
	TASK_CHECK(switched.lastStats().rebuiltBlocks <= 8);

	// Switching back restores the all-fine directed geometry.
	LodSurface fineReference(r, values);
	fineReference.setLayout(allFine);
	switched.setLayout(allFine);
	TASK_CHECK(sameDirectedGeometry(switched.mesh(), fineReference.mesh()));
	return 0;
}

static int reuse_read_statistics_and_distant_blocks()
{
	Region r(-1, -1, -1, 33, 33, 33);
	// Shell confined to the corner octant.
	std::vector<float> values;
	for (int z = -1; z <= 33; ++z)
	for (int y = -1; y <= 33; ++y)
	for (int x = -1; x <= 33; ++x)
	{
		const float dx = static_cast<float>(x) - 6.0f;
		const float dy = static_cast<float>(y) - 6.0f;
		const float dz = static_cast<float>(z) - 6.0f;
		values.push_back(20.0f - (dx * dx + dy * dy + dz * dz));
	}

	LodSurface surface(r, values);
	std::vector<LodBlock> layout;
	int id = 1;
	for (int bz = 0; bz < 2; ++bz)
	for (int by = 0; by < 2; ++by)
	for (int bx = 0; bx < 2; ++bx)
	{
		layout.push_back(LodBlock(id++,
			Vector3DInt32(bx * 16, by * 16, bz * 16), 4, 4));
	}
	surface.setLayout(layout);
	const uint64_t fullReads = surface.lastStats().sourceReads;
	const uint32_t farRevision = surface.blockRevision(8);

	// A corner edit rebuilds a strict subset and reads strictly fewer samples.
	surface.updateSamples(std::vector<SampleEdit>{
		SampleEdit(Vector3DInt32(2, 2, 2), -99.0f) });
	const LodSurfaceStats after = surface.lastStats();
	TASK_CHECK(after.rebuiltBlocks >= 1);
	TASK_CHECK(after.rebuiltBlocks < 8);
	TASK_CHECK(after.sourceReads > 0);
	TASK_CHECK(after.sourceReads < fullReads);
	TASK_CHECK(surface.blockRevision(8) == farRevision);

	// mesh() never re-reads the source: repeated calls leave stats untouched.
	const LodSurfaceStats beforeMesh = surface.lastStats();
	const Mesh<Vertex<float> > m1 = surface.mesh();
	const Mesh<Vertex<float> > m2 = surface.mesh();
	TASK_CHECK(surface.lastStats().sourceReads == beforeMesh.sourceReads);
	TASK_CHECK(m1.getNoOfVertices() == m2.getNoOfVertices());

	// Invalid layout is rejected and preserves revisions and statistics.
	bool threw = false;
	try
	{
		std::vector<LodBlock> bad;
		bad.push_back(LodBlock(9, Vector3DInt32(0, 0, 0), 3, 1)); // odd cells
		surface.setLayout(bad);
	}
	catch (const std::invalid_argument&) { threw = true; }
	TASK_CHECK(threw);
	TASK_CHECK(surface.blockRevision(8) == farRevision);
	TASK_CHECK(surface.lastStats().sourceReads == after.sourceReads);
	return 0;
}

static int reuse_invalid_layout_leaves_resource()
{
	Region r(-1, -1, -1, kBox + 1, kBox + 1, kBox + 1);
	const std::vector<float> values = sphereField(r, 8.0f, 8.0f, 8.0f);

	LodSurface surface(r, values);
	const std::vector<LodBlock> good = twoBlockLayout(0, 1);
	surface.setLayout(good);
	const Mesh<Vertex<float> > goodMesh = surface.mesh();

	struct BadCase { const char* name; std::vector<LodBlock> blocks; };
	std::vector<BadCase> cases;

	{
		BadCase c; c.name = "odd cells";
		c.blocks.push_back(LodBlock(1, Vector3DInt32(0, 0, 0), 3, 1));
		cases.push_back(c);
	}
	{
		BadCase c; c.name = "step not power of two";
		c.blocks.push_back(LodBlock(1, Vector3DInt32(0, 0, 0), 4, 3));
		cases.push_back(c);
	}
	{
		// Two coplanar neighbours whose steps differ by a factor of four.
		BadCase c; c.name = "bad step ratio";
		c.blocks.push_back(LodBlock(1, Vector3DInt32(0, 0, 0), 8, 1));
		c.blocks.push_back(LodBlock(2, Vector3DInt32(8, 0, 0), 2, 4));
		cases.push_back(c);
	}

	for (size_t i = 0; i < cases.size(); ++i)
	{
		bool threw = false;
		try { surface.setLayout(cases[i].blocks); }
		catch (const std::invalid_argument&) { threw = true; }
		TASK_CHECK(threw);
		TASK_CHECK(sameDirectedGeometry(goodMesh, surface.mesh()));
	}
	return 0;
}

const struct task_case task_cases[] =
{
	{ "seams", "six_orientations_plane", seams_six_orientations_plane },
	{ "seams", "six_orientations_sphere", seams_six_orientations_sphere },
	{ "seams", "multi_face_corner", seams_multi_face_corner },
	{ "seams", "interface_shared_vertices", seams_coarse_fine_interface_vertices },
	{ "seams", "three_level_chain", seams_three_level_chain },

	{ "edits", "local_propagation_and_normals", edits_local_propagation_and_normals },
	{ "edits", "cross_block_and_rollback", edits_cross_block_and_rollback },
	{ "edits", "two_interleaved_resources", edits_two_interleaved_resources },

	{ "reuse", "identical_and_reordered_layout", reuse_identical_and_reordered_layout },
	{ "reuse", "local_lod_switch_equivalence", reuse_local_lod_switch_equivalence },
	{ "reuse", "read_statistics_and_distant_blocks",
		reuse_read_statistics_and_distant_blocks },
	{ "reuse", "invalid_layout_leaves_resource",
		reuse_invalid_layout_leaves_resource }
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
