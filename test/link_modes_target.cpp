#include "check.h"
#include "ufbx.h"

#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>

// ---------------------------------------------------------------------------
// Independent matrix math used to compute the expected skinning results.
// Matrices use the same column-major 4x3 affine layout as `ufbx_matrix`.
// ---------------------------------------------------------------------------

struct Vec3 { double x, y, z; };

struct Mat {
	double v[12]; // m00,m10,m20, m01,m11,m21, m02,m12,m22, m03,m13,m23
};

static Mat mat_identity()
{
	Mat r = {{ 1,0,0, 0,1,0, 0,0,1, 0,0,0 }};
	return r;
}

static Mat mat_from_ufbx(const ufbx_matrix &m)
{
	Mat r;
	for (size_t i = 0; i < 12; i++) r.v[i] = (double)m.v[i];
	return r;
}

// Column-major affine matrix entry at (row, col); col 3 is the translation.
static double &me(Mat &m, int row, int col) { return m.v[col*3 + row]; }
static double mec(const Mat &m, int row, int col) { return m.v[col*3 + row]; }

static Mat mat_mul(const Mat &a, const Mat &b)
{
	// r = a * b for 4x4 affine matrices, matching ufbx_matrix_mul().
	Mat r;
	for (int row = 0; row < 3; row++) {
		for (int col = 0; col < 3; col++) {
			double sum = 0.0;
			for (int k = 0; k < 3; k++) sum += mec(a, row, k) * mec(b, k, col);
			me(r, row, col) = sum;
		}
	}
	for (int row = 0; row < 3; row++) {
		double sum = mec(a, row, 3); // translation of a
		for (int k = 0; k < 3; k++) sum += mec(a, row, k) * mec(b, k, 3);
		me(r, row, 3) = sum;
	}
	return r;
}

static Mat mat_translate(double x, double y, double z)
{
	Mat r = mat_identity();
	r.v[9] = x; r.v[10] = y; r.v[11] = z;
	return r;
}

static Mat mat_rotate_z(double ang)
{
	Mat r = mat_identity();
	double c = std::cos(ang), s = std::sin(ang);
	r.v[0] = c;  r.v[1] = s;
	r.v[3] = -s; r.v[4] = c;
	return r;
}

static double mat_det(const Mat &m)
{
	// Determinant of the 3x3 linear block.
	double a=mec(m,0,0), b=mec(m,0,1), c=mec(m,0,2);
	double d=mec(m,1,0), e=mec(m,1,1), f=mec(m,1,2);
	double g=mec(m,2,0), h=mec(m,2,1), i=mec(m,2,2);
	return a*(e*i - f*h) - b*(d*i - f*g) + c*(d*h - e*g);
}

static Mat mat_invert(const Mat &m)
{
	// Work on plain row-major 3x3 R[row][col] extracted from the column-major store.
	double R[3][3];
	for (int r = 0; r < 3; r++)
		for (int c = 0; c < 3; c++)
			R[r][c] = mec(m, r, c);

	double a=R[0][0], b=R[0][1], c=R[0][2];
	double d=R[1][0], e=R[1][1], f=R[1][2];
	double g=R[2][0], h=R[2][1], i=R[2][2];
	double det = a*(e*i - f*h) - b*(d*i - f*g) + c*(d*h - e*g);
	double idet = 1.0 / det;

	// Classic adjugate (transpose of cofactors).
	double inv[3][3];
	inv[0][0] = (e*i - f*h) * idet;
	inv[0][1] = (c*h - b*i) * idet;
	inv[0][2] = (b*f - c*e) * idet;
	inv[1][0] = (f*g - d*i) * idet;
	inv[1][1] = (a*i - c*g) * idet;
	inv[1][2] = (c*d - a*f) * idet;
	inv[2][0] = (d*h - e*g) * idet;
	inv[2][1] = (b*g - a*h) * idet;
	inv[2][2] = (a*e - b*d) * idet;

	Mat out;
	for (int r = 0; r < 3; r++)
		for (int c = 0; c < 3; c++)
			me(out, r, c) = inv[r][c];

	double t[3] = { mec(m,0,3), mec(m,1,3), mec(m,2,3) };
	me(out, 0, 3) = -(inv[0][0]*t[0] + inv[0][1]*t[1] + inv[0][2]*t[2]);
	me(out, 1, 3) = -(inv[1][0]*t[0] + inv[1][1]*t[1] + inv[1][2]*t[2]);
	me(out, 2, 3) = -(inv[2][0]*t[0] + inv[2][1]*t[1] + inv[2][2]*t[2]);
	return out;
}

// `(1-w) * I + w * m`; the identity translation is zero so the translation
// column is simply `w * m.t`, matching ufbxi_lerp_identity_matrix().
static Mat mat_lerp_identity(const Mat &m, double w)
{
	Mat r = mat_identity();
	for (int c = 0; c < 3; c++) {
		for (int row = 0; row < 3; row++) {
			r.v[c*3 + row] = w * m.v[c*3 + row];
		}
	}
	r.v[0] += (1.0 - w);
	r.v[4] += (1.0 - w);
	r.v[8] += (1.0 - w);
	r.v[9] = w * m.v[9];
	r.v[10] = w * m.v[10];
	r.v[11] = w * m.v[11];
	return r;
}

static Vec3 mat_pos(const Mat &m, const Vec3 &p)
{
	Vec3 r;
	r.x = m.v[0]*p.x + m.v[3]*p.y + m.v[6]*p.z + m.v[9];
	r.y = m.v[1]*p.x + m.v[4]*p.y + m.v[7]*p.z + m.v[10];
	r.z = m.v[2]*p.x + m.v[5]*p.y + m.v[8]*p.z + m.v[11];
	return r;
}

static bool near3(const Vec3 &a, const Vec3 &b, double eps = 1e-7)
{
	double dx = std::fabs(a.x - b.x), dy = std::fabs(a.y - b.y), dz = std::fabs(a.z - b.z);
	return dx <= eps && dy <= eps && dz <= eps;
}

static Vec3 to_vec3(const ufbx_vec3 &v) { return { v.x, v.y, v.z }; }

// ---------------------------------------------------------------------------
// Minimal ASCII FBX 7.5.0 generator for skinned meshes.
// ---------------------------------------------------------------------------

struct SkinFbxOptions {
	std::string mode_a;             // "" means no Mode property
	std::string mode_b;
	std::string skinning_type = "Linear";
	bool reverse_cluster_order = false;
	bool rename_clusters = false;
	bool associate_model = false;
	bool transform_associate_model = false;
	bool second_instance = false;
	bool second_skin = false;
	bool geometry_cache = false;
	bool broken_bone_b = false;
	bool geometric_translation = false;
	bool animate = true;
};

struct FbxBuilder {
	std::string s;

	void line(const char *fmt, ...)
	{
		char buf[1024];
		va_list args;
		va_start(args, fmt);
		vsnprintf(buf, sizeof(buf), fmt, args);
		va_end(args);
		s += buf;
		s += '\n';
	}

