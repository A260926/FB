// Target tests for ufbx_fit_skin_weights().
//
// The scenes are produced entirely in C by emitting small ASCII FBX documents
// into memory and loading them through ufbx_load_memory(), so the solver is
// exercised through the real parser, evaluator and skinning pipeline rather
// than hand-built structures.

#include "ufbx.h"
#include "task_support/check.h"

#include <math.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

// ---------------------------------------------------------------------------
// Small double precision matrix/vector helpers used to build scenes and to
// independently verify the solver. Matrices are 4x3 affine, column major to
// match ufbx_matrix.v[].
// ---------------------------------------------------------------------------

typedef struct { double v[12]; } t_mat;
typedef struct { double x, y, z; } t_vec;

static t_mat t_identity(void)
{
	t_mat r = {{ 1,0,0, 0,1,0, 0,0,1, 0,0,0 }};
	return r;
}

static t_mat t_translation(double x, double y, double z)
{
	t_mat r = t_identity();
	r.v[9] = x; r.v[10] = y; r.v[11] = z;
	return r;
}

static t_mat t_rotz(double deg)
{
	double a = deg * 3.14159265358979323846 / 180.0;
	double c = cos(a), s = sin(a);
	t_mat r = t_identity();
	r.v[0] = c;  r.v[1] = s;
	r.v[3] = -s; r.v[4] = c;
	return r;
}

static t_mat t_mul(const t_mat *a, const t_mat *b)
{
	t_mat r;
	for (int col = 0; col < 4; col++) {
		for (int row = 0; row < 3; row++) {
			double sum = 0.0;
			for (int k = 0; k < 3; k++) sum += a->v[k*3+row] * b->v[col*3+k];
			if (col == 3) sum += a->v[9+row];
			r.v[col*3+row] = sum;
		}
	}
	return r;
}

static t_vec t_xform(const t_mat *m, t_vec p)
{
	t_vec r;
	r.x = m->v[0]*p.x + m->v[3]*p.y + m->v[6]*p.z + m->v[9];
	r.y = m->v[1]*p.x + m->v[4]*p.y + m->v[7]*p.z + m->v[10];
	r.z = m->v[2]*p.x + m->v[5]*p.y + m->v[8]*p.z + m->v[11];
	return r;
}

// Inverse of an affine R|t matrix: R^T | -R^T t.
static t_mat t_inverse_affine(const t_mat *a)
{
	t_mat r = t_identity();
	for (int i = 0; i < 3; i++)
		for (int j = 0; j < 3; j++)
			r.v[j*3+i] = a->v[i*3+j];
	r.v[9]  = -(r.v[0]*a->v[9]  + r.v[3]*a->v[10] + r.v[6]*a->v[11]);
	r.v[10] = -(r.v[1]*a->v[9]  + r.v[4]*a->v[10] + r.v[7]*a->v[11]);
	r.v[11] = -(r.v[2]*a->v[9]  + r.v[5]*a->v[10] + r.v[8]*a->v[11]);
	return r;
}

static t_mat t_from_ufbx(const ufbx_matrix *m)
{
	t_mat r;
	for (int i = 0; i < 12; i++) r.v[i] = (double)m->v[i];
	return r;
}

static t_vec t_from_ufbx_v(ufbx_vec3 v)
{
	t_vec r = { (double)v.x, (double)v.y, (double)v.z };
	return r;
}

static double t_dist(t_vec a, t_vec b)
{
	double dx = a.x-b.x, dy = a.y-b.y, dz = a.z-b.z;
	return sqrt(dx*dx + dy*dy + dz*dz);
}

// ---------------------------------------------------------------------------
// Scene description
// ---------------------------------------------------------------------------

#define T_MAX_BONES 8
#define T_MAX_VERTS 12
#define T_MAX_SAMPLES 64

typedef struct {
	int parent;            // -1 for root
	t_mat bind;            // bind world matrix
	t_vec bind_pos;        // bind local translation (node Lcl Translation)
	double rot0, rot1;     // local Z rotation at t=0 and t=1 (degrees)
	double ty0, ty1;       // local Y translation at t=0 and t=1
} t_bone;

typedef struct {
	int has_morph;
	int morph_vertex;      // vertex displaced by the morph
	t_vec morph_offset;
	double morph0, morph1; // DeformPercent (0..100) at t=0 and t=1
} t_morph;

typedef struct {
	int num_bones;
	int num_vertices;
	t_bone bones[T_MAX_BONES];
	t_vec vertices[T_MAX_VERTS];
	double weights[T_MAX_VERTS][T_MAX_BONES]; // per vertex, per bone, >= 0
	t_vec mesh_node_pos;
	int has_morph;
	int dual_quaternion; // emit a DUAL_QUATERNION skin instead of LINEAR
	t_morph morph;
} t_scene_desc;

// ---------------------------------------------------------------------------
// ASCII FBX emitter
// ---------------------------------------------------------------------------

typedef struct {
	char *data;
	size_t size, cap;
} t_buf;

static void t_buf_init(t_buf *b)
{
	b->cap = 1 << 16;
	b->size = 0;
	b->data = (char*)malloc(b->cap);
	b->data[0] = '\0';
}

static void t_buf_cat(t_buf *b, const char *s)
{
	size_t n = strlen(s);
	if (b->size + n + 1 > b->cap) {
		while (b->size + n + 1 > b->cap) b->cap *= 2;
		b->data = (char*)realloc(b->data, b->cap);
	}
	memcpy(b->data + b->size, s, n);
	b->size += n;
	b->data[b->size] = '\0';
}

static void t_buf_printf(t_buf *b, const char *fmt, ...)
{
	char tmp[512];
	va_list args;
	va_start(args, fmt);
	vsnprintf(tmp, sizeof tmp, fmt, args);
	va_end(args);
	t_buf_cat(b, tmp);
}

static void t_emit_matrix(t_buf *b, const char *name, const t_mat *m)
{
	t_buf_printf(b, "\t\t\t%s: *16 {\n\t\t\t\ta: ", name);
	for (int col = 0; col < 4; col++) {
		for (int row = 0; row < 4; row++) {
			double val;
			if (col < 3) val = m->v[col*3+row];
			else val = (row == 3) ? 1.0 : 0.0;
			t_buf_printf(b, "%s%.12g", (col || row) ? ", " : "", val);
		}
	}
	t_buf_cat(b, "\n\t\t\t} \n");
}

// Object id scheme.
#define T_ID_MESH_GEO 100
#define T_ID_MESH_NODE 101
#define T_ID_BONE_NODE(i) (110 + (i))
#define T_ID_BONE_ATTR(i) (120 + (i))
#define T_ID_SKIN 300
#define T_ID_CLUSTER(i) (301 + (i))
#define T_ID_BLEND 210
#define T_ID_BLEND_CHAN 211
#define T_ID_BLEND_SHAPE 212
#define T_ID_STACK 500
#define T_ID_LAYER 501
#define T_ID_MORPH_NODE 530
#define T_ID_MORPH_CURVE 531
#define T_ID_BONE_NODE_ANIM(i) (540 + (i)*2)
#define T_ID_BONE_CURVE(i) (541 + (i)*2)
#define T_KTIME_PER_SEC 46186158000LL

