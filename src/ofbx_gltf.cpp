#include "ofbx_gltf.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace ofbx
{

namespace
{

// ---------------------------------------------------------------------------
// Small double precision math library (column major matrices, like DMatrix).
// ---------------------------------------------------------------------------

struct V3
{
	double x = 0, y = 0, z = 0;
};

struct Q4
{
	double x = 0, y = 0, z = 0, w = 1;
};

struct M4
{
	double m[16];

	static M4 identity()
	{
		M4 r{{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1}};
		return r;
	}
};

static V3 v3(double x, double y, double z) { return {x, y, z}; }
static V3 v3(const DVec3& v) { return {v.x, v.y, v.z}; }

static bool finiteV3(const V3& v)
{
	return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

static bool finiteM4(const M4& a)
{
	for (double v : a.m) if (!std::isfinite(v)) return false;
	return true;
}

static M4 toM4(const DMatrix& d)
{
	M4 r;
	std::memcpy(r.m, d.m, sizeof(r.m));
	return r;
}

static M4 mul(const M4& a, const M4& b)
{
	M4 r;
	for (int j = 0; j < 4; ++j)
		for (int i = 0; i < 4; ++i)
		{
			double s = 0;
			for (int k = 0; k < 4; ++k) s += a.m[i + k * 4] * b.m[k + j * 4];
			r.m[i + j * 4] = s;
		}
	return r;
}

static V3 crossV(const V3& a, const V3& b)
{
	return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

static double dotV(const V3& a, const V3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static double lenV(const V3& a) { return std::sqrt(dotV(a, a)); }
static V3 normV(const V3& a)
{
	double l = lenV(a);
	return {a.x / l, a.y / l, a.z / l};
}

static Q4 qAxisAngle(const V3& axis, double angle_rad)
{
	const double h = angle_rad * 0.5;
	const double s = std::sin(h);
	Q4 q{axis.x * s, axis.y * s, axis.z * s, std::cos(h)};
	double l = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
	q.x /= l; q.y /= l; q.z /= l; q.w /= l;
	return q;
}

static Q4 qNormalize(Q4 q)
{
	double l = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
	return {q.x / l, q.y / l, q.z / l, q.w / l};
}

// Quaternion from a pure rotation matrix (upper 3x3, column major).
static Q4 quatFromRotation(const M4& a)
{
	const double m00 = a.m[0], m01 = a.m[4], m02 = a.m[8];
	const double m10 = a.m[1], m11 = a.m[5], m12 = a.m[9];
	const double m20 = a.m[2], m21 = a.m[6], m22 = a.m[10];
	Q4 q;
	const double trace = m00 + m11 + m22;
	if (trace > 0)
	{
		double s = std::sqrt(trace + 1.0) * 2;
		q.w = 0.25 * s;
		q.x = (m21 - m12) / s;
		q.y = (m02 - m20) / s;
		q.z = (m10 - m01) / s;
	}
	else if (m00 > m11 && m00 > m22)
	{
		double s = std::sqrt(1.0 + m00 - m11 - m22) * 2;
		q.w = (m21 - m12) / s;
		q.x = 0.25 * s;
		q.y = (m01 + m10) / s;
		q.z = (m02 + m20) / s;
	}
	else if (m11 > m22)
	{
		double s = std::sqrt(1.0 + m11 - m00 - m22) * 2;
		q.w = (m02 - m20) / s;
		q.x = (m01 + m10) / s;
		q.y = 0.25 * s;
		q.z = (m12 + m21) / s;
	}
	else
	{
		double s = std::sqrt(1.0 + m22 - m00 - m11) * 2;
		q.w = (m10 - m01) / s;
		q.x = (m02 + m20) / s;
		q.y = (m12 + m21) / s;
		q.z = 0.25 * s;
	}
	return qNormalize(q);
}

// Decompose an affine, shear free matrix into TRS. Returns false when the
// 3x3 part is singular, flips in a way we cannot represent or carries shear.
struct TRS
{
	V3 t;
	Q4 r;
	V3 s{1, 1, 1};
};

static bool decomposeTRS(const M4& a, TRS& out)
{
	V3 c0{a.m[0], a.m[1], a.m[2]};
	V3 c1{a.m[4], a.m[5], a.m[6]};
	V3 c2{a.m[8], a.m[9], a.m[10]};
	const double sx = lenV(c0), sy = lenV(c1), sz = lenV(c2);
	const double EPS = 1e-9;
	const double ref = std::max(1.0, std::max(sx, std::max(sy, sz)));
	if (sx < EPS || sy < EPS || sz < EPS) return false;

	V3 n0 = normV(c0), n1 = normV(c1), n2 = normV(c2);
	if (std::abs(dotV(n0, n1)) > EPS * ref) return false;
	if (std::abs(dotV(n0, n2)) > EPS * ref) return false;
	if (std::abs(dotV(n1, n2)) > EPS * ref) return false;

	// Handle a reflection: flip the third axis so the rotation part stays a
	// proper rotation and carry the sign in the scale.
	double det = dotV(n0, crossV(n1, n2));
	if (det < 0)
	{
		n2 = v3(-n2.x, -n2.y, -n2.z);
		// rebuild c2 with the sign moved into scale.
	}
	M4 rot = M4::identity();
	rot.m[0] = n0.x; rot.m[1] = n0.y; rot.m[2] = n0.z;
	rot.m[4] = n1.x; rot.m[5] = n1.y; rot.m[6] = n1.z;
	rot.m[8] = n2.x; rot.m[9] = n2.y; rot.m[10] = n2.z;
	// Orthogonality check of the (possibly corrected) frame.
	if (std::abs(dotV(n0, crossV(n1, n2)) - 1.0) > EPS * ref) return false;

	out.s = v3(sx, sy, det < 0 ? -sz : sz);
	out.r = quatFromRotation(rot);
	out.t = v3(a.m[12], a.m[13], a.m[14]);
	return true;
}

// General 4x4 inverse via Gauss-Jordan. Fails on singular/non-finite input.
static bool invertM4(const M4& in, M4& out)
{
	double a[4][8];
	for (int r = 0; r < 4; ++r)
		for (int c = 0; c < 4; ++c)
		{
			a[r][c] = in.m[c * 4 + r];
			a[r][c + 4] = (r == c) ? 1.0 : 0.0;
		}
	for (int col = 0; col < 4; ++col)
	{
		int pivot = col;
		double best = std::abs(a[col][col]);
		for (int r = col + 1; r < 4; ++r)
			if (std::abs(a[r][col]) > best) { best = std::abs(a[r][col]); pivot = r; }
		if (best < 1e-15) return false;
		if (pivot != col)
			for (int c = 0; c < 8; ++c) std::swap(a[col][c], a[pivot][c]);
		double d = a[col][col];
		for (int c = 0; c < 8; ++c) a[col][c] /= d;
		for (int r = 0; r < 4; ++r)
		{
			if (r == col) continue;
			double f = a[r][col];
			for (int c = 0; c < 8; ++c) a[r][c] -= f * a[col][c];
		}
	}
	for (int r = 0; r < 4; ++r)
		for (int c = 0; c < 4; ++c)
		{
			out.m[c * 4 + r] = a[r][c + 4];
			if (!std::isfinite(out.m[c * 4 + r])) return false;
		}
	return true;
}

static const double DEG2RAD = 3.1415926535897932384626433832795028 / 180.0;

// ---------------------------------------------------------------------------
// Error reporting
// ---------------------------------------------------------------------------

struct ExportError
{
	GltfError code;
	std::string message;
};

[[noreturn]] static void fail(GltfError code, const char* fmt, ...)
{
	ExportError e{code, {}};
	char buffer[512];
	va_list args;
	va_start(args, fmt);
	std::vsnprintf(buffer, sizeof(buffer), fmt, args);
	va_end(args);
	e.message = buffer;
	throw e;
}

// ---------------------------------------------------------------------------
// Output GLB document model
// ---------------------------------------------------------------------------

enum AnimPath { PATH_TRANSLATION = 0, PATH_ROTATION = 1, PATH_SCALE = 2 };

struct JsonNode
{
	std::string name;
	int parent = -1;
	std::vector<u32> children;

	bool has_t = false, has_r = false, has_s = false;
	float t[3] = {0, 0, 0};
	float r[4] = {0, 0, 0, 1};
	float s[3] = {1, 1, 1};

	int mesh = -1;
	int skin = -1;
};

struct Accessor
{
	int buffer_view = -1;
	int component_type = 0; // glTF component type
	std::string type;       // SCALAR/VEC3/VEC4/MAT4
	u32 count = 0;
	bool has_min_max = false;
	std::vector<float> mn, mx;
};

struct BufferView
{
	u32 offset = 0;
	u32 length = 0;
	int target = 0; // 0 = unset
};

struct Primitive
{
	int positions = -1;
	int indices = -1;
	int joints = -1;
	int weights = -1;
};

struct JsonMesh
{
	std::string name;
	Primitive primitive;
};

struct JsonSkin
{
	std::vector<u32> joints;
	int ibm = -1;
};

struct Sampler
{
	int input = -1;
	int output = -1;
};

struct Channel
{
	int sampler = -1;
	u32 node = 0;
	AnimPath path;
};

// Per axis animated track, before it is turned into accessors.
struct ScalarTrack
{
	std::vector<float> times;   // output seconds (zero based)
	std::vector<float> values;  // raw FBX units: component value / degrees
};

struct PendingChannel
{
	u32 node;
	AnimPath path;
	int axis; // 0..2
	int track; // index into Builder::owned_tracks, -1 when static
	float static_value;
};

class Builder
{
public:
	Builder(const IScene& sc, const AnimationLayer& al, double b, double e)
		: scene(sc), layer(al), begin(b), end(e)
	{
	}

	GltfError run(GltfResult& out)
	{
		try
		{
			validateInterval();
			buildNodeChains();
			buildAnimationTracks();
			buildMeshesAndSkins();
			emitBinary();
			emitGlb();
		}
		catch (const ExportError& e)
		{
			out.code = e.code;
			out.error_message.assign(e.message.begin(), e.message.end());
			out.error_message.push_back('\0');
			out.message = out.error_message.data();
			return e.code;
		}
		// Success: replace previous output only now.
		out.code = GltfError::OK;
		out.error_message.clear();
		out.error_message.push_back('\0');
		out.message = "";
		out.glb.swap(glb_bytes);
		out.node_map.swap(node_map);
		return GltfError::OK;
	}

private:
	const IScene& scene;
	const AnimationLayer& layer;
	double begin, end;

	std::vector<JsonNode> nodes;
	std::vector<JsonMesh> meshes;
	std::vector<JsonSkin> skins;
	std::vector<Accessor> accessors;
	std::vector<BufferView> views;
	std::vector<Sampler> samplers;
	std::vector<Channel> channels;
	std::vector<PendingChannel> pending_channels;
	std::vector<unsigned char> bin;
	std::vector<unsigned char> glb_bytes;
	std::map<u64, u32> node_map;

	// Source object id -> first node of its chain (and cached terminal).
	std::unordered_map<u64, u32> chain_head;
	std::unordered_map<u64, u32> chain_terminal;

	// -------------------------------------------------------------------
	void validateInterval()
	{
		if (!std::isfinite(begin) || !std::isfinite(end) || begin < 0.0 || !(begin < end))
			fail(GltfError::INVALID_ARGUMENT, "invalid export interval [%g, %g)", begin, end);
		if (!std::isfinite((float)begin) || !std::isfinite((float)end))
			fail(GltfError::NON_FINITE_TIME, "interval bounds are not finite float seconds");
		if ((float)begin == (float)end)
			fail(GltfError::NON_FINITE_TIME, "interval bounds collapse to one float second");
	}

	static float toFiniteFloat(double v, GltfError code, const char* what)
	{
		if (!std::isfinite(v)) fail(code, "%s is not finite", what);
		float f = (float)v;
		if (!std::isfinite(f)) fail(GltfError::NUMERIC_OVERFLOW, "%s overflows float", what);
		return f;
	}

	u32 addNode(const std::string& name)
	{
		JsonNode n;
		n.name = name;
		nodes.push_back(n);
		if (nodes.size() > 1u << 28) fail(GltfError::SIZE_OVERFLOW, "too many output nodes");
		return (u32)(nodes.size() - 1);
	}

	void linkChild(u32 parent, u32 child)
	{
		nodes[parent].children.push_back(child);
		nodes[child].parent = (int)parent;
	}

	void setTranslation(u32 n, const V3& t)
	{
		if (!finiteV3(t)) fail(GltfError::NUMERIC_OVERFLOW, "non finite translation");
		nodes[n].has_t = true;
		nodes[n].t[0] = toFiniteFloat(t.x, GltfError::NUMERIC_OVERFLOW, "translation.x");
		nodes[n].t[1] = toFiniteFloat(t.y, GltfError::NUMERIC_OVERFLOW, "translation.y");
		nodes[n].t[2] = toFiniteFloat(t.z, GltfError::NUMERIC_OVERFLOW, "translation.z");
	}

	void setScale(u32 n, const V3& s)
	{
		// Explicit factor scale nodes may legitimately carry a zero (degenerate
		// but representable in glTF); only non finite values are fatal here.
		// General-matrix singularity is rejected separately by decomposeTRS().
		if (!finiteV3(s)) fail(GltfError::NUMERIC_OVERFLOW, "non finite scale");
		nodes[n].has_s = true;
		nodes[n].s[0] = toFiniteFloat(s.x, GltfError::NUMERIC_OVERFLOW, "scale.x");
		nodes[n].s[1] = toFiniteFloat(s.y, GltfError::NUMERIC_OVERFLOW, "scale.y");
		nodes[n].s[2] = toFiniteFloat(s.z, GltfError::NUMERIC_OVERFLOW, "scale.z");
	}

	void setRotation(u32 n, const Q4& q)
	{
		if (!std::isfinite(q.x) || !std::isfinite(q.y) || !std::isfinite(q.z) || !std::isfinite(q.w))
			fail(GltfError::NUMERIC_OVERFLOW, "non finite rotation");
		nodes[n].has_r = true;
		nodes[n].r[0] = toFiniteFloat(q.x, GltfError::NUMERIC_OVERFLOW, "rotation.x");
		nodes[n].r[1] = toFiniteFloat(q.y, GltfError::NUMERIC_OVERFLOW, "rotation.y");
		nodes[n].r[2] = toFiniteFloat(q.z, GltfError::NUMERIC_OVERFLOW, "rotation.z");
		nodes[n].r[3] = toFiniteFloat(q.w, GltfError::NUMERIC_OVERFLOW, "rotation.w");
	}

	// ---------------------------------------------------------------
	// Node chains: every isNode() object becomes a TRS chain whose
	// terminal node carries the object's complete local transform.
	// ---------------------------------------------------------------
	struct ChainIds
	{
		u32 head;
		u32 terminal;
		u32 axis_t[3];
		u32 axis_r[3]; // indexed by euler axis 0=X,1=Y,2=Z
		u32 axis_s[3];
	};

	std::unordered_map<u64, ChainIds> chains;

	static std::string objName(const Object& o)
	{
		std::string n = o.name;
		if (n.empty()) { n = "id"; n += std::to_string(o.id); }
		return n;
	}

	u32 appendFactorNode(u32 parent, const std::string& label)
	{
		u32 n = addNode(label);
		linkChild(parent, n);
		return n;
	}

	ChainIds buildChain(const Object& obj)
	{
		const DVec3 st = obj.getLocalTranslation();
		const DVec3 sr = obj.getLocalRotation();
		const DVec3 ss = obj.getLocalScaling();
		const LocalTransformFactors f = getLocalTransformFactors(obj, st, sr, ss);
		if (f.rotation_order == RotationOrder::SPHERIC_XYZ)
			fail(GltfError::UNSUPPORTED_INPUT, "object %llu uses unsupported SPHERIC_XYZ rotation order",
				(unsigned long long)obj.id);
		for (const DVec3* v : {&f.translation, &f.rotation, &f.scaling, &f.pre_rotation, &f.post_rotation,
			&f.rotation_offset, &f.rotation_pivot, &f.scaling_offset, &f.scaling_pivot})
			if (!std::isfinite(v->x) || !std::isfinite(v->y) || !std::isfinite(v->z))
				fail(GltfError::NUMERIC_OVERFLOW, "non finite transform factor on object %llu",
					(unsigned long long)obj.id);

		const std::string base = objName(obj) + "#";
		ChainIds c;
		u32 cur = addNode(base + "T0");
		c.head = cur;

		// Translation split into three independent axis nodes.
		c.axis_t[0] = cur;
		setTranslation(cur, v3(f.translation.x, 0, 0));
		cur = appendFactorNode(cur, base + "T1");
		c.axis_t[1] = cur;
		setTranslation(cur, v3(0, f.translation.y, 0));
		cur = appendFactorNode(cur, base + "T2");
		c.axis_t[2] = cur;
		setTranslation(cur, v3(0, 0, f.translation.z));

		cur = appendFactorNode(cur, base + "RotationOffset");
		setTranslation(cur, v3(f.rotation_offset));
		cur = appendFactorNode(cur, base + "RotationPivot");
		setTranslation(cur, v3(f.rotation_pivot));
		cur = appendFactorNode(cur, base + "PreRotation");
		setRotation(cur, quatFromRotation(toM4(eulerRotationMatrix(f.pre_rotation, RotationOrder::EULER_XYZ))));

		// Three independent euler axis nodes, ordered so parent->child
		// multiplication reproduces the FBX rotation order exactly.
		const RotationOrder ro = f.rotation_order;
		int parent_first[6][3] = {
			{2, 1, 0}, // EULER_XYZ: Rz * Ry * Rx
			{1, 2, 0}, // EULER_XZY: Ry * Rz * Rx
			{0, 2, 1}, // EULER_YZX: Rx * Rz * Ry
			{2, 0, 1}, // EULER_YXZ: Rz * Rx * Ry
			{1, 0, 2}, // EULER_ZXY: Ry * Rx * Rz
			{0, 1, 2}, // EULER_ZYX: Rx * Ry * Rz
		};
		const int order_idx = (int)ro;
		const V3 axes[3] = {v3(1, 0, 0), v3(0, 1, 0), v3(0, 0, 1)};
		const char* axis_name[3] = {"X", "Y", "Z"};
		const double static_rot[3] = {f.rotation.x, f.rotation.y, f.rotation.z};
		for (int k = 0; k < 3; ++k)
		{
			const int axis = parent_first[order_idx][k];
			cur = appendFactorNode(cur, base + std::string("Rot") + axis_name[axis]);
			c.axis_r[axis] = cur;
			setRotation(cur, qAxisAngle(axes[axis], static_rot[axis] * DEG2RAD));
		}

		cur = appendFactorNode(cur, base + "PostRotation");
		setRotation(cur, quatFromRotation(
			toM4(eulerRotationMatrix(DVec3{-f.post_rotation.x, -f.post_rotation.y, -f.post_rotation.z},
				RotationOrder::EULER_ZYX))));
		cur = appendFactorNode(cur, base + "RotationPivotInv");
		setTranslation(cur, v3(-f.rotation_pivot.x, -f.rotation_pivot.y, -f.rotation_pivot.z));
		cur = appendFactorNode(cur, base + "ScalingOffset");
		setTranslation(cur, v3(f.scaling_offset));
		cur = appendFactorNode(cur, base + "ScalingPivot");
		setTranslation(cur, v3(f.scaling_pivot));

		c.axis_s[0] = appendFactorNode(cur, base + "S0");
		cur = c.axis_s[0];
		setScale(cur, v3(f.scaling.x, 1, 1));
		c.axis_s[1] = appendFactorNode(cur, base + "S1");
		cur = c.axis_s[1];
		setScale(cur, v3(1, f.scaling.y, 1));
		c.axis_s[2] = appendFactorNode(cur, base + "S2");
		cur = c.axis_s[2];
		setScale(cur, v3(1, 1, f.scaling.z));
		cur = appendFactorNode(cur, base + "ScalingPivotInv");
		setTranslation(cur, v3(-f.scaling_pivot.x, -f.scaling_pivot.y, -f.scaling_pivot.z));

		c.terminal = cur;
		return c;
	}

	void buildNodeChains()
	{
		// Deterministic order: scene root first, then the source object table.
		std::vector<const Object*> node_objects;
		if (const Object* root = scene.getRoot()) node_objects.push_back(root);
		for (int i = 0; i < scene.getAllObjectCount(); ++i)
		{
			const Object* o = scene.getAllObjects()[i];
			if (o->isNode()) node_objects.push_back(o);
		}
		for (const Object* o : node_objects)
		{
			ChainIds c = buildChain(*o);
			chains.emplace(o->id, c);
			chain_head.emplace(o->id, c.head);
			chain_terminal.emplace(o->id, c.terminal);
			node_map.emplace(o->id, c.terminal);
		}
		// Reparent chains according to the source hierarchy.
		for (const Object* o : node_objects)
		{
			const Object* p = o->getParent();
			u32 head = chain_head.at(o->id);
			if (!p)
			{
				// scene root, stays parentless
				(void)head;
				continue;
			}
			auto it = chain_terminal.find(p->id);
			if (it == chain_terminal.end())
				fail(GltfError::INVALID_ARGUMENT, "parent node %llu missing", (unsigned long long)p->id);
			linkChild(it->second, head);
		}
	}

	// ---------------------------------------------------------------
	// Animation
	// ---------------------------------------------------------------

	// Replicates AnimationCurveNodeImpl::getNodeLocalTransform for one axis.
	static float evalAxis(const AnimationCurve& curve, i64 fbx_time)
	{
		const int count = curve.getKeyCount();
		const i64* times = curve.getKeyTime();
		const float* values = curve.getKeyValue();
		if (fbx_time < times[0]) fbx_time = times[0];
		if (fbx_time > times[count - 1]) fbx_time = times[count - 1];
		for (int i = 1; i < count; ++i)
		{
			if (times[i] >= fbx_time)
			{
				float t = float(double(fbx_time - times[i - 1]) / double(times[i] - times[i - 1]));
				return values[i - 1] * (1 - t) + values[i] * t;
			}
		}
		return values[0];
	}

	struct Key
	{
		i64 tick;
		float value;
	};

	// Builds the full output track (own keys + interval endpoints +
	// multi-turn midpoint splits for rotation axes).
	ScalarTrack buildTrack(const AnimationCurve& curve, bool is_rotation, const char* ctx)
	{
		const int n = curve.getKeyCount();
		if (n < 1) fail(GltfError::UNSUPPORTED_INPUT, "%s: empty curve", ctx);
		const i64* ticks = curve.getKeyTime();
		const float* values = curve.getKeyValue();
		for (int i = 0; i < n; ++i)
		{
			if (!std::isfinite(values[i])) fail(GltfError::NUMERIC_OVERFLOW, "%s: non finite key value", ctx);
			if (i > 0 && ticks[i] <= ticks[i - 1])
				fail(GltfError::UNSORTED_KEYS, "%s: key times not strictly increasing", ctx);
			// Every input key time must be a finite float second, even when the
			// key itself lies outside the exported interval.
			const double key_seconds = fbxTimeToSeconds(ticks[i]);
			if (!std::isfinite(key_seconds) || !std::isfinite((float)key_seconds))
				fail(GltfError::NON_FINITE_TIME, "%s: key time not representable as finite float seconds", ctx);
		}
		const i64 tick_begin = secondsToFbxTime(begin);
		const i64 tick_end = secondsToFbxTime(end);

		std::vector<Key> keys;
		// Endpoint at begin.
		keys.push_back({tick_begin, evalAxis(curve, tick_begin)});
		for (int i = 0; i < n; ++i)
		{
			if (ticks[i] > tick_begin && ticks[i] < tick_end) keys.push_back({ticks[i], values[i]});
		}
		keys.push_back({tick_end, evalAxis(curve, tick_end)});

		// Every emitted time must be a distinguishable finite float second.
		// The two interval endpoints are pinned to exactly 0 and (end-begin)
		// so output time provably starts at zero regardless of tick rounding.
		const float out_span = toFiniteFloat(end - begin, GltfError::NON_FINITE_TIME, "interval span");
		auto tick_to_out = [&](i64 tick) -> float {
			if (tick == tick_begin) return 0.0f;
			if (tick == tick_end) return out_span;
			double sec = fbxTimeToSeconds(tick) - begin;
			if (!std::isfinite(sec) || !std::isfinite((float)sec))
				fail(GltfError::NON_FINITE_TIME, "%s: key time not representable as float seconds", ctx);
			return (float)sec;
		};
		std::vector<float> out_times;
		for (const Key& k : keys) out_times.push_back(tick_to_out(k.tick));
		for (size_t i = 1; i < out_times.size(); ++i)
			if (!(out_times[i] > out_times[i - 1]))
				fail(GltfError::NON_FINITE_TIME,
					"%s: adjacent keys collapse to the same float second and cannot be kept distinct", ctx);

		ScalarTrack track;
		if (!is_rotation)
		{
			track.times = out_times;
			track.values.reserve(keys.size());
			for (const Key& k : keys) track.values.push_back(k.value);
			return track;
		}

		// Rotation: split any segment covering more than 180 degrees into
		// finite midpoint keys so glTF shortest-arc SLERP keeps the original
		// turning direction (handles 450 deg, negative turns, full loops).
		struct Seg { i64 t0, t1; float v0, v1; };
		std::vector<Seg> segs;
		for (size_t i = 1; i < keys.size(); ++i)
			segs.push_back({keys[i - 1].tick, keys[i].tick, keys[i - 1].value, keys[i].value});

		std::vector<Key> split;
		split.push_back(keys.front());
		size_t total_budget = 1 << 24;
		for (Seg sg : segs)
		{
			std::vector<Seg> stack;
			stack.push_back(sg);
			while (!stack.empty())
			{
				Seg s = stack.back();
				stack.pop_back();
				const float dv = s.v1 - s.v0;
				// Split anything at or beyond 180 degrees: an exact 180 degree
				// pair has no defined shortest arc, and anything larger must
				// keep its original turning direction.
				if (std::abs(dv) < 180.0f - 1e-6f)
				{
					if (split.size() + 1 > total_budget)
						fail(GltfError::SIZE_OVERFLOW, "%s: too many split rotation keys", ctx);
					split.push_back({s.t1, s.v1});
					continue;
				}
				if (s.t1 - s.t0 < 2)
					fail(GltfError::NON_FINITE_TIME, "%s: cannot split multi-turn segment at tick resolution", ctx);
				const i64 mid_t = s.t0 + (s.t1 - s.t0) / 2;
				const float mid_v = s.v0 + dv * 0.5f;
				// Push right half first so left half is processed first.
				stack.push_back({mid_t, s.t1, mid_v, s.v1});
				stack.push_back({s.t0, mid_t, s.v0, mid_v});
			}
		}
		track.times.reserve(split.size());
		track.values.reserve(split.size());
		float prev_time = -std::numeric_limits<float>::infinity();
		for (const Key& k : split)
		{
			float t = tick_to_out(k.tick);
			if (!(t > prev_time))
				fail(GltfError::NON_FINITE_TIME, "%s: split key collides in float time", ctx);
			prev_time = t;
			track.times.push_back(t);
			track.values.push_back(k.value);
		}
		return track;
	}

	void buildAnimationTracks()
	{
		static const char* props[3] = {"Lcl Translation", "Lcl Rotation", "Lcl Scaling"};
		// id -> source object, gathered once.
		std::unordered_map<u64, const Object*> by_id;
		if (const Object* root = scene.getRoot()) by_id.emplace(root->id, root);
		for (int i = 0; i < scene.getAllObjectCount(); ++i)
			by_id.emplace(scene.getAllObjects()[i]->id, scene.getAllObjects()[i]);

		for (auto& kv : chains)
		{
			const u64 id = kv.first;
			ChainIds& c = kv.second;
			const Object* obj = by_id.at(id);

			for (int p = 0; p < 3; ++p)
			{
				const AnimationCurveNode* cn = layer.getCurveNode(*obj, props[p]);
				const DVec3 static_vals[3] = {obj->getLocalTranslation(), obj->getLocalRotation(), obj->getLocalScaling()};
				// When a curve node exists, its d|X/d|Y/d|Z defaults replace the
				// object's static components on every uncurved axis.
				DVec3 defaults = static_vals[p];
				if (cn) defaults = cn->getNodeLocalTransform(begin);
				for (int axis = 0; axis < 3; ++axis)
				{
					const AnimationCurve* curve = cn ? cn->getCurve(axis) : nullptr;
					const float default_component =
						(float)(axis == 0 ? defaults.x : axis == 1 ? defaults.y : defaults.z);
					if (!std::isfinite(default_component))
						fail(GltfError::NUMERIC_OVERFLOW, "object %llu default component non finite",
							(unsigned long long)id);

					u32 target_node;
					AnimPath path;
					if (p == 0) { target_node = c.axis_t[axis]; path = PATH_TRANSLATION; }
					else if (p == 1) { target_node = c.axis_r[axis]; path = PATH_ROTATION; }
					else { target_node = c.axis_s[axis]; path = PATH_SCALE; }

					// Bake the curve node default into the factor node; an actual
					// curve overrides it through animation channels.
					if (p == 0)
						setTranslation(target_node, axis == 0 ? v3(default_component, 0, 0)
							: axis == 1 ? v3(0, default_component, 0) : v3(0, 0, default_component));
					else if (p == 1)
					{
						const V3 axes[3] = {v3(1, 0, 0), v3(0, 1, 0), v3(0, 0, 1)};
						setRotation(target_node, qAxisAngle(axes[axis], (double)default_component * DEG2RAD));
					}
					else
						setScale(target_node, axis == 0 ? v3(default_component, 1, 1)
							: axis == 1 ? v3(1, default_component, 1) : v3(1, 1, default_component));

					if (!curve) continue;
					char ctx[160];
					std::snprintf(ctx, sizeof(ctx), "object %llu %s axis %d",
						(unsigned long long)id, props[p], axis);
					owned_tracks.push_back(buildTrack(*curve, p == 1, ctx));
					pending_channels.push_back(
						{target_node, path, axis, (int)owned_tracks.size() - 1, default_component});
				}
			}
		}
	}

	std::vector<ScalarTrack> owned_tracks;

	// ---------------------------------------------------------------
	// Meshes and skins
	// ---------------------------------------------------------------
	struct Influence
	{
		const Object* bone;
		const Cluster* cluster;
		double weight;
	};

	void buildMeshesAndSkins()
	{
		for (int mi = 0; mi < scene.getMeshCount(); ++mi)
		{
			const Mesh* mesh = scene.getMesh(mi);
			if (mesh->getBlendShape() || (mesh->getGeometry() && mesh->getGeometry()->getBlendShape()))
				fail(GltfError::UNSUPPORTED_INPUT, "mesh %llu uses blend shapes, which are not supported",
					(unsigned long long)mesh->id);

			const GeometryData& gd = mesh->getGeometryData();
			if (!gd.hasVertices())
				fail(GltfError::UNSUPPORTED_INPUT, "mesh %llu has no vertices", (unsigned long long)mesh->id);
			const Vec3Attributes pos = gd.getPositions();
			if (!pos.values)
				fail(GltfError::UNSUPPORTED_INPUT, "mesh %llu has no positions", (unsigned long long)mesh->id);
			const int corner_count = pos.count;
			const int cp_count = pos.values_count;

			// Triangles only.
			std::vector<int> tri_corners;
			for (int pi = 0; pi < gd.getPartitionCount(); ++pi)
			{
				const GeometryPartition part = gd.getPartition(pi);
				for (int poly = 0; poly < part.polygon_count; ++poly)
				{
					const auto& p = part.polygons[poly];
					if (p.vertex_count != 3)
						fail(GltfError::UNSUPPORTED_INPUT,
							"mesh %llu polygon %d has %d vertices, only triangles are supported",
							(unsigned long long)mesh->id, poly, p.vertex_count);
					tri_corners.push_back(p.from_vertex);
					tri_corners.push_back(p.from_vertex + 1);
					tri_corners.push_back(p.from_vertex + 2);
				}
			}
			if (tri_corners.empty())
				fail(GltfError::UNSUPPORTED_INPUT, "mesh %llu has no triangles", (unsigned long long)mesh->id);

			// Control point for every polygon corner.
			std::vector<int> corner_cp(corner_count);
			for (int k = 0; k < corner_count; ++k)
				corner_cp[k] = pos.indices ? pos.indices[k] : k;
			for (int idx : tri_corners)
				if (idx < 0 || idx >= corner_count)
					fail(GltfError::OUT_OF_RANGE, "mesh %llu corner index out of range",
						(unsigned long long)mesh->id);

			JsonMesh jm;
			jm.name = objName(*mesh);

			// Skin processing.
			const Skin* skin = mesh->getSkin();
			std::vector<std::vector<Influence>> per_cp;
			std::vector<const Object*> joint_bones;
			std::unordered_map<u64, int> joint_index;
			std::vector<M4> ibm;
			int synthetic_joint = -1;   // output node index
			int synthetic_joint_index = -1; // index inside this skin's joints array
			const M4 geo = toM4(mesh->getGeometricMatrix());
			if (!finiteM4(geo))
				fail(GltfError::NUMERIC_OVERFLOW, "mesh %llu geometric matrix non finite",
					(unsigned long long)mesh->id);

			if (skin)
			{
				per_cp.resize(cp_count);
				// Duplicate cluster detection per bone.
				std::unordered_map<u64, const Cluster*> seen;
				for (int ci = 0; ci < skin->getClusterCount(); ++ci)
				{
					const Cluster* cl = skin->getCluster(ci);
					const Object* link = cl->getLink();
					if (!link)
						fail(GltfError::INVALID_SKIN, "skin cluster %llu has no link",
							(unsigned long long)cl->id);
					if (!link->isNode())
						fail(GltfError::INVALID_SKIN, "cluster %llu link is not a scene node",
							(unsigned long long)cl->id);
					const M4 tl = toM4(cl->getTransformLinkMatrix());
					const M4 tm = toM4(cl->getTransformMatrix());
					if (!finiteM4(tl) || !finiteM4(tm))
						fail(GltfError::INVALID_SKIN, "cluster %llu bind matrices contain non finite values",
							(unsigned long long)cl->id);
					M4 inv_tl;
					if (!invertM4(tl, inv_tl))
						fail(GltfError::INVALID_SKIN, "cluster %llu has singular TransformLink binding",
							(unsigned long long)cl->id);

					auto dup = seen.find(link->id);
					const bool identical_duplicate = dup != seen.end();
					if (identical_duplicate)
					{
						const Cluster* prev = dup->second;
						const M4 prev_tl = toM4(prev->getTransformLinkMatrix());
						const M4 prev_tm = toM4(prev->getTransformMatrix());
						bool same = true;
						for (int k = 0; k < 16; ++k)
							if (prev_tl.m[k] != tl.m[k] || prev_tm.m[k] != tm.m[k]) same = false;
						if (!same)
							fail(GltfError::INVALID_SKIN,
								"bone %llu has duplicate clusters with different bind matrices",
								(unsigned long long)link->id);
						// Identical binding: merge. Its weights still feed the
						// same joint below; only joint/IBM registration is skipped.
					}
					else
						seen.emplace(link->id, cl);

					if (cl->getIndicesCount() != cl->getWeightsCount())
						fail(GltfError::INVALID_SKIN, "cluster %llu index/weight count mismatch",
							(unsigned long long)cl->id);
					for (int k = 0; k < cl->getIndicesCount(); ++k)
					{
						const int vi = cl->getIndices()[k];
						const double w = cl->getWeights()[k];
						if (!std::isfinite(w))
							fail(GltfError::INVALID_SKIN, "cluster %llu non finite weight",
								(unsigned long long)cl->id);
						if (w < 0)
							fail(GltfError::INVALID_SKIN, "cluster %llu negative weight at vertex %d",
								(unsigned long long)cl->id, vi);
						if (vi < 0 || vi >= cp_count)
							fail(GltfError::OUT_OF_RANGE, "cluster %llu vertex index %d out of range [0,%d)",
								(unsigned long long)cl->id, vi, cp_count);
						if (w == 0) continue; // zero weight is not a positive influence
						// Merge same-bone weights at a vertex (duplicate clusters
						// with identical bindings collapse into one influence).
						bool merged = false;
						for (Influence& existing : per_cp[vi])
							if (existing.bone == link) { existing.weight += w; merged = true; break; }
						if (!merged) per_cp[vi].push_back({link, cl, w});
					}

					if (!identical_duplicate)
					{
						// Register joint and its IBM = inverse(TransformLink) * Transform * geometric.
						joint_index.emplace(link->id, (int)joint_bones.size());
						joint_bones.push_back(link);
						ibm.push_back(mul(mul(inv_tl, tm), geo));
					}
				}

				// Influence limits, normalization and the unweighted synthetic joint.
				bool need_synthetic = false;
				for (int v = 0; v < cp_count; ++v)
				{
					auto& infs = per_cp[v];
					if (infs.empty()) { need_synthetic = true; continue; }
					std::unordered_set<u64> distinct;
					double sum = 0;
					for (auto& inf : infs)
					{
						distinct.insert(inf.bone->id);
						sum += inf.weight;
					}
					if (distinct.size() > 4)
						fail(GltfError::INVALID_SKIN,
							"mesh %llu vertex %d has %zu positive influences, at most 4 are supported",
							(unsigned long long)mesh->id, v, distinct.size());
					if (sum <= 0)
					{
						infs.clear();
						need_synthetic = true;
						continue;
					}
					for (auto& influence : infs) influence.weight /= sum; // positive weights normalized to 1
				}
				if (need_synthetic)
				{
					// Synthetic joint hangs directly under the mesh chain terminal
					// and carries identity, so its world always equals the mesh
					// terminal's complete (animated) world pose.
					char nm[128];
					std::snprintf(nm, sizeof(nm), "%s#SyntheticJoint", objName(*mesh).c_str());
					synthetic_joint = (int)addNode(nm);
					linkChild(chain_terminal.at(mesh->id), (u32)synthetic_joint);
					joint_bones.push_back(nullptr);
					synthetic_joint_index = (int)joint_bones.size() - 1;
					ibm.push_back(geo); // world_syn * G * cp == meshWorld * G * cp
				}
			}

			// --- Emit positions (one expanded vertex per triangle corner) ---
			std::vector<V3> out_pos;
			std::vector<std::array<u32, 4>> out_joints;
			std::vector<std::array<float, 4>> out_weights;
			out_pos.reserve(tri_corners.size());
			out_joints.reserve(tri_corners.size());
			out_weights.reserve(tri_corners.size());

			auto get_joint_idx = [&](const Object* bone) -> int {
				auto it = joint_index.find(bone->id);
				if (it == joint_index.end())
					fail(GltfError::INVALID_SKIN, "internal: joint not registered for bone %llu",
						(unsigned long long)bone->id);
				return it->second;
			};

			for (int corner : tri_corners)
			{
				const Vec3 p = pos.get(corner);
				if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z))
					fail(GltfError::NUMERIC_OVERFLOW, "mesh %llu non finite position",
						(unsigned long long)mesh->id);
				out_pos.push_back(v3(p.x, p.y, p.z));

				if (skin)
				{
					std::array<u32, 4> j{0, 0, 0, 0};
					std::array<float, 4> w{0, 0, 0, 0};
					const int cp = corner_cp[corner];
					const auto& infs = per_cp[cp];
					if (infs.empty())
					{
						j[0] = (u32)synthetic_joint_index;
						w[0] = 1.0f;
					}
					else
					{
						for (size_t k = 0; k < infs.size() && k < 4; ++k)
						{
							j[k] = (u32)get_joint_idx(infs[k].bone);
							w[k] = toFiniteFloat(infs[k].weight, GltfError::NUMERIC_OVERFLOW, "skin weight");
						}
					}
					out_joints.push_back(j);
					out_weights.push_back(w);
				}
			}

			int pos_accessor = addFloatAccessor(out_pos, "POSITION", true);
			jm.primitive.positions = pos_accessor;
			if (skin)
			{
				const bool wide = joint_bones.size() > 65535;
				int j_accessor = wide
					? addUIntAccessor(out_joints, "JOINTS_0", 4)
					: addUShortAccessor(out_joints, "JOINTS_0");
				int w_accessor = addFloatAccessor(out_weights, "WEIGHTS_0", false);
				jm.primitive.joints = j_accessor;
				jm.primitive.weights = w_accessor;

				JsonSkin js;
				for (const Object* bone : joint_bones)
				{
					if (bone == nullptr) js.joints.push_back((u32)synthetic_joint);
					else js.joints.push_back(chain_terminal.at(bone->id));
				}
				js.ibm = addMat4Accessor(ibm);
				skins.push_back(js);
			}
			meshes.push_back(jm);
			const u32 mesh_idx = (u32)(meshes.size() - 1);

			// Geometric transform lives on its own child node affecting only
			// this mesh; the skin (if any) is instantiated from that node.
			const u32 terminal = chain_terminal.at(mesh->id);
			const u32 geo_node = addNode(objName(*mesh) + "#Geometric");
			linkChild(terminal, geo_node);
			TRS gtrs;
			if (!decomposeTRS(geo, gtrs))
				fail(GltfError::SINGULAR_TRANSFORM,
					"mesh %llu geometric transform cannot be represented as glTF TRS",
					(unsigned long long)mesh->id);
			if (gtrs.t.x != 0 || gtrs.t.y != 0 || gtrs.t.z != 0) setTranslation(geo_node, gtrs.t);
			setRotation(geo_node, gtrs.r);
			if (gtrs.s.x != 1 || gtrs.s.y != 1 || gtrs.s.z != 1) setScale(geo_node, gtrs.s);
			nodes[geo_node].mesh = (int)mesh_idx;
			if (skin) nodes[geo_node].skin = (int)(skins.size() - 1);
		}
	}

	// ---------------------------------------------------------------
	// Binary buffer / accessor construction
	// ---------------------------------------------------------------
	u32 alignBuffer(u32 alignment)
	{
		u32 off = (u32)bin.size();
		u32 rem = off % alignment;
		if (rem)
		{
			for (u32 i = 0; i < alignment - rem; ++i) bin.push_back(0);
			off = (u32)bin.size();
		}
		return off;
	}

	template <typename T>
	int addView(const T* data, u32 count, u32 alignment, int target)
	{
		if (count > std::numeric_limits<u32>::max() / sizeof(T))
			fail(GltfError::SIZE_OVERFLOW, "buffer view size overflow");
		const u32 bytes = count * (u32)sizeof(T);
		const u32 off = alignBuffer(alignment);
		if ((u64)off + bytes > std::numeric_limits<u32>::max() - 8)
			fail(GltfError::SIZE_OVERFLOW, "binary buffer exceeds uint32 range");
		const auto* p = reinterpret_cast<const unsigned char*>(data);
		bin.insert(bin.end(), p, p + bytes);
		BufferView v{off, bytes, target};
		views.push_back(v);
		return (int)(views.size() - 1);
	}

	int addFloatAccessor(const std::vector<V3>& data, const char* /*tag*/, bool minmax)
	{
		// V3 is a double precision math type; glTF POSITION data is float32.
		std::vector<std::array<float, 3>> f;
		f.reserve(data.size());
		for (const V3& p : data)
			f.push_back({
				toFiniteFloat(p.x, GltfError::NUMERIC_OVERFLOW, "position.x"),
				toFiniteFloat(p.y, GltfError::NUMERIC_OVERFLOW, "position.y"),
				toFiniteFloat(p.z, GltfError::NUMERIC_OVERFLOW, "position.z")});
		Accessor a;
		a.buffer_view = addView(f.data(), (u32)f.size(), 4, 34962);
		a.component_type = 5126;
		a.type = "VEC3";
		a.count = (u32)f.size();
		if (minmax)
		{
			std::array<float, 3> lo = f[0], hi = f[0];
			for (const auto& p : f)
				for (int k = 0; k < 3; ++k)
				{
					lo[k] = std::min(lo[k], p[k]);
					hi[k] = std::max(hi[k], p[k]);
				}
			a.has_min_max = true;
			a.mn = {lo[0], lo[1], lo[2]};
			a.mx = {hi[0], hi[1], hi[2]};
		}
		accessors.push_back(a);
		return (int)(accessors.size() - 1);
	}

	int addFloatAccessor(const std::vector<std::array<float, 4>>& data, const char*, bool)
	{
		Accessor a;
		a.buffer_view = addView(data.data(), (u32)data.size(), 4, 34962);
		a.component_type = 5126;
		a.type = "VEC4";
		a.count = (u32)data.size();
		accessors.push_back(a);
		return (int)(accessors.size() - 1);
	}

	int addUShortAccessor(const std::vector<std::array<u32, 4>>& data, const char*)
	{
		std::vector<std::array<u16, 4>> conv;
		conv.reserve(data.size());
		for (const auto& a : data)
		{
			std::array<u16, 4> c;
			for (int k = 0; k < 4; ++k)
			{
				if (a[k] > 65535) fail(GltfError::SIZE_OVERFLOW, "joint index exceeds ushort range");
				c[k] = (u16)a[k];
			}
			conv.push_back(c);
		}
		Accessor a;
		a.buffer_view = addView(conv.data(), (u32)conv.size(), 4, 34962);
		a.component_type = 5123;
		a.type = "VEC4";
		a.count = (u32)conv.size();
		accessors.push_back(a);
		return (int)(accessors.size() - 1);
	}

	int addUIntAccessor(const std::vector<std::array<u32, 4>>& data, const char*, u32)
	{
		Accessor a;
		a.buffer_view = addView(data.data(), (u32)data.size(), 4, 34962);
		a.component_type = 5125;
		a.type = "VEC4";
		a.count = (u32)data.size();
		accessors.push_back(a);
		return (int)(accessors.size() - 1);
	}

	int addMat4Accessor(const std::vector<M4>& data)
	{
		std::vector<float> flat;
		flat.reserve(data.size() * 16);
		for (const M4& m : data)
			for (int k = 0; k < 16; ++k)
				flat.push_back(toFiniteFloat(m.m[k], GltfError::NUMERIC_OVERFLOW, "inverse bind matrix"));
		Accessor a;
		a.buffer_view = addView(flat.data(), (u32)flat.size(), 16, 0);
		a.component_type = 5126;
		a.type = "MAT4";
		a.count = (u32)data.size();
		accessors.push_back(a);
		return (int)(accessors.size() - 1);
	}

	int addTimeAccessor(const std::vector<float>& times)
	{
		Accessor a;
		a.buffer_view = addView(times.data(), (u32)times.size(), 4, 0);
		a.component_type = 5126;
		a.type = "SCALAR";
		a.count = (u32)times.size();
		a.has_min_max = true;
		a.mn = {times.front()};
		a.mx = {times.back()};
		accessors.push_back(a);
		return (int)(accessors.size() - 1);
	}

	template <typename T>
	int addPlainAccessor(const std::vector<T>& data, const char* type, u32 elem_align)
	{
		Accessor a;
		a.buffer_view = addView(data.data(), (u32)data.size(), elem_align, 0);
		a.component_type = 5126;
		a.type = type;
		a.count = (u32)data.size();
		accessors.push_back(a);
		return (int)(accessors.size() - 1);
	}

	void emitBinary()
	{
		// Turn pending tracks into samplers/channels. Rotation tracks become
		// VEC4 quaternion outputs, translations/scales VEC3; every track keeps
		// its own time accessor (channels may be fully desynchronized).
		for (const PendingChannel& pc : pending_channels)
		{
			const ScalarTrack& tr = owned_tracks[pc.track];
			if (tr.times.size() < 2 || tr.times.size() != tr.values.size())
				fail(GltfError::INTERNAL_ERROR, "malformed animation track");
			int input = addTimeAccessor(tr.times);
			int output;
			if (pc.path == PATH_ROTATION)
			{
				const V3 axes[3] = {v3(1, 0, 0), v3(0, 1, 0), v3(0, 0, 1)};
				std::vector<std::array<float, 4>> quats;
				quats.reserve(tr.values.size());
				for (float deg : tr.values)
				{
					Q4 q = qAxisAngle(axes[pc.axis], (double)deg * DEG2RAD);
					quats.push_back({(float)q.x, (float)q.y, (float)q.z, (float)q.w});
				}
				output = addPlainAccessor(quats, "VEC4", 16);
			}
			else if (pc.path == PATH_TRANSLATION)
			{
				std::vector<std::array<float, 3>> vecs;
				vecs.reserve(tr.values.size());
				for (float v : tr.values)
					vecs.push_back(pc.axis == 0 ? std::array<float, 3>{v, 0, 0}
						: pc.axis == 1 ? std::array<float, 3>{0, v, 0}
							: std::array<float, 3>{0, 0, v});
				output = addPlainAccessor(vecs, "VEC3", 4);
			}
			else
			{
				std::vector<std::array<float, 3>> vecs;
				vecs.reserve(tr.values.size());
				for (float v : tr.values)
					vecs.push_back(pc.axis == 0 ? std::array<float, 3>{v, 1, 1}
						: pc.axis == 1 ? std::array<float, 3>{1, v, 1}
							: std::array<float, 3>{1, 1, v});
				output = addPlainAccessor(vecs, "VEC3", 4);
			}
			Sampler sm{input, output};
			samplers.push_back(sm);
			Channel ch{(int)(samplers.size() - 1), pc.node, pc.path};
			channels.push_back(ch);
		}
		// Pad BIN chunk to 4 bytes; glTF requires a minimum 1 byte buffer.
		alignBuffer(4);
		if (bin.empty())
		{
			bin.push_back(0); bin.push_back(0); bin.push_back(0); bin.push_back(0);
		}
	}

	// ---------------------------------------------------------------
	// JSON + GLB assembly
	// ---------------------------------------------------------------
	static std::string jsonEscape(const std::string& s)
	{
		std::string o;
		o.reserve(s.size() + 2);
		for (char c : s)
		{
			switch (c)
			{
				case '"': o += "\\\""; break;
				case '\\': o += "\\\\"; break;
				case '\n': o += "\\n"; break;
				case '\r': o += "\\r"; break;
				case '\t': o += "\\t"; break;
				default:
					if ((unsigned char)c < 0x20)
					{
						char buf[8];
						std::snprintf(buf, sizeof(buf), "\\u%04x", (unsigned char)c);
						o += buf;
					}
					else o += c;
			}
		}
		return o;
	}

	static std::string fnum(float f)
	{
		char buf[32];
		std::snprintf(buf, sizeof(buf), "%.9g", (double)f);
		return buf;
	}

	void emitGlb()
	{
		if (bin.size() > (u64)std::numeric_limits<u32>::max() - 1024)
			fail(GltfError::SIZE_OVERFLOW, "GLB binary chunk too large");
		std::string j;
		j.reserve(4096 + nodes.size() * 128);
		j += "{\"asset\":{\"version\":\"2.0\",\"generator\":\"ofbx_gltf\"},\"scene\":0,\"scenes\":[{\"nodes\":[";
		bool first = true;
		for (u32 i = 0; i < nodes.size(); ++i)
			if (nodes[i].parent == -1)
			{
				if (!first) j += ",";
				j += std::to_string(i);
				first = false;
			}
		j += "]}],\"nodes\":[";
		for (u32 i = 0; i < nodes.size(); ++i)
		{
			const JsonNode& n = nodes[i];
			if (i) j += ",";
			j += "{\"name\":\"";
			j += jsonEscape(n.name);
			j += "\"";
			if (n.has_t)
				j += ",\"translation\":[" + fnum(n.t[0]) + "," + fnum(n.t[1]) + "," + fnum(n.t[2]) + "]";
			if (n.has_r)
				j += ",\"rotation\":[" + fnum(n.r[0]) + "," + fnum(n.r[1]) + "," + fnum(n.r[2]) + "," + fnum(n.r[3]) + "]";
			if (n.has_s)
				j += ",\"scale\":[" + fnum(n.s[0]) + "," + fnum(n.s[1]) + "," + fnum(n.s[2]) + "]";
			if (!n.children.empty())
			{
				j += ",\"children\":[";
				for (size_t k = 0; k < n.children.size(); ++k)
				{
					if (k) j += ",";
					j += std::to_string(n.children[k]);
				}
				j += "]";
			}
			if (n.mesh >= 0) j += ",\"mesh\":" + std::to_string(n.mesh);
			if (n.skin >= 0) j += ",\"skin\":" + std::to_string(n.skin);
			j += "}";
		}
		j += "],\"meshes\":[";
		for (u32 i = 0; i < meshes.size(); ++i)
		{
			const JsonMesh& m = meshes[i];
			if (i) j += ",";
			j += "{\"name\":\"" + jsonEscape(m.name) + "\",\"primitives\":[{\"mode\":4,\"attributes\":{\"POSITION\":";
			j += std::to_string(m.primitive.positions);
			if (m.primitive.joints >= 0)
			{
				j += ",\"JOINTS_0\":" + std::to_string(m.primitive.joints);
				j += ",\"WEIGHTS_0\":" + std::to_string(m.primitive.weights);
			}
			j += "}}]}";
		}
		j += "],\"skins\":[";
		for (u32 i = 0; i < skins.size(); ++i)
		{
			const JsonSkin& sk = skins[i];
			if (i) j += ",";
			j += "{\"joints\":[";
			for (size_t k = 0; k < sk.joints.size(); ++k)
			{
				if (k) j += ",";
				j += std::to_string(sk.joints[k]);
			}
			j += "],\"inverseBindMatrices\":" + std::to_string(sk.ibm) + "}";
		}
		j += "],\"accessors\":[";
		for (u32 i = 0; i < accessors.size(); ++i)
		{
			const Accessor& a = accessors[i];
			if (i) j += ",";
			j += "{\"bufferView\":" + std::to_string(a.buffer_view);
			j += ",\"componentType\":" + std::to_string(a.component_type);
			j += ",\"count\":" + std::to_string(a.count);
			j += ",\"type\":\"" + a.type + "\"";
			if (a.has_min_max)
			{
				j += ",\"min\":[";
				for (size_t k = 0; k < a.mn.size(); ++k) { if (k) j += ","; j += fnum(a.mn[k]); }
				j += "],\"max\":[";
				for (size_t k = 0; k < a.mx.size(); ++k) { if (k) j += ","; j += fnum(a.mx[k]); }
				j += "]";
			}
			j += "}";
		}
		j += "],\"bufferViews\":[";
		for (u32 i = 0; i < views.size(); ++i)
		{
			const BufferView& v = views[i];
			if (i) j += ",";
			j += "{\"buffer\":0,\"byteOffset\":" + std::to_string(v.offset)
				+ ",\"byteLength\":" + std::to_string(v.length);
			if (v.target) j += ",\"target\":" + std::to_string(v.target);
			j += "}";
		}
		j += "],\"buffers\":[{\"byteLength\":" + std::to_string(bin.size()) + "}]";
		if (!samplers.empty())
		{
			j += ",\"animations\":[{\"name\":\"layer\",\"samplers\":[";
			for (u32 i = 0; i < samplers.size(); ++i)
			{
				if (i) j += ",";
				j += "{\"input\":" + std::to_string(samplers[i].input)
					+ ",\"output\":" + std::to_string(samplers[i].output)
					+ ",\"interpolation\":\"LINEAR\"}";
			}
			j += "],\"channels\":[";
			for (u32 i = 0; i < channels.size(); ++i)
			{
				const Channel& c = channels[i];
				if (i) j += ",";
				const char* path = c.path == PATH_TRANSLATION ? "translation"
					: c.path == PATH_ROTATION ? "rotation" : "scale";
				j += "{\"sampler\":" + std::to_string(c.sampler)
					+ ",\"target\":{\"node\":" + std::to_string(c.node)
					+ ",\"path\":\"" + path + "\"}}";
			}
			j += "]}]";
		}
		j += "}";

		// JSON chunk padded with spaces, BIN chunk padded with zero bytes.
		while (j.size() % 4 != 0) j.push_back(' ');
		const u32 json_len = (u32)j.size();
		const u32 bin_len = (u32)bin.size();
		const u32 total = 12 + 8 + json_len + (bin_len ? 8 + bin_len : 0);
		glb_bytes.resize(total);
		u32 w = 0;
		auto put32 = [&](u32 v) {
			std::memcpy(glb_bytes.data() + w, &v, 4);
			w += 4;
		};
		put32(0x46546C67); // glTF
		put32(2);
		put32(total);
		put32(json_len);
		put32(0x4E4F534A); // JSON
		std::memcpy(glb_bytes.data() + w, j.data(), json_len);
		w += json_len;
		if (bin_len)
		{
			put32(bin_len);
			put32(0x004E4942); // BIN
			std::memcpy(glb_bytes.data() + w, bin.data(), bin_len);
			w += bin_len;
		}
		if (w != total) fail(GltfError::INTERNAL_ERROR, "GLB length mismatch");
	}
};

} // namespace

GltfError exportGltf(const IScene& scene, const AnimationLayer& layer,
	double begin, double end, GltfResult& out)
{
	Builder b(scene, layer, begin, end);
	return b.run(out);
}

} // namespace ofbx