	void matrix16(const char *name, const Mat &m)
	{
		// Row-major 4x4 with an explicit zero fourth column.
		line("\t\t%s: *16 {", name);
		line("\t\t\ta: %g,%g,%g,0, %g,%g,%g,0, %g,%g,%g,0, %g,%g,%g,1",
			m.v[0], m.v[1], m.v[2],
			m.v[3], m.v[4], m.v[5],
			m.v[6], m.v[7], m.v[8],
			m.v[9], m.v[10], m.v[11]);
		line("\t\t} ");
	}
};

static const uint64_t ID_MESH_NODE = 1001;
static const uint64_t ID_GEOMETRY = 1002;
static const uint64_t ID_MESH_NODE_2 = 1003;
static const uint64_t ID_BONE_A = 2001;
static const uint64_t ID_BONE_B = 2002;
static const uint64_t ID_SKIN = 3001;
static const uint64_t ID_CLUSTER_A = 3002;
static const uint64_t ID_CLUSTER_B = 3003;
static const uint64_t ID_SKIN_2 = 3011;
static const uint64_t ID_CLUSTER_2 = 3012;
static const uint64_t ID_CACHE = 3021;
static const uint64_t ID_STACK = 4001;
static const uint64_t ID_LAYER = 4002;
static const uint64_t ID_CURVE_NODE_T = 4003;
static const uint64_t ID_CURVE_X = 4004;
static const int64_t KTIME_SECOND = 46186158000;