static void t_emit(t_buf *b, const t_scene_desc *d)
{
	int nb = d->num_bones, nv = d->num_vertices;

	t_buf_cat(b, "; FBX 7.1.0 project file\n");
	t_buf_cat(b, "FBXHeaderExtension:  {\n\tFBXHeaderVersion: 1003\n\tFBXVersion: 7100\n}\n");
	t_buf_cat(b, "GlobalSettings:  {\n\tVersion: 1000\n\tProperties70:  {\n"
		"\t\tP: \"UpAxis\", \"int\", \"Integer\", \"\",1\n"
		"\t\tP: \"UpAxisSign\", \"int\", \"Integer\", \"\",1\n"
		"\t\tP: \"FrontAxis\", \"int\", \"Integer\", \"\",2\n"
		"\t\tP: \"FrontAxisSign\", \"int\", \"Integer\", \"\",1\n"
		"\t\tP: \"CoordAxis\", \"int\", \"Integer\", \"\",0\n"
		"\t\tP: \"CoordAxisSign\", \"int\", \"Integer\", \"\",1\n"
		"\t\tP: \"UnitScaleFactor\", \"double\", \"Number\", \"\",1\n"
		"\t\tP: \"TimeMode\", \"enum\", \"\", \"\",11\n"
		"\t}\n}\n\n");
	t_buf_cat(b, "Documents:  {\n\tCount: 1\n\tDocument: 1, \"\", \"Scene\" {\n\t\tRootNode: 0\n\t}\n}\n\n");

	t_buf_printf(b, "Definitions:  {\n\tVersion: 100\n\tCount: 9\n"
		"\tObjectType: \"GlobalSettings\" { Count: 1 }\n"
		"\tObjectType: \"Model\" { Count: %d }\n"
		"\tObjectType: \"Geometry\" { Count: 2 }\n"
		"\tObjectType: \"Deformer\" { Count: %d }\n"
		"\tObjectType: \"Pose\" { Count: 1 }\n"
		"\tObjectType: \"NodeAttribute\" { Count: %d }\n"
		"\tObjectType: \"AnimationStack\" { Count: 1 }\n"
		"\tObjectType: \"AnimationLayer\" { Count: 1 }\n"
		"}\n\n", nb + 1, nb + 3, nb);

	t_buf_cat(b, "Objects:  {\n");

	// Mesh geometry.
	t_buf_printf(b, "\tGeometry: %d, \"Geo\", \"Mesh\" {\n", T_ID_MESH_GEO);
	t_buf_printf(b, "\t\tVertices: *%d {\n\t\t\ta: ", nv * 3);
	for (int v = 0; v < nv; v++)
		t_buf_printf(b, "%s%.12g,%.12g,%.12g", v ? ", " : "", d->vertices[v].x, d->vertices[v].y, d->vertices[v].z);
	t_buf_cat(b, "\n\t\t} \n");
	// One triangle per three vertices, padded by a degenerate triangle if needed.
	int tris = (nv + 2) / 3;
	t_buf_printf(b, "\t\tPolygonVertexIndex: *%d {\n\t\t\ta: ", tris * 3);
	for (int t = 0; t < tris; t++) {
		int base = t * 3;
		for (int k = 0; k < 3; k++) {
			int idx = base + k;
			if (idx >= nv) idx = nv - 1;
			int encoded = (k == 2) ? ~idx : idx;
			t_buf_printf(b, "%s%d", (t || k) ? ", " : "", encoded);
		}
	}
	t_buf_cat(b, "\n\t\t} \n\t\tGeometryVersion: 124\n");
	t_buf_cat(b, "\t}\n");

	// Mesh model node.
	t_buf_printf(b, "\tModel: %d, \"MeshNode\", \"Mesh\" {\n\t\tVersion: 232\n\t\tProperties70:  {\n", T_ID_MESH_NODE);
	t_buf_printf(b, "\t\tP: \"Lcl Translation\", \"Lcl Translation\", \"\", \"A\",%.12g,%.12g,%.12g\n",
		d->mesh_node_pos.x, d->mesh_node_pos.y, d->mesh_node_pos.z);
	t_buf_cat(b, "\t\tP: \"Lcl Rotation\", \"Lcl Rotation\", \"\", \"A\",0,0,0\n"
		"\t\tP: \"Lcl Scaling\", \"Lcl Scaling\", \"\", \"A\",1,1,1\n\t}\n\t}\n");

	// Bone attributes and model nodes.
	for (int i = 0; i < nb; i++) {
		t_buf_printf(b, "\tNodeAttribute: %d, \"Attr%d\", \"LimbNode\" { TypeFlags: \"Skeleton\" }\n",
			T_ID_BONE_ATTR(i), i);
	}
	for (int i = 0; i < nb; i++) {
		t_buf_printf(b, "\tModel: %d, \"Bone%d\", \"LimbNode\" {\n\t\tVersion: 232\n\t\tProperties70:  {\n",
			T_ID_BONE_NODE(i), i);
		t_buf_printf(b, "\t\tP: \"Lcl Translation\", \"Lcl Translation\", \"\", \"A\",%.12g,%.12g,0\n",
			d->bones[i].bind_pos.x, d->bones[i].bind_pos.y);
		t_buf_printf(b, "\t\tP: \"Lcl Rotation\", \"Lcl Rotation\", \"\", \"A\",0,0,%.12g\n", d->bones[i].rot0);
		t_buf_cat(b, "\t\tP: \"Lcl Scaling\", \"Lcl Scaling\", \"\", \"A\",1,1,1\n\t}\n\t}\n");
	}

	// Bind pose.
	t_buf_cat(b, "\tPose: 400, \"BindPose\", \"BindPose\" {\n\t\tType: \"BindPose\"\n\t\tVersion: 100\n");
	t_buf_printf(b, "\t\tNbPoseNodes: %d\n", nb + 1);
	t_buf_cat(b, "\t\tPoseNode: {\n");
	t_buf_printf(b, "\t\t\tNode: %d\n", T_ID_MESH_NODE);
	t_mat mid = t_translation(d->mesh_node_pos.x, d->mesh_node_pos.y, d->mesh_node_pos.z);
	t_emit_matrix(b, "Matrix", &mid);
	t_buf_cat(b, "\t\t}\n");
	for (int i = 0; i < nb; i++) {
		t_buf_cat(b, "\t\tPoseNode: {\n");
		t_buf_printf(b, "\t\t\tNode: %d\n", T_ID_BONE_NODE(i));
		t_emit_matrix(b, "Matrix", &d->bones[i].bind);
		t_buf_cat(b, "\t\t}\n");
	}
	t_buf_cat(b, "\t}\n");

	// Skin deformer and clusters.
	t_buf_printf(b, "\tDeformer: %d, \"Skin\", \"Skin\" {\n\t\tVersion: 101\n\t\tSkinningType: \"%s\"\n\t}\n",
		T_ID_SKIN, d->dual_quaternion ? "DualQuaternion" : "Linear");
	for (int c = 0; c < nb; c++) {
		t_mat inv = t_inverse_affine(&d->bones[c].bind);
		t_buf_printf(b, "\tDeformer: %d, \"Cluster%d\", \"Cluster\" {\n\t\tVersion: 100\n", T_ID_CLUSTER(c), c);
		t_buf_printf(b, "\t\tIndexes: *%d {\n\t\t\ta: ", nv);
		for (int v = 0; v < nv; v++) t_buf_printf(b, "%s%d", v ? ", " : "", v);
		t_buf_cat(b, "\n\t\t} \n");
		t_buf_printf(b, "\t\tWeights: *%d {\n\t\t\ta: ", nv);
		for (int v = 0; v < nv; v++) t_buf_printf(b, "%s%.12g", v ? ", " : "", d->weights[v][c]);
		t_buf_cat(b, "\n\t\t} \n");
		t_emit_matrix(b, "Transform", &inv);
		t_emit_matrix(b, "TransformLink", &d->bones[c].bind);
		t_buf_cat(b, "\t}\n");
	}

	// Explicit blend shape deformer/channel/shape so the morph weight can be
	// animated through the DeformPercent property.
	if (d->has_morph) {
		t_buf_printf(b, "\tDeformer: %d, \"Blend\", \"BlendShape\" { Version: 100 }\n", T_ID_BLEND);
		t_buf_printf(b, "\tDeformer: %d, \"BlendChan\", \"BlendShapeChannel\" {\n"
			"\t\tVersion: 100\n\t\tDeformPercent: %.12g\n\t\tFullWeights: *1 { a: 100 }\n\t}\n",
			T_ID_BLEND_CHAN, d->morph.morph0);
		t_buf_printf(b, "\tGeometry: %d, \"MorphShape\", \"Shape\" {\n"
			"\t\tIndexes: *1 { a: %d }\n"
			"\t\tVertices: *3 { a: %.12g,%.12g,%.12g }\n\t}\n",
			T_ID_BLEND_SHAPE, d->morph.morph_vertex,
			d->morph.morph_offset.x, d->morph.morph_offset.y, d->morph.morph_offset.z);
	}

	// Animation: one stack/layer, one curve node per bone rotation and one for
	// the morph weight.
	t_buf_cat(b, "\tAnimationStack: 500, \"Stack\", \"\" {\n\t\tProperties70:  {\n"
		"\t\tP: \"LocalStop\", \"KTime\", \"Time\", \"\",46186158000\n"
		"\t\tP: \"ReferenceStop\", \"KTime\", \"Time\", \"\",46186158000\n\t}\n\t}\n");
	t_buf_cat(b, "\tAnimationLayer: 501, \"Layer\", \"\" {}\n");

	for (int i = 0; i < nb; i++) {
		t_buf_printf(b, "\tAnimationCurveNode: %d, \"Rot%d\", \"\" {\n\t\tProperties70:  {\n"
			"\t\tP: \"d|X\", \"Number\", \"\", \"A\",0\n\t\tP: \"d|Y\", \"Number\", \"\", \"A\",0\n"
			"\t\tP: \"d|Z\", \"Number\", \"\", \"A\",0\n\t}\n\t}\n", T_ID_BONE_NODE_ANIM(i), i);
		t_buf_printf(b, "\tAnimationCurve: %d, \"Curve%d\", \"\" {\n\t\tDefault: 0\n\t\tKeyVer: 4009\n"
			"\t\tKeyTime: *2 { a: 0,%lld }\n"
			"\t\tKeyValueFloat: *2 { a: %.12g,%.12g }\n"
			"\t\tKeyAttrFlags: *2 { a: 1032,1032 }\n"
			"\t\tKeyAttrDataFloat: *8 { a: 0,0,0,0, 0,0,0,0 }\n"
			"\t\tKeyAttrRefCount: *2 { a: 1,1 }\n\t}\n",
			T_ID_BONE_CURVE(i), i, (long long)T_KTIME_PER_SEC, d->bones[i].rot0, d->bones[i].rot1);
	}
	if (d->has_morph) {
		t_buf_printf(b, "\tAnimationCurveNode: %d, \"MorphWeight\", \"\" {\n\t\tProperties70:  {\n"
			"\t\tP: \"d|X\", \"Number\", \"\", \"A\",0\n\t}\n\t}\n", T_ID_MORPH_NODE);
		t_buf_printf(b, "\tAnimationCurve: %d, \"MorphCurve\", \"\" {\n\t\tDefault: 0\n\t\tKeyVer: 4009\n"
			"\t\tKeyTime: *2 { a: 0,%lld }\n"
			"\t\tKeyValueFloat: *2 { a: %.12g,%.12g }\n"
			"\t\tKeyAttrFlags: *2 { a: 1032,1032 }\n"
			"\t\tKeyAttrDataFloat: *8 { a: 0,0,0,0, 0,0,0,0 }\n"
			"\t\tKeyAttrRefCount: *2 { a: 1,1 }\n\t}\n",
			T_ID_MORPH_CURVE, (long long)T_KTIME_PER_SEC, d->morph.morph0, d->morph.morph1);
	}

	t_buf_cat(b, "}\n\n");

	// Connections.
	t_buf_cat(b, "Connections:  {\n");
	t_buf_printf(b, "\tC: \"OO\",%d,0\n", T_ID_MESH_NODE);
	for (int i = 0; i < nb; i++) {
		if (d->bones[i].parent >= 0)
			t_buf_printf(b, "\tC: \"OO\",%d,%d\n", T_ID_BONE_NODE(i), T_ID_BONE_NODE(d->bones[i].parent));
		else
			t_buf_printf(b, "\tC: \"OO\",%d,0\n", T_ID_BONE_NODE(i));
		t_buf_printf(b, "\tC: \"OO\",%d,%d\n", T_ID_BONE_ATTR(i), T_ID_BONE_NODE(i));
	}
	t_buf_printf(b, "\tC: \"OO\",%d,%d\n", T_ID_SKIN, T_ID_MESH_GEO);
	t_buf_printf(b, "\tC: \"OO\",%d,%d\n", T_ID_MESH_GEO, T_ID_MESH_NODE);
	for (int c = 0; c < nb; c++) {
		t_buf_printf(b, "\tC: \"OO\",%d,%d\n", T_ID_CLUSTER(c), T_ID_SKIN);
		t_buf_printf(b, "\tC: \"OO\",%d,%d\n", T_ID_BONE_NODE(c), T_ID_CLUSTER(c));
	}
	if (d->has_morph) {
		t_buf_printf(b, "\tC: \"OO\",%d,%d\n", T_ID_BLEND, T_ID_MESH_GEO);
		t_buf_printf(b, "\tC: \"OO\",%d,%d\n", T_ID_BLEND_CHAN, T_ID_BLEND);
		t_buf_printf(b, "\tC: \"OO\",%d,%d\n", T_ID_BLEND_SHAPE, T_ID_BLEND_CHAN);
	}
	t_buf_cat(b, "\tC: \"OO\",501,500\n");
	for (int i = 0; i < nb; i++) {
		t_buf_printf(b, "\tC: \"OO\",%d,501\n", T_ID_BONE_NODE_ANIM(i));
		t_buf_printf(b, "\tC: \"OP\",%d,%d, \"Lcl Rotation\"\n", T_ID_BONE_NODE_ANIM(i), T_ID_BONE_NODE(i));
		t_buf_printf(b, "\tC: \"OP\",%d,%d, \"d|Z\"\n", T_ID_BONE_CURVE(i), T_ID_BONE_NODE_ANIM(i));
	}
	if (d->has_morph) {
		t_buf_cat(b, "\tC: \"OO\",530,501\n");
		t_buf_printf(b, "\tC: \"OP\",%d,%d, \"DeformPercent\"\n", T_ID_MORPH_NODE, T_ID_BLEND_CHAN);
		t_buf_printf(b, "\tC: \"OP\",%d,%d, \"d|X\"\n", T_ID_MORPH_CURVE, T_ID_MORPH_NODE);
	}
	t_buf_cat(b, "}\n");
}

