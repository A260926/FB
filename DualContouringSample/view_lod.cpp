/*

View-dependent level of detail on top of the Dual Contouring octree.

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

#include "view_lod.h"

#include "qef.h"

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <unordered_map>

// ----------------------------------------------------------------------------
// Rules shared with octree.cpp (kept file-local there, replicated here so the
// original BuildOctree/SimplifyOctree path stays untouched).

namespace
{

const int MATERIAL_AIR = 0;
const int MATERIAL_SOLID = 1;

const float QEF_ERROR = 1e-6f;
const int QEF_SWEEPS = 4;

const glm::ivec3 CHILD_MIN_OFFSETS[] =
{
	glm::ivec3(0, 0, 0),
	glm::ivec3(0, 0, 1),
	glm::ivec3(0, 1, 0),
	glm::ivec3(0, 1, 1),
	glm::ivec3(1, 0, 0),
	glm::ivec3(1, 0, 1),
	glm::ivec3(1, 1, 0),
	glm::ivec3(1, 1, 1),
};

const int edgevmap[12][2] =
{
	{0,4},{1,5},{2,6},{3,7},	// x-axis
	{0,2},{1,3},{4,6},{5,7},	// y-axis
	{0,1},{2,3},{4,5},{6,7}		// z-axis
};

const int cellProcFaceMask[12][3] = {{0,4,0},{1,5,0},{2,6,0},{3,7,0},{0,2,1},{4,6,1},{1,3,1},{5,7,1},{0,1,2},{2,3,2},{4,5,2},{6,7,2}} ;
const int cellProcEdgeMask[6][5] = {{0,1,2,3,0},{4,5,6,7,0},{0,4,1,5,1},{2,6,3,7,1},{0,2,4,6,2},{1,3,5,7,2}} ;

const int faceProcFaceMask[3][4][3] = {
	{{4,0,0},{5,1,0},{6,2,0},{7,3,0}},
	{{2,0,1},{6,4,1},{3,1,1},{7,5,1}},
	{{1,0,2},{3,2,2},{5,4,2},{7,6,2}}
} ;

const int faceProcEdgeMask[3][4][6] = {
	{{1,4,0,5,1,1},{1,6,2,7,3,1},{0,4,6,0,2,2},{0,5,7,1,3,2}},
	{{0,2,3,0,1,0},{0,6,7,4,5,0},{1,2,0,6,4,2},{1,3,1,7,5,2}},
	{{1,1,0,3,2,0},{1,5,4,7,6,0},{0,1,5,0,4,1},{0,3,7,2,6,1}}
};

const int edgeProcEdgeMask[3][2][5] = {
	{{3,2,1,0,0},{7,6,5,4,0}},
	{{5,1,4,0,1},{7,3,6,2,1}},
	{{6,4,2,0,2},{7,5,3,1,2}},
};

const int processEdgeMask[3][4] = {{3,2,1,0},{7,5,6,4},{11,10,9,8}} ;

enum SelectionState
{
	STATE_UNCOMPUTED = 0,
	STATE_EXPANDED = 1,
	STATE_COLLAPSED = 2,
};

// ----------------------------------------------------------------------------
// Density sampler: every real evaluation goes through here so the construction
// sample count is exact.

struct DensitySampler
{
	explicit DensitySampler(const std::function<float(const glm::vec3&)>& fn)
		: func(fn)
		, calls(0)
	{
	}

	float eval(const glm::vec3& p)
	{
		++calls;
		return func(p);
	}

	const std::function<float(const glm::vec3&)>&	func;
	size_t											calls;
};

// ----------------------------------------------------------------------------

glm::vec3 approximateZeroCrossing(DensitySampler& sampler, const glm::vec3& p0, const glm::vec3& p1)
{
	// same 8-step scan as ApproximateZeroCrossingPosition in octree.cpp
	float minValue = 100000.f;
	float t = 0.f;
	float currentT = 0.f;
	const int steps = 8;
	const float increment = 1.f / (float)steps;
	while (currentT <= 1.f)
	{
		const glm::vec3 p = p0 + ((p1 - p0) * currentT);
		const float density = glm::abs(sampler.eval(p));
		if (density < minValue)
		{
			minValue = density;
			t = currentT;
		}

		currentT += increment;
	}

	return p0 + ((p1 - p0) * t);
}

// ----------------------------------------------------------------------------

glm::vec3 calculateNormal(DensitySampler& sampler, const glm::vec3& p)
{
	const float H = 0.001f;
	const float dx = sampler.eval(p + glm::vec3(H, 0.f, 0.f)) - sampler.eval(p - glm::vec3(H, 0.f, 0.f));
	const float dy = sampler.eval(p + glm::vec3(0.f, H, 0.f)) - sampler.eval(p - glm::vec3(0.f, H, 0.f));
	const float dz = sampler.eval(p + glm::vec3(0.f, 0.f, H)) - sampler.eval(p - glm::vec3(0.f, 0.f, H));

	return glm::normalize(glm::vec3(dx, dy, dz));
}

} // namespace

// ----------------------------------------------------------------------------

struct ViewLod::LodDraw
{
	LodDraw()
		: index(-1)
		, corners(0)
		, position(0.f)
		, averageNormal(0.f)
		, qef()
	{
	}

	int				index;
	int				corners;
	glm::vec3		position;
	glm::vec3		averageNormal;
	svd::QefData	qef;
};

// ----------------------------------------------------------------------------

struct ViewLod::LodNode
{
	LodNode()
		: type(Node_None)
		, min(0)
		, size(0)
		, draw(nullptr)
		, id(-1)
		, bboxMin(0.f)
		, bboxMax(0.f)
	{
		for (int i = 0; i < 8; i++)
		{
			children[i] = nullptr;
		}
	}

	OctreeNodeType	type;
	glm::ivec3		min;
	int				size;
	LodNode*		children[8];
	LodDraw*		draw;
	int				id;
	glm::vec3		bboxMin;
	glm::vec3		bboxMax;
};

// ----------------------------------------------------------------------------

ViewLod::ViewLod(const ivec3& min, int size, std::function<float(const vec3&)> density)
	: root_(nullptr)
	, hasUpdate_(false)
{
	if (!density)
	{
		throw std::invalid_argument("ViewLod requires a density function");
	}

	if (size < 1 || (size & (size - 1)) != 0)
	{
		throw std::invalid_argument("ViewLod size must be a positive power of two");
	}

	size_t densityCalls = 0;
	root_ = buildRecursive(min, size, density, densityCalls);
	stats_.densityCalls = densityCalls;

	if (root_)
	{
		// stable ids give every selection pass value-semantic state vectors
		int nextId = 0;
		std::vector<LodNode*> stack(1, root_);
		while (!stack.empty())
		{
			LodNode* node = stack.back();
			stack.pop_back();
			node->id = nextId++;
			for (int i = 0; i < 8; i++)
			{
				if (node->children[i])
				{
					stack.push_back(node->children[i]);
				}
			}
		}

		buildCandidates(root_);

		const size_t nodeCount = static_cast<size_t>(nextId);
		collapsedState_.assign(nodeCount, 0);
		workingState_.assign(nodeCount, STATE_EXPANDED);
		forceFineState_.assign(nodeCount, 0);
	}
}

// ----------------------------------------------------------------------------

ViewLod::~ViewLod()
{
	destroyNode(root_);
}

// ----------------------------------------------------------------------------

ViewLod::LodNode* ViewLod::buildRecursive(const ivec3& nodeMin, int nodeSize,
                                          const std::function<float(const vec3&)>& density,
                                          size_t& densityCalls)
{
	DensitySampler sampler(density);

	if (nodeSize == 1)
	{
		int corners = 0;
		for (int i = 0; i < 8; i++)
		{
			const ivec3 cornerPos = nodeMin + CHILD_MIN_OFFSETS[i];
			const float value = sampler.eval(vec3(cornerPos));
			const int material = value < 0.f ? MATERIAL_SOLID : MATERIAL_AIR;
			corners |= (material << i);
		}

		if (corners == 0 || corners == 255)
		{
			// fully inside or fully outside the volume: no surface here
			densityCalls += sampler.calls;
			return nullptr;
		}

		const int MAX_CROSSINGS = 6;
		int edgeCount = 0;
		vec3 averageNormal(0.f);
		svd::QefSolver qef;

		for (int i = 0; i < 12 && edgeCount < MAX_CROSSINGS; i++)
		{
			const int c1 = edgevmap[i][0];
			const int c2 = edgevmap[i][1];

			const int m1 = (corners >> c1) & 1;
			const int m2 = (corners >> c2) & 1;

			if ((m1 == MATERIAL_AIR && m2 == MATERIAL_AIR) ||
				(m1 == MATERIAL_SOLID && m2 == MATERIAL_SOLID))
			{
				continue;
			}

			const vec3 p1 = vec3(nodeMin + CHILD_MIN_OFFSETS[c1]);
			const vec3 p2 = vec3(nodeMin + CHILD_MIN_OFFSETS[c2]);
			const vec3 p = approximateZeroCrossing(sampler, p1, p2);
			const vec3 n = calculateNormal(sampler, p);
			qef.add(p.x, p.y, p.z, n.x, n.y, n.z);

			averageNormal += n;
			edgeCount++;
		}

		densityCalls += sampler.calls;

		svd::Vec3 qefPosition;
		qef.solve(qefPosition, QEF_ERROR, QEF_SWEEPS, QEF_ERROR);

		LodNode* leaf = new LodNode;
		leaf->type = Node_Leaf;
		leaf->min = nodeMin;
		leaf->size = nodeSize;
		leaf->draw = new LodDraw;
		leaf->draw->position = vec3(qefPosition.x, qefPosition.y, qefPosition.z);
		leaf->draw->qef = qef.getData();

		const vec3 boxMin = vec3(nodeMin);
		const vec3 boxMax = vec3(nodeMin + ivec3(nodeSize));
		if (leaf->draw->position.x < boxMin.x || leaf->draw->position.x > boxMax.x ||
			leaf->draw->position.y < boxMin.y || leaf->draw->position.y > boxMax.y ||
			leaf->draw->position.z < boxMin.z || leaf->draw->position.z > boxMax.z)
		{
			// superbox fallback, identical to the original leaf construction
			const auto& mp = qef.getMassPoint();
			leaf->draw->position = vec3(mp.x, mp.y, mp.z);
		}

		leaf->draw->averageNormal = glm::normalize(averageNormal / (float)edgeCount);
		leaf->draw->corners = corners;
		leaf->bboxMin = leaf->draw->position;
		leaf->bboxMax = leaf->draw->position;

		return leaf;
	}

	const int childSize = nodeSize / 2;
	LodNode* node = new LodNode;
	node->type = Node_Internal;
	node->min = nodeMin;
	node->size = nodeSize;

	bool hasChildren = false;
	for (int i = 0; i < 8; i++)
	{
		const ivec3 childMin = nodeMin + (CHILD_MIN_OFFSETS[i] * childSize);
		node->children[i] = buildRecursive(childMin, childSize, density, densityCalls);
		hasChildren |= (node->children[i] != nullptr);
	}

	if (!hasChildren)
	{
		delete node;
		return nullptr;
	}

	return node;
}

// ----------------------------------------------------------------------------

void ViewLod::buildCandidates(LodNode* node)
{
	if (!node || node->type == Node_Leaf)
	{
		return;
	}

	for (int i = 0; i < 8; i++)
	{
		buildCandidates(node->children[i]);
	}

	// Accumulate exactly like the bottom-up pass of SimplifyOctree, but keep
	// the children alive instead of destroying them.
	svd::QefSolver qef;
	int signs[8] = { -1, -1, -1, -1, -1, -1, -1, -1 };
	int midsign = -1;

	for (int i = 0; i < 8; i++)
	{
		LodNode* child = node->children[i];
		if (!child)
		{
			continue;
		}

		qef.add(child->draw->qef);
		midsign = (child->draw->corners >> (7 - i)) & 1;
		signs[i] = (child->draw->corners >> i) & 1;
	}

	svd::Vec3 qefPosition;
	qef.solve(qefPosition, QEF_ERROR, QEF_SWEEPS, QEF_ERROR);
	vec3 position(qefPosition.x, qefPosition.y, qefPosition.z);

	const vec3 boxMin = vec3(node->min);
	const vec3 boxMax = vec3(node->min + ivec3(node->size));
	if (position.x < boxMin.x || position.x > boxMax.x ||
		position.y < boxMin.y || position.y > boxMax.y ||
		position.z < boxMin.z || position.z > boxMax.z)
	{
		const auto& mp = qef.getMassPoint();
		position = vec3(mp.x, mp.y, mp.z);
	}

	LodDraw* draw = new LodDraw;
	for (int i = 0; i < 8; i++)
	{
		if (signs[i] == -1)
		{
			draw->corners |= ((midsign == -1 ? 0 : midsign) << i);
		}
		else
		{
			draw->corners |= (signs[i] << i);
		}
	}

	draw->averageNormal = vec3(0.f);
	for (int i = 0; i < 8; i++)
	{
		if (node->children[i])
		{
			draw->averageNormal += node->children[i]->draw->averageNormal;
		}
	}
	draw->averageNormal = glm::normalize(draw->averageNormal);
	draw->position = position;
	draw->qef = qef.getData();

	node->draw = draw;

	// AABB over every non-empty finest descendant vertex.
	node->bboxMin = vec3(std::numeric_limits<float>::infinity());
	node->bboxMax = vec3(-std::numeric_limits<float>::infinity());
	for (int i = 0; i < 8; i++)
	{
		LodNode* child = node->children[i];
		if (!child)
		{
			continue;
		}

		node->bboxMin = glm::min(node->bboxMin, child->bboxMin);
		node->bboxMax = glm::max(node->bboxMax, child->bboxMax);
	}
}

// ----------------------------------------------------------------------------

void ViewLod::destroyNode(LodNode* node)
{
	if (!node)
	{
		return;
	}

	for (int i = 0; i < 8; i++)
	{
		destroyNode(node->children[i]);
	}

	delete node->draw;
	delete node;
}

// ----------------------------------------------------------------------------

size_t ViewLod::countNodes() const
{
	size_t count = 0;
	std::vector<const LodNode*> stack(1, root_);
	while (!stack.empty())
	{
		const LodNode* node = stack.back();
		stack.pop_back();
		++count;
		for (int i = 0; i < 8; i++)
		{
			if (node->children[i])
			{
				stack.push_back(node->children[i]);
			}
		}
	}

	return count;
}

// ----------------------------------------------------------------------------
// Selection
// ----------------------------------------------------------------------------

bool ViewLod::nearPlaneTouches(const LodNode* node, const LodCamera& camera) const
{
	// smallest camera-space depth attained anywhere in the AABB
	const vec3 centre = (node->bboxMin + node->bboxMax) * 0.5f;
	const vec3 extents = (node->bboxMax - node->bboxMin) * 0.5f;
	const float minDepth = glm::dot(centre - camera.eye, camera.forward)
	                     - glm::dot(extents, glm::abs(camera.forward));
	return minDepth <= camera.nearDistance;
}

// ----------------------------------------------------------------------------

float ViewLod::projectedError(const LodNode* node, const LodCamera& camera) const
{
	const auto project = [&camera](const vec3& p) -> glm::vec2
	{
		const vec3 rel = p - camera.eye;
		const float depth = glm::dot(rel, camera.forward);
		return glm::vec2(
			camera.focalPixels * glm::dot(rel, camera.right) / depth,
			camera.focalPixels * glm::dot(rel, camera.up) / depth);
	};

	const glm::vec2 qp = project(node->draw->position);
	float error = 0.f;
	for (int ix = 0; ix < 2; ix++)
	{
		for (int iy = 0; iy < 2; iy++)
		{
			for (int iz = 0; iz < 2; iz++)
			{
				const vec3 corner(
					ix == 0 ? node->bboxMin.x : node->bboxMax.x,
					iy == 0 ? node->bboxMin.y : node->bboxMax.y,
					iz == 0 ? node->bboxMin.z : node->bboxMax.z);
				const glm::vec2 pp = project(corner);
				error = glm::max(error, glm::length(pp - qp));
			}
		}
	}

	return error;
}

// ----------------------------------------------------------------------------

void ViewLod::selectRecursive(LodNode* node,
                              const LodCamera& camera,
                              float collapsePixels, float splitPixels,
                              std::vector<char>& state,
                              size_t& errorTests)
{
	if (!node || state[node->id] != STATE_UNCOMPUTED)
	{
		return;
	}

	if (node->type == Node_Leaf)
	{
		state[node->id] = STATE_EXPANDED;
		return;
	}

	// bottom-up: descendants are selected first, every update
	for (int i = 0; i < 8; i++)
	{
		selectRecursive(node->children[i], camera,
		                collapsePixels, splitPixels, state, errorTests);
	}

	const bool hardBlocked =
		nearPlaneTouches(node, camera) ||
		node->draw->corners == 0 || node->draw->corners == 255;

	const bool wasCollapsed = hasUpdate_ && collapsedState_[node->id] != 0;

	if (hardBlocked)
	{
		// near-plane box intersection and all-air/all-solid candidates may
		// never collapse, hysteresis or not
		state[node->id] = STATE_EXPANDED;
		return;
	}

	const float error = projectedError(node, camera);
	++errorTests;

	if (wasCollapsed)
	{
		// hysteresis: a collapsed node stays until the split threshold is passed
		state[node->id] = (error > splitPixels) ? STATE_EXPANDED : STATE_COLLAPSED;
	}
	else
	{
		state[node->id] = (error <= collapsePixels) ? STATE_COLLAPSED : STATE_EXPANDED;
	}
}

// ----------------------------------------------------------------------------

void ViewLod::collectFrontier(const LodNode* node, const std::vector<char>& state,
                              const std::vector<char>& forceFine,
                              std::vector<const LodNode*>& out) const
{
	if (!node)
	{
		return;
	}

	if (node->type != Node_Leaf &&
	    (forceFine.empty() || forceFine[node->id] == 0) &&
	    (state.empty() || state[node->id] == STATE_COLLAPSED))
	{
		out.push_back(node);
		return;
	}

	if (node->type == Node_Leaf)
	{
		out.push_back(node);
		return;
	}

	for (int i = 0; i < 8; i++)
	{
		collectFrontier(node->children[i], state, forceFine, out);
	}
}

// ----------------------------------------------------------------------------

namespace
{

uint64_t cellKey(const glm::ivec3& rootMin, const glm::ivec3& cellMin)
{
	const uint64_t bits = 21;
	const uint64_t mask = (uint64_t(1) << bits) - 1;
	const uint64_t dx = (uint64_t)(cellMin.x - rootMin.x) & mask;
	const uint64_t dy = (uint64_t)(cellMin.y - rootMin.y) & mask;
	const uint64_t dz = (uint64_t)(cellMin.z - rootMin.z) & mask;
	return dx | (dy << bits) | (dz << (2 * bits));
}

} // namespace

void ViewLod::balance(std::vector<const LodNode*>& cells,
                      const std::vector<char>& state,
                      std::vector<char>& forceFine,
                      size_t& balanceSplits)
{
	const ivec3 rootMin = root_->min;
	const int rootSize = root_->size;

	bool changed = true;
	while (changed)
	{
		changed = false;

		std::unordered_map<uint64_t, const LodNode*> occupied;
		occupied.reserve(cells.size() * 2);
		for (const LodNode* cell : cells)
		{
			occupied[cellKey(rootMin, cell->min)] = cell;
		}

		for (const LodNode* cell : cells)
		{
			const int s = cell->size;

			for (int axis = 0; axis < 3; axis++)
			{
				for (int sign = -1; sign <= 1; sign += 2)
				{
					// equal-sized neighbour across this face: already balanced
					{
						ivec3 nMin = cell->min;
						nMin[axis] += sign * s;
						auto it = occupied.find(cellKey(rootMin, nMin));
						if (it != occupied.end() && it->second->size == s)
						{
							continue;
						}
					}

					// 2:1 balance: a face neighbour exactly twice the cell
					// size is legal; only coarser neighbours are split.
					const vec3 centre = vec3(cell->min) + vec3(float(s) * 0.5f);
					const float faceCoord = sign > 0
						? float(cell->min[axis] + s)
						: float(cell->min[axis]);
					for (int larger = s * 4; larger <= rootSize; larger *= 2)
					{
						vec3 probe = centre;
						probe[axis] = faceCoord + sign * 1e-3f;

						ivec3 nMin;
						for (int k = 0; k < 3; k++)
						{
							const int rel = int(std::floor((probe[k] - float(rootMin[k])) / float(larger)));
							nMin[k] = rootMin[k] + rel * larger;
						}

						// outside the sampled cube: there is no neighbour there
						bool insideDomain = true;
						for (int k = 0; k < 3; k++)
						{
							if (nMin[k] < rootMin[k] || nMin[k] + larger > rootMin[k] + rootSize)
							{
								insideDomain = false;
							}
						}

						if (!insideDomain)
						{
							continue;
						}

						auto it = occupied.find(cellKey(rootMin, nMin));
						if (it == occupied.end() || it->second->size != larger)
						{
							continue;
						}

						const LodNode* neighbour = it->second;

						// require a shared face with positive area; cells that
						// only meet on an edge or a corner must not constrain us
						const bool faceShared = sign > 0
							? (nMin[axis] == cell->min[axis] + s)
							: (nMin[axis] + larger == cell->min[axis]);
						if (!faceShared)
						{
							continue;
						}

						bool positiveArea = true;
						for (int k = 0; k < 3; k++)
						{
							if (k == axis)
							{
								continue;
							}

							if (nMin[k] >= cell->min[k] + s || nMin[k] + larger <= cell->min[k])
							{
								positiveArea = false;
								break;
							}
						}

						if (positiveArea && forceFine[neighbour->id] == 0)
						{
							forceFine[neighbour->id] = 1;
							++balanceSplits;
							changed = true;
						}
					}
				}
			}
		}

		if (changed)
		{
			cells.clear();
			collectFrontier(root_, state, forceFine, cells);
		}
	}
}

// ----------------------------------------------------------------------------

void ViewLod::update(const LodCamera& camera, float collapsePixels, float splitPixels)
{
	const float cameraValues[] =
	{
		camera.eye.x, camera.eye.y, camera.eye.z,
		camera.right.x, camera.right.y, camera.right.z,
		camera.up.x, camera.up.y, camera.up.z,
		camera.forward.x, camera.forward.y, camera.forward.z,
		camera.focalPixels, camera.nearDistance,
	};
	for (float value : cameraValues)
	{
		if (!std::isfinite(value))
		{
			throw std::invalid_argument("ViewLod::update received a non-finite camera parameter");
		}
	}

	if (!std::isfinite(collapsePixels) || !std::isfinite(splitPixels) ||
	    collapsePixels < 0.f || !(collapsePixels < splitPixels))
	{
		throw std::invalid_argument("ViewLod::update requires 0 <= collapsePixels < splitPixels");
	}

	if (!(camera.focalPixels > 0.f) || !(camera.nearDistance > 0.f))
	{
		throw std::invalid_argument("ViewLod::update requires positive focalPixels and nearDistance");
	}

	if (!root_)
	{
		frontier_.clear();
		stats_.errorTests = 0;
		stats_.balanceSplits = 0;
		hasUpdate_ = true;
		return;
	}

	const size_t nodeCount = countNodes();

	if (collapsedState_.size() != nodeCount)
	{
		collapsedState_.assign(nodeCount, 0);
		workingState_.assign(nodeCount, STATE_EXPANDED);
		forceFineState_.assign(nodeCount, 0);
	}

	// work on local copies first: an exception leaves the old frontier intact
	std::vector<char> state(nodeCount, STATE_UNCOMPUTED);
	std::vector<char> forceFine(nodeCount, 0);
	std::vector<const LodNode*> cells;

	size_t errorTests = 0;
	selectRecursive(root_, camera, collapsePixels, splitPixels, state, errorTests);
	collectFrontier(root_, state, forceFine, cells);

	size_t balanceSplits = 0;
	balance(cells, state, forceFine, balanceSplits);

	// commit: hysteresis is remembered on this object; balance-forced
	// refinements explicitly leave the node expanded for next time
	for (size_t id = 0; id < nodeCount; id++)
	{
		collapsedState_[id] =
			(state[id] == STATE_COLLAPSED && forceFine[id] == 0) ? 1 : 0;
	}

	workingState_ = std::move(state);
	forceFineState_ = std::move(forceFine);
	frontier_ = std::move(cells);
	stats_.errorTests = errorTests;
	stats_.balanceSplits = balanceSplits;
	hasUpdate_ = true;
}

// ----------------------------------------------------------------------------
// Mesh generation: the original mixed-depth ContourCell/Face/EdgeProc
// recursion, adapted to the permanent LodNode hierarchy.  A node behaves as a
// pseudo leaf exactly when it is a cell of the current frontier.
// ----------------------------------------------------------------------------

bool ViewLod::isFrontierCell(const LodNode* node,
                             const std::vector<char>& working,
                             const std::vector<char>& forceFine)
{
	if (node->type == Node_Leaf)
	{
		return true;
	}

	return forceFine[node->id] == 0 && working[node->id] == STATE_COLLAPSED;
}

// ----------------------------------------------------------------------------

void ViewLod::contourProcessEdge(const LodNode* node[4], int dir, IndexBuffer& indexBuffer)
{
	int minSize = 1000000;
	int minIndex = 0;
	int indices[4] = { -1, -1, -1, -1 };
	bool flip = false;
	bool signChange[4] = { false, false, false, false };

	for (int i = 0; i < 4; i++)
	{
		const int edge = processEdgeMask[dir][i];
		const int c1 = edgevmap[edge][0];
		const int c2 = edgevmap[edge][1];

		const int m1 = (node[i]->draw->corners >> c1) & 1;
		const int m2 = (node[i]->draw->corners >> c2) & 1;

		if (node[i]->size < minSize)
		{
			minSize = node[i]->size;
			minIndex = i;
			flip = m1 != MATERIAL_AIR;
		}

		indices[i] = node[i]->draw->index;
		signChange[i] =
			(m1 == MATERIAL_AIR && m2 != MATERIAL_AIR) ||
			(m1 != MATERIAL_AIR && m2 == MATERIAL_AIR);
	}

	if (signChange[minIndex])
	{
		if (!flip)
		{
			indexBuffer.push_back(indices[0]);
			indexBuffer.push_back(indices[1]);
			indexBuffer.push_back(indices[3]);

			indexBuffer.push_back(indices[0]);
			indexBuffer.push_back(indices[3]);
			indexBuffer.push_back(indices[2]);
		}
		else
		{
			indexBuffer.push_back(indices[0]);
			indexBuffer.push_back(indices[3]);
			indexBuffer.push_back(indices[1]);

			indexBuffer.push_back(indices[0]);
			indexBuffer.push_back(indices[2]);
			indexBuffer.push_back(indices[3]);
		}
	}
}

// ----------------------------------------------------------------------------

void ViewLod::contourEdgeProc(const LodNode* node[4], int dir, IndexBuffer& indexBuffer,
                              const std::vector<char>& working,
                              const std::vector<char>& forceFine)
{
	if (!node[0] || !node[1] || !node[2] || !node[3])
	{
		return;
	}

	if (isFrontierCell(node[0], working, forceFine) &&
	    isFrontierCell(node[1], working, forceFine) &&
	    isFrontierCell(node[2], working, forceFine) &&
	   	isFrontierCell(node[3], working, forceFine))
	{
		contourProcessEdge(node, dir, indexBuffer);
		return;
	}

	for (int i = 0; i < 2; i++)
	{
		const LodNode* edgeNodes[4];
		const int c[4] =
		{
			edgeProcEdgeMask[dir][i][0],
			edgeProcEdgeMask[dir][i][1],
			edgeProcEdgeMask[dir][i][2],
			edgeProcEdgeMask[dir][i][3],
		};

		for (int j = 0; j < 4; j++)
		{
			if (isFrontierCell(node[j], working, forceFine))
			{
				edgeNodes[j] = node[j];
			}
			else
			{
				edgeNodes[j] = node[j]->children[c[j]];
			}
		}

		contourEdgeProc(edgeNodes, edgeProcEdgeMask[dir][i][4], indexBuffer, working, forceFine);
	}
}

// ----------------------------------------------------------------------------

void ViewLod::contourFaceProc(const LodNode* node[2], int dir, IndexBuffer& indexBuffer,
                              const std::vector<char>& working,
                              const std::vector<char>& forceFine)
{
	if (!node[0] || !node[1])
	{
		return;
	}

	if (isFrontierCell(node[0], working, forceFine) &&
	    isFrontierCell(node[1], working, forceFine))
	{
		return;
	}

	for (int i = 0; i < 4; i++)
	{
		const LodNode* faceNodes[2];
		const int c[2] =
		{
			faceProcFaceMask[dir][i][0],
			faceProcFaceMask[dir][i][1],
		};

		for (int j = 0; j < 2; j++)
		{
			if (isFrontierCell(node[j], working, forceFine))
			{
				faceNodes[j] = node[j];
			}
			else
			{
				faceNodes[j] = node[j]->children[c[j]];
			}
		}

		contourFaceProc(faceNodes, faceProcFaceMask[dir][i][2], indexBuffer, working, forceFine);
	}

	const int orders[2][4] =
	{
		{ 0, 0, 1, 1 },
		{ 0, 1, 0, 1 },
	};
	for (int i = 0; i < 4; i++)
	{
		const LodNode* edgeNodes[4];
		const int c[4] =
		{
			faceProcEdgeMask[dir][i][1],
			faceProcEdgeMask[dir][i][2],
			faceProcEdgeMask[dir][i][3],
			faceProcEdgeMask[dir][i][4],
		};

		const int* order = orders[faceProcEdgeMask[dir][i][0]];
		for (int j = 0; j < 4; j++)
		{
			const LodNode* from = node[order[j]];
			if (isFrontierCell(from, working, forceFine))
			{
				edgeNodes[j] = from;
			}
			else
			{
				edgeNodes[j] = from->children[c[j]];
			}
		}

		contourEdgeProc(edgeNodes, faceProcEdgeMask[dir][i][5], indexBuffer, working, forceFine);
	}
}

// ----------------------------------------------------------------------------

void ViewLod::contourCellProc(const LodNode* node, IndexBuffer& indexBuffer,
                              const std::vector<char>& working,
                              const std::vector<char>& forceFine)
{
	if (!node)
	{
		return;
	}

	if (isFrontierCell(node, working, forceFine))
	{
		return;
	}

	for (int i = 0; i < 8; i++)
	{
		contourCellProc(node->children[i], indexBuffer, working, forceFine);
	}

	for (int i = 0; i < 12; i++)
	{
		const LodNode* faceNodes[2];
		const int c[2] = { cellProcFaceMask[i][0], cellProcFaceMask[i][1] };

		faceNodes[0] = node->children[c[0]];
		faceNodes[1] = node->children[c[1]];

		contourFaceProc(faceNodes, cellProcFaceMask[i][2], indexBuffer, working, forceFine);
	}

	for (int i = 0; i < 6; i++)
	{
		const LodNode* edgeNodes[4];
		const int c[4] =
		{
			cellProcEdgeMask[i][0],
			cellProcEdgeMask[i][1],
			cellProcEdgeMask[i][2],
			cellProcEdgeMask[i][3],
		};

		for (int j = 0; j < 4; j++)
		{
			edgeNodes[j] = node->children[c[j]];
		}

		contourEdgeProc(edgeNodes, cellProcEdgeMask[i][4], indexBuffer, working, forceFine);
	}
}

// ----------------------------------------------------------------------------

void ViewLod::assignVertexIndices(const LodNode* node,
                                  const std::vector<char>& working,
                                  const std::vector<char>& forceFine,
                                  VertexBuffer& vertexBuffer)
{
	if (!node)
	{
		return;
	}

	if (isFrontierCell(node, working, forceFine))
	{
		node->draw->index = static_cast<int>(vertexBuffer.size());
		vertexBuffer.push_back(MeshVertex(node->draw->position, node->draw->averageNormal));
		return;
	}

	for (int i = 0; i < 8; i++)
	{
		assignVertexIndices(node->children[i], working, forceFine, vertexBuffer);
	}
}

// ----------------------------------------------------------------------------

void ViewLod::mesh(VertexBuffer& vertexBuffer, IndexBuffer& indexBuffer) const
{
	vertexBuffer.clear();
	indexBuffer.clear();

	if (!root_)
	{
		return;
	}

	// Before the first update the constructor-initialised vectors describe the
	// finest unit-leaf frontier; after an update they describe the committed one.
	assignVertexIndices(root_, workingState_, forceFineState_, vertexBuffer);
	contourCellProc(root_, indexBuffer, workingState_, forceFineState_);
}

// ----------------------------------------------------------------------------

std::vector<LodFrontierRecord> ViewLod::frontier() const
{
	std::vector<LodFrontierRecord> records;
	if (!root_)
	{
		return records;
	}

	std::vector<const LodNode*> cells;
	if (hasUpdate_)
	{
		cells = frontier_;
	}
	else
	{
		collectFrontier(root_, workingState_, forceFineState_, cells);
	}

	records.reserve(cells.size());
	for (const LodNode* cell : cells)
	{
		records.push_back(LodFrontierRecord(
			cell->min, cell->size, cell->draw->position, cell->draw->averageNormal));
	}

	std::sort(records.begin(), records.end(),
	          [](const LodFrontierRecord& a, const LodFrontierRecord& b)
	          {
	          	if (a.min.x != b.min.x) return a.min.x < b.min.x;
	          	if (a.min.y != b.min.y) return a.min.y < b.min.y;
	          	if (a.min.z != b.min.z) return a.min.z < b.min.z;
	          	return a.size < b.size;
	          });

	return records;
}

// ----------------------------------------------------------------------------

const LodStats& ViewLod::lastStats() const
{
	return stats_;
}