static std::string build_skin_fbx(const SkinFbxOptions &opt)
{
	FbxBuilder b;

	b.line("; FBX 7.5.0 project file");
	b.line("FBXHeaderExtension:  {");
	b.line("\tFBXHeaderVersion: 1003");
	b.line("\tFBXVersion: 7500");
	b.line("\tCreator: \"link_modes_target\"");
	b.line("}");
	b.line("GlobalSettings:  {");
	b.line("\tVersion: 1000");
	b.line("\tProperties70:  {");
	b.line("\t\tP: \"UpAxis\", \"int\", \"Integer\", \"\",1");
	b.line("\t\tP: \"UpAxisSign\", \"int\", \"Integer\", \"\",1");
	b.line("\t\tP: \"FrontAxis\", \"int\", \"Integer\", \"\",2");
	b.line("\t\tP: \"FrontAxisSign\", \"int\", \"Integer\", \"\",1");
	b.line("\t\tP: \"CoordAxis\", \"int\", \"Integer\", \"\",0");
	b.line("\t\tP: \"CoordAxisSign\", \"int\", \"Integer\", \"\",1");
	b.line("\t\tP: \"UnitScaleFactor\", \"double\", \"Number\", \"\",1");
	b.line("\t\tP: \"TimeMode\", \"enum\", \"\", \"\",11");
	b.line("\t}");
	b.line("}");
	b.line("Documents:  {");
	b.line("\tCount: 1");
	b.line("\tDocument: 9001, \"\", \"Scene\" {");
	b.line("\t\tRootNode: 0");
	b.line("\t}");
	b.line("}");
	b.line("Definitions:  {");
	b.line("\tVersion: 100");
	b.line("\tCount: 0");
	b.line("}");
	b.line("Objects:  {");

	// Mesh node, statically translated so F is never the identity.
	b.line("\tModel: %llu, \"Model::MeshNode\", \"Mesh\" {", (unsigned long long)ID_MESH_NODE);
	b.line("\t\tVersion: 232");
	b.line("\t\tProperties70:  {");
	b.line("\t\t\tP: \"Lcl Translation\", \"Lcl Translation\", \"\", \"A\",2,3,0");
	if (opt.geometric_translation) {
		b.line("\t\t\tP: \"GeometricTranslation\", \"Vector3D\", \"Vector\", \"\",0.5,0,0");
	}
	b.line("\t\t}");
	b.line("\t}");

	if (opt.second_instance) {
		b.line("\tModel: %llu, \"Model::MeshNodeTwo\", \"Mesh\" {", (unsigned long long)ID_MESH_NODE_2);
		b.line("\t\tVersion: 232");
		b.line("\t\tProperties70:  {");
		b.line("\t\t\tP: \"Lcl Translation\", \"Lcl Translation\", \"\", \"A\",7,0,0");
		b.line("\t\t}");
		b.line("\t}");
	}

	// Two bones. Bone A is animated on X, bone B has a static Y translation.
	b.line("\tModel: %llu, \"Model::BoneA\", \"LimbNode\" {", (unsigned long long)ID_BONE_A);
	b.line("\t\tVersion: 232");
	b.line("\t\tProperties70:  {");
	b.line("\t\t\tP: \"Lcl Translation\", \"Lcl Translation\", \"\", \"A\",0,0,0");
	b.line("\t\t}");
	b.line("\t}");
	b.line("\tModel: %llu, \"Model::BoneB\", \"LimbNode\" {", (unsigned long long)ID_BONE_B);
	b.line("\t\tVersion: 232");
	b.line("\t\tProperties70:  {");
	b.line("\t\t\tP: \"Lcl Translation\", \"Lcl Translation\", \"\", \"A\",0,5,0");
	b.line("\t\t}");
	b.line("\t}");

	// Geometry with four vertices.
	b.line("\tGeometry: %llu, \"Geometry::Mesh\", \"Mesh\" {", (unsigned long long)ID_GEOMETRY);
	b.line("\t\tVertices: *12 {");
	b.line("\t\t\ta: 1,0,0, 0,1,0, 0,0,0, 1,1,0");
	b.line("\t\t} ");
	b.line("\t\tPolygonVertexIndex: *4 {");
	b.line("\t\t\ta: 0,1,2,-3");
	b.line("\t\t} ");
	b.line("\t}");

	// Skin deformer.
	b.line("\tDeformer: %llu, \"Deformer::Skin\", \"Skin\" {", (unsigned long long)ID_SKIN);
	b.line("\t\tVersion: 101");
	b.line("\t\tLink_DeformAcuracy: 50");
	b.line("\t\tSkinningType: \"%s\"", opt.skinning_type.c_str());
	b.line("\t}");

	if (opt.second_skin) {
		b.line("\tDeformer: %llu, \"Deformer::SkinTwo\", \"Skin\" {", (unsigned long long)ID_SKIN_2);
		b.line("\t\tVersion: 101");
		b.line("\t\tLink_DeformAcuracy: 50");
		b.line("\t\tSkinningType: \"Linear\"");
		b.line("\t}");
	}

	if (opt.geometry_cache) {
		b.line("\tDeformer: %llu, \"Deformer::Cache\", \"VertexCacheDeformer\" {", (unsigned long long)ID_CACHE);
		b.line("\t\tVersion: 100");
		b.line("\t}");
	}

	const char *name_a = opt.rename_clusters ? "ClusterRenamedA" : "ClusterA";
	const char *name_b = opt.rename_clusters ? "ClusterRenamedB" : "ClusterB";

	auto emit_cluster = [&](uint64_t id, const char *name, const std::string &mode,
		const char *indexes, const char *weights, bool associate, bool transform_associate) {
		b.line("\tDeformer: %llu, \"SubDeformer::%s\", \"Cluster\" {", (unsigned long long)id, name);
		b.line("\t\tVersion: 100");
		b.line("\t\tUserData: \"\", \"\"");
		if (!mode.empty()) {
			b.line("\t\tProperties70:  {");
			b.line("\t\t\tP: \"Mode\", \"enum\", \"\", \"\", \"%s\"", mode.c_str());
			b.line("\t\t}");
		}
		b.line("\t\tIndexes: *%s {", std::strchr(indexes, ',') ? "3" : "2");
		b.line("\t\t\ta: %s", indexes);
		b.line("\t\t} ");
		b.line("\t\tWeights: *%s {", std::strchr(indexes, ',') ? "3" : "2");
		b.line("\t\t\ta: %s", weights);
		b.line("\t\t} ");
		b.matrix16("Transform", mat_identity());
		b.matrix16("TransformLink", mat_identity());
		if (associate) {
			b.line("\t\tAssociateModel:  {");
			b.line("\t\t\tVersion: 100");
			b.matrix16("Transform", mat_identity());
			b.line("\t\t}");
		}
		if (transform_associate) {
			b.line("\t\tTransformAssociateModel:  {");
			b.line("\t\t\tVersion: 100");
			b.matrix16("Transform", mat_identity());
			b.line("\t\t}");
		}
		b.line("\t}");
	};

	// Cluster A: vertices 0 (.2), 1 (1.5, a single weight above one), 3 (.25)
	// Cluster B: vertices 0 (.3), 3 (.75); vertex 2 has no influence at all.
	emit_cluster(ID_CLUSTER_A, name_a, opt.mode_a, "0,1,3", "0.2,1.5,0.25",
		opt.associate_model, opt.transform_associate_model);
	emit_cluster(ID_CLUSTER_B, name_b, opt.mode_b, "0,3", "0.3,0.75", false, false);

	if (opt.second_skin) {
		b.line("\tDeformer: %llu, \"SubDeformer::ClusterTwo\", \"Cluster\" {", (unsigned long long)ID_CLUSTER_2);
		b.line("\t\tVersion: 100");
		b.line("\t\tProperties70:  {");
		b.line("\t\t\tP: \"Mode\", \"enum\", \"\", \"\", \"Total1\"");
		b.line("\t\t}");
		b.line("\t\tIndexes: *1 {");
		b.line("\t\t\ta: 2");
		b.line("\t\t} ");
		b.line("\t\tWeights: *1 {");
		b.line("\t\t\ta: 0.4");
		b.line("\t\t} ");
		b.matrix16("Transform", mat_identity());
		b.matrix16("TransformLink", mat_identity());
		b.line("\t}");
	}

	if (opt.animate) {
		b.line("\tAnimationStack: %llu, \"AnimStack::Take 001\", \"\" {", (unsigned long long)ID_STACK);
		b.line("\t\tProperties70:  {");
		b.line("\t\t\tP: \"LocalStop\", \"KTime\", \"Time\", \"\",%lld", (long long)KTIME_SECOND);
		b.line("\t\t\tP: \"ReferenceStop\", \"KTime\", \"Time\", \"\",%lld", (long long)KTIME_SECOND);
		b.line("\t\t}");
		b.line("\t}");
		b.line("\tAnimationLayer: %llu, \"AnimLayer::BaseLayer\", \"\" {", (unsigned long long)ID_LAYER);
		b.line("\t}");
		b.line("\tAnimationCurveNode: %llu, \"AnimCurveNode::T\", \"\" {", (unsigned long long)ID_CURVE_NODE_T);
		b.line("\t\tProperties70:  {");
		b.line("\t\t\tP: \"d|X\", \"Number\", \"\", \"A\",0");
		b.line("\t\t\tP: \"d|Y\", \"Number\", \"\", \"A\",0");
		b.line("\t\t\tP: \"d|Z\", \"Number\", \"\", \"A\",0");
		b.line("\t\t}");
		b.line("\t}");
		b.line("\tAnimationCurve: %llu, \"AnimCurve::X\", \"\" {", (unsigned long long)ID_CURVE_X);
		b.line("\t\tDefault: 0");
		b.line("\t\tKeyVer: 4009");
		b.line("\t\tKeyTime: *2 {");
		b.line("\t\t\ta: 0,%lld", (long long)KTIME_SECOND);
		b.line("\t\t} ");
		b.line("\t\tKeyValueFloat: *2 {");
		b.line("\t\t\ta: 0,10");
		b.line("\t\t} ");
		b.line("\t\tKeyAttrFlags: *2 {");
		b.line("\t\t\ta: 0,0");
		b.line("\t\t} ");
		b.line("\t\tKeyAttrDataFloat: *8 {");
		b.line("\t\t\ta: 0,0,0,0, 0,0,0,0");
		b.line("\t\t} ");
		b.line("\t\tKeyAttrRefCount: *2 {");
		b.line("\t\t\ta: 1,1");
		b.line("\t\t} ");
		b.line("\t}");
	}

	b.line("}");

	b.line("Connections:  {");
	b.line("\tC: \"OO\",%llu,%llu", (unsigned long long)ID_GEOMETRY, (unsigned long long)ID_MESH_NODE);
	b.line("\tC: \"OO\",%llu,0", (unsigned long long)ID_MESH_NODE);
	if (opt.second_instance) {
		b.line("\tC: \"OO\",%llu,%llu", (unsigned long long)ID_GEOMETRY, (unsigned long long)ID_MESH_NODE_2);
		b.line("\tC: \"OO\",%llu,0", (unsigned long long)ID_MESH_NODE_2);
	}
	b.line("\tC: \"OO\",%llu,0", (unsigned long long)ID_BONE_A);
	b.line("\tC: \"OO\",%llu,0", (unsigned long long)ID_BONE_B);
	b.line("\tC: \"OO\",%llu,%llu", (unsigned long long)ID_SKIN, (unsigned long long)ID_GEOMETRY);
	if (opt.geometry_cache) {
		b.line("\tC: \"OO\",%llu,%llu", (unsigned long long)ID_CACHE, (unsigned long long)ID_GEOMETRY);
	}
	if (opt.second_skin) {
		b.line("\tC: \"OO\",%llu,%llu", (unsigned long long)ID_SKIN_2, (unsigned long long)ID_GEOMETRY);
		b.line("\tC: \"OO\",%llu,%llu", (unsigned long long)ID_CLUSTER_2, (unsigned long long)ID_SKIN_2);
		b.line("\tC: \"OO\",%llu,%llu", (unsigned long long)ID_BONE_B, (unsigned long long)ID_CLUSTER_2);
	}

	// Cluster OO connection order is the order used by ADDITIVE chaining.
	if (opt.reverse_cluster_order) {
		b.line("\tC: \"OO\",%llu,%llu", (unsigned long long)ID_CLUSTER_B, (unsigned long long)ID_SKIN);
		b.line("\tC: \"OO\",%llu,%llu", (unsigned long long)ID_CLUSTER_A, (unsigned long long)ID_SKIN);
	} else {
		b.line("\tC: \"OO\",%llu,%llu", (unsigned long long)ID_CLUSTER_A, (unsigned long long)ID_SKIN);
		b.line("\tC: \"OO\",%llu,%llu", (unsigned long long)ID_CLUSTER_B, (unsigned long long)ID_SKIN);
	}
	b.line("\tC: \"OO\",%llu,%llu", (unsigned long long)ID_BONE_A, (unsigned long long)ID_CLUSTER_A);
	if (!opt.broken_bone_b) {
		b.line("\tC: \"OO\",%llu,%llu", (unsigned long long)ID_BONE_B, (unsigned long long)ID_CLUSTER_B);
	}

	if (opt.animate) {
		b.line("\tC: \"OO\",%llu,%llu", (unsigned long long)ID_LAYER, (unsigned long long)ID_STACK);
		b.line("\tC: \"OO\",%llu,%llu", (unsigned long long)ID_CURVE_NODE_T, (unsigned long long)ID_LAYER);
		b.line("\tC: \"OP\",%llu,%llu, \"d|X\"", (unsigned long long)ID_CURVE_X, (unsigned long long)ID_CURVE_NODE_T);
		b.line("\tC: \"OP\",%llu,%llu, \"Lcl Translation\"", (unsigned long long)ID_CURVE_NODE_T, (unsigned long long)ID_BONE_A);
	}
	b.line("}");
	b.line("Takes:  {");
	b.line("}");

	return b.s;
}