static ufbx_scene *t_load(const t_scene_desc *d, ufbx_error *error)
{
	t_buf b;
	t_buf_init(&b);
	t_emit(&b, d);
	ufbx_load_opts opts = { 0 };
	ufbx_scene *scene = ufbx_load_memory(b.data, b.size, &opts, error);
	free(b.data);
	return scene;
}

// ---------------------------------------------------------------------------
// Independent reference: build the same least squares problem directly from
// the evaluated ufbx frames and brute-force every simplex support, in plain
// double precision, so the library solver's optimum can be checked against an
// independent implementation.
// ---------------------------------------------------------------------------

typedef struct {
	t_mat cluster_world[T_MAX_SAMPLES][T_MAX_BONES];
	t_vec morphed[T_MAX_SAMPLES][T_MAX_VERTS];
	t_vec reference[T_MAX_SAMPLES][T_MAX_VERTS];
	int num_samples, num_vertices, num_bones;
} t_ref_data;

// Evaluate the frame at time t and copy out the raw quantities the fitter uses.
static int t_collect(t_ref_data *rd, const ufbx_scene *scene, const double *times, int num_samples)
{
	ufbx_evaluate_opts eo = { 0 };
	eo.evaluate_skinning = true;
	ufbx_mesh *m0 = scene->meshes.data[0];
	rd->num_samples = num_samples;
	rd->num_vertices = (int)m0->num_vertices;
	rd->num_bones = (int)m0->skin_deformers.data[0]->clusters.count;

	for (int s = 0; s < num_samples; s++) {
		ufbx_error e;
		ufbx_scene *frame = ufbx_evaluate_scene(scene, NULL, times[s], &eo, &e);
		if (!frame) return 0;
		ufbx_mesh *m = frame->meshes.data[0];
		ufbx_skin_deformer *sk = m->skin_deformers.data[0];
		for (int c = 0; c < rd->num_bones; c++)
			rd->cluster_world[s][c] = t_from_ufbx(&sk->clusters.data[c]->geometry_to_world);
		for (int v = 0; v < rd->num_vertices; v++) {
			ufbx_vec3 local = m->vertices.data[v];
			for (size_t b = 0; b < m->blend_deformers.count; b++)
				ufbx_add_blend_vertex_offsets(m->blend_deformers.data[b], &local, 1, (ufbx_real)1.0);
			rd->morphed[s][v] = t_from_ufbx_v(local);
			rd->reference[s][v] = t_from_ufbx_v(m->skinned_position.values.data[v]);
		}
		ufbx_free_scene(frame);
	}
	return 1;
}

