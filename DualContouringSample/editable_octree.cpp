/*
	Editable, incrementally refreshable octree for the Dual Contouring sample.

	The tree keeps the geometry of the finest (unit sized) cells for its whole
	lifetime.  Density changes are announced with Update(), which resamples
	only the unit cells whose evaluation footprint can touch the update box.
	Meshing builds a temporary node hierarchy from that persisted geometry and
	simplifies the temporary copy, so requesting a coarse threshold can never
	destroy the fine data.

	The contouring rules (edge crossing approximation, central difference
	normals, at most six crossing edges and QEF placement) intentionally mirror
	octree.cpp.  Only where the density values come from differs: here they are
	obtained from the user supplied closure instead of the global Density_Func.
*/

#include "editable_octree.h"
#include "octree.h"
#include "qef.h"

#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <utility>

// ----------------------------------------------------------------------------

namespace
{
	const int MATERIAL_AIR = 0;
	const int MATERIAL_SOLID = 1;

	const float QEF_ERROR = 1e-6f;
	const int QEF_SWEEPS = 4;

	// Same tables as octree.cpp.  They describe the dual contouring connection
	// rules and the corner ordering of a cell.
	const glm::ivec3 CHILD_MIN_OFFSETS[] =
	{
		glm::ivec3( 0, 0, 0 ),
		glm::ivec3( 0, 0, 1 ),
		glm::ivec3( 0, 1, 0 ),
		glm::ivec3( 0, 1, 1 ),
		glm::ivec3( 1, 0, 0 ),
		glm::ivec3( 1, 0, 1 ),
		glm::ivec3( 1, 1, 0 ),
		glm::ivec3( 1, 1, 1 ),
	};

	const int edgevmap[12][2] =
	{
		{0,4},{1,5},{2,6},{3,7},	// x-axis
		{0,2},{1,3},{4,6},{5,7},	// y-axis
		{0,1},{2,3},{4,5},{6,7}		// z-axis
	};

	// ------------------------------------------------------------------------

	struct CellKey
	{
		int x, y, z;

		bool operator==(const CellKey& other) const
		{
			return x == other.x && y == other.y && z == other.z;
		}
	};

	struct CellKeyHash
	{
		std::size_t operator()(const CellKey& k) const
		{
			const std::uint32_t a = static_cast<std::uint32_t>(k.x);
			const std::uint32_t b = static_cast<std::uint32_t>(k.y);
			const std::uint32_t c = static_cast<std::uint32_t>(k.z);
			return static_cast<std::size_t>(a * 73856093u ^
			                                b * 19349663u ^
			                                c * 83492791u);
		}
	};

	struct NodeKey
	{
		int x, y, z, size;

		bool operator==(const NodeKey& other) const
		{
			return x == other.x && y == other.y && z == other.z &&
			       size == other.size;
		}
	};

	struct NodeKeyHash
	{
		std::size_t operator()(const NodeKey& k) const
		{
			const std::uint32_t a = static_cast<std::uint32_t>(k.x);
			const std::uint32_t b = static_cast<std::uint32_t>(k.y);
			const std::uint32_t c = static_cast<std::uint32_t>(k.z);
			const std::uint32_t s = static_cast<std::uint32_t>(k.size);
			return static_cast<std::size_t>(a * 73856093u ^
			                                b * 19349663u ^
			                                c * 83492791u ^
			                                s * 2654435761u);
		}
	};

	struct LeafRecord
	{
		LeafRecord()
			: corners(0)
		{
		}

		int corners;
		glm::vec3 position;
		glm::vec3 averageNormal;
		svd::QefData qef;
	};

	// ------------------------------------------------------------------------

	bool IsPowerOfTwo(int value)
	{
		return value > 0 && (value & (value - 1)) == 0;
	}
}

// ----------------------------------------------------------------------------

struct EditableOctree::Impl
{
	Impl(const glm::ivec3& min,
	     int size,
	     std::function<float(const glm::vec3&)> densityFunc)
		: rootMin(min)
		, rootSize(size)
		, density(densityFunc)
		, currentCalls(0)
	{
	}

