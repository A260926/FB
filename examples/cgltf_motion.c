/*
 * cgltf_motion.c - implementation of the windowless animation player and
 * looping root-motion extraction declared in cgltf_motion.h.
 *
 * The implementation only reads cgltf data: nothing is ever written into
 * cgltf_node, accessor contents or buffer memory. All working storage is
 * owned by the player and allocated once at create time.
 */
#include "cgltf_motion.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#ifndef CGLTF_MOTION_FLT_EPS
#define CGLTF_MOTION_FLT_EPS 1e-5f
#endif

typedef struct motion_channel {
	cgltf_node* node;
	cgltf_animation_path_type path;
	cgltf_interpolation_type interpolation;
	cgltf_size key_count;
	cgltf_size components;       /* 3 for translation/scale, 4 for rotation */
	cgltf_size value_stride;    /* components, or 3 * components for cubic */
	cgltf_float* keys;          /* owned, key_count entries */
	cgltf_float* values;        /* owned, key_count * value_stride entries */
} motion_channel;

typedef struct motion_node {
	cgltf_node* node;
	cgltf_size parent;          /* player node index, or CGLTF_MOTION_NO_NODE */
	cgltf_float static_local[16];
	cgltf_float local[16];
	cgltf_float world[16];
	cgltf_bool animated_translation;
	cgltf_bool animated_rotation;
	cgltf_bool animated_scale;
	cgltf_float translation[3];
	cgltf_float rotation[4];
	cgltf_float scale[3];
} motion_node;

#define CGLTF_MOTION_NO_NODE ((cgltf_size)(-1))

struct cgltf_motion_player {
	const cgltf_data* data;
	cgltf_size nodes_count;
	motion_node* nodes;
	cgltf_size channels_count;
	motion_channel* channels;
	cgltf_size extract_index;
	cgltf_float time_begin;
	cgltf_float time_end;
	cgltf_float g0[16];         /* G(a) */
	cgltf_float inv_g0[16];
	cgltf_float cycle[16];      /* C = inverse(G0) * G(b) */
	cgltf_float* pose_storage;  /* nodes_count * 16 floats */
};

/* ------------------------------------------------------------------ */
/* small matrix / quaternion helpers, all column major 4x4            */
/* ------------------------------------------------------------------ */

static cgltf_bool m_is_finite(const cgltf_float* m)
{
	for (int i = 0; i < 16; ++i)
	{
		if (!isfinite(m[i]))
			return 0;
	}
	return 1;
}

static void m_identity(cgltf_float* out)
{
	memset(out, 0, 16 * sizeof(cgltf_float));
	out[0] = out[5] = out[10] = out[15] = 1.f;
}

/* out = a * b */
static void m_multiply(cgltf_float* out, const cgltf_float* a, const cgltf_float* b)
{
	cgltf_float tmp[16];
	for (int col = 0; col < 4; ++col)
	{
		for (int row = 0; row < 4; ++row)
		{
			tmp[col * 4 + row] =
				a[0 * 4 + row] * b[col * 4 + 0] +
				a[1 * 4 + row] * b[col * 4 + 1] +
				a[2 * 4 + row] * b[col * 4 + 2] +
				a[3 * 4 + row] * b[col * 4 + 3];
		}
	}
	memcpy(out, tmp, sizeof(tmp));
}