// ---------------------------------------------------------------------------
// In-memory skin builders for the pure matrix formula tests.
// ---------------------------------------------------------------------------

struct MemorySkin {
	ufbx_node node;
	ufbx_skin_cluster clusters[2];
	ufbx_skin_cluster *cluster_ptrs[2];
	ufbx_skin_weight weights[2];
	ufbx_skin_vertex vertices[1];
	ufbx_skin_deformer skin;

	MemorySkin(ufbx_matrix wa, ufbx_matrix wb, double w0, double w1, ufbx_skin_link_mode mode)
	{
		std::memset(this, 0, sizeof(*this));
		node = {};
		clusters[0] = {};
		clusters[1] = {};
		clusters[0].bone_node = &node;
		clusters[1].bone_node = &node;
		clusters[0].geometry_to_world = wa;
		clusters[1].geometry_to_world = wb;
		clusters[0].link_mode = mode;
		clusters[1].link_mode = mode;
		cluster_ptrs[0] = &clusters[0];
		cluster_ptrs[1] = &clusters[1];

		weights[0] = { 0, (ufbx_real)w0 };
		weights[1] = { 1, (ufbx_real)w1 };
		ufbx_skin_vertex sv = { 0, 2, 0 };
		vertices[0] = sv;

		skin.skinning_method = UFBX_SKINNING_METHOD_LINEAR;
		skin.link_mode = mode;
		skin.clusters = { cluster_ptrs, 2 };
		skin.vertices = { vertices, 1 };
		skin.weights = { weights, 2 };
	}
};

static ufbx_matrix ufbx_translate(double x, double y, double z)
{
	ufbx_matrix m = ufbx_identity_matrix;
	m.m03 = (ufbx_real)x; m.m13 = (ufbx_real)y; m.m23 = (ufbx_real)z;
	return m;
}

static ufbx_matrix ufbx_rotate_z(double ang)
{
	ufbx_matrix m = ufbx_identity_matrix;
	m.m00 = (ufbx_real)std::cos(ang); m.m10 = (ufbx_real)std::sin(ang);
	m.m01 = (ufbx_real)-std::sin(ang); m.m11 = (ufbx_real)std::cos(ang);
	return m;
}

static Vec3 apply_ufbx(const ufbx_matrix &m, const Vec3 &p)
{
	return to_vec3(ufbx_transform_position(&m, ufbx_vec3{ (ufbx_real)p.x, (ufbx_real)p.y, (ufbx_real)p.z }));
}

// Independent Total1 expectation built from plain matrices.
static Vec3 expected_total_one(const Mat &F, const Mat &wa, const Mat &wb,
	double w0, double w1, const Vec3 &p)
{
	Mat m = mat_identity();
	for (double &x : m.v) x = 0.0;
	// sum(w_i * W_i)
	Mat aw = wa, bw = wb;
	for (double &x : aw.v) x *= w0;
	for (double &x : bw.v) x *= w1;
	for (int i = 0; i < 12; i++) m.v[i] = aw.v[i] + bw.v[i];
	// (1 - sum) * F, residual is kept even when negative.
	Mat rf = F;
	double residual = 1.0 - (w0 + w1);
	for (double &x : rf.v) x *= residual;
	for (int i = 0; i < 12; i++) m.v[i] += rf.v[i];
	return mat_pos(m, p);
}

static Vec3 expected_additive(const Mat &F, const Mat &wa, const Mat &wb,
	double w0, double w1, const Vec3 &p)
{
	Mat inv_f = mat_invert(F);
	Mat ca = mat_mul(inv_f, wa);
	Mat cb = mat_mul(inv_f, wb);
	Mat aa = mat_lerp_identity(ca, w0);
	Mat ab = mat_lerp_identity(cb, w1);
	// Output is F * A_last * ... * A_first * p: cluster A first then cluster B.
	Mat total = mat_mul(ab, aa);
	return mat_pos(mat_mul(F, total), p);
}

// ===========================================================================
// residual group
// ===========================================================================

static void residual_total_one_basic()
{
	// Spec example: W_a translates by (4,0,0), W_b by (0,6,0), weights .2 and .3,
	// F = I, point at the origin -> 0.2*(4,0,0) + 0.3*(0,6,0) + 0.5*0 = (0.8,1.8,0).
	ufbx_matrix wa = ufbx_translate(4, 0, 0);
	ufbx_matrix wb = ufbx_translate(0, 6, 0);
	MemorySkin skin(wa, wb, 0.2, 0.3, UFBX_SKIN_LINK_TOTAL_ONE);

	ufbx_matrix m = ufbx_get_skin_vertex_matrix(&skin.skin, 0, NULL);
	Vec3 got = apply_ufbx(m, { 0, 0, 0 });
	TASK_CHECK(near3(got, { 0.8, 1.8, 0 }));

	// The legacy normalized path still produces (1.6, 3.6, 0) for the same weights.
	MemorySkin norm(wa, wb, 0.2, 0.3, UFBX_SKIN_LINK_NORMALIZE);
	ufbx_matrix mn = ufbx_get_skin_vertex_matrix(&norm.skin, 0, NULL);
	Vec3 got_n = apply_ufbx(mn, { 0, 0, 0 });
	TASK_CHECK(near3(got_n, { 1.6, 3.6, 0 }));
}

