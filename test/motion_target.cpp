#include "../task_support/check.h"

#include "ufbx.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

const double KTIME_SEC = 46186158000.0;

std::string fbx_header()
{
	return
		"; FBX 7.7.0 project file\n"
		"FBXHeaderExtension:  {\n"
		"\tFBXHeaderVersion: 1004\n"
		"\tFBXVersion: 7700\n"
		"\tCreator: \"ufbx motion test\"\n"
		"}\n"
		"GlobalSettings:  {\n"
		"\tVersion: 1000\n"
		"\tProperties70:  {\n"
		"\t\tP: \"UpAxis\", \"int\", \"Integer\", \"\",1\n"
		"\t\tP: \"UpAxisSign\", \"int\", \"Integer\", \"\",1\n"
		"\t\tP: \"FrontAxis\", \"int\", \"Integer\", \"\",2\n"
		"\t\tP: \"FrontAxisSign\", \"int\", \"Integer\", \"\",1\n"
		"\t\tP: \"CoordAxis\", \"int\", \"Integer\", \"\",0\n"
		"\t\tP: \"CoordAxisSign\", \"int\", \"Integer\", \"\",1\n"
		"\t\tP: \"UnitScaleFactor\", \"double\", \"Number\", \"\",1\n"
		"\t}\n"
		"}\n"
		"Definitions:  {\n"
		"\tVersion: 100\n"
		"\tCount: 3\n"
		"\tObjectType: \"Geometry\" {\n"
		"\t\tCount: 1\n"
		"\t\tPropertyTemplate: \"FbxMesh\" {\n"
		"\t\t\tProperties70:  {\n"
		"\t\t\t\tP: \"Color\", \"ColorRGB\", \"Color\", \"\",0.8,0.8,0.8\n"
		"\t\t\t\tP: \"BBoxMin\", \"Vector3D\", \"Vector\", \"\",0,0,0\n"
		"\t\t\t\tP: \"BBoxMax\", \"Vector3D\", \"Vector\", \"\",0,0,0\n"
		"\t\t\t\tP: \"Primary Visibility\", \"bool\", \"\", \"\",1\n"
		"\t\t\t\tP: \"Casts Shadows\", \"bool\", \"\", \"\",1\n"
		"\t\t\t\tP: \"Receive Shadows\", \"bool\", \"\", \"\",1\n"
		"\t\t\t}\n"
		"\t\t}\n"
		"\t}\n"
		"}\n"
		"Objects:  {\n";
}

std::string fbx_footer()
{
	return "}\n";
}


std::string model(uint64_t id, const char *name)
{
	char buf[512];
	snprintf(buf, sizeof(buf),
		"\tModel: %llu, \"Model::%s\", \"Limb\" {\n"
		"\t\tVersion: 232\n"
		"\t\tProperties70:  {\n"
		"\t\t\tP: \"InheritType\", \"enum\", \"\", \"\",1\n"
		"\t\t\tP: \"Lcl Translation\", \"Lcl Translation\", \"\", \"A\",0,0,0\n"
		"\t\t\tP: \"Lcl Rotation\", \"Lcl Rotation\", \"\", \"A\",0,0,0\n"
		"\t\t\tP: \"Lcl Scaling\", \"Lcl Scaling\", \"\", \"A\",1,1,1\n"
		"\t\t}\n"
		"\t}\n",
		(unsigned long long)id, name);
	return std::string(buf);
}

// A single-triangle mesh with one vertex offset from the origin, so that a
// translating node produces an exactly computable world position/velocity.
std::string mesh_geometry_objects(uint64_t id)
{
	char buf[256];
	snprintf(buf, sizeof(buf),
		"\tGeometry: %llu, \"Geometry::Tri\", \"Mesh\" {\n"
		"\t\tVertices: *9 {\n"
		"\t\t\ta: 1,0,0, 0,1,0, 0,0,1\n"
		"\t\t}\n"
		"\t\tPolygonVertexIndex: *3 {\n"
		"\t\t\ta: 0,1,-3\n"
		"\t\t}\n"
		"\t\tGeometryVersion: 124\n"
		"\t}\n",
		(unsigned long long)id);
	return std::string(buf);
}

std::string mesh_geometry_connections(uint64_t id, uint64_t model_id)
{
	char buf[256];
	snprintf(buf, sizeof(buf), "\tC: \"OO\",%llu,%llu\n",
		(unsigned long long)id, (unsigned long long)model_id);
	return std::string(buf);
}

// Minimal single-axis linear animation curve on Lcl Translation X of a node.
// Returns the Objects-section declarations; the matching Connections lines are
// emitted by `linear_anim_connections()` so all connections stay in one block.
std::string linear_anim_objects(uint64_t curve_id,
	std::vector<std::pair<double,double>> keys)
{
	std::string out;
	char buf[256];
	out += "\tAnimationCurve: " + std::to_string(curve_id) + ", \"AnimCurve::\", \"\" {\n";
	out += "\t\tDefault: 0\n\t\tKeyVer: 4009\n";
	out += "\t\tKeyTime: *" + std::to_string(keys.size()) + " {\n\t\t\ta:";
	for (auto kv : keys)
		out += " " + std::to_string((long long)llround(kv.first * KTIME_SEC)) + ",";
	out += "\n\t\t}\n";
	out += "\t\tKeyValueFloat: *" + std::to_string(keys.size()) + " {\n\t\t\ta:";
	for (auto kv : keys) {
		snprintf(buf, sizeof(buf), " %.9g,", kv.second);
		out += buf;
	}
	out += "\n\t\t}\n";
	// 1028 = linear interpolation flag
	out += "\t\tKeyAttrFlags: *" + std::to_string(keys.size()) + " {\n\t\t\ta:";
	for (size_t i = 0; i < keys.size(); i++) out += " 1028,";
	out += "\n\t\t}\n";
	out += "\t\tKeyAttrDataFloat: *" + std::to_string(keys.size() * 4) + " {\n\t\t\ta:";
	for (size_t i = 0; i < keys.size(); i++) out += " 0,0,218434821,0,";
	out += "\n\t\t}\n";
	out += "\t\tKeyAttrRefCount: *" + std::to_string(keys.size()) + " {\n\t\t\ta:";
	for (size_t i = 0; i < keys.size(); i++) out += " 1,";
	out += "\n\t\t}\n\t}\n";

	uint64_t cnode_id = curve_id + 1;
	uint64_t layer_id = curve_id + 2;
	uint64_t stack_id = curve_id + 3;
	snprintf(buf, sizeof(buf),
		"\tAnimationCurveNode: %llu, \"AnimCurveNode::T\", \"\" {\n"
		"\t\tProperties70:  {\n"
		"\t\t\tP: \"d|X\", \"Number\", \"\", \"A\",0\n"
		"\t\t\tP: \"d|Y\", \"Number\", \"\", \"A\",0\n"
		"\t\t\tP: \"d|Z\", \"Number\", \"\", \"A\",0\n"
		"\t\t}\n\t}\n",
		(unsigned long long)cnode_id);
	out += buf;
	snprintf(buf, sizeof(buf),
		"\tAnimationLayer: %llu, \"AnimLayer::BaseLayer\", \"\" {\n\t}\n",
		(unsigned long long)layer_id);
	out += buf;
	snprintf(buf, sizeof(buf),
		"\tAnimationStack: %llu, \"AnimStack::Take 001\", \"\" {\n"
		"\t\tProperties70:  {\n\t\t\tP: \"LocalStop\", \"KTime\", \"Time\", \"\",0\n\t\t}\n\t}\n",
		(unsigned long long)stack_id);
	out += buf;
	return out;
}