static cgltf_bool m_inverse(cgltf_float* out, const cgltf_float* in)
{
	cgltf_float s[6];
	s[0] = in[0] * in[5] - in[1] * in[4];
	s[1] = in[0] * in[6] - in[2] * in[4];
	s[2] = in[0] * in[7] - in[3] * in[4];
	s[3] = in[1] * in[6] - in[2] * in[5];
	s[4] = in[1] * in[7] - in[3] * in[5];
	s[5] = in[2] * in[7] - in[3] * in[6];

	cgltf_float c[6];
	c[0] = in[8] * in[13] - in[9] * in[12];
	c[1] = in[8] * in[14] - in[10] * in[12];
	c[2] = in[8] * in[15] - in[11] * in[12];
	c[3] = in[9] * in[14] - in[10] * in[13];
	c[4] = in[9] * in[15] - in[11] * in[13];
	c[5] = in[10] * in[15] - in[11] * in[14];

	cgltf_float det = s[0] * c[5] - s[1] * c[4] + s[2] * c[3] +
		s[3] * c[2] - s[4] * c[1] + s[5] * c[0];
	if (!isfinite(det) || fabsf(det) < 1e-20f)
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

/* Integer power by squaring; exponent may be negative. */
static void m_power(cgltf_float* out, const cgltf_float* base, long long exponent)
{
	cgltf_float b[16];
	cgltf_float tmp[16];

	if (exponent < 0)
	{
		if (!m_inverse(b, base))
		{
			m_identity(out);
			return;
		}
		exponent = -exponent;
	}
	else
	{
		memcpy(b, base, sizeof(b));
	}

	m_identity(out);
	memcpy(tmp, b, sizeof(tmp));
	while (exponent > 0)
	{
		if (exponent & 1)
		{
			m_multiply(out, out, tmp);
		}
		exponent >>= 1;
		if (exponent > 0)
			m_multiply(tmp, tmp, tmp);
	}
}

static void quat_normalize(cgltf_float* q)
{
	cgltf_float len = sqrtf(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
	if (len < 1e-20f)
	{
		q[0] = q[1] = q[2] = 0.f;
		q[3] = 1.f;
		return;
	}
	cgltf_float inv = 1.f / len;
	for (int i = 0; i < 4; ++i)
		q[i] *= inv;
}

/* Shortest-arc slerp. Sign flipping of the second key is the glTF LINEAR
 * rule and is applied to the interpolated pair only, never to stored data. */
static void quat_slerp_shortest(const cgltf_float* a, const cgltf_float* b_in,
		cgltf_float alpha, cgltf_float* out)
{
	cgltf_float b[4];
	memcpy(b, b_in, sizeof(b));

	cgltf_float dot = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3];
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
		if (dot > 1.f)
			dot = 1.f;
		if (dot < -1.f)
			dot = -1.f;
		cgltf_float theta = acosf(dot);
		cgltf_float sin_theta = sinf(theta);
		cgltf_float w0 = sinf((1.f - alpha) * theta) / sin_theta;
		cgltf_float w1 = sinf(alpha * theta) / sin_theta;
		for (int i = 0; i < 4; ++i)
			out[i] = a[i] * w0 + b[i] * w1;
	}
	quat_normalize(out);
}

static void trs_compose(cgltf_float* out, const cgltf_float* t,
		const cgltf_float* r, const cgltf_float* s)
{
	cgltf_float qx = r[0], qy = r[1], qz = r[2], qw = r[3];
	cgltf_float sx = s[0], sy = s[1], sz = s[2];

	out[0] = (1.f - 2.f * qy * qy - 2.f * qz * qz) * sx;
	out[1] = (2.f * qx * qy + 2.f * qz * qw) * sx;
	out[2] = (2.f * qx * qz - 2.f * qy * qw) * sx;
	out[3] = 0.f;

	out[4] = (2.f * qx * qy - 2.f * qz * qw) * sy;
	out[5] = (1.f - 2.f * qx * qx - 2.f * qz * qz) * sy;
	out[6] = (2.f * qy * qz + 2.f * qx * qw) * sy;
	out[7] = 0.f;

	out[8] = (2.f * qx * qz + 2.f * qy * qw) * sz;
	out[9] = (2.f * qy * qz - 2.f * qx * qw) * sz;
	out[10] = (1.f - 2.f * qx * qx - 2.f * qy * qy) * sz;
	out[11] = 0.f;

	out[12] = t[0];
	out[13] = t[1];
	out[14] = t[2];
	out[15] = 1.f;
}

/* Rigid = orthonormal 3x3 (no scale, no shear) with positive determinant
 * (no reflection) and an affine last row. This helper tests the literal
 * static matrix of a matrix node on the extraction chain; TRS nodes are
 * checked structurally via scale_is_identity(). */
static cgltf_bool matrix_is_rigid(const cgltf_float* m)
{
	if (!m_is_finite(m))
		return 0;
	if (fabsf(m[3]) > CGLTF_MOTION_FLT_EPS || fabsf(m[7]) > CGLTF_MOTION_FLT_EPS ||
			fabsf(m[11]) > CGLTF_MOTION_FLT_EPS || fabsf(m[15] - 1.f) > CGLTF_MOTION_FLT_EPS)
		return 0;

	for (int col = 0; col < 3; ++col)
	{
		cgltf_float len2 = 0.f;
		for (int row = 0; row < 3; ++row)
			len2 += m[col * 4 + row] * m[col * 4 + row];
		if (fabsf(len2 - 1.f) > 1e-4f)
			return 0;
		for (int other = col + 1; other < 3; ++other)
		{
			cgltf_float dot = 0.f;
			for (int row = 0; row < 3; ++row)
				dot += m[col * 4 + row] * m[other * 4 + row];
			if (fabsf(dot) > 1e-4f)
				return 0;
		}
	}

	cgltf_float det =
		m[0] * (m[5] * m[10] - m[6] * m[9]) -
		m[4] * (m[1] * m[10] - m[2] * m[9]) +
		m[8] * (m[1] * m[6] - m[2] * m[5]);
	return det > 0.f;
}

