#include "heightfield.h"
#include "intersection.h"

#include <cmath>
#include <algorithm>
#include <cstring>
#include <unordered_set>
#include <functional>

namespace {

const float kHFMaxFloat = 3.402823466e+38f;

bool IsPowerOfTwo(int n) { return n >= 2 && (n & (n - 1)) == 0; }

// Exact AABB of a single cell from its four corner heights. A bilinear patch
// is bounded by the AABB of its four corners.
Bounds LeafBounds(const hf::HFData& d, int cx, int cz)
{
	const float dx = d.W / float(d.N);
	const float dz = d.D / float(d.N);

	const float h00 = d.Height(cx,   cz);
	const float h10 = d.Height(cx+1, cz);
	const float h01 = d.Height(cx,   cz+1);
	const float h11 = d.Height(cx+1, cz+1);

	const float ymin = std::min(std::min(h00, h10), std::min(h01, h11));
	const float ymax = std::max(std::max(h00, h10), std::max(h01, h11));

	Bounds b;
	b.lower = Vec3(cx*dx, ymin, cz*dz);
	b.upper = Vec3((cx+1)*dx, ymax, (cz+1)*dz);
	return b;
}

// Bilinear patch coefficients for cell (cx, cz):
// h(u, v) = a + bu*u + bv*v + buv*u*v, u,v in [0,1].
struct CellEq
{
	float a, bu, bv, buv;
};

CellEq MakeCellEq(const hf::HFData& d, int cx, int cz)
{
	const float h00 = d.Height(cx,   cz);
	const float h10 = d.Height(cx+1, cz);
	const float h01 = d.Height(cx,   cz+1);
	const float h11 = d.Height(cx+1, cz+1);
	CellEq e;
	e.a   = h00;
	e.bu  = h10 - h00;
	e.bv  = h01 - h00;
	e.buv = h00 - h10 - h01 + h11;
	return e;
}

// Slab test of an AABB against a non-unit ray. An axis with a zero direction
// component is handled explicitly. Entering/leaving parameters may be negative.
bool RayAABB(const Vec3& o, const Vec3& d, const Bounds& b, float& tEnter, float& tLeave)
{
	float lo = -kHFMaxFloat, hi = kHFMaxFloat;

	for (int axis = 0; axis < 3; ++axis)
	{
		const float p = o[axis];
		const float r = d[axis];
		const float bmin = b.lower[axis];
		const float bmax = b.upper[axis];

		if (fabsf(r) < 1.e-30f)
		{
			if (p < bmin || p > bmax)
				return false;
		}
		else
		{
			float t1 = (bmin - p) / r;
			float t2 = (bmax - p) / r;
			if (t1 > t2)
				std::swap(t1, t2);
			lo = std::max(lo, t1);
			hi = std::min(hi, t2);
			if (lo > hi)
				return false;
		}
	}

	tEnter = lo;
	tLeave = hi;
	return true;
}

// Evaluate all valid roots of the cell equation against ray (o, rd). The
// equation is quadratic in the ray parameter t; every real root is tested, so
// multi-root (e.g. saddle) cells are handled exactly. The nearest positive
// root inside the unit parameter square replaces best.
void IntersectCell(const hf::HFData& d, int cx, int cz, const CellEq& e,
                   const Vec3& o, const Vec3& rd, hf::Hit& best)
{
	const float dx = d.W / float(d.N);
	const float dz = d.D / float(d.N);

	// u(t) = u0 + ud*t, v(t) = v0 + vd*t
	const float u0 = (o.x - cx*dx) / dx;
	const float ud = rd.x / dx;
	const float v0 = (o.z - cz*dz) / dz;
	const float vd = rd.z / dz;

	// oy + rd.y*t = h(u(t), v(t))  =>  A*t^2 + B*t + C = 0
	const float A = e.buv * ud * vd;
	const float B = e.bu*ud + e.bv*vd + e.buv*(u0*vd + v0*ud) - rd.y;
	const float C = e.a + e.bu*u0 + e.bv*v0 + e.buv*u0*v0 - o.y;

	float roots[2];
	int numRoots = 0;

	if (fabsf(A) < 1.e-20f)
	{
		if (fabsf(B) < 1.e-30f)
			return; // perfectly coplanar/parallel rays are excluded
		roots[numRoots++] = -C / B;
	}
	else
	{
		const float disc = B*B - 4.0f*A*C;
		if (disc < 0.0f)
			return; // discriminant zero is an exact tangent contact, excluded
		const float sq = sqrtf(disc);
		// numerically stable root pair (Numerical Recipes style)
		const float q = -0.5f * (B + (B < 0.0f ? -sq : sq));
		roots[numRoots++] = q / A;
		roots[numRoots++] = C / q;
	}

	for (int i = 0; i < numRoots; ++i)
	{
		const float t = roots[i];
		if (!(t > 0.0f) || t >= best.t)
			continue;

		float u = u0 + ud*t;
		float v = v0 + vd*t;

		// shared edges are valid in both adjacent cells; a tolerance keeps a
		// numerically edge-hugging ray from falling through the crack
		const float eps = 1.e-5f;
		if (u < -eps || v < -eps || u > 1.0f + eps || v > 1.0f + eps)
			continue;

		u = std::min(1.0f, std::max(0.0f, u));
		v = std::min(1.0f, std::max(0.0f, v));

		best.t = t;
		best.cellX = cx;
		best.cellZ = cz;
		best.u = u;
		best.v = v;
		best.localPoint = o + rd*t;
		const float hx = (e.bu + e.buv*v) / dx;
		const float hz = (e.bv + e.buv*u) / dz;
		best.localNormal = SafeNormalize(Vec3(-hx, 1.0f, -hz));
	}
}

// Cell owning a local xz point under the edge convention: smaller row then
// smaller column wins; the outer border clamps to the single adjacent cell.
void CellAt(const hf::HFData& d, float x, float z, int& cx, int& cz, float& u, float& v)
{
	const float dx = d.W / float(d.N);
	const float dz = d.D / float(d.N);

	float fx = std::min(float(d.N - 1), std::max(0.0f, x / dx));
	float fz = std::min(float(d.N - 1), std::max(0.0f, z / dz));

	cx = int(fx);
	cz = int(fz);
	u = std::min(1.0f, std::max(0.0f, fx - float(cx)));
	v = std::min(1.0f, std::max(0.0f, fz - float(cz)));
}

// Pack a quadtree block into a membership key. Level 0 is the root.
int BlockKey(int level, int x0, int z0, int N)
{
	return (level*N + z0)*N + x0;
}

int LevelOf(int blockSize, int N)
{
	int level = 0;
	for (int s = blockSize; s < N; s *= 2)
		++level;
	return level;
}

} // namespace

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

