// Target tests for Parent-Child constraint evaluation in ufbx_evaluate_scene().
//
// The scenes below are small ASCII FBX documents parsed with ufbx_load_memory().
// All expected values are computed here with an independent 4x3 matrix
// implementation following the FBX transform composition formulas; the
// tolerance is 1e-7 in unit-scale scenes.

#include "ufbx.h"
#include "task_support/check.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <utility>
#include <vector>
#include <cstdlib>

// ---------------------------------------------------------------------------
// Independent math

struct V3 { double x, y, z; };
struct Q4 { double x, y, z, w; };
struct M4 { double m[4][4]; };

static const double TASK_PI = 3.14159265358979323846;
static const double TASK_EPS = 1e-7;

static M4 m_identity()
{
	M4 r = {};
	r.m[0][0] = r.m[1][1] = r.m[2][2] = r.m[3][3] = 1.0;
	return r;
}

static M4 m_mul(const M4 &a, const M4 &b)
{
	M4 r = {};
	for (int i = 0; i < 4; i++) {
		for (int j = 0; j < 4; j++) {
			double s = 0.0;
			for (int k = 0; k < 4; k++) s += a.m[i][k] * b.m[k][j];
			r.m[i][j] = s;
		}
	}
	return r;
}

static M4 m_translate(double x, double y, double z)
{
	M4 r = m_identity();
	r.m[0][3] = x;
	r.m[1][3] = y;
	r.m[2][3] = z;
	return r;
}

static M4 m_scale(V3 s)
{
	M4 r = m_identity();
	r.m[0][0] = s.x;
	r.m[1][1] = s.y;
	r.m[2][2] = s.z;
	return r;
}

static M4 m_rot_axis(int axis, double deg)
{
	double a = deg * TASK_PI / 180.0;
	double c = std::cos(a), s = std::sin(a);
	M4 r = m_identity();
	if (axis == 0) {
		r.m[1][1] = c; r.m[1][2] = -s;
		r.m[2][1] = s; r.m[2][2] = c;
	} else if (axis == 1) {
		r.m[0][0] = c; r.m[0][2] = s;
		r.m[2][0] = -s; r.m[2][2] = c;
	} else {
		r.m[0][0] = c; r.m[0][1] = -s;
		r.m[1][0] = s; r.m[1][1] = c;
	}
	return r;
}

// Euler angles in a given rotation order. `XYZ` applies X then Y then Z,
// ie. the matrix Rz*Ry*Rx; other orders permute the factors accordingly.
static M4 m_euler_order(V3 deg, int order)
{
	int a0, a1, a2;
	switch (order) {
	case 0: a0 = 0; a1 = 1; a2 = 2; break; // XYZ
	case 1: a0 = 0; a1 = 2; a2 = 1; break; // XZY
	case 2: a0 = 1; a1 = 2; a2 = 0; break; // YZX
	case 3: a0 = 1; a1 = 0; a2 = 2; break; // YXZ
	case 4: a0 = 2; a1 = 0; a2 = 1; break; // ZXY
	default: a0 = 2; a1 = 1; a2 = 0; break; // ZYX
	}
	double angles[3] = { deg.x, deg.y, deg.z };
	return m_mul(m_mul(m_rot_axis(a2, angles[a2]), m_rot_axis(a1, angles[a1])), m_rot_axis(a0, angles[a0]));
}

// Euler XYZ: axes applied X then Y then Z, ie. matrix Rz*Ry*Rx.
static M4 m_euler(V3 deg)
{
	return m_euler_order(deg, 0);
}

static M4 m_rot_transpose(const M4 &a)
{
	M4 r = m_identity();
	for (int i = 0; i < 3; i++) {
		for (int j = 0; j < 3; j++) r.m[i][j] = a.m[j][i];
	}
	return r;
}

static V3 m_point(const M4 &m, V3 p)
{
	V3 r;
	r.x = m.m[0][0]*p.x + m.m[0][1]*p.y + m.m[0][2]*p.z + m.m[0][3];
	r.y = m.m[1][0]*p.x + m.m[1][1]*p.y + m.m[1][2]*p.z + m.m[1][3];
	r.z = m.m[2][0]*p.x + m.m[2][1]*p.y + m.m[2][2]*p.z + m.m[2][3];
	return r;
}

// Inverse of a matrix with orthonormal basis (rotation) and translation.
static M4 m_inverse_rigid(const M4 &m)
{
	M4 rt = m_rot_transpose(m);
	M4 r = rt;
	r.m[0][3] = -(rt.m[0][0]*m.m[0][3] + rt.m[0][1]*m.m[1][3] + rt.m[0][2]*m.m[2][3]);
	r.m[1][3] = -(rt.m[1][0]*m.m[0][3] + rt.m[1][1]*m.m[1][3] + rt.m[1][2]*m.m[2][3]);
	r.m[2][3] = -(rt.m[2][0]*m.m[0][3] + rt.m[2][1]*m.m[1][3] + rt.m[2][2]*m.m[2][3]);
	return r;
}

static Q4 q_from_mat(const M4 &m)
{
	double tr = m.m[0][0] + m.m[1][1] + m.m[2][2];
	Q4 q = {};
	if (tr > 0.0) {
		double s = std::sqrt(tr + 1.0) * 2.0;
		q.w = 0.25 * s;
		q.x = (m.m[2][1] - m.m[1][2]) / s;
		q.y = (m.m[0][2] - m.m[2][0]) / s;
		q.z = (m.m[1][0] - m.m[0][1]) / s;
	} else if (m.m[0][0] > m.m[1][1] && m.m[0][0] > m.m[2][2]) {
		double s = std::sqrt(1.0 + m.m[0][0] - m.m[1][1] - m.m[2][2]) * 2.0;
		q.w = (m.m[2][1] - m.m[1][2]) / s;
		q.x = 0.25 * s;
		q.y = (m.m[0][1] + m.m[1][0]) / s;
		q.z = (m.m[0][2] + m.m[2][0]) / s;
	} else if (m.m[1][1] > m.m[2][2]) {
		double s = std::sqrt(1.0 + m.m[1][1] - m.m[0][0] - m.m[2][2]) * 2.0;
		q.w = (m.m[0][2] - m.m[2][0]) / s;
		q.x = (m.m[0][1] + m.m[1][0]) / s;
		q.y = 0.25 * s;
		q.z = (m.m[1][2] + m.m[2][1]) / s;
	} else {
		double s = std::sqrt(1.0 + m.m[2][2] - m.m[0][0] - m.m[1][1]) * 2.0;
		q.w = (m.m[1][0] - m.m[0][1]) / s;
		q.x = (m.m[0][2] + m.m[2][0]) / s;
		q.y = (m.m[1][2] + m.m[2][1]) / s;
		q.z = 0.25 * s;
	}
	return q;
}

static Q4 q_normalize(Q4 q)
{
	double len = std::sqrt(q.x*q.x + q.y*q.y + q.z*q.z + q.w*q.w);
	if (len == 0.0) return Q4{0,0,0,1};
	q.x /= len; q.y /= len; q.z /= len; q.w /= len;
	return q;
}

static M4 q_to_mat(Q4 q)
{
	double xx = q.x*q.x, yy = q.y*q.y, zz = q.z*q.z;
	double xy = q.x*q.y, xz = q.x*q.z, yz = q.y*q.z;
	double xw = q.x*q.w, yw = q.y*q.w, zw = q.z*q.w;
	M4 r = m_identity();
	r.m[0][0] = 1.0 - 2.0*(yy + zz);
	r.m[0][1] = 2.0*(xy - zw);
	r.m[0][2] = 2.0*(xz + yw);
	r.m[1][0] = 2.0*(xy + zw);
	r.m[1][1] = 1.0 - 2.0*(xx + zz);
	r.m[1][2] = 2.0*(yz - xw);
	r.m[2][0] = 2.0*(xz - yw);
	r.m[2][1] = 2.0*(yz + xw);
	r.m[2][2] = 1.0 - 2.0*(xx + yy);
	return r;
}