// Brute-force optimal weights for one vertex over its candidate bone set.
// Returns the minimum objective; weights are written to out_w[num_bones].
static double t_bruteforce_vertex(const t_ref_data *rd, int vertex, const int *candidates,
	int num_cand, int max_w, double *out_w)
{
	double best = INFINITY;
	for (int i = 0; i < num_cand; i++) out_w[i] = 0.0;

	for (int mask = 1; mask < (1 << num_cand); mask++) {
		int support[8], r = 0;
		for (int i = 0; i < num_cand; i++) if (mask & (1 << i)) support[r++] = i;
		if (r > max_w) continue;

		// Candidate weight vector for this support: explore the simplex face on
		// a fine grid plus its corners/edges, which is exact enough to identify
		// the global minimum support and objective for these small problems.
		// For r==1 the only feasible point is the single unit weight.
		if (r == 1) {
			double w[8] = { 0 }; w[0] = 1.0;
			double obj = 0.0;
			for (int s = 0; s < rd->num_samples; s++) {
				t_vec pred = t_xform(&rd->cluster_world[s][candidates[support[0]]], rd->morphed[s][vertex]);
				obj += t_dist(pred, rd->reference[s][vertex]) * t_dist(pred, rd->reference[s][vertex]);
			}
			if (obj < best) { best = obj; for (int i = 0; i < num_cand; i++) out_w[i] = 0.0; out_w[support[0]] = 1.0; }
			continue;
		}

		// Project reference onto the support columns with sum-to-one using a
		// small least squares normal-equation solve, then clamp to the simplex.
		// Pivot out the last support coordinate.
		int n = r - 1;
		double A[8][8] = {{0}}, rhs[8] = {0};
		for (int s = 0; s < rd->num_samples; s++) {
			t_vec cp = t_xform(&rd->cluster_world[s][candidates[support[r-1]]], rd->morphed[s][vertex]);
			t_vec d[8];
			for (int a = 0; a < n; a++) {
				t_vec ap = t_xform(&rd->cluster_world[s][candidates[support[a]]], rd->morphed[s][vertex]);
				d[a] = (t_vec){ ap.x-cp.x, ap.y-cp.y, ap.z-cp.z };
			}
			t_vec rr = { rd->reference[s][vertex].x-cp.x, rd->reference[s][vertex].y-cp.y,
				rd->reference[s][vertex].z-cp.z };
			for (int a = 0; a < n; a++) {
				rhs[a] += d[a].x*rr.x + d[a].y*rr.y + d[a].z*rr.z;
				for (int b = 0; b < n; b++)
					A[a][b] += d[a].x*d[b].x + d[a].y*d[b].y + d[a].z*d[b].z;
			}
		}
		// Gaussian elimination with pivoting on the n x n system A y = rhs.
		double M[8][9];
		for (int a = 0; a < n; a++) {
			for (int b = 0; b < n; b++) M[a][b] = A[a][b];
			M[a][n] = rhs[a];
		}
		int pivcol[8], rank = 0, row = 0;
		for (int col = 0; col < n && row < n; col++) {
			int sel = row;
			for (int a = row+1; a < n; a++) if (fabs(M[a][col]) > fabs(M[sel][col])) sel = a;
			if (fabs(M[sel][col]) < 1e-13) continue;
			for (int b = 0; b <= n; b++) { double tmp = M[row][b]; M[row][b] = M[sel][b]; M[sel][b] = tmp; }
			double inv = 1.0 / M[row][col];
			for (int b = 0; b <= n; b++) M[row][b] *= inv;
			for (int a = 0; a < n; a++) if (a != row) {
				double f = M[a][col];
				for (int b = 0; b <= n; b++) M[a][b] -= f * M[row][b];
			}
			pivcol[rank++] = col; row++;
		}
		double y[8] = { 0 };
		for (int i = 0; i < rank; i++) y[pivcol[i]] = M[i][n];
		double full[8] = { 0 };
		double sumy = 0; for (int a = 0; a < n; a++) sumy += y[a];
		for (int a = 0; a < n; a++) full[a] = y[a];
		full[r-1] = 1.0 - sumy;
		// If the unconstrained face point leaves the simplex it is not feasible;
		// the true constrained optimum then lies on a smaller face handled by
		// another mask, so skip this one.
		int feasible = 1;
		for (int a = 0; a < r; a++) if (full[a] < -1e-10) feasible = 0;
		if (!feasible) continue;
		double sum = 0;
		for (int a = 0; a < r; a++) { full[a] = full[a] > 0 ? full[a] : 0; sum += full[a]; }
		for (int a = 0; a < r; a++) full[a] /= sum;

		double obj = 0.0;
		for (int s = 0; s < rd->num_samples; s++) {
			t_vec pred = { 0, 0, 0 };
			for (int a = 0; a < r; a++) {
				t_vec col = t_xform(&rd->cluster_world[s][candidates[support[a]]], rd->morphed[s][vertex]);
				pred.x += full[a]*col.x; pred.y += full[a]*col.y; pred.z += full[a]*col.z;
			}
			double dd = t_dist(pred, rd->reference[s][vertex]);
			obj += dd*dd;
		}
		if (obj < best) {
			best = obj;
			for (int i = 0; i < num_cand; i++) out_w[i] = 0.0;
			for (int a = 0; a < r; a++) out_w[support[a]] = full[a];
		}
	}
	return best;
}

// ---------------------------------------------------------------------------
// Test cases
// ---------------------------------------------------------------------------

static const double t_times3[] = { 0.0, 0.5, 1.0 };
static const double t_times2[] = { 0.0, 1.0 };

static int opt_unequal_topk_not_optimal(void)
{
	// Four bones, six vertices; a vertex whose smallest-weight bone is needed
	// for the optimum, so taking the top-K by raw weight is suboptimal.
	t_scene_desc d;
	memset(&d, 0, sizeof d);
	d.num_bones = 4; d.num_vertices = 6;
	d.mesh_node_pos = (t_vec){ 0, 0, 0 };
	double bx[4] = { 0, 2, 4, 6 };
	int par[4] = { -1, -1, -1, -1 };
	for (int i = 0; i < 4; i++) {
		d.bones[i].parent = par[i];
		d.bones[i].bind_pos = (t_vec){ bx[i], 0, 0 };
		d.bones[i].bind = t_translation(bx[i], 0, 0);
		d.bones[i].rot0 = 0; d.bones[i].rot1 = 15.0 * (i + 1);
		d.bones[i].ty0 = d.bones[i].ty1 = 0;
	}
	// Vertex 4 gets four unequal weights where the lowest-ranked bone still
	// moves to a position the others cannot reproduce.
	double W[6][4] = {
		{ 0.70, 0.20, 0.10, 0.00 },
		{ 0.10, 0.60, 0.20, 0.10 },
		{ 0.05, 0.15, 0.50, 0.30 },
		{ 0.30, 0.10, 0.10, 0.50 },
		{ 0.55, 0.30, 0.10, 0.05 },
		{ 0.25, 0.25, 0.25, 0.25 },
	};
	for (int v = 0; v < 6; v++) {
		d.vertices[v] = (t_vec){ (double)v, 0, 0 };
		for (int c = 0; c < 4; c++) d.weights[v][c] = W[v][c];
	}

	ufbx_error error;
	ufbx_scene *scene = t_load(&d, &error);
	TASK_CHECK(scene != NULL);

	t_ref_data rd;
	TASK_CHECK(t_collect(&rd, scene, t_times3, 3));

	// Independent optimum for vertex 4 with max_weights = 2.
	int cand[4] = { 0, 1, 2, 3 };
	double ref_w[4];
	double ref_obj = t_bruteforce_vertex(&rd, 4, cand, 4, 2, ref_w);
	TASK_CHECK(isfinite(ref_obj));

	ufbx_skin_weight_fit_opts opts = { 0 };
	opts.sample_times = t_times3; opts.num_samples = 3;
	opts.max_weights = 2; opts.max_error = 1e9;
	ufbx_skin_weight_fit_report rep;
	ufbx_scene *fit = ufbx_fit_skin_weights(scene, NULL, 0, &opts, &rep, &error);
	TASK_CHECK(fit != NULL);
	TASK_CHECK(rep.max_error >= 0.0);

	// The fitted vertex 4 must use at most two weights, all non-negative and
	// summing to one.
	ufbx_skin_deformer *sk = fit->meshes.data[0]->skin_deformers.data[0];
	ufbx_skin_vertex sv4 = sk->vertices.data[4];
	TASK_CHECK(sv4.num_weights <= 2);
	double sum = 0; int neg = 0;
	for (uint32_t j = 0; j < sv4.num_weights; j++) {
		ufbx_skin_weight w = sk->weights.data[sv4.weight_begin + j];
		TASK_CHECK(w.weight > 0.0f);
		if (w.weight < 0.0f) neg = 1;
		sum += (double)w.weight;
	}
	TASK_CHECK(!neg);
	TASK_CHECK(fabs(sum - 1.0) < 1e-6);

	// The fitted objective (squared error over samples) must match the
	// independent brute-force optimum within the solver tolerance.
	double fit_obj = 0.0;
	for (int s = 0; s < 3; s++) {
		t_vec pred = { 0, 0, 0 };
		for (uint32_t j = 0; j < sv4.num_weights; j++) {
			ufbx_skin_weight w = sk->weights.data[sv4.weight_begin + j];
			t_vec col = t_xform(&rd.cluster_world[s][w.cluster_index], rd.morphed[s][4]);
			pred.x += (double)w.weight*col.x; pred.y += (double)w.weight*col.y; pred.z += (double)w.weight*col.z;
		}
		double dd = t_dist(pred, rd.reference[s][4]);
		fit_obj += dd*dd;
	}
	TASK_CHECK(fabs(fit_obj - ref_obj) <= 1e-9 * (1.0 + ref_obj));

	// Naive top-2 truncation of the original weights (re-normalized) must be
	// strictly worse than the global optimum, proving the fitter does not just
	// keep the largest raw weights.
	double topk_obj = 0.0;
	{
		// Original weights for vertex 4: {0.55,0.30,0.10,0.05}; top two are 0/1.
		double tw[2] = { 0.55, 0.30 };
		double tsum = tw[0] + tw[1];
		tw[0] /= tsum; tw[1] /= tsum;
		for (int s = 0; s < 3; s++) {
			t_vec pred = { 0, 0, 0 };
			for (int a = 0; a < 2; a++) {
				t_vec col = t_xform(&rd.cluster_world[s][a], rd.morphed[s][4]);
				pred.x += tw[a]*col.x; pred.y += tw[a]*col.y; pred.z += tw[a]*col.z;
			}
			double dd = t_dist(pred, rd.reference[s][4]);
			topk_obj += dd*dd;
		}
	}
	TASK_CHECK(topk_obj > fit_obj + 1e-9);

	ufbx_free_scene(fit);
	ufbx_free_scene(scene);
	return 0;
}

