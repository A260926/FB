/*
 * root_motion_test.c - tests for examples/cgltf_motion.{h,c}.
 *
 * The tests build cgltf_data in memory (float accessors backed by one
 * malloc'd buffer) and also play the real checks/motion.gltf document.
 * An independent reference implementation below re-derives every sampled
 * world matrix and the accumulated root-motion formula, so assertions
 * compare numbers instead of merely checking finiteness.
 *
 * cgltf's implementation section has no include guard of its own, so this
 * TU pulls in cgltf exactly once, through cgltf_motion.h.
 *
 * Build (see checks/check-root-motion.sh):
 *   cc -std=c99 -I. test/root_motion_test.c examples/cgltf_motion.c -lm
 */
#define CGLTF_IMPLEMENTATION
#include "examples/cgltf_motion.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TEST_EPS 1e-5f

/* ------------------------------------------------------------------ */
/* tiny test harness                                                   */
/* ------------------------------------------------------------------ */

static int g_asserts_in_case = 0;
static int g_cases = 0;
static int g_failures = 0;

#define CHECK(cond)                                                      \
	do {                                                             \
		g_asserts_in_case++;                                     \
		if (!(cond)) {                                           \
			g_failures++;                                    \
			printf("FAIL %s:%d: %s\n", __FILE__, __LINE__,   \
					#cond);                            \
		}                                                        \
	} while (0)

static void begin_case(void)
{
	g_asserts_in_case = 0;
}

static void end_case(const char* name)
{
	if (g_asserts_in_case == 0)
	{
		printf("FAIL case %s ran no assertions\n", name);
		g_failures++;
	}
	else
	{
		g_cases++;
	}
}

static float max_abs_diff(const float* a, const float* b)
{
	float d = 0.f;
	for (int i = 0; i < 16; ++i)
	{
		float x = fabsf(a[i] - b[i]);
		if (x > d)
			d = x;
	}
	return d;
}

static void check_matrix(const char* tag, const float* got, const float* want)
{
	float d = max_abs_diff(got, want);
	if (d > TEST_EPS)
	{
		g_asserts_in_case++;
		g_failures++;
		printf("FAIL matrix %s max diff %.8g\n", tag, d);
	}
	else
	{
		g_asserts_in_case++;
	}
}

static void check_scalar(const char* tag, float got, float want)
{
	if (fabsf(got - want) > TEST_EPS)
	{
		g_asserts_in_case++;
		g_failures++;
		printf("FAIL scalar %s got %.8g want %.8g\n", tag, got, want);
	}
	else
	{
		g_asserts_in_case++;
	}
}

/* ------------------------------------------------------------------ */
/* matrix / quaternion reference math (column major)                   */
/* ------------------------------------------------------------------ */

static void ref_identity(float* m)
{
	memset(m, 0, 16 * sizeof(float));
	m[0] = m[5] = m[10] = m[15] = 1.f;
}

static void ref_mul(float* out, const float* a, const float* b)
{
	float t[16];
	for (int c = 0; c < 4; ++c)
		for (int r = 0; r < 4; ++r)
			t[c * 4 + r] = a[r] * b[c * 4] + a[4 + r] * b[c * 4 + 1] +
				a[8 + r] * b[c * 4 + 2] + a[12 + r] * b[c * 4 + 3];
	memcpy(out, t, sizeof(t));
}

static int ref_inverse(float* out, const float* in)
{
	float s[6];
	s[0] = in[0] * in[5] - in[1] * in[4];
	s[1] = in[0] * in[6] - in[2] * in[4];
	s[2] = in[0] * in[7] - in[3] * in[4];
	s[3] = in[1] * in[6] - in[2] * in[5];
	s[4] = in[1] * in[7] - in[3] * in[5];
	s[5] = in[2] * in[7] - in[3] * in[6];

	float c[6];
	c[0] = in[8] * in[13] - in[9] * in[12];
	c[1] = in[8] * in[14] - in[10] * in[12];
	c[2] = in[8] * in[15] - in[11] * in[12];
	c[3] = in[9] * in[14] - in[10] * in[13];
	c[4] = in[9] * in[15] - in[11] * in[13];
	c[5] = in[10] * in[15] - in[11] * in[14];

	float det = s[0] * c[5] - s[1] * c[4] + s[2] * c[3] +
		s[3] * c[2] - s[4] * c[1] + s[5] * c[0];
	if (fabsf(det) < 1e-20f)
		return 0;
	det = 1.f / det;

	out[0] = ( in[5] * c[5] - in[6] * c[4] + in[7] * c[3]) * det;
	out[1] = (-in[1] * c[5] + in[2] * c[4] - in[3] * c[3]) * det;
	out[2] = ( in[13] * s[5] - in[14] * s[4] + in[15] * s[3]) * det;
	out[3] = (-in[9] * s[5] + in[10] * s[4] - in[11] * s[3]) * det;
	out[4] = (-in[4] * c[5] + in[6] * c[2] - in[7] * c[1]) * det;
	out[5] = ( in[0] * c[5] - in[2] * c[2] + in[3] * c[1]) * det;
	out[6] = (-in[12] * s[5] + in[14] * s[2] - in[15] * s[1]) * det;
	out[7] = ( in[8] * s[5] - in[10] * s[2] + in[11] * s[1]) * det;
	out[8] = ( in[4] * c[4] - in[5] * c[2] + in[7] * c[0]) * det;
	out[9] = (-in[0] * c[4] + in[1] * c[2] - in[3] * c[0]) * det;
	out[10] = ( in[12] * s[4] - in[13] * s[2] + in[15] * s[0]) * det;
	out[11] = (-in[8] * s[4] + in[9] * s[2] - in[11] * s[0]) * det;
	out[12] = (-in[4] * c[3] + in[5] * c[1] - in[6] * c[0]) * det;
	out[13] = ( in[0] * c[3] - in[1] * c[1] + in[2] * c[0]) * det;
	out[14] = (-in[12] * s[3] + in[13] * s[1] - in[14] * s[0]) * det;
	out[15] = ( in[8] * s[3] - in[9] * s[1] + in[10] * s[0]) * det;
	return 1;
}

static void ref_power(float* out, const float* base, long long exponent)
{
	float b[16], t[16];
	if (exponent < 0)
	{
		ref_inverse(b, base);
		exponent = -exponent;
	}
	else
	{
		memcpy(b, base, sizeof(b));
	}
	ref_identity(out);
	memcpy(t, b, sizeof(t));
	while (exponent > 0)
	{
		if (exponent & 1)
			ref_mul(out, out, t);
		exponent >>= 1;
		if (exponent > 0)
			ref_mul(t, t, t);
	}
}

static void ref_quat_norm(float* q)
{
	float l = sqrtf(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
	if (l < 1e-20f)
	{
		q[0] = q[1] = q[2] = 0.f;
		q[3] = 1.f;
		return;
	}
	for (int i = 0; i < 4; ++i)
		q[i] /= l;
}

static void ref_slerp(const float* a, const float* bin, float alpha, float* out)
{
	float b[4];
	memcpy(b, bin, sizeof(b));
	float dot = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3];
	if (dot < 0.f)
	{
		dot = -dot;
		for (int i = 0; i < 4; ++i)
			b[i] = -b[i];
	}
	if (dot > 0.9995f)
	{
		for (int i = 0; i < 4; ++i)
			out[i] = a[i] * (1.f - alpha) + b[i] * alpha;
	}
	else
	{
		if (dot > 1.f) dot = 1.f;
		if (dot < -1.f) dot = -1.f;
		float th = acosf(dot);
		float sh = sinf(th);
		float w0 = sinf((1.f - alpha) * th) / sh;
		float w1 = sinf(alpha * th) / sh;
		for (int i = 0; i < 4; ++i)
			out[i] = a[i] * w0 + b[i] * w1;
	}
	ref_quat_norm(out);
}

static void ref_compose(float* out, const float* t, const float* r, const float* s)
{
	float qx = r[0], qy = r[1], qz = r[2], qw = r[3];
	out[0] = (1.f - 2.f * qy * qy - 2.f * qz * qz) * s[0];
	out[1] = (2.f * qx * qy + 2.f * qz * qw) * s[0];
	out[2] = (2.f * qx * qz - 2.f * qy * qw) * s[0];
	out[3] = 0.f;
	out[4] = (2.f * qx * qy - 2.f * qz * qw) * s[1];
	out[5] = (1.f - 2.f * qx * qx - 2.f * qz * qz) * s[1];
	out[6] = (2.f * qy * qz + 2.f * qx * qw) * s[1];
	out[7] = 0.f;
	out[8] = (2.f * qx * qz + 2.f * qy * qw) * s[2];
	out[9] = (2.f * qy * qz - 2.f * qx * qw) * s[2];
	out[10] = (1.f - 2.f * qx * qx - 2.f * qy * qy) * s[2];
	out[11] = 0.f;
	out[12] = t[0];
	out[13] = t[1];
	out[14] = t[2];
	out[15] = 1.f;
}

static void quat_from_axis_z(float angle, float q[4])
{
	q[0] = 0.f;
	q[1] = 0.f;
	q[2] = sinf(angle * 0.5f);
	q[3] = cosf(angle * 0.5f);
}

static int mat_finite(const float* m)
{
	for (int i = 0; i < 16; ++i)
		if (!isfinite(m[i]))
			return 0;
	return 1;
}

/* ------------------------------------------------------------------ */
/* in-memory cgltf_data builder                                        */
/* ------------------------------------------------------------------ */

#define MAX_ACC 64
#define MAX_VIEWS 64
#define MAX_SAM 32
#define MAX_CH 32
#define MAX_STORAGE_FLOATS 8192
#define MAX_NODES 32

typedef struct builder {
	cgltf_data data;
	cgltf_node nodes[MAX_NODES];
	cgltf_size nodes_count;
	cgltf_scene scene;
	cgltf_node* scene_roots[MAX_NODES];
	cgltf_buffer buffer;
	cgltf_buffer_view views[MAX_VIEWS];
	cgltf_size views_count;
	cgltf_accessor accessors[MAX_ACC];
	cgltf_size accessors_count;
	cgltf_animation animation;
	cgltf_animation_sampler samplers[MAX_SAM];
	cgltf_size samplers_count;
	cgltf_animation_channel channels[MAX_CH];
	cgltf_size channels_count;
	float storage[MAX_STORAGE_FLOATS];
	cgltf_size storage_floats_used;
	cgltf_node** child_lists[MAX_NODES];
} builder;

static void builder_init(builder* b, cgltf_size nodes_count)
{
	memset(b, 0, sizeof(*b));
	b->nodes_count = nodes_count;
	for (cgltf_size i = 0; i < nodes_count; ++i)
	{
		b->nodes[i].scale[0] = b->nodes[i].scale[1] =
			b->nodes[i].scale[2] = 1.f;
		b->nodes[i].rotation[3] = 1.f;
	}
}

static void builder_free(builder* b)
{
	for (cgltf_size i = 0; i < b->nodes_count; ++i)
		free(b->nodes[i].children);
}

static cgltf_accessor* builder_add_accessor(builder* b, cgltf_type type,
		cgltf_size count, const float* values, cgltf_size float_count)
{
	cgltf_size comps = cgltf_num_components(type);
	if (b->accessors_count >= MAX_ACC || b->views_count >= MAX_VIEWS ||
			b->storage_floats_used + float_count > MAX_STORAGE_FLOATS)
		return NULL;

	cgltf_size byte_offset = b->storage_floats_used * sizeof(float);
	memcpy(b->storage + b->storage_floats_used, values,
			float_count * sizeof(float));
	b->storage_floats_used += float_count;

	cgltf_buffer_view* view = &b->views[b->views_count];
	memset(view, 0, sizeof(*view));
	view->buffer = &b->buffer;
	view->offset = byte_offset;
	view->size = float_count * sizeof(float);
	view->stride = 0;

	cgltf_accessor* acc = &b->accessors[b->accessors_count];
	memset(acc, 0, sizeof(*acc));
	acc->buffer_view = view;
	acc->component_type = cgltf_component_type_r_32f;
	acc->type = type;
	acc->count = count;
	acc->offset = 0;
	acc->stride = (cgltf_size)comps * sizeof(float);

	b->views_count++;
	b->accessors_count++;
	return acc;
}

static void node_set_trs(builder* b, cgltf_size idx,
		const float* t, const float* r, const float* s)
{
	cgltf_node* n = &b->nodes[idx];
	if (t) { memcpy(n->translation, t, 3 * sizeof(float)); n->has_translation = 1; }
	if (r) { memcpy(n->rotation, r, 4 * sizeof(float)); n->has_rotation = 1; }
	if (s) { memcpy(n->scale, s, 3 * sizeof(float)); n->has_scale = 1; }
}

static void node_set_matrix(builder* b, cgltf_size idx, const float m[16])
{
	cgltf_node* n = &b->nodes[idx];
	memcpy(n->matrix, m, 16 * sizeof(float));
	n->has_matrix = 1;
}

static void node_link(builder* b, cgltf_size parent, cgltf_size child)
{
	cgltf_node* p = &b->nodes[parent];
	cgltf_size n = p->children_count;
	cgltf_node** list = (cgltf_node**)realloc(p->children,
			(n + 1) * sizeof(cgltf_node*));
	list[n] = &b->nodes[child];
	p->children = list;
	p->children_count = n + 1;
	b->nodes[child].parent = &b->nodes[parent];
}

static cgltf_animation_sampler* builder_add_sampler(builder* b,
		cgltf_interpolation_type interp, cgltf_accessor* in, cgltf_accessor* out)
{
	if (b->samplers_count >= MAX_SAM)
		return NULL;
	cgltf_animation_sampler* s = &b->samplers[b->samplers_count++];
	memset(s, 0, sizeof(*s));
	s->input = in;
	s->output = out;
	s->interpolation = interp;
	return s;
}

static cgltf_animation_channel* builder_add_channel(builder* b,
		cgltf_animation_sampler* sampler, cgltf_size node_idx,
		cgltf_animation_path_type path)
{
	if (b->channels_count >= MAX_CH)
		return NULL;
	cgltf_animation_channel* c = &b->channels[b->channels_count++];
	memset(c, 0, sizeof(*c));
	c->sampler = sampler;
	c->target_node = &b->nodes[node_idx];
	c->target_path = path;
	return c;
}

static void builder_finalize(builder* b)
{
	b->buffer.data = b->storage;
	b->buffer.size = b->storage_floats_used * sizeof(float);

	cgltf_size roots = 0;
	for (cgltf_size i = 0; i < b->nodes_count; ++i)
		if (b->nodes[i].parent == NULL)
			b->scene_roots[roots++] = &b->nodes[i];

	b->scene.nodes = b->scene_roots;
	b->scene.nodes_count = roots;

	b->data.nodes = b->nodes;
	b->data.nodes_count = b->nodes_count;
	b->data.scenes = &b->scene;
	b->data.scenes_count = 1;
	b->data.scene = &b->scene;
	b->data.buffers = &b->buffer;
	b->data.buffers_count = 1;
	b->data.buffer_views = b->views;
	b->data.buffer_views_count = b->views_count;
	b->data.accessors = b->accessors;
	b->data.accessors_count = b->accessors_count;
	b->data.animations = &b->animation;
	b->data.animations_count = 1;
	b->animation.samplers = b->samplers;
	b->animation.samplers_count = b->samplers_count;
	b->animation.channels = b->channels;
	b->animation.channels_count = b->channels_count;
}

/* ------------------------------------------------------------------ */
/* independent reference playback                                      */
/* ------------------------------------------------------------------ */

static cgltf_size ref_find_segment(const float* keys, cgltf_size count, float t)
{
	cgltf_size lo = 0;
	cgltf_size hi = count - 1;
	while (hi - lo > 1)
	{
		cgltf_size mid = lo + (hi - lo) / 2;
		if (keys[mid] <= t) lo = mid;
		else hi = mid;
	}
	return lo;
}

/* Fills out[0..comps-1] for one channel's target path at time t. */
static void ref_sample(const cgltf_animation_channel* channel, float t, float* out)
{
	const cgltf_animation_sampler* sampler = channel->sampler;
	cgltf_size comps = channel->target_path ==
		cgltf_animation_path_type_rotation ? 4 : 3;
	cgltf_bool cubic = sampler->interpolation ==
		cgltf_interpolation_type_cubic_spline;
	cgltf_size keys_n = sampler->input->count;
	float keys[64];
	for (cgltf_size i = 0; i < keys_n; ++i)
		cgltf_accessor_read_float(sampler->input, i, &keys[i], 1);

	/* For cubic spline the output holds 3 elements per key; the actual
	 * value of key k is element 3*k+1, between its in/out tangents. */
	if (t <= keys[0])
	{
		cgltf_accessor_read_float(sampler->output, cubic ? 1 : 0, out, comps);
		if (comps == 4) ref_quat_norm(out);
		return;
	}
	cgltf_size last = keys_n - 1;
	if (t >= keys[last])
	{
		cgltf_accessor_read_float(sampler->output,
				cubic ? 3 * last + 1 : last, out, comps);
		if (comps == 4) ref_quat_norm(out);
		return;
	}

	cgltf_size i = ref_find_segment(keys, keys_n, t);
	float dt = keys[i + 1] - keys[i];
	float alpha = (t - keys[i]) / dt;

	if (sampler->interpolation == cgltf_interpolation_type_step)
	{
		cgltf_accessor_read_float(sampler->output, i, out, comps);
		if (comps == 4) ref_quat_norm(out);
		return;
	}

	if (sampler->interpolation == cgltf_interpolation_type_linear)
	{
		float v0[4], v1[4];
		cgltf_accessor_read_float(sampler->output, i, v0, comps);
		cgltf_accessor_read_float(sampler->output, i + 1, v1, comps);
		if (comps == 4)
		{
			ref_slerp(v0, v1, alpha, out);
		}
		else
		{
			for (cgltf_size c = 0; c < comps; ++c)
				out[c] = v0[c] * (1.f - alpha) + v1[c] * alpha;
		}
		return;
	}

	/* cubic spline: each key occupies three interleaved output elements.
	 * cgltf_accessor_read_float returns one element per call, so read the
	 * three elements (in-tangent, value, out-tangent) of each key. */
	float block[24];
	for (cgltf_size g = 0; g < 3; ++g)
	{
		cgltf_accessor_read_float(sampler->output, 3 * i + g,
				block + g * comps, comps);
		cgltf_accessor_read_float(sampler->output, 3 * (i + 1) + g,
				block + (3 + g) * comps, comps);
	}
	float* v0 = block + comps;
	float* out0 = block + 2 * comps;
	float* in1 = block + 3 * comps;
	float* v1 = block + 4 * comps;

	float a = alpha, a2 = a * a, a3 = a2 * a;
	float h00 = 2.f * a3 - 3.f * a2 + 1.f;
	float h10 = a3 - 2.f * a2 + a;
	float h01 = -2.f * a3 + 3.f * a2;
	float h11 = a3 - a2;
	for (cgltf_size c = 0; c < comps; ++c)
		out[c] = h00 * v0[c] + h10 * dt * out0[c] +
			h01 * v1[c] + h11 * dt * in1[c];
	if (comps == 4) ref_quat_norm(out);
}

static void ref_local(const builder* b, cgltf_size idx, float t, float out[16])
{
	const cgltf_node* node = &b->nodes[idx];
	if (node->has_matrix)
	{
		memcpy(out, node->matrix, 16 * sizeof(float));
		return;
	}

	float tr[3] = {0.f, 0.f, 0.f};
	float q[4] = {0.f, 0.f, 0.f, 1.f};
	float sc[3] = {1.f, 1.f, 1.f};
	if (node->has_translation) memcpy(tr, node->translation, sizeof(tr));
	if (node->has_rotation) memcpy(q, node->rotation, sizeof(q));
	if (node->has_scale) memcpy(sc, node->scale, sizeof(sc));

	for (cgltf_size c = 0; c < b->channels_count; ++c)
	{
		const cgltf_animation_channel* ch = &b->channels[c];
		if (ch->target_node != node)
			continue;
		float v[4];
		ref_sample(ch, t, v);
		if (ch->target_path == cgltf_animation_path_type_translation)
			memcpy(tr, v, sizeof(tr));
		else if (ch->target_path == cgltf_animation_path_type_rotation)
			memcpy(q, v, sizeof(q));
		else if (ch->target_path == cgltf_animation_path_type_scale)
			memcpy(sc, v, sizeof(sc));
	}
	ref_compose(out, tr, q, sc);
}

static void ref_world(const builder* b, cgltf_size idx, float t, float out[16])
{
	float chain[MAX_NODES][16];
	cgltf_size depth = 0;
	const cgltf_node* cur = &b->nodes[idx];
	for (;;)
	{
		cgltf_size ci = (cgltf_size)(cur - b->nodes);
		ref_local(b, ci, t, chain[depth++]);
		if (cur->parent == NULL)
			break;
		cur = cur->parent;
	}
	ref_identity(out);
	for (cgltf_size d = depth; d > 0; --d)
		ref_mul(out, out, chain[d - 1]);
}

/* Reference root motion: same formulas as the spec, evaluated from the
 * reference sampled hierarchy. */
static void wrap_phase(float a, float span, float t, long long* k, float* u)
{
	double w = ((double)t - (double)a) / (double)span;
	double kd = floor(w);
	*k = (long long)kd;
	*u = (float)((double)a + ((double)t - (double)a) - kd * (double)span);
}

static void ref_root_motion(const builder* b, cgltf_motion_player* p,
		cgltf_size extract, float t, float out[16])
{
	float a = cgltf_motion_time_begin(p);
	float bb = cgltf_motion_time_end(p);
	long long k;
	float u;
	wrap_phase(a, bb - a, t, &k, &u);

	float g0[16], gb[16], gu[16];
	ref_world(b, extract, a, g0);
	ref_world(b, extract, bb, gb);
	ref_world(b, extract, u, gu);

	float inv_g0[16];
	ref_inverse(inv_g0, g0);
	float C[16];
	ref_mul(C, inv_g0, gb);
	float A[16];
	ref_mul(A, inv_g0, gu);
	float Ck[16];
	ref_power(Ck, C, k);

	float tmp[16];
	ref_mul(tmp, Ck, A);
	ref_mul(tmp, g0, tmp);
	ref_mul(out, tmp, inv_g0);
}

/* Extended character: (G0 C^k invG0) W(u), what M*P must reconstruct. */
static void ref_extended_world(const builder* b, cgltf_motion_player* p,
		cgltf_size extract, cgltf_size node, float t, float out[16])
{
	float a = cgltf_motion_time_begin(p);
	float bb = cgltf_motion_time_end(p);
	long long k;
	float u;
	wrap_phase(a, bb - a, t, &k, &u);

	float g0[16], gb[16];
	ref_world(b, extract, a, g0);
	ref_world(b, extract, bb, gb);
	float inv_g0[16];
	ref_inverse(inv_g0, g0);
	float C[16];
	ref_mul(C, inv_g0, gb);
	float Ck[16];
	ref_power(Ck, C, k);
	float transport[16];
	ref_mul(transport, g0, Ck);
	ref_mul(transport, transport, inv_g0);

	float w[16];
	ref_world(b, node, u, w);
	ref_mul(out, transport, w);
}

/* Universal numeric checks shared by every valid playback case. */
static void universal_checks(const char* tag, builder* b,
		cgltf_motion_player* p, cgltf_size extract,
		const float* times, cgltf_size ntimes)
{
	float a = cgltf_motion_time_begin(p);
	float bb = cgltf_motion_time_end(p);
	float g0_ref[16];
	ref_world(b, extract, a, g0_ref);

	for (cgltf_size i = 0; i < ntimes; ++i)
	{
		float t = times[i];
		cgltf_motion_pose pose;
		cgltf_motion_matrix motion;
		cgltf_motion_result r = cgltf_motion_evaluate(p, t, &pose, &motion);
		CHECK(r == CGLTF_MOTION_OK);
		CHECK(pose.nodes_count == b->nodes_count);
		CHECK(mat_finite(motion.m));
		CHECK(mat_finite(pose.world_matrices));

		char label[64];
		for (cgltf_size n = 0; n < b->nodes_count; ++n)
		{
			float want[16];
			ref_extended_world(b, p, extract, n, t, want);
			float rebuilt[16];
			ref_mul(rebuilt, motion.m, pose.world_matrices + n * 16);
			snprintf(label, sizeof(label), "%s t%.3f node%llu rebuild",
					tag, t, (unsigned long long)n);
			check_matrix(label, rebuilt, want);
		}

		/* Extraction node is pinned to its start pose. */
		snprintf(label, sizeof(label), "%s t%.3f pinned", tag, t);
		check_matrix(label, pose.world_matrices + extract * 16, g0_ref);

		/* Accumulated motion against the independent formula. */
		float m_ref[16];
		ref_root_motion(b, p, extract, t, m_ref);
		snprintf(label, sizeof(label), "%s t%.3f motion", tag, t);
		check_matrix(label, motion.m, m_ref);
	}

	/* Delta algebra over arbitrary, unequal, negative and multi-loop
	 * times; query order is shuffled on purpose. */
	if (ntimes >= 3)
	{
		cgltf_size i0 = 0, i1 = ntimes / 2, i2 = ntimes - 1;
		float t0 = times[i0], t1 = times[i1], t2 = times[i2];
		cgltf_motion_matrix d01, d12, d02, dself, d21, d10;
		CHECK(cgltf_motion_delta(p, t1, t2, &d12) == CGLTF_MOTION_OK);
		CHECK(cgltf_motion_delta(p, t0, t1, &d01) == CGLTF_MOTION_OK);
		CHECK(cgltf_motion_delta(p, t0, t2, &d02) == CGLTF_MOTION_OK);
		CHECK(cgltf_motion_delta(p, t1, t1, &dself) == CGLTF_MOTION_OK);
		CHECK(cgltf_motion_delta(p, t2, t1, &d21) == CGLTF_MOTION_OK);
		CHECK(cgltf_motion_delta(p, t1, t0, &d10) == CGLTF_MOTION_OK);

		float chained[16];
		ref_mul(chained, d01.m, d12.m);
		check_matrix("delta chain", chained, d02.m);

		float ident[16];
		ref_identity(ident);
		check_matrix("delta self", dself.m, ident);

		float inv_01[16], expect_10[16];
		ref_inverse(inv_01, d01.m);
		ref_mul(expect_10, d21.m, d10.m);
		check_matrix("delta reverse", inv_01, d10.m);
		(void)expect_10;

		/* Pause / reverse / far-then-near determinism. */
		cgltf_motion_matrix again_far, again_near, first;
		CHECK(cgltf_motion_evaluate(p, t2, NULL, &again_far) == CGLTF_MOTION_OK);
		CHECK(cgltf_motion_evaluate(p, t0, NULL, &first) == CGLTF_MOTION_OK);
		CHECK(cgltf_motion_evaluate(p, t2, NULL, &again_near) == CGLTF_MOTION_OK);
		check_matrix("far-near determinism", again_far.m, again_near.m);
		float m0_ref[16];
		ref_root_motion(b, p, extract, t0, m0_ref);
		check_matrix("near value", first.m, m0_ref);
	}

	/* Exact hit on b starts the next cycle and stays continuous. Probe a
	 * single float-width below the seam so the value reflects the boundary
	 * jump itself (measured < 3e-6) rather than motion over a window. */
	cgltf_motion_matrix at_end, before_end;
	CHECK(cgltf_motion_evaluate(p, bb, NULL, &at_end) == CGLTF_MOTION_OK);
	CHECK(cgltf_motion_evaluate(p, nextafterf(bb, -1e30f), NULL, &before_end)
			== CGLTF_MOTION_OK);
	check_matrix("end continuity", at_end.m, before_end.m);
	float end_ref[16];
	ref_root_motion(b, p, extract, bb, end_ref);
	check_matrix("end next cycle", at_end.m, end_ref);

	/* Lower seam: the last instant of the previous (negative) cycle meets
	 * t == a continuously, exercising the negative-k power path. */
	cgltf_motion_matrix at_begin, below_begin;
	CHECK(cgltf_motion_evaluate(p, a, NULL, &at_begin) == CGLTF_MOTION_OK);
	CHECK(cgltf_motion_evaluate(p, nextafterf(a, -1e30f), NULL, &below_begin)
			== CGLTF_MOTION_OK);
	check_matrix("begin continuity", at_begin.m, below_begin.m);
	(void)a;
}

/* ------------------------------------------------------------------ */
/* case 1: the shipped motion.gltf document                            */
/* ------------------------------------------------------------------ */

/* Forward declaration: shared by the positive scale case and the
 * rejection cases defined below. */
static cgltf_motion_player* make_minimal_player(builder* b, cgltf_size extract);

static void case_motion_gltf(void)
{
	begin_case();

	cgltf_options options = {0};
	cgltf_data* data = NULL;
	CHECK(cgltf_parse_file(&options, "checks/motion.gltf", &data) ==
			cgltf_result_success);
	CHECK(cgltf_load_buffers(&options, data, "checks/motion.gltf") ==
			cgltf_result_success);
	CHECK(cgltf_validate(data) == cgltf_result_success);

	cgltf_motion_player* p = NULL;
	cgltf_motion_result r = cgltf_motion_create(data, 0, 2, &p);
	CHECK(r == CGLTF_MOTION_OK);
	if (r != CGLTF_MOTION_OK)
	{
		printf("create failed: %s\n", cgltf_motion_result_string(r));
		cgltf_free(data);
		end_case("motion.gltf");
		return;
	}
	CHECK(cgltf_motion_nodes_count(p) == 3);
	check_scalar("range begin", cgltf_motion_time_begin(p), 0.f);
	check_scalar("range end", cgltf_motion_time_end(p), 1.f);

	/* Hand-computed anchors: node 2 world translation is (4,1,0) at t=0
	 * and (5,1,0) at t=1. */
	cgltf_motion_pose pose;
	cgltf_motion_matrix motion;
	CHECK(cgltf_motion_evaluate(p, 0.f, &pose, &motion) == CGLTF_MOTION_OK);
	check_scalar("pinned node2 x", pose.world_matrices[2 * 16 + 12], 4.f);
	check_scalar("pinned node2 y", pose.world_matrices[2 * 16 + 13], 1.f);
	float ident[16];
	ref_identity(ident);
	check_matrix("motion at begin identity", motion.m, ident);

	CHECK(cgltf_motion_evaluate(p, 1.f, &pose, &motion) == CGLTF_MOTION_OK);
	/* At the exact end time the extraction node is still pinned. */
	check_scalar("pinned at end x", pose.world_matrices[2 * 16 + 12], 4.f);
	check_scalar("pinned at end y", pose.world_matrices[2 * 16 + 13], 1.f);

	/* Rebuild ordinary sampled worlds at in-cycle times. */
	float w2_0[16], w2_1[16];
	cgltf_node_transform_world(&data->nodes[2], w2_0);
	/* Static transform matches t=0 since keys start on the static TRS. */
	check_scalar("static world x", w2_0[12], 4.f);
	check_scalar("static world y", w2_0[13], 1.f);

	cgltf_motion_pose pose1;
	cgltf_motion_matrix m1;
	CHECK(cgltf_motion_evaluate(p, 1.f, &pose1, &m1) == CGLTF_MOTION_OK);
	float rebuilt2[16];
	ref_mul(rebuilt2, m1.m, pose1.world_matrices + 2 * 16);
	/* Node 1 at t=1 has translation (2,0) and quaternion (0,0,.7071,.7071).
	 * Its rotation column 1 is (-1,0,0), so node 2's offset (0,1) maps to
	 * (-1,0); the world translation is (4,0)+(2,0)+(-1,0) = (5,0). */
	check_scalar("rebuilt end x", rebuilt2[12], 5.f);
	check_scalar("rebuilt end y", rebuilt2[13], 0.f);
	/* Upper 3x3 spot checks for the quaternion (0,0,.7071,.7071). */
	check_scalar("rebuilt R00", rebuilt2[0], 0.f);
	check_scalar("rebuilt R11", rebuilt2[5], 0.f);
	check_scalar("rebuilt R10", rebuilt2[4], -1.f);
	(void)w2_1;

	/* Two players on different extraction nodes, interleaved queries. */
	cgltf_motion_player* q = NULL;
	CHECK(cgltf_motion_create(data, 0, 1, &q) == CGLTF_MOTION_OK);
	cgltf_motion_matrix mp, mq;
	CHECK(cgltf_motion_evaluate(p, 2.5f, NULL, &mp) == CGLTF_MOTION_OK);
	CHECK(cgltf_motion_evaluate(q, 0.25f, NULL, &mq) == CGLTF_MOTION_OK);
	CHECK(cgltf_motion_evaluate(p, -1.25f, NULL, &mp) == CGLTF_MOTION_OK);
	CHECK(cgltf_motion_evaluate(q, 3.75f, NULL, &mq) == CGLTF_MOTION_OK);
	CHECK(mat_finite(mp.m) && mat_finite(mq.m));
	/* Repeating the same query on a player is stable. */
	cgltf_motion_matrix mp2;
	CHECK(cgltf_motion_evaluate(p, -1.25f, NULL, &mp2) == CGLTF_MOTION_OK);
	check_matrix("player stable", mp.m, mp2.m);

	/* Independent reference model with identical content, so the parsed
	 * document can be checked against the formula oracle too. */
	builder ref;
	builder_init(&ref, 3);
	node_set_trs(&ref, 0, (float[3]){4.f, 0.f, 0.f}, NULL, NULL);
	node_link(&ref, 0, 1);
	node_link(&ref, 1, 2);
	node_set_trs(&ref, 2, (float[3]){0.f, 1.f, 0.f}, NULL, NULL);
	{
		float rkeys[2] = {0.f, 1.f};
		float tvals[6] = {0.f, 0.f, 0.f, 2.f, 0.f, 0.f};
		float qz[4];
		quat_from_axis_z(1.5707963f, qz);
		float qvals[8] = {0.f, 0.f, 0.f, 1.f, qz[0], qz[1], qz[2], qz[3]};
		cgltf_accessor* kt = builder_add_accessor(&ref, cgltf_type_scalar, 2,
				rkeys, 2);
		cgltf_accessor* vt = builder_add_accessor(&ref, cgltf_type_vec3, 2,
				tvals, 6);
		cgltf_accessor* kq = builder_add_accessor(&ref, cgltf_type_scalar, 2,
				rkeys, 2);
		cgltf_accessor* vq = builder_add_accessor(&ref, cgltf_type_vec4, 2,
				qvals, 8);
		cgltf_animation_sampler* st = builder_add_sampler(&ref,
				cgltf_interpolation_type_linear, kt, vt);
		cgltf_animation_sampler* sq = builder_add_sampler(&ref,
				cgltf_interpolation_type_linear, kq, vq);
		builder_add_channel(&ref, st, 1, cgltf_animation_path_type_translation);
		builder_add_channel(&ref, sq, 1, cgltf_animation_path_type_rotation);
		builder_finalize(&ref);
		CHECK(cgltf_validate(&ref.data) == cgltf_result_success);
	}

	float times[] = {-2.5f, -1.25f, -1.f, -0.0001f, 0.f, 0.25f, 0.5f,
		0.75f, 1.f, 1.0001f, 2.f, 2.5f, 4.f};
	for (cgltf_size i = 0; i < sizeof(times) / sizeof(times[0]); ++i)
	{
		float t = times[i];
		cgltf_motion_pose po;
		cgltf_motion_matrix mo;
		CHECK(cgltf_motion_evaluate(p, t, &po, &mo) == CGLTF_MOTION_OK);
		/* Extraction node pinned to its start world translation/rotation. */
		check_scalar("file pinned x", po.world_matrices[2 * 16 + 12], 4.f);
		check_scalar("file pinned y", po.world_matrices[2 * 16 + 13], 1.f);
		/* Accumulated motion against the matrix-power formula oracle. */
		float m_ref[16];
		ref_root_motion(&ref, p, 2, t, m_ref);
		check_matrix("file motion oracle", mo.m, m_ref);
		/* In-place rebuild reproduces the extended transported character. */
		for (cgltf_size n = 0; n < 3; ++n)
		{
			float want[16], rebuilt[16];
			ref_extended_world(&ref, p, 2, n, t, want);
			ref_mul(rebuilt, mo.m, po.world_matrices + n * 16);
			check_matrix("file rebuild", rebuilt, want);
		}
	}

	/* Two full forward cycles and one negative cycle, via the oracle
	 * rather than a linear per-cycle translation guess. */
	float fwd_ref[16], neg_ref[16];
	ref_root_motion(&ref, p, 2, 2.f, fwd_ref);
	ref_root_motion(&ref, p, 2, -1.f, neg_ref);
	cgltf_motion_matrix m2, mneg;
	CHECK(cgltf_motion_evaluate(p, 2.f, NULL, &m2) == CGLTF_MOTION_OK);
	CHECK(cgltf_motion_evaluate(p, -1.f, NULL, &mneg) == CGLTF_MOTION_OK);
	check_matrix("two cycle", m2.m, fwd_ref);
	check_matrix("neg cycle", mneg.m, neg_ref);

	builder_free(&ref);
	cgltf_motion_release(q);
	cgltf_motion_release(p);
	cgltf_free(data);
	end_case("motion.gltf");
}

/* ------------------------------------------------------------------ */
/* case 2: CUBICSPLINE rotation, raw keys (no sign flipping)           */
/* ------------------------------------------------------------------ */

static void case_cubic_rotation(void)
{
	begin_case();
	builder b;
	builder_init(&b, 1);

	float q0[4] = {0.f, 0.f, 0.f, 1.f};
	float q1[4] = {0.f, 0.70710678f, 0.f, 0.70710678f};
	/* Deliberately sign-opposed component values in tangents to prove the
	 * dt scaling and that no tangent/key gets flipped. */
	float out0[4] = {0.f, 0.2f, 0.f, 0.f};
	float in1[4] = {0.f, -0.1f, 0.f, 0.f};

	float keys[2] = {0.f, 2.f};
	/* glTF cubic output: 3 vec4 elements per key, interleaved as
	 * [in-tangent, value, out-tangent] at key 0 then key 1. */
	float values[24];
	memcpy(values + 0, (float[4]){0,0,0,0}, 4 * sizeof(float));
	memcpy(values + 4, q0, 4 * sizeof(float));
	memcpy(values + 8, out0, 4 * sizeof(float));
	memcpy(values + 12, in1, 4 * sizeof(float));
	memcpy(values + 16, q1, 4 * sizeof(float));
	memcpy(values + 20, (float[4]){0,0,0,0}, 4 * sizeof(float));

	cgltf_accessor* kin = builder_add_accessor(&b, cgltf_type_scalar, 2, keys, 2);
	cgltf_accessor* vout = builder_add_accessor(&b, cgltf_type_vec4, 6, values, 24);
	cgltf_animation_sampler* s = builder_add_sampler(&b,
			cgltf_interpolation_type_cubic_spline, kin, vout);
	builder_add_channel(&b, s, 0, cgltf_animation_path_type_rotation);
	builder_finalize(&b);
	CHECK(cgltf_validate(&b.data) == cgltf_result_success);

	cgltf_motion_player* p = NULL;
	CHECK(cgltf_motion_create(&b.data, 0, 0, &p) == CGLTF_MOTION_OK);

	float times[] = {-3.f, -1.25f, 0.f, 0.5f, 1.f, 1.75f, 2.f, 4.f, 5.25f};
	universal_checks("cubic", &b, p, 0, times, sizeof(times) / sizeof(times[0]));

	/* Explicit midpoint number from the component Hermite formula. */
	float alpha = 0.5f, dt = 2.f;
	float h00 = 2.f * alpha * alpha * alpha - 3.f * alpha * alpha + 1.f;
	float h10 = alpha * alpha * alpha - 2.f * alpha * alpha + alpha;
	float h01 = -2.f * alpha * alpha * alpha + 3.f * alpha * alpha;
	float h11 = alpha * alpha * alpha - alpha * alpha;
	float qmid[4];
	for (int i = 0; i < 4; ++i)
		qmid[i] = h00 * q0[i] + h10 * dt * out0[i] +
			h01 * q1[i] + h11 * dt * in1[i];
	ref_quat_norm(qmid);
	float want[16];
	ref_compose(want, (float[3]){0,0,0}, qmid, (float[3]){1,1,1});
	cgltf_motion_matrix got;
	CHECK(cgltf_motion_evaluate(p, 1.f, NULL, &got) == CGLTF_MOTION_OK);
	check_matrix("cubic midpoint", got.m, want);

	/* The rotation stays a proper rotation at every sampled time. */
	for (int i = 0; i < 9; ++i)
	{
		cgltf_motion_matrix m;
		CHECK(cgltf_motion_evaluate(p, times[i], NULL, &m) == CGLTF_MOTION_OK);
		check_scalar("rotation last row w", m.m[15], 1.f);
		check_scalar("rotation no shear 3", m.m[3], 0.f);
		check_scalar("rotation no shear 7", m.m[7], 0.f);
	}

	cgltf_motion_release(p);
	builder_free(&b);
	end_case("cubic rotation");
}

/* ------------------------------------------------------------------ */
/* case 3: LINEAR quaternion with a sign-negated key                   */
/* ------------------------------------------------------------------ */

static void case_linear_antipodal(void)
{
	begin_case();
	builder b;
	builder_init(&b, 1);

	/* q1 is the negation of the +120 degree y rotation, so the raw dot
	 * with identity is negative and shortest-arc must flip it. */
	float q0[4] = {0.f, 0.f, 0.f, 1.f};
	float q1[4] = {0.f, -0.8660254f, 0.f, -0.5f};
	float keys[2] = {0.f, 1.f};
	float values[8];
	memcpy(values, q0, sizeof(q0));
	memcpy(values + 4, q1, sizeof(q1));

	cgltf_accessor* kin = builder_add_accessor(&b, cgltf_type_scalar, 2, keys, 2);
	cgltf_accessor* vout = builder_add_accessor(&b, cgltf_type_vec4, 2, values, 8);
	cgltf_animation_sampler* s = builder_add_sampler(&b,
			cgltf_interpolation_type_linear, kin, vout);
	builder_add_channel(&b, s, 0, cgltf_animation_path_type_rotation);
	builder_finalize(&b);
	CHECK(cgltf_validate(&b.data) == cgltf_result_success);

	cgltf_motion_player* p = NULL;
	CHECK(cgltf_motion_create(&b.data, 0, 0, &p) == CGLTF_MOTION_OK);

	/* Midpoint follows the short arc: +60 degrees about y. */
	float qwant[4];
	ref_slerp(q0, q1, 0.5f, qwant);
	float want[16];
	ref_compose(want, (float[3]){0,0,0}, qwant, (float[3]){1,1,1});
	cgltf_motion_matrix got;
	CHECK(cgltf_motion_evaluate(p, 0.5f, NULL, &got) == CGLTF_MOTION_OK);
	check_matrix("antipodal midpoint", got.m, want);
	/* End orientation equals the equivalent positive rotation. */
	float qend[4] = {0.f, 0.8660254f, 0.f, 0.5f};
	float want_end[16];
	ref_compose(want_end, (float[3]){0,0,0}, qend, (float[3]){1,1,1});
	cgltf_motion_matrix at_end;
	CHECK(cgltf_motion_evaluate(p, 1.f, NULL, &at_end) == CGLTF_MOTION_OK);
	check_matrix("antipodal end", at_end.m, want_end);

	float times[] = {-2.f, 0.f, 0.25f, 0.5f, 0.75f, 1.f, 3.f};
	universal_checks("antipodal", &b, p, 0, times,
			sizeof(times) / sizeof(times[0]));

	cgltf_motion_release(p);
	builder_free(&b);
	end_case("linear antipodal");
}

/* ------------------------------------------------------------------ */
/* case 4: different key ranges per channel + STEP child               */
/* ------------------------------------------------------------------ */

static void case_mixed_key_times(void)
{
	begin_case();
	builder b;
	builder_init(&b, 2);
	node_link(&b, 0, 1);

	float trans_keys[2] = {0.f, 2.f};
	float trans_vals[6] = {0.f, 0.f, 0.f, 4.f, 0.f, 0.f};
	float rot_keys[2] = {0.5f, 1.5f};
	float q0[4] = {0.f, 0.f, 0.f, 1.f};
	float q1[4];
	quat_from_axis_z(1.5707963f, q1);
	float rot_vals[8];
	memcpy(rot_vals, q0, sizeof(q0));
	memcpy(rot_vals + 4, q1, sizeof(q1));

	float step_keys[3] = {0.f, 1.f, 2.f};
	float step_vals[9] = {0.f, 0.f, 0.f, 5.f, 0.f, 0.f, 9.f, 0.f, 0.f};

	cgltf_accessor* k1 = builder_add_accessor(&b, cgltf_type_scalar, 2,
			trans_keys, 2);
	cgltf_accessor* v1 = builder_add_accessor(&b, cgltf_type_vec3, 2,
			trans_vals, 6);
	cgltf_accessor* k2 = builder_add_accessor(&b, cgltf_type_scalar, 2,
			rot_keys, 2);
	cgltf_accessor* v2 = builder_add_accessor(&b, cgltf_type_vec4, 2,
			rot_vals, 8);
	cgltf_accessor* k3 = builder_add_accessor(&b, cgltf_type_scalar, 3,
			step_keys, 3);
	cgltf_accessor* v3 = builder_add_accessor(&b, cgltf_type_vec3, 3,
			step_vals, 9);

	cgltf_animation_sampler* s1 = builder_add_sampler(&b,
			cgltf_interpolation_type_linear, k1, v1);
	cgltf_animation_sampler* s2 = builder_add_sampler(&b,
			cgltf_interpolation_type_linear, k2, v2);
	cgltf_animation_sampler* s3 = builder_add_sampler(&b,
			cgltf_interpolation_type_step, k3, v3);
	builder_add_channel(&b, s1, 0, cgltf_animation_path_type_translation);
	builder_add_channel(&b, s2, 0, cgltf_animation_path_type_rotation);
	builder_add_channel(&b, s3, 1, cgltf_animation_path_type_translation);
	builder_finalize(&b);
	CHECK(cgltf_validate(&b.data) == cgltf_result_success);

	cgltf_motion_player* p = NULL;
	CHECK(cgltf_motion_create(&b.data, 0, 0, &p) == CGLTF_MOTION_OK);
	/* Overall range spans the union: [0, 2]. */
	check_scalar("mixed begin", cgltf_motion_time_begin(p), 0.f);
	check_scalar("mixed end", cgltf_motion_time_end(p), 2.f);

	/* Clamping: at 0.25 the rotation track is still at its first key. */
	float w_clamp[16];
	ref_world(&b, 0, 0.25f, w_clamp);
	check_scalar("clamped trans x", w_clamp[12], 0.5f);
	check_scalar("clamped rot R00", w_clamp[0], 1.f);
	/* Past the rotation track's last key it clamps to q1. */
	float w_high[16];
	ref_world(&b, 0, 1.75f, w_high);
	check_scalar("high trans x", w_high[12], 3.5f);
	check_scalar("clamped high R00", w_high[0], 0.f);

	/* STEP semantics on the child: it holds the left key of its segment. */
	float local_before[16], local_after[16];
	ref_local(&b, 1, 0.999f, local_before);
	ref_local(&b, 1, 1.001f, local_after);
	check_scalar("step before local x", local_before[12], 0.f);
	check_scalar("step after local x", local_after[12], 5.f);
	/* And the parent translation keeps moving linearly across the seam. */
	float w_before[16], w_after[16];
	ref_world(&b, 0, 0.999f, w_before);
	ref_world(&b, 0, 1.001f, w_after);
	check_scalar("parent before x", w_before[12], 1.998f);
	check_scalar("parent after x", w_after[12], 2.002f);

	/* Static values survive where no channel covers a property. */
	cgltf_motion_pose pose;
	cgltf_motion_matrix motion;
	CHECK(cgltf_motion_evaluate(p, 0.7f, &pose, &motion) == CGLTF_MOTION_OK);
	CHECK(mat_finite(pose.world_matrices));

	float times[] = {-4.f, -0.5f, 0.f, 0.25f, 0.7f, 1.f, 1.5f, 1.999f,
		2.f, 3.5f, 6.f};
	universal_checks("mixed", &b, p, 0, times,
			sizeof(times) / sizeof(times[0]));

	cgltf_motion_release(p);
	builder_free(&b);
	end_case("mixed key times");
}

/* ------------------------------------------------------------------ */
/* case 5: negative time and multi-loop turning with a parent bias      */
/* ------------------------------------------------------------------ */

static void case_negative_multiloop(void)
{
	begin_case();
	builder b;
	builder_init(&b, 2);
	node_set_trs(&b, 0, (float[3]){2.f, 3.f, 0.f}, NULL, NULL);
	node_link(&b, 0, 1);

	float keys[2] = {0.f, 1.f};
	float tv[6] = {0.f, 0.f, 0.f, 1.f, 0.f, 0.f};
	float q0[4] = {0.f, 0.f, 0.f, 1.f};
	float q1[4];
	quat_from_axis_z(3.14159265f, q1);
	float qv[8];
	memcpy(qv, q0, sizeof(q0));
	memcpy(qv + 4, q1, sizeof(q1));

	cgltf_accessor* kt = builder_add_accessor(&b, cgltf_type_scalar, 2, keys, 2);
	cgltf_accessor* vt = builder_add_accessor(&b, cgltf_type_vec3, 2, tv, 6);
	cgltf_accessor* kq = builder_add_accessor(&b, cgltf_type_scalar, 2, keys, 2);
	cgltf_accessor* vq = builder_add_accessor(&b, cgltf_type_vec4, 2, qv, 8);
	cgltf_animation_sampler* st = builder_add_sampler(&b,
			cgltf_interpolation_type_linear, kt, vt);
	cgltf_animation_sampler* sq = builder_add_sampler(&b,
			cgltf_interpolation_type_linear, kq, vq);
	builder_add_channel(&b, st, 1, cgltf_animation_path_type_translation);
	builder_add_channel(&b, sq, 1, cgltf_animation_path_type_rotation);
	builder_finalize(&b);
	CHECK(cgltf_validate(&b.data) == cgltf_result_success);

	cgltf_motion_player* p = NULL;
	CHECK(cgltf_motion_create(&b.data, 0, 1, &p) == CGLTF_MOTION_OK);

	float times[] = {-3.5f, -2.f, -1.0001f, -1.f, -0.25f, 0.f, 0.5f,
		1.f, 2.f, 2.75f, 5.f};
	universal_checks("multiloop", &b, p, 1, times,
			sizeof(times) / sizeof(times[0]));

	/* Explicit wrap check: t = -1.25 -> k = -2, u = 0.75. */
	long long k;
	float u;
	wrap_phase(0.f, 1.f, -1.25f, &k, &u);
	check_scalar("wrap negative k stays phase", u, 0.75f);
	CHECK(k == -2);
	cgltf_motion_matrix m;
	CHECK(cgltf_motion_evaluate(p, -1.25f, NULL, &m) == CGLTF_MOTION_OK);
	float refm[16];
	ref_root_motion(&b, p, 1, -1.25f, refm);
	check_matrix("negative explicit", m.m, refm);

	cgltf_motion_release(p);
	builder_free(&b);
	end_case("negative multiloop");
}

/* ------------------------------------------------------------------ */
/* case 6: non-identity start pose                                     */
/* ------------------------------------------------------------------ */

static void case_nonidentity_start(void)
{
	begin_case();
	builder b;
	builder_init(&b, 1);
	float qstart[4];
	quat_from_axis_z(0.7f, qstart);
	node_set_trs(&b, 0, (float[3]){1.f, -2.f, 0.5f}, qstart, NULL);

	float keys[2] = {0.f, 1.f};
	float tv[6] = {0.f, 0.f, 0.f, 0.f, 3.f, 0.f};
	cgltf_accessor* kt = builder_add_accessor(&b, cgltf_type_scalar, 2, keys, 2);
	cgltf_accessor* vt = builder_add_accessor(&b, cgltf_type_vec3, 2, tv, 6);
	cgltf_animation_sampler* st = builder_add_sampler(&b,
			cgltf_interpolation_type_linear, kt, vt);
	builder_add_channel(&b, st, 0, cgltf_animation_path_type_translation);
	builder_finalize(&b);
	CHECK(cgltf_validate(&b.data) == cgltf_result_success);

	cgltf_motion_player* p = NULL;
	CHECK(cgltf_motion_create(&b.data, 0, 0, &p) == CGLTF_MOTION_OK);

	float g0[16];
	ref_world(&b, 0, 0.f, g0);
	float times[] = {-1.f, 0.f, 0.3f, 1.f, 2.25f, 4.f};
	for (cgltf_size i = 0; i < sizeof(times) / sizeof(times[0]); ++i)
	{
		cgltf_motion_pose pose;
		cgltf_motion_matrix motion;
		CHECK(cgltf_motion_evaluate(p, times[i], &pose, &motion) == CGLTF_MOTION_OK);
		check_matrix("nonidentity pinned", pose.world_matrices, g0);
	}
	universal_checks("nonidentity", &b, p, 0, times,
			sizeof(times) / sizeof(times[0]));

	cgltf_motion_release(p);
	builder_free(&b);
	end_case("nonidentity start");
}

/* ------------------------------------------------------------------ */
/* case 7: scale side branches and uniform chain scale                 */
/* ------------------------------------------------------------------ */

static void case_scale_branches(void)
{
	begin_case();
	builder b;
	builder_init(&b, 4);
	/* 0 root (rigid) -> 1 extract node (animated translation only).
	 * 0 -> 2 side branch carrying non-uniform scale -> 3 child. The scale
	 * lives outside the extraction chain and must be accepted. */
	node_link(&b, 0, 1);
	node_link(&b, 0, 2);
	node_link(&b, 2, 3);
	node_set_trs(&b, 2, NULL, NULL, (float[3]){1.f, 2.5f, 0.5f});

	float keys[2] = {0.f, 1.f};
	float tv[6] = {0.f, 0.f, 0.f, 2.f, 0.f, 0.f};
	cgltf_accessor* kt = builder_add_accessor(&b, cgltf_type_scalar, 2, keys, 2);
	cgltf_accessor* vt = builder_add_accessor(&b, cgltf_type_vec3, 2, tv, 6);
	cgltf_animation_sampler* st = builder_add_sampler(&b,
			cgltf_interpolation_type_linear, kt, vt);
	builder_add_channel(&b, st, 1, cgltf_animation_path_type_translation);
	builder_finalize(&b);
	CHECK(cgltf_validate(&b.data) == cgltf_result_success);

	cgltf_motion_player* p = NULL;
	CHECK(cgltf_motion_create(&b.data, 0, 1, &p) == CGLTF_MOTION_OK);

	float times[] = {-0.5f, 0.f, 0.25f, 0.75f, 1.f, 3.f};
	universal_checks("scale", &b, p, 1, times,
			sizeof(times) / sizeof(times[0]));
	cgltf_motion_release(p);
	builder_free(&b);

	/* A unit-scale track on the chain is rigid and therefore accepted. */
	{
		builder bu;
		builder_init(&bu, 1);
		float keys2[2] = {0.f, 1.f};
		float sv[6] = {1.f, 1.f, 1.f, 1.f, 1.f, 1.f};
		cgltf_accessor* ks = builder_add_accessor(&bu, cgltf_type_scalar, 2,
				keys2, 2);
		cgltf_accessor* vs = builder_add_accessor(&bu, cgltf_type_vec3, 2,
				sv, 6);
		cgltf_animation_sampler* ss = builder_add_sampler(&bu,
				cgltf_interpolation_type_linear, ks, vs);
		builder_add_channel(&bu, ss, 0, cgltf_animation_path_type_scale);
		builder_finalize(&bu);
		cgltf_motion_player* pu = NULL;
		CHECK(cgltf_motion_create(&bu.data, 0, 0, &pu) == CGLTF_MOTION_OK);
		cgltf_motion_pose pose;
		CHECK(cgltf_motion_evaluate(pu, 0.5f, &pose, NULL) == CGLTF_MOTION_OK);
		check_scalar("unit scale pose", pose.world_matrices[0], 1.f);
		cgltf_motion_release(pu);
		builder_free(&bu);
	}

	/* Uniform but non-unit scale on the chain is still non-rigid and is
	 * rejected, even though side branches may scale the same way. */
	{
		builder bs;
		builder_init(&bs, 1);
		node_set_trs(&bs, 0, NULL, NULL, (float[3]){2.f, 2.f, 2.f});
		float keys3[2] = {0.f, 1.f};
		float vals[6] = {0, 0, 0, 1, 0, 0};
		cgltf_accessor* ki = builder_add_accessor(&bs, cgltf_type_scalar, 2,
				keys3, 2);
		cgltf_accessor* vo = builder_add_accessor(&bs, cgltf_type_vec3, 2,
				vals, 6);
		cgltf_animation_sampler* s = builder_add_sampler(&bs,
				cgltf_interpolation_type_linear, ki, vo);
		builder_add_channel(&bs, s, 0,
				cgltf_animation_path_type_translation);
		cgltf_motion_player* ps = make_minimal_player(&bs, 0);
		CHECK(ps == NULL);
		builder_free(&bs);
	}

	/* Matrix node without any channel still participates in the hierarchy. */
	{
		builder b2;
		builder_init(&b2, 2);
		float rigid_m[16];
		ref_compose(rigid_m, (float[3]){1.f, 1.f, 0.f},
				(float[4]){0, 0, 0, 1}, (float[3]){1, 1, 1});
		node_set_matrix(&b2, 0, rigid_m);
		node_link(&b2, 0, 1);
		float keys2[2] = {0.f, 1.f};
		float tv[6] = {0.f, 0.f, 0.f, 2.f, 0.f, 0.f};
		cgltf_accessor* kt = builder_add_accessor(&b2, cgltf_type_scalar, 2,
				keys2, 2);
		cgltf_accessor* vt = builder_add_accessor(&b2, cgltf_type_vec3, 2,
				tv, 6);
		cgltf_animation_sampler* st = builder_add_sampler(&b2,
				cgltf_interpolation_type_linear, kt, vt);
		builder_add_channel(&b2, st, 1, cgltf_animation_path_type_translation);
		builder_finalize(&b2);
		CHECK(cgltf_validate(&b2.data) == cgltf_result_success);
		cgltf_motion_player* p2 = NULL;
		CHECK(cgltf_motion_create(&b2.data, 0, 1, &p2) == CGLTF_MOTION_OK);
		float times2[] = {0.f, 0.5f, 1.f, 2.f};
		universal_checks("matrix ancestor", &b2, p2, 1, times2,
				sizeof(times2) / sizeof(times2[0]));
		cgltf_motion_release(p2);
		builder_free(&b2);
	}
	end_case("scale branches");
}

/* ------------------------------------------------------------------ */
/* rejection cases                                                     */
/* ------------------------------------------------------------------ */

static cgltf_motion_player* make_minimal_player(builder* b, cgltf_size extract)
{
	builder_finalize(b);
	cgltf_motion_player* p = NULL;
	cgltf_motion_result r = cgltf_motion_create(&b->data, 0, extract, &p);
	CHECK(r != CGLTF_MOTION_OK);
	return p;
}

static void case_reject_weights(void)
{
	begin_case();
	builder b;
	builder_init(&b, 1);
	/* A weights channel is refused before anything else is inspected. */
	float keys[1] = {0.f};
	float vals[1] = {1.f};
	cgltf_accessor* ki = builder_add_accessor(&b, cgltf_type_scalar, 1, keys, 1);
	cgltf_accessor* vo = builder_add_accessor(&b, cgltf_type_scalar, 1, vals, 1);
	cgltf_animation_sampler* s = builder_add_sampler(&b,
			cgltf_interpolation_type_linear, ki, vo);
	builder_add_channel(&b, s, 0, cgltf_animation_path_type_weights);
	cgltf_motion_player* p = make_minimal_player(&b, 0);
	CHECK(p == NULL);
	builder_free(&b);
	end_case("reject weights");
}

static void case_reject_duplicate(void)
{
	begin_case();
	builder b;
	builder_init(&b, 1);
	float keys[2] = {0.f, 1.f};
	float v1[6] = {0, 0, 0, 1, 0, 0};
	float v2[6] = {0, 0, 0, 0, 1, 0};
	cgltf_accessor* k1 = builder_add_accessor(&b, cgltf_type_scalar, 2, keys, 2);
	cgltf_accessor* o1 = builder_add_accessor(&b, cgltf_type_vec3, 2, v1, 6);
	cgltf_accessor* k2 = builder_add_accessor(&b, cgltf_type_scalar, 2, keys, 2);
	cgltf_accessor* o2 = builder_add_accessor(&b, cgltf_type_vec3, 2, v2, 6);
	cgltf_animation_sampler* s1 = builder_add_sampler(&b,
			cgltf_interpolation_type_linear, k1, o1);
	cgltf_animation_sampler* s2 = builder_add_sampler(&b,
			cgltf_interpolation_type_linear, k2, o2);
	builder_add_channel(&b, s1, 0, cgltf_animation_path_type_translation);
	builder_add_channel(&b, s2, 0, cgltf_animation_path_type_translation);
	cgltf_motion_player* p = make_minimal_player(&b, 0);
	CHECK(p == NULL);
	builder_free(&b);
	end_case("reject duplicate target");
}

static void case_reject_bad_keys(void)
{
	begin_case();
	builder b;
	builder_init(&b, 1);
	float keys[3] = {0.f, 1.f, 1.f}; /* not strictly ascending */
	float vals[9] = {0, 0, 0, 1, 0, 0, 2, 0, 0};
	cgltf_accessor* ki = builder_add_accessor(&b, cgltf_type_scalar, 3, keys, 3);
	cgltf_accessor* vo = builder_add_accessor(&b, cgltf_type_vec3, 3, vals, 9);
	cgltf_animation_sampler* s = builder_add_sampler(&b,
			cgltf_interpolation_type_step, ki, vo);
	builder_add_channel(&b, s, 0, cgltf_animation_path_type_translation);
	cgltf_motion_player* p = make_minimal_player(&b, 0);
	CHECK(p == NULL);
	builder_free(&b);
	end_case("reject bad keys");
}

static void case_reject_matrix_animated(void)
{
	begin_case();
	builder b;
	builder_init(&b, 1);
	float m[16];
	ref_identity(m);
	node_set_matrix(&b, 0, m);
	float keys[2] = {0.f, 1.f};
	float vals[6] = {0, 0, 0, 1, 0, 0};
	cgltf_accessor* ki = builder_add_accessor(&b, cgltf_type_scalar, 2, keys, 2);
	cgltf_accessor* vo = builder_add_accessor(&b, cgltf_type_vec3, 2, vals, 6);
	cgltf_animation_sampler* s = builder_add_sampler(&b,
			cgltf_interpolation_type_linear, ki, vo);
	builder_add_channel(&b, s, 0, cgltf_animation_path_type_translation);
	cgltf_motion_player* p = make_minimal_player(&b, 0);
	CHECK(p == NULL);
	builder_free(&b);
	end_case("reject matrix animated");
}

static void case_reject_non_rigid(void)
{
	begin_case();

	/* Non-uniform scale on an ancestor. */
	{
		builder b;
		builder_init(&b, 2);
		node_set_trs(&b, 0, NULL, NULL, (float[3]){1.f, 2.f, 1.f});
		node_link(&b, 0, 1);
		float keys[2] = {0.f, 1.f};
		float vals[6] = {0, 0, 0, 1, 0, 0};
		cgltf_accessor* ki = builder_add_accessor(&b, cgltf_type_scalar, 2,
				keys, 2);
		cgltf_accessor* vo = builder_add_accessor(&b, cgltf_type_vec3, 2,
				vals, 6);
		cgltf_animation_sampler* s = builder_add_sampler(&b,
				cgltf_interpolation_type_linear, ki, vo);
		builder_add_channel(&b, s, 1, cgltf_animation_path_type_translation);
		cgltf_motion_player* p = make_minimal_player(&b, 1);
		CHECK(p == NULL);
		builder_free(&b);
	}

	/* Shear baked into an ancestor matrix node. */
	{
		builder b;
		builder_init(&b, 2);
		float m[16];
		ref_identity(m);
		m[4] = 0.5f; /* shear term */
		node_set_matrix(&b, 0, m);
		node_link(&b, 0, 1);
		float keys[2] = {0.f, 1.f};
		float vals[6] = {0, 0, 0, 1, 0, 0};
		cgltf_accessor* ki = builder_add_accessor(&b, cgltf_type_scalar, 2,
				keys, 2);
		cgltf_accessor* vo = builder_add_accessor(&b, cgltf_type_vec3, 2,
				vals, 6);
		cgltf_animation_sampler* s = builder_add_sampler(&b,
				cgltf_interpolation_type_linear, ki, vo);
		builder_add_channel(&b, s, 1, cgltf_animation_path_type_translation);
		cgltf_motion_player* p = make_minimal_player(&b, 1);
		CHECK(p == NULL);
		builder_free(&b);
	}

	/* Non-uniform animated scale key on the extraction node. */
	{
		builder b;
		builder_init(&b, 1);
		float keys[2] = {0.f, 1.f};
		float vals[6] = {1, 1, 1, 2, 2, 3};
		cgltf_accessor* ki = builder_add_accessor(&b, cgltf_type_scalar, 2,
				keys, 2);
		cgltf_accessor* vo = builder_add_accessor(&b, cgltf_type_vec3, 2,
				vals, 6);
		cgltf_animation_sampler* s = builder_add_sampler(&b,
				cgltf_interpolation_type_linear, ki, vo);
		builder_add_channel(&b, s, 0, cgltf_animation_path_type_scale);
		cgltf_motion_player* p = make_minimal_player(&b, 0);
		CHECK(p == NULL);
		builder_free(&b);
	}
	end_case("reject non rigid");
}

static void case_reject_empty_and_zero(void)
{
	begin_case();

	/* Empty animation: no samplers and no channels. */
	{
		builder b;
		builder_init(&b, 1);
		builder_finalize(&b);
		cgltf_motion_player* p = NULL;
		cgltf_motion_result r = cgltf_motion_create(&b.data, 0, 0, &p);
		CHECK(r == CGLTF_MOTION_ERROR_EMPTY_ANIMATION);
		CHECK(p == NULL);
		builder_free(&b);
	}

	/* Zero duration: a single key cannot define a loop interval. */
	{
		builder b;
		builder_init(&b, 1);
		float keys[1] = {3.f};
		float vals[3] = {0, 0, 0};
		cgltf_accessor* ki = builder_add_accessor(&b, cgltf_type_scalar, 1,
				keys, 1);
		cgltf_accessor* vo = builder_add_accessor(&b, cgltf_type_vec3, 1,
				vals, 3);
		cgltf_animation_sampler* s = builder_add_sampler(&b,
				cgltf_interpolation_type_step, ki, vo);
		builder_add_channel(&b, s, 0, cgltf_animation_path_type_translation);
		cgltf_motion_player* p = make_minimal_player(&b, 0);
		CHECK(p == NULL);
		builder_free(&b);
	}
	end_case("reject empty and zero duration");
}

static void case_reject_arguments(void)
{
	begin_case();
	builder b;
	builder_init(&b, 1);
	float keys[2] = {0.f, 1.f};
	float vals[6] = {0, 0, 0, 1, 0, 0};
	cgltf_accessor* ki = builder_add_accessor(&b, cgltf_type_scalar, 2, keys, 2);
	cgltf_accessor* vo = builder_add_accessor(&b, cgltf_type_vec3, 2, vals, 6);
	cgltf_animation_sampler* s = builder_add_sampler(&b,
			cgltf_interpolation_type_linear, ki, vo);
	builder_add_channel(&b, s, 0, cgltf_animation_path_type_translation);
	builder_finalize(&b);

	cgltf_motion_player* p = NULL;
	CHECK(cgltf_motion_create(NULL, 0, 0, &p) ==
			CGLTF_MOTION_ERROR_INVALID_ARGUMENT);
	CHECK(cgltf_motion_create(&b.data, 0, 0, NULL) ==
			CGLTF_MOTION_ERROR_INVALID_ARGUMENT);
	CHECK(cgltf_motion_create(&b.data, 7, 0, &p) ==
			CGLTF_MOTION_ERROR_ANIMATION_INDEX);
	CHECK(cgltf_motion_create(&b.data, 0, 9, &p) ==
			CGLTF_MOTION_ERROR_NODE_INDEX);
	CHECK(p == NULL);

	CHECK(cgltf_motion_create(&b.data, 0, 0, &p) == CGLTF_MOTION_OK);
	cgltf_motion_matrix out;
	CHECK(cgltf_motion_evaluate(p, NAN, NULL, &out) ==
			CGLTF_MOTION_ERROR_NON_FINITE_TIME);
	CHECK(cgltf_motion_evaluate(p, INFINITY, NULL, &out) ==
			CGLTF_MOTION_ERROR_NON_FINITE_TIME);
	CHECK(cgltf_motion_delta(p, 0.f, NAN, &out) ==
			CGLTF_MOTION_ERROR_NON_FINITE_TIME);
	cgltf_motion_release(p);
	cgltf_motion_release(NULL); /* must be safe */
	builder_free(&b);
	end_case("reject arguments");
}

/* ------------------------------------------------------------------ */
/* node array order must not matter: child indices precede parents     */
/* ------------------------------------------------------------------ */

static void case_unordered_hierarchy(void)
{
	begin_case();
	builder b;
	builder_init(&b, 3);
	/* Logical hierarchy root -> mid -> leaf, but stored in reverse index
	 * order: node 2 is the root, 1 its child, 0 the leaf. */
	node_set_trs(&b, 2, (float[3]){1.f, 2.f, 0.f}, NULL, NULL);
	node_link(&b, 2, 1);
	node_link(&b, 1, 0);

	float keys[2] = {0.f, 1.f};
	float tv[6] = {0.f, 0.f, 0.f, 3.f, 0.f, 0.f};
	float q0[4] = {0.f, 0.f, 0.f, 1.f};
	float q1[4];
	quat_from_axis_z(1.5707963f, q1);
	float qv[8];
	memcpy(qv, q0, sizeof(q0));
	memcpy(qv + 4, q1, sizeof(q1));

	cgltf_accessor* kt = builder_add_accessor(&b, cgltf_type_scalar, 2, keys, 2);
	cgltf_accessor* vt = builder_add_accessor(&b, cgltf_type_vec3, 2, tv, 6);
	cgltf_accessor* kq = builder_add_accessor(&b, cgltf_type_scalar, 2, keys, 2);
	cgltf_accessor* vq = builder_add_accessor(&b, cgltf_type_vec4, 2, qv, 8);
	cgltf_animation_sampler* st = builder_add_sampler(&b,
			cgltf_interpolation_type_linear, kt, vt);
	cgltf_animation_sampler* sq = builder_add_sampler(&b,
			cgltf_interpolation_type_linear, kq, vq);
	builder_add_channel(&b, st, 1, cgltf_animation_path_type_translation);
	builder_add_channel(&b, sq, 1, cgltf_animation_path_type_rotation);
	builder_finalize(&b);
	CHECK(cgltf_validate(&b.data) == cgltf_result_success);

	cgltf_motion_player* p = NULL;
	CHECK(cgltf_motion_create(&b.data, 0, 0, &p) == CGLTF_MOTION_OK);
	float times[] = {-2.f, -0.5f, 0.f, 0.33f, 1.f, 1.66f, 3.f, 4.25f};
	universal_checks("unordered", &b, p, 0, times,
			sizeof(times) / sizeof(times[0]));
	cgltf_motion_release(p);
	builder_free(&b);
	end_case("unordered hierarchy");
}

/* ------------------------------------------------------------------ */
/* multi-chunk delta recomposition and two-player storage isolation    */
/* ------------------------------------------------------------------ */

static void case_delta_partitions(void)
{
	begin_case();
	builder b;
	builder_init(&b, 2);
	node_set_trs(&b, 0, (float[3]){0.f, 0.f, 0.f}, NULL, NULL);
	node_link(&b, 0, 1);
	float keys[2] = {0.f, 1.f};
	float tv[6] = {0.f, 0.f, 0.f, 1.f, 0.f, 0.f};
	float q0[4] = {0.f, 0.f, 0.f, 1.f};
	float q1[4];
	quat_from_axis_z(2.0943951f, q1); /* 120 degrees */
	float qv[8];
	memcpy(qv, q0, sizeof(q0));
	memcpy(qv + 4, q1, sizeof(q1));
	cgltf_accessor* kt = builder_add_accessor(&b, cgltf_type_scalar, 2, keys, 2);
	cgltf_accessor* vt = builder_add_accessor(&b, cgltf_type_vec3, 2, tv, 6);
	cgltf_accessor* kq = builder_add_accessor(&b, cgltf_type_scalar, 2, keys, 2);
	cgltf_accessor* vq = builder_add_accessor(&b, cgltf_type_vec4, 2, qv, 8);
	cgltf_animation_sampler* st = builder_add_sampler(&b,
			cgltf_interpolation_type_linear, kt, vt);
	cgltf_animation_sampler* sq = builder_add_sampler(&b,
			cgltf_interpolation_type_linear, kq, vq);
	builder_add_channel(&b, st, 1, cgltf_animation_path_type_translation);
	builder_add_channel(&b, sq, 1, cgltf_animation_path_type_rotation);
	builder_finalize(&b);
	CHECK(cgltf_validate(&b.data) == cgltf_result_success);

	cgltf_motion_player* p = NULL;
	CHECK(cgltf_motion_create(&b.data, 0, 1, &p) == CGLTF_MOTION_OK);

	/* Unequal, out-of-order-spanning partition including a negative time
	 * and multi-loop endpoints. */
	float ts[] = {-1.25f, -0.3f, 0.7f, 2.4f, 3.1f};
	cgltf_size n = sizeof(ts) / sizeof(ts[0]);
	cgltf_motion_matrix acc;
	ref_identity(acc.m);
	for (cgltf_size i = 0; i + 1 < n; ++i)
	{
		cgltf_motion_matrix step;
		CHECK(cgltf_motion_delta(p, ts[i], ts[i + 1], &step) == CGLTF_MOTION_OK);
		ref_mul(acc.m, acc.m, step.m);
	}
	cgltf_motion_matrix whole;
	CHECK(cgltf_motion_delta(p, ts[0], ts[n - 1], &whole) == CGLTF_MOTION_OK);
	check_matrix("multi chunk delta", acc.m, whole.m);

	/* The same product against the closed-form inverse(M(from))*M(to). */
	cgltf_motion_matrix mf, mt;
	CHECK(cgltf_motion_evaluate(p, ts[0], NULL, &mf) == CGLTF_MOTION_OK);
	CHECK(cgltf_motion_evaluate(p, ts[n - 1], NULL, &mt) == CGLTF_MOTION_OK);
	float inv_mf[16], closed[16];
	ref_inverse(inv_mf, mf.m);
	ref_mul(closed, inv_mf, mt.m);
	check_matrix("closed form delta", whole.m, closed);

	/* Two players keep independent pose storage under interleaved use. */
	cgltf_motion_player* q = NULL;
	CHECK(cgltf_motion_create(&b.data, 0, 0, &q) == CGLTF_MOTION_OK);
	cgltf_motion_pose pp, pq;
	cgltf_motion_matrix mp, mq;
	CHECK(cgltf_motion_evaluate(p, 0.37f, &pp, &mp) == CGLTF_MOTION_OK);
	CHECK(cgltf_motion_evaluate(q, 0.91f, &pq, &mq) == CGLTF_MOTION_OK);
	CHECK(pp.world_matrices != pq.world_matrices);
	/* Re-querying q must not have altered p's pinned node values. */
	float g0p[16];
	ref_world(&b, 1, 0.f, g0p);
	check_matrix("isolated player p", pp.world_matrices + 1 * 16, g0p);
	float g0q[16];
	ref_world(&b, 0, 0.f, g0q);
	check_matrix("isolated player q", pq.world_matrices + 0 * 16, g0q);
	cgltf_motion_release(q);
	cgltf_motion_release(p);
	builder_free(&b);
	end_case("delta partitions and isolation");
}

/* ------------------------------------------------------------------ */
/* the player must not mutate cgltf_node, accessor or buffer memory     */
/* ------------------------------------------------------------------ */

static void case_source_immutability(void)
{
	begin_case();
	builder b;
	builder_init(&b, 2);
	node_set_trs(&b, 0, (float[3]){3.f, 0.f, 0.f}, NULL, NULL);
	node_link(&b, 0, 1);
	float keys[2] = {0.f, 1.f};
	float tv[6] = {0.f, 0.f, 0.f, 2.f, 0.f, 0.f};
	float q0[4] = {0.f, 0.f, 0.f, 1.f};
	float q1[4];
	quat_from_axis_z(1.5707963f, q1);
	float qv[8];
	memcpy(qv, q0, sizeof(q0));
	memcpy(qv + 4, q1, sizeof(q1));
	cgltf_accessor* kt = builder_add_accessor(&b, cgltf_type_scalar, 2, keys, 2);
	cgltf_accessor* vt = builder_add_accessor(&b, cgltf_type_vec3, 2, tv, 6);
	cgltf_accessor* kq = builder_add_accessor(&b, cgltf_type_scalar, 2, keys, 2);
	cgltf_accessor* vq = builder_add_accessor(&b, cgltf_type_vec4, 2, qv, 8);
	cgltf_animation_sampler* st = builder_add_sampler(&b,
			cgltf_interpolation_type_linear, kt, vt);
	cgltf_animation_sampler* sq = builder_add_sampler(&b,
			cgltf_interpolation_type_linear, kq, vq);
	builder_add_channel(&b, st, 1, cgltf_animation_path_type_translation);
	builder_add_channel(&b, sq, 1, cgltf_animation_path_type_rotation);
	builder_finalize(&b);

	/* Snapshot everything the spec says must stay untouched. */
	float storage_snapshot[MAX_STORAGE_FLOATS];
	memcpy(storage_snapshot, b.storage,
			b.storage_floats_used * sizeof(float));
	float node_snapshot[MAX_NODES][10];
	for (cgltf_size i = 0; i < b.nodes_count; ++i)
	{
		memcpy(node_snapshot[i], b.nodes[i].translation, 3 * sizeof(float));
		memcpy(node_snapshot[i] + 3, b.nodes[i].rotation, 4 * sizeof(float));
		memcpy(node_snapshot[i] + 7, b.nodes[i].scale, 3 * sizeof(float));
	}

	cgltf_motion_player* p = NULL;
	CHECK(cgltf_motion_create(&b.data, 0, 1, &p) == CGLTF_MOTION_OK);
	float probe[] = {-4.f, -1.0001f, 0.f, 0.3f, 1.f, 2.5f, 7.f};
	for (cgltf_size i = 0; i < sizeof(probe) / sizeof(probe[0]); ++i)
	{
		cgltf_motion_pose pose;
		cgltf_motion_matrix motion, delta;
		CHECK(cgltf_motion_evaluate(p, probe[i], &pose, &motion)
				== CGLTF_MOTION_OK);
		CHECK(cgltf_motion_delta(p, probe[0], probe[i], &delta)
				== CGLTF_MOTION_OK);
		(void)pose;
	}

	for (cgltf_size i = 0; i < b.storage_floats_used; ++i)
		check_scalar("buffer float unchanged", b.storage[i], storage_snapshot[i]);
	for (cgltf_size i = 0; i < b.nodes_count; ++i)
	{
		for (int j = 0; j < 3; ++j)
			check_scalar("node translation unchanged",
					b.nodes[i].translation[j], node_snapshot[i][j]);
		for (int j = 0; j < 4; ++j)
			check_scalar("node rotation unchanged",
					b.nodes[i].rotation[j], node_snapshot[i][3 + j]);
		for (int j = 0; j < 3; ++j)
			check_scalar("node scale unchanged",
					b.nodes[i].scale[j], node_snapshot[i][7 + j]);
	}
	cgltf_motion_release(p);
	builder_free(&b);
	end_case("source immutability");
}

int main(void)
{
	case_motion_gltf();
	case_cubic_rotation();
	case_linear_antipodal();
	case_mixed_key_times();
	case_negative_multiloop();
	case_nonidentity_start();
	case_scale_branches();
	case_unordered_hierarchy();
	case_delta_partitions();
	case_source_immutability();
	case_reject_weights();
	case_reject_duplicate();
	case_reject_bad_keys();
	case_reject_matrix_animated();
	case_reject_non_rigid();
	case_reject_empty_and_zero();
	case_reject_arguments();

	if (g_failures != 0)
	{
		printf("ROOT_MOTION_FAILURE failures=%d cases=%d\n",
				g_failures, g_cases);
		return 1;
	}
	printf("ROOT_MOTION_OK cases=%d\n", g_cases);
	return 0;
}