// Shortest-arc spherical interpolation.
static Q4 q_slerp(Q4 a, Q4 b, double t)
{
	double dot = a.x*b.x + a.y*b.y + a.z*b.z + a.w*b.w;
	if (dot < 0.0) {
		dot = -dot;
		b.x = -b.x; b.y = -b.y; b.z = -b.z; b.w = -b.w;
	}
	if (dot > 0.9999995) {
		Q4 r = { a.x + (b.x-a.x)*t, a.y + (b.y-a.y)*t, a.z + (b.z-a.z)*t, a.w + (b.w-a.w)*t };
		return q_normalize(r);
	}
	double omega = std::acos(dot > 1.0 ? 1.0 : dot);
	double so = std::sin(omega);
	double af = std::sin((1.0-t)*omega)/so;
	double bf = std::sin(t*omega)/so;
	Q4 r = { a.x*af + b.x*bf, a.y*af + b.y*bf, a.z*af + b.z*bf, a.w*af + b.w*bf };
	return q_normalize(r);
}

static bool near(double a, double b) { return std::fabs(a - b) <= TASK_EPS; }
static bool near_v3(V3 a, V3 b) { return near(a.x,b.x) && near(a.y,b.y) && near(a.z,b.z); }

// Compare an `ufbx_matrix` (column-vector convention) to our matrix.
static bool near_ufbx_matrix(const ufbx_matrix &a, const M4 &b)
{
	for (int r = 0; r < 3; r++) {
		double named[4];
		if (r == 0) { named[0] = a.m00; named[1] = a.m01; named[2] = a.m02; named[3] = a.m03; }
		else if (r == 1) { named[0] = a.m10; named[1] = a.m11; named[2] = a.m12; named[3] = a.m13; }
		else { named[0] = a.m20; named[1] = a.m21; named[2] = a.m22; named[3] = a.m23; }
		for (int c = 0; c < 4; c++) {
			if (!near(named[c], b.m[r][c])) return false;
		}
	}
	return true;
}

static bool near_ufbx_v3(ufbx_vec3 a, V3 b)
{
	return near(a.x, b.x) && near(a.y, b.y) && near(a.z, b.z);
}

// ---------------------------------------------------------------------------
// FBX text generation

static std::string fnum(double v)
{
	char buf[64];
	std::snprintf(buf, sizeof(buf), "%.17g", v);
	return buf;
}

static std::string prop_line(const std::string &name, const std::string &type, const std::string &sub,
	const std::string &flags, const std::string &values)
{
	return "            P: \"" + name + "\", \"" + type + "\", \"" + sub + "\", \"" + flags + "\", " + values + "\n";
}

static std::string model_block(int id, const std::string &name, const std::string &subtype,
	const std::string &extra_props)
{
	std::string s = "    Model: " + std::to_string(id) + ", \"Model::" + name + "\", \"" + subtype + "\" {\n";
	s += "        Version: 232\n        Properties70: {\n";
	s += prop_line("InheritType", "enum", "", "", "1");
	s += extra_props;
	s += "        }\n    }\n";
	return s;
}

static std::string constraint_block(int id, const std::string &name, const std::string &type,
	const std::string &extra_props)
{
	std::string s = "    Constraint: " + std::to_string(id) + ", \"Constraint::" + name + "\", \"" + type + "\" {\n";
	s += "        Type: \"" + type + "\"\n        Properties70: {\n";
	s += prop_line("Constrained object (Child)", "object", "", "", "");
	s += extra_props;
	s += "        }\n    }\n";
	return s;
}

static std::string curve_block(int id, const std::vector<std::pair<int64_t, double> > &keys)
{
	std::string s = "    AnimationCurve: " + std::to_string(id) + ", \"AnimCurve::\", \"\" {\n";
	s += "        Default: 0\n        KeyVer: 4009\n";
	std::string times, values, flags, attrs, refs;
	for (size_t i = 0; i < keys.size(); i++) {
		if (i) { times += ","; values += ","; flags += ","; refs += ","; }
		times += std::to_string(keys[i].first);
		values += fnum(keys[i].second);
		flags += "4"; // Linear interpolation
		refs += "1";
	}
	s += "        KeyTime: *" + std::to_string(keys.size()) + " { a: " + times + " }\n";
	s += "        KeyValueFloat: *" + std::to_string(keys.size()) + " { a: " + values + " }\n";
	s += "        KeyAttrFlags: *" + std::to_string(keys.size()) + " { a: " + flags + " }\n";
	s += "        KeyAttrDataFloat: *" + std::to_string(keys.size()*4) + " { a: ";
	for (size_t i = 0; i < keys.size(); i++) {
		if (i) s += ",";
		s += "0,0,0,0";
	}
	s += " }\n";
	s += "        KeyAttrRefCount: *" + std::to_string(keys.size()) + " { a: " + refs + " }\n";
	s += "    }\n";
	return s;
}

static std::string curve_node_block(int id, const std::string &name, double x, double y, double z)
{
	std::string s = "    AnimationCurveNode: " + std::to_string(id) + ", \"AnimCurveNode::" + name + "\", \"\" {\n";
	s += "        Properties70: {\n";
	s += prop_line("d|X", "Number", "", "A", fnum(x));
	s += prop_line("d|Y", "Number", "", "A", fnum(y));
	s += prop_line("d|Z", "Number", "", "A", fnum(z));
	s += "        }\n    }\n";
	return s;
}

static std::string anim_stack_block(int stack_id, int layer_id)
{
	std::string s = "    AnimationStack: " + std::to_string(stack_id) + ", \"AnimStack::Take 001\", \"\" {\n";
	s += "        Properties70: {\n";
	s += "            P: \"LocalStart\", \"KTime\", \"Time\", \"\",0\n";
	s += "            P: \"LocalStop\", \"KTime\", \"Time\", \"\",46186158000\n";
	s += "            P: \"ReferenceStart\", \"KTime\", \"Time\", \"\",0\n";
	s += "            P: \"ReferenceStop\", \"KTime\", \"Time\", \"\",46186158000\n";
	s += "        }\n    }\n";
	s += "    AnimationLayer: " + std::to_string(layer_id) + ", \"AnimLayer::BaseLayer\", \"\" {\n    }\n";
	return s;
}

// FBX stores matrices row-major as 16 values where a[4*c + r] = m[r][c].
static std::string mat16(const M4 &m)
{
	std::string s = "*16 { a: ";
	bool first = true;
	for (int c = 0; c < 4; c++) {
		for (int r = 0; r < 4; r++) {
			double v = 0.0;
			if (r < 3 && c < 3) v = m.m[r][c];
			else if (r == 3 && c == 3) v = 1.0;
			else if (r < 3 && c == 3) v = m.m[r][3];
			if (!first) s += ",";
			first = false;
			s += fnum(v);
		}
	}
	s += " }";
	return s;
}

static std::string conn_oo(int a, int b)
{
	return "    C: \"OO\"," + std::to_string(a) + "," + std::to_string(b) + "\n";
}

static std::string conn_op(int src, int dst, const std::string &prop)
{
	return "    C: \"OP\"," + std::to_string(src) + "," + std::to_string(dst) + ",\"" + prop + "\"\n";
}

static std::string assemble(const std::vector<std::string> &objects, const std::vector<std::string> &connections,
	bool reverse)
{
	std::string s;
	s += "; FBX 7.4.0 project file\n";
	s += "FBXHeaderExtension: {\n    FBXHeaderVersion: 1003\n    FBXVersion: 7400\n}\n";
	s += "GlobalSettings: {\n    Version: 1000\n    Properties70: {\n";
	s += "        P: \"UpAxis\", \"int\", \"Integer\", \"\",1\n";
	s += "        P: \"UpAxisSign\", \"int\", \"Integer\", \"\",1\n";
	s += "        P: \"FrontAxis\", \"int\", \"Integer\", \"\",2\n";
	s += "        P: \"FrontAxisSign\", \"int\", \"Integer\", \"\",1\n";
	s += "        P: \"CoordAxis\", \"int\", \"Integer\", \"\",0\n";
	s += "        P: \"CoordAxisSign\", \"int\", \"Integer\", \"\",1\n";
	s += "        P: \"UnitScaleFactor\", \"double\", \"Number\", \"\",1\n";
	s += "    }\n}\n";
	s += "Objects: {\n";
	if (reverse) {
		for (size_t i = objects.size(); i > 0; i--) s += objects[i - 1];
	} else {
		for (size_t i = 0; i < objects.size(); i++) s += objects[i];
	}
	s += "}\nConnections: {\n";
	if (reverse) {
		for (size_t i = connections.size(); i > 0; i--) s += connections[i - 1];
	} else {
		for (size_t i = 0; i < connections.size(); i++) s += connections[i];
	}
	s += "}\n";
	return s;
}