	// Counts every actual evaluation of the user field function.
	float Evaluate(const glm::vec3& p)
	{
		++currentCalls;
		return density(p);
	}

	glm::vec3 ApproximateZeroCrossingPosition(const glm::vec3& p0,
	                                          const glm::vec3& p1)
	{
		// Same scan as the upstream implementation.
		float minValue = 100000.f;
		float t = 0.f;
		float currentT = 0.f;
		const int steps = 8;
		const float increment = 1.f / (float)steps;
		while (currentT <= 1.f)
		{
			const glm::vec3 p = p0 + ((p1 - p0) * currentT);
			const float densityValue = glm::abs(Evaluate(p));
			if (densityValue < minValue)
			{
				minValue = densityValue;
				t = currentT;
			}

			currentT += increment;
		}

		return p0 + ((p1 - p0) * t);
	}

	glm::vec3 CalculateSurfaceNormal(const glm::vec3& p)
	{
		// The 0.001 offset means a cell's data also reads up to 0.001 outside
		// its own bounds.  Update() takes that halo into account.
		const float H = 0.001f;
		const float dx = Evaluate(p + glm::vec3(H, 0.f, 0.f)) -
		                 Evaluate(p - glm::vec3(H, 0.f, 0.f));
		const float dy = Evaluate(p + glm::vec3(0.f, H, 0.f)) -
		                 Evaluate(p - glm::vec3(0.f, H, 0.f));
		const float dz = Evaluate(p + glm::vec3(0.f, 0.f, H)) -
		                 Evaluate(p - glm::vec3(0.f, 0.f, H));

		return glm::normalize(glm::vec3(dx, dy, dz));
	}

	// Rebuilds the geometry of one unit cell.  Returns false when the cell is
	// entirely air or entirely solid, in which case no leaf is kept.
	bool BuildLeaf(const glm::ivec3& cellMin, LeafRecord& out)
	{
		int corners = 0;
		for (int i = 0; i < 8; i++)
		{
			const glm::ivec3 cornerPos = cellMin + CHILD_MIN_OFFSETS[i];
			const float densityValue = Evaluate(glm::vec3(cornerPos));
			const int material = densityValue < 0.f ? MATERIAL_SOLID : MATERIAL_AIR;
			corners |= (material << i);
		}

		if (corners == 0 || corners == 255)
		{
			// Voxel is completely inside or outside the volume.
			return false;
		}

		const int MAX_CROSSINGS = 6;
		int edgeCount = 0;
		glm::vec3 averageNormal(0.f);
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
				// No zero crossing on this edge.
				continue;
			}

			const glm::vec3 p1 = glm::vec3(cellMin + CHILD_MIN_OFFSETS[c1]);
			const glm::vec3 p2 = glm::vec3(cellMin + CHILD_MIN_OFFSETS[c2]);
			const glm::vec3 p = ApproximateZeroCrossingPosition(p1, p2);
			const glm::vec3 n = CalculateSurfaceNormal(p);
			qef.add(p.x, p.y, p.z, n.x, n.y, n.z);

			averageNormal += n;