static cgltf_bool scale_is_identity(const cgltf_float* s)
{
	return isfinite(s[0]) && isfinite(s[1]) && isfinite(s[2]) &&
			fabsf(s[0] - 1.f) <= 1e-5f &&
			fabsf(s[1] - 1.f) <= 1e-5f &&
			fabsf(s[2] - 1.f) <= 1e-5f;
}

/* ------------------------------------------------------------------ */
/* accessor unpacking                                                  */
/* ------------------------------------------------------------------ */

static cgltf_motion_result unpack_accessor(const cgltf_accessor* accessor,
		cgltf_float** out_data, cgltf_size* out_count)
{
	if (accessor == NULL)
		return CGLTF_MOTION_ERROR_BAD_ACCESSOR;

	cgltf_size available = cgltf_accessor_unpack_floats(accessor, NULL, 0);
	if (available == 0)
		return CGLTF_MOTION_ERROR_BAD_ACCESSOR;

	cgltf_float* data = (cgltf_float*)malloc(available * sizeof(cgltf_float));
	if (data == NULL)
		return CGLTF_MOTION_ERROR_OUT_OF_MEMORY;

	if (cgltf_accessor_unpack_floats(accessor, data, available) != available)
	{
		free(data);
		return CGLTF_MOTION_ERROR_BAD_ACCESSOR;
	}

	*out_data = data;
	*out_count = available;
	return CGLTF_MOTION_OK;
}

/* ------------------------------------------------------------------ */
/* channel sampling                                                    */
/* ------------------------------------------------------------------ */

/* Find the segment [lo, lo+1] owning time t; lo == key_count-1 when t is
 * at or past the last key. Out-of-range times are clamped by the caller
 * through the returned alpha. */
static cgltf_size find_segment(const motion_channel* channel, cgltf_float t)
{
	cgltf_size lo = 0;
	cgltf_size hi = channel->key_count - 1;
	while (hi - lo > 1)
	{
		cgltf_size mid = lo + (hi - lo) / 2;
		if (channel->keys[mid] <= t)
			lo = mid;
		else
			hi = mid;
	}
	return lo;
}

static void sample_channel(const motion_channel* channel, cgltf_float t,
		cgltf_float* out)
{
	cgltf_size comps = channel->components;
	/* For CUBICSPLINE each key stores [in-tangent, value, out-tangent], so
	 * the key's actual value starts one element block later. */
	cgltf_size value_offset =
		channel->interpolation == cgltf_interpolation_type_cubic_spline
			? comps : 0;

	if (t <= channel->keys[0])
	{
		memcpy(out, channel->values + value_offset,
				comps * sizeof(cgltf_float));
		if (channel->path == cgltf_animation_path_type_rotation)
			quat_normalize(out);
		return;
	}

	cgltf_size last = channel->key_count - 1;
	if (t >= channel->keys[last])
	{
		memcpy(out,
				channel->values + last * channel->value_stride + value_offset,
				comps * sizeof(cgltf_float));
		if (channel->path == cgltf_animation_path_type_rotation)
			quat_normalize(out);
		return;
	}

	cgltf_size i = find_segment(channel, t);
	cgltf_float t0 = channel->keys[i];
	cgltf_float t1 = channel->keys[i + 1];
	cgltf_float dt = t1 - t0;
	cgltf_float alpha = (t - t0) / dt;

	const cgltf_float* base0 = channel->values + i * channel->value_stride;
	const cgltf_float* base1 =
		channel->values + (i + 1) * channel->value_stride;

	if (channel->interpolation == cgltf_interpolation_type_step)
	{
		memcpy(out, base0 + value_offset, comps * sizeof(cgltf_float));
		if (channel->path == cgltf_animation_path_type_rotation)
			quat_normalize(out);
		return;
	}

	if (channel->interpolation == cgltf_interpolation_type_linear)
	{
		if (channel->path == cgltf_animation_path_type_rotation)
		{
			quat_slerp_shortest(base0, base1, alpha, out);
			return;
		}
		for (cgltf_size c = 0; c < comps; ++c)
			out[c] = base0[c] * (1.f - alpha) + base1[c] * alpha;
		return;
	}

	/* CUBICSPLINE: per-key layout [in-tangent, value, out-tangent], each
	 * comps floats. Tangents are scaled by the real inter-key seconds as
	 * the glTF Hermite formula demands; keys and tangents are consumed
	 * verbatim, no per-key sign flipping. */
	const cgltf_float* v0 = base0 + comps;
	const cgltf_float* out0 = base0 + 2 * comps;
	const cgltf_float* in1 = base1;
	const cgltf_float* v1 = base1 + comps;

	cgltf_float a = alpha;
	cgltf_float a2 = a * a;
	cgltf_float a3 = a2 * a;
	cgltf_float h00 = 2.f * a3 - 3.f * a2 + 1.f;
	cgltf_float h10 = a3 - 2.f * a2 + a;
	cgltf_float h01 = -2.f * a3 + 3.f * a2;
	cgltf_float h11 = a3 - a2;

	for (cgltf_size c = 0; c < comps; ++c)
	{
		out[c] = h00 * v0[c] +
			h10 * dt * out0[c] +
			h01 * v1[c] +
			h11 * dt * in1[c];
	}
	if (channel->path == cgltf_animation_path_type_rotation)
		quat_normalize(out);
}