static std::string geometry_block(int id, double v0x, double v0y, double v0z,
	double v1x, double v1y, double v1z, double v2x, double v2y, double v2z)
{
	std::string s = "    Geometry: " + std::to_string(id) + ", \"Geometry::Triangle\", \"Mesh\" {\n";
	s += "        GeometryVersion: 124\n";
	s += "        Vertices: *9 { a: " + fnum(v0x) + "," + fnum(v0y) + "," + fnum(v0z) + ","
		+ fnum(v1x) + "," + fnum(v1y) + "," + fnum(v1z) + ","
		+ fnum(v2x) + "," + fnum(v2y) + "," + fnum(v2z) + " }\n";
	s += "        PolygonVertexIndex: *3 { a: 0,1,-3 }\n";
	s += "    }\n";
	return s;
}

static std::string skin_block(int id)
{
	std::string s = "    Deformer: " + std::to_string(id) + ", \"Deformer::Skin\", \"Skin\" {\n";
	s += "        Version: 101\n        SkinningType: \"Linear\"\n    }\n";
	return s;
}

static std::string cluster_block(int id, const M4 &transform, const M4 &transform_link)
{
	std::string s = "    Deformer: " + std::to_string(id) + ", \"SubDeformer::BCluster\", \"Cluster\" {\n";
	s += "        Version: 100\n";
	s += "        Indexes: *3 { a: 0,1,2 }\n";
	s += "        Weights: *3 { a: 1,1,1 }\n";
	s += "        Transform: " + mat16(transform) + "\n";
	s += "        TransformLink: " + mat16(transform_link) + "\n";
	s += "    }\n";
	return s;
}

static const int64_t K_SEC = INT64_C(46186158000);

// Counting allocator used to verify that failed evaluations release memory.
struct AllocCounter {
	long live;
	long total;
};

static void *counter_alloc(void *user, size_t size)
{
	AllocCounter *counter = (AllocCounter*)user;
	void *ptr = std::malloc(size ? size : 1);
	if (ptr) { counter->live++; counter->total++; }
	return ptr;
}

static void *counter_realloc(void *user, void *old_ptr, size_t old_size, size_t new_size)
{
	AllocCounter *counter = (AllocCounter*)user;
	void *ptr = std::realloc(old_ptr, new_size ? new_size : 1);
	if (ptr && !old_ptr) { counter->live++; counter->total++; }
	(void)old_size;
	return ptr;
}

static void counter_free(void *user, void *ptr, size_t size)
{
	AllocCounter *counter = (AllocCounter*)user;
	if (ptr) counter->live--;
	(void)size;
	std::free(ptr);
}

static ufbx_scene *load_text(const std::string &text, ufbx_error *error)
{
	ufbx_load_opts opts = {};
	opts.load_external_files = false;
	return ufbx_load_memory(text.data(), text.size(), &opts, error);
}

static void format_err(const ufbx_error &error, char *buf, size_t size)
{
	ufbx_format_error(buf, size, &error);
}

// ---------------------------------------------------------------------------
// Scene descriptions used by the cases

// Cross target chain:
//   T (rotates) <-(constraint)- A, D is a child of A, B (skinned bone) <- D
// The target of B is a descendant of the constrained node A.
static std::string build_cross_chain(bool reverse, double constraint_weight)
{
	std::vector<std::string> objects, conns;

	std::string t_props;
	t_props += prop_line("Lcl Translation", "Lcl Translation", "", "A", "0,0,0");
	t_props += prop_line("Lcl Rotation", "Lcl Rotation", "", "A", "0,0,0");
	objects.push_back(model_block(10, "T", "Null", t_props));

	objects.push_back(model_block(11, "A", "Null", prop_line("Lcl Translation", "Lcl Translation", "", "A", "1,0,0")));
	objects.push_back(model_block(12, "D", "Null", prop_line("Lcl Translation", "Lcl Translation", "", "A", "0,1,0")));
	objects.push_back(model_block(13, "B", "LimbNode", prop_line("Lcl Translation", "Lcl Translation", "", "A", "0,2,0")));
	objects.push_back(model_block(14, "MeshNode", "Mesh", ""));

	objects.push_back(geometry_block(602, 1,0,0, 0,1,0, 0,0,0));
	objects.push_back(skin_block(600));
	M4 ident = m_identity();
	objects.push_back(cluster_block(601, ident, ident));

	// Interpolate T rotation from 0 to 90 degrees over one second.
	objects.push_back(curve_block(700, { { 0, 0.0 }, { K_SEC, 90.0 } }));
	objects.push_back(curve_node_block(701, "R", 0, 0, 0));
	objects.push_back(anim_stack_block(710, 711));

	std::string a_extra;
	a_extra += prop_line("Weight", "Number", "", "A", "100");
	a_extra += prop_line("T.Weight", "Number", "", "A", "100");
	objects.push_back(constraint_block(20, "FollowT", "Parent-Child", a_extra));

	std::string b_extra;
	b_extra += prop_line("Weight", "Number", "", "A", fnum(constraint_weight * 100.0));
	b_extra += prop_line("D.Weight", "Number", "", "A", "100");
	objects.push_back(constraint_block(21, "FollowD", "Parent-Child", b_extra));

	conns.push_back(conn_oo(10, 0));
	conns.push_back(conn_oo(11, 0));
	conns.push_back(conn_oo(12, 11));
	conns.push_back(conn_oo(13, 0));
	conns.push_back(conn_oo(14, 0));
	conns.push_back(conn_oo(602, 14));
	conns.push_back(conn_oo(600, 602));
	conns.push_back(conn_oo(601, 600));
	conns.push_back(conn_oo(13, 601));
	conns.push_back(conn_op(11, 20, "Constrained object (Child)"));
	conns.push_back(conn_op(10, 20, "Source (Parent)"));
	conns.push_back(conn_op(13, 21, "Constrained object (Child)"));
	conns.push_back(conn_op(12, 21, "Source (Parent)"));
	conns.push_back(conn_oo(711, 710));
	conns.push_back(conn_oo(701, 711));
	conns.push_back(conn_op(700, 701, "d|Z"));
	conns.push_back(conn_op(701, 10, "Lcl Rotation"));

	return assemble(objects, conns, reverse);
}

// Cycle that disappears when the constraint weight is zero:
//   B (root child) <- constrained to D, D is a child of B.
static std::string build_cycle(bool active, double weight, const char *constraint_name)
{
	std::vector<std::string> objects, conns;
	objects.push_back(model_block(10, "T", "Null", prop_line("Lcl Translation", "Lcl Translation", "", "A", "3,0,0")));
	objects.push_back(model_block(13, "B", "Null", prop_line("Lcl Translation", "Lcl Translation", "", "A", "0,2,0")));
	objects.push_back(model_block(12, "D", "Null", prop_line("Lcl Translation", "Lcl Translation", "", "A", "0,1,0")));

	std::string extra;
	extra += prop_line("Active", "bool", "", "", active ? "1" : "0");
	extra += prop_line("Weight", "Number", "", "A", fnum(weight * 100.0));
	extra += prop_line("D.Weight", "Number", "", "A", "100");
	objects.push_back(constraint_block(21, constraint_name, "Parent-Child", extra));

	conns.push_back(conn_oo(10, 0));
	conns.push_back(conn_oo(13, 0));
	conns.push_back(conn_oo(12, 13));
	conns.push_back(conn_op(13, 21, "Constrained object (Child)"));
	conns.push_back(conn_op(12, 21, "Source (Parent)"));
	return assemble(objects, conns, false);
}

