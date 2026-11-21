#include "heightfield.h"
#include "scene.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

namespace hf_detail
{
	// Fixed quadtree node. Leaves store a square block of cells and their
	// four corner heights; internal nodes only own child pointers and bounds.
	struct HFNode
	{
		Bounds bounds;

		bool leaf;

		int originX;
		int originZ;
		int side;

		// internal nodes
		std::shared_ptr<const HFNode> children[4];

		// leaves: side*side cells, row major over the block
		std::vector<float> h00;
		std::vector<float> h10;
		std::vector<float> h01;
		std::vector<float> h11;
	};
}

namespace
{
	using hf_detail::HFNode;

	const int kLeafCells = 8;
	const double kRootEps = 1.0e-12;

	inline int LeafIndex(int side, int lx, int lz)
	{
		return lz*side + lx;
	}

	inline double AsDouble(float v)
	{
		return static_cast<double>(v);
	}

	// Ray versus axis aligned box, slab test in double precision.
	bool RayAABB(double ox, double oy, double oz, double dx, double dy, double dz,
		const Bounds& bounds, float tmax)
	{
		const double lx = AsDouble(bounds.lower.x);
		const double ly = AsDouble(bounds.lower.y);
		const double lz = AsDouble(bounds.lower.z);
		const double ux = AsDouble(bounds.upper.x);
		const double uy = AsDouble(bounds.upper.y);
		const double uz = AsDouble(bounds.upper.z);

		double tnear = 0.0;
		double tfar = AsDouble(tmax);

		const double o[3] = { ox, oy, oz };
		const double d[3] = { dx, dy, dz };
		const double lo[3] = { lx, ly, lz };
		const double hi[3] = { ux, uy, uz };

		for (int axis=0; axis < 3; ++axis)
		{
			if (std::fabs(d[axis]) < 1.0e-30)
			{
				if (o[axis] < lo[axis] || o[axis] > hi[axis])
					return false;
			}
			else
			{
				double inv = 1.0/d[axis];
				double t1 = (lo[axis]-o[axis])*inv;
				double t2 = (hi[axis]-o[axis])*inv;
				if (t1 > t2)
					std::swap(t1, t2);

				tnear = std::max(tnear, t1);
				tfar = std::min(tfar, t2);

				if (tnear > tfar)
					return false;
			}
		}

		return tfar >= 0.0;
	}

	// Solve c2*t^2 + c1*t + c0 = 0, appending the real roots. Tangent roots
	// (discriminant numerically zero) collapse to a single root.
	void SolveQuadratic(double c2, double c1, double c0, double roots[2], int& numRoots)
	{
		numRoots = 0;

		const double scale = std::max(1.0,
			std::max(std::fabs(c2), std::max(std::fabs(c1), std::fabs(c0))));

		if (std::fabs(c2) <= kRootEps*scale)
		{
			if (std::fabs(c1) <= kRootEps*scale)
				return;

			roots[numRoots++] = -c0/c1;
			return;
		}

		double disc = c1*c1 - 4.0*c2*c0;
		const double discTol = kRootEps*std::max(1.0,
			std::max(c1*c1, std::fabs(4.0*c2*c0)));

		if (disc < -discTol)
			return;

		if (disc < 0.0)
			disc = 0.0;

		const double sq = std::sqrt(disc);
		const double q = -0.5*(c1 + (c1 < 0.0 ? -sq : sq));

		roots[numRoots++] = q/c2;

		if (sq > 0.0)
			roots[numRoots++] = c0/q;
	}
}

// ---------------------------------------------------------------------------
// HeightFieldSnapshot
// ---------------------------------------------------------------------------

HeightFieldSnapshot::HeightFieldSnapshot()
	: root_(), n_(0), logN_(0), width_(0.0f), depth_(0.0f), version_(0)
{
}

HeightFieldSnapshot::~HeightFieldSnapshot()
{
}

HeightFieldSnapshot::HeightFieldSnapshot(const HeightFieldSnapshot& other)
	: root_(other.root_), n_(other.n_), logN_(other.logN_),
	  width_(other.width_), depth_(other.depth_), version_(other.version_)
{
}