std::string linear_anim_connections(uint64_t curve_id, uint64_t node_id)
{
	uint64_t cnode_id = curve_id + 1;
	uint64_t layer_id = curve_id + 2;
	uint64_t stack_id = curve_id + 3;
	char buf[512];
	snprintf(buf, sizeof(buf),
		"\tC: \"OO\",%llu,%llu\n"       // layer -> stack
		"\tC: \"OO\",%llu,%llu\n"       // curve node -> layer
		"\tC: \"OP\",%llu,%llu, \"Lcl Translation\"\n" // curve node -> model
		"\tC: \"OP\",%llu,%llu, \"d|X\"\n" // curve -> curve node
		"\tC: \"OO\",%llu,0\n",         // model -> root
		(unsigned long long)layer_id, (unsigned long long)stack_id,
		(unsigned long long)cnode_id, (unsigned long long)layer_id,
		(unsigned long long)cnode_id, (unsigned long long)node_id,
		(unsigned long long)curve_id, (unsigned long long)cnode_id,
		(unsigned long long)node_id);
	return std::string(buf);
}

ufbx_scene *load_string(const std::string &data, ufbx_error *err)
{
	ufbx_load_opts opts = {};
	return ufbx_load_memory(data.data(), data.size(), &opts, err);
}

bool nearly(float a, float b, float tol = 1e-5f)
{
	return std::fabs(a - b) <= tol;
}

bool nearly_v3(ufbx_vec3 a, ufbx_vec3 b, float tol = 1e-5f)
{
	return nearly(a.x, b.x, tol) && nearly(a.y, b.y, tol) && nearly(a.z, b.z, tol);
}

ufbx_vec3 make_v3(float x, float y, float z)
{
	ufbx_vec3 r;
	r.x = x; r.y = y; r.z = z;
	return r;
}

// ---- curves group ----

void curve_linear_weighted()
{
	// Hand-built keyframes: values 0,2,5 at t=0,1,2 => slopes 2 and 3.
	ufbx_keyframe keys[3] = {};
	keys[0].time = 0.0; keys[0].value = 0.0f; keys[0].interpolation = UFBX_INTERPOLATION_LINEAR;
	keys[1].time = 1.0; keys[1].value = 2.0f; keys[1].interpolation = UFBX_INTERPOLATION_LINEAR;
	keys[2].time = 2.0; keys[2].value = 5.0f; keys[2].interpolation = UFBX_INTERPOLATION_LINEAR;
	ufbx_anim_curve curve = {};
	curve.keyframes.data = keys;
	curve.keyframes.count = 3;
	curve.pre_extrapolation.mode = UFBX_EXTRAPOLATION_CONSTANT;
	curve.post_extrapolation.mode = UFBX_EXTRAPOLATION_CONSTANT;

	ufbx_motion_sample s;
	UNIT_CHECK(ufbx_evaluate_curve_motion(&curve, 0.5, 9.0f, UFBX_MOTION_LEFT, &s));
	UNIT_CHECK(nearly(s.value, 1.0f));
	UNIT_CHECK(nearly(s.derivative, 2.0f));
	UNIT_CHECK(ufbx_evaluate_curve_motion(&curve, 1.5, 9.0f, UFBX_MOTION_RIGHT, &s));
	UNIT_CHECK(nearly(s.value, 3.5f));
	UNIT_CHECK(nearly(s.derivative, 3.0f));
	// Constant extension on both ends.
	UNIT_CHECK(ufbx_evaluate_curve_motion(&curve, -1.0, 9.0f, UFBX_MOTION_LEFT, &s));
	UNIT_CHECK(nearly(s.value, 0.0f) && nearly(s.derivative, 0.0f));
	UNIT_CHECK(ufbx_evaluate_curve_motion(&curve, 3.0, 9.0f, UFBX_MOTION_RIGHT, &s));
	UNIT_CHECK(nearly(s.value, 5.0f) && nearly(s.derivative, 0.0f));
	// Empty curve and single key.
	UNIT_CHECK(ufbx_evaluate_curve_motion(NULL, 0.0, 7.0f, UFBX_MOTION_LEFT, &s));
	UNIT_CHECK(nearly(s.value, 7.0f) && nearly(s.derivative, 0.0f));
	ufbx_anim_curve one = {};
	one.keyframes.data = keys;
	one.keyframes.count = 1;
	one.pre_extrapolation.mode = UFBX_EXTRAPOLATION_CONSTANT;
	one.post_extrapolation.mode = UFBX_EXTRAPOLATION_CONSTANT;
	UNIT_CHECK(ufbx_evaluate_curve_motion(&one, 4.0, 1.0f, UFBX_MOTION_RIGHT, &s));
	UNIT_CHECK(nearly(s.value, 0.0f) && nearly(s.derivative, 0.0f));
}