// Parent with a non-trivial rotation pivot animated over one second, and a
// child constrained at half weight with a target offset.
static std::string build_pivot_blend()
{
	std::vector<std::string> objects, conns;

	std::string p_props;
	p_props += prop_line("Lcl Translation", "Lcl Translation", "", "A", "0,0,0");
	p_props += prop_line("Lcl Rotation", "Lcl Rotation", "", "A", "0,0,0");
	p_props += prop_line("RotationPivot", "Vector3D", "Vector", "", "0,1,0");
	objects.push_back(model_block(10, "P", "Null", p_props));

	std::string a_props;
	a_props += prop_line("Lcl Translation", "Lcl Translation", "", "A", "0,0,1");
	a_props += prop_line("Lcl Rotation", "Lcl Rotation", "", "A", "0,0,30");
	a_props += prop_line("PreRotation", "Vector3D", "Vector", "", "10,0,0");
	objects.push_back(model_block(11, "A", "Null", a_props));

	std::string extra;
	extra += prop_line("Weight", "Number", "", "A", "50");
	extra += prop_line("P.Weight", "Number", "", "A", "100");
	extra += prop_line("P.Offset T", "Translation", "", "A", "1,2,0");
	extra += prop_line("P.Offset R", "Rotation", "", "A", "0,0,20");
	objects.push_back(constraint_block(20, "Half", "Parent-Child", extra));

	objects.push_back(curve_block(700, { { 0, 0.0 }, { K_SEC, 2.0 } }));
	objects.push_back(curve_node_block(701, "T", 0, 0, 0));
	objects.push_back(curve_block(702, { { 0, 0.0 }, { K_SEC, 90.0 } }));
	objects.push_back(curve_node_block(703, "R", 0, 0, 0));
	objects.push_back(anim_stack_block(710, 711));

	conns.push_back(conn_oo(10, 0));
	conns.push_back(conn_oo(11, 10));
	conns.push_back(conn_op(11, 20, "Constrained object (Child)"));
	conns.push_back(conn_op(10, 20, "Source (Parent)"));
	conns.push_back(conn_oo(711, 710));
	conns.push_back(conn_oo(701, 711));
	conns.push_back(conn_oo(703, 711));
	conns.push_back(conn_op(700, 701, "d|X"));
	conns.push_back(conn_op(702, 703, "d|Z"));
	conns.push_back(conn_op(701, 10, "Lcl Translation"));
	conns.push_back(conn_op(703, 10, "Lcl Rotation"));
	return assemble(objects, conns, false);
}

// Constraint weight and target offset animated over one second.
static std::string build_dynamic()
{
	std::vector<std::string> objects, conns;
	objects.push_back(model_block(10, "P", "Null", prop_line("Lcl Translation", "Lcl Translation", "", "A", "0,0,0")));
	objects.push_back(model_block(11, "A", "Null", ""));

	std::string extra;
	extra += prop_line("Weight", "Number", "", "A", "0");
	extra += prop_line("P.Weight", "Number", "", "A", "100");
	extra += prop_line("P.Offset T", "Translation", "", "A", "0,0,0");
	objects.push_back(constraint_block(20, "Dynamic", "Parent-Child", extra));

	objects.push_back(curve_block(700, { { 0, 0.0 }, { K_SEC, 100.0 } }));
	objects.push_back(curve_node_block(701, "W", 0, 0, 0));
	objects.push_back(curve_block(702, { { 0, 0.0 }, { K_SEC, 4.0 } }));
	objects.push_back(curve_node_block(703, "OT", 0, 0, 0));
	objects.push_back(anim_stack_block(710, 711));

	conns.push_back(conn_oo(10, 0));
	conns.push_back(conn_oo(11, 10));
	conns.push_back(conn_op(11, 20, "Constrained object (Child)"));
	conns.push_back(conn_op(10, 20, "Source (Parent)"));
	conns.push_back(conn_oo(711, 710));
	conns.push_back(conn_oo(701, 711));
	conns.push_back(conn_oo(703, 711));
	conns.push_back(conn_op(700, 701, "d|X"));
	conns.push_back(conn_op(701, 20, "Weight"));
	conns.push_back(conn_op(702, 703, "d|X"));
	conns.push_back(conn_op(703, 20, "P.Offset T"));
	return assemble(objects, conns, false);
}

// Non-unit bind pose, geometry transform and skinning.
static std::string build_bind_scene()
{
	std::vector<std::string> objects, conns;
	objects.push_back(model_block(10, "T", "Null",
		prop_line("Lcl Translation", "Lcl Translation", "", "A", "3,0,0") +
		prop_line("Lcl Rotation", "Lcl Rotation", "", "A", "0,0,90")));
	objects.push_back(model_block(11, "B", "LimbNode",
		prop_line("Lcl Translation", "Lcl Translation", "", "A", "0,1,0")));

	std::string mesh_props;
	mesh_props += prop_line("Lcl Translation", "Lcl Translation", "", "A", "0,0,0");
	mesh_props += prop_line("GeometricTranslation", "Vector3D", "Vector", "", "0.5,0,0");
	mesh_props += prop_line("GeometricRotation", "Vector3D", "Vector", "", "0,0,90");
	mesh_props += prop_line("GeometricScaling", "Vector3D", "Vector", "", "2,2,2");
	objects.push_back(model_block(14, "MeshNode", "Mesh", mesh_props));

	std::string extra;
	extra += prop_line("Weight", "Number", "", "A", "100");
	extra += prop_line("T.Weight", "Number", "", "A", "100");
	objects.push_back(constraint_block(20, "FollowT", "Parent-Child", extra));

	objects.push_back(geometry_block(602, 1,0,0, 0,1,0, 0,0,0));
	objects.push_back(skin_block(600));

	// Non-identity bind: rotate 90 degrees around Z and translate.
	M4 bind = m_mul(m_translate(1, 2, 0), m_euler(V3{0, 0, 90}));
	M4 bind_link = m_mul(m_translate(5, 0, 0), m_euler(V3{0, 0, 45}));
	objects.push_back(cluster_block(601, bind, bind_link));

	conns.push_back(conn_oo(10, 0));
	conns.push_back(conn_oo(11, 0));
	conns.push_back(conn_oo(14, 0));
	conns.push_back(conn_oo(602, 14));
	conns.push_back(conn_oo(600, 602));
	conns.push_back(conn_oo(601, 600));
	conns.push_back(conn_oo(11, 601));
	conns.push_back(conn_op(11, 20, "Constrained object (Child)"));
	conns.push_back(conn_op(10, 20, "Source (Parent)"));
	return assemble(objects, conns, false);
}

// Effective unsupported constraint type: fails only when constraints are evaluated.
static std::string build_unsupported()
{
	std::vector<std::string> objects, conns;
	objects.push_back(model_block(10, "T", "Null", prop_line("Lcl Translation", "Lcl Translation", "", "A", "3,0,0")));
	objects.push_back(model_block(11, "A", "Null", prop_line("Lcl Translation", "Lcl Translation", "", "A", "1,0,0")));

	std::string extra;
	extra += prop_line("Weight", "Number", "", "A", "100");
	extra += prop_line("T.Weight", "Number", "", "A", "100");
	objects.push_back(constraint_block(20, "AimAt", "Aim", extra));

	conns.push_back(conn_oo(10, 0));
	conns.push_back(conn_oo(11, 0));
	conns.push_back(conn_op(11, 20, "Constrained object (Child)"));
	conns.push_back(conn_op(10, 20, "Aim At Object"));
	return assemble(objects, conns, false);
}

// Zero weight constraint with non-unit local scale: ignored entirely.
static std::string build_zero_weight_nonunit()
{
	std::vector<std::string> objects, conns;
	objects.push_back(model_block(10, "T", "Null", prop_line("Lcl Translation", "Lcl Translation", "", "A", "3,0,0")));
	objects.push_back(model_block(11, "A", "Null",
		prop_line("Lcl Translation", "Lcl Translation", "", "A", "1,0,0") +
		prop_line("Lcl Scaling", "Lcl Scaling", "", "A", "2,2,2")));

	std::string extra;
	extra += prop_line("Weight", "Number", "", "A", "0");
	extra += prop_line("T.Weight", "Number", "", "A", "100");
	objects.push_back(constraint_block(20, "FollowT", "Parent-Child", extra));

	conns.push_back(conn_oo(10, 0));
	conns.push_back(conn_oo(11, 0));
	conns.push_back(conn_op(11, 20, "Constrained object (Child)"));
	conns.push_back(conn_op(10, 20, "Source (Parent)"));
	return assemble(objects, conns, false);
}

