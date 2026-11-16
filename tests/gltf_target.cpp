// Offline FBX -> GLB exporter target tests.
//
// The cases synthesize small ASCII FBX documents (same dialect as
// task_support/rig.fbx), load them through the production ofbx::load() path,
// export through ofbx::exportGltf(), then independently parse/validate/sample
// the GLB with vendor/cgltf and compare against both the source production
// path (Object::evalLocal / getGlobalTransform-style accumulation) and hand
// computed matrices and skin points.
#include "task_support/check.h"

#include "src/ofbx_gltf.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

#define CGLTF_IMPLEMENTATION
#include "vendor/cgltf/cgltf.h"

namespace
{

using ofbx::u32;
using ofbx::u64;
using ofbx::DVec3;
using ofbx::DMatrix;

constexpr double TICKS_PER_SECOND = 46186158000.0;
constexpr double PI = 3.14159265358979323846;

std::string tempPath(const char* name)
{
	const char* dir = std::getenv("TMPDIR");
	std::string p = dir ? dir : "/tmp/";
	if (!p.empty() && p.back() != '/') p.push_back('/');
	p += name;
	return p;
}

ofbx::IScene* loadText(const std::string& text, std::vector<unsigned char>& storage)
{
	storage.assign(text.begin(), text.end());
	return ofbx::load(storage.data(), storage.size(), 0);
}

// ---------------------------------------------------------------------------
// ASCII FBX builders
// ---------------------------------------------------------------------------

std::string curve(u64 id, const std::vector<double>& times, const std::vector<double>& values)
{
	std::string s = " AnimationCurve: " + std::to_string(id) + ", \"AC\", \"\" {\n";
	s += "  KeyTime: *" + std::to_string(times.size()) + " { a:";
	for (double t : times) s += " " + std::to_string((long long)(t * TICKS_PER_SECOND)) + ",";
	s.back() = ';';
	s += " }\n  KeyValueFloat: *" + std::to_string(values.size()) + " { a:";
	for (double v : values) s += " " + std::to_string(v) + ",";
	s.back() = ';';
	s += " }\n }\n";
	return s;
}

const char* AXIS_PROP[3] = {"d|X", "d|Y", "d|Z"};

// One animated property (curve node) with up to three independent axis curves.
struct AxisCurve
{
	int axis;                 // 0=X,1=Y,2=Z
	std::vector<double> times;
	std::vector<double> values;
	double default_value = 0;
};

struct FbxSceneBuilder
{
	std::string objects;
	std::vector<std::string> connections;
	u64 next_id = 100;

	void connOO(u64 from, u64 to)
	{
		connections.push_back(" C: \"OO\"," + std::to_string(from) + "," + std::to_string(to) + "\n");
	}
	void connOP(u64 from, u64 to, const char* prop)
	{
		connections.push_back(" C: \"OP\"," + std::to_string(from) + "," + std::to_string(to)
			+ ",\"" + prop + "\"\n");
	}

	void addNull(u64 id, const char* name)
	{
		objects += " Model: " + std::to_string(id) + ", \"Model::" + name + "\", \"Null\" { }\n";
		connOO(id, 0);
	}

	// Adds a limb node with arbitrary transform properties.
	void addLimb(u64 id, const char* name, u64 parent, int rotation_order = 0,
		const DVec3& lcl_t = {0, 0, 0}, const DVec3& lcl_r = {0, 0, 0}, const DVec3& lcl_s = {1, 1, 1},
		const DVec3& r_pivot = {0, 0, 0}, const DVec3& s_pivot = {0, 0, 0},
		const DVec3& r_offset = {0, 0, 0}, const DVec3& s_offset = {0, 0, 0},
		const DVec3& pre = {0, 0, 0}, const DVec3& post = {0, 0, 0})
	{
		objects += " Model: " + std::to_string(id) + ", \"Model::" + name + "\", \"LimbNode\" {\n  Properties70: {\n";
		auto vec = [](const char* n, const DVec3& v) {
			char b[256];
			std::snprintf(b, sizeof(b), "   P: \"%s\", \"Vector3D\", \"Vector\", \"\",%.12g,%.12g,%.12g\n",
				n, v.x, v.y, v.z);
			return std::string(b);
		};
		objects += vec("Lcl Translation", lcl_t);
		objects += vec("Lcl Rotation", lcl_r);
		objects += vec("Lcl Scaling", lcl_s);
		objects += vec("RotationPivot", r_pivot);
		objects += vec("ScalingPivot", s_pivot);
		objects += vec("RotationOffset", r_offset);
		objects += vec("ScalingOffset", s_offset);
		objects += vec("PreRotation", pre);
		objects += vec("PostRotation", post);
		char order_buf[96];
		std::snprintf(order_buf, sizeof(order_buf),
			"   P: \"RotationOrder\", \"enum\", \"\", \"A\",%d\n", rotation_order);
		objects += order_buf;
		objects += "  }\n }\n";
		connOO(id, parent);
	}

	void addAnimatedProperty(u64 model_id, const char* property, const std::vector<AxisCurve>& axes,
		double unanimated_default = 0.0)
	{
		const u64 cn_id = next_id++;
		objects += " AnimationCurveNode: " + std::to_string(cn_id) + ", \"ACN\", \"\" {\n  Properties70: {\n";
		for (int a = 0; a < 3; ++a)
		{
			double dflt = unanimated_default;
			for (const AxisCurve& ac : axes) if (ac.axis == a) dflt = ac.default_value;
			char b[128];
			std::snprintf(b, sizeof(b), "   P: \"d|%c\", \"Number\", \"\", \"A\",%.12g\n",
				a == 0 ? 'X' : a == 1 ? 'Y' : 'Z', dflt);
			objects += b;
		}
		objects += "  }\n }\n";
		connOP(cn_id, model_id, property);
		layer_curve_nodes.push_back(cn_id);
		for (const AxisCurve& ac : axes)
		{
			const u64 curve_id = next_id++;
			objects += curve(curve_id, ac.times, ac.values);
			connOP(curve_id, cn_id, AXIS_PROP[ac.axis]);
		}
	}

	void addStackLayer(u64 stack_id, u64 layer_id)
	{
		objects += " AnimationStack: " + std::to_string(stack_id) + ", \"Stack\", \"\" { }\n";
		objects += " AnimationLayer: " + std::to_string(layer_id) + ", \"Layer\", \"\" { }\n";
		connOO(layer_id, stack_id);
	}

	void attachCurveNodesToLayer(u64 layer_id)
	{
		// All AnimationCurveNode objects created so far attach to the layer.
		// Tracked externally via markLayerCurveNode.
		for (u64 id : layer_curve_nodes) connOO(id, layer_id);
	}

	std::vector<u64> layer_curve_nodes;

	std::string finish(u64 stack_id, u64 layer_id)
	{
		attachCurveNodesToLayer(layer_id);
		std::string fbx = "; FBX 7.4.0 project file\nFBXHeaderExtension: { FBXHeaderVersion: 1003\n FBXVersion: 7400\n}\nObjects: {\n";
		fbx += objects;
		fbx += "}\nConnections: {\n";
		for (const std::string& c : connections) fbx += c;
		fbx += "}\n";
		(void)stack_id;
		return fbx;
	}
};

// ---------------------------------------------------------------------------
// glTF sampling helpers (glTF 2.0 LINEAR rules)
// ---------------------------------------------------------------------------

void mulMat4(const float a[16], const float b[16], float out[16])
{
	float r[16];
	for (int j = 0; j < 4; ++j)
		for (int i = 0; i < 4; ++i)
		{
			float s = 0;
			for (int k = 0; k < 4; ++k) s += a[i + k * 4] * b[k + j * 4];
			r[i + j * 4] = s;
		}
	for (int i = 0; i < 16; ++i) out[i] = r[i];
}

void slerp4(const float a[4], const float b[4], float t, float out[4])
{
	float d = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3];
	float bb[4] = {b[0], b[1], b[2], b[3]};
	if (d < 0)
	{
		d = -d;
		for (float& x : bb) x = -x;
	}
	if (d > 0.9995f)
	{
		for (int i = 0; i < 4; ++i) out[i] = a[i] + (bb[i] - a[i]) * t;
		return;
	}
	float theta = std::acos(std::fmin(1.0f, std::fmax(-1.0f, d)));
	float s0 = std::sin((1 - t) * theta) / std::sin(theta);
	float s1 = std::sin(t * theta) / std::sin(theta);
	for (int i = 0; i < 4; ++i) out[i] = s0 * a[i] + s1 * bb[i];
}