HeightField::HeightField() {}

bool HeightField::Initialize(int N, float W, float D, const float* heights, int count)
{
	if (!IsPowerOfTwo(N))
		return false;
	if (!(W > 0.0f && std::isfinite(W)))
		return false;
	if (!(D > 0.0f && std::isfinite(D)))
		return false;
	const int stride = N + 1;
	if (count != stride*stride || heights == NULL)
		return false;
	for (int i = 0; i < count; ++i)
	{
		if (!std::isfinite(heights[i]))
			return false;
	}

	auto d = std::make_shared<hf::HFData>();
	d->N = N;
	d->stride = stride;
	d->W = W;
	d->D = D;

	d->rows.resize(stride);
	for (int z = 0; z < stride; ++z)
	{
		auto row = std::make_shared<std::vector<float>>(stride);
		std::memcpy(row->data(), heights + z*stride, stride*sizeof(float));
		d->rows[z] = row;
	}

	// full fixed quadtree: immutable nodes with exact bottom-up bounds, each
	// parent sharing ownership of its four children
	struct BuildState { int x0, z0, size; };
	std::function<std::shared_ptr<hf::HFNode>(const BuildState&)> build =
		[&](const BuildState& f) -> std::shared_ptr<hf::HFNode>
	{
		auto node = std::make_shared<hf::HFNode>();
		node->cellX = f.x0;
		node->cellZ = f.z0;
		node->size = f.size;

		if (f.size == 1)
		{
			node->bounds = LeafBounds(*d, f.x0, f.z0);
			return node;
		}

		const int half = f.size / 2;
		node->child[0] = build(BuildState{f.x0,        f.z0,        half});
		node->child[1] = build(BuildState{f.x0 + half, f.z0,        half});
		node->child[2] = build(BuildState{f.x0,        f.z0 + half, half});
		node->child[3] = build(BuildState{f.x0 + half, f.z0 + half, half});
		node->bounds = Union(node->child[0]->bounds, node->child[1]->bounds);
		node->bounds = Union(node->bounds, node->child[2]->bounds);
		node->bounds = Union(node->bounds, node->child[3]->bounds);
		return node;
	};
	d->root = build(BuildState{0, 0, N});

	data_ = d;
	version_ = 1;
	return true;
}