HeightFieldSnapshot& HeightFieldSnapshot::operator=(const HeightFieldSnapshot& other)
{
	if (this != &other)
	{
		root_ = other.root_;
		n_ = other.n_;
		logN_ = other.logN_;
		width_ = other.width_;
		depth_ = other.depth_;
		version_ = other.version_;
	}
	return *this;
}

Bounds HeightFieldSnapshot::LocalBounds() const
{
	return root_ ? root_->bounds : Bounds();
}

namespace
{
	// Descend to the leaf owning cell (cx, cz).
	const HFNode* FindLeaf(const HFNode* node, int cx, int cz)
	{
		while (!node->leaf)
		{
			const int half = node->side/2;
			const int right = cx >= node->originX + half ? 1 : 0;
			const int lower = cz >= node->originZ + half ? 1 : 0;
			node = node->children[lower*2 + right].get();
		}
		return node;
	}

	// Locate the canonical cell for a local (x,z) point. Shared grid edges
	// resolve to the smaller row, then smaller column cell; the outer
	// boundary keeps its unique cell. Returns the leaf, the cell's local
	// index and bilinear parameters.
	struct CanonicalLocation
	{
		const HFNode* leaf;
		int index;
		double su;
		double sv;
	};

	CanonicalLocation LocateCanonical(const HFNode* root, int n,
		float width, float depth, double x, double z)
	{
		const double cellW = AsDouble(width)/static_cast<double>(n);
		const double cellD = AsDouble(depth)/static_cast<double>(n);

		int cx = int(x/cellW);
		int cz = int(z/cellD);
		cx = std::min(std::max(cx, 0), n-1);
		cz = std::min(std::max(cz, 0), n-1);

		double su = x/cellW - static_cast<double>(cx);
		double sv = z/cellD - static_cast<double>(cz);

		// points exactly on a shared edge move to the lower index cell
		if (su <= 1.0e-9 && cx > 0)
		{
			--cx;
			su = 1.0;
		}
		if (sv <= 1.0e-9 && cz > 0)
		{
			--cz;
			sv = 1.0;
		}

		const HFNode* leaf = FindLeaf(root, cx, cz);
		const int index = LeafIndex(leaf->side, cx - leaf->originX, cz - leaf->originZ);

		CanonicalLocation location;
		location.leaf = leaf;
		location.index = index;
		location.su = su;
		location.sv = sv;
		return location;
	}
}

float HeightFieldSnapshot::VertexHeight(int x, int z) const
{
	if (!root_ || x < 0 || z < 0 || x > n_ || z > n_)
		return 0.0f;

	const int cx = std::min(x, n_-1);
	const int cz = std::min(z, n_-1);
	const HFNode* leaf = FindLeaf(root_.get(), cx, cz);

	const int lx = cx - leaf->originX;
	const int lz = cz - leaf->originZ;
	const int idx = LeafIndex(leaf->side, lx, lz);

	if (x == cx && z == cz)
		return leaf->h00[idx];
	if (x == cx+1 && z == cz)
		return leaf->h10[idx];
	if (x == cx && z == cz+1)
		return leaf->h01[idx];

	return leaf->h11[idx];
}