void applyAnimation(cgltf_data* data, float time)
{
	for (cgltf_size a = 0; a < data->animations_count; ++a)
	{
		const cgltf_animation& anim = data->animations[a];
		for (cgltf_size s = 0; s < anim.samplers_count; ++s)
		{
			const cgltf_animation_sampler& sm = anim.samplers[s];
			const cgltf_size n = sm.input->count;
			cgltf_size i0 = 0, i1 = n - 1;
			float t0, t1;
			cgltf_accessor_read_float(sm.input, 0, &t0, 1);
			cgltf_accessor_read_float(sm.input, n - 1, &t1, 1);
			for (cgltf_size i = 1; i < n; ++i)
			{
				float t;
				cgltf_accessor_read_float(sm.input, i, &t, 1);
				if (t >= time)
				{
					i1 = i;
					i0 = i - 1;
					cgltf_accessor_read_float(sm.input, i0, &t0, 1);
					t1 = t;
					break;
				}
			}
			float alpha = t1 > t0 ? (time - t0) / (t1 - t0) : 0.0f;
			cgltf_size comps = cgltf_num_components(sm.output->type);
			float v0[16], v1[16], v[16];
			cgltf_accessor_read_float(sm.output, i0, v0, comps);
			cgltf_accessor_read_float(sm.output, i1, v1, comps);
			for (cgltf_size c = 0; c < anim.channels_count; ++c)
			{
				if (anim.channels[c].sampler != &sm) continue;
				cgltf_node* node = anim.channels[c].target_node;
				if (anim.channels[c].target_path == cgltf_animation_path_type_rotation && comps == 4)
				{
					slerp4(v0, v1, alpha, v);
					for (int k = 0; k < 4; ++k) node->rotation[k] = v[k];
				}
				else
				{
					for (cgltf_size k = 0; k < comps; ++k) v[k] = v0[k] + (v1[k] - v0[k]) * alpha;
					if (anim.channels[c].target_path == cgltf_animation_path_type_translation)
						for (int k = 0; k < 3; ++k) node->translation[k] = v[k];
					else if (anim.channels[c].target_path == cgltf_animation_path_type_scale)
						for (int k = 0; k < 3; ++k) node->scale[k] = v[k];
				}
			}
		}
	}
}

void nodeWorld(const cgltf_node* node, float out[16])
{
	float local[16];
	cgltf_node_transform_local(node, local);
	if (!node->parent)
	{
		for (int i = 0; i < 16; ++i) out[i] = local[i];
		return;
	}
	float pw[16];
	nodeWorld(node->parent, pw);
	mulMat4(pw, local, out);
}

// ---------------------------------------------------------------------------
// Source production path sampling
// ---------------------------------------------------------------------------

const ofbx::Object* findObject(const ofbx::IScene* scene, u64 id)
{
	for (int i = 0; i < scene->getAllObjectCount(); ++i)
		if (scene->getAllObjects()[i]->id == id) return scene->getAllObjects()[i];
	if (scene->getRoot() && scene->getRoot()->id == id) return scene->getRoot();
	return nullptr;
}

ofbx::DMatrix sourceLocal(const ofbx::Object* obj, const ofbx::AnimationLayer* layer, double time)
{
	ofbx::DVec3 t = obj->getLocalTranslation();
	ofbx::DVec3 r = obj->getLocalRotation();
	ofbx::DVec3 s = obj->getLocalScaling();
	if (layer)
	{
		if (auto* cn = layer->getCurveNode(*obj, "Lcl Translation")) t = cn->getNodeLocalTransform(time);
		if (auto* cn = layer->getCurveNode(*obj, "Lcl Rotation")) r = cn->getNodeLocalTransform(time);
		if (auto* cn = layer->getCurveNode(*obj, "Lcl Scaling")) s = cn->getNodeLocalTransform(time);
	}
	return obj->evalLocal(t, r, s);
}

ofbx::DMatrix sourceWorld(const ofbx::Object* obj, const ofbx::AnimationLayer* layer, double time,
	std::map<u64, ofbx::DMatrix>& cache)
{
	auto it = cache.find(obj->id);
	if (it != cache.end()) return it->second;
	ofbx::DMatrix local = sourceLocal(obj, layer, time);
	const ofbx::Object* parent = obj->getParent();
	if (!parent)
	{
		cache[obj->id] = local;
		return local;
	}
	ofbx::DMatrix pw = sourceWorld(parent, layer, time, cache);
	ofbx::DMatrix world{};
	for (int col = 0; col < 4; ++col)
		for (int row = 0; row < 4; ++row)
			for (int k = 0; k < 4; ++k)
				world.m[col * 4 + row] += pw.m[k * 4 + row] * local.m[col * 4 + k];
	cache[obj->id] = world;
	return world;
}

bool matricesClose(const float a[16], const ofbx::DMatrix& b, double eps = 2e-5)
{
	for (int i = 0; i < 16; ++i)
	{
		double ref = std::max(1.0, std::abs(b.m[i]));
		if (std::abs((double)a[i] - b.m[i]) > eps * ref) return false;
	}
	return true;
}

ofbx::DMatrix dmul(const ofbx::DMatrix& a, const ofbx::DMatrix& b)
{
	ofbx::DMatrix r{};
	for (int col = 0; col < 4; ++col)
		for (int row = 0; row < 4; ++row)
			for (int k = 0; k < 4; ++k)
				r.m[col * 4 + row] += a.m[k * 4 + row] * b.m[col * 4 + k];
	return r;
}

ofbx::DMatrix dinverse(const ofbx::DMatrix& in)
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
	ofbx::DMatrix out{};
	for (int r = 0; r < 4; ++r)
		for (int c = 0; c < 4; ++c)
			out.m[c * 4 + r] = a[r][c + 4];
	return out;
}

ofbx::DVec3 xformPoint(const ofbx::DMatrix& m, ofbx::DVec3 p)
{
	return {m.m[0] * p.x + m.m[4] * p.y + m.m[8] * p.z + m.m[12],
		m.m[1] * p.x + m.m[5] * p.y + m.m[9] * p.z + m.m[13],
		m.m[2] * p.x + m.m[6] * p.y + m.m[10] * p.z + m.m[14]};
}

bool vecClose(const ofbx::DVec3& a, const ofbx::DVec3& b, double eps = 2e-5)
{
	return std::abs(a.x - b.x) < eps && std::abs(a.y - b.y) < eps && std::abs(a.z - b.z) < eps;
}

struct ParsedGlb
{
	cgltf_data* data = nullptr;
	~ParsedGlb() { if (data) cgltf_free(data); }
};

bool parseGlb(const ofbx::GltfResult& result, ParsedGlb& out)
{
	cgltf_options options = {};
	if (cgltf_parse(&options, result.glb.data(), result.glb.size(), &out.data) != cgltf_result_success)
		return false;
	if (cgltf_validate(out.data) != cgltf_result_success) return false;
	return cgltf_load_buffers(&options, out.data, nullptr) == cgltf_result_success;
}

// ===========================================================================
// factor group
// ===========================================================================

