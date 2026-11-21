#pragma once

// Editable bilinear height field with immutable, persistent snapshots.
//
// The field covers N*N cells from (N+1)*(N+1) row-major heights, N being a
// power of two >= 2. Local coordinates are x in [0,W], z in [0,D] and each
// cell is a bilinear patch y = h(x,z) with front normal (-dh/dx, 1, -dh/dz).
//
// HeightField owns a fixed quadtree and supports local, persistent edits:
// unchanged subtrees are shared between versions through shared ownership,
// snapshots keep their version alive independently and no history is retained
// once the old snapshots are released.

#include "maths.h"

#include <memory>
#include <vector>

namespace hf_detail
{
	struct HFNode;
}

struct HeightEdit
{
	CUDA_CALLABLE HeightEdit() : x(0), z(0), height(0.0f) {}
	CUDA_CALLABLE HeightEdit(int x, int z, float height) : x(x), z(z), height(height) {}

	int x;
	int z;
	float height;
};

struct HeightUpdateStats
{
	HeightUpdateStats() : leavesRecomputed(0), nodesRecomputed(0) {}

	// number of leaf cells actually rebuilt this update
	int leavesRecomputed;
	// number of internal quadtree nodes actually created/recomputed this update
	int nodesRecomputed;
};

struct HeightFieldHit
{
	HeightFieldHit() : t(0.0f), cellX(0), cellZ(0), u(0.0f), v(0.0f) {}

	// ray parameter of the closest positive root
	float t;
	// canonical cell owning the hit (shared edges belong to the lower row,
	// then lower column cell; the outer boundary uses its unique cell)
	int cellX;
	int cellZ;
	// bilinear parameters inside the canonical cell, both in [0,1]
	float u;
	float v;
	// local space hit position and geometric normal
	Vec3 position;
	Vec3 normal;
};

struct HeightFieldQueryStats
{
	HeightFieldQueryStats() : nodesVisited(0), leavesTested(0) {}

	int nodesVisited;
	int leavesTested;
};

class HeightField;

class HeightFieldSnapshot
{
public:
	HeightFieldSnapshot();
	~HeightFieldSnapshot();

	HeightFieldSnapshot(const HeightFieldSnapshot&);
	HeightFieldSnapshot& operator=(const HeightFieldSnapshot&);

	CUDA_CALLABLE bool Valid() const { return root_ != NULL; }
	CUDA_CALLABLE int Version() const { return version_; }
	CUDA_CALLABLE int N() const { return n_; }
	CUDA_CALLABLE float Width() const { return width_; }
	CUDA_CALLABLE float Depth() const { return depth_; }

	// local space axis aligned bounds of this version
	Bounds LocalBounds() const;
	float VertexHeight(int x, int z) const;

	// Intersect a local space ray (it does not need to be unit length);
	// only roots with 0 < t <= tmax are considered. All valid roots of every
	// visited cell equation are examined and the closest positive root wins.
	bool Intersect(const Vec3& origin, const Vec3& dir, float tmax,
		HeightFieldHit& outHit, HeightFieldQueryStats* outStats=NULL) const;

	// Uniform parameter sampling over the whole local xz rectangle:
	// u, v in [0,1] map to x = u*W, z = v*D. Sampling and inversion share
	// the same convention.
	void Sample(float u, float v, Vec3& outPosition, Vec3& outNormal) const;
	bool Inverse(const Vec3& localPosition, float& outU, float& outV) const;

	// Surface derivatives at parameter (u,v).
	void Derivatives(float u, float v, float& outDhDx, float& outDhDz) const;

	// Local area density 1/(W*D*sqrt(1+hx*hx+hz*hz)). The world space area
	// density is this value divided by the squared instance scale.
	float AreaDensity(float u, float v) const;

private:
	friend class HeightField;

	std::shared_ptr<const hf_detail::HFNode> root_;

	int n_;
	int logN_;
	float width_;
	float depth_;
	int version_;
};

class HeightField
{
public:
	// Throws std::invalid_argument when N is not a power of two >= 2, W or D
	// are not finite and positive, heights is null or contains a non-finite
	// value. The height array is (N+1)*(N+1) floats in row major order.
	HeightField(int N, float width, float depth, const float* heights);
	~HeightField();

	CUDA_CALLABLE int N() const { return n_; }
	CUDA_CALLABLE float Width() const { return width_; }
	CUDA_CALLABLE float Depth() const { return depth_; }
	CUDA_CALLABLE int Version() const { return version_; }

	// Apply a batch of vertex edits. The whole batch is validated before
	// anything changes; repeated coordinates resolve to the last entry.
	// On failure the heights, tree, version and stats are left untouched.
	// An empty batch or one whose final values equal the current heights is
	// a no-op: no new version and zero reported work.
	bool Update(const HeightEdit* edits, int count, HeightUpdateStats& outStats);

	// Capture the current version. The snapshot owns everything it needs and
	// remains valid through later edits.
	HeightFieldSnapshot Snapshot() const;

private:
	int n_;
	int logN_;
	float width_;
	float depth_;
	int version_;

	std::vector<float> heights_;
	std::shared_ptr<const hf_detail::HFNode> root_;
};

struct Primitive;
struct Transform;

// Intersect a height field primitive in world space using the given
// interpolated instance transform. Optional outputs recover the canonical
// cell and its in-cell parameters for lighting calculations.
bool HeightFieldPrimitiveIntersect(const Primitive& primitive,
	const Transform& transform, const Vec3& origin, const Vec3& dir,
	float& outT, Vec3* outNormal, float* outU=NULL, float* outV=NULL,
	int* outCellX=NULL, int* outCellZ=NULL);

// Uniform parameter sampling of an instanced height field.
void HeightFieldPrimitiveSample(const Primitive& primitive,
	const Transform& transform, float u, float v,
	Vec3& outPosition, Vec3& outNormal);

// Recover the uniform (u,v) parameters of a world space point, using the
// same convention as HeightFieldPrimitiveSample.
bool HeightFieldPrimitiveInverse(const Primitive& primitive,
	const Transform& transform, const Vec3& worldPosition,
	float& outU, float& outV);

// World space area density at parameter (u,v): divides the local density by
// the squared instance scale.
float HeightFieldPrimitiveAreaDensity(const Primitive& primitive,
	const Transform& transform, float u, float v);