bool HeightFieldSnapshot::Intersect(const Vec3& origin, const Vec3& dir, float tmax,
	HeightFieldHit& outHit, HeightFieldQueryStats* outStats) const
{
	HeightFieldQueryStats stats;

	if (!root_)
	{
		if (outStats)
			*outStats = stats;
		return false;
	}

	const double ox = AsDouble(origin.x);
	const double oy = AsDouble(origin.y);
	const double oz = AsDouble(origin.z);
	const double dx = AsDouble(dir.x);
	const double dy = AsDouble(dir.y);
	const double dz = AsDouble(dir.z);

	const double cellW = AsDouble(width_)/static_cast<double>(n_);
	const double cellD = AsDouble(depth_)/static_cast<double>(n_);
	const double invCellW = 1.0/cellW;
	const double invCellD = 1.0/cellD;

	const HFNode* stack[128];
	int stackCount = 0;
	stack[stackCount++] = root_.get();

	double bestT = AsDouble(tmax);
	bool found = false;

	int bestCellX = 0;
	int bestCellZ = 0;
	double bestSu = 0.0;
	double bestSv = 0.0;

	while (stackCount > 0)
	{
		const HFNode* node = stack[--stackCount];
		++stats.nodesVisited;

		if (!RayAABB(ox, oy, oz, dx, dy, dz, node->bounds, float(bestT)))
			continue;

		if (!node->leaf)
		{
			stack[stackCount++] = node->children[0].get();
			stack[stackCount++] = node->children[1].get();
			stack[stackCount++] = node->children[2].get();
			stack[stackCount++] = node->children[3].get();
			continue;
		}

		++stats.leavesTested;

		const int side = node->side;

		for (int lz=0; lz < side; ++lz)
		{
			for (int lx=0; lx < side; ++lx)
			{
				const int idx = LeafIndex(side, lx, lz);
				const int cx = node->originX + lx;
				const int cz = node->originZ + lz;

				const double h00 = AsDouble(node->h00[idx]);
				const double h10 = AsDouble(node->h10[idx]);
				const double h01 = AsDouble(node->h01[idx]);
				const double h11 = AsDouble(node->h11[idx]);

				// h(su,sv) = A + B*su + C*sv + D*su*sv
				const double A = h00;
				const double B = h10 - h00;
				const double C = h01 - h00;
				const double D = h11 - h10 - h01 + h00;

				const double alpha0 = ox*invCellW - static_cast<double>(cx);
				const double alpha1 = dx*invCellW;
				const double beta0 = oz*invCellD - static_cast<double>(cz);
				const double beta1 = dz*invCellD;

				// F(t) = oy + dy*t - h(su(t), sv(t)) = 0, rewritten as
				// c2*t^2 + c1*t + c0 = 0
				const double c2 = D*alpha1*beta1;
				const double c1 = D*(alpha0*beta1 + alpha1*beta0)
					+ B*alpha1 + C*beta1 - dy;
				const double c0 = D*alpha0*beta0 + B*alpha0 + C*beta0 + A - oy;

				double roots[2];
				int numRoots = 0;
				SolveQuadratic(c2, c1, c0, roots, numRoots);

				for (int r=0; r < numRoots; ++r)
				{
					const double t = roots[r];

					if (!(t > 0.0) || t >= bestT)
						continue;

					const double su = alpha0 + alpha1*t;
					const double sv = beta0 + beta1*t;

					// shared edges are valid for both adjacent cells
					if (su < -kRootEps || su > 1.0 + kRootEps ||
						sv < -kRootEps || sv > 1.0 + kRootEps)
						continue;

					bestT = t;
					found = true;
					bestCellX = cx;
					bestCellZ = cz;
					bestSu = std::min(1.0, std::max(0.0, su));
					bestSv = std::min(1.0, std::max(0.0, sv));
				}
			}
		}
	}

	if (outStats)
		*outStats = stats;

	if (!found)
		return false;

	// Resolve the canonical cell for edge derivatives: shared grid edges
	// resolve to the smaller row, then smaller column cell; the outer
	// boundary keeps its unique cell.
	const double hitX = ox + dx*bestT;
	const double hitZ = oz + dz*bestT;
	const double hitY = oy + dy*bestT;

	const CanonicalLocation location =
		LocateCanonical(root_.get(), n_, width_, depth_, hitX, hitZ);

	const HFNode* leaf = location.leaf;
	const int idx = location.index;

	const double h00 = AsDouble(leaf->h00[idx]);
	const double h10 = AsDouble(leaf->h10[idx]);
	const double h01 = AsDouble(leaf->h01[idx]);
	const double h11 = AsDouble(leaf->h11[idx]);

	const double B = h10 - h00;
	const double C = h01 - h00;
	const double D = h11 - h10 - h01 + h00;

	const double hxD = (B + D*location.sv)*invCellW;
	const double hzD = (C + D*location.su)*invCellD;

	double invLen = 1.0/std::sqrt(1.0 + hxD*hxD + hzD*hzD);
	const Vec3 normal(float(-hxD*invLen), float(invLen), float(-hzD*invLen));

	outHit.t = float(bestT);
	outHit.cellX = bestCellX;
	outHit.cellZ = bestCellZ;
	outHit.u = float(bestSu);
	outHit.v = float(bestSv);
	outHit.position = Vec3(float(hitX), float(hitY), float(hitZ));
	outHit.normal = normal;

	return true;
}

