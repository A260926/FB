/*

View-dependent level of detail on top of the Dual Contouring octree.

The fine unit-cell tree is sampled once at construction and never destroyed:
every internal node keeps the candidate vertex (corners, QEF, average normal)
that the original bottom-up SimplifyOctree pass would have produced, together
with an axis-aligned bounding box over all of its non-empty finest descendant
vertices.  update() only changes which nodes make up the current frontier; it
never calls the density function and never mutates the shared hierarchy, so the
hysteresis lives inside this object and two ViewLod instances never interfere.

Copyright (C) 2011  Tao Ju (original octree rules)

This library is free software; you can redistribute it and/or
modify it under the terms of the GNU Lesser General Public License
(LGPL) as published by the Free Software Foundation; either
version 2.1 of the License, or (at your option) any later version.

This library is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
Lesser General Public License for more details.

You should have received a copy of the GNU Lesser General Public
License along with this library; if not, write to the Free Software
Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
*/

#ifndef		HAS_VIEW_LOD_H_BEEN_INCLUDED
#define		HAS_VIEW_LOD_H_BEEN_INCLUDED

#include "mesh.h"
#include "octree.h"

#include <glm/glm.hpp>
using glm::vec3;
using glm::ivec3;

#include <cstddef>
#include <functional>
#include <vector>

// ----------------------------------------------------------------------------

// Orthonormal camera basis (right, up, forward are guaranteed by the caller to
// be mutually orthogonal unit vectors).  A world point p projects to
//   focalPixels * (dot(p-eye,right), dot(p-eye,up)) / dot(p-eye,forward).
struct LodCamera
{
	LodCamera()
		: eye(0.f)
		, right(1.f, 0.f, 0.f)
		, up(0.f, 1.f, 0.f)
		, forward(0.f, 0.f, 1.f)
		, focalPixels(1.f)
		, nearDistance(0.f)
	{
	}

	vec3	eye;
	vec3	right;
	vec3	up;
	vec3	forward;
	float	focalPixels;
	float	nearDistance;
};

// ----------------------------------------------------------------------------

// One cell of the current frontier.  Every non-empty fine leaf is covered by
// exactly one record and records never overlap.
struct LodFrontierRecord
{
	LodFrontierRecord()
		: min(0)
		, size(0)
		, position(0.f)
		, normal(0.f)
	{
	}

	LodFrontierRecord(const ivec3& _min, int _size, const vec3& _position, const vec3& _normal)
		: min(_min)
		, size(_size)
		, position(_position)
		, normal(_normal)
	{
	}

	ivec3	min;
	int		size;
	vec3	position;
	vec3	normal;
};

// ----------------------------------------------------------------------------

struct LodStats
{
	LodStats()
		: densityCalls(0)
		, errorTests(0)
		, balanceSplits(0)
	{
	}

	size_t	densityCalls;	// real density samples made during construction only
	size_t	errorTests;		// projection error evaluations in the last update
	size_t	balanceSplits;	// forced 2:1 balance refinements in the last update
};

// ----------------------------------------------------------------------------

class ViewLod
{
public:

	// Builds the full unit-cell tree for the [min, min+size] cube (size must be
	// a positive power of two).  The density functor is only used here and is
	// invoked with the same sampling rules as ConstructLeaf.
	ViewLod(const ivec3& min, int size, std::function<float(const vec3&)> density);
	~ViewLod();

	// Re-selects the frontier for the given camera.  Throws std::invalid_argument
	// on a non-finite argument, non-positive focalPixels/nearDistance or a
	// collapse/split pair that does not satisfy 0 <= collapse < split; the
	// previous frontier, mesh state and stats are left untouched in that case.
	void update(const LodCamera& camera, float collapsePixels, float splitPixels);

	// Runs the original mixed-depth ContourCell/Face/EdgeProc recursion over the
	// current frontier.  Repeated calls produce identical output and never change
	// the frontier; no density calls are made.
	void mesh(VertexBuffer& vertexBuffer, IndexBuffer& indexBuffer) const;

	// Frontier records sorted by (min.x, min.y, min.z, size).
	std::vector<LodFrontierRecord> frontier() const;

	const LodStats& lastStats() const;

private:

	ViewLod(const ViewLod&);
	ViewLod& operator=(const ViewLod&);

	struct LodNode;
	struct LodDraw;

	LodNode*				root_;
	std::vector<const LodNode*>	frontier_;
	std::vector<char>		collapsedState_;	// persistent hysteresis: 1 = currently collapsed
	std::vector<char>		workingState_;		// selection used by the last mesh/frontier
	std::vector<char>		forceFineState_;	// balance-forced refinements of the last update
	LodStats				stats_;
	bool					hasUpdate_;

	// Construction helpers (density is touched here only).
	LodNode*	buildRecursive(const ivec3& nodeMin, int nodeSize,
	                           const std::function<float(const vec3&)>& density,
	                           size_t& densityCalls);
	void		buildCandidates(LodNode* node);
	void		destroyNode(LodNode* node);
	size_t		countNodes() const;

	// Selection helpers.
	float		projectedError(const LodNode* node, const LodCamera& camera) const;
	bool		nearPlaneTouches(const LodNode* node, const LodCamera& camera) const;
	void		selectRecursive(LodNode* node,
	                            const LodCamera& camera,
	                            float collapsePixels, float splitPixels,
	                            std::vector<char>& state,
	                            size_t& errorTests);
	void		collectFrontier(const LodNode* node, const std::vector<char>& state,
	                            const std::vector<char>& forceFine,
	                            std::vector<const LodNode*>& out) const;
	void		balance(std::vector<const LodNode*>& cells,
	                    const std::vector<char>& state,
	                    std::vector<char>& forceFine,
	                    size_t& balanceSplits);

	// Original mixed-depth contour recursion over the current frontier.
	static bool	isFrontierCell(const LodNode* node,
	                           const std::vector<char>& working,
	                           const std::vector<char>& forceFine);
	static void	assignVertexIndices(const LodNode* node,
	                                const std::vector<char>& working,
	                                const std::vector<char>& forceFine,
	                                VertexBuffer& vertexBuffer);
	static void	contourProcessEdge(const LodNode* node[4], int dir, IndexBuffer& indexBuffer);
	static void	contourEdgeProc(const LodNode* node[4], int dir, IndexBuffer& indexBuffer,
	                            const std::vector<char>& working,
	                            const std::vector<char>& forceFine);
	static void	contourFaceProc(const LodNode* node[2], int dir, IndexBuffer& indexBuffer,
	                            const std::vector<char>& working,
	                            const std::vector<char>& forceFine);
	static void	contourCellProc(const LodNode* node, IndexBuffer& indexBuffer,
	                            const std::vector<char>& working,
	                            const std::vector<char>& forceFine);
};

// ----------------------------------------------------------------------------

#endif	// HAS_VIEW_LOD_H_BEEN_INCLUDED