/* ------------------------------------------------------------------ */
/* hierarchy evaluation                                                */
/* ------------------------------------------------------------------ */

static void evaluate_sampled(cgltf_motion_player* player, cgltf_float time)
{
	for (cgltf_size i = 0; i < player->nodes_count; ++i)
	{
		motion_node* mn = &player->nodes[i];
		cgltf_node* node = mn->node;

		if (node->has_translation)
			memcpy(mn->translation, node->translation, 3 * sizeof(cgltf_float));
		else
			mn->translation[0] = mn->translation[1] = mn->translation[2] = 0.f;

		if (node->has_rotation)
			memcpy(mn->rotation, node->rotation, 4 * sizeof(cgltf_float));
		else
		{
			mn->rotation[0] = mn->rotation[1] = mn->rotation[2] = 0.f;
			mn->rotation[3] = 1.f;
		}

		if (node->has_scale)
			memcpy(mn->scale, node->scale, 3 * sizeof(cgltf_float));
		else
			mn->scale[0] = mn->scale[1] = mn->scale[2] = 1.f;

		mn->animated_translation = 0;
		mn->animated_rotation = 0;
		mn->animated_scale = 0;
	}

	for (cgltf_size i = 0; i < player->channels_count; ++i)
	{
		const motion_channel* channel = &player->channels[i];
		cgltf_size node_index = (cgltf_size)(channel->node - player->data->nodes);
		motion_node* mn = &player->nodes[node_index];
		cgltf_float tmp[4];

		sample_channel(channel, time, tmp);

		switch (channel->path)
		{
			case cgltf_animation_path_type_translation:
				memcpy(mn->translation, tmp, 3 * sizeof(cgltf_float));
				mn->animated_translation = 1;
				break;
			case cgltf_animation_path_type_rotation:
				memcpy(mn->rotation, tmp, 4 * sizeof(cgltf_float));
				mn->animated_rotation = 1;
				break;
			case cgltf_animation_path_type_scale:
				memcpy(mn->scale, tmp, 3 * sizeof(cgltf_float));
				mn->animated_scale = 1;
				break;
			default:
				break;
		}
	}

	for (cgltf_size i = 0; i < player->nodes_count; ++i)
	{
		motion_node* mn = &player->nodes[i];
		if (mn->node->has_matrix)
		{
			memcpy(mn->local, mn->static_local, 16 * sizeof(cgltf_float));
		}
		else
		{
			trs_compose(mn->local, mn->translation, mn->rotation, mn->scale);
		}
	}

	/* Walk parent chains explicitly so node array order does not matter. */
	for (cgltf_size i = 0; i < player->nodes_count; ++i)
	{
		motion_node* mn = &player->nodes[i];
		cgltf_size depth = 0;
		const motion_node* cur = mn;
		while (cur != NULL)
		{
			++depth;
			if (cur->parent == CGLTF_MOTION_NO_NODE)
				break;
			cur = &player->nodes[cur->parent];
		}

		cgltf_float chain[depth][16];
		depth = 0;
		cur = mn;
		while (cur != NULL)
		{
			memcpy(chain[depth], cur->local, 16 * sizeof(cgltf_float));
			++depth;
			if (cur->parent == CGLTF_MOTION_NO_NODE)
				break;
			cur = &player->nodes[cur->parent];
		}

		m_identity(mn->world);
		for (cgltf_size d = depth; d > 0; --d)
			m_multiply(mn->world, mn->world, chain[d - 1]);
	}
}