void HeightFieldSnapshot::Derivatives(float u, float v, float& outDhDx, float& outDhDz) const
{
	if (!root_)
	{
		outDhDx = 0.0f;
		outDhDz = 0.0f;
		return;
	}

	u = std::min(1.0f, std::max(0.0f, u));
	v = std::min(1.0f, std::max(0.0f, v));

	const double x = AsDouble(u)*AsDouble(width_);
	const double z = AsDouble(v)*AsDouble(depth_);
	const double cellW = AsDouble(width_)/static_cast<double>(n_);
	const double cellD = AsDouble(depth_)/static_cast<double>(n_);

	const CanonicalLocation location =
		LocateCanonical(root_.get(), n_, width_, depth_, x, z);

	const double h00 = AsDouble(location.leaf->h00[location.index]);
	const double h10 = AsDouble(location.leaf->h10[location.index]);
	const double h01 = AsDouble(location.leaf->h01[location.index]);
	const double h11 = AsDouble(location.leaf->h11[location.index]);

	const double B = h10 - h00;
	const double C = h01 - h00;
	const double D = h11 - h10 - h01 + h00;

	outDhDx = float((B + D*location.sv)/cellW);
	outDhDz = float((C + D*location.su)/cellD);
}

void HeightFieldSnapshot::Sample(float u, float v, Vec3& outPosition, Vec3& outNormal) const
{
	if (!root_)
	{
		outPosition = Vec3(0.0f);
		outNormal = Vec3(0.0f, 1.0f, 0.0f);
		return;
	}

	u = std::min(1.0f, std::max(0.0f, u));
	v = std::min(1.0f, std::max(0.0f, v));

	const double x = AsDouble(u)*AsDouble(width_);
	const double z = AsDouble(v)*AsDouble(depth_);

	const CanonicalLocation location =
		LocateCanonical(root_.get(), n_, width_, depth_, x, z);

	const double h00 = AsDouble(location.leaf->h00[location.index]);
	const double h10 = AsDouble(location.leaf->h10[location.index]);
	const double h01 = AsDouble(location.leaf->h01[location.index]);
	const double h11 = AsDouble(location.leaf->h11[location.index]);

	const double B = h10 - h00;
	const double C = h01 - h00;
	const double D = h11 - h10 - h01 + h00;

	const double y = h00 + B*location.su + C*location.sv + D*location.su*location.sv;

	const double cellW = AsDouble(width_)/static_cast<double>(n_);
	const double cellD = AsDouble(depth_)/static_cast<double>(n_);
	const double hxD = (B + D*location.sv)/cellW;
	const double hzD = (C + D*location.su)/cellD;
	const double invLen = 1.0/std::sqrt(1.0 + hxD*hxD + hzD*hzD);

	outPosition = Vec3(float(x), float(y), float(z));
	outNormal = Vec3(float(-hxD*invLen), float(invLen), float(-hzD*invLen));
}

bool HeightFieldSnapshot::Inverse(const Vec3& localPosition, float& outU, float& outV) const
{
	if (!root_)
		return false;

	const float eps = 1.0e-5f*std::max(width_, depth_);

	if (localPosition.x < -eps || localPosition.x > width_ + eps ||
		localPosition.z < -eps || localPosition.z > depth_ + eps)
		return false;

	outU = std::min(1.0f, std::max(0.0f, localPosition.x/width_));
	outV = std::min(1.0f, std::max(0.0f, localPosition.z/depth_));
	return true;
}

float HeightFieldSnapshot::AreaDensity(float u, float v) const
{
	if (!root_)
		return 0.0f;

	float hx, hz;
	Derivatives(u, v, hx, hz);

	const double denom = AsDouble(width_)*AsDouble(depth_)
		*std::sqrt(1.0 + AsDouble(hx)*AsDouble(hx) + AsDouble(hz)*AsDouble(hz));

	return float(1.0/denom);
}

// ---------------------------------------------------------------------------
// HeightField
// ---------------------------------------------------------------------------

namespace
{
	int ComputeLog2(int n)
	{
		int logN = 0;
		while ((1 << logN) < n)
			++logN;
		return logN;
	}