static int opt_multi_sample_support_switch(void)
{
	// Multiple samples force a different best support than any single pose.
	// Bones fan out over time so the optimal 2-bone choice changes; the fitter
	// must keep the support that is best over ALL samples together.
	t_scene_desc d;
	memset(&d, 0, sizeof d);
	d.num_bones = 3; d.num_vertices = 3;
	d.mesh_node_pos = (t_vec){ 0, 0, 0 };
	double bx[3] = { 0, 1, 2 };
	for (int i = 0; i < 3; i++) {
		d.bones[i].parent = -1;
		d.bones[i].bind_pos = (t_vec){ bx[i], 0, 0 };
		d.bones[i].bind = t_translation(bx[i], 0, 0);
		d.bones[i].rot0 = 0;
		d.bones[i].rot1 = (i == 0) ? 60.0 : (i == 1 ? -40.0 : 20.0);
		d.bones[i].ty0 = d.bones[i].ty1 = 0;
	}
	double W[3][3] = {
		{ 0.5, 0.3, 0.2 },
		{ 0.2, 0.5, 0.3 },
		{ 0.34, 0.33, 0.33 },
	};
	for (int v = 0; v < 3; v++) {
		d.vertices[v] = (t_vec){ (double)v, 0, 0 };
		for (int c = 0; c < 3; c++) d.weights[v][c] = W[v][c];
	}

	ufbx_error error;
	ufbx_scene *scene = t_load(&d, &error);
	TASK_CHECK(scene != NULL);
	t_ref_data rd;
	TASK_CHECK(t_collect(&rd, scene, t_times3, 3));

	int cand[3] = { 0, 1, 2 };
	double ref_w[3];
	double ref_obj = t_bruteforce_vertex(&rd, 2, cand, 3, 1, ref_w);

	ufbx_skin_weight_fit_opts opts = { 0 };
	opts.sample_times = t_times3; opts.num_samples = 3;
	opts.max_weights = 1; opts.max_error = 1e9;
	ufbx_skin_weight_fit_report rep;
	ufbx_scene *fit = ufbx_fit_skin_weights(scene, NULL, 0, &opts, &rep, &error);
	TASK_CHECK(fit != NULL);

	ufbx_skin_deformer *sk = fit->meshes.data[0]->skin_deformers.data[0];
	ufbx_skin_vertex sv = sk->vertices.data[2];
	TASK_CHECK(sv.num_weights == 1);
	double fit_obj = 0.0;
	for (int s = 0; s < 3; s++) {
		ufbx_skin_weight w = sk->weights.data[sv.weight_begin];
		t_vec col = t_xform(&rd.cluster_world[s][w.cluster_index], rd.morphed[s][2]);
		double dd = t_dist(col, rd.reference[s][2]);
		fit_obj += dd*dd;
	}
	TASK_CHECK(fabs(fit_obj - ref_obj) <= 1e-9 * (1.0 + ref_obj));

	ufbx_free_scene(fit);
	ufbx_free_scene(scene);
	return 0;
}

static int opt_duplicate_columns(void)
{
	// Two bones with identical bind and identical motion produce duplicate
	// cluster columns; the solver must remain stable and still reach the optimum.
	t_scene_desc d;
	memset(&d, 0, sizeof d);
	d.num_bones = 3; d.num_vertices = 3;
	d.mesh_node_pos = (t_vec){ 0, 0, 0 };
	double bx[3] = { 0, 1, 1 }; // bones 1 and 2 share the same origin
	for (int i = 0; i < 3; i++) {
		d.bones[i].parent = -1;
		d.bones[i].bind_pos = (t_vec){ bx[i], 0, 0 };
		d.bones[i].bind = t_translation(bx[i], 0, 0);
		d.bones[i].rot0 = 0;
		// Bones 1 and 2 rotate identically -> duplicate trajectories.
		d.bones[i].rot1 = (i == 0) ? 25.0 : 40.0;
		d.bones[i].ty0 = d.bones[i].ty1 = 0;
	}
	double W[3][3] = {
		{ 0.6, 0.2, 0.2 },
		{ 0.2, 0.5, 0.3 },
		{ 0.1, 0.45, 0.45 },
	};
	for (int v = 0; v < 3; v++) {
		d.vertices[v] = (t_vec){ (double)v, 0, 0 };
		for (int c = 0; c < 3; c++) d.weights[v][c] = W[v][c];
	}

	ufbx_error error;
	ufbx_scene *scene = t_load(&d, &error);
	TASK_CHECK(scene != NULL);
	t_ref_data rd;
	TASK_CHECK(t_collect(&rd, scene, t_times2, 2));

	int cand[3] = { 0, 1, 2 };
	double ref_w[3];
	double ref_obj = t_bruteforce_vertex(&rd, 2, cand, 3, 2, ref_w);

	ufbx_skin_weight_fit_opts opts = { 0 };
	opts.sample_times = t_times2; opts.num_samples = 2;
	opts.max_weights = 2; opts.max_error = 1e9;
	ufbx_skin_weight_fit_report rep;
	ufbx_scene *fit = ufbx_fit_skin_weights(scene, NULL, 0, &opts, &rep, &error);
	TASK_CHECK(fit != NULL);
	TASK_CHECK(isfinite(rep.max_error));

	ufbx_skin_deformer *sk = fit->meshes.data[0]->skin_deformers.data[0];
	double fit_obj = 0.0;
	for (int s = 0; s < 2; s++) {
		t_vec pred = { 0, 0, 0 };
		ufbx_skin_vertex sv = sk->vertices.data[2];
		for (uint32_t j = 0; j < sv.num_weights; j++) {
			ufbx_skin_weight w = sk->weights.data[sv.weight_begin + j];
			t_vec col = t_xform(&rd.cluster_world[s][w.cluster_index], rd.morphed[s][2]);
			pred.x += (double)w.weight*col.x; pred.y += (double)w.weight*col.y; pred.z += (double)w.weight*col.z;
		}
		double dd = t_dist(pred, rd.reference[s][2]);
		fit_obj += dd*dd;
	}
	TASK_CHECK(fabs(fit_obj - ref_obj) <= 1e-9 * (1.0 + ref_obj));

	ufbx_free_scene(fit);
	ufbx_free_scene(scene);
	return 0;
}