int factor_pivots()
{
	FbxSceneBuilder b;
	const u64 root = 1, limb = 2;
	b.addNull(root, "Root");
	// Pivots, offsets, Pre/PostRotation and non uniform scale all at once.
	b.addLimb(limb, "PivotLimb", root, 0,
		{3, 0, 0}, {20, 0, 0}, {2, 3, 4},
		{1, 2, 3}, {-1, 0, 2}, {0, 5, 0}, {0, 0, -2},
		{0, 0, 30}, {0, 40, 0});
	const u64 stack = 10, layer = 11;
	b.addStackLayer(stack, layer);
	std::string text = b.finish(stack, layer);

	std::vector<unsigned char> storage;
	ofbx::IScene* scene = loadText(text, storage);
	TASK_CHECK(scene != nullptr);
	const ofbx::AnimationLayer* al = scene->getAnimationStack(0)->getLayer(0);
	TASK_CHECK(al);

	ofbx::GltfResult result;
	ofbx::GltfError err = ofbx::exportGltf(*scene, *al, 0.0, 1.0, result);
	TASK_CHECK(err == ofbx::GltfError::OK);
	TASK_CHECK(result.node_map.count(limb) == 1);

	ParsedGlb glb;
	TASK_CHECK(parseGlb(result, glb));
	TASK_CHECK(glb.data->animations_count == 0); // fully static scene

	const ofbx::Object* obj = findObject(scene, limb);
	for (double t : {0.0, 0.25, 0.5, 0.75, 1.0})
	{
		std::map<u64, ofbx::DMatrix> cache;
		ofbx::DMatrix sw = sourceWorld(obj, al, t, cache);
		float gw[16];
		nodeWorld(&glb.data->nodes[result.node_map.at(limb)], gw);
		TASK_CHECK(matricesClose(gw, sw));
	}

	// The shared factor definition must agree with evalLocal exactly.
	ofbx::LocalTransformFactors f = ofbx::getLocalTransformFactors(*obj,
		obj->getLocalTranslation(), obj->getLocalRotation(), obj->getLocalScaling());
	TASK_CHECK(f.rotation_pivot.x == 1.0 && f.rotation_pivot.y == 2.0 && f.rotation_pivot.z == 3.0);
	TASK_CHECK(f.pre_rotation.z == 30.0 && f.post_rotation.y == 40.0);
	ofbx::DMatrix composed = ofbx::composeLocalTransform(f);
	ofbx::DMatrix direct = obj->evalLocal(obj->getLocalTranslation(), obj->getLocalRotation(),
		obj->getLocalScaling());
	for (int i = 0; i < 16; ++i) TASK_CHECK(std::abs(composed.m[i] - direct.m[i]) < 1e-12);

	scene->destroy();
	return 0;
}

int factor_layout_and_defaults()
{
	FbxSceneBuilder b;
	const u64 root = 1, limb = 2;
	b.addNull(root, "Root");
	// Curve node with a non zero default but only one animated axis: the other
	// two axes must pick up the curve node default, not the object static TRS.
	b.addLimb(limb, "DefaultLimb", root, 0, {0, 0, 0}, {0, 0, 0}, {1, 1, 1});
	u64 cn = b.next_id++;
	b.objects += " AnimationCurveNode: " + std::to_string(cn) + ", \"ACN\", \"\" {\n  Properties70: {\n"
		"   P: \"d|X\", \"Number\", \"\", \"A\",0\n"
		"   P: \"d|Y\", \"Number\", \"\", \"A\",90\n"
		"   P: \"d|Z\", \"Number\", \"\", \"A\",0\n  }\n }\n";
	b.connOP(cn, limb, "Lcl Rotation");
	b.layer_curve_nodes.push_back(cn);
	u64 curve_id = b.next_id++;
	b.objects += curve(curve_id, {0, 1}, {0, 90});
	b.connOP(curve_id, cn, "d|X");
	const u64 stack = 10, layer = 11;
	b.addStackLayer(stack, layer);
	std::string text = b.finish(stack, layer);

	std::vector<unsigned char> storage;
	ofbx::IScene* scene = loadText(text, storage);
	TASK_CHECK(scene != nullptr);
	const ofbx::AnimationLayer* al = scene->getAnimationStack(0)->getLayer(0);

	ofbx::GltfResult result;
	TASK_CHECK(ofbx::exportGltf(*scene, *al, 0.0, 1.0, result) == ofbx::GltfError::OK);

	// GLB physical layout: declared total length equals the bytes, every chunk
	// is 4 byte aligned and no extensions are present.
	TASK_CHECK(result.glb.size() >= 20);
	u32 total;
	std::memcpy(&total, result.glb.data() + 8, 4);
	TASK_CHECK(total == result.glb.size());
	TASK_CHECK(result.glb.size() % 4 == 0);

	ParsedGlb glb;
	TASK_CHECK(parseGlb(result, glb));
	// POSITION-less skeleton still validates; time accessor min/max present.
	const ofbx::Object* obj = findObject(scene, limb);
	for (double t : {0.1, 0.5, 0.9})
	{
		std::map<u64, ofbx::DMatrix> cache;
		ofbx::DMatrix sw = sourceWorld(obj, al, t, cache);
		// Re-parse for a clean node state each sample.
		ParsedGlb sample;
		TASK_CHECK(parseGlb(result, sample));
		applyAnimation(sample.data, (float)t);
		float gw[16];
		nodeWorld(&sample.data->nodes[result.node_map.at(limb)], gw);
		TASK_CHECK(matricesClose(gw, sw));
	}
	bool has_time_minmax = false;
	for (cgltf_size i = 0; i < glb.data->accessors_count; ++i)
	{
		cgltf_accessor& ac = glb.data->accessors[i];
		if (ac.type == cgltf_type_scalar)
		{
			TASK_CHECK(ac.has_min && ac.has_max);
			has_time_minmax = true;
		}
		TASK_CHECK(ac.buffer_view->offset % 4 == 0);
		TASK_CHECK(ac.buffer_view->size % 4 == 0);
	}
	TASK_CHECK(has_time_minmax);
	TASK_CHECK(glb.data->data_extensions_count == 0);

	scene->destroy();
	return 0;
}

// ===========================================================================
// motion group
// ===========================================================================

int motion_desynced_six_orders()
{
	for (int order = 0; order < 6; ++order)
	{
		FbxSceneBuilder b;
		const u64 root = 1, limb = 2;
		b.addNull(root, "Root");
		b.addLimb(limb, "OrderLimb", root, order, {1, 2, 3}, {0, 0, 0}, {1, 1, 1});
		// X keys and Z keys use different time tables (desynchronized).
		b.addAnimatedProperty(limb, "Lcl Rotation", {
			{0, {0.0, 0.4, 1.0}, {0, 70, 30}},
			{2, {0.0, 0.7, 1.0}, {0, -60, 80}},
		});
		// Y stays on its curve node default (no d|Y curve): default 0, which
		// exercises the uncurved-axis static path.
		const u64 stack = 10, layer = 11;
		b.addStackLayer(stack, layer);
		std::string text = b.finish(stack, layer);

		std::vector<unsigned char> storage;
		ofbx::IScene* scene = loadText(text, storage);
		TASK_CHECK(scene != nullptr);
		const ofbx::AnimationLayer* al = scene->getAnimationStack(0)->getLayer(0);

		ofbx::GltfResult result;
		// Non zero begin: output time must restart at zero.
		TASK_CHECK(ofbx::exportGltf(*scene, *al, 0.2, 1.2, result) == ofbx::GltfError::OK);

		// Each euler axis keeps its own sampler (no merged quaternion curve).
		ParsedGlb glb;
		TASK_CHECK(parseGlb(result, glb));
		unsigned rotation_channels = 0;
		for (cgltf_size i = 0; i < glb.data->animations[0].channels_count; ++i)
			if (glb.data->animations[0].channels[i].target_path == cgltf_animation_path_type_rotation)
				++rotation_channels;
		TASK_CHECK(rotation_channels == 2);
		// Two distinct input accessors (distinct time tables).
		const cgltf_accessor* in0 = glb.data->animations[0].samplers[0].input;
		const cgltf_accessor* in1 = glb.data->animations[0].samplers[1].input;
		TASK_CHECK(in0 != in1);
		float first_time;
		cgltf_accessor_read_float(in0, 0, &first_time, 1);
		TASK_CHECK(first_time == 0.0f);

		const ofbx::Object* obj = findObject(scene, limb);
		for (double t : {0.2, 0.31, 0.55, 0.8, 1.0, 1.19})
		{
			std::map<u64, ofbx::DMatrix> cache;
			ofbx::DMatrix sw = sourceWorld(obj, al, t, cache);
			ParsedGlb sample;
			TASK_CHECK(parseGlb(result, sample));
			applyAnimation(sample.data, (float)(t - 0.2));
			float gw[16];
			nodeWorld(&sample.data->nodes[result.node_map.at(limb)], gw);
			TASK_CHECK(matricesClose(gw, sw));
		}
		scene->destroy();
	}
	return 0;
}