// ---------------------------------------------------------------------------
// Batch editing with persistent partial updates
// ---------------------------------------------------------------------------

bool HeightField::Update(const hf::Edit* edits, int count, hf::EditStats* statsOut)
{
	if (statsOut)
	{
		statsOut->leavesRecomputed = 0;
		statsOut->nodesRecomputed = 0;
	}
	if (!data_)
		return false;
	if (count < 0 || (count > 0 && edits == NULL))
		return false;

	hf::HFData& cur = *data_;
	const int N = cur.N;
	const int stride = cur.stride;

	// validate the entire batch before touching anything
	for (int i = 0; i < count; ++i)
	{
		const hf::Edit& e = edits[i];
		if (e.x < 0 || e.x >= stride || e.z < 0 || e.z >= stride)
			return false;
		if (!std::isfinite(e.height))
			return false;
	}

	if (count == 0)
		return true;

	// fold duplicates, last entry for a coordinate wins
	std::unordered_map<long long, int> lastIndex;
	lastIndex.reserve(count * 2);
	for (int i = 0; i < count; ++i)
		lastIndex[static_cast<long long>(edits[i].z)*stride + edits[i].x] = i;

	bool anyChange = false;
	for (const auto& kv : lastIndex)
	{
		const hf::Edit& e = edits[kv.second];
		if (e.height != cur.Height(e.x, e.z))
		{
			anyChange = true;
			break;
		}
	}
	if (!anyChange)
		return true; // no new version, no work, no allocation

	// copy only the rows that actually change
	std::vector<std::shared_ptr<std::vector<float>>> nextRows = cur.rows;
	std::vector<unsigned char> rowMarked(stride, 0);
	for (const auto& kv : lastIndex)
	{
		const hf::Edit& e = edits[kv.second];
		if (e.height == cur.Height(e.x, e.z))
			continue;
		if (!rowMarked[e.z])
		{
			rowMarked[e.z] = 1;
			nextRows[e.z] = std::make_shared<std::vector<float>>(*cur.rows[e.z]);
		}
		(*nextRows[e.z])[e.x] = e.height;
	}

	// mark leaves (cells) touching any changed vertex
	std::vector<unsigned char> cellMarked(N*N, 0);
	int leafCount = 0;
	for (const auto& kv : lastIndex)
	{
		const hf::Edit& e = edits[kv.second];
		if (e.height == cur.Height(e.x, e.z))
			continue;
		const int x0 = std::max(0, e.x - 1);
		const int x1 = std::min(N - 1, e.x);
		const int z0 = std::max(0, e.z - 1);
		const int z1 = std::min(N - 1, e.z);
		for (int cz = z0; cz <= z1; ++cz)
			for (int cx = x0; cx <= x1; ++cx)
			{
				const int idx = cz*N + cx;
				if (!cellMarked[idx])
				{
					cellMarked[idx] = 1;
					++leafCount;
				}
			}
	}

	// new immutable generation: share untouched rows; the tree is rebuilt
	// persistently below, reusing every untouched subtree
	auto next = std::make_shared<hf::HFData>(cur);
	next->rows = nextRows;

	// membership set of every block that must be (re)created: touched leaves
	// plus all their ancestors
	std::unordered_set<int> rebuildKeys;
	rebuildKeys.reserve(size_t(4) * N);
	struct Block { int x0, z0, size; };
	std::vector<Block> toRebuild;
	for (int cz = 0; cz < N; ++cz)
		for (int cx = 0; cx < N; ++cx)
			if (cellMarked[cz*N + cx])
			{
				toRebuild.push_back(Block{cx, cz, 1});
				rebuildKeys.insert(BlockKey(LevelOf(1, N), cx, cz, N));
			}

	for (int size = 1; size < N; size *= 2)
	{
		const int parentSize = size * 2;
		const int grid = N / parentSize;
		std::vector<unsigned char> parentMarked(grid*grid, 0);
		const int currentCount = int(toRebuild.size());
		for (int i = 0; i < currentCount; ++i)
		{
			const Block& b = toRebuild[i];
			if (b.size != size)
				continue;
			const int px = b.x0 / parentSize;
			const int pz = b.z0 / parentSize;
			const int pidx = pz*grid + px;
			if (!parentMarked[pidx])
			{
				parentMarked[pidx] = 1;
				const Block pb{px*parentSize, pz*parentSize, parentSize};
				toRebuild.push_back(pb);
				rebuildKeys.insert(BlockKey(LevelOf(parentSize, N), pb.x0, pb.z0, N));
			}
		}
	}

	// persistent rebuild: blocks outside rebuildKeys keep their existing node
	// (and whole subtree) by sharing the exact same per-node shared pointer;
	// only marked blocks allocate. Per-node ownership means a dropped
	// generation frees precisely the nodes nothing else references.
	int nodesRecomputed = 0;
	std::function<std::shared_ptr<const hf::HFNode>(
		const std::shared_ptr<const hf::HFNode>&, int, int, int)>
	rebuild = [&](const std::shared_ptr<const hf::HFNode>& oldNode,
	              int level, int x0, int z0)
		-> std::shared_ptr<const hf::HFNode>
	{
		const int key = BlockKey(level, x0, z0, N);
		if (rebuildKeys.find(key) == rebuildKeys.end())
			return oldNode; // share the unchanged subtree, no new allocation

		auto node = std::make_shared<hf::HFNode>();
		node->cellX = oldNode->cellX;
		node->cellZ = oldNode->cellZ;
		node->size = oldNode->size;
		++nodesRecomputed;

		if (node->size == 1)
		{
			node->bounds = LeafBounds(*next, x0, z0);
			return node;
		}

		const int half = node->size / 2;
		const int ox[4] = {0, half, 0, half};
		const int oz[4] = {0, 0, half, half};
		for (int k = 0; k < 4; ++k)
		{
			node->child[k] = rebuild(oldNode->child[k], level + 1,
			                         x0 + ox[k], z0 + oz[k]);
			node->bounds = (k == 0)
				? node->child[k]->bounds
				: Union(node->bounds, node->child[k]->bounds);
		}
		return node;
	};
	next->root = rebuild(cur.root, 0, 0, 0);

	// Nodes not reachable from next->root lose their owners here; snapshots of
	// older generations keep their own root graphs, and releasing them frees
	// their exclusive nodes automatically. No full height/tree copy occurs.
	data_ = next;
	++version_;

	if (statsOut)
	{
		statsOut->leavesRecomputed = leafCount;
		statsOut->nodesRecomputed = nodesRecomputed;
	}
	return true;
}