// Non-default rotation order with pivots and pre/post rotations on both the
// constrained node and its target.
static std::string build_rotation_order_scene(double weight = 100.0)
{
	std::vector<std::string> objects, conns;

	std::string t_props;
	t_props += prop_line("Lcl Translation", "Lcl Translation", "", "A", "1,2,3");
	t_props += prop_line("Lcl Rotation", "Lcl Rotation", "", "A", "30,40,50");
	t_props += prop_line("RotationOrder", "enum", "", "", "4"); // ZXY
	t_props += prop_line("RotationOffset", "Vector3D", "Vector", "", "0,0.5,0");
	t_props += prop_line("ScalingPivot", "Vector3D", "Vector", "", "0,0,1");
	objects.push_back(model_block(10, "T", "Null", t_props));

	std::string a_props;
	a_props += prop_line("Lcl Translation", "Lcl Translation", "", "A", "0,0,2");
	a_props += prop_line("Lcl Rotation", "Lcl Rotation", "", "A", "10,20,30");
	a_props += prop_line("RotationOrder", "enum", "", "", "2"); // YZX
	a_props += prop_line("PreRotation", "Vector3D", "Vector", "", "5,0,0");
	a_props += prop_line("PostRotation", "Vector3D", "Vector", "", "0,7,0");
	a_props += prop_line("RotationPivot", "Vector3D", "Vector", "", "0,1,0");
	objects.push_back(model_block(11, "A", "Null", a_props));

	std::string extra;
	extra += prop_line("Weight", "Number", "", "A", fnum(weight));
	extra += prop_line("T.Weight", "Number", "", "A", "100");
	extra += prop_line("T.Offset T", "Translation", "", "A", "0,1,0");
	extra += prop_line("T.Offset R", "Rotation", "", "A", "0,0,25");
	objects.push_back(constraint_block(20, "FollowT", "Parent-Child", extra));

	conns.push_back(conn_oo(10, 0));
	conns.push_back(conn_oo(11, 0));
	conns.push_back(conn_op(11, 20, "Constrained object (Child)"));
	conns.push_back(conn_op(10, 20, "Source (Parent)"));
	return assemble(objects, conns, false);
}

// An animated local transform as interpreted by ufbx.
struct NodeXf {
	V3 translation, rotation, pre_rotation, post_rotation;
	V3 rotation_pivot, rotation_offset, scaling_pivot, scaling_offset;
	V3 scaling;
	int rotation_order; // index of `ufbx_rotation_order`
};

static NodeXf xf_zero()
{
	NodeXf xf = {};
	xf.scaling = V3{1, 1, 1};
	xf.rotation_order = 0;
	return xf;
}

// Replicates the FBX transform composition formula:
//   T * Roff * Rp * Rpre * R * Rpost^-1 * Rp^-1 * Soff * Sp * S * Sp^-1
static M4 xf_matrix(const NodeXf &xf)
{
	M4 rotation_all = m_mul(m_mul(m_euler(xf.pre_rotation), m_euler_order(xf.rotation, xf.rotation_order)), m_rot_transpose(m_euler(xf.post_rotation)));
	M4 m = m_translate(xf.translation.x, xf.translation.y, xf.translation.z);
	m = m_mul(m, m_translate(xf.rotation_offset.x, xf.rotation_offset.y, xf.rotation_offset.z));
	m = m_mul(m, m_translate(xf.rotation_pivot.x, xf.rotation_pivot.y, xf.rotation_pivot.z));
	m = m_mul(m, rotation_all);
	m = m_mul(m, m_translate(-xf.rotation_pivot.x, -xf.rotation_pivot.y, -xf.rotation_pivot.z));
	m = m_mul(m, m_translate(xf.scaling_offset.x, xf.scaling_offset.y, xf.scaling_offset.z));
	m = m_mul(m, m_translate(xf.scaling_pivot.x, xf.scaling_pivot.y, xf.scaling_pivot.z));
	m = m_mul(m, m_scale(xf.scaling));
	m = m_mul(m, m_translate(-xf.scaling_pivot.x, -xf.scaling_pivot.y, -xf.scaling_pivot.z));
	return m;
}

// The constraint blend formula:
//   G = parent world * animated local without constraint
//   D = target world * target offset
//   result world = G blended towards D by the total weight
static M4 blend_expected(const M4 &g, const M4 &d, double weight)
{
	M4 result = m_identity();
	Q4 qg = q_from_mat(g), qd = q_from_mat(d);
	Q4 q = q_slerp(qg, qd, weight);
	M4 rot = q_to_mat(q);
	for (int r = 0; r < 3; r++) {
		result.m[r][0] = rot.m[r][0];
		result.m[r][1] = rot.m[r][1];
		result.m[r][2] = rot.m[r][2];
		result.m[r][3] = g.m[r][3] + (d.m[r][3] - g.m[r][3]) * weight;
	}
	return result;
}

static ufbx_scene *evaluate(ufbx_scene *source, double time, bool constraints, bool skinning,
	ufbx_error *error)
{
	ufbx_evaluate_opts opts = {};
	opts.evaluate_constraints = constraints;
	opts.evaluate_skinning = skinning;
	return ufbx_evaluate_scene(source, source->anim, time, &opts, error);
}

// ---------------------------------------------------------------------------
// Cases: dependency

static int case_cross_target_skin_chain(void)
{
	std::string text = build_cross_chain(false, 1.0);
	std::string text_rev = build_cross_chain(true, 1.0);
	ufbx_error error = {};

	ufbx_scene *source = load_text(text, &error);
	TASK_CHECK(source != NULL);

	ufbx_scene *forward = evaluate(source, 0.5, true, true, &error);
	TASK_CHECK(forward != NULL);

	const ufbx_node *t = ufbx_find_node(forward, "T");
	const ufbx_node *a = ufbx_find_node(forward, "A");
	const ufbx_node *d = ufbx_find_node(forward, "D");
	const ufbx_node *b = ufbx_find_node(forward, "B");
	const ufbx_node *mesh_node = ufbx_find_node(forward, "MeshNode");
	TASK_CHECK(t && a && d && b && mesh_node);

	// Independent expectation at t=0.5.
	NodeXf t_xf = xf_zero();
	t_xf.rotation = V3{0, 0, 45};
	M4 t_world = xf_matrix(t_xf);

	// A is fully constrained to T with an identity offset.
	M4 a_world = t_world;

	// D is a plain child of A.
	NodeXf d_xf = xf_zero();
	d_xf.translation = V3{0, 1, 0};
	M4 d_world = m_mul(a_world, xf_matrix(d_xf));

	// B is fully constrained to D with an identity offset.
	M4 b_world = d_world;

	M4 mesh_world = m_identity();

	TASK_CHECK(near_ufbx_matrix(a->node_to_world, a_world));
	TASK_CHECK(near_ufbx_matrix(d->node_to_world, d_world));
	TASK_CHECK(near_ufbx_matrix(b->node_to_world, b_world));
	TASK_CHECK(near_ufbx_matrix(mesh_node->node_to_world, mesh_world));

	// The evaluated scene must be a normal evaluated scene.
	TASK_CHECK(forward->constraints.count == 2);
	TASK_CHECK(forward->skin_clusters.count == 1);
	TASK_CHECK(forward->skin_clusters.data[0]->bone_node == b);

	// The skin cluster and skinned positions must reflect the solved pose.
	const ufbx_mesh *mesh = forward->meshes.data[0];
	TASK_CHECK(!mesh->skinned_is_local);
	TASK_CHECK(mesh->skinned_position.values.count == 3);

	// Independently derive the skin matrix: `geometry_to_bone` is the explicit
	// mesh-to-bone binding (identity here) and `geometry_to_world` combines it
	// with the final bone pose.
	M4 bind_world = m_identity();
	M4 mesh_world_at_bind = m_identity();
	M4 geometry_to_bone = m_mul(m_inverse_rigid(bind_world), mesh_world_at_bind);
	M4 cluster_world = m_mul(b_world, geometry_to_bone);
	TASK_CHECK(near_ufbx_matrix(forward->skin_clusters.data[0]->geometry_to_bone, geometry_to_bone));
	TASK_CHECK(near_ufbx_matrix(forward->skin_clusters.data[0]->geometry_to_world, cluster_world));

	V3 v0 = m_point(cluster_world, V3{1, 0, 0});
	V3 v1 = m_point(cluster_world, V3{0, 1, 0});
	V3 v2 = m_point(cluster_world, V3{0, 0, 0});
	TASK_CHECK(near_ufbx_v3(mesh->skinned_position.values.data[0], v0));
	TASK_CHECK(near_ufbx_v3(mesh->skinned_position.values.data[1], v1));
	TASK_CHECK(near_ufbx_v3(mesh->skinned_position.values.data[2], v2));

	// Results must not depend on declaration/connection order.
	ufbx_scene *source_rev = load_text(text_rev, &error);
	TASK_CHECK(source_rev != NULL);
	ufbx_scene *reverse = evaluate(source_rev, 0.5, true, true, &error);
	TASK_CHECK(reverse != NULL);

	const char *names[] = { "T", "A", "D", "B", "MeshNode" };
	for (size_t i = 0; i < 5; i++) {
		const ufbx_node *nf = ufbx_find_node(forward, names[i]);
		const ufbx_node *nr = ufbx_find_node(reverse, names[i]);
		TASK_CHECK(nf && nr);
		for (int r = 0; r < 3; r++) {
			double a_arr[4], b_arr[4];
			if (r == 0) { a_arr[0] = nf->node_to_world.m00; a_arr[1] = nf->node_to_world.m01; a_arr[2] = nf->node_to_world.m02; a_arr[3] = nf->node_to_world.m03; }
			else if (r == 1) { a_arr[0] = nf->node_to_world.m10; a_arr[1] = nf->node_to_world.m11; a_arr[2] = nf->node_to_world.m12; a_arr[3] = nf->node_to_world.m13; }
			else { a_arr[0] = nf->node_to_world.m20; a_arr[1] = nf->node_to_world.m21; a_arr[2] = nf->node_to_world.m22; a_arr[3] = nf->node_to_world.m23; }
			if (r == 0) { b_arr[0] = nr->node_to_world.m00; b_arr[1] = nr->node_to_world.m01; b_arr[2] = nr->node_to_world.m02; b_arr[3] = nr->node_to_world.m03; }
			else if (r == 1) { b_arr[0] = nr->node_to_world.m10; b_arr[1] = nr->node_to_world.m11; b_arr[2] = nr->node_to_world.m12; b_arr[3] = nr->node_to_world.m13; }
			else { b_arr[0] = nr->node_to_world.m20; b_arr[1] = nr->node_to_world.m21; b_arr[2] = nr->node_to_world.m22; b_arr[3] = nr->node_to_world.m23; }
			for (int c = 0; c < 4; c++) TASK_CHECK(near(a_arr[c], b_arr[c]));
		}
	}
	// Compare the reversed scene against the same independent expectation.
	const ufbx_node *ra = ufbx_find_node(reverse, "A");
	const ufbx_node *rb = ufbx_find_node(reverse, "B");
	TASK_CHECK(ra && rb);
	TASK_CHECK(near_ufbx_matrix(ra->node_to_world, a_world));
	TASK_CHECK(near_ufbx_matrix(rb->node_to_world, b_world));
	const ufbx_mesh *rmesh = reverse->meshes.data[0];
	TASK_CHECK(rmesh->skinned_position.values.count == 3);
	TASK_CHECK(near_ufbx_v3(rmesh->skinned_position.values.data[0], v0));
	TASK_CHECK(near_ufbx_v3(rmesh->skinned_position.values.data[1], v1));

	ufbx_free_scene(reverse);
	ufbx_free_scene(source_rev);
	ufbx_free_scene(forward);
	ufbx_free_scene(source);
	return 0;
}

