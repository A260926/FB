// Target tests for Parent-Child constraint evaluation.
//
// The cases below generate small ASCII FBX 7.4 files entirely in memory, load
// them through the real ufbx loader and evaluate them with
// `ufbx_evaluate_opts.evaluate_constraints`. Node poses and final skinned
// vertices are cross-checked against an independent column-vector matrix
// reference implemented in this file.
#include "task_support/check.h"

#include "ufbx.h"

#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

const double PI = 3.14159265358979323846;
// The production build uses double-precision ufbx_real and must meet the
// required 1e-7 bound. A float build is allowed the extra rounding that
// accumulates through the transform chains.
const double EPS = sizeof(ufbx_real) == sizeof(float) ? 2e-6 : 1e-7;
const int64_t KTIME_SECOND = 46186158000;

// -- Independent column-major 4x4 matrix reference ---------------------------
// Stored as m[column * 4 + row], points are column vectors.

struct Mat4 {
	double m[16];
};

Mat4 ref_identity()
{
	Mat4 r;
	for (int i = 0; i < 16; i++) r.m[i] = 0.0;
	r.m[0] = r.m[5] = r.m[10] = r.m[15] = 1.0;
	return r;
}

Mat4 ref_mul(const Mat4 &a, const Mat4 &b)
{
	Mat4 r;
	for (int c = 0; c < 4; c++) {
		for (int row = 0; row < 4; row++) {
			double s = 0.0;
			for (int k = 0; k < 4; k++) s += a.m[k*4+row] * b.m[c*4+k];
			r.m[c*4+row] = s;
		}
	}
	return r;
}

Mat4 ref_translation(double x, double y, double z)
{
	Mat4 r = ref_identity();
	r.m[12] = x; r.m[13] = y; r.m[14] = z;
	return r;
}

Mat4 ref_rot_axis(char axis, double deg)
{
	double a = deg * PI / 180.0, c = std::cos(a), s = std::sin(a);
	Mat4 r = ref_identity();
	if (axis == 'x') {
		r.m[5] = c; r.m[6] = s; r.m[9] = -s; r.m[10] = c;
	} else if (axis == 'y') {
		r.m[0] = c; r.m[10] = c; r.m[2] = -s; r.m[8] = s;
	} else {
		r.m[0] = c; r.m[1] = s; r.m[4] = -s; r.m[5] = c;
	}
	return r;
}

// FBX rotation order names describe the order axes are *applied*, so the
// matrix multiplication order is the reverse.
Mat4 ref_rot_euler(const ufbx_vec3 &r, ufbx_rotation_order order)
{
	Mat4 rx = ref_rot_axis('x', r.x), ry = ref_rot_axis('y', r.y), rz = ref_rot_axis('z', r.z);
	switch (order) {
	case UFBX_ROTATION_ORDER_XYZ: return ref_mul(rz, ref_mul(ry, rx));
	case UFBX_ROTATION_ORDER_XZY: return ref_mul(ry, ref_mul(rz, rx));
	case UFBX_ROTATION_ORDER_YZX: return ref_mul(rx, ref_mul(rz, ry));
	case UFBX_ROTATION_ORDER_YXZ: return ref_mul(rz, ref_mul(rx, ry));
	case UFBX_ROTATION_ORDER_ZXY: return ref_mul(ry, ref_mul(rx, rz));
	case UFBX_ROTATION_ORDER_ZYX: return ref_mul(rx, ref_mul(ry, rz));
	default: return rx;
	}
}

struct Quat {
	double x, y, z, w;
};

Quat ref_quat_from_matrix(const Mat4 &m)
{
	double t = m.m[0] + m.m[5] + m.m[10];
	Quat q;
	if (t > 0.0) {
		double s = std::sqrt(t + 1.0) * 2.0;
		q.w = 0.25 * s;
		q.x = (m.m[6] - m.m[9]) / s;
		q.y = (m.m[8] - m.m[2]) / s;
		q.z = (m.m[1] - m.m[4]) / s;
	} else if (m.m[0] > m.m[5] && m.m[0] > m.m[10]) {
		double s = std::sqrt(1.0 + m.m[0] - m.m[5] - m.m[10]) * 2.0;
		q.w = (m.m[6] - m.m[9]) / s;
		q.x = 0.25 * s;
		q.y = (m.m[4] + m.m[1]) / s;
		q.z = (m.m[8] + m.m[2]) / s;
	} else if (m.m[5] > m.m[10]) {
		double s = std::sqrt(1.0 + m.m[5] - m.m[0] - m.m[10]) * 2.0;
		q.w = (m.m[8] - m.m[2]) / s;
		q.x = (m.m[4] + m.m[1]) / s;
		q.y = 0.25 * s;
		q.z = (m.m[9] + m.m[6]) / s;
	} else {
		double s = std::sqrt(1.0 + m.m[10] - m.m[0] - m.m[5]) * 2.0;
		q.w = (m.m[1] - m.m[4]) / s;
		q.x = (m.m[8] + m.m[2]) / s;
		q.y = (m.m[9] + m.m[6]) / s;
		q.z = 0.25 * s;
	}
	return q;
}

Mat4 ref_quat_to_matrix(const Quat &q)
{
	double xx = q.x*q.x, yy = q.y*q.y, zz = q.z*q.z;
	double xy = q.x*q.y, xz = q.x*q.z, yz = q.y*q.z;
	double wx = q.w*q.x, wy = q.w*q.y, wz = q.w*q.z;
	Mat4 r = ref_identity();
	r.m[0] = 1.0 - 2.0*(yy+zz); r.m[1] = 2.0*(xy+wz);       r.m[2] = 2.0*(xz-wy);
	r.m[4] = 2.0*(xy-wz);       r.m[5] = 1.0 - 2.0*(xx+zz); r.m[6] = 2.0*(yz+wx);
	r.m[8] = 2.0*(xz+wy);       r.m[9] = 2.0*(yz-wx);       r.m[10] = 1.0 - 2.0*(xx+yy);
	return r;
}

Quat ref_slerp(Quat a, Quat b, double t)
{
	double dot = a.x*b.x + a.y*b.y + a.z*b.z + a.w*b.w;
	if (dot < 0.0) {
		b.x = -b.x; b.y = -b.y; b.z = -b.z; b.w = -b.w;
		dot = -dot;
	}
	if (dot > 0.9995) {
		Quat r = {
			a.x + (b.x-a.x)*t, a.y + (b.y-a.y)*t,
			a.z + (b.z-a.z)*t, a.w + (b.w-a.w)*t
		};
		double n = std::sqrt(r.x*r.x+r.y*r.y+r.z*r.z+r.w*r.w);
		r.x/=n; r.y/=n; r.z/=n; r.w/=n;
		return r;
	}
	double theta = std::acos(std::fmin(1.0, std::fmax(-1.0, dot)));
	double so = std::sin(theta);
	double fa = std::sin((1.0-t)*theta)/so, fb = std::sin(t*theta)/so;
	return { a.x*fa+b.x*fb, a.y*fa+b.y*fb, a.z*fa+b.z*fb, a.w*fa+b.w*fb };
}