int motion_multiturn_pivot()
{
	FbxSceneBuilder b;
	const u64 root = 1, plus = 2, minus = 3, loops = 4;
	b.addNull(root, "Root");
	// 450 degree positive turn around a rotation pivot.
	b.addLimb(plus, "Plus450", root, 0, {0, 0, 0}, {0, 0, 0}, {1, 1, 1}, {2, 0, 0});
	b.addAnimatedProperty(plus, "Lcl Rotation", {{2, {0, 1}, {0, 450}}});
	// Negative 450 degree turn.
	b.addLimb(minus, "Minus450", root, 0, {0, 0, 0}, {0, 0, 0}, {1, 1, 1}, {0, 3, 0});
	b.addAnimatedProperty(minus, "Lcl Rotation", {{2, {0, 1}, {0, -450}}});
	// More than two full turns (810 degrees) crossing several loops.
	b.addLimb(loops, "Loops810", root, 0, {1, 1, 0}, {0, 0, 0}, {1, 1, 1});
	b.addAnimatedProperty(loops, "Lcl Rotation", {{0, {0, 0.5, 1}, {0, 405, 810}}});
	const u64 stack = 10, layer = 11;
	b.addStackLayer(stack, layer);
	std::string text = b.finish(stack, layer);

	std::vector<unsigned char> storage;
	ofbx::IScene* scene = loadText(text, storage);
	TASK_CHECK(scene != nullptr);
	const ofbx::AnimationLayer* al = scene->getAnimationStack(0)->getLayer(0);

	ofbx::GltfResult result;
	TASK_CHECK(ofbx::exportGltf(*scene, *al, 0.0, 1.0, result) == ofbx::GltfError::OK);

	// The 450 degree segment must have been subdivided (more than two keys).
	unsigned max_keys = 0;
	ParsedGlb glb;
	TASK_CHECK(parseGlb(result, glb));
	for (cgltf_size i = 0; i < glb.data->animations[0].samplers_count; ++i)
		max_keys = std::max(max_keys, (unsigned)glb.data->animations[0].samplers[i].input->count);
	TASK_CHECK(max_keys >= 4);

	for (u64 id : {plus, minus, loops})
	{
		const ofbx::Object* obj = findObject(scene, id);
		for (double t : {0.05, 0.2, 0.37, 0.5, 0.63, 0.8, 0.95})
		{
			std::map<u64, ofbx::DMatrix> cache;
			ofbx::DMatrix sw = sourceWorld(obj, al, t, cache);
			ParsedGlb sample;
			TASK_CHECK(parseGlb(result, sample));
			applyAnimation(sample.data, (float)t);
			float gw[16];
			nodeWorld(&sample.data->nodes[result.node_map.at(id)], gw);
			// Multi-turn accumulation tightens the tolerance needs; 2e-5 holds.
			TASK_CHECK(matricesClose(gw, sw, 2e-5));
		}
	}

	// Explicit turning-direction check at t=0.25 for the +450 pivot node.
	// The source euler angle there is 112.5 degrees. A naive bake of the final
	// 450 degree quaternion (equivalent to 90) on one 0->1 shortest-arc track
	// would instead sit at 22.5 degrees; the pivot makes the two translations
	// distinguishable, so assert the long-way value wins.
	const double angle = 112.5 * PI / 180.0;
	ParsedGlb sample;
	TASK_CHECK(parseGlb(result, sample));
	applyAnimation(sample.data, 0.25f);
	float gw[16];
	nodeWorld(&sample.data->nodes[result.node_map.at(plus)], gw);
	std::map<u64, ofbx::DMatrix> cache;
	ofbx::DMatrix sw = sourceWorld(findObject(scene, plus), al, 0.25, cache);
	TASK_CHECK(matricesClose(gw, sw));
	// Local of the pivot node: T(p) R(t) T(-p), translation = p - R(t)*p.
	const ofbx::DVec3 correct{2.0 - 2.0 * std::cos(angle), -2.0 * std::sin(angle), 0.0};
	const ofbx::DVec3 naive{2.0 - 2.0 * std::cos(0.25 * 90.0 * PI / 180.0),
		-2.0 * std::sin(0.25 * 90.0 * PI / 180.0), 0.0};
	ofbx::DVec3 glb_translation{gw[12], gw[13], gw[14]};
	TASK_CHECK(vecClose(glb_translation, correct, 2e-5));
	TASK_CHECK(!vecClose(glb_translation, naive, 1e-3));

	scene->destroy();
	return 0;
}

int motion_scaled_parent()
{
	FbxSceneBuilder b;
	const u64 root = 1, parent = 2, child = 3;
	b.addNull(root, "Root");
	// Non uniform, animated parent scale; child carries its own pivot rotation.
	b.addLimb(parent, "ScaleParent", root, 0, {0, 0, 0}, {0, 0, 0}, {1, 1, 1});
	b.addAnimatedProperty(parent, "Lcl Scaling", {
		{0, {0, 1}, {1, 3}, 1.0},
		{1, {0, 1}, {2, 0.5}, 1.0},
	}, 1.0);
	b.addLimb(child, "PivotChild", parent, 0, {2, 0, 0}, {0, 0, 90}, {1, 1, 1}, {0.5, 0, 0});
	b.addAnimatedProperty(child, "Lcl Rotation", {{2, {0, 1}, {0, 180}}});
	const u64 stack = 10, layer = 11;
	b.addStackLayer(stack, layer);
	std::string text = b.finish(stack, layer);

	std::vector<unsigned char> storage;
	ofbx::IScene* scene = loadText(text, storage);
	TASK_CHECK(scene != nullptr);
	const ofbx::AnimationLayer* al = scene->getAnimationStack(0)->getLayer(0);

	ofbx::GltfResult result;
	TASK_CHECK(ofbx::exportGltf(*scene, *al, 0.0, 1.0, result) == ofbx::GltfError::OK);

	for (u64 id : {parent, child})
	{
		const ofbx::Object* obj = findObject(scene, id);
		for (double t : {0.0, 0.13, 0.33, 0.5, 0.71, 0.9, 1.0})
		{
			std::map<u64, ofbx::DMatrix> cache;
			ofbx::DMatrix sw = sourceWorld(obj, al, t, cache);
			ParsedGlb sample;
			TASK_CHECK(parseGlb(result, sample));
			applyAnimation(sample.data, (float)t);
			float gw[16];
			nodeWorld(&sample.data->nodes[result.node_map.at(id)], gw);
			TASK_CHECK(matricesClose(gw, sw));
		}
	}
	scene->destroy();
	return 0;
}

// ===========================================================================
// skin group
// ===========================================================================

std::string matrixLiteral(const ofbx::DMatrix& m)
{
	std::string s = "*16 { a:";
	for (int i = 0; i < 16; ++i) s += " " + std::to_string(m.m[i]) + ",";
	s.back() = ';';
	s += " }";
	return s;
}

ofbx::DMatrix translationMat(ofbx::DVec3 t)
{
	ofbx::DMatrix m = ofbx::translationMatrix(t);
	return m;
}