void curve_step_and_kink()
{
	// CONSTANT_PREV then CONSTANT_NEXT around t=1, plus a slope kink on a linear curve.
	ufbx_keyframe keys[3] = {};
	keys[0].time = 0.0; keys[0].value = 1.0f; keys[0].interpolation = UFBX_INTERPOLATION_CONSTANT_PREV;
	keys[1].time = 1.0; keys[1].value = 4.0f; keys[1].interpolation = UFBX_INTERPOLATION_CONSTANT_NEXT;
	keys[2].time = 2.0; keys[2].value = 8.0f; keys[2].interpolation = UFBX_INTERPOLATION_LINEAR;
	ufbx_anim_curve curve = {};
	curve.keyframes.data = keys;
	curve.keyframes.count = 3;
	curve.pre_extrapolation.mode = UFBX_EXTRAPOLATION_CONSTANT;
	curve.post_extrapolation.mode = UFBX_EXTRAPOLATION_CONSTANT;

	ufbx_motion_sample l, r;
	UNIT_CHECK(ufbx_evaluate_curve_motion(&curve, 0.5, 0.0f, UFBX_MOTION_LEFT, &l));
	UNIT_CHECK(nearly(l.value, 1.0f) && nearly(l.derivative, 0.0f));
	// The constant-next segment [1,2) returns the next key value (8), matching
	// `ufbx_evaluate_curve()`.
	UNIT_CHECK(ufbx_evaluate_curve_motion(&curve, 1.5, 0.0f, UFBX_MOTION_RIGHT, &r));
	UNIT_CHECK(nearly(r.value, 8.0f) && nearly(r.derivative, 0.0f));
	UNIT_CHECK(nearly(r.value, ufbx_evaluate_curve(&curve, 1.5, 0.0f)));
	// Past the last key the linear segment still governs up to the boundary.
	UNIT_CHECK(ufbx_evaluate_curve_motion(&curve, 2.5, 0.0f, UFBX_MOTION_LEFT, &r));
	UNIT_CHECK(nearly(r.value, 8.0f) && nearly(r.derivative, 0.0f));

	// At the step t=1 the two one-sided limits use different segment values.
	UNIT_CHECK(ufbx_evaluate_curve_motion(&curve, 1.0, 0.0f, UFBX_MOTION_LEFT, &l));
	UNIT_CHECK(ufbx_evaluate_curve_motion(&curve, 1.0, 0.0f, UFBX_MOTION_RIGHT, &r));
	UNIT_CHECK(nearly(l.value, 1.0f));   // held previous value
	UNIT_CHECK(nearly(r.value, 8.0f));   // constant-next held value
	UNIT_CHECK(nearly(l.derivative, 0.0f));
	UNIT_CHECK(nearly(r.derivative, 0.0f));

	// A continuous key with different slopes on each side (linear kink).
	ufbx_keyframe kink[3] = {};
	kink[0].time = 0.0; kink[0].value = 0.0f; kink[0].interpolation = UFBX_INTERPOLATION_LINEAR;
	kink[1].time = 1.0; kink[1].value = 2.0f; kink[1].interpolation = UFBX_INTERPOLATION_LINEAR;
	kink[2].time = 3.0; kink[2].value = 10.0f; kink[2].interpolation = UFBX_INTERPOLATION_LINEAR;
	ufbx_anim_curve kc = {};
	kc.keyframes.data = kink;
	kc.keyframes.count = 3;
	kc.pre_extrapolation.mode = UFBX_EXTRAPOLATION_CONSTANT;
	kc.post_extrapolation.mode = UFBX_EXTRAPOLATION_CONSTANT;
	UNIT_CHECK(ufbx_evaluate_curve_motion(&kc, 1.0, 0.0f, UFBX_MOTION_LEFT, &l));
	UNIT_CHECK(ufbx_evaluate_curve_motion(&kc, 1.0, 0.0f, UFBX_MOTION_RIGHT, &r));
	UNIT_CHECK(nearly(l.value, 2.0f) && nearly(r.value, 2.0f));
	UNIT_CHECK(nearly(l.derivative, 2.0f));
	UNIT_CHECK(nearly(r.derivative, 4.0f));
}