Mat4 ref_invert_rigid(const Mat4 &m)
{
	Mat4 r = ref_identity();
	for (int c = 0; c < 3; c++)
		for (int row = 0; row < 3; row++)
			r.m[c*4+row] = m.m[row*4+c]; // transpose rotation
	for (int row = 0; row < 3; row++) {
		double s = 0.0;
		for (int k = 0; k < 3; k++) s += r.m[k*4+row] * m.m[12+k];
		r.m[12+row] = -s;
	}
	return r;
}

void ref_transform_point(const Mat4 &m, const double v[3], double out[3])
{
	for (int row = 0; row < 3; row++) {
		out[row] = m.m[0*4+row]*v[0] + m.m[1*4+row]*v[1]
			+ m.m[2*4+row]*v[2] + m.m[3*4+row];
	}
}

// FBX local transform for unit scale with rotation pivot / offsets and
// pre/post rotation:
// M = T * Roff * Rp * Rpre * R * Rpost^-1 * Rp^-1
Mat4 ref_local_matrix(const ufbx_vec3 &translation, const ufbx_vec3 &rotation,
	ufbx_rotation_order order, const ufbx_vec3 &pre, const ufbx_vec3 &post,
	const ufbx_vec3 &rot_pivot, const ufbx_vec3 &rot_offset)
{
	Mat4 t = ref_translation(translation.x, translation.y, translation.z);
	Mat4 roff = ref_translation(rot_offset.x, rot_offset.y, rot_offset.z);
	Mat4 rp = ref_translation(rot_pivot.x, rot_pivot.y, rot_pivot.z);
	Mat4 rp_inv = ref_translation(-rot_pivot.x, -rot_pivot.y, -rot_pivot.z);
	Mat4 rpre = ref_rot_euler(pre, UFBX_ROTATION_ORDER_XYZ);
	Mat4 r = ref_rot_euler(rotation, order);
	Mat4 rpost_inv = ref_invert_rigid(ref_rot_euler(post, UFBX_ROTATION_ORDER_XYZ));
	return ref_mul(t, ref_mul(roff, ref_mul(rp, ref_mul(rpre, ref_mul(r, ref_mul(rpost_inv, rp_inv))))));
}

bool near_vec(double a, double b) { return std::fabs(a-b) <= EPS; }

bool check_matrix(const Mat4 &ref, const ufbx_matrix &got, const char *what)
{
	// ufbx_matrix stores four column vectors of three rows.
	for (int c = 0; c < 4; c++) {
		for (int row = 0; row < 3; row++) {
			double g = got.cols[c].v[row];
			double want = ref.m[c*4+row];
			if (!near_vec(g, want)) {
				std::fprintf(stderr, "matrix mismatch %s col=%d row=%d got=%.9f want=%.9f\n",
					what, c, row, g, want);
				return false;
			}
		}
	}
	return true;
}

Mat4 ref_from_ufbx(const ufbx_matrix &g)
{
	Mat4 r = ref_identity();
	for (int c = 0; c < 4; c++)
		for (int row = 0; row < 3; row++)
			r.m[c*4+row] = g.cols[c].v[row];
	return r;
}

// -- ASCII FBX 7.4 generator -------------------------------------------------

struct Key { double time; double value; };

struct FbxBuilder {
	std::vector<std::string> objects;
	std::vector<std::string> connections;
	int next_id = 100;

	int alloc_id() { return next_id++; }

	static std::string fnum(double v)
	{
		char buf[64];
		std::snprintf(buf, sizeof(buf), "%.10g", v);
		return std::string(buf);
	}

	void oo(int src, int dst)
	{
		connections.push_back("C: \"OO\"," + std::to_string(src) + "," + std::to_string(dst));
	}
	void op(int src, int dst, const std::string &prop)
	{
		connections.push_back("C: \"OP\"," + std::to_string(src) + "," + std::to_string(dst) + ",\"" + prop + "\"");
	}

	struct NodeDef {
		int id;
		std::string name;
	};

	NodeDef add_node(const std::string &name, const std::vector<std::string> &extra_props = {},
		const char *type = "Null", int inherit_type = 1)
	{
		int id = alloc_id();
		std::string s = "\tModel: " + std::to_string(id) + ", \"Model::" + name + "\", \"" + type + "\" {\n"
			"\t\tVersion: 232\n\t\tProperties70: {\n"
			"\t\t\tP: \"InheritType\", \"enum\", \"\", \"\"," + std::to_string(inherit_type) + "\n";
		for (const std::string &p : extra_props) s += "\t\t\t" + p + "\n";
		s += "\t\t}\n\t}\n";
		objects.push_back(s);
		oo(id, 0);
		return { id, name };
	}

	void add_curve(int curve_id, const std::vector<Key> &keys)
	{
		std::string c = "\tAnimationCurve: " + std::to_string(curve_id) + ", \"AnimCurve::\", \"\" {\n"
			"\t\tDefault: 0\n\t\tKeyVer: 4009\n";
		c += "\t\tKeyTime: *" + std::to_string(keys.size()) + " {\n\t\t\ta: ";
		for (size_t i = 0; i < keys.size(); i++) {
			if (i) c += ",";
			c += std::to_string((int64_t)(keys[i].time * KTIME_SECOND));
		}
		c += "\n\t\t}\n\t\tKeyValueFloat: *" + std::to_string(keys.size()) + " {\n\t\t\ta: ";
		for (size_t i = 0; i < keys.size(); i++) {
			if (i) c += ",";
			c += fnum(keys[i].value);
		}
		c += "\n\t\t}\n";
		// Flag 4 = linear interpolation, zero tangent attributes.
		c += "\t\tKeyAttrFlags: *" + std::to_string(keys.size()) + " {\n\t\t\ta: ";
		for (size_t i = 0; i < keys.size(); i++) { if (i) c += ","; c += "4"; }
		c += "\n\t\t}\n";
		c += "\t\tKeyAttrDataFloat: *" + std::to_string(keys.size()*4) + " {\n\t\t\ta: ";
		for (size_t i = 0; i < keys.size(); i++) { if (i) c += ","; c += "0,0,0,0"; }
		c += "\n\t\t}\n";
		c += "\t\tKeyAttrRefCount: *" + std::to_string(keys.size()) + " {\n\t\t\ta: ";
		for (size_t i = 0; i < keys.size(); i++) { if (i) c += ","; c += "1"; }
		c += "\n\t\t}\n\t}\n";
		objects.push_back(c);
	}

	// One animated scalar property.
	void animate_scalar(int elem_id, const std::string &prop, const std::vector<Key> &keys)
	{
		int node_id = alloc_id(), curve_id = alloc_id();
		objects.push_back("\tAnimationCurveNode: " + std::to_string(node_id)
			+ ", \"AnimCurveNode::" + prop + "\", \"\" {\n\t\tProperties70: {\n"
			"\t\t\tP: \"d|X\", \"Number\", \"\", \"A\",0\n\t\t}\n\t}\n");
		add_curve(curve_id, keys);
		oo(node_id, 90);
		op(curve_id, node_id, "d|X");
		op(node_id, elem_id, prop);
	}

