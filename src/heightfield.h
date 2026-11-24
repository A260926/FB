#pragma once

// Editable bilinear height field, traced directly as a smooth surface.
//
// The grid is N x N cells with (N+1) x (N+1) row-major heights, N a power of
// two. Each cell is a bilinear Coons patch y = h(x, z); rays intersect the
// exact quadratic cell equation (no triangle proxies, marching or single
// Newton steps). A fixed quadtree over the cells prunes ray traversal, and
// edits refresh only the touched leaves and their ancestors.
//
// Immutable snapshots own their data through shared ownership: old snapshots
// (and primitives holding them) keep seeing the old surface while new edits
// proceed, and releasing a snapshot drops the history it was the last user of.

#include "maths.h"

#include <memory>
#include <vector>
#include <cstdint>

namespace hf {

// One requested vertex change. Duplicate coordinates are allowed within a
// batch; the last entry for a coordinate wins.
struct Edit
{
	int x;
	int z;
	float height;
};

// Work actually performed by an update.
struct EditStats
{
	int leavesRecomputed;    // cells touching edited vertices
	int nodesRecomputed;     // hierarchy nodes created or recomputed
};

struct Hit
{
	float t;                 // ray parameter, non-unit rays supported
	int cellX;               // cell column in [0, N)
	int cellZ;               // cell row    in [0, N)
	float u;                 // in-cell parameter in [0, 1] along local x
	float v;                 // in-cell parameter in [0, 1] along local z
	Vec3 localPoint;         // hit point in height field local space
	Vec3 localNormal;        // unit geometric normal (-hx, 1, -hz)
};

// Persistent quadtree node. Bounds are conservative AABBs of every cell below
// the node, in local space. Each internal node shares ownership of its four
// children; an update allocates new nodes only for touched leaves and their
// ancestors and reuses every other subtree by shared pointer. Releasing a
// snapshot therefore frees exactly the nodes that generation alone was
// pinning, with no full-tree scan or rebuild.
struct HFNode
{
	Bounds bounds;
	std::shared_ptr<const HFNode> child[4]; // empty pointers mark a leaf node
	int cellX;                              // leaf: first cell column
	int cellZ;                              // leaf: first cell row
	int size;                               // node block edge length in cells

	HFNode() : cellX(0), cellZ(0), size(0) {}
	bool IsLeaf() const { return child[0] == nullptr; }
};

// Shared, immutable surface data. Snapshots are cheap to copy: they share the
// height rows and the persistent node graph with the live field and each
// other.
struct HFData
{
	HFData() : N(0), stride(0), W(0.0f), D(0.0f) {}

	int N;                   // cells per side, power of two >= 2
	int stride;              // N + 1
	float W;                 // local extent along x
	float D;                 // local extent along z

	// (N+1) x (N+1) row-major finite heights, kept as one immutable array per
	// row: an edit replaces only the rows it touches, so old snapshots retain
	// the old row arrays without a full height-array copy on every update.
	std::vector<std::shared_ptr<std::vector<float>>> rows;

	// Persistent quadtree root; transitively owns the reachable node graph.
	std::shared_ptr<const HFNode> root;

	int HeightIndex(int x, int z) const { return z*stride + x; }
	float Height(int x, int z) const { return (*rows[z])[x]; }
};

} // namespace hf

class HeightField
{
public:
	HeightField();

	// Returns false (and leaves the field empty) if the arguments are invalid:
	// N must be a power of two >= 2, W/D finite and positive, every height
	// finite, exactly (N+1)*(N+1) heights in row-major order.
	bool Initialize(int N, float W, float D, const float* heights, int count);

	bool Initialized() const { return static_cast<bool>(data_); }
	int Version() const { return version_; }
	int GridSize() const { return data_ ? data_->N : 0; }

	// Validate the whole batch first; on any invalid coordinate, non-finite
	// height or uninitialized field nothing changes: no allocation, no version
	// bump, zero work stats. An empty batch, or one whose final values all
	// equal the current heights, does not create a version.
	bool Update(const hf::Edit* edits, int count, hf::EditStats* stats);