// Builds a scene with two meshes sharing one bone but using different
// Transform / TransformLink / geometric transforms, plus an unweighted point.
std::string buildSkinFbx(FbxSceneBuilder& b)
{
	const u64 root = 1, bone = 2;
	const u64 mesh1 = 3, geom1 = 4, skin1 = 5, cluster1 = 6;
	const u64 mesh2 = 7, geom2 = 8, skin2 = 9, cluster2 = 10, cluster2b = 11;
	b.addNull(root, "Root");
	b.addLimb(bone, "SharedBone", root, 0, {5, 0, 0});
	(void)b;

	// Mesh 1: triangle with geometric translation (0,1,0).
	b.objects += " Model: " + std::to_string(mesh1) + ", \"Model::MeshA\", \"Mesh\" {\n  Properties70: {\n"
		"   P: \"GeometricTranslation\", \"Vector3D\", \"Vector\", \"\",0,1,0\n  }\n }\n";
	b.connOO(mesh1, root);
	b.objects += " Geometry: " + std::to_string(geom1) + ", \"Geometry::A\", \"Mesh\" {\n"
		"  Vertices: *9 { a: 0,0,0,1,0,0,0,1,0; }\n"
		"  PolygonVertexIndex: *3 { a: 0,1,-3; }\n }\n";
	b.connOO(geom1, mesh1);
	b.objects += " Deformer: " + std::to_string(skin1) + ", \"SkinA\", \"Skin\" { Version: 101\n }\n";
	b.connOO(skin1, geom1);
	// Binding: Transform = translation(2,3,4); TransformLink = identity.
	ofbx::DMatrix tm1 = translationMat({2, 3, 4});
	ofbx::DMatrix tl1 = ofbx::translationMatrix({0, 0, 0});
	b.objects += " Deformer: " + std::to_string(cluster1) + ", \"ClusterA\", \"Cluster\" {\n"
		"  Version: 100\n  Indexes: *3 { a: 0,1,2; }\n  Weights: *3 { a: 1,1,1; }\n"
		"  Transform: " + matrixLiteral(tm1) + "\n"
		"  TransformLink: " + matrixLiteral(tl1) + "\n }\n";
	b.connOO(cluster1, skin1);
	b.connOO(bone, cluster1);

	// Mesh 2: two triangles. Binding uses scale(2) Transform and a non-unit
	// TransformLink translation(1,1,1); vertex 3 is left unweighted.
	b.objects += " Model: " + std::to_string(mesh2) + ", \"Model::MeshB\", \"Mesh\" { }\n";
	b.connOO(mesh2, root);
	b.objects += " Geometry: " + std::to_string(geom2) + ", \"Geometry::B\", \"Mesh\" {\n"
		"  Vertices: *12 { a: 0,0,0,1,0,0,0,1,0,1,1,0; }\n"
		"  PolygonVertexIndex: *6 { a: 0,1,-3,1,2,-4; }\n }\n";
	b.connOO(geom2, mesh2);
	b.objects += " Deformer: " + std::to_string(skin2) + ", \"SkinB\", \"Skin\" { Version: 101\n }\n";
	b.connOO(skin2, geom2);
	ofbx::DMatrix tm2{};
	tm2.m[0] = 2; tm2.m[5] = 2; tm2.m[10] = 2; tm2.m[15] = 1;
	ofbx::DMatrix tl2 = translationMat({1, 1, 1});
	auto cluster_text = [&](u64 id, const std::string& indexes, const std::string& weights) {
		return " Deformer: " + std::to_string(id) + ", \"ClusterX\", \"Cluster\" {\n"
			"  Version: 100\n  Indexes: " + indexes + "\n  Weights: " + weights + "\n"
			"  Transform: " + matrixLiteral(tm2) + "\n"
			"  TransformLink: " + matrixLiteral(tl2) + "\n }\n";
	};
	// Two identical clusters for the same bone: must merge (same bind matrices).
	b.objects += cluster_text(cluster2, "*3 { a: 0,1,2; }", "*3 { a: 1,1,1; }");
	b.connOO(cluster2, skin2);
	b.connOO(bone, cluster2);
	b.objects += cluster_text(cluster2b, "*3 { a: 0,1,2; }", "*3 { a: 1,1,1; }");
	b.connOO(cluster2b, skin2);
	b.connOO(bone, cluster2b);

	const u64 stack = 20, layer = 21;
	b.addStackLayer(stack, layer);
	(void)geom1;
	return b.finish(stack, layer);
}

int skin_two_bindings_and_unweighted()
{
	FbxSceneBuilder b;
	const u64 bone = 2, mesh1 = 3, mesh2 = 7;
	std::string text = buildSkinFbx(b);
	std::vector<unsigned char> storage;
	ofbx::IScene* scene = loadText(text, storage);
	TASK_CHECK(scene != nullptr);
	TASK_CHECK(scene->getMeshCount() == 2);
	const ofbx::AnimationLayer* al = scene->getAnimationStack(0)->getLayer(0);

	ofbx::GltfResult result;
	ofbx::GltfError skin2_err = ofbx::exportGltf(*scene, *al, 0.0, 1.0, result);
	if (skin2_err != ofbx::GltfError::OK) std::fprintf(stderr, "SKIN2_EXPORT_ERR %d %s\n", (int)skin2_err, result.message ? result.message : "");
	TASK_CHECK(skin2_err == ofbx::GltfError::OK);
	ParsedGlb glb;
	TASK_CHECK(parseGlb(result, glb));
	// Two meshes must not have been merged, and each keeps its own skin.
	TASK_CHECK(glb.data->meshes_count == 2);
	TASK_CHECK(glb.data->skins_count == 2);
	// They share the same bone terminal joint.
	TASK_CHECK(glb.data->skins[0].joints[0] == glb.data->skins[1].joints[0]);
	// But their inverse bind matrices differ.
	TASK_CHECK(glb.data->skins[0].inverse_bind_matrices != glb.data->skins[1].inverse_bind_matrices);
	// Skin B carries a synthetic joint for the unweighted control point.
	TASK_CHECK(glb.data->skins[1].joints_count == 2);

	const ofbx::Object* bone_obj = findObject(scene, bone);
	const ofbx::Mesh* m1 = scene->getMesh(0);
	const ofbx::Mesh* m2 = scene->getMesh(1);

	// Hand computed check for mesh A, control point 0 at p=(0,0,0):
	// geometric G = T(0,1,0); Transform = T(2,3,4); TransformLink = I.
	// expected = boneWorld * T(2,3,4) * G * 0 = boneWorld * (2,4,4).
	// Bone local is T(5,0,0), root identity.
	for (double t : {0.0, 0.5, 1.0})
	{
		std::map<u64, ofbx::DMatrix> cache;
		ofbx::DMatrix bone_world = sourceWorld(bone_obj, al, t, cache);
		ofbx::DVec3 hand_a = xformPoint(bone_world, {2, 4, 4});

		// Source production skin formula.
		const ofbx::Cluster* cl = m1->getSkin()->getCluster(0);
		ofbx::DMatrix geo1 = m1->getGeometricMatrix();
		ofbx::DMatrix ibm1 = dmul(dinverse(cl->getTransformLinkMatrix()),
			dmul(cl->getTransformMatrix(), geo1));
		ofbx::DVec3 src_a = xformPoint(dmul(bone_world, ibm1), {0, 0, 0});
		TASK_CHECK(vecClose(src_a, hand_a));

		// GLB side: evaluate and skin control point 0.
		ParsedGlb sample;
		TASK_CHECK(parseGlb(result, sample));
		applyAnimation(sample.data, (float)t);
		float jw[16];
		nodeWorld(glb.data->skins[0].joints[0], jw);
		float ibm[16];
		cgltf_accessor_read_float(glb.data->skins[0].inverse_bind_matrices, 0, ibm, 16);
		float comb[16];
		mulMat4(jw, ibm, comb);
		ofbx::DVec3 glb_a = {comb[12], comb[13], comb[14]};
		TASK_CHECK(vecClose(glb_a, hand_a));

		// Mesh B unweighted point (control point 3 at (1,1,0)): synthetic
		// joint follows the mesh's complete world pose; G is identity there.
		const ofbx::Object* mesh2_obj = findObject(scene, mesh2);
		ofbx::DMatrix mesh_world = sourceWorld(mesh2_obj, al, t, cache);
		ofbx::DVec3 hand_b = xformPoint(mesh_world, {1, 1, 0});
		float sjw[16];
		nodeWorld(glb.data->skins[1].joints[1], sjw);
		// synthetic joint world == mesh terminal world.
		float mw[16];
		nodeWorld(&sample.data->nodes[result.node_map.at(mesh2)], mw);
		for (int i = 0; i < 16; ++i) TASK_CHECK(std::abs(sjw[i] - mw[i]) < 2e-5);
		ofbx::DVec3 syn_point = {
			sjw[0] * 1 + sjw[4] * 1 + sjw[12],
			sjw[1] * 1 + sjw[5] * 1 + sjw[13],
			sjw[2] * 1 + sjw[6] * 1 + sjw[14]};
		TASK_CHECK(vecClose(syn_point, hand_b));
	}

	// Layout / accessor rules: POSITION is float VEC3 with min/max, joints and
	// weights are VEC4, IBM is float MAT4; all buffer views are 4 byte aligned.
	bool saw_position_minmax = false;
	for (cgltf_size m = 0; m < glb.data->meshes_count; ++m)
	{
		const cgltf_primitive& pr = glb.data->meshes[m].primitives[0];
		TASK_CHECK(pr.type == cgltf_primitive_type_triangles);
		// Non indexed triangle list: vertex count is a multiple of three.
		const cgltf_accessor* pos = nullptr;
		const cgltf_accessor* jts = nullptr;
		const cgltf_accessor* wts = nullptr;
		for (cgltf_size i = 0; i < pr.attributes_count; ++i)
		{
			if (pr.attributes[i].type == cgltf_attribute_type_position) pos = pr.attributes[i].data;
			if (pr.attributes[i].type == cgltf_attribute_type_joints) jts = pr.attributes[i].data;
			if (pr.attributes[i].type == cgltf_attribute_type_weights) wts = pr.attributes[i].data;
		}
		TASK_CHECK(pos && pos->component_type == cgltf_component_type_r_32f && pos->type == cgltf_type_vec3);
		TASK_CHECK(pos->count % 3 == 0);
		TASK_CHECK(pos->has_min && pos->has_max);
		TASK_CHECK(pos->min[0] <= pos->max[0]);
		saw_position_minmax = true;
		TASK_CHECK(jts && jts->type == cgltf_type_vec4);
		TASK_CHECK(wts && wts->component_type == cgltf_component_type_r_32f && wts->type == cgltf_type_vec4);
	}
	TASK_CHECK(saw_position_minmax);
	for (cgltf_size s = 0; s < glb.data->skins_count; ++s)
	{
		cgltf_accessor* ibm = glb.data->skins[s].inverse_bind_matrices;
		TASK_CHECK(ibm && ibm->component_type == cgltf_component_type_r_32f && ibm->type == cgltf_type_mat4);
		TASK_CHECK(ibm->buffer_view->offset % 16 == 0);
	}
	for (cgltf_size v = 0; v < glb.data->buffer_views_count; ++v)
		TASK_CHECK(glb.data->buffer_views[v].offset % 4 == 0
			&& glb.data->buffer_views[v].size % 4 == 0);
	scene->destroy();
	return 0;
}