	// One animated vec3 property; pass nullptr for unused components.
	void animate_vec3(int elem_id, const std::string &prop,
		const std::vector<Key> *kx, const std::vector<Key> *ky, const std::vector<Key> *kz)
	{
		int node_id = alloc_id();
		objects.push_back("\tAnimationCurveNode: " + std::to_string(node_id)
			+ ", \"AnimCurveNode::" + prop + "\", \"\" {\n\t\tProperties70: {\n"
			"\t\t\tP: \"d|X\", \"Number\", \"\", \"A\",0\n"
			"\t\t\tP: \"d|Y\", \"Number\", \"\", \"A\",0\n"
			"\t\t\tP: \"d|Z\", \"Number\", \"\", \"A\",0\n\t\t}\n\t}\n");
		oo(node_id, 90);
		const char *comps[3] = { "d|X", "d|Y", "d|Z" };
		const std::vector<Key> *ks[3] = { kx, ky, kz };
		for (int i = 0; i < 3; i++) {
			if (!ks[i]) continue;
			int curve_id = alloc_id();
			add_curve(curve_id, *ks[i]);
			op(curve_id, node_id, comps[i]);
		}
		op(node_id, elem_id, prop);
	}

	struct ConstraintDef {
		int id;
	};

	// Single-target Parent constraint. Channel flags default to the supported
	// range; callers can override them for rejection tests.
	ConstraintDef add_parent_constraint(const std::string &name, int child_id, int target_id,
		const std::string &target_name, double weight = 100.0, double target_weight = 100.0,
		const ufbx_vec3 *offset_t = nullptr, const ufbx_vec3 *offset_r = nullptr,
		bool active = true, bool partial_axis = false, bool scale_axis = false,
		const ufbx_vec3 *transform_offset_t = nullptr)
	{
		int id = alloc_id();
		ufbx_vec3 ot = offset_t ? *offset_t : ufbx_vec3{ 0, 0, 0 };
		ufbx_vec3 orr = offset_r ? *offset_r : ufbx_vec3{ 0, 0, 0 };
		ufbx_vec3 tot = transform_offset_t ? *transform_offset_t : ufbx_vec3{ 0, 0, 0 };
		std::string s = "\tConstraint: " + std::to_string(id) + ", \"Constraint::" + name + "\", \"Parent-Child\" {\n"
			"\t\tType: \"Parent-Child\"\n\t\tProperties70: {\n"
			"\t\t\tP: \"Constrained object (Child)\", \"object\", \"\", \"\"\n"
			"\t\t\tP: \"Active\", \"bool\", \"\", \"\"," + std::string(active ? "1" : "0") + "\n"
			"\t\t\tP: \"Weight\", \"Number\", \"\", \"A\"," + fnum(weight) + "\n";
		if (partial_axis)
			s += "\t\t\tP: \"AffectTranslationX\", \"bool\", \"\", \"\",0\n";
		if (scale_axis)
			s += "\t\t\tP: \"AffectScaleX\", \"bool\", \"\", \"\",1\n";
		s += "\t\t\tP: \"" + target_name + ".Weight\", \"Number\", \"\", \"A\"," + fnum(target_weight) + "\n"
			"\t\t\tP: \"" + target_name + ".Offset T\", \"Translation\", \"\", \"A\","
			+ fnum(ot.x) + "," + fnum(ot.y) + "," + fnum(ot.z) + "\n"
			"\t\t\tP: \"" + target_name + ".Offset R\", \"Translation\", \"\", \"A\","
			+ fnum(orr.x) + "," + fnum(orr.y) + "," + fnum(orr.z) + "\n"
			"\t\t\tP: \"Translation\", \"Vector\", \"\", \"A\","
			+ fnum(tot.x) + "," + fnum(tot.y) + "," + fnum(tot.z) + "\n"
			"\t\t}\n\t}\n";
		objects.push_back(s);
		op(child_id, id, "Constrained object (Child)");
		op(target_id, id, "Source (Parent)");
		return { id };
	}

	// A non-Parent constraint type to exercise the "other valid type" rejection.
	int add_position_constraint(const std::string &name, int child_id, int target_id)
	{
		int id = alloc_id();
		objects.push_back("\tConstraint: " + std::to_string(id) + ", \"Constraint::" + name
			+ "\", \"Position From Positions\" {\n\t\tType: \"Position From Positions\"\n\t\tProperties70: {\n"
			"\t\t\tP: \"Constrained Object\", \"object\", \"\", \"\"\n"
			"\t\t\tP: \"Position.Weight\", \"Number\", \"\", \"A\",100\n\t\t}\n\t}\n");
		op(child_id, id, "Constrained Object");
		op(target_id, id, "Source (Position)");
		return id;
	}

	void add_second_target(int constraint_id, int target_id, const std::string &target_name)
	{
		op(target_id, constraint_id, "Source (Parent)");
		// Second target weight property; parsed as another target entry.
		(void)target_name;
	}

	struct MeshDef {
		int mesh_id, geom_id, skin_id, cluster_id, mesh_node_id;
	};

	// Single-bone rigid skin. bind_geometry_to_bone and bind_bone_world are
	// column-major 16-entry matrices.
	MeshDef add_skinned_mesh(const std::string &name, int bone_id,
		const std::vector<ufbx_vec3> &verts, const double *geometry_to_bone,
		const double *bind_bone_world, const std::vector<std::string> &mesh_node_props = {},
		int parent_id = -1)
	{
		int mesh_node = add_node(name, mesh_node_props, "Mesh").id;
		if (parent_id >= 0) {
			// Replace the default root parenting added by add_node.
			connections.pop_back();
			oo(mesh_node, parent_id);
		}
		int geom = alloc_id(), skin = alloc_id(), cluster = alloc_id();
		std::string g = "\tGeometry: " + std::to_string(geom) + ", \"Geometry::" + name + "\", \"Mesh\" {\n"
			"\t\tGeometryVersion: 124\n\t\tVertices: *" + std::to_string(verts.size()*3) + " {\n\t\t\ta: ";
		for (size_t i = 0; i < verts.size(); i++) {
			if (i) g += ",";
			g += fnum(verts[i].x) + "," + fnum(verts[i].y) + "," + fnum(verts[i].z);
		}
		int n = (int)verts.size();
		g += "\n\t\t}\n\t\tPolygonVertexIndex: *" + std::to_string(n) + " {\n\t\t\ta: ";
		for (int i = 0; i < n; i++) { if (i) g += ","; g += std::to_string(i == n-1 ? -(i+1) : i); }
		g += "\n\t\t}\n\t}\n";
		objects.push_back(g);
		oo(geom, mesh_node);

		objects.push_back("\tDeformer: " + std::to_string(skin) + ", \"Deformer::Skin\", \"Skin\" {\n"
			"\t\tVersion: 101\n\t\tSkinningType: \"Linear\"\n\t}\n");
		oo(skin, geom);

		std::string c = "\tDeformer: " + std::to_string(cluster) + ", \"SubDeformer::Cluster\", \"Cluster\" {\n"
			"\t\tVersion: 100\n"
			"\t\tIndexes: *" + std::to_string(n) + " {\n\t\t\ta: ";
		for (int i = 0; i < n; i++) { if (i) c += ","; c += std::to_string(i); }
		c += "\n\t\t}\n\t\tWeights: *" + std::to_string(n) + " {\n\t\t\ta: ";
		for (int i = 0; i < n; i++) { if (i) c += ","; c += "1"; }
		c += "\n\t\t}\n\t\tTransform: *16 {\n\t\t\ta: ";
		for (int i = 0; i < 16; i++) { if (i) c += ","; c += fnum(geometry_to_bone[i]); }
		c += "\n\t\t}\n\t\tTransformLink: *16 {\n\t\t\ta: ";
		for (int i = 0; i < 16; i++) { if (i) c += ","; c += fnum(bind_bone_world[i]); }
		c += "\n\t\t}\n\t}\n";
		objects.push_back(c);
		oo(cluster, skin);
		op(bone_id, cluster, "");
		return { mesh_node, geom, skin, cluster, mesh_node };
	}