/* ------------------------------------------------------------------ */
/* create / validation                                                 */
/* ------------------------------------------------------------------ */

static void free_player_content(cgltf_motion_player* player)
{
	if (player == NULL)
		return;
	if (player->channels != NULL)
	{
		for (cgltf_size i = 0; i < player->channels_count; ++i)
		{
			free(player->channels[i].keys);
			free(player->channels[i].values);
		}
		free(player->channels);
	}
	free(player->nodes);
	free(player->pose_storage);
	free(player);
}

const char* cgltf_motion_result_string(cgltf_motion_result result)
{
	switch (result)
	{
		case CGLTF_MOTION_OK:
			return "ok";
		case CGLTF_MOTION_ERROR_INVALID_ARGUMENT:
			return "invalid argument";
		case CGLTF_MOTION_ERROR_OUT_OF_MEMORY:
			return "out of memory";
		case CGLTF_MOTION_ERROR_ANIMATION_INDEX:
			return "animation index out of range";
		case CGLTF_MOTION_ERROR_NODE_INDEX:
			return "node index out of range";
		case CGLTF_MOTION_ERROR_EMPTY_ANIMATION:
			return "animation has no samplers or channels";
		case CGLTF_MOTION_ERROR_ZERO_DURATION:
			return "animation duration is zero";
		case CGLTF_MOTION_ERROR_NON_FINITE_TIME:
			return "accessor contains NaN or infinite values";
		case CGLTF_MOTION_ERROR_KEYS_NOT_STRICTLY_ASCENDING:
			return "sampler input keys are not strictly ascending";
		case CGLTF_MOTION_ERROR_DUPLICATE_TARGET:
			return "two channels target the same node property";
		case CGLTF_MOTION_ERROR_WEIGHTS_UNSUPPORTED:
			return "morph weights animation is not supported";
		case CGLTF_MOTION_ERROR_MATRIX_NODE_ANIMATED:
			return "matrix nodes cannot be animated with TRS channels";
		case CGLTF_MOTION_ERROR_NON_RIGID_CHAIN:
			return "extraction chain has non-uniform scale, shear or reflection";
		case CGLTF_MOTION_ERROR_BAD_ACCESSOR:
			return "animation accessor is missing or malformed";
		case CGLTF_MOTION_ERROR_INTERNAL:
			return "internal evaluation error";
	}
	return "unknown error";
}