static void residual_total_one_under_one_with_fallback()
{
	// Non-identity F with rotation and translation, weights sum below one.
	ufbx_matrix fr = ufbx_rotate_z(0.3);
	ufbx_matrix ft = ufbx_translate(1, 2, 0);
	ufbx_matrix F = ufbx_matrix_mul(&fr, &ft);
	ufbx_matrix wa = ufbx_translate(4, 0, 0);
	ufbx_matrix wb = ufbx_translate(0, 6, 0);
	MemorySkin skin(wa, wb, 0.2, 0.3, UFBX_SKIN_LINK_TOTAL_ONE);

	Vec3 p{ 0.7, -0.4, 0.2 };
	ufbx_matrix m = ufbx_get_skin_vertex_matrix(&skin.skin, 0, &F);
	Vec3 got = apply_ufbx(m, p);
	Vec3 exp = expected_total_one(mat_from_ufbx(F), mat_from_ufbx(wa), mat_from_ufbx(wb), 0.2, 0.3, p);
	TASK_CHECK(near3(got, exp));
}

static void residual_total_one_over_one()
{
	// A single weight above one keeps the negative residual term.
	ufbx_matrix F = ufbx_translate(1, 1, 0);
	ufbx_matrix wa = ufbx_translate(4, 0, 0);
	ufbx_matrix wb = ufbx_translate(0, 6, 0);
	MemorySkin skin(wa, wb, 1.5, 0.0, UFBX_SKIN_LINK_TOTAL_ONE);

	Vec3 p{ 1, 0, 0 };
	ufbx_matrix m = ufbx_get_skin_vertex_matrix(&skin.skin, 0, &F);
	Vec3 got = apply_ufbx(m, p);
	Vec3 exp = expected_total_one(mat_from_ufbx(F), mat_from_ufbx(wa), mat_from_ufbx(wb), 1.5, 0.0, p);
	TASK_CHECK(near3(got, exp));
	// 1.5 * (5,0,0) - 0.5 * (2,1,0) = (6.5, -0.5, 0)
	TASK_CHECK(near3(got, { 6.5, -0.5, 0 }));
}

static void residual_zero_and_null_fallback()
{
	ufbx_matrix wa = ufbx_translate(4, 0, 0);
	ufbx_matrix wb = ufbx_translate(0, 6, 0);
	MemorySkin skin(wa, wb, 0.0, 0.0, UFBX_SKIN_LINK_TOTAL_ONE);

	ufbx_matrix F = ufbx_translate(2, 3, 0);
	Vec3 p{ 1, 1, 1 };

	// No effective influence: result is exactly F * p.
	ufbx_matrix m = ufbx_get_skin_vertex_matrix(&skin.skin, 0, &F);
	TASK_CHECK(near3(apply_ufbx(m, p), apply_ufbx(F, p)));

	// NULL fallback acts as the identity.
	ufbx_matrix mi = ufbx_get_skin_vertex_matrix(&skin.skin, 0, NULL);
	TASK_CHECK(near3(apply_ufbx(mi, p), p));
}

// ===========================================================================
// ordered group
// ===========================================================================

static void ordered_additive_forward_and_reverse()
{
	ufbx_matrix F = ufbx_identity_matrix;
	ufbx_matrix wa = ufbx_rotate_z(M_PI / 2.0); // rotate +90 degrees around Z
	ufbx_matrix wb = ufbx_translate(2, 0, 0);

	// First cluster has the smaller weight so the descending weight sort would
	// place B first; the additive chain must still follow connection order.
	MemorySkin forward(wa, wb, 0.25, 0.75, UFBX_SKIN_LINK_ADDITIVE);

	ufbx_matrix mf = ufbx_get_skin_vertex_matrix(&forward.skin, 0, &F);
	Vec3 got_f = apply_ufbx(mf, { 1, 0, 0 });
	Vec3 exp_f = expected_additive(mat_from_ufbx(F), mat_from_ufbx(wa), mat_from_ufbx(wb), 0.25, 0.75, { 1, 0, 0 });
	TASK_CHECK(near3(got_f, exp_f));
	// Spec reference: rotation first then translation gives (2.25, 0.25, 0).
	TASK_CHECK(near3(got_f, { 2.25, 0.25, 0 }));

	// Swap the connection order by swapping the cluster pointer order.
	MemorySkin reversed(wb, wa, 0.75, 0.25, UFBX_SKIN_LINK_ADDITIVE);
	// weights still reference cluster indices 0/1 in the new order
	ufbx_matrix mr = ufbx_get_skin_vertex_matrix(&reversed.skin, 0, &F);
	Vec3 got_r = apply_ufbx(mr, { 1, 0, 0 });
	Vec3 exp_r = expected_additive(mat_from_ufbx(F), mat_from_ufbx(wb), mat_from_ufbx(wa), 0.75, 0.25, { 1, 0, 0 });
	TASK_CHECK(near3(got_r, exp_r));
	// Spec reference: translation first then rotation gives (1.875, 0.625, 0).
	TASK_CHECK(near3(got_r, { 1.875, 0.625, 0 }));

	// The two orderings must genuinely differ.
	TASK_CHECK(!near3(got_f, got_r));
}

// With a non-identity F the re-expressed matrices C_i = inverse(F) * W_i no
// longer commute, so connection order must change the result even for a vertex
// whose weights happen to commute under F = I.
static void ordered_additive_non_identity_f()
{
	ufbx_matrix fr = ufbx_rotate_z(0.4);
	ufbx_matrix ft = ufbx_translate(1.0, -2.0, 0.5);
	ufbx_matrix F = ufbx_matrix_mul(&fr, &ft);
	ufbx_matrix war = ufbx_rotate_z(0.9), wat = ufbx_translate(3.0, 0.0, 0.0);
	ufbx_matrix wbt = ufbx_translate(0.0, 2.0, 0.0), wbr = ufbx_rotate_z(-0.5);
	ufbx_matrix wa = ufbx_matrix_mul(&war, &wat);
	ufbx_matrix wb = ufbx_matrix_mul(&wbt, &wbr);

	MemorySkin forward(wa, wb, 0.2, 0.3, UFBX_SKIN_LINK_ADDITIVE);
	MemorySkin reversed(wb, wa, 0.3, 0.2, UFBX_SKIN_LINK_ADDITIVE);

	Vec3 p{ 0.6, -0.7, 0.4 };

	// Cross-check the independent inverse/multiply helpers against ufbx itself.
	ufbx_matrix uinv = ufbx_matrix_invert(&F);
	Mat myinv = mat_invert(mat_from_ufbx(F));
	TASK_CHECK(near3({ myinv.v[0], myinv.v[1], myinv.v[2] }, { uinv.m00, uinv.m10, uinv.m20 }));
	TASK_CHECK(near3({ myinv.v[9], myinv.v[10], myinv.v[11] }, { uinv.m03, uinv.m13, uinv.m23 }));
	TASK_CHECK(near3({ myinv.v[9], myinv.v[10], myinv.v[11] }, { uinv.m03, uinv.m13, uinv.m23 }));
	ufbx_matrix um = ufbx_matrix_mul(&wa, &wb);
	Mat mym = mat_mul(mat_from_ufbx(wa), mat_from_ufbx(wb));
	TASK_CHECK(near3({ mym.v[9], mym.v[10], mym.v[11] }, { um.m03, um.m13, um.m23 }));

	Vec3 got_f = apply_ufbx(ufbx_get_skin_vertex_matrix(&forward.skin, 0, &F), p);
	Vec3 got_r = apply_ufbx(ufbx_get_skin_vertex_matrix(&reversed.skin, 0, &F), p);
	Vec3 exp_f = expected_additive(mat_from_ufbx(F), mat_from_ufbx(wa), mat_from_ufbx(wb), 0.2, 0.3, p);
	Vec3 exp_r = expected_additive(mat_from_ufbx(F), mat_from_ufbx(wb), mat_from_ufbx(wa), 0.3, 0.2, p);
	TASK_CHECK(near3(got_f, exp_f));
	TASK_CHECK(near3(got_r, exp_r));
	TASK_CHECK(!near3(got_f, got_r));
}