	std::string build()
	{
		std::string s =
			"; FBX 7.4.0 project file\n"
			"FBXHeaderExtension: {\n\tFBXHeaderVersion: 1003\n\tFBXVersion: 7400\n}\n"
			"GlobalSettings: {\n\tVersion: 1000\n\tProperties70: {\n"
			"\t\tP: \"UpAxis\", \"int\", \"Integer\", \"\",1\n"
			"\t\tP: \"UpAxisSign\", \"int\", \"Integer\", \"\",1\n"
			"\t\tP: \"FrontAxis\", \"int\", \"Integer\", \"\",2\n"
			"\t\tP: \"FrontAxisSign\", \"int\", \"Integer\", \"\",1\n"
			"\t\tP: \"CoordAxis\", \"int\", \"Integer\", \"\",0\n"
			"\t\tP: \"CoordAxisSign\", \"int\", \"Integer\", \"\",1\n"
			"\t\tP: \"UnitScaleFactor\", \"double\", \"Number\", \"\",1\n"
			"\t}\n}\n"
			"Objects: {\n"
			"\tAnimationStack: 80, \"AnimStack::Take 001\", \"\" {\n\t}\n"
			"\tAnimationLayer: 90, \"AnimLayer::BaseLayer\", \"\" {\n\t}\n";
		for (const std::string &o : objects) s += o;
		s += "}\nConnections: {\n\tC: \"OO\",90,80\n";
		for (const std::string &c : connections) s += "\t" + c + "\n";
		s += "}\n";
		return s;
	}
};

ufbx_scene *load_string(const std::string &data, ufbx_error *error,
	ufbx_inherit_mode_handling inherit_handling = UFBX_INHERIT_MODE_HANDLING_HELPER_NODES)
{
	ufbx_load_opts opts = {};
	opts.load_external_files = false;
	opts.inherit_mode_handling = inherit_handling;
	return ufbx_load_memory(data.data(), data.size(), &opts, error);
}

ufbx_scene *evaluate(const ufbx_scene *src, double time, bool constraints, bool skinning, ufbx_error *error)
{
	ufbx_evaluate_opts opts = {};
	opts.evaluate_constraints = constraints;
	opts.evaluate_skinning = skinning;
	return ufbx_evaluate_scene(src, src->anim, time, &opts, error);
}

std::string format(ufbx_error *error)
{
	char buf[1024];
	ufbx_format_error(buf, sizeof(buf), error);
	return std::string(buf);
}

ufbx_node *node(ufbx_scene *s, const char *name) { return ufbx_find_node(s, name); }

// Sample a linearly animated scalar at time t from the same key list used to
// build the file, matching constant extrapolation outside the key range.
double sample_linear(const std::vector<Key> &keys, double t)
{
	if (t <= keys.front().time) return keys.front().value;
	if (t >= keys.back().time) return keys.back().value;
	for (size_t i = 0; i + 1 < keys.size(); i++) {
		if (t >= keys[i].time && t <= keys[i+1].time) {
			double f = (t - keys[i].time) / (keys[i+1].time - keys[i].time);
			return keys[i].value + (keys[i+1].value - keys[i].value) * f;
		}
	}
	return keys.back().value;
}

// Column-major identity matrix in FBX array order.
void identity_16(double out[16])
{
	for (int i = 0; i < 16; i++) out[i] = 0.0;
	out[0] = out[5] = out[10] = out[15] = 1.0;
}

} // namespace

// ===========================================================================
// dependency group
// ===========================================================================

// Cross target chains feeding a real skinned mesh: T -> A (constrained),
// A -> C (plain child), C -> B (constrained), B drives the skin bone.
static int dependency_cross_chain_skin()
{
	FbxBuilder fb;
	auto T = fb.add_node("T");
	auto A = fb.add_node("A");
	auto C = fb.add_node("C", {
		"P: \"Lcl Translation\", \"Lcl Translation\", \"\", \"A\",1,0,0",
	});
	auto B = fb.add_node("B");
	fb.oo(C.id, A.id);
	fb.oo(B.id, 0);

	ufbx_vec3 a_off = { 1, 0, 0 };
	fb.add_parent_constraint("FollowT", A.id, T.id, "T", 100, 100, &a_off);
	fb.add_parent_constraint("FollowC", B.id, C.id, "C");

	// Animate T translation and rotation across the second.
	std::vector<Key> t_tx = { {0,0},{1,4} };
	std::vector<Key> t_rz = { {0,0},{1,90} };
	fb.animate_vec3(T.id, "Lcl Translation", &t_tx, nullptr, nullptr);
	fb.animate_vec3(T.id, "Lcl Rotation", nullptr, nullptr, &t_rz);

	double g2b[16], bind[16];
	identity_16(g2b);
	identity_16(bind);
	// Non-identity geometry->bone: shift the bind space by (0.5, 0, 0).
	g2b[12] = 0.5;
	std::vector<ufbx_vec3> verts = { {0,0,0}, {1,0,0}, {0,1,0} };
	fb.add_skinned_mesh("Mesh", B.id, verts, g2b, bind);

	std::string data = fb.build();
	ufbx_error error = {};
	ufbx_scene *src = load_string(data, &error);
	TASK_CHECK(src != nullptr);

	const double times[3] = { 0.0, 0.25, 1.0 };
	for (double t : times) {
		ufbx_scene *e = evaluate(src, t, true, true, &error);
		if (!e) { std::fprintf(stderr, "%s\n", format(&error).c_str()); TASK_CHECK(false); }

		// Independent expectation.
		double tx = sample_linear({ {0,0},{1,4} }, t);
		double rz = sample_linear({ {0,0},{1,90} }, t);
		Mat4 t_world = ref_mul(ref_translation(tx, 0, 0), ref_rot_axis('z', rz));

		// A: G = identity (root), D = T * offset(1,0,0), weight 1.
		Mat4 off = ref_translation(1, 0, 0);
		Mat4 a_world = ref_mul(t_world, off);

		// C keeps its plain local translation (1,0,0).
		Mat4 c_local = ref_translation(1, 0, 0);
		Mat4 c_world = ref_mul(a_world, c_local);

		// B: G = identity (root), D = C, weight 1.
		Mat4 b_world = c_world;

		TASK_CHECK(check_matrix(a_world, node(e, "A")->node_to_world, "A"));
		TASK_CHECK(check_matrix(c_world, node(e, "C")->node_to_world, "C"));
		TASK_CHECK(check_matrix(b_world, node(e, "B")->node_to_world, "B"));

		// Skinned vertex: bone world * geometry_to_bone applied to each vertex.
		Mat4 deform = ref_mul(b_world, [&]{
			Mat4 m = ref_identity(); m.m[12] = 0.5; return m;
		}());
		ufbx_mesh *mesh = e->meshes.data[0];
		TASK_CHECK(mesh->skinned_position.values.count == verts.size());
		for (size_t i = 0; i < verts.size(); i++) {
			double v[3] = { verts[i].x, verts[i].y, verts[i].z };
			double want[3];
			ref_transform_point(deform, v, want);
			ufbx_vec3 got = mesh->skinned_position.values.data[i];
			if (!near_vec(got.x, want[0]) || !near_vec(got.y, want[1]) || !near_vec(got.z, want[2])) {
				std::fprintf(stderr, "vert %zu t=%.2f got=(%.9f,%.9f,%.9f) want=(%.9f,%.9f,%.9f)\n",
					i, t, got.x, got.y, got.z, want[0], want[1], want[2]);
				TASK_CHECK(false);
			}
		}
		ufbx_free_scene(e);
	}

	// Source scene must be untouched.
	TASK_CHECK(node(src, "A")->node_to_world.m03 == 0.0);
	ufbx_free_scene(src);
	return 0;
}