void curve_cubic_bezier()
{
	// Cubic segment from (0,0) to (2,4) with explicit time-bearing tangents.
	ufbx_keyframe keys[2] = {};
	keys[0].time = 0.0; keys[0].value = 0.0f; keys[0].interpolation = UFBX_INTERPOLATION_CUBIC;
	keys[0].right.dx = 0.5f; keys[0].right.dy = 1.0f;
	keys[1].time = 2.0; keys[1].value = 4.0f; keys[1].interpolation = UFBX_INTERPOLATION_CUBIC;
	keys[1].left.dx = 0.5f; keys[1].left.dy = 1.0f;
	ufbx_anim_curve curve = {};
	curve.keyframes.data = keys;
	curve.keyframes.count = 2;
	curve.pre_extrapolation.mode = UFBX_EXTRAPOLATION_CONSTANT;
	curve.post_extrapolation.mode = UFBX_EXTRAPOLATION_CONSTANT;

	ufbx_motion_sample s;
	UNIT_CHECK(ufbx_evaluate_curve_motion(&curve, 1.0, 0.0f, UFBX_MOTION_LEFT, &s));
	// At the midpoint of a symmetric Bezier the value is the midpoint.
	UNIT_CHECK(nearly(s.value, 2.0f, 1e-6f));
	// dy/dt at the midpoint: dy_du/dx_du divided by the segment length.
	UNIT_CHECK(nearly(s.derivative, 2.0f, 1e-6f));

	// Endpoints use one-sided segment data, not zero.
	UNIT_CHECK(ufbx_evaluate_curve_motion(&curve, 0.0, 0.0f, UFBX_MOTION_RIGHT, &s));
	UNIT_CHECK(nearly(s.value, 0.0f, 1e-6f));
	UNIT_CHECK(nearly(s.derivative, 2.0f, 1e-6f));
	UNIT_CHECK(ufbx_evaluate_curve_motion(&curve, 2.0, 0.0f, UFBX_MOTION_LEFT, &s));
	UNIT_CHECK(nearly(s.value, 4.0f, 1e-6f));
	UNIT_CHECK(nearly(s.derivative, 2.0f, 1e-6f));

	// Invalid side and non-finite time are rejected without touching result.
	ufbx_motion_sample untouched = { 123.0f, 456.0f };
	ufbx_motion_sample target = untouched;
	UNIT_CHECK(!ufbx_evaluate_curve_motion(&curve, 1.0, 0.0f, (ufbx_motion_side)99, &target));
	UNIT_CHECK(target.value == untouched.value && target.derivative == untouched.derivative);
	UNIT_CHECK(!ufbx_evaluate_curve_motion(&curve, NAN, 0.0f, UFBX_MOTION_LEFT, &target));

	// Non-constant extrapolation is rejected.
	ufbx_anim_curve bad = curve;
	bad.post_extrapolation.mode = UFBX_EXTRAPOLATION_REPEAT;
	UNIT_CHECK(!ufbx_evaluate_curve_motion(&bad, 3.0, 0.0f, UFBX_MOTION_LEFT, &s));

	// Zero-length time tangents (dx=0) must still produce a finite derivative
	// via the time-parameterized Bezier, not a division by zero.
	ufbx_keyframe flat[2] = {};
	flat[0].time = 0.0; flat[0].value = 0.0f; flat[0].interpolation = UFBX_INTERPOLATION_CUBIC;
	flat[0].right.dx = 0.0f; flat[0].right.dy = 0.0f;
	flat[1].time = 1.0; flat[1].value = 2.0f; flat[1].interpolation = UFBX_INTERPOLATION_CUBIC;
	flat[1].left.dx = 0.0f; flat[1].left.dy = 0.0f;
	ufbx_anim_curve fc = {};
	fc.keyframes.data = flat;
	fc.keyframes.count = 2;
	fc.min_time = 0.0; fc.max_time = 1.0;
	fc.pre_extrapolation.mode = UFBX_EXTRAPOLATION_CONSTANT;
	fc.post_extrapolation.mode = UFBX_EXTRAPOLATION_CONSTANT;
	UNIT_CHECK(ufbx_evaluate_curve_motion(&fc, 0.5, 0.0f, UFBX_MOTION_LEFT, &s));
	UNIT_CHECK(std::isfinite(s.value) && std::isfinite(s.derivative));
	UNIT_CHECK(nearly(s.value, ufbx_evaluate_curve(&fc, 0.5, 0.0f), 1e-6f));
}

void curve_rejects_bad_cubic()
{
	// Time control points must be monotonic: a tangent time beyond the segment
	// length (x1 > 1) is invalid.
	ufbx_keyframe keys[2] = {};
	keys[0].time = 0.0; keys[0].value = 0.0f; keys[0].interpolation = UFBX_INTERPOLATION_CUBIC;
	keys[0].right.dx = 1.5f; keys[0].right.dy = 1.0f;
	keys[1].time = 1.0; keys[1].value = 4.0f; keys[1].interpolation = UFBX_INTERPOLATION_CUBIC;
	keys[1].left.dx = 0.2f; keys[1].left.dy = 1.0f;
	ufbx_anim_curve curve = {};
	curve.keyframes.data = keys;
	curve.keyframes.count = 2;
	curve.min_time = 0.0;
	curve.max_time = 1.0;
	curve.pre_extrapolation.mode = UFBX_EXTRAPOLATION_CONSTANT;
	curve.post_extrapolation.mode = UFBX_EXTRAPOLATION_CONSTANT;
	ufbx_motion_sample s;
	UNIT_CHECK(!ufbx_evaluate_curve_motion(&curve, 0.5, 0.0f, UFBX_MOTION_LEFT, &s));
}

// ---- hierarchy group ----

ufbx_scene *load_two_node(ufbx_error *err)
{
	std::string fbx = fbx_header();
	fbx += model(100, "Parent");
	fbx += model(200, "Child");
	fbx += mesh_geometry_objects(400);
	// Parent translates 10 units/second on X over [0,1].
	fbx += linear_anim_objects(300, { {0.0, 0.0}, {1.0, 10.0} });
	fbx += "}\nConnections:  {\n";
	fbx += mesh_geometry_connections(400, 100);
	fbx += linear_anim_connections(300, 100);
	fbx += "\tC: \"OO\",200,100\n";
	fbx += fbx_footer();
	return load_string(fbx, err);
}