std::string buildBadClusterFbx()
{
	FbxSceneBuilder b;
	const u64 root = 1, bone = 2, mesh = 3, geom = 4, skin = 5, c1 = 6, c2 = 7;
	b.addNull(root, "Root");
	b.addLimb(bone, "Bone", root, 0, {0, 0, 0});
	b.objects += " Model: " + std::to_string(mesh) + ", \"Model::Mesh\", \"Mesh\" { }\n";
	b.connOO(mesh, root);
	b.objects += " Geometry: " + std::to_string(geom) + ", \"Geometry\", \"Mesh\" {\n"
		"  Vertices: *9 { a: 0,0,0,1,0,0,0,1,0; }\n"
		"  PolygonVertexIndex: *3 { a: 0,1,-3; }\n }\n";
	b.connOO(geom, mesh);
	b.objects += " Deformer: " + std::to_string(skin) + ", \"Skin\", \"Skin\" { }\n";
	b.connOO(skin, geom);
	ofbx::DMatrix id = ofbx::translationMatrix({0, 0, 0});
	ofbx::DMatrix shifted = ofbx::translationMatrix({9, 9, 9});
	auto cluster = [&](u64 cid, const ofbx::DMatrix& tm) {
		return " Deformer: " + std::to_string(cid) + ", \"C\", \"Cluster\" {\n"
			"  Indexes: *1 { a: 0; }\n  Weights: *1 { a: 1; }\n"
			"  Transform: " + matrixLiteral(tm) + "\n"
			"  TransformLink: " + matrixLiteral(id) + "\n }\n";
	};
	b.objects += cluster(c1, id);
	b.connOO(c1, skin);
	b.connOO(bone, c1);
	b.objects += cluster(c2, shifted); // same bone, different bind matrix
	b.connOO(c2, skin);
	b.connOO(bone, c2);
	const u64 stack = 20, layer = 21;
	b.addStackLayer(stack, layer);
	return b.finish(stack, layer);
}

// Triangle geometry, five distinct bones each influencing one shared vertex:
// exceeds the four-positive-influence limit and must be rejected.
std::string buildFiveInfluenceFbx()
{
	FbxSceneBuilder b;
	const u64 root = 1;
	b.addNull(root, "Root");
	for (u64 bone = 2; bone < 7; ++bone)
	{
		b.addLimb(bone, ("Bone" + std::to_string(bone)).c_str(), root, 0, {double(bone), 0, 0});
	}
	const u64 mesh = 20, geom = 21, skin = 22;
	b.objects += " Model: " + std::to_string(mesh) + ", \"Model::Mesh\", \"Mesh\" { }\n";
	b.connOO(mesh, root);
	b.objects += " Geometry: " + std::to_string(geom) + ", \"Geometry\", \"Mesh\" {\n"
		"  Vertices: *9 { a: 0,0,0,1,0,0,0,1,0; }\n"
		"  PolygonVertexIndex: *3 { a: 0,1,-3; }\n }\n";
	b.connOO(geom, mesh);
	b.objects += " Deformer: " + std::to_string(skin) + ", \"Skin\", \"Skin\" { }\n";
	b.connOO(skin, geom);
	ofbx::DMatrix id = ofbx::translationMatrix({0, 0, 0});
	for (u64 bone = 2; bone < 7; ++bone)
	{
		const u64 cluster = 30 + bone;
		b.objects += " Deformer: " + std::to_string(cluster) + ", \"C\", \"Cluster\" {\n"
			"  Indexes: *1 { a: 0; }\n  Weights: *1 { a: 1; }\n"
			"  Transform: " + matrixLiteral(id) + "\n"
			"  TransformLink: " + matrixLiteral(id) + "\n }\n";
		b.connOO(cluster, skin);
		b.connOO(bone, cluster);
	}
	const u64 stack = 40, layer = 41;
	b.addStackLayer(stack, layer);
	return b.finish(stack, layer);
}

// A cluster carrying a negative weight must be rejected.
std::string buildNegativeWeightFbx()
{
	FbxSceneBuilder b;
	const u64 root = 1, bone = 2, mesh = 3, geom = 4, skin = 5, cluster = 6;
	b.addNull(root, "Root");
	b.addLimb(bone, "Bone", root, 0, {0, 0, 0});
	b.objects += " Model: " + std::to_string(mesh) + ", \"Model::Mesh\", \"Mesh\" { }\n";
	b.connOO(mesh, root);
	b.objects += " Geometry: " + std::to_string(geom) + ", \"Geometry\", \"Mesh\" {\n"
		"  Vertices: *9 { a: 0,0,0,1,0,0,0,1,0; }\n"
		"  PolygonVertexIndex: *3 { a: 0,1,-3; }\n }\n";
	b.connOO(geom, mesh);
	b.objects += " Deformer: " + std::to_string(skin) + ", \"Skin\", \"Skin\" { }\n";
	b.connOO(skin, geom);
	ofbx::DMatrix id = ofbx::translationMatrix({0, 0, 0});
	b.objects += " Deformer: " + std::to_string(cluster) + ", \"C\", \"Cluster\" {\n"
		"  Indexes: *1 { a: 0; }\n  Weights: *1 { a: -0.5; }\n"
		"  Transform: " + matrixLiteral(id) + "\n"
		"  TransformLink: " + matrixLiteral(id) + "\n }\n";
	b.connOO(cluster, skin);
	b.connOO(bone, cluster);
	const u64 stack = 20, layer = 21;
	b.addStackLayer(stack, layer);
	return b.finish(stack, layer);
}