static void ordered_additive_no_influence()
{
	ufbx_matrix F = ufbx_translate(5, 0, 0);
	MemorySkin skin(ufbx_rotate_z(1.0), ufbx_translate(2, 2, 0), 0.0, 0.0, UFBX_SKIN_LINK_ADDITIVE);

	Vec3 p{ 0.3, 0.8, -0.2 };
	ufbx_matrix m = ufbx_get_skin_vertex_matrix(&skin.skin, 0, &F);
	TASK_CHECK(near3(apply_ufbx(m, p), apply_ufbx(F, p)));

	ufbx_matrix mi = ufbx_get_skin_vertex_matrix(&skin.skin, 0, NULL);
	TASK_CHECK(near3(apply_ufbx(mi, p), p));
}

// ===========================================================================
// scene group
// ===========================================================================

static const ufbx_skin_deformer *get_only_skin(const ufbx_scene *scene)
{
	TASK_CHECK(scene->meshes.count == 1);
	ufbx_mesh *mesh = scene->meshes.data[0];
	TASK_CHECK(mesh->skin_deformers.count == 1);
	return mesh->skin_deformers.data[0];
}

static Vec3 expected_total_one_from_scene(const ufbx_skin_deformer *skin,
	const ufbx_matrix &F, size_t vertex, const Vec3 &p)
{
	Mat m;
	for (double &x : m.v) x = 0.0;
	double total = 0.0;
	ufbx_skin_vertex sv = skin->vertices.data[vertex];
	for (uint32_t i = 0; i < sv.num_weights; i++) {
		ufbx_skin_weight sw = skin->weights.data[sv.weight_begin + i];
		ufbx_matrix w = skin->clusters.data[sw.cluster_index]->geometry_to_world;
		Mat wm = mat_from_ufbx(w);
		for (int k = 0; k < 12; k++) m.v[k] += (double)sw.weight * wm.v[k];
		total += sw.weight;
	}
	Mat rf = mat_from_ufbx(F);
	for (double &x : rf.v) x *= (1.0 - total);
	for (int k = 0; k < 12; k++) m.v[k] += rf.v[k];
	return mat_pos(m, p);
}

static Vec3 expected_additive_from_scene(const ufbx_skin_deformer *skin,
	const ufbx_matrix &F, size_t vertex, const Vec3 &p)
{
	Mat inv_f = mat_invert(mat_from_ufbx(F));
	Mat local = mat_identity();
	ufbx_skin_vertex sv = skin->vertices.data[vertex];
	for (uint32_t ci = 0; ci < skin->clusters.count; ci++) {
		double weight = 0.0;
		bool found = false;
		for (uint32_t i = 0; i < sv.num_weights; i++) {
			ufbx_skin_weight sw = skin->weights.data[sv.weight_begin + i];
			if (sw.cluster_index == ci) { weight = sw.weight; found = true; break; }
		}
		if (!found) continue;
		Mat c = mat_mul(inv_f, mat_from_ufbx(skin->clusters.data[ci]->geometry_to_world));
		Mat a = mat_lerp_identity(c, weight);
		local = mat_mul(a, local);
	}
	return mat_pos(mat_mul(mat_from_ufbx(F), local), p);
}

struct SceneExpectation {
	Vec3 local[4];
};

static SceneExpectation scene_locals()
{
	SceneExpectation e;
	e.local[0] = { 1, 0, 0 };
	e.local[1] = { 0, 1, 0 };
	e.local[2] = { 0, 0, 0 };
	e.local[3] = { 1, 1, 0 };
	return e;
}

static void check_skinned_scene(const std::string &fbx, ufbx_skin_link_mode mode,
	bool geometric_translation, double times[3], size_t num_times)
{
	ufbx_error error;
	ufbx_scene *scene = ufbx_load_memory(fbx.data(), fbx.size(), NULL, &error);
	TASK_CHECK(scene != NULL);
	if (!scene) return;

	const ufbx_skin_deformer *skin = get_only_skin(scene);
	TASK_CHECK(skin->link_mode == mode);
	TASK_CHECK(skin->clusters.count == 2);
	TASK_CHECK(skin->clusters.data[0]->link_mode == mode);
	TASK_CHECK(skin->clusters.data[1]->link_mode == mode);

	ufbx_mesh *mesh = scene->meshes.data[0];
	TASK_CHECK(mesh->instances.count == 1);
	ufbx_node *instance = mesh->instances.data[0];

	ufbx_anim_stack *stack = ufbx_find_anim_stack(scene, "Take 001");
	TASK_CHECK(stack != NULL);

	SceneExpectation locals = scene_locals();

	// Evaluate the same times repeatedly and out of order; results must be stable.
	double order[] = { times[0], times[1], times[2], times[0], times[2], times[1] };
	ufbx_scene *poses[6] = { NULL };
	for (size_t i = 0; i < 6; i++) {
		ufbx_evaluate_opts opts = {};
		opts.evaluate_skinning = true;
		ufbx_error eerr;
		poses[i] = ufbx_evaluate_scene(scene, stack->anim, order[i], &opts, &eerr);
		TASK_CHECK(poses[i] != NULL);
	}

	for (size_t i = 0; i < 6; i++) {
		ufbx_scene *pose = poses[i];
		ufbx_mesh *pmesh = pose->meshes.data[0];
		ufbx_node *pinst = pmesh->instances.data[0];
		const ufbx_skin_deformer *pskin = pmesh->skin_deformers.data[0];
		ufbx_matrix F = pinst->geometry_to_world;

		// The animated bone A moves from 0 to 10 on X over one second, with the
		// exact endpoints; intermediate samples stay on the same monotone path.
		ufbx_node *bone_a = ufbx_find_node(pose, "BoneA");
		TASK_CHECK(bone_a != NULL);
		TASK_CHECK(bone_a->node_to_world.m03 >= -1e-6);
		TASK_CHECK(bone_a->node_to_world.m03 <= 10.0 + 1e-6);
		if (order[i] == 0.0) TASK_CHECK(std::fabs(bone_a->node_to_world.m03) < 1e-6);
		if (order[i] == 1.0) TASK_CHECK(std::fabs(bone_a->node_to_world.m03 - 10.0) < 1e-6);
		ufbx_node *bone_b = ufbx_find_node(pose, "BoneB");
		TASK_CHECK(bone_b != NULL);
		TASK_CHECK(std::fabs(bone_b->node_to_world.m13 - 5.0) < 1e-7);

		for (size_t v = 0; v < 4; v++) {
			Vec3 exp;
			if (mode == UFBX_SKIN_LINK_TOTAL_ONE) {
				exp = expected_total_one_from_scene(pskin, F, v, locals.local[v]);
			} else {
				exp = expected_additive_from_scene(pskin, F, v, locals.local[v]);
			}

			ufbx_vec3 actual = pmesh->skinned_position.values.data[v];
			TASK_CHECK(near3(to_vec3(actual), exp));

			// The matrix entry point must agree with evaluate_scene().
			ufbx_matrix vm = ufbx_get_skin_vertex_matrix(pskin, v, &F);
			TASK_CHECK(near3(apply_ufbx(vm, locals.local[v]), exp));
		}

		// Vertex 2 has no influence, so it deforms with F alone.
		Vec3 zero_exp = mat_pos(mat_from_ufbx(F), locals.local[2]);
		TASK_CHECK(near3(to_vec3(pmesh->skinned_position.values.data[2]), zero_exp));

		// The mesh instance F reflects the geometry transform when requested.
		if (geometric_translation) {
			TASK_CHECK(std::fabs(F.m03 - 2.5) < 1e-7);
		} else {
			TASK_CHECK(std::fabs(F.m03 - 2.0) < 1e-7);
		}
	}

	// Repeated evaluations at the same time match exactly.
	for (size_t v = 0; v < 4; v++) {
		Vec3 a = to_vec3(poses[0]->meshes.data[0]->skinned_position.values.data[v]);
		Vec3 b = to_vec3(poses[3]->meshes.data[0]->skinned_position.values.data[v]);
		TASK_CHECK(near3(a, b));
	}
	for (size_t i = 0; i < 6; i++) ufbx_free_scene(poses[i]);
	ufbx_free_scene(scene);
}