static int opt_boundary_zero_weight(void)
{
	// A vertex with a genuine zero weight keeps that zero influence out of the
	// fitted tables while the rest are normalized.
	t_scene_desc d;
	memset(&d, 0, sizeof d);
	d.num_bones = 3; d.num_vertices = 2;
	d.mesh_node_pos = (t_vec){ 0, 0, 0 };
	double bx[3] = { 0, 1, 2 };
	for (int i = 0; i < 3; i++) {
		d.bones[i].parent = -1;
		d.bones[i].bind_pos = (t_vec){ bx[i], 0, 0 };
		d.bones[i].bind = t_translation(bx[i], 0, 0);
		d.bones[i].rot0 = 0; d.bones[i].rot1 = 20.0;
		d.bones[i].ty0 = d.bones[i].ty1 = 0;
	}
	// Vertex 0 has an explicit zero on bone 2 and only two positive weights.
	d.vertices[0] = (t_vec){ 0, 0, 0 };
	d.weights[0][0] = 0.7; d.weights[0][1] = 0.3; d.weights[0][2] = 0.0;
	d.vertices[1] = (t_vec){ 1, 0, 0 };
	d.weights[1][0] = 0.2; d.weights[1][1] = 0.3; d.weights[1][2] = 0.5;

	ufbx_error error;
	ufbx_scene *scene = t_load(&d, &error);
	TASK_CHECK(scene != NULL);

	ufbx_skin_weight_fit_opts opts = { 0 };
	opts.sample_times = t_times2; opts.num_samples = 2;
	opts.max_weights = 4; opts.max_error = 1e9;
	ufbx_skin_weight_fit_report rep;
	ufbx_scene *fit = ufbx_fit_skin_weights(scene, NULL, 0, &opts, &rep, &error);
	TASK_CHECK(fit != NULL);
	ufbx_skin_deformer *sk = fit->meshes.data[0]->skin_deformers.data[0];
	ufbx_skin_vertex sv0 = sk->vertices.data[0];
	TASK_CHECK(sv0.num_weights == 2);
	int saw_zero_cluster = 0;
	double sum = 0;
	for (uint32_t j = 0; j < sv0.num_weights; j++) {
		ufbx_skin_weight w = sk->weights.data[sv0.weight_begin + j];
		TASK_CHECK(w.weight > 0.0f);
		if (w.cluster_index == 2) saw_zero_cluster = 1;
		sum += (double)w.weight;
	}
	TASK_CHECK(!saw_zero_cluster);
	TASK_CHECK(fabs(sum - 1.0) < 1e-6);

	ufbx_free_scene(fit);
	ufbx_free_scene(scene);
	return 0;
}

// ---------------------------------------------------------------------------
// scene_binding group
// ---------------------------------------------------------------------------

static int bind_real_load_evaluate_morph(void)
{
	// A real loaded scene (through ufbx_load_memory) with a non-identity inverse
	// bind matrix, non-commuting bone hierarchy poses and an animated morph.
	t_scene_desc d;
	memset(&d, 0, sizeof d);
	d.num_bones = 3; d.num_vertices = 6;
	d.has_morph = 1;
	d.mesh_node_pos = (t_vec){ 3.0, -2.0, 1.0 }; // non-identity instance transform
	d.morph.morph_vertex = 0;
	d.morph.morph_offset = (t_vec){ 0.0, 1.0, 0.0 };
	d.morph.morph0 = 0; d.morph.morph1 = 100;

	// Non-commuting chain: B0 at origin rotated, B1 child of B0, B2 sibling.
	d.bones[0].parent = -1; d.bones[0].bind_pos = (t_vec){ 0, 0, 0 };
	d.bones[0].bind = t_rotz(45.0);
	d.bones[0].rot0 = 45.0; d.bones[0].rot1 = 90.0;
	d.bones[1].parent = 0; d.bones[1].bind_pos = (t_vec){ 2, 0, 0 };
	t_mat r45 = t_rotz(45.0), tr2 = t_translation(2, 0, 0);
	d.bones[1].bind = t_mul(&r45, &tr2);
	d.bones[1].rot0 = 0.0; d.bones[1].rot1 = 35.0;
	d.bones[2].parent = -1; d.bones[2].bind_pos = (t_vec){ 4, 0, 0 };
	d.bones[2].bind = t_translation(4, 1, 0);
	d.bones[2].rot0 = 0.0; d.bones[2].rot1 = -25.0;
	for (int i = 0; i < 3; i++) { d.bones[i].ty0 = 0; d.bones[i].ty1 = 0; }

	double W[6][3] = {
		{ 0.7, 0.2, 0.1 }, { 0.6, 0.3, 0.1 }, { 0.4, 0.4, 0.2 },
		{ 0.2, 0.4, 0.4 }, { 0.1, 0.3, 0.6 }, { 0.1, 0.2, 0.7 },
	};
	for (int v = 0; v < 6; v++) {
		d.vertices[v] = (t_vec){ (double)v, 0, 0 };
		for (int c = 0; c < 3; c++) d.weights[v][c] = W[v][c];
	}

	ufbx_error error;
	ufbx_scene *scene = t_load(&d, &error);
	TASK_CHECK(scene != NULL);
	TASK_CHECK(scene->meshes.data[0]->blend_deformers.count == 1);

	// Non-identity inverse bind present and finite.
	ufbx_skin_cluster *cl0 = scene->meshes.data[0]->skin_deformers.data[0]->clusters.data[0];
	int non_identity = 0;
	for (int i = 0; i < 12; i++) if (fabs((double)cl0->geometry_to_bone.v[i] - (i % 4 == 0 ? 1.0 : 0.0)) > 1e-9) non_identity = 1;
	TASK_CHECK(non_identity);

	ufbx_skin_weight_fit_opts opts = { 0 };
	opts.sample_times = t_times3; opts.num_samples = 3;
	opts.max_weights = 2; opts.max_error = 1e9;
	ufbx_skin_weight_fit_report rep;
	ufbx_scene *fit = ufbx_fit_skin_weights(scene, NULL, 0, &opts, &rep, &error);
	TASK_CHECK(fit != NULL);
	TASK_CHECK(isfinite(rep.max_error));

	// Re-evaluate the fitted scene at the sample times and confirm its skinned
	// output stays close to the source scene's skinned output.
	ufbx_evaluate_opts eo = { 0 }; eo.evaluate_skinning = true;
	for (int s = 0; s < 3; s++) {
		ufbx_scene *a = ufbx_evaluate_scene(scene, NULL, t_times3[s], &eo, &error);
		ufbx_scene *b = ufbx_evaluate_scene(fit, NULL, t_times3[s], &eo, &error);
		TASK_CHECK(a && b);
		ufbx_mesh *ma = a->meshes.data[0], *mb = b->meshes.data[0];
		for (size_t v = 0; v < ma->num_vertices; v++) {
			double dd = t_dist(t_from_ufbx_v(ma->skinned_position.values.data[v]),
				t_from_ufbx_v(mb->skinned_position.values.data[v]));
			TASK_CHECK(dd <= rep.max_error + 1e-5);
		}
		ufbx_free_scene(a); ufbx_free_scene(b);
	}

	ufbx_free_scene(fit);
	ufbx_free_scene(scene);
	return 0;
}

static int bind_cluster_reverse_tables(void)
{
	// Every cluster's vertices/weights reverse table must be rebuilt consistently
	// with the per-vertex tables and keep the cluster order and bind matrices.
	t_scene_desc d;
	memset(&d, 0, sizeof d);
	d.num_bones = 4; d.num_vertices = 6;
	d.mesh_node_pos = (t_vec){ 0, 0, 0 };
	double bx[4] = { 0, 1, 2, 3 };
	for (int i = 0; i < 4; i++) {
		d.bones[i].parent = -1;
		d.bones[i].bind_pos = (t_vec){ bx[i], 0, 0 };
		t_mat ri = t_rotz(10.0 * i), tri = t_translation(bx[i], 0, 0);
		d.bones[i].bind = t_mul(&ri, &tri);
		d.bones[i].rot0 = 10.0*i; d.bones[i].rot1 = 10.0*i + 12.0;
		d.bones[i].ty0 = d.bones[i].ty1 = 0;
	}
	double W[6][4] = {
		{ 0.7,0.2,0.1,0 }, { 0.1,0.6,0.2,0.1 }, { 0,0.1,0.7,0.2 },
		{ 0.2,0.1,0.1,0.6 }, { 0.3,0.3,0.2,0.2 }, { 0.4,0.3,0.2,0.1 },
	};
	for (int v = 0; v < 6; v++) {
		d.vertices[v] = (t_vec){ (double)v, 0, 0 };
		for (int c = 0; c < 4; c++) d.weights[v][c] = W[v][c];
	}

	ufbx_error error;
	ufbx_scene *scene = t_load(&d, &error);
	TASK_CHECK(scene != NULL);
	ufbx_matrix orig_bind[4];
	ufbx_skin_deformer *src_sk = scene->meshes.data[0]->skin_deformers.data[0];
	for (int c = 0; c < 4; c++) orig_bind[c] = src_sk->clusters.data[c]->geometry_to_bone;

	ufbx_skin_weight_fit_opts opts = { 0 };
	opts.sample_times = t_times3; opts.num_samples = 3;
	opts.max_weights = 2; opts.max_error = 1e9;
	ufbx_skin_weight_fit_report rep;
	ufbx_scene *fit = ufbx_fit_skin_weights(scene, NULL, 0, &opts, &rep, &error);
	TASK_CHECK(fit != NULL);

	ufbx_mesh *m = fit->meshes.data[0];
	ufbx_skin_deformer *sk = m->skin_deformers.data[0];
	TASK_CHECK(sk->clusters.count == 4);

	// Cross-check the reverse tables against the per-vertex tables.
	for (size_t c = 0; c < sk->clusters.count; c++) {
		ufbx_skin_cluster *cluster = sk->clusters.data[c];
		TASK_CHECK(cluster->vertices.count == cluster->weights.count);
		TASK_CHECK(cluster->num_weights == cluster->vertices.count);
		// Bind matrices preserved.
		int bind_same = 1;
		for (int k = 0; k < 12; k++)
			if ((double)cluster->geometry_to_bone.v[k] != (double)orig_bind[c].v[k]) bind_same = 0;
		TASK_CHECK(bind_same);
		for (size_t k = 0; k < cluster->vertices.count; k++) {
			uint32_t vx = cluster->vertices.data[k];
			ufbx_real rw = cluster->weights.data[k];
			ufbx_skin_vertex sv = sk->vertices.data[vx];
			int found = 0;
			for (uint32_t j = 0; j < sv.num_weights; j++) {
				ufbx_skin_weight w = sk->weights.data[sv.weight_begin + j];
				if (w.cluster_index == c) {
					TASK_CHECK((double)w.weight == (double)rw);
					found = 1;
				}
			}
			TASK_CHECK(found);
		}
	}
	// Total reverse-table entries equal total positive weights.
	size_t rev_total = 0;
	for (size_t c = 0; c < sk->clusters.count; c++) rev_total += sk->clusters.data[c]->num_weights;
	TASK_CHECK(rev_total == rep.total_weights);
	TASK_CHECK(sk->max_weights_per_vertex <= 2);

	ufbx_free_scene(fit);
	ufbx_free_scene(scene);
	return 0;
}