// Cluster referencing an out-of-range control point must be rejected.
std::string buildOutOfRangeIndexFbx()
{
	FbxSceneBuilder b;
	const u64 root = 1, bone = 2, mesh = 3, geom = 4, skin = 5, cluster = 6;
	b.addNull(root, "Root");
	b.addLimb(bone, "Bone", root, 0, {0, 0, 0});
	b.objects += " Model: " + std::to_string(mesh) + ", \"Model::Mesh\", \"Mesh\" { }\n";
	b.connOO(mesh, root);
	b.objects += " Geometry: " + std::to_string(geom) + ", \"Geometry\", \"Mesh\" {\n"
		"  Vertices: *9 { a: 0,0,0,1,0,0,0,1,0; }\n"
		"  PolygonVertexIndex: *3 { a: 0,1,-3; }\n }\n";
	b.connOO(geom, mesh);
	b.objects += " Deformer: " + std::to_string(skin) + ", \"Skin\", \"Skin\" { }\n";
	b.connOO(skin, geom);
	ofbx::DMatrix id = ofbx::translationMatrix({0, 0, 0});
	b.objects += " Deformer: " + std::to_string(cluster) + ", \"C\", \"Cluster\" {\n"
		"  Indexes: *1 { a: 99; }\n  Weights: *1 { a: 1; }\n"
		"  Transform: " + matrixLiteral(id) + "\n"
		"  TransformLink: " + matrixLiteral(id) + "\n }\n";
	b.connOO(cluster, skin);
	b.connOO(bone, cluster);
	const u64 stack = 20, layer = 21;
	b.addStackLayer(stack, layer);
	return b.finish(stack, layer);
}

// Cluster with no bone link must be rejected.
std::string buildMissingLinkFbx()
{
	FbxSceneBuilder b;
	const u64 root = 1, mesh = 3, geom = 4, skin = 5, cluster = 6;
	b.addNull(root, "Root");
	b.objects += " Model: " + std::to_string(mesh) + ", \"Model::Mesh\", \"Mesh\" { }\n";
	b.connOO(mesh, root);
	b.objects += " Geometry: " + std::to_string(geom) + ", \"Geometry\", \"Mesh\" {\n"
		"  Vertices: *9 { a: 0,0,0,1,0,0,0,1,0; }\n"
		"  PolygonVertexIndex: *3 { a: 0,1,-3; }\n }\n";
	b.connOO(geom, mesh);
	b.objects += " Deformer: " + std::to_string(skin) + ", \"Skin\", \"Skin\" { }\n";
	b.connOO(skin, geom);
	ofbx::DMatrix id = ofbx::translationMatrix({0, 0, 0});
	// Cluster present but never connected to any bone: link stays null.
	b.objects += " Deformer: " + std::to_string(cluster) + ", \"C\", \"Cluster\" {\n"
		"  Indexes: *1 { a: 0; }\n  Weights: *1 { a: 1; }\n"
		"  Transform: " + matrixLiteral(id) + "\n"
		"  TransformLink: " + matrixLiteral(id) + "\n }\n";
	b.connOO(cluster, skin);
	const u64 stack = 20, layer = 21;
	b.addStackLayer(stack, layer);
	return b.finish(stack, layer);
}

// A singular (zero scale) TransformLink cannot be inverted and must fail.
std::string buildSingularBindingFbx()
{
	FbxSceneBuilder b;
	const u64 root = 1, bone = 2, mesh = 3, geom = 4, skin = 5, cluster = 6;
	b.addNull(root, "Root");
	b.addLimb(bone, "Bone", root, 0, {0, 0, 0});
	b.objects += " Model: " + std::to_string(mesh) + ", \"Model::Mesh\", \"Mesh\" { }\n";
	b.connOO(mesh, root);
	b.objects += " Geometry: " + std::to_string(geom) + ", \"Geometry\", \"Mesh\" {\n"
		"  Vertices: *9 { a: 0,0,0,1,0,0,0,1,0; }\n"
		"  PolygonVertexIndex: *3 { a: 0,1,-3; }\n }\n";
	b.connOO(geom, mesh);
	b.objects += " Deformer: " + std::to_string(skin) + ", \"Skin\", \"Skin\" { }\n";
	b.connOO(skin, geom);
	ofbx::DMatrix id = ofbx::translationMatrix({0, 0, 0});
	ofbx::DMatrix singular{}; // all zeros -> non invertible
	b.objects += " Deformer: " + std::to_string(cluster) + ", \"C\", \"Cluster\" {\n"
		"  Indexes: *1 { a: 0; }\n  Weights: *1 { a: 1; }\n"
		"  Transform: " + matrixLiteral(id) + "\n"
		"  TransformLink: " + matrixLiteral(singular) + "\n }\n";
	b.connOO(cluster, skin);
	b.connOO(bone, cluster);
	const u64 stack = 20, layer = 21;
	b.addStackLayer(stack, layer);
	return b.finish(stack, layer);
}

// A curve whose key times are not strictly increasing must be rejected.
std::string buildUnsortedKeysFbx()
{
	FbxSceneBuilder b;
	const u64 root = 1, limb = 2;
	b.addNull(root, "Root");
	b.addLimb(limb, "Limb", root, 0, {0, 0, 0});
	const u64 cn = b.next_id++;
	b.objects += " AnimationCurveNode: " + std::to_string(cn) + ", \"ACN\", \"\" {\n  Properties70: {\n"
		"   P: \"d|X\", \"Number\", \"\", \"A\",0\n   P: \"d|Y\", \"Number\", \"\", \"A\",0\n"
		"   P: \"d|Z\", \"Number\", \"\", \"A\",0\n  }\n }\n";
	b.connOP(cn, limb, "Lcl Rotation");
	b.layer_curve_nodes.push_back(cn);
	const u64 ac = b.next_id++;
	// Second key time (0.5s) is earlier than the first (1.0s): not increasing.
	b.objects += " AnimationCurve: " + std::to_string(ac) + ", \"AC\", \"\" {\n"
		"  KeyTime: *2 { a: 46186158000,23093079000; }\n"
		"  KeyValueFloat: *2 { a: 0,90; }\n }\n";
	b.connOP(ac, cn, "d|Z");
	const u64 stack = 20, layer = 21;
	b.addStackLayer(stack, layer);
	return b.finish(stack, layer);
}

// Quad (non triangle) geometry must be rejected.
std::string buildQuadFbx()
{
	FbxSceneBuilder b;
	const u64 root = 1;
	b.addNull(root, "Root");
	const u64 mesh = 3, geom = 4;
	b.objects += " Model: " + std::to_string(mesh) + ", \"Model::Mesh\", \"Mesh\" { }\n";
	b.connOO(mesh, root);
	b.objects += " Geometry: " + std::to_string(geom) + ", \"Geometry\", \"Mesh\" {\n"
		"  Vertices: *12 { a: 0,0,0,1,0,0,1,1,0,0,1,0; }\n"
		"  PolygonVertexIndex: *4 { a: 0,1,2,-4; }\n }\n";
	b.connOO(geom, mesh);
	const u64 stack = 20, layer = 21;
	b.addStackLayer(stack, layer);
	return b.finish(stack, layer);
}

std::string buildBlendShapeFbx()
{
	FbxSceneBuilder b;
	const u64 root = 1, mesh = 3, geom = 4, bs = 8, channel = 9, shape = 12;
	b.addNull(root, "Root");
	b.objects += " Model: " + std::to_string(mesh) + ", \"Model::Mesh\", \"Mesh\" { }\n";
	b.connOO(mesh, root);
	b.objects += " Geometry: " + std::to_string(geom) + ", \"Geometry\", \"Mesh\" {\n"
		"  Vertices: *9 { a: 0,0,0,1,0,0,0,1,0; }\n"
		"  PolygonVertexIndex: *3 { a: 0,1,-3; }\n }\n";
	b.connOO(geom, mesh);
	b.objects += " Deformer: " + std::to_string(bs) + ", \"BS\", \"BlendShape\" { }\n";
	b.connOO(bs, geom);
	b.objects += " Deformer: " + std::to_string(channel) + ", \"BSC\", \"BlendShapeChannel\" { }\n";
	b.connOO(channel, bs);
	b.objects += " Geometry: " + std::to_string(shape) + ", \"Shape\", \"Shape\" {\n"
		"  Vertices: *9 { a: 0,0,1,1,0,1,0,1,1; }\n"
		"  Indexes: *3 { a: 0,1,2; }\n }\n";
	b.connOO(shape, channel);
	const u64 stack = 20, layer = 21;
	b.addStackLayer(stack, layer);
	return b.finish(stack, layer);
}