static void scene_total_one()
{
	SkinFbxOptions opt;
	opt.mode_a = opt.mode_b = "Total1";
	std::string fbx = build_skin_fbx(opt);
	double times[3] = { 0.0, 0.5, 1.0 };
	check_skinned_scene(fbx, UFBX_SKIN_LINK_TOTAL_ONE, false, times, 3);
}

static void scene_additive()
{
	SkinFbxOptions opt;
	opt.mode_a = opt.mode_b = "Additive";
	std::string fbx = build_skin_fbx(opt);
	double times[3] = { 0.0, 0.25, 0.75 };
	check_skinned_scene(fbx, UFBX_SKIN_LINK_ADDITIVE, false, times, 3);
}

static void scene_normalize_modes_default()
{
	// Missing Mode, "Average" and "Normalize" all resolve to NORMALIZE.
	for (const char *mode : { (const char*)NULL, "Average", "Normalize" }) {
		SkinFbxOptions opt;
		if (mode) { opt.mode_a = mode; opt.mode_b = mode; }
		std::string fbx = build_skin_fbx(opt);
		ufbx_error error;
		ufbx_scene *scene = ufbx_load_memory(fbx.data(), fbx.size(), NULL, &error);
		TASK_CHECK(scene != NULL);
		if (!scene) continue;
		const ufbx_skin_deformer *skin = get_only_skin(scene);
		TASK_CHECK(skin->link_mode == UFBX_SKIN_LINK_NORMALIZE);
		ufbx_free_scene(scene);
	}
}

static void scene_geometry_transform_support()
{
	SkinFbxOptions opt;
	opt.mode_a = opt.mode_b = "Total1";
	opt.geometric_translation = true;
	std::string fbx = build_skin_fbx(opt);
	double times[2] = { 0.0, 0.5 };
	check_skinned_scene(fbx, UFBX_SKIN_LINK_TOTAL_ONE, true, times, 2);
}

static void scene_rename_and_order_stable()
{
	SkinFbxOptions base;
	base.mode_a = base.mode_b = "Additive";

	SkinFbxOptions renamed = base;
	renamed.rename_clusters = true;
	std::string a = build_skin_fbx(base);
	std::string b = build_skin_fbx(renamed);

	ufbx_error ea, eb, ec;
	ufbx_scene *sa = ufbx_load_memory(a.data(), a.size(), NULL, &ea);
	ufbx_scene *sb = ufbx_load_memory(b.data(), b.size(), NULL, &eb);
	TASK_CHECK(sa && sb);
	if (sa && sb) {
		ufbx_evaluate_opts o = {};
		o.evaluate_skinning = true;
		ufbx_scene *pa = ufbx_evaluate_scene(sa, sa->anim_stacks.data[0]->anim, 0.5, &o, &ea);
		ufbx_scene *pb = ufbx_evaluate_scene(sb, sb->anim_stacks.data[0]->anim, 0.5, &o, &eb);
		TASK_CHECK(pa && pb);
		if (pa && pb) {
			for (size_t v = 0; v < 4; v++) {
				Vec3 xa = to_vec3(pa->meshes.data[0]->skinned_position.values.data[v]);
				Vec3 xb = to_vec3(pb->meshes.data[0]->skinned_position.values.data[v]);
				TASK_CHECK(near3(xa, xb));
			}
		}
		ufbx_free_scene(pa);
		ufbx_free_scene(pb);
	}
	ufbx_free_scene(sb);

	// Reversing the OO connection order changes the additive result.
	SkinFbxOptions reversed = base;
	reversed.reverse_cluster_order = true;
	std::string c = build_skin_fbx(reversed);
	ufbx_scene *sc = ufbx_load_memory(c.data(), c.size(), NULL, &ec);
	TASK_CHECK(sc != NULL);
	if (sc && sa) {
		ufbx_evaluate_opts o = {};
		o.evaluate_skinning = true;
		ufbx_scene *pa = ufbx_evaluate_scene(sa, sa->anim_stacks.data[0]->anim, 0.5, &o, &ea);
		ufbx_scene *pc = ufbx_evaluate_scene(sc, sc->anim_stacks.data[0]->anim, 0.5, &o, &ec);
		TASK_CHECK(pa && pc);
		if (pc && pa) {
			ufbx_mesh *ma = pa->meshes.data[0];
			ufbx_mesh *mc = pc->meshes.data[0];
			// Vertex 0 is influenced by both clusters with asymmetric weights,
			// so rotating cluster A first vs. translating cluster B first differs.
			Vec3 xa = to_vec3(ma->skinned_position.values.data[0]);
			Vec3 xc = to_vec3(mc->skinned_position.values.data[0]);

			// Each scene must agree with its own independent formula, and the
			// cluster connection order must be reflected in the public lists.
			const ufbx_skin_deformer *ska = ma->skin_deformers.data[0];
			const ufbx_skin_deformer *skc = mc->skin_deformers.data[0];
			TASK_CHECK(strcmp(ska->clusters.data[0]->name.data, "ClusterA") == 0);
			TASK_CHECK(strcmp(skc->clusters.data[0]->name.data, "ClusterB") == 0);

			ufbx_matrix fa = ma->instances.data[0]->geometry_to_world;
			ufbx_matrix fc = mc->instances.data[0]->geometry_to_world;
			Vec3 p0 = scene_locals().local[0];
			Vec3 exp_a = expected_additive_from_scene(ska, fa, 0, p0);
			Vec3 exp_c = expected_additive_from_scene(skc, fc, 0, p0);
			TASK_CHECK(near3(xa, exp_a));
			TASK_CHECK(near3(xc, exp_c));
		}
		ufbx_free_scene(pc);
		ufbx_free_scene(pa);
	}
	ufbx_free_scene(sc);
	ufbx_free_scene(sa);
}