	std::shared_ptr<HFNode> BuildLeaf(int originX, int originZ, int side,
		int n, float width, float depth, const std::vector<float>& heights)
	{
		std::shared_ptr<HFNode> node = std::make_shared<HFNode>();
		node->leaf = true;
		node->originX = originX;
		node->originZ = originZ;
		node->side = side;

		const int count = side*side;
		node->h00.resize(count);
		node->h10.resize(count);
		node->h01.resize(count);
		node->h11.resize(count);

		const double cellW = AsDouble(width)/static_cast<double>(n);
		const double cellD = AsDouble(depth)/static_cast<double>(n);

		double lowerX = DBL_MAX, lowerY = DBL_MAX, lowerZ = DBL_MAX;
		double upperX = -DBL_MAX, upperY = -DBL_MAX, upperZ = -DBL_MAX;

		for (int lz=0; lz < side; ++lz)
		{
			for (int lx=0; lx < side; ++lx)
			{
				const int cx = originX + lx;
				const int cz = originZ + lz;
				const int idx = LeafIndex(side, lx, lz);

				const float v00 = heights[cz*(n+1) + cx];
				const float v10 = heights[cz*(n+1) + cx + 1];
				const float v01 = heights[(cz+1)*(n+1) + cx];
				const float v11 = heights[(cz+1)*(n+1) + cx + 1];

				node->h00[idx] = v00;
				node->h10[idx] = v10;
				node->h01[idx] = v01;
				node->h11[idx] = v11;

				const double x0 = static_cast<double>(cx)*cellW;
				const double x1 = static_cast<double>(cx+1)*cellW;
				const double z0 = static_cast<double>(cz)*cellD;
				const double z1 = static_cast<double>(cz+1)*cellD;

				lowerX = std::min(lowerX, x0);
				upperX = std::max(upperX, x1);
				lowerZ = std::min(lowerZ, z0);
				upperZ = std::max(upperZ, z1);

				const double y0 = std::min(std::min(AsDouble(v00), AsDouble(v10)),
					std::min(AsDouble(v01), AsDouble(v11)));
				const double y1 = std::max(std::max(AsDouble(v00), AsDouble(v10)),
					std::max(AsDouble(v01), AsDouble(v11)));

				lowerY = std::min(lowerY, y0);
				upperY = std::max(upperY, y1);
			}
		}

		node->bounds = Bounds(
			Vec3(float(lowerX), float(lowerY), float(lowerZ)),
			Vec3(float(upperX), float(upperY), float(upperZ)));

		return node;
	}

	std::shared_ptr<HFNode> BuildTree(int originX, int originZ, int side,
		int n, float width, float depth, const std::vector<float>& heights)
	{
		if (side <= kLeafCells)
			return BuildLeaf(originX, originZ, side, n, width, depth, heights);

		const int half = side/2;

		std::shared_ptr<HFNode> node = std::make_shared<HFNode>();
		node->leaf = false;
		node->originX = originX;
		node->originZ = originZ;
		node->side = side;

		node->children[0] = BuildTree(originX,        originZ,        half, n, width, depth, heights);
		node->children[1] = BuildTree(originX + half, originZ,        half, n, width, depth, heights);
		node->children[2] = BuildTree(originX,        originZ + half, half, n, width, depth, heights);
		node->children[3] = BuildTree(originX + half, originZ + half, half, n, width, depth, heights);

		Bounds bounds = node->children[0]->bounds;
		for (int i=1; i < 4; ++i)
			bounds = Union(bounds, node->children[i]->bounds);
		node->bounds = bounds;

		return node;
	}
}

HeightField::HeightField(int N, float width, float depth, const float* heights)
	: n_(N), logN_(0), width_(width), depth_(depth), version_(1)
{
	if (N < 2 || (N & (N-1)) != 0)
		throw std::invalid_argument("HeightField: N must be a power of two >= 2");

	if (!std::isfinite(width) || !std::isfinite(depth) || width <= 0.0f || depth <= 0.0f)
		throw std::invalid_argument("HeightField: width and depth must be finite and positive");

	if (heights == NULL)
		throw std::invalid_argument("HeightField: heights must not be null");

	const int numVertices = (N+1)*(N+1);
	for (int i=0; i < numVertices; ++i)
	{
		if (!std::isfinite(heights[i]))
			throw std::invalid_argument("HeightField: heights must be finite");
	}

	logN_ = ComputeLog2(N);
	heights_.assign(heights, heights + numVertices);
	root_ = BuildTree(0, 0, N, N, width, depth, heights_);
}