HeightField::Snapshot HeightField::TakeSnapshot() const
{
	Snapshot s(data_);
	s.version_ = version_;
	return s;
}

// ---------------------------------------------------------------------------
// Snapshot queries
// ---------------------------------------------------------------------------

bool HeightField::Snapshot::Intersect(const Vec3& origin, const Vec3& dir, hf::Hit* hitOut) const
{
	if (!data_)
		return false;
	const hf::HFData& d = *data_;

	hf::Hit best;
	best.t = kHFMaxFloat;

	std::vector<const hf::HFNode*> stack;
	stack.push_back(d.root.get());

	float tRootEnter, tRootLeave;
	if (!RayAABB(origin, dir, d.root->bounds, tRootEnter, tRootLeave))
		return false;

	while (!stack.empty())
	{
		const hf::HFNode* node = stack.back();
		stack.pop_back();

		float tEnter, tLeave;
		if (!RayAABB(origin, dir, node->bounds, tEnter, tLeave))
			continue;
		if (tLeave <= 0.0f || tEnter >= best.t)
			continue;

		if (node->IsLeaf())
		{
			// leaves always cover exactly one cell
			const CellEq e = MakeCellEq(d, node->cellX, node->cellZ);
			IntersectCell(d, node->cellX, node->cellZ, e, origin, dir, best);
		}
		else
		{
			// schedule far children first so nearer subtrees are visited first
			struct Child { const hf::HFNode* node; float tEnter; bool hit; };
			Child children[4];
			for (int k = 0; k < 4; ++k)
			{
				float te, tl;
				children[k].node = node->child[k].get();
				children[k].hit = RayAABB(origin, dir, node->child[k]->bounds, te, tl)
				                  && tl > 0.0f && te < best.t;
				children[k].tEnter = te;
			}
			std::sort(children, children + 4, [](const Child& a, const Child& b)
			{
				if (a.hit != b.hit) return a.hit;
				return a.tEnter > b.tEnter; // far first -> popped last
			});
			for (int k = 0; k < 4; ++k)
				if (children[k].hit)
					stack.push_back(children[k].node);
		}
	}

	if (best.t >= kHFMaxFloat)
		return false;

	if (hitOut)
		*hitOut = best;
	return true;
}