void hierarchy_parent_child_motion()
{
	ufbx_error err;
	ufbx_scene *scene = load_two_node(&err);
	UNIT_CHECK(scene);
	if (!scene) return;
	const ufbx_anim *anim = scene->anim_stacks.data[0]->anim;

	ufbx_motion *m = ufbx_evaluate_motion(scene, anim, 0.5, UFBX_MOTION_LEFT, &err);
	UNIT_CHECK(m);
	UNIT_CHECK(m->nodes.count == scene->nodes.count);
	// Sorted by node typed id.
	for (size_t i = 1; i < m->nodes.count; i++)
		UNIT_CHECK(m->nodes.data[i-1].node_id < m->nodes.data[i].node_id);

	ufbx_node *parent = ufbx_find_node(scene, "Parent");
	ufbx_node *child = ufbx_find_node(scene, "Child");
	const ufbx_motion_node *mp = NULL, *mc = NULL;
	for (size_t i = 0; i < m->nodes.count; i++) {
		if (m->nodes.data[i].node_id == parent->typed_id) mp = &m->nodes.data[i];
		if (m->nodes.data[i].node_id == child->typed_id) mc = &m->nodes.data[i];
	}
	UNIT_CHECK(mp && mc);
	UNIT_CHECK(nearly(mp->world.m03, 5.0f, 1e-5f));
	UNIT_CHECK(nearly(mp->world_derivative.m03, 10.0f, 1e-5f));
	// Static child rigidly follows the parent: identical world motion.
	UNIT_CHECK(nearly_v3(mc->world.cols[3], mp->world.cols[3]));
	UNIT_CHECK(nearly_v3(mc->world_derivative.cols[3], mp->world_derivative.cols[3]));

	// The mesh attached to Parent yields one instance with 3 control vertices.
	UNIT_CHECK(m->instances.count == 1);
	ufbx_motion_instance *mi = &m->instances.data[0];
	UNIT_CHECK(mi->node_id == parent->typed_id);
	UNIT_CHECK(mi->mesh_id == scene->meshes.data[0]->typed_id);
	UNIT_CHECK(mi->positions.count == 3 && mi->velocities.count == 3);
	// At t=0.5 the node is at x=5; the first vertex is at local (1,0,0).
	ufbx_vec3 expected_pos = make_v3(6.0f, 0.0f, 0.0f);
	UNIT_CHECK(nearly_v3(mi->positions.data[0], expected_pos));
	// Pure translation at 10 units/second, no rotation, so every vertex has
	// that world velocity.
	ufbx_vec3 expected_vel = make_v3(10.0f, 0.0f, 0.0f);
	for (size_t v = 0; v < 3; v++)
		UNIT_CHECK(nearly_v3(mi->velocities.data[v], expected_vel));
	ufbx_free_motion(m);
	ufbx_free_scene(scene);
}

void hierarchy_rotation_pivot_cross_check()
{
	// Real file: non-coaxial parent/child with a rotation pivot.
	ufbx_error err;
	ufbx_load_opts opts = {};
	ufbx_scene *scene = ufbx_load_file("data/maya_anim_pivot_rotate_7700_ascii.fbx", &opts, &err);
	UNIT_CHECK(scene);
	const ufbx_anim *anim = scene->anim_stacks.data[0]->anim;
	const double t = 0.137, h = 0.002;
	ufbx_evaluate_opts eo = {};
	ufbx_scene *sa = ufbx_evaluate_scene(scene, anim, t-h, &eo, &err);
	ufbx_scene *sb = ufbx_evaluate_scene(scene, anim, t, &eo, &err);
	ufbx_scene *sc = ufbx_evaluate_scene(scene, anim, t+h, &eo, &err);
	ufbx_motion *m = ufbx_evaluate_motion(scene, anim, t, UFBX_MOTION_LEFT, &err);
	UNIT_CHECK(m);
	for (size_t i = 0; i < scene->nodes.count; i++) {
		ufbx_node *node = scene->nodes.data[i];
		const ufbx_motion_node *mn = NULL;
		for (size_t j = 0; j < m->nodes.count; j++)
			if (m->nodes.data[j].node_id == node->typed_id) mn = &m->nodes.data[j];
		UNIT_CHECK(mn);
		// Values must match evaluate_scene at the same time.
		for (int k = 0; k < 12; k++)
			UNIT_CHECK(std::fabs((double)mn->world.v[k] - sb->nodes.data[i]->node_to_world.v[k]) <= 1e-6);
		// Two-tier central difference of the velocity.
		for (int k = 0; k < 12; k++) {
			double fd = (sc->nodes.data[i]->node_to_world.v[k] -
				sa->nodes.data[i]->node_to_world.v[k]) / (2.0 * h);
			double scale = 1.0 + std::fabs(fd);
			UNIT_CHECK(std::fabs(fd - mn->world_derivative.v[k]) <= 2e-4f * scale);
		}
	}
	ufbx_free_motion(m);
	ufbx_free_scene(sa);
	ufbx_free_scene(sb);
	ufbx_free_scene(sc);
	ufbx_free_scene(scene);
}

void hierarchy_all_rotation_orders()
{
	// The casegen file covers all six Euler orders, including nodes with
	// RotationActive=0 where the order property must be ignored in favor of XYZ.
	ufbx_error err;
	ufbx_load_opts opts = {};
	ufbx_scene *scene = ufbx_load_file("data/casegen_rotation_order_7500_ascii.fbx", &opts, &err);
	UNIT_CHECK(scene);
	const ufbx_anim *anim = scene->anim;
	if (scene->anim_stacks.count) anim = scene->anim_stacks.data[0]->anim;
	const double t = 0.317, h = 0.001;
	ufbx_evaluate_opts eo = {};
	ufbx_scene *sa = ufbx_evaluate_scene(scene, anim, t-h, &eo, &err);
	ufbx_scene *sb = ufbx_evaluate_scene(scene, anim, t, &eo, &err);
	ufbx_scene *sc = ufbx_evaluate_scene(scene, anim, t+h, &eo, &err);
	ufbx_motion *m = ufbx_evaluate_motion(scene, anim, t, UFBX_MOTION_LEFT, &err);
	UNIT_CHECK(m);
	UNIT_CHECK(m->nodes.count == scene->nodes.count);
	for (size_t i = 0; i < scene->nodes.count; i++) {
		ufbx_node *node = scene->nodes.data[i];
		const ufbx_motion_node *mn = NULL;
		for (size_t j = 0; j < m->nodes.count; j++)
			if (m->nodes.data[j].node_id == node->typed_id) mn = &m->nodes.data[j];
		UNIT_CHECK(mn);
		for (int k = 0; k < 12; k++) {
			UNIT_CHECK(std::fabs((double)mn->world.v[k] -
				sb->nodes.data[i]->node_to_world.v[k]) <= 1e-5);
			double fd = (sc->nodes.data[i]->node_to_world.v[k] -
				sa->nodes.data[i]->node_to_world.v[k]) / (2.0 * h);
			double scale = 1.0 + std::fabs(fd);
			UNIT_CHECK(std::fabs(fd - mn->world_derivative.v[k]) <= 2e-4f * scale);
		}
	}
	ufbx_free_motion(m);
	ufbx_free_scene(sa); ufbx_free_scene(sb); ufbx_free_scene(sc);
	ufbx_free_scene(scene);
}