// A potential constraint cycle that exists only while a weight is positive.
// At weight zero the edge is removed and evaluation succeeds.
static int dependency_zero_weight_removes_cycle()
{
	FbxBuilder fb;
	auto T = fb.add_node("T");
	auto A = fb.add_node("A");
	auto B = fb.add_node("B");
	auto c1 = fb.add_parent_constraint("AtoB", A.id, B.id, "B");
	auto c2 = fb.add_parent_constraint("BtoA", B.id, A.id, "A");
	(void)c1;
	// Drive the second constraint's overall weight: 0 at t=0, 100 at t=1.
	fb.animate_scalar(c2.id, "Weight", { {0,0},{1,100} });
	(void)T;

	std::string data = fb.build();
	ufbx_error error = {};
	ufbx_scene *src = load_string(data, &error);
	TASK_CHECK(src != nullptr);

	// t=0: B's constraint is ignored, no cycle, plain hierarchy survives.
	ufbx_scene *e0 = evaluate(src, 0.0, true, false, &error);
	TASK_CHECK(e0 != nullptr);
	ufbx_free_scene(e0);

	// Repeated query at the same time stays consistent.
	ufbx_scene *e0b = evaluate(src, 0.0, true, false, &error);
	TASK_CHECK(e0b != nullptr);
	TASK_CHECK(node(e0b, "A")->node_to_world.m03 == node(e0b, "B")->node_to_world.m03);
	ufbx_free_scene(e0b);

	// t=1: both edges effective -> hard cycle error.
	ufbx_scene *e1 = evaluate(src, 1.0, true, false, &error);
	TASK_CHECK(e1 == nullptr);
	std::string msg = format(&error);
	TASK_CHECK(msg.find("cycle") != std::string::npos);
	TASK_CHECK(msg.find("AtoB") != std::string::npos || msg.find("BtoA") != std::string::npos);

	ufbx_free_scene(src);
	return 0;
}

// An effective joint constraint cycle is always rejected.
static int dependency_joint_cycle_rejected()
{
	FbxBuilder fb;
	auto A = fb.add_node("A");
	auto B = fb.add_node("B");
	fb.add_parent_constraint("AtoB", A.id, B.id, "B");
	fb.add_parent_constraint("BtoA", B.id, A.id, "A");

	ufbx_error error = {};
	ufbx_scene *src = load_string(fb.build(), &error);
	TASK_CHECK(src != nullptr);
	ufbx_scene *e = evaluate(src, 0.0, true, false, &error);
	TASK_CHECK(e == nullptr);
	std::string msg = format(&error);
	TASK_CHECK(msg.find("cycle") != std::string::npos);
	TASK_CHECK(msg.find("A") != std::string::npos);
	ufbx_free_scene(src);
	return 0;
}

// ===========================================================================
// blend group
// ===========================================================================

// Pivots, pre/post rotation, non-default rotation order and a half weight.
static int blend_pivot_half_weight()
{
	FbxBuilder fb;
	auto P = fb.add_node("P", {
		"P: \"RotationOrder\", \"enum\", \"\", \"\",2", // ZYX
		"P: \"Lcl Rotation\", \"Lcl Rotation\", \"\", \"A\",0,0,30",
	});
	auto X = fb.add_node("X", {
		"P: \"RotationPivot\", \"Vector3D\", \"Vector\", \"\",1,0,0",
		"P: \"RotationOffset\", \"Vector3D\", \"Vector\", \"\",0,2,0",
		"P: \"PreRotation\", \"Vector3D\", \"Vector\", \"\",0,0,45",
		"P: \"PostRotation\", \"Vector3D\", \"Vector\", \"\",0,0,-20",
		"P: \"Lcl Rotation\", \"Lcl Rotation\", \"\", \"A\",0,0,60",
	});
	fb.oo(X.id, P.id);
	auto T = fb.add_node("T", {
		"P: \"Lcl Translation\", \"Lcl Translation\", \"\", \"A\",3,4,0",
	});
	fb.add_parent_constraint("XFollow", X.id, T.id, "T", 50, 100);

	ufbx_error error = {};
	ufbx_scene *src = load_string(fb.build(), &error);
	TASK_CHECK(src != nullptr);
	ufbx_scene *e = evaluate(src, 0.0, true, false, &error);
	if (!e) { std::fprintf(stderr, "%s\n", format(&error).c_str()); TASK_CHECK(false); }

	// Independent matrices.
	Mat4 p_world = ref_rot_euler(ufbx_vec3{ 0,0,30 }, UFBX_ROTATION_ORDER_ZYX);
	Mat4 x_local = ref_local_matrix(
		ufbx_vec3{ 0,0,0 }, ufbx_vec3{ 0,0,60 }, UFBX_ROTATION_ORDER_XYZ,
		ufbx_vec3{ 0,0,45 }, ufbx_vec3{ 0,0,-20 },
		ufbx_vec3{ 1,0,0 }, ufbx_vec3{ 0,2,0 });
	Mat4 g_world = ref_mul(p_world, x_local);
	Mat4 d_world = ref_translation(3, 4, 0);

	Quat qg = ref_quat_from_matrix(g_world);
	Quat qd = ref_quat_from_matrix(d_world);
	Quat qb = ref_slerp(qg, qd, 0.5);
	Mat4 rot = ref_quat_to_matrix(qb);
	Mat4 blended = ref_identity();
	for (int i = 0; i < 16; i++) blended.m[i] = rot.m[i];
	blended.m[12] = g_world.m[12]*0.5 + d_world.m[12]*0.5;
	blended.m[13] = g_world.m[13]*0.5 + d_world.m[13]*0.5;
	blended.m[14] = g_world.m[14]*0.5 + d_world.m[14]*0.5;

	TASK_CHECK(check_matrix(p_world, node(e, "P")->node_to_world, "P"));
	TASK_CHECK(check_matrix(blended, node(e, "X")->node_to_world, "X blend"));

	// local_transform must be the blended world expressed in parent space.
	Mat4 want_local = ref_mul(ref_invert_rigid(p_world), blended);
	TASK_CHECK(check_matrix(want_local, node(e, "X")->node_to_parent, "X local"));
	ufbx_free_scene(e);
	ufbx_free_scene(src);
	return 0;
}