Vec3 HeightField::Snapshot::SamplePosition(float x, float z) const
{
	if (!data_)
		return Vec3(0.0f);
	const hf::HFData& d = *data_;
	x = std::min(d.W, std::max(0.0f, x));
	z = std::min(d.D, std::max(0.0f, z));

	int cx, cz;
	float u, v;
	CellAt(d, x, z, cx, cz, u, v);
	const CellEq e = MakeCellEq(d, cx, cz);
	return Vec3(x, e.a + e.bu*u + e.bv*v + e.buv*u*v, z);
}

Vec3 HeightField::Snapshot::SampleNormal(float x, float z) const
{
	if (!data_)
		return Vec3(0.0f, 1.0f, 0.0f);
	const hf::HFData& d = *data_;
	x = std::min(d.W, std::max(0.0f, x));
	z = std::min(d.D, std::max(0.0f, z));

	int cx, cz;
	float u, v;
	CellAt(d, x, z, cx, cz, u, v);
	const CellEq e = MakeCellEq(d, cx, cz);
	const float dx = d.W / float(d.N);
	const float dz = d.D / float(d.N);
	const float hx = (e.bu + e.buv*v) / dx;
	const float hz = (e.bv + e.buv*u) / dz;
	return SafeNormalize(Vec3(-hx, 1.0f, -hz));
}

Vec3 HeightField::Snapshot::SampleParam(float u, float v) const
{
	if (!data_)
		return Vec3(0.0f);
	u = std::min(1.0f, std::max(0.0f, u));
	v = std::min(1.0f, std::max(0.0f, v));
	return SamplePosition(u*data_->W, v*data_->D);
}

float HeightField::Snapshot::LocalAreaPdf(float x, float z) const
{
	if (!data_)
		return 0.0f;
	const hf::HFData& d = *data_;
	x = std::min(d.W, std::max(0.0f, x));
	z = std::min(d.D, std::max(0.0f, z));

	int cx, cz;
	float u, v;
	CellAt(d, x, z, cx, cz, u, v);
	const CellEq e = MakeCellEq(d, cx, cz);
	const float dx = d.W / float(d.N);
	const float dz = d.D / float(d.N);
	const float hx = (e.bu + e.buv*v) / dx;
	const float hz = (e.bv + e.buv*u) / dz;
	const float jacobian = sqrtf(1.0f + hx*hx + hz*hz);
	return 1.0f / (d.W*d.D*jacobian);
}

float HeightField::Snapshot::WorldAreaPdf(float x, float z, float scale) const
{
	if (!(scale > 0.0f) || !data_)
		return 0.0f;
	return LocalAreaPdf(x, z) / (scale*scale);
}

// ---------------------------------------------------------------------------
// Primitive integration
// ---------------------------------------------------------------------------

void SetHeightField(Primitive& primitive, const HeightField::Snapshot& snapshot)
{
	primitive.type = eHeightField;
	primitive.heightField.snapshot = snapshot;
}

bool IsHeightField(const Primitive& primitive)
{
	return primitive.type == eHeightField;
}

const HeightFieldGeometry& GetHeightField(const Primitive& primitive)
{
	return primitive.heightField;
}