int skin_rejections_keep_previous_output()
{
	// First produce a valid output (reuse the rig-style minimal scene).
	FbxSceneBuilder good;
	const u64 root = 1, limb = 2;
	good.addNull(root, "Root");
	good.addLimb(limb, "Limb", root, 0, {1, 0, 0});
	good.addAnimatedProperty(limb, "Lcl Rotation", {{2, {0, 1}, {0, 90}}});
	const u64 stack = 10, layer = 11;
	good.addStackLayer(stack, layer);
	std::vector<unsigned char> good_storage;
	ofbx::IScene* good_scene = loadText(good.finish(stack, layer), good_storage);
	TASK_CHECK(good_scene);
	ofbx::GltfResult result;
	TASK_CHECK(ofbx::exportGltf(*good_scene, *good_scene->getAnimationStack(0)->getLayer(0),
		0.0, 1.0, result) == ofbx::GltfError::OK);
	const size_t good_size = result.glb.size();
	const auto good_map = result.node_map;

	// Blend shape export must be rejected.
	std::vector<unsigned char> bs_storage;
	ofbx::IScene* bs_scene = loadText(buildBlendShapeFbx(), bs_storage);
	TASK_CHECK(bs_scene);
	ofbx::GltfError bs_err = ofbx::exportGltf(*bs_scene,
		*bs_scene->getAnimationStack(0)->getLayer(0), 0.0, 1.0, result);
	TASK_CHECK(bs_err == ofbx::GltfError::UNSUPPORTED_INPUT);
	TASK_CHECK(result.glb.size() == good_size); // previous output untouched
	TASK_CHECK(result.node_map == good_map);
	TASK_CHECK(result.code == ofbx::GltfError::UNSUPPORTED_INPUT);
	TASK_CHECK(result.message && result.message[0] != '\0');

	// Conflicting duplicate clusters must be rejected as invalid skin.
	std::vector<unsigned char> bad_storage;
	ofbx::IScene* bad_scene = loadText(buildBadClusterFbx(), bad_storage);
	TASK_CHECK(bad_scene);
	ofbx::GltfError bad_err = ofbx::exportGltf(*bad_scene,
		*bad_scene->getAnimationStack(0)->getLayer(0), 0.0, 1.0, result);
	TASK_CHECK(bad_err == ofbx::GltfError::INVALID_SKIN);
	TASK_CHECK(result.glb.size() == good_size); // still untouched
	TASK_CHECK(result.node_map == good_map);

	// Five distinct positive influences on one vertex must be rejected.
	std::vector<unsigned char> five_storage;
	ofbx::IScene* five_scene = loadText(buildFiveInfluenceFbx(), five_storage);
	TASK_CHECK(five_scene);
	ofbx::GltfError five_err = ofbx::exportGltf(*five_scene,
		*five_scene->getAnimationStack(0)->getLayer(0), 0.0, 1.0, result);
	TASK_CHECK(five_err == ofbx::GltfError::INVALID_SKIN);
	TASK_CHECK(result.glb.size() == good_size);

	// A negative weight must be rejected.
	std::vector<unsigned char> neg_storage;
	ofbx::IScene* neg_scene = loadText(buildNegativeWeightFbx(), neg_storage);
	TASK_CHECK(neg_scene);
	ofbx::GltfError neg_err = ofbx::exportGltf(*neg_scene,
		*neg_scene->getAnimationStack(0)->getLayer(0), 0.0, 1.0, result);
	TASK_CHECK(neg_err == ofbx::GltfError::INVALID_SKIN);
	TASK_CHECK(result.glb.size() == good_size);

	// An out-of-range cluster vertex must be rejected.
	std::vector<unsigned char> oor_storage;
	ofbx::IScene* oor_scene = loadText(buildOutOfRangeIndexFbx(), oor_storage);
	TASK_CHECK(oor_scene);
	ofbx::GltfError oor_err = ofbx::exportGltf(*oor_scene,
		*oor_scene->getAnimationStack(0)->getLayer(0), 0.0, 1.0, result);
	TASK_CHECK(oor_err == ofbx::GltfError::OUT_OF_RANGE);
	TASK_CHECK(result.glb.size() == good_size);

	// A cluster with no bone link must be rejected.
	std::vector<unsigned char> ml_storage;
	ofbx::IScene* ml_scene = loadText(buildMissingLinkFbx(), ml_storage);
	TASK_CHECK(ml_scene);
	ofbx::GltfError ml_err = ofbx::exportGltf(*ml_scene,
		*ml_scene->getAnimationStack(0)->getLayer(0), 0.0, 1.0, result);
	TASK_CHECK(ml_err == ofbx::GltfError::INVALID_SKIN);
	TASK_CHECK(result.glb.size() == good_size);

	// A singular TransformLink must be rejected rather than producing NaNs.
	std::vector<unsigned char> sg_storage;
	ofbx::IScene* sg_scene = loadText(buildSingularBindingFbx(), sg_storage);
	TASK_CHECK(sg_scene);
	ofbx::GltfError sg_err = ofbx::exportGltf(*sg_scene,
		*sg_scene->getAnimationStack(0)->getLayer(0), 0.0, 1.0, result);
	TASK_CHECK(sg_err == ofbx::GltfError::INVALID_SKIN);
	TASK_CHECK(result.glb.size() == good_size);

	// Non triangle geometry must be rejected.
	std::vector<unsigned char> quad_storage;
	ofbx::IScene* quad_scene = loadText(buildQuadFbx(), quad_storage);
	TASK_CHECK(quad_scene);
	ofbx::GltfError quad_err = ofbx::exportGltf(*quad_scene,
		*quad_scene->getAnimationStack(0)->getLayer(0), 0.0, 1.0, result);
	TASK_CHECK(quad_err == ofbx::GltfError::UNSUPPORTED_INPUT);
	TASK_CHECK(result.glb.size() == good_size);

	// Non strictly increasing key times must be rejected.
	std::vector<unsigned char> us_storage;
	ofbx::IScene* us_scene = loadText(buildUnsortedKeysFbx(), us_storage);
	TASK_CHECK(us_scene);
	ofbx::GltfError us_err = ofbx::exportGltf(*us_scene,
		*us_scene->getAnimationStack(0)->getLayer(0), 0.0, 1.0, result);
	TASK_CHECK(us_err == ofbx::GltfError::UNSORTED_KEYS);
	TASK_CHECK(result.glb.size() == good_size);

	// Invalid interval arguments fail without touching output.
	TASK_CHECK(ofbx::exportGltf(*good_scene, *good_scene->getAnimationStack(0)->getLayer(0),
		1.0, 1.0, result) == ofbx::GltfError::INVALID_ARGUMENT);
	TASK_CHECK(ofbx::exportGltf(*good_scene, *good_scene->getAnimationStack(0)->getLayer(0),
		-0.1, 1.0, result) == ofbx::GltfError::INVALID_ARGUMENT);
	TASK_CHECK(result.glb.size() == good_size);

	good_scene->destroy();
	bs_scene->destroy();
	bad_scene->destroy();
	five_scene->destroy();
	neg_scene->destroy();
	oor_scene->destroy();
	ml_scene->destroy();
	sg_scene->destroy();
	quad_scene->destroy();
	us_scene->destroy();
	return 0;
}

} // namespace

// ===========================================================================
// Exported case table (no main: task_support/driver.c drives it).
// ===========================================================================

extern const task_case task_cases[] = {
	{"factor", "pivot_factor_chain_matches_eval_local", factor_pivots},
	{"factor", "static_defaults_and_glb_layout", factor_layout_and_defaults},
	{"motion", "desynced_euler_all_six_orders", motion_desynced_six_orders},
	{"motion", "pivoted_multi_turn_negative_and_loops", motion_multiturn_pivot},
	{"motion", "nonuniform_scaled_parent_chain", motion_scaled_parent},
	{"skin", "two_meshes_shared_bone_distinct_bindings", skin_two_bindings_and_unweighted},
	{"skin", "unweighted_synthetic_and_rejections_keep_output", skin_rejections_keep_previous_output},
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