// Animated overall weight and animated target translation/rotation offset.
static int blend_dynamic_weight_and_offset()
{
	FbxBuilder fb;
	auto T = fb.add_node("T", {
		"P: \"Lcl Translation\", \"Lcl Translation\", \"\", \"A\",2,0,0",
	});
	auto A = fb.add_node("A");
	ufbx_vec3 off_r = { 0, 0, 90 };
	auto con = fb.add_parent_constraint("Follow", A.id, T.id, "T", 100, 100, nullptr, &off_r);

	std::vector<Key> weight_keys = { {0,0},{1,100} };
	std::vector<Key> off_tx = { {0,0},{1,4} };
	fb.animate_scalar(con.id, "Weight", weight_keys);
	fb.animate_vec3(con.id, "T.Offset T", &off_tx, nullptr, nullptr);

	ufbx_error error = {};
	ufbx_scene *src = load_string(fb.build(), &error);
	TASK_CHECK(src != nullptr);

	for (double t : { 0.0, 0.3, 0.7, 1.0 }) {
		ufbx_scene *e = evaluate(src, t, true, false, &error);
		if (!e) { std::fprintf(stderr, "%s\n", format(&error).c_str()); TASK_CHECK(false); }

		double w = sample_linear(weight_keys, t) / 100.0;
		double ox = sample_linear(off_tx, t);
		Mat4 t_world = ref_translation(2, 0, 0);
		Mat4 offset = ref_mul(ref_translation(ox, 0, 0), ref_rot_axis('z', 90));
		Mat4 d = ref_mul(t_world, offset);
		Mat4 g = ref_identity();
		Quat qb = ref_slerp(ref_quat_from_matrix(g), ref_quat_from_matrix(d), w);
		Mat4 rot = ref_quat_to_matrix(qb);
		Mat4 want = rot;
		want.m[12] = d.m[12]*w;
		want.m[13] = d.m[13]*w;
		want.m[14] = d.m[14]*w;
		TASK_CHECK(check_matrix(want, node(e, "A")->node_to_world, "dynamic"));
		ufbx_free_scene(e);
	}
	ufbx_free_scene(src);
	return 0;
}

// The SLERP must always take the shortest arc and stay normalized.
static int blend_shortest_arc_slerp()
{
	FbxBuilder fb;
	auto T = fb.add_node("T", {
		"P: \"Lcl Rotation\", \"Lcl Rotation\", \"\", \"A\",0,0,200",
	});
	auto A = fb.add_node("A");
	fb.add_parent_constraint("Follow", A.id, T.id, "T", 50, 100);

	ufbx_error error = {};
	ufbx_scene *src = load_string(fb.build(), &error);
	TASK_CHECK(src != nullptr);
	ufbx_scene *e = evaluate(src, 0.0, true, false, &error);
	TASK_CHECK(e != nullptr);

	Mat4 d = ref_rot_axis('z', 200);
	Quat qb = ref_slerp({ 0,0,0,1 }, ref_quat_from_matrix(d), 0.5);
	Mat4 want = ref_quat_to_matrix(qb);
	ufbx_quat got_q = node(e, "A")->local_transform.rotation;
	double n = got_q.x*got_q.x+got_q.y*got_q.y+got_q.z*got_q.z+got_q.w*got_q.w;
	TASK_CHECK(std::fabs(n-1.0) <= EPS);
	Mat4 got = ref_from_ufbx(node(e, "A")->node_to_world);
	for (int i = 0; i < 12; i++) TASK_CHECK(near_vec(got.m[i], want.m[i]));
	ufbx_free_scene(e);
	ufbx_free_scene(src);
	return 0;
}

// ===========================================================================
// scene group
// ===========================================================================

// With the option off, constraints never affect node transforms.
static int scene_option_off_keeps_hierarchy()
{
	FbxBuilder fb;
	auto T = fb.add_node("T", { "P: \"Lcl Translation\", \"Lcl Translation\", \"\", \"A\",5,0,0" });
	auto A = fb.add_node("A");
	fb.add_parent_constraint("Follow", A.id, T.id, "T");
	(void)T;

	ufbx_error error = {};
	ufbx_scene *src = load_string(fb.build(), &error);
	TASK_CHECK(src != nullptr);

	ufbx_scene *off = evaluate(src, 0.0, false, false, &error);
	TASK_CHECK(off != nullptr);
	// A stays at the origin instead of following T.
	TASK_CHECK(off->constraints.count == 1);
	TASK_CHECK(node(off, "A")->node_to_world.m03 == 0.0);
	TASK_CHECK(node(off, "T")->node_to_world.m03 == 5.0);

	ufbx_scene *plain = ufbx_evaluate_scene(src, src->anim, 0.0, nullptr, &error);
	TASK_CHECK(plain != nullptr);
	TASK_CHECK(check_matrix(ref_from_ufbx(node(plain, "A")->node_to_world),
		node(off, "A")->node_to_world, "off==default"));
	ufbx_free_scene(plain);
	ufbx_free_scene(off);
	ufbx_free_scene(src);
	return 0;
}

// Non-identity bind matrices and a geometry transform on the skinned node.
static int scene_non_identity_bind_and_geometry_transform()
{
	FbxBuilder fb;
	auto B = fb.add_node("B", {
		"P: \"Lcl Translation\", \"Lcl Translation\", \"\", \"A\",1,0,0",
	});
	// Geometry transform on the mesh node itself (unit local scale).
	double g2b[16], bind[16];
	identity_16(g2b);
	identity_16(bind);
	// geometry_to_bone: rotate then translate in bind space.
	Mat4 g2b_ref = ref_mul(ref_translation(0.25, 0, 0), ref_rot_axis('z', 90));
	for (int i = 0; i < 16; i++) g2b[i] = g2b_ref.m[i];
	std::vector<ufbx_vec3> verts = { {1,0,0}, {0,2,0} };
	fb.add_skinned_mesh("Mesh", B.id, verts, g2b, bind, {
		"P: \"GeometricTranslation\", \"Vector3D\", \"Vector\", \"\",0,0,0.5",
		"P: \"GeometricRotation\", \"Vector3D\", \"Vector\", \"\",0,0,90",
	});

	ufbx_error error = {};
	ufbx_scene *src = load_string(fb.build(), &error);
	TASK_CHECK(src != nullptr);
	ufbx_scene *e = evaluate(src, 0.0, true, true, &error);
	if (!e) { std::fprintf(stderr, "%s\n", format(&error).c_str()); TASK_CHECK(false); }

	ufbx_node *bone = node(e, "B");
	ufbx_node *mesh_node = node(e, "Mesh");
	Mat4 bone_world = ref_translation(1, 0, 0);
	TASK_CHECK(check_matrix(bone_world, bone->node_to_world, "bone"));

	// geometry_to_world must include the geometry transform. The mesh node is a
	// separate root child at identity, so only its geometry transform applies.
	Mat4 geo_ref = ref_mul(ref_translation(0, 0, 0.5), ref_rot_axis('z', 90));
	Mat4 expect_geo_world = geo_ref;
	TASK_CHECK(check_matrix(expect_geo_world, mesh_node->geometry_to_world, "geometry_to_world"));

	// The cluster's static geometry_to_bone already folds the geometry
	// transform in during loading; the skinned result uses the final bone.
	ufbx_skin_cluster *cluster = e->skin_clusters.data[0];
	Mat4 cluster_g2b = ref_from_ufbx(cluster->geometry_to_bone);
	Mat4 deform = ref_mul(bone_world, cluster_g2b);
	ufbx_mesh *mesh = e->meshes.data[0];
	for (size_t i = 0; i < verts.size(); i++) {
		double v[3] = { verts[i].x, verts[i].y, verts[i].z };
		double want[3];
		ref_transform_point(deform, v, want);
		ufbx_vec3 got = mesh->skinned_position.values.data[i];
		TASK_CHECK(near_vec(got.x, want[0]) && near_vec(got.y, want[1]) && near_vec(got.z, want[2]));
	}
	ufbx_free_scene(e);
	ufbx_free_scene(src);
	return 0;
}