cgltf_motion_result cgltf_motion_create(
		const cgltf_data* data,
		cgltf_size animation_index,
		cgltf_size extract_node_index,
		cgltf_motion_player** out_player)
{
	if (data == NULL || out_player == NULL)
		return CGLTF_MOTION_ERROR_INVALID_ARGUMENT;
	if (animation_index >= data->animations_count)
		return CGLTF_MOTION_ERROR_ANIMATION_INDEX;
	if (extract_node_index >= data->nodes_count)
		return CGLTF_MOTION_ERROR_NODE_INDEX;

	const cgltf_animation* animation = &data->animations[animation_index];
	if (animation->samplers_count == 0 || animation->channels_count == 0)
		return CGLTF_MOTION_ERROR_EMPTY_ANIMATION;

	cgltf_motion_player* player =
		(cgltf_motion_player*)calloc(1, sizeof(cgltf_motion_player));
	if (player == NULL)
		return CGLTF_MOTION_ERROR_OUT_OF_MEMORY;

	player->data = data;
	player->nodes_count = data->nodes_count;
	player->extract_index = extract_node_index;
	player->time_begin = 0.f;
	player->time_end = 0.f;

	cgltf_motion_result result = CGLTF_MOTION_OK;

	player->channels = (motion_channel*)calloc(
			animation->channels_count, sizeof(motion_channel));
	if (player->channels == NULL)
	{
		result = CGLTF_MOTION_ERROR_OUT_OF_MEMORY;
		goto fail;
	}

	cgltf_bool have_range = 0;
	for (cgltf_size ch = 0; ch < animation->channels_count; ++ch)
	{
		const cgltf_animation_channel* src = &animation->channels[ch];
		motion_channel* dst = &player->channels[ch];

		if (src->target_path == cgltf_animation_path_type_weights ||
				src->target_path == cgltf_animation_path_type_invalid)
		{
			result = CGLTF_MOTION_ERROR_WEIGHTS_UNSUPPORTED;
			goto fail;
		}
		if (src->target_node == NULL || src->sampler == NULL)
		{
			result = CGLTF_MOTION_ERROR_BAD_ACCESSOR;
			goto fail;
		}
		if (src->target_node->has_matrix)
		{
			result = CGLTF_MOTION_ERROR_MATRIX_NODE_ANIMATED;
			goto fail;
		}

		for (cgltf_size prev = 0; prev < ch; ++prev)
		{
			if (player->channels[prev].node == src->target_node &&
					player->channels[prev].path == src->target_path)
			{
				result = CGLTF_MOTION_ERROR_DUPLICATE_TARGET;
				goto fail;
			}
		}

		dst->node = src->target_node;
		dst->path = src->target_path;
		dst->interpolation = src->sampler->interpolation;
		dst->components =
			(src->target_path == cgltf_animation_path_type_rotation) ? 4 : 3;
		dst->value_stride = dst->components *
			(src->sampler->interpolation == cgltf_interpolation_type_cubic_spline ? 3 : 1);

		cgltf_size key_floats = 0;
		result = unpack_accessor(src->sampler->input, &dst->keys, &key_floats);
		if (result != CGLTF_MOTION_OK)
			goto fail;
		dst->key_count = key_floats; /* scalar accessor: one float per key */
		if (dst->key_count == 0)
		{
			result = CGLTF_MOTION_ERROR_BAD_ACCESSOR;
			goto fail;
		}
		for (cgltf_size k = 0; k < dst->key_count; ++k)
		{
			if (!isfinite(dst->keys[k]))
			{
				result = CGLTF_MOTION_ERROR_NON_FINITE_TIME;
				goto fail;
			}
		}

		cgltf_size value_floats = 0;
		result = unpack_accessor(src->sampler->output, &dst->values, &value_floats);
		if (result != CGLTF_MOTION_OK)
			goto fail;
		if (value_floats != dst->key_count * dst->value_stride)
		{
			result = CGLTF_MOTION_ERROR_BAD_ACCESSOR;
			goto fail;
		}
		for (cgltf_size k = 0; k < value_floats; ++k)
		{
			if (!isfinite(dst->values[k]))
			{
				result = CGLTF_MOTION_ERROR_BAD_ACCESSOR;
				goto fail;
			}
		}

		for (cgltf_size k = 1; k < dst->key_count; ++k)
		{
			if (!(dst->keys[k] > dst->keys[k - 1]))
			{
				result = CGLTF_MOTION_ERROR_KEYS_NOT_STRICTLY_ASCENDING;
				goto fail;
			}
		}

		if (!have_range)
		{
			player->time_begin = dst->keys[0];
			player->time_end = dst->keys[dst->key_count - 1];
			have_range = 1;
		}
		else
		{
			if (dst->keys[0] < player->time_begin)
				player->time_begin = dst->keys[0];
			if (dst->keys[dst->key_count - 1] > player->time_end)
				player->time_end = dst->keys[dst->key_count - 1];
		}
		player->channels_count++;
	}

	if (!have_range || !isfinite(player->time_begin) || !isfinite(player->time_end))
	{
		result = CGLTF_MOTION_ERROR_NON_FINITE_TIME;
		goto fail;
	}
	if (!(player->time_end > player->time_begin))
	{
		result = CGLTF_MOTION_ERROR_ZERO_DURATION;
		goto fail;
	}

	player->nodes = (motion_node*)calloc(player->nodes_count, sizeof(motion_node));
	if (player->nodes == NULL)
	{
		result = CGLTF_MOTION_ERROR_OUT_OF_MEMORY;
		goto fail;
	}

	for (cgltf_size i = 0; i < player->nodes_count; ++i)
	{
		motion_node* mn = &player->nodes[i];
		mn->node = &data->nodes[i];
		mn->parent = CGLTF_MOTION_NO_NODE;
		if (data->nodes[i].parent != NULL)
			mn->parent = (cgltf_size)(data->nodes[i].parent - data->nodes);
		cgltf_node_transform_local(&data->nodes[i], mn->static_local);
	}

	/* Mark animated nodes so the rigidity check can find scale channels. */
	for (cgltf_size i = 0; i < player->channels_count; ++i)
	{
		motion_channel* channel = &player->channels[i];
		cgltf_size node_index = (cgltf_size)(channel->node - data->nodes);
		motion_node* mn = &player->nodes[node_index];
		if (channel->path == cgltf_animation_path_type_translation)
			mn->animated_translation = 1;
		else if (channel->path == cgltf_animation_path_type_rotation)
			mn->animated_rotation = 1;
		else if (channel->path == cgltf_animation_path_type_scale)
			mn->animated_scale = 1;
	}

	/* Rigidity of the extraction node and every ancestor. Side branches
	 * are intentionally unrestricted. */
	for (cgltf_size cursor = extract_node_index;
			cursor != CGLTF_MOTION_NO_NODE;
			cursor = player->nodes[cursor].parent)
	{
		motion_node* mn = &player->nodes[cursor];

		if (mn->node->has_matrix)
		{
			if (!matrix_is_rigid(mn->static_local))
			{
				result = CGLTF_MOTION_ERROR_NON_RIGID_CHAIN;
				goto fail;
			}
			continue;
		}

		/* The extraction chain must be a rigid body: only unit scale is
		 * allowed, so any non-unit static or animated scale (uniform or
		 * not) is rejected. Side branches are never walked here. */
		if (mn->animated_scale)
		{
			/* A scale channel fully determines the node's scale at every
			 * time (out-of-range times clamp to the end values), so the
			 * static scale is never used and is not inspected. For the
			 * interpolated scale to stay exactly unit, LINEAR/STEP need
			 * unit key values; CUBIC additionally needs zero tangents so
			 * the Hermite curve stays constant at unit. */
			for (cgltf_size i = 0; i < player->channels_count; ++i)
			{
				motion_channel* channel = &player->channels[i];
				if (channel->node != mn->node ||
						channel->path != cgltf_animation_path_type_scale)
					continue;
				for (cgltf_size k = 0; k < channel->key_count; ++k)
				{
					const cgltf_float* p =
						channel->values + k * channel->value_stride;
					if (channel->interpolation ==
							cgltf_interpolation_type_cubic_spline)
					{
						const cgltf_float* in_t = p;          /* in-tangent */
						const cgltf_float* val = p + 3;       /* value */
						const cgltf_float* out_t = p + 6;      /* out-tangent */
						if (!scale_is_identity(val) ||
								fabsf(in_t[0]) > 1e-5f || fabsf(in_t[1]) > 1e-5f ||
								fabsf(in_t[2]) > 1e-5f ||
								fabsf(out_t[0]) > 1e-5f || fabsf(out_t[1]) > 1e-5f ||
								fabsf(out_t[2]) > 1e-5f)
						{
							result = CGLTF_MOTION_ERROR_NON_RIGID_CHAIN;
							goto fail;
						}
					}
					else if (!scale_is_identity(p))
					{
						result = CGLTF_MOTION_ERROR_NON_RIGID_CHAIN;
						goto fail;
					}
				}
			}
		}
		else
		{
			cgltf_float static_scale[3] = {1.f, 1.f, 1.f};
			if (mn->node->has_scale)
				memcpy(static_scale, mn->node->scale, sizeof(static_scale));
			if (!scale_is_identity(static_scale))
			{
				result = CGLTF_MOTION_ERROR_NON_RIGID_CHAIN;
				goto fail;
			}
		}
	}

	player->pose_storage = (cgltf_float*)malloc(
			player->nodes_count * 16 * sizeof(cgltf_float));
	if (player->pose_storage == NULL)
	{
		result = CGLTF_MOTION_ERROR_OUT_OF_MEMORY;
		goto fail;
	}

	/* G0 = G(a); C = inverse(G0) * G(b) with both endpoint poses taken
	 * from the ordinary, clamped sampler evaluation. */
	evaluate_sampled(player, player->time_begin);
	memcpy(player->g0, player->nodes[extract_node_index].world,
			16 * sizeof(cgltf_float));
	if (!m_inverse(player->inv_g0, player->g0))
	{
		result = CGLTF_MOTION_ERROR_INTERNAL;
		goto fail;
	}

	cgltf_float gb[16];
	evaluate_sampled(player, player->time_end);
	memcpy(gb, player->nodes[extract_node_index].world,
			16 * sizeof(cgltf_float));

	m_multiply(player->cycle, player->inv_g0, gb);

	*out_player = player;
	return CGLTF_MOTION_OK;

fail:
	free_player_content(player);
	return result;
}