Bounds HeightFieldBounds(const HeightField::Snapshot& snapshot)
{
	if (!snapshot.Valid())
		return Bounds();
	return snapshot.Data()->root->bounds;
}

Bounds HeightFieldPrimitiveBounds(const Primitive& primitive)
{
	const Bounds local = HeightFieldBounds(primitive.heightField.snapshot);
	const Bounds startBounds = TransformBounds(primitive.startTransform, local);
	const Bounds endBounds = TransformBounds(primitive.endTransform, local);
	return Union(startBounds, endBounds);
}

bool HeightFieldPrimitiveIntersect(const Primitive& primitive, const Ray& ray,
                                   float& outT, Vec3* outNormal, hf::Hit* localHit)
{
	const HeightField::Snapshot& snapshot = primitive.heightField.snapshot;
	if (!snapshot.Valid())
		return false;

	const Transform transform = InterpolateTransform(primitive.startTransform, primitive.endTransform, ray.time);
	if (!(transform.s > 0.0f))
		return false;

	const Vec3 localOrigin = InverseTransformPoint(transform, ray.origin);
	const Vec3 localDir = InverseTransformVector(transform, ray.dir);

	hf::Hit hit;
	if (!snapshot.Intersect(localOrigin, localDir, &hit))
		return false;

	// d_world = R (s d_local), so a local parameter t describes the same
	// point in world space: o_w + d_w t = p + R s (o_l + d_l t). The world
	// ray parameter is therefore the local parameter (non-unit rays included),
	// matching the mesh primitive's convention.
	outT = hit.t;
	if (outNormal)
		*outNormal = SafeNormalize(TransformVector(transform, hit.localNormal));
	if (localHit)
		*localHit = hit;
	return true;
}

void HeightFieldSample(const Primitive& primitive, float u, float v,
                       Vec3& worldPos, Vec3& worldNormal)
{
	const HeightField::Snapshot& snapshot = primitive.heightField.snapshot;
	const Transform transform = InterpolateTransform(primitive.startTransform, primitive.endTransform, 0.5f);

	const Vec3 local = snapshot.SampleParam(u, v);
	const Vec3 n = snapshot.SampleNormal(local.x, local.z);
	worldPos = TransformPoint(transform, local);
	worldNormal = SafeNormalize(TransformVector(transform, n));
}

float HeightFieldWorldAreaPdf(const Primitive& primitive, const Vec3& worldPoint, float time)
{
	const HeightField::Snapshot& snapshot = primitive.heightField.snapshot;
	if (!snapshot.Valid())
		return 0.0f;

	const Transform transform = InterpolateTransform(primitive.startTransform, primitive.endTransform, time);
	if (!(transform.s > 0.0f))
		return 0.0f;

	const Vec3 local = InverseTransformPoint(transform, worldPoint);
	return snapshot.WorldAreaPdf(local.x, local.z, transform.s);
}

float HeightFieldDirectionPdf(const Primitive& primitive, const Vec3& lightWorldPoint,
                              const Vec3& refPoint, float time)
{
	const HeightField::Snapshot& snapshot = primitive.heightField.snapshot;
	if (!snapshot.Valid())
		return 0.0f;

	const Transform transform = InterpolateTransform(primitive.startTransform, primitive.endTransform, time);
	if (!(transform.s > 0.0f))
		return 0.0f;

	const Vec3 local = InverseTransformPoint(transform, lightWorldPoint);
	const float areaPdf = snapshot.WorldAreaPdf(local.x, local.z, transform.s);

	// two-sided light: use the absolute cosine at the light point
	const Vec3 lightNormal = SafeNormalize(TransformVector(
		transform, snapshot.SampleNormal(local.x, local.z)));
	const Vec3 towards = refPoint - lightWorldPoint;
	const float distSq = LengthSq(towards);
	const float dist = sqrtf(distSq);
	if (!(dist > 0.0f))
		return 0.0f;
	const float cosAbs = Abs(Dot(lightNormal, towards/dist));
	if (cosAbs <= 0.0f)
		return REAL_MAX; // grazing: zero-cosine connections carry no energy

	return areaPdf*distSq/cosAbs;
}