static int bind_pose_time_zero(void)
{
	// The returned scene's initial pose matches an evaluate_scene(source, 0).
	t_scene_desc d;
	memset(&d, 0, sizeof d);
	d.num_bones = 2; d.num_vertices = 3;
	d.mesh_node_pos = (t_vec){ 1, 1, 0 };
	d.bones[0].parent = -1; d.bones[0].bind_pos = (t_vec){ 0,0,0 };
	d.bones[0].bind = t_identity(); d.bones[0].rot0 = 10; d.bones[0].rot1 = 80;
	d.bones[1].parent = 0; d.bones[1].bind_pos = (t_vec){ 2,0,0 };
	d.bones[1].bind = t_translation(2,0,0); d.bones[1].rot0 = 0; d.bones[1].rot1 = 40;
	d.bones[0].ty0 = d.bones[0].ty1 = d.bones[1].ty0 = d.bones[1].ty1 = 0;
	double W[3][2] = { {0.8,0.2},{0.5,0.5},{0.2,0.8} };
	for (int v = 0; v < 3; v++) {
		d.vertices[v] = (t_vec){ (double)v,0,0 };
		d.weights[v][0] = W[v][0]; d.weights[v][1] = W[v][1];
	}

	ufbx_error error;
	ufbx_scene *scene = t_load(&d, &error);
	TASK_CHECK(scene != NULL);
	ufbx_evaluate_opts eo = { 0 }; eo.evaluate_skinning = true;
	ufbx_scene *zero = ufbx_evaluate_scene(scene, NULL, 0.0, &eo, &error);
	TASK_CHECK(zero != NULL);

	ufbx_skin_weight_fit_opts opts = { 0 };
	opts.sample_times = t_times2; opts.num_samples = 2;
	opts.max_weights = 1; opts.max_error = 1e9;
	ufbx_skin_weight_fit_report rep;
	ufbx_scene *fit = ufbx_fit_skin_weights(scene, NULL, 0, &opts, &rep, &error);
	TASK_CHECK(fit != NULL);

	// Node transforms at time zero match the source time-zero pose.
	for (size_t i = 0; i < scene->nodes.count; i++) {
		ufbx_node *sn = scene->nodes.data[i];
		ufbx_node *fn = fit->nodes.data[i];
		for (int k = 0; k < 12; k++)
			TASK_CHECK((double)fn->node_to_world.v[k] == (double)zero->nodes.data[i]->node_to_world.v[k]);
		(void)sn;
	}
	ufbx_free_scene(zero);
	ufbx_free_scene(fit);
	ufbx_free_scene(scene);
	return 0;
}

static int bind_unsupported_and_invalid(void)
{
	t_scene_desc d;
	memset(&d, 0, sizeof d);
	d.num_bones = 2; d.num_vertices = 3;
	d.mesh_node_pos = (t_vec){ 0,0,0 };
	for (int i = 0; i < 2; i++) {
		d.bones[i].parent = -1; d.bones[i].bind_pos = (t_vec){ (double)i,0,0 };
		d.bones[i].bind = t_translation((double)i,0,0);
		d.bones[i].rot0 = 0; d.bones[i].rot1 = 30; d.bones[i].ty0 = d.bones[i].ty1 = 0;
	}
	double W[3][2] = { {1,0},{0.5,0.5},{0,1} };
	for (int v = 0; v < 3; v++) {
		d.vertices[v] = (t_vec){ (double)v,0,0 };
		d.weights[v][0] = W[v][0]; d.weights[v][1] = W[v][1];
	}

	// Dual quaternion skin is rejected.
	d.dual_quaternion = 1;
	ufbx_error error;
	ufbx_scene *dq = t_load(&d, &error);
	TASK_CHECK(dq != NULL);
	TASK_CHECK(dq->meshes.data[0]->skin_deformers.data[0]->skinning_method == UFBX_SKINNING_METHOD_DUAL_QUATERNION);
	ufbx_skin_weight_fit_opts opts = { 0 };
	opts.sample_times = t_times2; opts.num_samples = 2;
	opts.max_weights = 1; opts.max_error = 1e9;
	ufbx_skin_weight_fit_report rep;
	TASK_CHECK(ufbx_fit_skin_weights(dq, NULL, 0, &opts, &rep, &error) == NULL);
	ufbx_free_scene(dq);
	d.dual_quaternion = 0;

	ufbx_scene *scene = t_load(&d, &error);
	TASK_CHECK(scene != NULL);

	// Bad mesh typed id.
	TASK_CHECK(ufbx_fit_skin_weights(scene, NULL, 999, &opts, &rep, &error) == NULL);
	// Bad sample counts and ordering.
	ufbx_skin_weight_fit_opts bad = opts;
	double bad_times[65]; for (int i = 0; i < 65; i++) bad_times[i] = (double)i;
	bad.sample_times = bad_times; bad.num_samples = 65;
	TASK_CHECK(ufbx_fit_skin_weights(scene, NULL, 0, &bad, &rep, &error) == NULL);
	bad.num_samples = 2; bad.sample_times = (double[]){ 1.0, 1.0 };
	TASK_CHECK(ufbx_fit_skin_weights(scene, NULL, 0, &bad, &rep, &error) == NULL);
	bad.sample_times = (double[]){ 0.0, NAN };
	TASK_CHECK(ufbx_fit_skin_weights(scene, NULL, 0, &bad, &rep, &error) == NULL);
	bad.sample_times = t_times2; bad.num_samples = 2; bad.max_weights = 0;
	TASK_CHECK(ufbx_fit_skin_weights(scene, NULL, 0, &bad, &rep, &error) == NULL);
	bad.max_weights = 5;
	TASK_CHECK(ufbx_fit_skin_weights(scene, NULL, 0, &bad, &rep, &error) == NULL);
	bad.max_weights = 1; bad.max_error = -1.0;
	TASK_CHECK(ufbx_fit_skin_weights(scene, NULL, 0, &bad, &rep, &error) == NULL);
	bad.max_error = 1e9; bad.sample_times = NULL; bad.num_samples = 1;
	TASK_CHECK(ufbx_fit_skin_weights(scene, NULL, 0, &bad, &rep, &error) == NULL);

	ufbx_free_scene(scene);
	return 0;
}

// ---------------------------------------------------------------------------
// ownership group
// ---------------------------------------------------------------------------