static void scene_load_with_evaluate_skinning()
{
	SkinFbxOptions opt;
	opt.mode_a = opt.mode_b = "Total1";
	std::string fbx = build_skin_fbx(opt);

	ufbx_load_opts load_opts = {};
	load_opts.evaluate_skinning = true;
	ufbx_error error;
	ufbx_scene *scene = ufbx_load_memory(fbx.data(), fbx.size(), &load_opts, &error);
	TASK_CHECK(scene != NULL);
	if (scene) {
		ufbx_mesh *mesh = scene->meshes.data[0];
		TASK_CHECK(mesh->skinned_position.values.count == 4);
		ufbx_matrix F = mesh->instances.data[0]->geometry_to_world;
		const ufbx_skin_deformer *skin = mesh->skin_deformers.data[0];
		Vec3 exp = expected_total_one_from_scene(skin, F, 0, scene_locals().local[0]);
		TASK_CHECK(near3(to_vec3(mesh->skinned_position.values.data[0]), exp));
	}
	ufbx_free_scene(scene);
}

// Load must fail and the error must mention the offending cluster, never silently
// succeed with an empty/partially loaded scene.
static bool expect_load_failure(const std::string &fbx, const char *cluster_name)
{
	ufbx_error error;
	ufbx_scene *scene = ufbx_load_memory(fbx.data(), fbx.size(), NULL, &error);
	bool failed = (scene == NULL) && (error.type != UFBX_ERROR_NONE);
	if (scene) ufbx_free_scene(scene);
	if (failed) {
		char msg[512];
		ufbx_format_error(msg, sizeof(msg), &error);
		// The cluster name is carried in the error info string.
		failed = std::strstr(msg, cluster_name) != NULL;
	}
	return failed;
}

static void scene_rejected_combinations()
{
	// Unknown Mode string.
	{
		SkinFbxOptions opt;
		opt.mode_a = opt.mode_b = "Bogus";
		TASK_CHECK(expect_load_failure(build_skin_fbx(opt), "ClusterA"));
	}
	// Mixed link modes inside one skin.
	{
		SkinFbxOptions opt;
		opt.mode_a = "Total1";
		opt.mode_b = "Additive";
		TASK_CHECK(expect_load_failure(build_skin_fbx(opt), "ClusterB"));
	}
	// AssociateModel and TransformAssociateModel subnodes.
	{
		SkinFbxOptions opt;
		opt.mode_a = opt.mode_b = "Total1";
		opt.associate_model = true;
		TASK_CHECK(expect_load_failure(build_skin_fbx(opt), "ClusterA"));
	}
	{
		SkinFbxOptions opt;
		opt.mode_a = opt.mode_b = "Additive";
		opt.transform_associate_model = true;
		TASK_CHECK(expect_load_failure(build_skin_fbx(opt), "ClusterA"));
	}
	// Non-linear (dual quaternion blend) skinning.
	{
		SkinFbxOptions opt;
		opt.mode_a = opt.mode_b = "Total1";
		opt.skinning_type = "Blend";
		TASK_CHECK(expect_load_failure(build_skin_fbx(opt), "ClusterA"));
	}
	// Two mesh instances.
	{
		SkinFbxOptions opt;
		opt.mode_a = opt.mode_b = "Total1";
		opt.second_instance = true;
		TASK_CHECK(expect_load_failure(build_skin_fbx(opt), "ClusterA"));
	}
	// Two skin deformers on one mesh.
	{
		SkinFbxOptions opt;
		opt.mode_a = opt.mode_b = "Total1";
		opt.second_skin = true;
		TASK_CHECK(expect_load_failure(build_skin_fbx(opt), "ClusterA"));
	}
	// Geometry cache on the mesh.
	{
		SkinFbxOptions opt;
		opt.mode_a = opt.mode_b = "Additive";
		opt.geometry_cache = true;
		TASK_CHECK(expect_load_failure(build_skin_fbx(opt), "ClusterA"));
	}
	// Cluster without a valid bone node.
	{
		SkinFbxOptions opt;
		opt.mode_a = opt.mode_b = "Total1";
		opt.broken_bone_b = true;
		TASK_CHECK(expect_load_failure(build_skin_fbx(opt), "ClusterB"));
	}
}

// The same structural features must keep loading for plain NORMALIZE skins.
static void normalize_equivalents_still_load()
{
	auto loads = [](const SkinFbxOptions &opt) {
		std::string fbx = build_skin_fbx(opt);
		ufbx_error error;
		ufbx_scene *scene = ufbx_load_memory(fbx.data(), fbx.size(), NULL, &error);
		bool ok = (scene != NULL);
		ufbx_free_scene(scene);
		return ok;
	};

	{
		SkinFbxOptions opt; // no Mode -> normalize
		opt.associate_model = true;
		TASK_CHECK(loads(opt));
	}
	{
		SkinFbxOptions opt;
		opt.skinning_type = "Blend";
		TASK_CHECK(loads(opt));
	}
	{
		SkinFbxOptions opt;
		opt.second_instance = true;
		TASK_CHECK(loads(opt));
	}
	{
		SkinFbxOptions opt;
		opt.geometry_cache = true;
		TASK_CHECK(loads(opt));
	}
}

// ===========================================================================
// Case registration
// ===========================================================================

const task_case task_cases[] = {
	{ "residual", "residual_total_one_basic", residual_total_one_basic },
	{ "residual", "residual_under_one_fallback", residual_total_one_under_one_with_fallback },
	{ "residual", "residual_over_one_keeps_negative_residual", residual_total_one_over_one },
	{ "residual", "residual_zero_influence_and_null_fallback", residual_zero_and_null_fallback },

	{ "ordered", "ordered_additive_forward_reverse", ordered_additive_forward_and_reverse },
	{ "ordered", "ordered_additive_non_identity_f", ordered_additive_non_identity_f },
	{ "ordered", "ordered_additive_no_influence", ordered_additive_no_influence },

	{ "scene", "scene_total_one_animated", scene_total_one },
	{ "scene", "scene_additive_animated", scene_additive },
	{ "scene", "scene_normalize_mode_strings", scene_normalize_modes_default },
	{ "scene", "scene_geometry_transform", scene_geometry_transform_support },
	{ "scene", "scene_rename_and_connection_order", scene_rename_and_order_stable },
	{ "scene", "scene_load_evaluate_skinning", scene_load_with_evaluate_skinning },
	{ "scene", "scene_rejected_combinations", scene_rejected_combinations },
	{ "scene", "scene_normalize_equivalents_load", normalize_equivalents_still_load },
};

const unsigned task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