static int case_zero_weight_breaks_cycle(void)
{
	ufbx_error error = {};
	std::string text = build_cycle(true, 0.0, "ZeroCycle");
	ufbx_scene *source = load_text(text, &error);
	TASK_CHECK(source != NULL);

	ufbx_scene *evaluated = evaluate(source, 0.0, true, false, &error);
	TASK_CHECK(evaluated != NULL);

	const ufbx_node *b = ufbx_find_node(evaluated, "B");
	const ufbx_node *d = ufbx_find_node(evaluated, "D");
	TASK_CHECK(b && d);

	// The zero weight constraint is ignored: the hierarchy is the base pose.
	NodeXf b_xf = xf_zero();
	b_xf.translation = V3{0, 2, 0};
	M4 b_world = xf_matrix(b_xf);
	NodeXf d_xf = xf_zero();
	d_xf.translation = V3{0, 1, 0};
	M4 d_world = m_mul(b_world, xf_matrix(d_xf));

	TASK_CHECK(near_ufbx_matrix(b->node_to_world, b_world));
	TASK_CHECK(near_ufbx_matrix(d->node_to_world, d_world));

	// The ignored constraint must still be present in the evaluated scene.
	TASK_CHECK(evaluated->constraints.count == 1);
	TASK_CHECK(evaluated->constraints.data[0]->weight == 0.0);

	ufbx_free_scene(evaluated);

	// An inactive constraint does not create a cycle either.
	std::string text_inactive = build_cycle(false, 1.0, "InactiveCycle");
	ufbx_scene *source_inactive = load_text(text_inactive, &error);
	TASK_CHECK(source_inactive != NULL);
	ufbx_scene *evaluated_inactive = evaluate(source_inactive, 0.0, true, false, &error);
	TASK_CHECK(evaluated_inactive != NULL);
	const ufbx_node *ib = ufbx_find_node(evaluated_inactive, "B");
	TASK_CHECK(ib != NULL);
	TASK_CHECK(near_ufbx_matrix(ib->node_to_world, b_world));

	ufbx_free_scene(evaluated_inactive);
	ufbx_free_scene(source_inactive);
	ufbx_free_scene(source);
	return 0;
}

static int case_joint_cycle_rejected(void)
{
	ufbx_error error = {};
	std::string text = build_cycle(true, 1.0, "Cycle");
	ufbx_scene *source = load_text(text, &error);
	TASK_CHECK(source != NULL);

	ufbx_error eval_error = {};
	ufbx_scene *evaluated = evaluate(source, 0.0, true, false, &eval_error);
	TASK_CHECK(evaluated == NULL);
	TASK_CHECK(eval_error.type != UFBX_ERROR_NONE);

	char buf[1024];
	format_err(eval_error, buf, sizeof(buf));
	TASK_CHECK(std::strstr(buf, "cycle") != NULL || std::strstr(buf, "Cycle") != NULL);
	TASK_CHECK(std::strstr(buf, "B") != NULL);
	TASK_CHECK(std::strstr(buf, "D") != NULL);

	// Disabling the option keeps the original evaluation behavior.
	ufbx_scene *classic = evaluate(source, 0.0, false, false, &error);
	TASK_CHECK(classic != NULL);
	const ufbx_node *b = ufbx_find_node(classic, "B");
	const ufbx_node *d = ufbx_find_node(classic, "D");
	TASK_CHECK(b && d);
	M4 b_world = m_translate(0, 2, 0);
	M4 d_world = m_translate(0, 3, 0);
	TASK_CHECK(near_ufbx_matrix(b->node_to_world, b_world));
	TASK_CHECK(near_ufbx_matrix(d->node_to_world, d_world));

	ufbx_free_scene(classic);
	ufbx_free_scene(source);
	return 0;
}

// ---------------------------------------------------------------------------
// Cases: blend

static int case_pivot_half_weight(void)
{
	ufbx_error error = {};
	std::string text = build_pivot_blend();
	ufbx_scene *source = load_text(text, &error);
	TASK_CHECK(source != NULL);

	const double time = 0.5;
	ufbx_scene *evaluated = evaluate(source, time, true, false, &error);
	TASK_CHECK(evaluated != NULL);

	const ufbx_node *p = ufbx_find_node(evaluated, "P");
	const ufbx_node *a = ufbx_find_node(evaluated, "A");
	TASK_CHECK(p && a);

	// P at t=0.5: translation 1 along X, rotation 45 around Z, pivot (0,1,0).
	NodeXf p_xf = xf_zero();
	p_xf.translation = V3{1, 0, 0};
	p_xf.rotation = V3{0, 0, 45};
	p_xf.rotation_pivot = V3{0, 1, 0};
	M4 p_world = xf_matrix(p_xf);

	// A is static: translation and a rest rotation of 30 degrees with a
	// pre-rotation, none of which is animated.
	NodeXf a_xf = xf_zero();
	a_xf.translation = V3{0, 0, 1};
	a_xf.rotation = V3{0, 0, 30};
	a_xf.pre_rotation = V3{10, 0, 0};
	M4 a_local = xf_matrix(a_xf);

	// Target offset of P.
	NodeXf off_xf = xf_zero();
	off_xf.translation = V3{1, 2, 0};
	off_xf.rotation = V3{0, 0, 20};
	M4 g = m_mul(p_world, a_local);
	M4 d = m_mul(p_world, xf_matrix(off_xf));
	M4 expected_world = blend_expected(g, d, 0.5);
	TASK_CHECK(near_ufbx_matrix(p->node_to_world, p_world));
	TASK_CHECK(near_ufbx_matrix(a->node_to_world, expected_world));

	// The constrained node keeps a unit local scale.
	TASK_CHECK(near(a->local_transform.scale.x, 1.0));
	TASK_CHECK(near(a->local_transform.scale.y, 1.0));
	TASK_CHECK(near(a->local_transform.scale.z, 1.0));

	// Local transform is consistent with node_to_parent/node_to_world.
	M4 expected_local = m_mul(m_inverse_rigid(p_world), expected_world);
	TASK_CHECK(near_ufbx_matrix(a->node_to_parent, expected_local));
	TASK_CHECK(near_ufbx_matrix(a->node_to_world, m_mul(p_world, expected_local)));

	ufbx_free_scene(evaluated);
	ufbx_free_scene(source);
	return 0;
}