	// Immutable view of the current generation. The snapshot keeps the data
	// alive independently of later edits.
	class Snapshot
	{
	public:
		Snapshot() {}
		explicit Snapshot(std::shared_ptr<hf::HFData> d) : data_(std::move(d)) {}

		bool Valid() const { return static_cast<bool>(data_); }
		int Version() const { return version_; }
		int GridSize() const { return data_ ? data_->N : 0; }
		float Width() const { return data_ ? data_->W : 0.0f; }
		float Depth() const { return data_ ? data_->D : 0.0f; }

		// Exact bilinear surface intersection in local space. All valid roots
		// of every visited cell equation are considered; the nearest positive
		// root is returned. Shared cell edges cannot be missed. Tangent
		// contacts and perfectly coplanar rays may be excluded.
		bool Intersect(const Vec3& origin, const Vec3& dir, hf::Hit* hit) const;

		// Surface position at a local xz point, clamped to the domain.
		Vec3 SamplePosition(float x, float z) const;

		// Unit geometric normal at a local xz point (edge convention: the cell
		// with the smaller row, then smaller column; the outer border uses the
		// single adjacent cell).
		Vec3 SampleNormal(float x, float z) const;

		// Map the unit-square sampling convention back to a surface point.
		Vec3 SampleParam(float u, float v) const;

		// Probability density of the unit-square sampling convention at the
		// given surface point, with respect to local surface area.
		float LocalAreaPdf(float x, float z) const;

		// Same density with respect to world surface area for an instance with
		// uniform scale s (> 0): local area maps to world area scaled by s^2.
		float WorldAreaPdf(float x, float z, float scale) const;

		const std::shared_ptr<hf::HFData>& Data() const { return data_; }

	private:
		std::shared_ptr<hf::HFData> data_;
		int version_ = 0;
		friend class HeightField;
	};

	Snapshot TakeSnapshot() const;

private:
	std::shared_ptr<hf::HFData> data_;
	int version_ = 0;
};

// Instance wiring: a primitive holds one snapshot through this wrapper.
// Multiple primitives may share a snapshot; the wrapper is non-trivial, so it
// lives outside the primitive's trivial geometry union.
struct HeightFieldGeometry
{
	HeightField::Snapshot snapshot;
};

// Primitive-level wiring; the definitions in heightfield.cpp require the full
// Primitive/Ray declarations from scene.h/intersection.h.
struct Primitive;
struct Ray;

void SetHeightField(Primitive& primitive, const HeightField::Snapshot& snapshot);
bool IsHeightField(const Primitive& primitive);
const HeightFieldGeometry& GetHeightField(const Primitive& primitive);

// Local-space bounds of a snapshot (the quadtree root bounds).
Bounds HeightFieldBounds(const HeightField::Snapshot& snapshot);

Bounds HeightFieldPrimitiveBounds(const Primitive& primitive);
bool HeightFieldPrimitiveIntersect(const Primitive& primitive, const Ray& ray,
                                   float& outT, Vec3* outNormal, hf::Hit* localHit = NULL);
void HeightFieldSample(const Primitive& primitive, float u, float v,
                       Vec3& worldPos, Vec3& worldNormal);

// World surface-area density of the uniform-parameter sampling convention at
// a world point on the instance (the point is transformed back into local
// space). Returns zero if the point is not over a valid snapshot.
float HeightFieldWorldAreaPdf(const Primitive& primitive, const Vec3& worldPoint, float time);

// Solid-angle density of the pointwise convention as seen from refPoint:
// world area density times the true connection distance squared, divided by
// the absolute cosine at the light point. A zero cosine cannot be connected
// (the contribution is zero); callers must treat that case separately.
float HeightFieldDirectionPdf(const Primitive& primitive, const Vec3& lightWorldPoint,
                               const Vec3& refPoint, float time);