// Repeat queries, reverse time order, and source-scene immutability.
static int scene_repeat_and_reverse_time()
{
	FbxBuilder fb;
	auto T = fb.add_node("T");
	auto A = fb.add_node("A");
	fb.add_parent_constraint("Follow", A.id, T.id, "T");
	std::vector<Key> t_tx = { {0,0},{1,8} };
	fb.animate_vec3(T.id, "Lcl Translation", &t_tx, nullptr, nullptr);

	ufbx_error error = {};
	ufbx_scene *src = load_string(fb.build(), &error);
	TASK_CHECK(src != nullptr);

	ufbx_vec3 snapshots[4];
	const double order[4] = { 1.0, 0.5, 0.0, 0.5 };
	for (int i = 0; i < 4; i++) {
		ufbx_scene *e = evaluate(src, order[i], true, false, &error);
		TASK_CHECK(e != nullptr);
		snapshots[i] = node(e, "A")->node_to_world.cols[3];
		ufbx_free_scene(e);
	}
	// The two 0.5 queries must match regardless of surrounding order.
	TASK_CHECK(near_vec(snapshots[1].x, snapshots[3].x));
	TASK_CHECK(near_vec(snapshots[1].y, snapshots[3].y));

	ufbx_scene *again = evaluate(src, 0.5, true, false, &error);
	TASK_CHECK(again != nullptr);
	TASK_CHECK(near_vec(node(again, "A")->node_to_world.m03, snapshots[1].x));
	ufbx_free_scene(again);

	// Source scene remains in its bind state.
	TASK_CHECK(node(src, "A")->node_to_world.m03 == 0.0);
	TASK_CHECK(node(src, "T")->node_to_world.m03 == 0.0);
	ufbx_free_scene(src);
	return 0;
}

// Unsupported configurations return NULL with descriptive errors, while
// scenes without any effective constraint keep the ordinary evaluation.
static int scene_rejected_configs_and_limits()
{
	ufbx_error error = {};

	// Non-Parent active constraint type.
	{
		FbxBuilder fb;
		auto T = fb.add_node("T"), A = fb.add_node("A");
		fb.add_position_constraint("Point", A.id, T.id);
		ufbx_scene *src = load_string(fb.build(), &error);
		TASK_CHECK(src != nullptr);
		ufbx_scene *e = evaluate(src, 0, true, false, &error);
		TASK_CHECK(e == nullptr);
		TASK_CHECK(format(&error).find("Parent") != std::string::npos);
		ufbx_free_scene(src);
	}

	// Multiple targets.
	{
		FbxBuilder fb;
		auto T1 = fb.add_node("T1"), T2 = fb.add_node("T2"), A = fb.add_node("A");
		auto con = fb.add_parent_constraint("Multi", A.id, T1.id, "T1");
		fb.add_second_target(con.id, T2.id, "T2");
		ufbx_scene *src = load_string(fb.build(), &error);
		TASK_CHECK(src != nullptr);
		ufbx_scene *e = evaluate(src, 0, true, false, &error);
		TASK_CHECK(e == nullptr);
		TASK_CHECK(format(&error).find("one target") != std::string::npos);
		ufbx_free_scene(src);
	}

	// Partial translation axis.
	{
		FbxBuilder fb;
		auto T = fb.add_node("T"), A = fb.add_node("A");
		fb.add_parent_constraint("Partial", A.id, T.id, "T", 100, 100, nullptr, nullptr, true, true);
		ufbx_scene *src = load_string(fb.build(), &error);
		TASK_CHECK(src != nullptr);
		ufbx_scene *e = evaluate(src, 0, true, false, &error);
		TASK_CHECK(e == nullptr);
		TASK_CHECK(format(&error).find("channel") != std::string::npos);
		ufbx_free_scene(src);
	}

	// Enabled scale channel.
	{
		FbxBuilder fb;
		auto T = fb.add_node("T"), A = fb.add_node("A");
		fb.add_parent_constraint("Scaled", A.id, T.id, "T", 100, 100, nullptr, nullptr, true, false, true);
		ufbx_scene *src = load_string(fb.build(), &error);
		TASK_CHECK(src != nullptr);
		ufbx_scene *e = evaluate(src, 0, true, false, &error);
		TASK_CHECK(e == nullptr);
		TASK_CHECK(format(&error).find("scale") != std::string::npos);
		ufbx_free_scene(src);
	}

	// Non-identity constraint-level transform offset.
	{
		FbxBuilder fb;
		auto T = fb.add_node("T"), A = fb.add_node("A");
		ufbx_vec3 tot = { 1, 2, 3 };
		fb.add_parent_constraint("OwnOffset", A.id, T.id, "T", 100, 100, nullptr, nullptr,
			true, false, false, &tot);
		ufbx_scene *src = load_string(fb.build(), &error);
		TASK_CHECK(src != nullptr);
		TASK_CHECK(src->constraints.data[0]->transform_offset.translation.y == 2.0);
		ufbx_scene *e = evaluate(src, 0, true, false, &error);
		TASK_CHECK(e == nullptr);
		TASK_CHECK(format(&error).find("transform offset") != std::string::npos);
		ufbx_free_scene(src);
	}

	// Non-unit target offset scale (exposed via the "T.Offset S" property).
	{
		FbxBuilder fb;
		auto T = fb.add_node("T"), A = fb.add_node("A");
		fb.add_parent_constraint("OffS", A.id, T.id, "T");
		for (std::string &o : fb.objects) {
			if (o.find("Constraint::OffS") != std::string::npos) {
				size_t pos = o.find("\t\t}\n\t}");
				o.insert(pos, "\t\t\tP: \"T.Offset S\", \"Scaling\", \"\", \"A\",2,2,2\n");
			}
		}
		ufbx_scene *src = load_string(fb.build(), &error);
		TASK_CHECK(src != nullptr);
		ufbx_scene *e = evaluate(src, 0, true, false, &error);
		TASK_CHECK(e == nullptr);
		TASK_CHECK(format(&error).find("unit") != std::string::npos);
		ufbx_free_scene(src);
	}

	// Negative target weight.
	{
		FbxBuilder fb;
		auto T = fb.add_node("T"), A = fb.add_node("A");
		fb.add_parent_constraint("Neg", A.id, T.id, "T", 100, -50);
		ufbx_scene *src = load_string(fb.build(), &error);
		TASK_CHECK(src != nullptr);
		ufbx_scene *e = evaluate(src, 0, true, false, &error);
		TASK_CHECK(e == nullptr);
		TASK_CHECK(format(&error).find("negative") != std::string::npos);
		ufbx_free_scene(src);
	}

	// Inactive constraint plus a non-unit scale node: no effective constraint,
	// so the ordinary evaluation applies and scene limits are not enforced.
	{
		FbxBuilder fb;
		auto T = fb.add_node("T"), A = fb.add_node("A", {
			"P: \"Lcl Scaling\", \"Lcl Scaling\", \"\", \"A\",2,2,2",
		});
		fb.add_parent_constraint("Idle", A.id, T.id, "T", 100, 100, nullptr, nullptr, false);
		ufbx_scene *src = load_string(fb.build(), &error);
		TASK_CHECK(src != nullptr);
		ufbx_scene *e = evaluate(src, 0, true, false, &error);
		TASK_CHECK(e != nullptr);
		TASK_CHECK(node(e, "A")->local_transform.scale.x == 2.0);
		ufbx_free_scene(e);
		ufbx_free_scene(src);
	}

	// An effective constraint plus a non-unit scale node elsewhere fails.
	{
		FbxBuilder fb;
		auto T = fb.add_node("T"), A = fb.add_node("A"), S = fb.add_node("S", {
			"P: \"Lcl Scaling\", \"Lcl Scaling\", \"\", \"A\",3,3,3",
		});
		fb.add_parent_constraint("Active", A.id, T.id, "T");
		(void)S;
		ufbx_scene *src = load_string(fb.build(), &error);
		TASK_CHECK(src != nullptr);
		ufbx_scene *e = evaluate(src, 0, true, false, &error);
		TASK_CHECK(e == nullptr);
		TASK_CHECK(format(&error).find("NORMAL") != std::string::npos
			|| format(&error).find("scale") != std::string::npos);
		ufbx_free_scene(src);
	}

	// An effective constraint plus a non-NORMAL inherit mode fails.
	{
		FbxBuilder fb;
		auto T = fb.add_node("T");
		auto P = fb.add_node("P");
		// InheritType 0 = RrSs (componentwise scale), preserved on load.
		// Nest under P so A is not a top-level node (those are forced normal).
		auto A = fb.add_node("A", {}, "Null", 0);
		fb.connections.pop_back();
		fb.oo(A.id, P.id);
		fb.add_parent_constraint("Inherit", A.id, T.id, "T");
		ufbx_scene *src = load_string(fb.build(), &error, UFBX_INHERIT_MODE_HANDLING_PRESERVE);
		TASK_CHECK(src != nullptr);
		TASK_CHECK(node(src, "A")->inherit_mode != UFBX_INHERIT_MODE_NORMAL);
		ufbx_scene *e = evaluate(src, 0, true, false, &error);
		TASK_CHECK(e == nullptr);
		TASK_CHECK(format(&error).find("NORMAL") != std::string::npos);
		ufbx_free_scene(src);
	}

	return 0;
}