static int own_source_unchanged_on_failure(void)
{
	t_scene_desc d;
	memset(&d, 0, sizeof d);
	d.num_bones = 2; d.num_vertices = 3;
	d.mesh_node_pos = (t_vec){ 0,0,0 };
	for (int i = 0; i < 2; i++) {
		d.bones[i].parent = -1; d.bones[i].bind_pos = (t_vec){ (double)i,0,0 };
		d.bones[i].bind = t_translation((double)i,0,0);
		d.bones[i].rot0 = 0; d.bones[i].rot1 = 60; d.bones[i].ty0 = d.bones[i].ty1 = 0;
	}
	double W[3][2] = { {1,0},{0.5,0.5},{0,1} };
	for (int v = 0; v < 3; v++) {
		d.vertices[v] = (t_vec){ (double)v,0,0 };
		d.weights[v][0] = W[v][0]; d.weights[v][1] = W[v][1];
	}
	ufbx_error error;
	ufbx_scene *scene = t_load(&d, &error);
	TASK_CHECK(scene != NULL);
	ufbx_skin_deformer *sk = scene->meshes.data[0]->skin_deformers.data[0];
	size_t orig_count = sk->weights.count;
	ufbx_skin_weight orig_first = sk->weights.data[0];

	ufbx_skin_weight_fit_opts opts = { 0 };
	opts.sample_times = t_times2; opts.num_samples = 2;
	opts.max_weights = 1; opts.max_error = 0.0; // impossible for mixed vertices
	ufbx_skin_weight_fit_report rep;
	memset(&rep, 0xAB, sizeof rep);
	ufbx_scene *failed = ufbx_fit_skin_weights(scene, NULL, 0, &opts, &rep, &error);
	TASK_CHECK(failed == NULL);
	// Report untouched on failure (still filled with the sentinel pattern).
	TASK_CHECK(rep.worst_vertex == (size_t)0xABABABABABABABABULL);
	// Source tables untouched.
	TASK_CHECK(scene->meshes.data[0]->skin_deformers.data[0]->weights.count == orig_count);
	TASK_CHECK(scene->meshes.data[0]->skin_deformers.data[0]->weights.data[0].cluster_index == orig_first.cluster_index);
	TASK_CHECK((double)scene->meshes.data[0]->skin_deformers.data[0]->weights.data[0].weight == (double)orig_first.weight);

	ufbx_free_scene(scene);
	return 0;
}

static int own_independent_results_and_source_free(void)
{
	t_scene_desc d;
	memset(&d, 0, sizeof d);
	d.num_bones = 3; d.num_vertices = 4;
	d.mesh_node_pos = (t_vec){ 0,0,0 };
	for (int i = 0; i < 3; i++) {
		d.bones[i].parent = -1; d.bones[i].bind_pos = (t_vec){ (double)i,0,0 };
		d.bones[i].bind = t_translation((double)i,0,0);
		d.bones[i].rot0 = 0; d.bones[i].rot1 = 20.0*(i+1); d.bones[i].ty0 = d.bones[i].ty1 = 0;
	}
	double W[4][3] = { {0.6,0.3,0.1},{0.2,0.6,0.2},{0.1,0.3,0.6},{0.34,0.33,0.33} };
	for (int v = 0; v < 4; v++) {
		d.vertices[v] = (t_vec){ (double)v,0,0 };
		for (int c = 0; c < 3; c++) d.weights[v][c] = W[v][c];
	}
	ufbx_error error;
	ufbx_scene *scene = t_load(&d, &error);
	TASK_CHECK(scene != NULL);

	ufbx_skin_weight_fit_opts opts = { 0 };
	opts.sample_times = t_times3; opts.num_samples = 3;
	opts.max_weights = 2; opts.max_error = 1e9;
	ufbx_skin_weight_fit_report r1, r2;
	ufbx_scene *f1 = ufbx_fit_skin_weights(scene, NULL, 0, &opts, &r1, &error);
	ufbx_scene *f2 = ufbx_fit_skin_weights(scene, NULL, 0, &opts, &r2, &error);
	TASK_CHECK(f1 && f2);
	TASK_CHECK(f1 != f2);

	// Mutating one result must not affect the other.
	ufbx_skin_deformer *s1 = f1->meshes.data[0]->skin_deformers.data[0];
	ufbx_skin_deformer *s2 = f2->meshes.data[0]->skin_deformers.data[0];
	TASK_CHECK(s1->weights.data != s2->weights.data);
	ufbx_real before = s2->weights.data[0].weight;
	s1->weights.data[0].weight = (ufbx_real)0.12345;
	TASK_CHECK((double)s2->weights.data[0].weight == (double)before);

	// Freeing the source scene must leave each result independently animatable.
	ufbx_free_scene(scene);
	ufbx_evaluate_opts eo = { 0 }; eo.evaluate_skinning = true;
	ufbx_scene *e1 = ufbx_evaluate_scene(f1, NULL, 0.5, &eo, &error);
	ufbx_scene *e2 = ufbx_evaluate_scene(f2, NULL, 0.5, &eo, &error);
	TASK_CHECK(e1 && e2);
	TASK_CHECK(e1->meshes.data[0]->skinned_position.values.count == 4);
	TASK_CHECK(e2->meshes.data[0]->skinned_position.values.count == 4);
	ufbx_free_scene(e1); ufbx_free_scene(e2);

	// sample_times array need not outlive the call.
	ufbx_free_scene(f1);
	ufbx_free_scene(f2);
	return 0;
}

static int own_repeated_unordered_samples(void)
{
	// Validation rejects non-increasing times; the same scene fitted twice with
	// different sample sets gives independent, consistent results.
	t_scene_desc d;
	memset(&d, 0, sizeof d);
	d.num_bones = 2; d.num_vertices = 3;
	d.mesh_node_pos = (t_vec){ 0,0,0 };
	for (int i = 0; i < 2; i++) {
		d.bones[i].parent = -1; d.bones[i].bind_pos = (t_vec){ (double)i,0,0 };
		d.bones[i].bind = t_translation((double)i,0,0);
		d.bones[i].rot0 = 0; d.bones[i].rot1 = 45; d.bones[i].ty0 = d.bones[i].ty1 = 0;
	}
	double W[3][2] = { {0.9,0.1},{0.5,0.5},{0.1,0.9} };
	for (int v = 0; v < 3; v++) {
		d.vertices[v] = (t_vec){ (double)v,0,0 };
		d.weights[v][0] = W[v][0]; d.weights[v][1] = W[v][1];
	}
	ufbx_error error;
	ufbx_scene *scene = t_load(&d, &error);
	TASK_CHECK(scene != NULL);

	ufbx_skin_weight_fit_opts o1 = { 0 };
	double t1[2] = { 0.0, 0.5 };
	o1.sample_times = t1; o1.num_samples = 2; o1.max_weights = 1; o1.max_error = 1e9;
	ufbx_skin_weight_fit_report rep1;
	ufbx_scene *f1 = ufbx_fit_skin_weights(scene, NULL, 0, &o1, &rep1, &error);
	TASK_CHECK(f1 != NULL);

	// A second call using a different (still valid) sample set works and the
	// first result is unaffected.
	ufbx_skin_weight_fit_opts o2 = o1;
	double t2[3] = { 0.0, 0.25, 0.75 };
	o2.sample_times = t2; o2.num_samples = 3;
	ufbx_skin_weight_fit_report rep2;
	ufbx_scene *f2 = ufbx_fit_skin_weights(scene, NULL, 0, &o2, &rep2, &error);
	TASK_CHECK(f2 != NULL);
	TASK_CHECK(rep1.total_weights == rep2.total_weights);
	ufbx_free_scene(f1);
	ufbx_free_scene(f2);

	// Out-of-order times are rejected.
	ufbx_skin_weight_fit_opts bad = o1;
	double tb[2] = { 0.5, 0.0 };
	bad.sample_times = tb;
	ufbx_skin_weight_fit_report rep;
	TASK_CHECK(ufbx_fit_skin_weights(scene, NULL, 0, &bad, &rep, &error) == NULL);

	ufbx_free_scene(scene);
	return 0;
}

// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------

const struct task_case task_cases[] = {
	{ "optimization", "unequal_topk_not_optimal", opt_unequal_topk_not_optimal },
	{ "optimization", "multi_sample_support_switch", opt_multi_sample_support_switch },
	{ "optimization", "duplicate_columns", opt_duplicate_columns },
	{ "optimization", "boundary_zero_weight", opt_boundary_zero_weight },

	{ "scene_binding", "real_load_evaluate_morph", bind_real_load_evaluate_morph },
	{ "scene_binding", "cluster_reverse_tables", bind_cluster_reverse_tables },
	{ "scene_binding", "pose_time_zero", bind_pose_time_zero },
	{ "scene_binding", "unsupported_and_invalid", bind_unsupported_and_invalid },

	{ "ownership", "source_unchanged_on_failure", own_source_unchanged_on_failure },
	{ "ownership", "independent_results_and_source_free", own_independent_results_and_source_free },
	{ "ownership", "repeated_unordered_samples", own_repeated_unordered_samples },
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