HeightField::~HeightField()
{
}

bool HeightField::Update(const HeightEdit* edits, int count, HeightUpdateStats& outStats)
{
	outStats = HeightUpdateStats();

	if (count < 0 || (count > 0 && edits == NULL))
		return false;

	if (count == 0)
		return true;

	// validate the entire batch before touching anything
	for (int i=0; i < count; ++i)
	{
		if (edits[i].x < 0 || edits[i].x > n_ ||
			edits[i].z < 0 || edits[i].z > n_ ||
			!std::isfinite(edits[i].height))
			return false;
	}

	// fold repeated coordinates, last entry wins
	std::unordered_map<int, int> keyToIndex;
	std::vector<HeightEdit> merged;
	merged.reserve(static_cast<size_t>(count));

	for (int i=0; i < count; ++i)
	{
		const int key = edits[i].z*(n_+1) + edits[i].x;
		auto found = keyToIndex.find(key);
		if (found == keyToIndex.end())
		{
			keyToIndex.emplace(key, int(merged.size()));
			merged.push_back(edits[i]);
		}
		else
		{
			merged[found->second] = edits[i];
		}
	}

	// keep only entries that actually change the heights
	std::vector<HeightEdit> changed;
	changed.reserve(merged.size());
	for (size_t i=0; i < merged.size(); ++i)
	{
		const int index = merged[i].z*(n_+1) + merged[i].x;
		if (merged[i].height != heights_[index])
			changed.push_back(merged[i]);
	}

	// empty edit or no net changes: no new version
	if (changed.empty())
		return true;

	// collect the distinct leaf cells touched by the edited vertices directly:
	// every interior vertex touches four cells, so this stays proportional to
	// the edit count rather than scanning the whole N*N field
	std::unordered_set<int> touchedCells;
	touchedCells.reserve(changed.size()*4);

	for (size_t i=0; i < changed.size(); ++i)
	{
		heights_[changed[i].z*(n_+1) + changed[i].x] = changed[i].height;

		const int x0 = std::max(0, changed[i].x - 1);
		const int x1 = std::min(n_-1, changed[i].x);
		const int z0 = std::max(0, changed[i].z - 1);
		const int z1 = std::min(n_-1, changed[i].z);

		for (int cz=z0; cz <= z1; ++cz)
			for (int cx=x0; cx <= x1; ++cx)
				touchedCells.insert(cz*n_ + cx);
	}

	// mark ancestor paths of every touched leaf
	std::unordered_set<uint64_t> markedNodes;
	const int leafSide = std::min(kLeafCells, n_);

	auto nodeKey = [](int level, int ox, int oz) -> uint64_t
	{
		return (uint64_t(level) << 40) | (uint64_t(oz) << 20) | uint64_t(ox);
	};

	for (int cell : touchedCells)
	{
		const int cx = cell % n_;
		const int cz = cell / n_;

		int ox = (cx/leafSide)*leafSide;
		int oz = (cz/leafSide)*leafSide;
		int side = leafSide;
		int level = logN_ - ComputeLog2(leafSide);

		markedNodes.insert(nodeKey(level, ox, oz));

		while (side < n_)
		{
			side *= 2;
			ox = (ox/side)*side;
			oz = (oz/side)*side;
			--level;
			markedNodes.insert(nodeKey(level, ox, oz));
		}
	}

	// Only nodes present in markedNodes are ever rebuilt; unchanged subtrees
	// keep their exact old shared_ptr, which is what makes snapshots cheap
	// and lets released versions free their rebuilt history.
	struct RebuildCounter
	{
		RebuildCounter() : leaves(0), nodes(0) {}
		int leaves;
		int nodes;
	};

	RebuildCounter counter;

	std::function<std::shared_ptr<const HFNode>(
		const std::shared_ptr<const HFNode>&, int, int, int)> rebuild;
	rebuild = [&](const std::shared_ptr<const HFNode>& oldNode,
		int originX, int originZ, int side)
		-> std::shared_ptr<const HFNode>
	{
		if (side <= leafSide)
		{
			++counter.leaves;
			return BuildLeaf(originX, originZ, side, n_, width_, depth_, heights_);
		}

		const int half = side/2;
		const int origins[4][2] =
		{
			{ originX,        originZ },
			{ originX + half, originZ },
			{ originX,        originZ + half },
			{ originX + half, originZ + half },
		};

		std::shared_ptr<const HFNode> children[4];

		for (int k=0; k < 4; ++k)
		{
			const int level = logN_ - ComputeLog2(half);
			if (markedNodes.count(nodeKey(level, origins[k][0], origins[k][1])) == 0)
			{
				children[k] = oldNode->children[k];
			}
			else
			{
				children[k] = rebuild(oldNode->children[k],
					origins[k][0], origins[k][1], half);
			}
		}

		++counter.nodes;

		std::shared_ptr<HFNode> node = std::make_shared<HFNode>();
		node->leaf = false;
		node->originX = originX;
		node->originZ = originZ;
		node->side = side;
		for (int k=0; k < 4; ++k)
			node->children[k] = children[k];

		Bounds bounds = node->children[0]->bounds;
		for (int k=1; k < 4; ++k)
			bounds = Union(bounds, node->children[k]->bounds);
		node->bounds = bounds;

		return node;
	};

	root_ = rebuild(root_, 0, 0, n_);
	++version_;

	outStats.leavesRecomputed = counter.leaves;
	outStats.nodesRecomputed = counter.nodes;

	return true;
}