// The provided task_support/parent_chain.fbx fixture, loaded from disk and
// evaluated with both constraints and skinning enabled.
static int dependency_parent_chain_fixture()
{
	ufbx_error error = {};
	ufbx_load_opts load = {};
	load.load_external_files = false;
	ufbx_scene *src = ufbx_load_file("task_support/parent_chain.fbx", &load, &error);
	if (!src) {
		// The runner always executes from the repository root.
		std::fprintf(stderr, "parent_chain.fbx: %s\n", format(&error).c_str());
		TASK_CHECK(false);
	}
	TASK_CHECK(src->constraints.count == 2);

	ufbx_evaluate_opts opts = {};
	opts.evaluate_constraints = true;
	opts.evaluate_skinning = true;
	ufbx_scene *e = ufbx_evaluate_scene(src, src->anim, 0.0, &opts, &error);
	if (!e) { std::fprintf(stderr, "%s\n", format(&error).c_str()); TASK_CHECK(false); }

	// Independent expectation derived from the fixture's static setup:
	// T sits at (2,0) rotated 90 deg about Z. A follows T with offset (1,0,0),
	// which the rotation maps to (0,1): A=(2,1). C is a plain child at local
	// (1,0,0), again mapped by the chain rotation: C=(2,2). B follows C, so
	// B=(2,2). The single bone B skins the mesh; its first vertex at local
	// (1,0,0) maps to (2,3).
	Mat4 t_world = ref_mul(ref_translation(2, 0, 0), ref_rot_axis('z', 90));
	TASK_CHECK(check_matrix(t_world, node(e, "T")->node_to_world, "T fixture"));
	Mat4 a_world = ref_mul(t_world, ref_translation(1, 0, 0));
	TASK_CHECK(check_matrix(a_world, node(e, "A")->node_to_world, "A fixture"));
	Mat4 c_world = ref_mul(a_world, ref_translation(1, 0, 0));
	TASK_CHECK(check_matrix(c_world, node(e, "C")->node_to_world, "C fixture"));
	TASK_CHECK(check_matrix(c_world, node(e, "B")->node_to_world, "B fixture"));

	ufbx_mesh *mesh = e->meshes.data[0];
	TASK_CHECK(mesh->skinned_position.values.count == 3);
	ufbx_vec3 v0 = mesh->skinned_position.values.data[0];
	TASK_CHECK(near_vec(v0.x, 2.0) && near_vec(v0.y, 3.0) && near_vec(v0.z, 0.0));

	ufbx_free_scene(e);
	ufbx_free_scene(src);
	return 0;
}

// ===========================================================================

extern const struct task_case task_cases[];
const struct task_case task_cases[] = {
	{ "dependency", "cross_chain_skinned", dependency_cross_chain_skin },
	{ "dependency", "zero_weight_removes_cycle", dependency_zero_weight_removes_cycle },
	{ "dependency", "joint_cycle_rejected", dependency_joint_cycle_rejected },
	{ "dependency", "parent_chain_fixture", dependency_parent_chain_fixture },

	{ "blend", "pivot_half_weight", blend_pivot_half_weight },
	{ "blend", "dynamic_weight_and_offset", blend_dynamic_weight_and_offset },
	{ "blend", "shortest_arc_slerp", blend_shortest_arc_slerp },

	{ "scene", "option_off_keeps_hierarchy", scene_option_off_keeps_hierarchy },
	{ "scene", "non_identity_bind_geometry_transform", scene_non_identity_bind_and_geometry_transform },
	{ "scene", "repeat_and_reverse_time", scene_repeat_and_reverse_time },
	{ "scene", "rejected_configs_and_limits", scene_rejected_configs_and_limits },
};
const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