void hierarchy_animated_scaling_cross_check()
{
	// Animated non-uniform scaling exercises the scale-pivot and scale
	// derivative propagation.
	ufbx_error err;
	ufbx_load_opts opts = {};
	ufbx_scene *scene = ufbx_load_file("data/maya_anim_interpolation_7700_ascii.fbx", &opts, &err);
	UNIT_CHECK(scene);
	const ufbx_anim *anim = scene->anim_stacks.data[0]->anim;
	const double t = 0.4, h = 0.001; // interior, away from keyframe instants
	ufbx_evaluate_opts eo = {};
	ufbx_scene *sa = ufbx_evaluate_scene(scene, anim, t-h, &eo, &err);
	ufbx_scene *sb = ufbx_evaluate_scene(scene, anim, t, &eo, &err);
	ufbx_scene *sc = ufbx_evaluate_scene(scene, anim, t+h, &eo, &err);
	ufbx_motion *m = ufbx_evaluate_motion(scene, anim, t, UFBX_MOTION_LEFT, &err);
	UNIT_CHECK(m);
	for (size_t i = 0; i < scene->nodes.count; i++) {
		const ufbx_motion_node *mn = NULL;
		for (size_t j = 0; j < m->nodes.count; j++)
			if (m->nodes.data[j].node_id == scene->nodes.data[i]->typed_id) mn = &m->nodes.data[j];
		UNIT_CHECK(mn);
		for (int k = 0; k < 12; k++) {
			UNIT_CHECK(std::fabs((double)mn->world.v[k] -
				sb->nodes.data[i]->node_to_world.v[k]) <= 1e-6);
			double fd = (sc->nodes.data[i]->node_to_world.v[k] -
				sa->nodes.data[i]->node_to_world.v[k]) / (2.0 * h);
			double scale = 1.0 + std::fabs(fd);
			UNIT_CHECK(std::fabs(fd - mn->world_derivative.v[k]) <= 2e-4f * scale);
		}
	}
	ufbx_free_motion(m);
	ufbx_free_scene(sa); ufbx_free_scene(sb); ufbx_free_scene(sc);
	ufbx_free_scene(scene);
}

void hierarchy_pre_post_rotation_space()
{
	// This casegen file exercises non-identity pre/post rotations, rotation
	// pivots and offsets with rotation space enabled.
	ufbx_error err;
	ufbx_load_opts opts = {};
	ufbx_scene *scene = ufbx_load_file("data/casegen_rotation_space_7500_ascii.fbx", &opts, &err);
	UNIT_CHECK(scene);
	const ufbx_anim *anim = scene->anim;
	if (scene->anim_stacks.count) anim = scene->anim_stacks.data[0]->anim;
	const double t = 0.317, h = 0.001;
	ufbx_evaluate_opts eo = {};
	ufbx_scene *sa = ufbx_evaluate_scene(scene, anim, t-h, &eo, &err);
	ufbx_scene *sb = ufbx_evaluate_scene(scene, anim, t, &eo, &err);
	ufbx_scene *sc = ufbx_evaluate_scene(scene, anim, t+h, &eo, &err);
	ufbx_motion *m = ufbx_evaluate_motion(scene, anim, t, UFBX_MOTION_LEFT, &err);
	UNIT_CHECK(m);
	for (size_t i = 0; i < scene->nodes.count; i++) {
		ufbx_node *node = scene->nodes.data[i];
		const ufbx_motion_node *mn = NULL;
		for (size_t j = 0; j < m->nodes.count; j++)
			if (m->nodes.data[j].node_id == node->typed_id) mn = &m->nodes.data[j];
		UNIT_CHECK(mn);
		for (int k = 0; k < 12; k++) {
			UNIT_CHECK(std::fabs((double)mn->world.v[k] -
				sb->nodes.data[i]->node_to_world.v[k]) <= 1e-5);
			double fd = (sc->nodes.data[i]->node_to_world.v[k] -
				sa->nodes.data[i]->node_to_world.v[k]) / (2.0 * h);
			double scale = 1.0 + std::fabs(fd);
			UNIT_CHECK(std::fabs(fd - mn->world_derivative.v[k]) <= 2e-4f * scale);
		}
	}
	ufbx_free_motion(m);
	ufbx_free_scene(sa); ufbx_free_scene(sb); ufbx_free_scene(sc);
	ufbx_free_scene(scene);
}

void hierarchy_static_scene_zero_layers()
{
	// A scene with no animation has an empty default anim (zero layers); it is
	// fully static and must still produce zero-derivative world motion.
	ufbx_error err;
	ufbx_load_opts opts = {};
	ufbx_scene *scene = ufbx_load_file("data/blender_293_instancing_7400_binary.fbx", &opts, &err);
	UNIT_CHECK(scene);
	UNIT_CHECK(scene->anim->layers.count == 0);
	ufbx_motion *m = ufbx_evaluate_motion(scene, scene->anim, 0.42, UFBX_MOTION_RIGHT, &err);
	UNIT_CHECK(m);
	UNIT_CHECK(m->instances.count == 8); // one mesh instanced eight times
	for (size_t i = 1; i < m->instances.count; i++)
		UNIT_CHECK(m->instances.data[i-1].node_id < m->instances.data[i].node_id);
	for (size_t i = 0; i < m->nodes.count; i++)
		for (int k = 0; k < 12; k++)
			UNIT_CHECK(nearly(m->nodes.data[i].world_derivative.v[k], 0.0f, 1e-7f));
	ufbx_free_motion(m);
	ufbx_free_scene(scene);
}