HeightFieldSnapshot HeightField::Snapshot() const
{
	HeightFieldSnapshot snapshot;
	snapshot.root_ = root_;
	snapshot.n_ = n_;
	snapshot.logN_ = logN_;
	snapshot.width_ = width_;
	snapshot.depth_ = depth_;
	snapshot.version_ = version_;
	return snapshot;
}

// ---------------------------------------------------------------------------
// Instance helpers
// ---------------------------------------------------------------------------

namespace
{
	const HeightFieldSnapshot& SnapshotOf(const Primitive& primitive)
	{
		return *primitive.heightfield.snapshot;
	}
}

bool HeightFieldPrimitiveIntersect(const Primitive& primitive,
	const Transform& transform, const Vec3& origin, const Vec3& dir,
	float& outT, Vec3* outNormal, float* outU, float* outV,
	int* outCellX, int* outCellZ)
{
	const HeightFieldSnapshot& snapshot = SnapshotOf(primitive);

	// height field instances only support positive uniform scale
	if (!(transform.s > 0.0f))
		return false;

	const Vec3 localOrigin = InverseTransformPoint(transform, origin);
	const Vec3 localDir = InverseTransformVector(transform, dir);

	HeightFieldHit hit;
	if (!snapshot.Intersect(localOrigin, localDir, REAL_MAX, hit))
		return false;

	outT = hit.t;

	if (outNormal)
		*outNormal = SafeNormalize(TransformVector(transform, hit.normal));
	if (outU)
		*outU = hit.u;
	if (outV)
		*outV = hit.v;
	if (outCellX)
		*outCellX = hit.cellX;
	if (outCellZ)
		*outCellZ = hit.cellZ;

	return true;
}

void HeightFieldPrimitiveSample(const Primitive& primitive,
	const Transform& transform, float u, float v,
	Vec3& outPosition, Vec3& outNormal)
{
	const HeightFieldSnapshot& snapshot = SnapshotOf(primitive);

	Vec3 localPosition;
	Vec3 localNormal;
	snapshot.Sample(u, v, localPosition, localNormal);

	outPosition = TransformPoint(transform, localPosition);
	outNormal = SafeNormalize(TransformVector(transform, localNormal));
}

bool HeightFieldPrimitiveInverse(const Primitive& primitive,
	const Transform& transform, const Vec3& worldPosition,
	float& outU, float& outV)
{
	const HeightFieldSnapshot& snapshot = SnapshotOf(primitive);
	const Vec3 localPosition = InverseTransformPoint(transform, worldPosition);
	return snapshot.Inverse(localPosition, outU, outV);
}

float HeightFieldPrimitiveAreaDensity(const Primitive& primitive,
	const Transform& transform, float u, float v)
{
	const HeightFieldSnapshot& snapshot = SnapshotOf(primitive);
	const float localDensity = snapshot.AreaDensity(u, v);
	return localDensity/(transform.s*transform.s);
}