cgltf_size cgltf_motion_nodes_count(const cgltf_motion_player* player)
{
	return player ? player->nodes_count : 0;
}

cgltf_float cgltf_motion_time_begin(const cgltf_motion_player* player)
{
	return player ? player->time_begin : 0.f;
}

cgltf_float cgltf_motion_time_end(const cgltf_motion_player* player)
{
	return player ? player->time_end : 0.f;
}

cgltf_motion_result cgltf_motion_evaluate(
		cgltf_motion_player* player,
		cgltf_float time_seconds,
		cgltf_motion_pose* out_pose,
		cgltf_motion_matrix* out_motion)
{
	if (player == NULL)
		return CGLTF_MOTION_ERROR_INVALID_ARGUMENT;
	if (!isfinite(time_seconds))
		return CGLTF_MOTION_ERROR_NON_FINITE_TIME;

	/* Absolute-time wrap: k = floor((t-a)/(b-a)), u in [a, b). Exact hit
	 * on b starts the next cycle; double precision keeps huge and negative
	 * times stable. */
	double begin = (double)player->time_begin;
	double span = (double)player->time_end - begin;
	double wrapped = ((double)time_seconds - begin) / span;
	double kd = floor(wrapped);
	double phase = begin + ((double)time_seconds - begin) - kd * span;
	cgltf_float u = (cgltf_float)phase;

	evaluate_sampled(player, u);

	const cgltf_float* g = player->nodes[player->extract_index].world;

	cgltf_float inv_g[16];
	if (!m_inverse(inv_g, g))
		return CGLTF_MOTION_ERROR_INTERNAL;

	if (out_pose != NULL)
	{
		/* P_i = G0 * inverse(G(u)) * W_i(u) */
		cgltf_float fix[16];
		m_multiply(fix, player->g0, inv_g);

		for (cgltf_size i = 0; i < player->nodes_count; ++i)
		{
			cgltf_float* dst = player->pose_storage + i * 16;
			m_multiply(dst, fix, player->nodes[i].world);
			if (!m_is_finite(dst))
				return CGLTF_MOTION_ERROR_INTERNAL;
		}

		out_pose->world_matrices = player->pose_storage;
		out_pose->nodes_count = player->nodes_count;
	}

	if (out_motion != NULL)
	{
		if (kd > 9.2e18 || kd < -9.2e18)
			return CGLTF_MOTION_ERROR_NON_FINITE_TIME;
		long long k = (long long)kd;

		cgltf_float a_rel[16];   /* A = inverse(G0) * G(u) */
		m_multiply(a_rel, player->inv_g0, g);

		cgltf_float cycle_k[16]; /* C^k, valid for negative k */
		m_power(cycle_k, player->cycle, k);

		cgltf_float tmp[16];
		/* M = G0 * C^k * A * inverse(G0) */
		m_multiply(tmp, cycle_k, a_rel);
		m_multiply(tmp, player->g0, tmp);
		m_multiply(out_motion->m, tmp, player->inv_g0);

		if (!m_is_finite(out_motion->m))
			return CGLTF_MOTION_ERROR_INTERNAL;
	}

	return CGLTF_MOTION_OK;
}

cgltf_motion_result cgltf_motion_delta(
		cgltf_motion_player* player,
		cgltf_float from_seconds,
		cgltf_float to_seconds,
		cgltf_motion_matrix* out_delta)
{
	if (player == NULL || out_delta == NULL)
		return CGLTF_MOTION_ERROR_INVALID_ARGUMENT;

	cgltf_motion_matrix m_from;
	cgltf_motion_matrix m_to;
	cgltf_motion_result result =
		cgltf_motion_evaluate(player, from_seconds, NULL, &m_from);
	if (result != CGLTF_MOTION_OK)
		return result;
	result = cgltf_motion_evaluate(player, to_seconds, NULL, &m_to);
	if (result != CGLTF_MOTION_OK)
		return result;

	cgltf_float inv_from[16];
	if (!m_inverse(inv_from, m_from.m))
		return CGLTF_MOTION_ERROR_INTERNAL;
	m_multiply(out_delta->m, inv_from, m_to.m);

	if (!m_is_finite(out_delta->m))
		return CGLTF_MOTION_ERROR_INTERNAL;
	return CGLTF_MOTION_OK;
}

void cgltf_motion_release(cgltf_motion_player* player)
{
	free_player_content(player);
}