			edgeCount++;
		}

		svd::Vec3 qefPosition;
		qef.solve(qefPosition, QEF_ERROR, QEF_SWEEPS, QEF_ERROR);

		out.corners = corners;
		out.position = glm::vec3(qefPosition.x, qefPosition.y, qefPosition.z);
		out.qef = qef.getData();

		const glm::vec3 min = glm::vec3(cellMin);
		const glm::vec3 max = glm::vec3(cellMin + glm::ivec3(1));
		if (out.position.x < min.x || out.position.x > max.x ||
			out.position.y < min.y || out.position.y > max.y ||
			out.position.z < min.z || out.position.z > max.z)
		{
			const auto& mp = qef.getMassPoint();
			out.position = glm::vec3(mp.x, mp.y, mp.z);
		}

		out.averageNormal = glm::normalize(averageNormal / (float)edgeCount);

		return true;
	}

	// ------------------------------------------------------------------------

	void BuildAllLeaves()
	{
		for (int z = 0; z < rootSize; z++)
		{
			for (int y = 0; y < rootSize; y++)
			{
				for (int x = 0; x < rootSize; x++)
				{
					++lastStats.rebuilt_leaves;
					const glm::ivec3 cellMin = rootMin + glm::ivec3(x, y, z);
					LeafRecord record;
					if (BuildLeaf(cellMin, record))
					{
						const CellKey key = { cellMin.x, cellMin.y, cellMin.z };
						leaves[key] = record;
					}
				}
			}
		}
	}

	// Rebuilds the cells whose evaluation footprint intersects the closed
	// update box.  A cell of min m evaluates the field at its corners and on
	// its edges (inside [m, m+1]) and central-difference normals up to 0.001
	// outside it, hence the halo.
	void RebuildRegion(const glm::vec3& lo, const glm::vec3& hi)
	{
		// Evaluation footprint of a unit cell is [m - H, m + 1 + H].  The
		// margin also covers the rounding of the edge parameter t.
		const double H = static_cast<double>(0.001f) + 0.001;
		const double loX = (double)glm::min(lo.x, hi.x);
		const double loY = (double)glm::min(lo.y, hi.y);
		const double loZ = (double)glm::min(lo.z, hi.z);
		const double hiX = (double)glm::max(lo.x, hi.x);
		const double hiY = (double)glm::max(lo.y, hi.y);
		const double hiZ = (double)glm::max(lo.z, hi.z);

		int x0 = (int)std::ceil(loX - 1.0 - H);
		int y0 = (int)std::ceil(loY - 1.0 - H);
		int z0 = (int)std::ceil(loZ - 1.0 - H);
		int x1 = (int)std::floor(hiX + H);
		int y1 = (int)std::floor(hiY + H);
		int z1 = (int)std::floor(hiZ + H);

		x0 = glm::max(x0, rootMin.x);
		y0 = glm::max(y0, rootMin.y);
		z0 = glm::max(z0, rootMin.z);
		x1 = glm::min(x1, rootMin.x + rootSize - 1);
		y1 = glm::min(y1, rootMin.y + rootSize - 1);
		z1 = glm::min(z1, rootMin.z + rootSize - 1);

		for (int z = z0; z <= z1; z++)
		{
			for (int y = y0; y <= y1; y++)
			{
				for (int x = x0; x <= x1; x++)
				{
					++lastStats.rebuilt_leaves;
					const glm::ivec3 cellMin(x, y, z);
					const CellKey key = { x, y, z };
					LeafRecord record;
					if (BuildLeaf(cellMin, record))
					{
						leaves[key] = record;
					}
					else
					{
						leaves.erase(key);
					}
				}
			}
		}
	}

	// ------------------------------------------------------------------------

	static void MarkAncestors(const glm::ivec3& cell,
	                          int rootSize,
	                          const glm::ivec3& rootMin,
	                          std::unordered_set<NodeKey, NodeKeyHash>& out)
	{
		const glm::ivec3 rel = cell - rootMin;
		for (int size = 2; size <= rootSize; size <<= 1)
		{
			const glm::ivec3 p = rootMin + glm::ivec3(
				(rel.x / size) * size,
				(rel.y / size) * size,
				(rel.z / size) * size);
			const NodeKey key = { p.x, p.y, p.z, size };
			out.insert(key);
		}
	}

	OctreeNode* BuildView(const glm::ivec3& nodeMin,
	                      int nodeSize,
	                      const std::unordered_set<NodeKey, NodeKeyHash>& ancestors) const
	{
		if (nodeSize == 1)
		{
			const CellKey key = { nodeMin.x, nodeMin.y, nodeMin.z };
			auto it = leaves.find(key);
			if (it == leaves.end())
			{
				return nullptr;
			}

			OctreeNode* leaf = new OctreeNode(Node_Leaf);
			leaf->min = nodeMin;
			leaf->size = 1;
			leaf->drawInfo = new OctreeDrawInfo;
			leaf->drawInfo->index = -1;
			leaf->drawInfo->corners = it->second.corners;
			leaf->drawInfo->position = it->second.position;
			leaf->drawInfo->averageNormal = it->second.averageNormal;
			leaf->drawInfo->qef = it->second.qef;
			return leaf;
		}

		const NodeKey key = { nodeMin.x, nodeMin.y, nodeMin.z, nodeSize };
		if (ancestors.find(key) == ancestors.end())
		{
			return nullptr;
		}

		OctreeNode* node = new OctreeNode(Node_Internal);
		node->min = nodeMin;
		node->size = nodeSize;

		const int childSize = nodeSize / 2;
		bool hasChild = false;
		for (int i = 0; i < 8; i++)
		{
			const glm::ivec3 childMin =
				nodeMin + CHILD_MIN_OFFSETS[i] * childSize;
			node->children[i] = BuildView(childMin, childSize, ancestors);
			hasChild |= (node->children[i] != nullptr);
		}

		if (!hasChild)
		{
			delete node;
			return nullptr;
		}

		return node;
	}

	// ------------------------------------------------------------------------

	glm::ivec3 rootMin;
	int rootSize;
	std::function<float(const glm::vec3&)> density;
	std::unordered_map<CellKey, LeafRecord, CellKeyHash> leaves;
	std::size_t currentCalls;
	EditableOctreeStats lastStats;
};