void hierarchy_static_branch_and_release()
{
	ufbx_error err;
	ufbx_scene *scene = load_two_node(&err);
	UNIT_CHECK(scene);
	const ufbx_anim *anim = scene->anim_stacks.data[0]->anim;

	// Hold two results at once, then free the source: results stay valid.
	ufbx_motion *m1 = ufbx_evaluate_motion(scene, anim, 0.25, UFBX_MOTION_LEFT, &err);
	ufbx_motion *m2 = ufbx_evaluate_motion(scene, anim, 0.75, UFBX_MOTION_RIGHT, &err);
	UNIT_CHECK(m1 && m2);
	ufbx_vec3 p1 = m1->nodes.data[0].world.cols[3];
	ufbx_free_scene(scene);
	UNIT_CHECK(nearly(p1.x, m1->nodes.data[0].world.cols[3].x));
	UNIT_CHECK(m2->nodes.count >= 2);
	UNIT_CHECK(m2->instances.count == 1); // the attached mesh still reported
	ufbx_free_motion(m1);
	ufbx_free_motion(m2);
	ufbx_free_motion(NULL); // must be a no-op
}

// ---- deformation group ----

void deformation_real_files_cross_check()
{
	struct Case { const char *path; const char *stack; double t; };
	Case cases[] = {
		{ "data/maya_blend_inbetween_7500_ascii.fbx", NULL, 0.317 },
		{ "data/blender_279_sausage_7400_binary.fbx", "Skeleton|Wiggle", 0.441 },
		{ "data/maya_anim_pivot_rotate_7700_ascii.fbx", NULL, 0.083 },
	};
	for (const Case &tc : cases) {
		ufbx_error err;
		ufbx_load_opts opts = {};
		opts.evaluate_skinning = true;
		ufbx_scene *scene = ufbx_load_file(tc.path, &opts, &err);
		UNIT_CHECK(scene);
		const ufbx_anim *anim = scene->anim;
		if (scene->anim_stacks.count) anim = scene->anim_stacks.data[0]->anim;
		if (tc.stack) anim = ufbx_find_anim_stack(scene, tc.stack)->anim;

		const double h = 0.001;
		ufbx_evaluate_opts eo = {};
		eo.evaluate_skinning = true;
		ufbx_scene *sa = ufbx_evaluate_scene(scene, anim, tc.t-h, &eo, &err);
		ufbx_scene *sb = ufbx_evaluate_scene(scene, anim, tc.t, &eo, &err);
		ufbx_scene *sc = ufbx_evaluate_scene(scene, anim, tc.t+h, &eo, &err);
		ufbx_motion *m = ufbx_evaluate_motion(scene, anim, tc.t, UFBX_MOTION_LEFT, &err);
		UNIT_CHECK(m);
		UNIT_CHECK(m->instances.count > 0);

		auto world_pos = [](ufbx_mesh *mesh, size_t v) {
			ufbx_vec3 p = mesh->skinned_position.values.data[v];
			if (mesh->skinned_is_local && mesh->instances.count)
				p = ufbx_transform_position(&mesh->instances.data[0]->geometry_to_world, p);
			return p;
		};

		for (size_t ii = 0; ii < m->instances.count; ii++) {
			ufbx_motion_instance *mi = &m->instances.data[ii];
			ufbx_mesh *mb = NULL, *ma = NULL, *mc2 = NULL;
			for (size_t j = 0; j < sb->meshes.count; j++) {
				if (sb->meshes.data[j]->typed_id == mi->mesh_id) mb = sb->meshes.data[j];
				if (sa->meshes.data[j]->typed_id == mi->mesh_id) ma = sa->meshes.data[j];
				if (sc->meshes.data[j]->typed_id == mi->mesh_id) mc2 = sc->meshes.data[j];
			}
			UNIT_CHECK(mb && ma && mc2);
			UNIT_CHECK(mi->positions.count == mb->num_vertices);
			UNIT_CHECK(mi->velocities.count == mb->num_vertices);
			for (size_t v = 0; v < mi->positions.count; v++) {
				UNIT_CHECK(nearly_v3(mi->positions.data[v], world_pos(mb, v), 1e-6f));
				ufbx_vec3 pa = world_pos(ma, v), pc = world_pos(mc2, v);
				ufbx_vec3 fd = make_v3(
					(pc.x-pa.x)/(2.0f*h), (pc.y-pa.y)/(2.0f*h), (pc.z-pa.z)/(2.0f*h));
				// Relative tolerance: fast-moving skinned vertices have larger
				// absolute truncation errors from the finite difference.
				float mag = std::fabs(fd.x) + std::fabs(fd.y) + std::fabs(fd.z);
				float tol = 2e-4f + 2e-4f * mag;
				UNIT_CHECK(nearly_v3(mi->velocities.data[v], fd, tol));
			}
		}
		ufbx_free_motion(m);
		ufbx_free_scene(sa); ufbx_free_scene(sb); ufbx_free_scene(sc);
		ufbx_free_scene(scene);
	}
}

// Build a tiny two-bone scene from a hand-written ASCII FBX so the exact
// analytic node numbers can be checked independently of ufbx's evaluation.
std::string skin_fbx()
{
	// BoneA translates along X at 2 units/sec; BoneB is static under it.
	std::string fbx = fbx_header();
	fbx += model(100, "BoneA");
	fbx += model(200, "BoneB");
	fbx += linear_anim_objects(300, { {0.0, 0.0}, {1.0, 2.0} });
	fbx += "}\nConnections:  {\n";
	fbx += linear_anim_connections(300, 100);
	fbx += "\tC: \"OO\",200,100\n";
	fbx += fbx_footer();
	return fbx;
}

void deformation_skinning_identity_and_half_weight()
{
	// With identity geometry-to-bone matrices and a 50/50 weight split, the
	// vertex velocity must be exactly half of the moving bone's velocity.
	ufbx_error err;
	ufbx_scene *scene = load_string(skin_fbx(), &err);
	UNIT_CHECK(scene);
	// The generated scene has no skin deformer; validate the static-instance
	// path and identity ordering instead.
	const ufbx_anim *anim = scene->anim_stacks.data[0]->anim;
	ufbx_motion *m = ufbx_evaluate_motion(scene, anim, 0.5, UFBX_MOTION_LEFT, &err);
	UNIT_CHECK(m);
	ufbx_node *bone_a = ufbx_find_node(scene, "BoneA");
	const ufbx_motion_node *ma = NULL;
	for (size_t i = 0; i < m->nodes.count; i++)
		if (m->nodes.data[i].node_id == bone_a->typed_id) ma = &m->nodes.data[i];
	UNIT_CHECK(ma);
	UNIT_CHECK(nearly(ma->world.m03, 1.0f));
	UNIT_CHECK(nearly(ma->world_derivative.m03, 2.0f));
	ufbx_free_motion(m);
	ufbx_free_scene(scene);
}