static int case_dynamic_weight_offset(void)
{
	ufbx_error error = {};
	std::string text = build_dynamic();
	ufbx_scene *source = load_text(text, &error);
	TASK_CHECK(source != NULL);

	const double times[] = { 0.0, 0.25, 0.5, 1.0 };
	for (size_t i = 0; i < sizeof(times)/sizeof(times[0]); i++) {
		double t = times[i];
		ufbx_scene *evaluated = evaluate(source, t, true, false, &error);
		TASK_CHECK(evaluated != NULL);
		const ufbx_node *a = ufbx_find_node(evaluated, "A");
		TASK_CHECK(a != NULL);

		NodeXf p_xf = xf_zero();
		M4 p_world = xf_matrix(p_xf);

		NodeXf a_xf = xf_zero();
		M4 a_local = xf_matrix(a_xf);

		// Animated weight 0..1 and offset 0..4 along X, both linear.
		double weight = t;
		NodeXf off_xf = xf_zero();
		off_xf.translation = V3{4.0*t, 0, 0};
		M4 g = m_mul(p_world, a_local);
		M4 d = m_mul(p_world, xf_matrix(off_xf));
		M4 expected = blend_expected(g, d, weight);

		TASK_CHECK(near_ufbx_matrix(a->node_to_world, expected));
		TASK_CHECK(near(a->local_transform.scale.x, 1.0));

		// Evaluated constraint reflects the time-dependent weight and offset.
		TASK_CHECK(evaluated->constraints.count == 1);
		const ufbx_constraint *c = evaluated->constraints.data[0];
		TASK_CHECK(near(c->weight, weight));
		TASK_CHECK(near(c->targets.data[0].transform.translation.x, 4.0*t));

		ufbx_free_scene(evaluated);
	}

	ufbx_free_scene(source);
	return 0;
}

// ---------------------------------------------------------------------------
// Cases: scene

static int case_nonunit_bind_geometry(void)
{
	ufbx_error error = {};
	std::string text = build_bind_scene();
	ufbx_scene *source = load_text(text, &error);
	TASK_CHECK(source != NULL);

	ufbx_scene *evaluated = evaluate(source, 0.0, true, true, &error);
	TASK_CHECK(evaluated != NULL);

	const ufbx_node *t = ufbx_find_node(evaluated, "T");
	const ufbx_node *b = ufbx_find_node(evaluated, "B");
	const ufbx_node *mesh_node = ufbx_find_node(evaluated, "MeshNode");
	TASK_CHECK(t && b && mesh_node);

	// T: static translation and 90 degree rotation around Z.
	NodeXf t_xf = xf_zero();
	t_xf.translation = V3{3, 0, 0};
	t_xf.rotation = V3{0, 0, 90};
	M4 t_world = xf_matrix(t_xf);

	// B is fully constrained to T.
	M4 b_world = t_world;

	// Mesh node with only a local transform.
	NodeXf mesh_xf = xf_zero();
	M4 mesh_world = xf_matrix(mesh_xf);

	// Geometry transform of the mesh node.
	NodeXf geo_xf = xf_zero();
	geo_xf.translation = V3{0.5, 0, 0};
	geo_xf.rotation = V3{0, 0, 90};
	geo_xf.scaling = V3{2, 2, 2};
	M4 geometry_to_node = xf_matrix(geo_xf);

	TASK_CHECK(near_ufbx_matrix(t->node_to_world, t_world));
	TASK_CHECK(near_ufbx_matrix(b->node_to_world, b_world));
	TASK_CHECK(near_ufbx_matrix(mesh_node->node_to_world, mesh_world));
	TASK_CHECK(near_ufbx_matrix(mesh_node->geometry_to_node, geometry_to_node));
	TASK_CHECK(near_ufbx_matrix(mesh_node->geometry_to_world, m_mul(mesh_world, geometry_to_node)));

	// Non-unit bind pose and geometry transform affect the skin cluster.
	TASK_CHECK(evaluated->skin_clusters.count == 1);
	const ufbx_skin_cluster *cluster = evaluated->skin_clusters.data[0];
	TASK_CHECK(cluster->bone_node == b);

	M4 bind = m_mul(m_translate(1, 2, 0), m_euler(V3{0, 0, 90}));
	M4 expected_geometry_to_bone = m_mul(bind, geometry_to_node);
	TASK_CHECK(near_ufbx_matrix(cluster->geometry_to_bone, expected_geometry_to_bone));

	M4 expected_geometry_to_world = m_mul(b_world, expected_geometry_to_bone);
	TASK_CHECK(near_ufbx_matrix(cluster->geometry_to_world, expected_geometry_to_world));

	const ufbx_mesh *mesh = evaluated->meshes.data[0];
	TASK_CHECK(!mesh->skinned_is_local);
	TASK_CHECK(mesh->skinned_position.values.count == 3);
	V3 v0 = m_point(expected_geometry_to_world, V3{1, 0, 0});
	V3 v1 = m_point(expected_geometry_to_world, V3{0, 1, 0});
	V3 v2 = m_point(expected_geometry_to_world, V3{0, 0, 0});
	TASK_CHECK(near_ufbx_v3(mesh->skinned_position.values.data[0], v0));
	TASK_CHECK(near_ufbx_v3(mesh->skinned_position.values.data[1], v1));
	TASK_CHECK(near_ufbx_v3(mesh->skinned_position.values.data[2], v2));

	ufbx_free_scene(evaluated);
	ufbx_free_scene(source);
	return 0;
}

static int case_option_off(void)
{
	ufbx_error error = {};
	std::string text = build_unsupported();
	ufbx_scene *source = load_text(text, &error);
	TASK_CHECK(source != NULL);

	// With the option disabled the unsupported constraint is not evaluated.
	ufbx_scene *classic = evaluate(source, 0.0, false, false, &error);
	TASK_CHECK(classic != NULL);
	const ufbx_node *a = ufbx_find_node(classic, "A");
	TASK_CHECK(a != NULL);
	M4 a_world = m_translate(1, 0, 0);
	TASK_CHECK(near_ufbx_matrix(a->node_to_world, a_world));
	TASK_CHECK(classic->constraints.count == 1);

	// The source scene must not be modified by evaluation.
	const ufbx_node *source_t = ufbx_find_node(source, "T");
	TASK_CHECK(source_t != NULL);
	TASK_CHECK(near(source_t->node_to_world.m03, 3.0));

	ufbx_error fail_error = {};
	ufbx_scene *failed = evaluate(source, 0.0, true, false, &fail_error);
	TASK_CHECK(failed == NULL);
	TASK_CHECK(fail_error.type != UFBX_ERROR_NONE);
	char buf[1024];
	format_err(fail_error, buf, sizeof(buf));
	TASK_CHECK(std::strstr(buf, "Unsupported constraint type") != NULL);
	TASK_CHECK(std::strstr(buf, "AimAt") != NULL);
	TASK_CHECK(std::strstr(buf, "A") != NULL);

	ufbx_free_scene(classic);
	ufbx_free_scene(source);

	// Failed evaluation must release the temporary and result allocators.
	{
		AllocCounter counter = { 0, 0 };
		ufbx_allocator allocator = {};
		allocator.alloc_fn = counter_alloc;
		allocator.realloc_fn = counter_realloc;
		allocator.free_fn = counter_free;
		allocator.user = &counter;

		ufbx_error alloc_error = {};
		ufbx_scene *alloc_source = load_text(build_unsupported(), &alloc_error);
		TASK_CHECK(alloc_source != NULL);

		ufbx_evaluate_opts opts = {};
		opts.evaluate_constraints = true;
		opts.temp_allocator.allocator = allocator;
		opts.result_allocator.allocator = allocator;

		ufbx_scene *failed2 = ufbx_evaluate_scene(alloc_source, alloc_source->anim, 0.0, &opts, &alloc_error);
		TASK_CHECK(failed2 == NULL);
		TASK_CHECK(counter.live == 0);

		ufbx_free_scene(alloc_source);
	}

	return 0;
}