// ----------------------------------------------------------------------------

EditableOctree::EditableOctree(const glm::ivec3& min,
                               int size,
                               std::function<float(const glm::vec3&)> density)
{
	if (!IsPowerOfTwo(size))
	{
		throw std::invalid_argument("EditableOctree size must be a power of two");
	}
	if (!density)
	{
		throw std::invalid_argument("EditableOctree requires a density function");
	}

	impl_.reset(new Impl(min, size, density));
	impl_->currentCalls = 0;
	impl_->lastStats = EditableOctreeStats();
	impl_->BuildAllLeaves();
	impl_->lastStats.density_calls = impl_->currentCalls;
}

// ----------------------------------------------------------------------------

EditableOctree::~EditableOctree()
{
}

// ----------------------------------------------------------------------------

EditableOctree::EditableOctree(EditableOctree&& other)
	: impl_(std::move(other.impl_))
{
}

// ----------------------------------------------------------------------------

EditableOctree& EditableOctree::operator=(EditableOctree&& other)
{
	impl_ = std::move(other.impl_);
	return *this;
}

// ----------------------------------------------------------------------------

void EditableOctree::Update(const glm::vec3& lo, const glm::vec3& hi)
{
	impl_->currentCalls = 0;
	impl_->lastStats = EditableOctreeStats();
	impl_->RebuildRegion(lo, hi);
	impl_->lastStats.density_calls = impl_->currentCalls;
}

// ----------------------------------------------------------------------------

void EditableOctree::Mesh(float threshold,
                          VertexBuffer& vertexBuffer,
                          IndexBuffer& indexBuffer) const
{
	vertexBuffer.clear();
	indexBuffer.clear();

	if (impl_->leaves.empty())
	{
		return;
	}

	// Collect the internal nodes that have at least one surface cell below
	// them; the temporary view then has exactly the same topology as a full
	// build.
	std::unordered_set<NodeKey, NodeKeyHash> ancestors;
	for (auto it = impl_->leaves.begin(); it != impl_->leaves.end(); ++it)
	{
		const glm::ivec3 cell(it->first.x, it->first.y, it->first.z);
		Impl::MarkAncestors(cell, impl_->rootSize, impl_->rootMin, ancestors);
	}

	OctreeNode* root = impl_->BuildView(impl_->rootMin, impl_->rootSize, ancestors);
	if (!root)
	{
		return;
	}

	// Simplify a temporary copy only, exactly as BuildOctree does upstream.
	// The persistent leaf geometry is never touched, so any later Mesh() at a
	// different threshold still starts from the full fine model.  A negative
	// threshold never collapses anything (QEF errors are non-negative) but we
	// still call the same routine to keep the code path identical.
	root = SimplifyOctree(root, threshold);

	GenerateMeshFromOctree(root, vertexBuffer, indexBuffer);
	DestroyOctree(root);
}

// ----------------------------------------------------------------------------

EditableOctree::Stats EditableOctree::LastUpdateStats() const
{
	return impl_->lastStats;
}

// ----------------------------------------------------------------------------