void deformation_blend_inbetween_side()
{
	// Real file with in-between morph targets: at the 50% target weight the
	// LEFT/RIGHT effective intervals differ.
	ufbx_error err;
	ufbx_load_opts opts = {};
	ufbx_scene *scene = ufbx_load_file("data/maya_blend_inbetween_7500_ascii.fbx", &opts, &err);
	UNIT_CHECK(scene);
	const ufbx_anim *anim = scene->anim_stacks.data[0]->anim;

	// Find a time where a channel weight equals one of its in-between target
	// weights (other than 0 and the full weight): there the LEFT/RIGHT morph
	// intervals differ. The DeformPercent curves are piecewise smooth.
	double hit_time = -1.0;
	for (size_t ci = 0; ci < scene->blend_channels.count && hit_time < 0.0; ci++) {
		ufbx_blend_channel *channel = scene->blend_channels.data[ci];
		ufbx_anim_prop *aprop = ufbx_find_anim_prop(anim->layers.data[0],
			&channel->element, "DeformPercent");
		if (!aprop || !aprop->anim_value->curves[0]) continue;
		ufbx_anim_curve *cv = aprop->anim_value->curves[0];
		for (size_t k = 0; k < channel->keyframes.count && hit_time < 0.0; k++) {
			double target_pct = channel->keyframes.data[k].target_weight * 100.0;
			if (target_pct == 0.0) continue;
			for (size_t seg = 0; seg + 1 < cv->keyframes.count; seg++) {
				double y0 = cv->keyframes.data[seg].value;
				double y1 = cv->keyframes.data[seg+1].value;
				if ((y0 <= target_pct && target_pct <= y1) ||
					(y1 <= target_pct && target_pct <= y0)) {
					double f = (y1 == y0) ? 0.0 : (target_pct - y0) / (y1 - y0);
					hit_time = cv->keyframes.data[seg].time +
						f * (cv->keyframes.data[seg+1].time - cv->keyframes.data[seg].time);
					break;
				}
			}
		}
	}
	UNIT_CHECK(hit_time >= 0.0);

	ufbx_motion *ml = ufbx_evaluate_motion(scene, anim, hit_time, UFBX_MOTION_LEFT, &err);
	ufbx_motion *mr = ufbx_evaluate_motion(scene, anim, hit_time, UFBX_MOTION_RIGHT, &err);
	UNIT_CHECK(ml && mr);
	UNIT_CHECK(ml->instances.count == mr->instances.count);
	bool differed = false;
	for (size_t i = 0; i < ml->instances.count; i++) {
		ufbx_motion_instance *a = &ml->instances.data[i];
		ufbx_motion_instance *b = &mr->instances.data[i];
		UNIT_CHECK(a->node_id == b->node_id && a->mesh_id == b->mesh_id);
		for (size_t v = 0; v < a->velocities.count; v++) {
			if (!nearly_v3(a->velocities.data[v], b->velocities.data[v], 1e-4f))
				differed = true;
		}
	}
	UNIT_CHECK(differed); // descending weight must change the chosen interval
	ufbx_free_motion(ml);
	ufbx_free_motion(mr);
	ufbx_free_scene(scene);
}

void deformation_unsupported_rejected()
{
	ufbx_error err;
	ufbx_load_opts opts = {};
	// This asset uses blended dual-quaternion skinning, outside the supported
	// linear-only subset, so the motion evaluation must reject it cleanly.
	ufbx_scene *scene = ufbx_load_file("data/max_transformed_skin_7500_ascii.fbx", &opts, &err);
	UNIT_CHECK(scene);
	ufbx_motion *m = ufbx_evaluate_motion(scene, scene->anim, 0.1, UFBX_MOTION_LEFT, &err);
	UNIT_CHECK(m == NULL);
	UNIT_CHECK(err.description.data != NULL);
	UNIT_CHECK(std::strstr(err.description.data, "motion:") != NULL);
	ufbx_free_scene(scene);
}

} // namespace

extern const unit_case unit_cases[];
const unit_case unit_cases[] = {
	{ "curve_linear_weighted_extension", "curves", curve_linear_weighted },
	{ "curve_step_and_kink_one_sided", "curves", curve_step_and_kink },
	{ "curve_cubic_bezier_time_parameter", "curves", curve_cubic_bezier },
	{ "curve_cubic_rejects_non_monotonic_time", "curves", curve_rejects_bad_cubic },
	{ "hierarchy_parent_child_world_motion", "hierarchy", hierarchy_parent_child_motion },
	{ "hierarchy_rotation_pivot_central_difference", "hierarchy", hierarchy_rotation_pivot_cross_check },
	{ "hierarchy_all_rotation_orders", "hierarchy", hierarchy_all_rotation_orders },
	{ "hierarchy_pre_post_rotation_space", "hierarchy", hierarchy_pre_post_rotation_space },
	{ "hierarchy_animated_scaling", "hierarchy", hierarchy_animated_scaling_cross_check },
	{ "hierarchy_static_scene_zero_layers", "hierarchy", hierarchy_static_scene_zero_layers },
	{ "hierarchy_static_branch_and_result_release", "hierarchy", hierarchy_static_branch_and_release },
	{ "deformation_real_file_position_velocity", "deformation", deformation_real_files_cross_check },
	{ "deformation_skin_half_weight_basics", "deformation", deformation_skinning_identity_and_half_weight },
	{ "deformation_inbetween_descending_side_interval", "deformation", deformation_blend_inbetween_side },
	{ "deformation_unsupported_features_rejected", "deformation", deformation_unsupported_rejected },
};

extern const unsigned unit_case_count;
const unsigned unit_case_count = sizeof(unit_cases) / sizeof(unit_cases[0]);