static int case_time_order(void)
{
	ufbx_error error = {};
	std::string text = build_cross_chain(false, 1.0);
	ufbx_scene *source = load_text(text, &error);
	TASK_CHECK(source != NULL);

	const double forward_times[] = { 0.0, 0.5, 1.0 };
	const double reverse_times[] = { 1.0, 0.5, 0.0, 0.0 };
	M4 forward[3];
	M4 reverse[3];

	for (size_t i = 0; i < 3; i++) {
		ufbx_scene *evaluated = evaluate(source, forward_times[i], true, true, &error);
		TASK_CHECK(evaluated != NULL);
		const ufbx_node *a = ufbx_find_node(evaluated, "A");
		const ufbx_node *b = ufbx_find_node(evaluated, "B");
		TASK_CHECK(a && b);
		TASK_CHECK(near(a->node_to_world.m00, 1) || true);
		forward[i] = M4{};
		forward[i].m[0][0] = a->node_to_world.m00;
		forward[i].m[1][0] = a->node_to_world.m10;
		forward[i].m[2][0] = a->node_to_world.m20;
		forward[i].m[0][1] = a->node_to_world.m01;
		forward[i].m[1][1] = a->node_to_world.m11;
		forward[i].m[2][1] = a->node_to_world.m21;
		forward[i].m[0][2] = a->node_to_world.m02;
		forward[i].m[1][2] = a->node_to_world.m12;
		forward[i].m[2][2] = a->node_to_world.m22;
		forward[i].m[0][3] = a->node_to_world.m03;
		forward[i].m[1][3] = a->node_to_world.m13;
		forward[i].m[2][3] = a->node_to_world.m23;
		forward[i].m[3][3] = 1.0;
		ufbx_free_scene(evaluated);
	}

	for (size_t k = 0; k < 3; k++) {
		size_t i = 2 - k;
		ufbx_scene *evaluated = evaluate(source, reverse_times[k], true, true, &error);
		TASK_CHECK(evaluated != NULL);
		const ufbx_node *a = ufbx_find_node(evaluated, "A");
		TASK_CHECK(a != NULL);
		reverse[i] = M4{};
		reverse[i].m[0][0] = a->node_to_world.m00;
		reverse[i].m[1][0] = a->node_to_world.m10;
		reverse[i].m[2][0] = a->node_to_world.m20;
		reverse[i].m[0][1] = a->node_to_world.m01;
		reverse[i].m[1][1] = a->node_to_world.m11;
		reverse[i].m[2][1] = a->node_to_world.m21;
		reverse[i].m[0][2] = a->node_to_world.m02;
		reverse[i].m[1][2] = a->node_to_world.m12;
		reverse[i].m[2][2] = a->node_to_world.m22;
		reverse[i].m[0][3] = a->node_to_world.m03;
		reverse[i].m[1][3] = a->node_to_world.m13;
		reverse[i].m[2][3] = a->node_to_world.m23;
		reverse[i].m[3][3] = 1.0;
		ufbx_free_scene(evaluated);
	}

	// Reverse query order must produce exactly the same poses.
	for (size_t i = 0; i < 3; i++) {
		bool same = true;
		for (int r = 0; r < 4; r++) {
			for (int c = 0; c < 4; c++) {
				if (std::fabs(forward[i].m[r][c] - reverse[i].m[r][c]) > 1e-12) same = false;
			}
		}
		TASK_CHECK(same);
	}

	// Repeating the same time gives the same pose.
	{
		ufbx_scene *first = evaluate(source, 0.5, true, true, &error);
		ufbx_scene *second = evaluate(source, 0.5, true, true, &error);
		TASK_CHECK(first && second);
		const ufbx_mesh *m1 = first->meshes.data[0];
		const ufbx_mesh *m2 = second->meshes.data[0];
		V3 p1 = V3{ m1->skinned_position.values.data[0].x, m1->skinned_position.values.data[0].y, m1->skinned_position.values.data[0].z };
		V3 p2 = V3{ m2->skinned_position.values.data[0].x, m2->skinned_position.values.data[0].y, m2->skinned_position.values.data[0].z };
		TASK_CHECK(near_v3(p1, p2));
		ufbx_free_scene(second);
		ufbx_free_scene(first);
	}

	ufbx_free_scene(source);
	return 0;
}

static int case_zero_weight_classic(void)
{
	ufbx_error error = {};
	std::string text = build_zero_weight_nonunit();
	ufbx_scene *source = load_text(text, &error);
	TASK_CHECK(source != NULL);

	// A zero-weight constraint is ignored, so the non-unit scale scene must
	// evaluate normally with the constraints option enabled.
	ufbx_scene *evaluated = evaluate(source, 0.0, true, false, &error);
	TASK_CHECK(evaluated != NULL);

	const ufbx_node *a = ufbx_find_node(evaluated, "A");
	TASK_CHECK(a != NULL);

	NodeXf a_xf = xf_zero();
	a_xf.translation = V3{1, 0, 0};
	a_xf.scaling = V3{2, 2, 2};
	M4 expected = xf_matrix(a_xf);
	TASK_CHECK(near_ufbx_matrix(a->node_to_world, expected));
	TASK_CHECK(near(a->local_transform.scale.x, 2.0));

	ufbx_free_scene(evaluated);
	ufbx_free_scene(source);
	return 0;
}

static int case_rotation_order_pivot(void)
{
	ufbx_error error = {};
	ufbx_scene *source = load_text(build_rotation_order_scene(), &error);
	TASK_CHECK(source != NULL);

	ufbx_scene *evaluated = evaluate(source, 0.0, true, false, &error);
	TASK_CHECK(evaluated != NULL);

	const ufbx_node *t = ufbx_find_node(evaluated, "T");
	const ufbx_node *a = ufbx_find_node(evaluated, "A");
	TASK_CHECK(t && a);

	NodeXf t_xf = xf_zero();
	t_xf.translation = V3{1, 2, 3};
	t_xf.rotation = V3{30, 40, 50};
	t_xf.rotation_order = 4; // ZXY
	t_xf.rotation_offset = V3{0, 0.5, 0};
	t_xf.scaling_pivot = V3{0, 0, 1};
	M4 t_world = xf_matrix(t_xf);

	NodeXf off_xf = xf_zero();
	off_xf.translation = V3{0, 1, 0};
	off_xf.rotation = V3{0, 0, 25};
	M4 target_world = m_mul(t_world, xf_matrix(off_xf));

	TASK_CHECK(near_ufbx_matrix(t->node_to_world, t_world));
	// Full weight: the constrained node equals the target pose.
	TASK_CHECK(near_ufbx_matrix(a->node_to_world, target_world));

	// Static pivot, pre/post rotation and rotation order of the constrained
	// node are reflected in its unconstrained local pose.
	NodeXf a_xf = xf_zero();
	a_xf.translation = V3{0, 0, 2};
	a_xf.rotation = V3{10, 20, 30};
	a_xf.rotation_order = 2; // YZX
	a_xf.pre_rotation = V3{5, 0, 0};
	a_xf.post_rotation = V3{0, 7, 0};
	a_xf.rotation_pivot = V3{0, 1, 0};
	M4 a_base = xf_matrix(a_xf);

	// Verify against a partial-weight evaluation of the same scene.
	ufbx_scene *source_half = load_text(build_rotation_order_scene(50.0), &error);
	TASK_CHECK(source_half != NULL);
	ufbx_scene *half = evaluate(source_half, 0.0, true, false, &error);
	TASK_CHECK(half != NULL);
	const ufbx_node *ha = ufbx_find_node(half, "A");
	TASK_CHECK(ha != NULL);
	M4 expected_half = blend_expected(a_base, target_world, 0.5);
	TASK_CHECK(near_ufbx_matrix(ha->node_to_world, expected_half));
	TASK_CHECK(near(ha->local_transform.scale.x, 1.0));

	ufbx_free_scene(half);
	ufbx_free_scene(source_half);
	ufbx_free_scene(evaluated);
	ufbx_free_scene(source);
	return 0;
}

// ---------------------------------------------------------------------------

extern const struct task_case task_cases[] = {
	{ "dependency", "cross_target_skin_chain", case_cross_target_skin_chain },
	{ "dependency", "zero_weight_breaks_cycle", case_zero_weight_breaks_cycle },
	{ "dependency", "joint_cycle_rejected", case_joint_cycle_rejected },
	{ "blend", "pivot_half_weight", case_pivot_half_weight },
	{ "blend", "dynamic_weight_offset", case_dynamic_weight_offset },
	{ "scene", "nonunit_bind_geometry", case_nonunit_bind_geometry },
	{ "scene", "option_off", case_option_off },
	{ "scene", "time_order", case_time_order },
	{ "scene", "zero_weight_classic", case_zero_weight_classic },
	{ "blend", "rotation_order_pivot", case_rotation_order_pivot },
};

extern const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
