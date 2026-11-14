/*
 * cgltf_retime - time-warp one animation of a cgltf asset.
 *
 * See cgltf_retime.h for the public interface.  The implementation lives in
 * this file and is meant to be compiled as its own translation unit, next to
 * the single translation unit that provides CGLTF_IMPLEMENTATION and
 * CGLTF_WRITE_IMPLEMENTATION (see task_support/cgltf_impl.c).
 *
 * The operation is split in two stages:
 *
 *   1. The selected animation is evaluated at a new set of key times derived
 *      from the piecewise-linear time mapping.  Values, differentials and
 *      glTF cubic-spline tangents are re-expressed with respect to the new
 *      (output) clock.  This yields a plain, self-contained description of
 *      the new animation.
 *
 *   2. The source asset is duplicated with a JSON round-trip; every buffer is
 *      re-encoded as an embedded data URI and decoded again, so the copy
 *      owns all of its bytes.  The selected animation of the copy is then
 *      replaced with newly allocated accessors.
 *
 * No floating point "baking" is performed: STEP, LINEAR and CUBICSPLINE
 * interpolation are kept exactly as in the source.
 */

#include "cgltf_retime.h"

#include "../cgltf_write.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

typedef struct retime_src_channel {
	const cgltf_animation_channel* channel;
	cgltf_interpolation_type interpolation;
	cgltf_animation_path_type path;
	cgltf_size comps;           /* scalar values per key snapshot */
	cgltf_size vec;             /* scalars per accessor element (1/3/4) */
	cgltf_size blocks;          /* 3 for CUBICSPLINE, 1 otherwise */
	cgltf_size keys;
	cgltf_float* times;         /* keys */
	cgltf_float* in_tangent;    /* keys * comps, cubic only */
	cgltf_float* values;        /* keys * comps */
	cgltf_float* out_tangent;   /* keys * comps, cubic only */
} retime_src_channel;

typedef struct retime_out_channel {
	cgltf_size keys;
	cgltf_size comps;
	cgltf_size vec;
	cgltf_size blocks;
	cgltf_type type;
	cgltf_interpolation_type interpolation;
	cgltf_float* times;         /* keys */
	cgltf_float* values;        /* keys * comps * blocks */
	cgltf_size accessor_count;  /* element count of the output accessor */
} retime_out_channel;

/* ------------------------------------------------------------------ */
/* small helpers                                                       */
/* ------------------------------------------------------------------ */

static int retime_mul_overflow(cgltf_size a, cgltf_size b, cgltf_size* out)
{
	if (a != 0 && b > (cgltf_size)-1 / a)
	{
		return 1;
	}
	*out = a * b;
	return 0;
}

/* prefix length of "data:application/octet-stream;base64," */
#define RETIME_DATA_URI_PREFIX "data:application/octet-stream;base64,"
#define RETIME_DATA_URI_PREFIX_LEN (sizeof(RETIME_DATA_URI_PREFIX) - 1)

static char* retime_data_uri(const void* data, cgltf_size size)
{
	static const char alphabet[] =
		"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	const unsigned char* bytes = (const unsigned char*)data;
	cgltf_size groups = (size + 2) / 3;
	cgltf_size text_size;
	char* text;

	if (groups > ((cgltf_size)-1 - RETIME_DATA_URI_PREFIX_LEN - 1) / 4)
	{
		return NULL;
	}
	text_size = RETIME_DATA_URI_PREFIX_LEN + groups * 4;
	text = (char*)malloc((size_t)text_size + 1);
	if (!text)
	{
		return NULL;
	}

	memcpy(text, RETIME_DATA_URI_PREFIX, RETIME_DATA_URI_PREFIX_LEN);
	{
		char* out = text + RETIME_DATA_URI_PREFIX_LEN;
		cgltf_size i;
		for (i = 0; i < size; i += 3)
		{
			cgltf_size remain = size - i;
			unsigned int a = bytes[i];
			unsigned int b = remain > 1 ? bytes[i + 1] : 0;
			unsigned int c = remain > 2 ? bytes[i + 2] : 0;
			unsigned int triple = (a << 16) | (b << 8) | c;

			*out++ = alphabet[(triple >> 18) & 63];
			*out++ = alphabet[(triple >> 12) & 63];
			*out++ = remain > 1 ? alphabet[(triple >> 6) & 63] : '=';
			*out++ = remain > 2 ? alphabet[triple & 63] : '=';
		}
		*out = '\0';
	}
	return text;
}

/* ------------------------------------------------------------------ */
/* accessor reading (offset/stride/sparse aware)                       */
/* ------------------------------------------------------------------ */

static cgltf_result retime_read_accessor(const cgltf_accessor* accessor,
		cgltf_float** out_values, cgltf_size* out_count, cgltf_size* out_vec)
{
	cgltf_size vec;
	cgltf_size count;
	cgltf_size total;
	cgltf_float* values;
	cgltf_size i;

	if (!accessor || !out_values)
	{
		return cgltf_result_invalid_gltf;
	}

	vec = cgltf_num_components(accessor->type);
	if (vec == 0 || vec > 4)
	{
		return cgltf_result_invalid_gltf;
	}

	count = accessor->count;
	if (count == 0 || accessor->component_type == cgltf_component_type_invalid)
	{
		return cgltf_result_invalid_gltf;
	}

	if (retime_mul_overflow(count, vec, &total))
	{
		return cgltf_result_data_too_short;
	}

	if (!accessor->buffer_view && !accessor->is_sparse)
	{
		/* Animation data must live in a buffer. */
		return cgltf_result_invalid_gltf;
	}

	if (accessor->buffer_view)
	{
		cgltf_size element_size = cgltf_calc_size(accessor->type, accessor->component_type);
		cgltf_size stride = accessor->stride ? accessor->stride : element_size;
		cgltf_size required;
		const cgltf_buffer_view* view = accessor->buffer_view;

		if (!view->data && (!view->buffer || !view->buffer->data))
		{
			return cgltf_result_data_too_short;
		}
		if (stride < element_size)
		{
			return cgltf_result_invalid_gltf;
		}
		if (retime_mul_overflow(stride, count - 1, &required) ||
				required > (cgltf_size)-1 - accessor->offset ||
				accessor->offset + required > (cgltf_size)-1 - element_size)
		{
			return cgltf_result_data_too_short;
		}
		required = accessor->offset + required + element_size;
		if (view->size < required)
		{
			return cgltf_result_data_too_short;
		}
	}

	if (accessor->is_sparse)
	{
		const cgltf_accessor_sparse* sparse = &accessor->sparse;
		cgltf_size element_size = cgltf_calc_size(accessor->type, accessor->component_type);
		cgltf_size stride = accessor->stride ? accessor->stride : element_size;
		cgltf_size indices_size = cgltf_component_size(sparse->indices_component_type);
		cgltf_size required;

		if (!sparse->indices_buffer_view || !sparse->values_buffer_view)
		{
			return cgltf_result_invalid_gltf;
		}
		if ((!sparse->indices_buffer_view->data &&
				(!sparse->indices_buffer_view->buffer ||
						!sparse->indices_buffer_view->buffer->data)) ||
				(!sparse->values_buffer_view->data &&
				(!sparse->values_buffer_view->buffer ||
						!sparse->values_buffer_view->buffer->data)))
		{
			return cgltf_result_data_too_short;
		}
		if (retime_mul_overflow(indices_size, sparse->count, &required) ||
				required > (cgltf_size)-1 - sparse->indices_byte_offset ||
				sparse->indices_buffer_view->size < sparse->indices_byte_offset + required)
		{
			return cgltf_result_data_too_short;
		}
		/* Sparse values are stored with the accessor's stride. */
		if (sparse->count > 0)
		{
			if (retime_mul_overflow(stride, sparse->count - 1, &required) ||
					required > (cgltf_size)-1 - element_size ||
					sparse->values_byte_offset > (cgltf_size)-1 - required - element_size)
			{
				return cgltf_result_data_too_short;
			}
			required = sparse->values_byte_offset + required + element_size;
			if (sparse->values_buffer_view->size < required)
			{
				return cgltf_result_data_too_short;
			}
		}
	}

	if (total > (cgltf_size)-1 / sizeof(cgltf_float))
	{
		return cgltf_result_out_of_memory;
	}

	values = (cgltf_float*)malloc((size_t)total * sizeof(cgltf_float));
	if (!values)
	{
		return cgltf_result_out_of_memory;
	}

	for (i = 0; i < count; ++i)
	{
		cgltf_float element[4];
		if (!cgltf_accessor_read_float(accessor, i, element, vec))
		{
			free(values);
			return cgltf_result_data_too_short;
		}
		memcpy(values + i * vec, element, (size_t)vec * sizeof(cgltf_float));
	}

	*out_values = values;
	if (out_count)
	{
		*out_count = count;
	}
	if (out_vec)
	{
		*out_vec = vec;
	}
	return cgltf_result_success;
}

/* Read only the first and last value of a SCALAR float input accessor. */
static cgltf_result retime_read_input_bounds(const cgltf_accessor* accessor,
		cgltf_float* first, cgltf_float* last)
{
	cgltf_float values[2];

	if (!accessor || accessor->type != cgltf_type_scalar ||
			accessor->component_type != cgltf_component_type_r_32f)
	{
		return cgltf_result_invalid_gltf;
	}
	if (accessor->count == 0)
	{
		return cgltf_result_invalid_gltf;
	}
	if (!accessor->buffer_view && !accessor->is_sparse)
	{
		return cgltf_result_invalid_gltf;
	}
	if (accessor->buffer_view &&
			(!accessor->buffer_view->data &&
			(!accessor->buffer_view->buffer ||
					!accessor->buffer_view->buffer->data)))
	{
		return cgltf_result_data_too_short;
	}
	if (!cgltf_accessor_read_float(accessor, 0, values, 1) ||
			!cgltf_accessor_read_float(accessor, accessor->count - 1, values + 1, 1))
	{
		return cgltf_result_data_too_short;
	}
	if (first)
	{
		*first = values[0];
	}
	if (last)
	{
		*last = values[1];
	}
	return cgltf_result_success;
}

/* ------------------------------------------------------------------ */
/* source channel extraction                                           */
/* ------------------------------------------------------------------ */

static void retime_free_src_channel(retime_src_channel* channel)
{
	free(channel->times);
	free(channel->in_tangent);
	free(channel->values);
	free(channel->out_tangent);
	memset(channel, 0, sizeof(*channel));
}

static cgltf_result retime_read_src_channel(const cgltf_data* data,
		const cgltf_animation_channel* channel, retime_src_channel* out)
{
	cgltf_result result;
	const cgltf_animation_sampler* sampler;
	const cgltf_accessor* input;
	const cgltf_accessor* output;
	cgltf_float* times = NULL;
	cgltf_float* raw = NULL;
	cgltf_size time_count = 0;
	cgltf_size raw_count = 0;
	cgltf_size raw_vec = 0;
	cgltf_size comps = 0;
	cgltf_size vec = 0;
	cgltf_size blocks;
	cgltf_size i;

	(void)data;
	memset(out, 0, sizeof(*out));

	if (!channel || !channel->sampler)
	{
		return cgltf_result_invalid_gltf;
	}
	sampler = channel->sampler;
	input = sampler->input;
	output = sampler->output;
	if (!input || !output)
	{
		return cgltf_result_invalid_gltf;
	}
	if (sampler->interpolation != cgltf_interpolation_type_linear &&
			sampler->interpolation != cgltf_interpolation_type_step &&
			sampler->interpolation != cgltf_interpolation_type_cubic_spline)
	{
		return cgltf_result_invalid_gltf;
	}

	switch (channel->target_path)
	{
	case cgltf_animation_path_type_translation:
	case cgltf_animation_path_type_scale:
		if (output->type != cgltf_type_vec3)
		{
			return cgltf_result_invalid_gltf;
		}
		comps = 3;
		vec = 3;
		break;
	case cgltf_animation_path_type_rotation:
		if (output->type != cgltf_type_vec4)
		{
			return cgltf_result_invalid_gltf;
		}
		comps = 4;
		vec = 4;
		break;
	case cgltf_animation_path_type_weights:
	{
		cgltf_size targets;
		if (output->type != cgltf_type_scalar)
		{
			return cgltf_result_invalid_gltf;
		}
		if (!channel->target_node || !channel->target_node->mesh ||
				!channel->target_node->mesh->primitives_count)
		{
			return cgltf_result_invalid_gltf;
		}
		targets = channel->target_node->mesh->primitives[0].targets_count;
		if (targets == 0)
		{
			return cgltf_result_invalid_gltf;
		}
		/* Weights are flattened SCALAR data, one value per morph target. */
		comps = targets;
		vec = 1;
		break;
	}
	default:
		return cgltf_result_invalid_gltf;
	}

	/* glTF requires FLOAT for sampler input; output may be normalized. */
	if (input->type != cgltf_type_scalar ||
			input->component_type != cgltf_component_type_r_32f)
	{
		return cgltf_result_invalid_gltf;
	}

	result = retime_read_accessor(input, &times, &time_count, NULL);
	if (result != cgltf_result_success)
	{
		return result;
	}
	for (i = 0; i < time_count; ++i)
	{
		if (!isfinite(times[i]))
		{
			free(times);
			return cgltf_result_invalid_gltf;
		}
		if (i > 0 && !(times[i] > times[i - 1]))
		{
			free(times);
			return cgltf_result_invalid_gltf;
		}
	}

	result = retime_read_accessor(output, &raw, &raw_count, &raw_vec);
	if (result != cgltf_result_success)
	{
		free(times);
		return result;
	}

	blocks = sampler->interpolation == cgltf_interpolation_type_cubic_spline ? 3 : 1;

	{
		cgltf_size per_key = 0;
		cgltf_size expected = 0;
		cgltf_size raw_total = 0;
		if (raw_vec != vec ||
				retime_mul_overflow(comps, blocks, &per_key) ||
				retime_mul_overflow(time_count, per_key, &expected) ||
				retime_mul_overflow(raw_count, raw_vec, &raw_total) ||
				expected != raw_total)
		{
			free(times);
			free(raw);
			return cgltf_result_invalid_gltf;
		}
	}

	out->channel = channel;
	out->interpolation = sampler->interpolation;
	out->path = channel->target_path;
	out->comps = comps;
	out->vec = vec;
	out->blocks = blocks;
	out->keys = time_count;
	out->times = times;

	out->values = (cgltf_float*)malloc((size_t)(time_count * comps) * sizeof(cgltf_float));
	if (!out->values)
	{
		free(raw);
		retime_free_src_channel(out);
		return cgltf_result_out_of_memory;
	}
	if (blocks == 3)
	{
		out->in_tangent = (cgltf_float*)malloc((size_t)(time_count * comps) * sizeof(cgltf_float));
		out->out_tangent = (cgltf_float*)malloc((size_t)(time_count * comps) * sizeof(cgltf_float));
		if (!out->in_tangent || !out->out_tangent)
		{
			free(raw);
			retime_free_src_channel(out);
			return cgltf_result_out_of_memory;
		}
	}

	for (i = 0; i < time_count; ++i)
	{
		const cgltf_float* block = raw + i * comps * blocks;
		if (blocks == 3)
		{
			memcpy(out->in_tangent + i * comps, block, (size_t)comps * sizeof(cgltf_float));
			memcpy(out->values + i * comps, block + comps, (size_t)comps * sizeof(cgltf_float));
			memcpy(out->out_tangent + i * comps, block + 2 * comps, (size_t)comps * sizeof(cgltf_float));
		}
		else
		{
			memcpy(out->values + i * comps, block, (size_t)comps * sizeof(cgltf_float));
		}
	}

	free(raw);
	return cgltf_result_success;
}

/* ------------------------------------------------------------------ */
/* value evaluation                                                    */
/* ------------------------------------------------------------------ */

static int retime_float_equal(cgltf_float a, cgltf_float b)
{
	return a == b;
}

/* Index of the last key <= t, or -1 when t precedes the first key. */
static long retime_key_index(const retime_src_channel* channel, cgltf_float t)
{
	cgltf_size lo = 0;
	cgltf_size hi = channel->keys;
	while (lo < hi)
	{
		cgltf_size mid = lo + (hi - lo) / 2;
		if (channel->times[mid] <= t)
		{
			lo = mid + 1;
		}
		else
		{
			hi = mid;
		}
	}
	return (long)lo - 1;
}

/* Index of the segment [times[i], times[i+1]] containing t (clamped). */
static cgltf_size retime_segment_index(const retime_src_channel* channel, cgltf_float t)
{
	cgltf_size lo = 0;
	cgltf_size hi = channel->keys - 1;
	while (lo + 1 < hi)
	{
		cgltf_size mid = lo + (hi - lo) / 2;
		if (channel->times[mid] <= t)
		{
			lo = mid;
		}
		else
		{
			hi = mid;
		}
	}
	return lo;
}

static void retime_lerp(const cgltf_float* a, const cgltf_float* b, double s,
		cgltf_float* out, cgltf_size comps)
{
	cgltf_size c;
	for (c = 0; c < comps; ++c)
	{
		out[c] = (cgltf_float)((double)a[c] + ((double)b[c] - (double)a[c]) * s);
	}
}

static void retime_normalize_quat(const cgltf_float* in, cgltf_float* out)
{
	double x = in[0], y = in[1], z = in[2], w = in[3];
	double len = sqrt(x * x + y * y + z * z + w * w);
	if (len > 0.0)
	{
		out[0] = (cgltf_float)(x / len);
		out[1] = (cgltf_float)(y / len);
		out[2] = (cgltf_float)(z / len);
		out[3] = (cgltf_float)(w / len);
	}
	else
	{
		out[0] = 0.0f;
		out[1] = 0.0f;
		out[2] = 0.0f;
		out[3] = 1.0f;
	}
}

/* Spherical linear interpolation along the shortest arc. */
static void retime_slerp(const cgltf_float* a, const cgltf_float* b, double s,
		cgltf_float* out)
{
	cgltf_float qa[4], qb[4];
	double dot = 0.0;
	double abs_dot;
	double theta;
	cgltf_size c;

	retime_normalize_quat(a, qa);
	retime_normalize_quat(b, qb);
	for (c = 0; c < 4; ++c)
	{
		dot += (double)qa[c] * (double)qb[c];
	}
	abs_dot = dot < 0.0 ? -dot : dot;

	if (abs_dot > 0.9995)
	{
		cgltf_float blended[4];
		retime_lerp(qa, qb, s, blended, 4);
		retime_normalize_quat(blended, out);
		return;
	}

	theta = acos(abs_dot);
	{
		double sin_theta = sin(theta);
		double wa = sin((1.0 - s) * theta) / sin_theta;
		double wb = sin(s * theta) / sin_theta;
		if (dot < 0.0)
		{
			wb = -wb;
		}
		for (c = 0; c < 4; ++c)
		{
			out[c] = (cgltf_float)(wa * (double)qa[c] + wb * (double)qb[c]);
		}
	}
	retime_normalize_quat(out, out);
}

/* glTF cubic Hermite value at parameter s within a segment. */
static void retime_cubic_value(const retime_src_channel* channel,
		cgltf_size segment, double s, cgltf_float* out)
{
	double s2 = s * s;
	double s3 = s2 * s;
	double h = (double)channel->times[segment + 1] - (double)channel->times[segment];
	double p0 = 2.0 * s3 - 3.0 * s2 + 1.0;
	double m0 = s3 - 2.0 * s2 + s;
	double p1 = -2.0 * s3 + 3.0 * s2;
	double m1 = s3 - s2;
	const cgltf_float* v0 = channel->values + segment * channel->comps;
	const cgltf_float* v1 = channel->values + (segment + 1) * channel->comps;
	const cgltf_float* t0 = channel->out_tangent + segment * channel->comps;
	const cgltf_float* t1 = channel->in_tangent + (segment + 1) * channel->comps;
	cgltf_size c;

	for (c = 0; c < channel->comps; ++c)
	{
		out[c] = (cgltf_float)(p0 * (double)v0[c] +
				m0 * h * (double)t0[c] +
				p1 * (double)v1[c] +
				m1 * h * (double)t1[c]);
	}
}

/* Differential of a cubic segment with respect to source time. */
static void retime_cubic_derivative(const retime_src_channel* channel,
		cgltf_size segment, double s, cgltf_float* out)
{
	double s2 = s * s;
	double h = (double)channel->times[segment + 1] - (double)channel->times[segment];
	double dp0 = 6.0 * s2 - 6.0 * s;
	double dm0 = 3.0 * s2 - 4.0 * s + 1.0;
	double dp1 = -6.0 * s2 + 6.0 * s;
	double dm1 = 3.0 * s2 - 2.0 * s;
	const cgltf_float* v0 = channel->values + segment * channel->comps;
	const cgltf_float* v1 = channel->values + (segment + 1) * channel->comps;
	const cgltf_float* t0 = channel->out_tangent + segment * channel->comps;
	const cgltf_float* t1 = channel->in_tangent + (segment + 1) * channel->comps;
	cgltf_size c;

	for (c = 0; c < channel->comps; ++c)
	{
		double derivative = dp0 * (double)v0[c] +
				dm0 * h * (double)t0[c] +
				dp1 * (double)v1[c] +
				dm1 * h * (double)t1[c];
		out[c] = (cgltf_float)(derivative / h);
	}
}

/* Value of a STEP/LINEAR/CUBIC channel at source time t. */
static void retime_value_at(const retime_src_channel* channel, cgltf_float t,
		cgltf_float* out)
{
	long index = retime_key_index(channel, t);

	if (index < 0)
	{
		memcpy(out, channel->values, (size_t)channel->comps * sizeof(cgltf_float));
		return;
	}
	if (index >= (long)channel->keys - 1)
	{
		memcpy(out, channel->values + ((size_t)channel->keys - 1) * channel->comps,
				(size_t)channel->comps * sizeof(cgltf_float));
		return;
	}
	if (retime_float_equal(channel->times[index], t))
	{
		memcpy(out, channel->values + (size_t)index * channel->comps,
				(size_t)channel->comps * sizeof(cgltf_float));
		return;
	}

	switch (channel->interpolation)
	{
	case cgltf_interpolation_type_step:
		memcpy(out, channel->values + (size_t)index * channel->comps,
				(size_t)channel->comps * sizeof(cgltf_float));
		break;
	case cgltf_interpolation_type_linear:
	{
		double t0 = (double)channel->times[index];
		double t1 = (double)channel->times[index + 1];
		double s = (t1 > t0) ? ((double)t - t0) / (t1 - t0) : 0.0;
		const cgltf_float* v0 = channel->values + (size_t)index * channel->comps;
		const cgltf_float* v1 = channel->values + ((size_t)index + 1) * channel->comps;
		if (channel->path == cgltf_animation_path_type_rotation)
		{
			retime_slerp(v0, v1, s, out);
		}
		else
		{
			retime_lerp(v0, v1, s, out, channel->comps);
		}
		break;
	}
	case cgltf_interpolation_type_cubic_spline:
	{
		double t0 = (double)channel->times[index];
		double t1 = (double)channel->times[index + 1];
		double s = (t1 > t0) ? ((double)t - t0) / (t1 - t0) : 0.0;
		retime_cubic_value(channel, (cgltf_size)index, s, out);
		break;
	}
	default:
		memcpy(out, channel->values, (size_t)channel->comps * sizeof(cgltf_float));
		break;
	}
}

/*
 * Differential with respect to source time at t, split into the left and right
 * limits (they differ only at a key that is also a time-map break).
 */
static void retime_derivatives_at(const retime_src_channel* channel, cgltf_float t,
		cgltf_float* left, cgltf_float* right)
{
	cgltf_size keys = channel->keys;
	long index;
	cgltf_size c;

	if (keys == 1 || t < channel->times[0] || t > channel->times[keys - 1])
	{
		for (c = 0; c < channel->comps; ++c)
		{
			left[c] = 0.0f;
			right[c] = 0.0f;
		}
		return;
	}

	index = retime_key_index(channel, t);
	if (index >= 0 && index < (long)keys && retime_float_equal(channel->times[index], t))
	{
		cgltf_size j = (cgltf_size)index;
		memcpy(left, channel->in_tangent + j * channel->comps,
				(size_t)channel->comps * sizeof(cgltf_float));
		memcpy(right, channel->out_tangent + j * channel->comps,
				(size_t)channel->comps * sizeof(cgltf_float));
		return;
	}

	{
		cgltf_size segment = retime_segment_index(channel, t);
		double t0 = (double)channel->times[segment];
		double t1 = (double)channel->times[segment + 1];
		double s = (t1 > t0) ? ((double)t - t0) / (t1 - t0) : 0.0;
		retime_cubic_derivative(channel, segment, s, left);
		memcpy(right, left, (size_t)channel->comps * sizeof(cgltf_float));
	}
}

/* ------------------------------------------------------------------ */
/* time mapping                                                        */
/* ------------------------------------------------------------------ */

typedef struct retime_map {
	const cgltf_float* source_times;
	const cgltf_float* output_times;
	cgltf_size count;
	cgltf_float source_begin;
	cgltf_float source_end;
} retime_map;

/* Segment i covers [source_times[i], source_times[i+1]]. */
static cgltf_size retime_map_segment(const retime_map* map, cgltf_float t)
{
	cgltf_size lo = 0;
	cgltf_size hi = map->count - 1;
	while (lo + 1 < hi)
	{
		cgltf_size mid = lo + (hi - lo) / 2;
		if (map->source_times[mid] <= t)
		{
			lo = mid;
		}
		else
		{
			hi = mid;
		}
	}
	return lo;
}

static double retime_map_time(const retime_map* map, cgltf_float t, cgltf_size segment)
{
	double s0 = (double)map->source_times[segment];
	double s1 = (double)map->source_times[segment + 1];
	double o0 = (double)map->output_times[segment];
	double o1 = (double)map->output_times[segment + 1];
	if (s1 <= s0)
	{
		return o0;
	}
	return o0 + ((double)t - s0) * (o1 - o0) / (s1 - s0);
}

/* dt_source / dt_output for a mapping segment. */
static double retime_map_inverse_slope(const retime_map* map, cgltf_size segment)
{
	double ds = (double)map->source_times[segment + 1] - (double)map->source_times[segment];
	double dout = (double)map->output_times[segment + 1] - (double)map->output_times[segment];
	return (ds > 0.0 && dout > 0.0) ? ds / dout : 0.0;
}

/* ------------------------------------------------------------------ */
/* output channel construction                                         */
/* ------------------------------------------------------------------ */

static void retime_free_out_channel(retime_out_channel* channel)
{
	free(channel->times);
	free(channel->values);
	memset(channel, 0, sizeof(*channel));
}

static int retime_cmp_float(const void* a, const void* b)
{
	cgltf_float fa = *(const cgltf_float*)a;
	cgltf_float fb = *(const cgltf_float*)b;
	return (fa < fb) ? -1 : (fa > fb) ? 1 : 0;
}

/*
 * Cubic rotation restriction: every time-map knot strictly inside the
 * sampler's open key interval must coincide with an existing key.  Inside a
 * cubic rotation segment a normalized unit-quaternion key cannot reproduce
 * the original Hermite trajectory exactly, so such edits are refused.
 */
static cgltf_result retime_check_cubic_rotation(const retime_src_channel* channel,
		const retime_map* map)
{
	cgltf_size i;

	if (channel->path != cgltf_animation_path_type_rotation ||
			channel->interpolation != cgltf_interpolation_type_cubic_spline ||
			channel->keys < 2)
	{
		return cgltf_result_success;
	}

	for (i = 0; i < map->count; ++i)
	{
		cgltf_float t = map->source_times[i];
		cgltf_size lo, hi;
		if (!(t > channel->times[0] && t < channel->times[channel->keys - 1]))
		{
			continue;
		}
		lo = 0;
		hi = channel->keys;
		while (lo < hi)
		{
			cgltf_size mid = lo + (hi - lo) / 2;
			if (channel->times[mid] < t)
			{
				lo = mid + 1;
			}
			else
			{
				hi = mid;
			}
		}
		if (lo >= channel->keys || !retime_float_equal(channel->times[lo], t))
		{
			return cgltf_result_invalid_options;
		}
	}
	return cgltf_result_success;
}

static cgltf_result retime_build_out_channel(const retime_src_channel* src,
		const retime_map* map, retime_out_channel* out)
{
	cgltf_float* candidates = NULL;
	cgltf_size candidate_count = 0;
	cgltf_size candidate_capacity = 0;
	cgltf_size i;
	cgltf_size key_count;
	cgltf_size blocks = src->blocks;
	cgltf_size per_key;
	cgltf_size total_values;
	cgltf_result result;

	memset(out, 0, sizeof(*out));

	result = retime_check_cubic_rotation(src, map);
	if (result != cgltf_result_success)
	{
		return result;
	}

	candidate_capacity = src->keys + map->count;
	candidates = (cgltf_float*)malloc((size_t)candidate_capacity * sizeof(cgltf_float));
	if (!candidates)
	{
		return cgltf_result_out_of_memory;
	}

	/* Keep every source key inside the span, then add the map knots. */
	for (i = 0; i < src->keys; ++i)
	{
		if (src->times[i] >= map->source_begin && src->times[i] <= map->source_end)
		{
			candidates[candidate_count++] = src->times[i];
		}
	}
	for (i = 0; i < map->count; ++i)
	{
		candidates[candidate_count++] = map->source_times[i];
	}
	qsort(candidates, (size_t)candidate_count, sizeof(cgltf_float), retime_cmp_float);
	{
		cgltf_size unique = 0;
		for (i = 0; i < candidate_count; ++i)
		{
			if (unique == 0 || !retime_float_equal(candidates[i], candidates[unique - 1]))
			{
				candidates[unique++] = candidates[i];
			}
		}
		candidate_count = unique;
	}

	key_count = candidate_count;
	if (retime_mul_overflow(src->comps, blocks, &per_key) ||
			retime_mul_overflow(key_count, per_key, &total_values))
	{
		free(candidates);
		return cgltf_result_out_of_memory;
	}

	out->times = (cgltf_float*)malloc((size_t)key_count * sizeof(cgltf_float));
	out->values = (cgltf_float*)malloc((size_t)total_values * sizeof(cgltf_float));
	if (!out->times || !out->values)
	{
		free(candidates);
		free(out->times);
		free(out->values);
		memset(out, 0, sizeof(*out));
		return cgltf_result_out_of_memory;
	}

	out->keys = key_count;
	out->comps = src->comps;
	out->vec = src->vec;
	out->blocks = blocks;
	out->interpolation = src->interpolation;
	out->type = src->path == cgltf_animation_path_type_weights
			? cgltf_type_scalar
			: (src->comps == 4 ? cgltf_type_vec4 : cgltf_type_vec3);
	out->accessor_count = total_values / src->vec;

	for (i = 0; i < key_count; ++i)
	{
		cgltf_float t = candidates[i];
		cgltf_size segment = retime_map_segment(map, t);
		cgltf_float* block = out->values + i * per_key;
		cgltf_float value[16];
		double inverse_slope_left;
		double inverse_slope_right;

		out->times[i] = (cgltf_float)retime_map_time(map, t, segment);
		if (i > 0 && !(out->times[i] > out->times[i - 1]))
		{
			free(candidates);
			retime_free_out_channel(out);
			return cgltf_result_invalid_options;
		}

		inverse_slope_left = retime_map_inverse_slope(map, segment);
		inverse_slope_right = inverse_slope_left;
		if (segment > 0 && retime_float_equal(map->source_times[segment], t))
		{
			inverse_slope_left = retime_map_inverse_slope(map, segment - 1);
		}

		retime_value_at(src, t, value);

		if (blocks == 3)
		{
			cgltf_float deriv_left[16];
			cgltf_float deriv_right[16];
			cgltf_float* in_tangent = block;
			cgltf_float* out_tangent = block + 2 * src->comps;
			cgltf_size c;

			retime_derivatives_at(src, t, deriv_left, deriv_right);

			/*
			 * Outside a key span a channel is constant.  The segment that
			 * leaves or enters such a region must have zero tangents, so the
			 * constant extension is reproduced exactly.
			 */
			if (retime_float_equal(t, src->times[0]) && map->source_begin < src->times[0])
			{
				for (c = 0; c < src->comps; ++c)
				{
					deriv_left[c] = 0.0f;
				}
			}
			if (retime_float_equal(t, src->times[src->keys - 1]) &&
					map->source_end > src->times[src->keys - 1])
			{
				for (c = 0; c < src->comps; ++c)
				{
					deriv_right[c] = 0.0f;
				}
			}

			for (c = 0; c < src->comps; ++c)
			{
				in_tangent[c] = (cgltf_float)((double)deriv_left[c] * inverse_slope_left);
				out_tangent[c] = (cgltf_float)((double)deriv_right[c] * inverse_slope_right);
			}
			memcpy(block + src->comps, value, (size_t)src->comps * sizeof(cgltf_float));
		}
		else
		{
			memcpy(block, value, (size_t)src->comps * sizeof(cgltf_float));
		}
	}

	free(candidates);
	return cgltf_result_success;
}

/* ------------------------------------------------------------------ */
/* duplication of the source asset                                     */
/* ------------------------------------------------------------------ */

static cgltf_result retime_duplicate(const cgltf_data* source, cgltf_data** out_data)
{
	cgltf_options options;
	char* json = NULL;
	cgltf_size json_size;
	cgltf_data* copy = NULL;
	cgltf_size i;
	cgltf_result result;

	*out_data = NULL;
	memset(&options, 0, sizeof(options));

	json_size = cgltf_write(&options, NULL, 0, source);
	if (json_size <= 1)
	{
		return cgltf_result_invalid_gltf;
	}
	json = (char*)malloc((size_t)json_size);
	if (!json)
	{
		return cgltf_result_out_of_memory;
	}
	if (cgltf_write(&options, json, json_size, source) != json_size)
	{
		free(json);
		return cgltf_result_io_error;
	}

	result = cgltf_parse(&options, json, json_size - 1, &copy);
	if (result != cgltf_result_success)
	{
		free(json);
		return result;
	}
	copy->file_data = json;
	copy->file_size = json_size;

	/* Replace every external buffer reference with self-contained bytes. */
	if (copy->buffers_count != source->buffers_count)
	{
		cgltf_free(copy);
		return cgltf_result_invalid_gltf;
	}
	for (i = 0; i < copy->buffers_count; ++i)
	{
		char* uri;
		if (source->buffers[i].size > 0 && !source->buffers[i].data)
		{
			result = cgltf_result_data_too_short;
			goto failure;
		}
		uri = retime_data_uri(source->buffers[i].data, source->buffers[i].size);
		if (!uri)
		{
			result = cgltf_result_out_of_memory;
			goto failure;
		}
		copy->memory.free_func(copy->memory.user_data, copy->buffers[i].uri);
		copy->buffers[i].uri = uri;
		copy->buffers[i].data = NULL;
		copy->buffers[i].data_free_method = cgltf_data_free_method_none;
	}

	result = cgltf_load_buffers(&options, copy, NULL);
	if (result != cgltf_result_success)
	{
		goto failure;
	}

	*out_data = copy;
	return cgltf_result_success;

failure:
	cgltf_free(copy);
	return result;
}

/* ------------------------------------------------------------------ */
/* relocation of internal pointer arrays after reallocation            */
/* ------------------------------------------------------------------ */

static void retime_free_extras(cgltf_data* data, cgltf_extras* extras)
{
	data->memory.free_func(data->memory.user_data, extras->data);
	extras->data = NULL;
}

static void retime_free_extensions(cgltf_data* data, cgltf_extension* extensions, cgltf_size count)
{
	cgltf_size i;
	for (i = 0; i < count; ++i)
	{
		data->memory.free_func(data->memory.user_data, extensions[i].name);
		data->memory.free_func(data->memory.user_data, extensions[i].data);
	}
	data->memory.free_func(data->memory.user_data, extensions);
}

static void retime_relocate_accessors(cgltf_data* data, cgltf_accessor* old_base,
		cgltf_accessor* new_base, cgltf_size old_count)
{
	cgltf_size i, j, k, m;

	if (!old_base || old_count == 0)
	{
		return;
	}

#define RETIME_RELOC_ACC(pointer) \
	do { if (pointer) { (pointer) = (cgltf_accessor*)(void*)((char*)(new_base) + \
			((char*)(pointer) - (char*)(old_base))); } } while (0)

	for (i = 0; i < data->meshes_count; ++i)
	{
		for (j = 0; j < data->meshes[i].primitives_count; ++j)
		{
			cgltf_primitive* prim = &data->meshes[i].primitives[j];
			RETIME_RELOC_ACC(prim->indices);
			for (k = 0; k < prim->attributes_count; ++k)
			{
				RETIME_RELOC_ACC(prim->attributes[k].data);
			}
			for (k = 0; k < prim->targets_count; ++k)
			{
				for (m = 0; m < prim->targets[k].attributes_count; ++m)
				{
					RETIME_RELOC_ACC(prim->targets[k].attributes[m].data);
				}
			}
			if (prim->has_draco_mesh_compression)
			{
				for (k = 0; k < prim->draco_mesh_compression.attributes_count; ++k)
				{
					RETIME_RELOC_ACC(prim->draco_mesh_compression.attributes[k].data);
				}
			}
		}
	}

	for (i = 0; i < data->skins_count; ++i)
	{
		RETIME_RELOC_ACC(data->skins[i].inverse_bind_matrices);
	}

	for (i = 0; i < data->nodes_count; ++i)
	{
		if (data->nodes[i].has_mesh_gpu_instancing)
		{
			for (k = 0; k < data->nodes[i].mesh_gpu_instancing.attributes_count; ++k)
			{
				RETIME_RELOC_ACC(data->nodes[i].mesh_gpu_instancing.attributes[k].data);
			}
		}
	}

	for (i = 0; i < data->animations_count; ++i)
	{
		for (j = 0; j < data->animations[i].samplers_count; ++j)
		{
			RETIME_RELOC_ACC(data->animations[i].samplers[j].input);
			RETIME_RELOC_ACC(data->animations[i].samplers[j].output);
		}
	}

#undef RETIME_RELOC_ACC
}

static void retime_relocate_buffer_views(cgltf_data* data, cgltf_buffer_view* old_base,
		cgltf_buffer_view* new_base, cgltf_size old_count)
{
	cgltf_size i, j;

	if (!old_base || old_count == 0)
	{
		return;
	}

#define RETIME_RELOC_VIEW(pointer) \
	do { if (pointer) { (pointer) = (cgltf_buffer_view*)(void*)((char*)(new_base) + \
			((char*)(pointer) - (char*)(old_base))); } } while (0)

	for (i = 0; i < data->accessors_count; ++i)
	{
		RETIME_RELOC_VIEW(data->accessors[i].buffer_view);
		if (data->accessors[i].is_sparse)
		{
			RETIME_RELOC_VIEW(data->accessors[i].sparse.indices_buffer_view);
			RETIME_RELOC_VIEW(data->accessors[i].sparse.values_buffer_view);
		}
	}

	for (i = 0; i < data->images_count; ++i)
	{
		RETIME_RELOC_VIEW(data->images[i].buffer_view);
	}

	for (i = 0; i < data->meshes_count; ++i)
	{
		for (j = 0; j < data->meshes[i].primitives_count; ++j)
		{
			if (data->meshes[i].primitives[j].has_draco_mesh_compression)
			{
				RETIME_RELOC_VIEW(data->meshes[i].primitives[j].draco_mesh_compression.buffer_view);
			}
		}
	}

#undef RETIME_RELOC_VIEW
}

static void retime_relocate_buffers(cgltf_data* data, cgltf_buffer* old_base,
		cgltf_buffer* new_base, cgltf_size old_count)
{
	cgltf_size i;

	if (!old_base || old_count == 0)
	{
		return;
	}

#define RETIME_RELOC_BUF(pointer) \
	do { if (pointer) { (pointer) = (cgltf_buffer*)(void*)((char*)(new_base) + \
			((char*)(pointer) - (char*)(old_base))); } } while (0)

	for (i = 0; i < data->buffer_views_count; ++i)
	{
		RETIME_RELOC_BUF(data->buffer_views[i].buffer);
		if (data->buffer_views[i].has_meshopt_compression)
		{
			RETIME_RELOC_BUF(data->buffer_views[i].meshopt_compression.buffer);
		}
	}

#undef RETIME_RELOC_BUF
}

/*
 * Reallocate one of the data arrays and repair the pointers into it.
 * kind: 0 = buffers, 1 = buffer views, 2 = accessors.
 */
static void* retime_grow(cgltf_data* data, void* old_base, cgltf_size element_size,
		cgltf_size old_count, cgltf_size new_count, int kind)
{
	void* new_base;
	cgltf_size bytes;

	if (new_count == 0 || new_count > (cgltf_size)-1 / element_size)
	{
		return NULL;
	}
	bytes = new_count * element_size;
	new_base = data->memory.alloc_func(data->memory.user_data, bytes);
	if (!new_base)
	{
		return NULL;
	}
	memset(new_base, 0, (size_t)bytes);
	if (old_base && old_count > 0)
	{
		memcpy(new_base, old_base, (size_t)(old_count * element_size));
	}

	if (kind == 0)
	{
		retime_relocate_buffers(data, (cgltf_buffer*)old_base, (cgltf_buffer*)new_base, old_count);
	}
	else if (kind == 1)
	{
		retime_relocate_buffer_views(data, (cgltf_buffer_view*)old_base, (cgltf_buffer_view*)new_base, old_count);
	}
	else
	{
		retime_relocate_accessors(data, (cgltf_accessor*)old_base, (cgltf_accessor*)new_base, old_count);
	}

	if (old_base)
	{
		data->memory.free_func(data->memory.user_data, old_base);
	}
	return new_base;
}

/* ------------------------------------------------------------------ */
/* applying the retimed animation to the copy                          */
/* ------------------------------------------------------------------ */

static cgltf_result retime_apply(cgltf_data* data, cgltf_size animation_index,
		const retime_out_channel* channels, cgltf_size channel_count)
{
	cgltf_animation* animation;
	cgltf_node** targets = NULL;
	cgltf_animation_path_type* paths = NULL;
	cgltf_size old_samplers_count;
	cgltf_size old_channels_count;
	cgltf_size i;

	if (animation_index >= data->animations_count)
	{
		return cgltf_result_invalid_options;
	}
	animation = &data->animations[animation_index];
	if (animation->channels_count != channel_count)
	{
		return cgltf_result_invalid_gltf;
	}

	targets = (cgltf_node**)malloc((size_t)(channel_count ? channel_count : 1) * sizeof(cgltf_node*));
	paths = (cgltf_animation_path_type*)malloc((size_t)(channel_count ? channel_count : 1) * sizeof(cgltf_animation_path_type));
	if (!targets || !paths)
	{
		free(targets);
		free(paths);
		return cgltf_result_out_of_memory;
	}
	for (i = 0; i < channel_count; ++i)
	{
		targets[i] = animation->channels[i].target_node;
		paths[i] = animation->channels[i].target_path;
	}

	/* Release the old sampler/channel arrays (they own extras/extensions). */
	old_samplers_count = animation->samplers_count;
	old_channels_count = animation->channels_count;
	for (i = 0; i < old_samplers_count; ++i)
	{
		retime_free_extensions(data, animation->samplers[i].extensions,
				animation->samplers[i].extensions_count);
		retime_free_extras(data, &animation->samplers[i].extras);
	}
	data->memory.free_func(data->memory.user_data, animation->samplers);
	for (i = 0; i < old_channels_count; ++i)
	{
		retime_free_extensions(data, animation->channels[i].extensions,
				animation->channels[i].extensions_count);
		retime_free_extras(data, &animation->channels[i].extras);
	}
	data->memory.free_func(data->memory.user_data, animation->channels);

	animation->samplers = NULL;
	animation->samplers_count = 0;
	animation->channels = NULL;
	animation->channels_count = 0;

	if (channel_count > 0)
	{
		animation->samplers = (cgltf_animation_sampler*)data->memory.alloc_func(
				data->memory.user_data, (size_t)channel_count * sizeof(cgltf_animation_sampler));
		animation->channels = (cgltf_animation_channel*)data->memory.alloc_func(
				data->memory.user_data, (size_t)channel_count * sizeof(cgltf_animation_channel));
		if (!animation->samplers || !animation->channels)
		{
			data->memory.free_func(data->memory.user_data, animation->samplers);
			data->memory.free_func(data->memory.user_data, animation->channels);
			animation->samplers = NULL;
			animation->channels = NULL;
			free(targets);
			free(paths);
			return cgltf_result_out_of_memory;
		}
		memset(animation->samplers, 0, (size_t)channel_count * sizeof(cgltf_animation_sampler));
		memset(animation->channels, 0, (size_t)channel_count * sizeof(cgltf_animation_channel));
	}
	for (i = 0; i < channel_count; ++i)
	{
		animation->samplers[i].interpolation = channels[i].interpolation;
		animation->channels[i].target_node = targets[i];
		animation->channels[i].target_path = paths[i];
	}
	animation->samplers_count = channel_count;
	animation->channels_count = channel_count;

	free(targets);
	free(paths);

	if (channel_count == 0)
	{
		return cgltf_result_success;
	}

	/* Compute the byte layout of the appended buffer. */
	{
		cgltf_size total_size = 0;
		cgltf_size* input_offsets = NULL;
		cgltf_size* output_offsets = NULL;
		unsigned char* buffer_data = NULL;
		cgltf_size buffer_index;
		cgltf_size old_buffers_count = data->buffers_count;
		cgltf_size old_views_count = data->buffer_views_count;
		cgltf_size old_accessors_count = data->accessors_count;

		input_offsets = (cgltf_size*)malloc((size_t)channel_count * sizeof(cgltf_size));
		output_offsets = (cgltf_size*)malloc((size_t)channel_count * sizeof(cgltf_size));
		if (!input_offsets || !output_offsets)
		{
			free(input_offsets);
			free(output_offsets);
			return cgltf_result_out_of_memory;
		}

		for (i = 0; i < channel_count; ++i)
		{
			cgltf_size input_bytes;
			cgltf_size output_bytes;
			cgltf_size elements;
			cgltf_size output_floats;
			if (retime_mul_overflow(channels[i].keys, sizeof(cgltf_float), &input_bytes))
			{
				free(input_offsets);
				free(output_offsets);
				return cgltf_result_out_of_memory;
			}
			if (retime_mul_overflow(channels[i].comps, channels[i].blocks, &elements) ||
					retime_mul_overflow(channels[i].keys, elements, &output_floats) ||
					retime_mul_overflow(output_floats, sizeof(cgltf_float), &output_bytes))
			{
				free(input_offsets);
				free(output_offsets);
				return cgltf_result_out_of_memory;
			}
			if (input_bytes > (cgltf_size)-1 - total_size)
			{
				free(input_offsets);
				free(output_offsets);
				return cgltf_result_out_of_memory;
			}
			input_offsets[i] = total_size;
			total_size += input_bytes;
			if (output_bytes > (cgltf_size)-1 - total_size)
			{
				free(input_offsets);
				free(output_offsets);
				return cgltf_result_out_of_memory;
			}
			output_offsets[i] = total_size;
			total_size += output_bytes;
		}

		buffer_data = (unsigned char*)malloc((size_t)total_size);
		if (!buffer_data)
		{
			free(input_offsets);
			free(output_offsets);
			return cgltf_result_out_of_memory;
		}
		for (i = 0; i < channel_count; ++i)
		{
			cgltf_size output_floats = channels[i].keys * channels[i].comps * channels[i].blocks;
			memcpy(buffer_data + input_offsets[i], channels[i].times,
					(size_t)(channels[i].keys * sizeof(cgltf_float)));
			memcpy(buffer_data + output_offsets[i], channels[i].values,
					(size_t)(output_floats * sizeof(cgltf_float)));
		}

		/* 1. append the buffer (repairs buffer pointers in buffer views). */
		{
			cgltf_buffer* grown = (cgltf_buffer*)retime_grow(data, data->buffers,
					sizeof(cgltf_buffer), old_buffers_count, old_buffers_count + 1, 0);
			cgltf_buffer* buffer;
			if (!grown)
			{
				free(buffer_data);
				free(input_offsets);
				free(output_offsets);
				return cgltf_result_out_of_memory;
			}
			data->buffers = grown;
			data->buffers_count = old_buffers_count + 1;
			buffer_index = old_buffers_count;
			buffer = &data->buffers[buffer_index];
			memset(buffer, 0, sizeof(*buffer));
			buffer->size = total_size;
			buffer->data = buffer_data;
			buffer->data_free_method = cgltf_data_free_method_memory_free;
			buffer->uri = retime_data_uri(buffer_data, total_size);
			if (!buffer->uri)
			{
				free(input_offsets);
				free(output_offsets);
				return cgltf_result_out_of_memory;
			}
		}

		/* 2. append one input and one output view per channel. */
		{
			void* grown = retime_grow(data, data->buffer_views, sizeof(cgltf_buffer_view),
					old_views_count, old_views_count + channel_count * 2, 1);
			if (!grown)
			{
				free(input_offsets);
				free(output_offsets);
				return cgltf_result_out_of_memory;
			}
			data->buffer_views = (cgltf_buffer_view*)grown;
			data->buffer_views_count = old_views_count + channel_count * 2;
		}
		for (i = 0; i < channel_count; ++i)
		{
			cgltf_buffer_view* input_view = &data->buffer_views[old_views_count + i * 2];
			cgltf_buffer_view* output_view = &data->buffer_views[old_views_count + i * 2 + 1];
			memset(input_view, 0, sizeof(*input_view));
			memset(output_view, 0, sizeof(*output_view));
			input_view->buffer = &data->buffers[buffer_index];
			input_view->offset = input_offsets[i];
			input_view->size = channels[i].keys * sizeof(cgltf_float);
			input_view->type = cgltf_buffer_view_type_invalid;
			output_view->buffer = &data->buffers[buffer_index];
			output_view->offset = output_offsets[i];
			output_view->size = channels[i].keys * channels[i].comps *
					channels[i].blocks * sizeof(cgltf_float);
			output_view->type = cgltf_buffer_view_type_invalid;
		}

		/* 3. append one input and one output accessor per channel. */
		{
			void* grown = retime_grow(data, data->accessors, sizeof(cgltf_accessor),
					old_accessors_count, old_accessors_count + channel_count * 2, 2);
			if (!grown)
			{
				free(input_offsets);
				free(output_offsets);
				return cgltf_result_out_of_memory;
			}
			data->accessors = (cgltf_accessor*)grown;
			data->accessors_count = old_accessors_count + channel_count * 2;
		}
		for (i = 0; i < channel_count; ++i)
		{
			cgltf_accessor* input_accessor = &data->accessors[old_accessors_count + i * 2];
			cgltf_accessor* output_accessor = &data->accessors[old_accessors_count + i * 2 + 1];
			memset(input_accessor, 0, sizeof(*input_accessor));
			memset(output_accessor, 0, sizeof(*output_accessor));

			input_accessor->component_type = cgltf_component_type_r_32f;
			input_accessor->type = cgltf_type_scalar;
			input_accessor->count = channels[i].keys;
			input_accessor->offset = 0;
			input_accessor->stride = sizeof(cgltf_float);
			input_accessor->buffer_view = &data->buffer_views[old_views_count + i * 2];
			input_accessor->has_min = 1;
			input_accessor->min[0] = channels[i].times[0];
			input_accessor->has_max = 1;
			input_accessor->max[0] = channels[i].times[channels[i].keys - 1];

			output_accessor->component_type = cgltf_component_type_r_32f;
			output_accessor->type = channels[i].type;
			output_accessor->count = channels[i].accessor_count;
			output_accessor->offset = 0;
			output_accessor->stride = channels[i].vec * sizeof(cgltf_float);
			output_accessor->buffer_view = &data->buffer_views[old_views_count + i * 2 + 1];

			animation->samplers[i].input = input_accessor;
			animation->samplers[i].output = output_accessor;
			animation->channels[i].sampler = &animation->samplers[i];
		}

		free(input_offsets);
		free(output_offsets);
	}

	return cgltf_result_success;
}

/* ------------------------------------------------------------------ */
/* public entry point                                                  */
/* ------------------------------------------------------------------ */

static cgltf_result retime_validate_arguments(const cgltf_data* source,
		cgltf_size animation_index, const cgltf_float* source_times,
		const cgltf_float* output_times, cgltf_size count)
{
	cgltf_size i;

	if (!source || animation_index >= source->animations_count)
	{
		return cgltf_result_invalid_options;
	}
	if (!source_times || !output_times || count < 2)
	{
		return cgltf_result_invalid_options;
	}
	for (i = 0; i < count; ++i)
	{
		if (!isfinite(source_times[i]) || !isfinite(output_times[i]))
		{
			return cgltf_result_invalid_options;
		}
		if (i > 0 && (!(source_times[i] > source_times[i - 1]) ||
				!(output_times[i] > output_times[i - 1])))
		{
			return cgltf_result_invalid_options;
		}
	}
	if (output_times[0] != 0.0f)
	{
		return cgltf_result_invalid_options;
	}
	return cgltf_result_success;
}

/* Reject compressed geometry/buffer extensions we cannot copy verbatim. */
static cgltf_result retime_reject_compressed(const cgltf_data* source)
{
	cgltf_size i, j;

	for (i = 0; i < source->buffer_views_count; ++i)
	{
		if (source->buffer_views[i].has_meshopt_compression)
		{
			return cgltf_result_unknown_format;
		}
	}
	for (i = 0; i < source->meshes_count; ++i)
	{
		for (j = 0; j < source->meshes[i].primitives_count; ++j)
		{
			if (source->meshes[i].primitives[j].has_draco_mesh_compression)
			{
				return cgltf_result_unknown_format;
			}
		}
	}
	return cgltf_result_success;
}

/*
 * Duplicate target (node, path) pairs are ambiguous and rejected.  A channel
 * may omit target.node (glTF applies it to every node); that is preserved.
 */
static cgltf_result retime_check_targets(const cgltf_animation* animation)
{
	cgltf_size i, j;

	for (i = 0; i < animation->channels_count; ++i)
	{
		const cgltf_animation_channel* a = &animation->channels[i];
		if (a->target_path == cgltf_animation_path_type_invalid)
		{
			return cgltf_result_invalid_gltf;
		}
		for (j = 0; j < i; ++j)
		{
			const cgltf_animation_channel* b = &animation->channels[j];
			if (a->target_node == b->target_node && a->target_path == b->target_path)
			{
				return cgltf_result_invalid_gltf;
			}
		}
	}
	return cgltf_result_success;
}

cgltf_result cgltf_retime_animation(const cgltf_data* source, cgltf_size animation_index,
		const cgltf_float* source_times, const cgltf_float* output_times,
		cgltf_size count, cgltf_data** result)
{
	cgltf_result status;
	const cgltf_animation* animation;
	retime_src_channel* source_channels = NULL;
	retime_out_channel* output_channels = NULL;
	cgltf_size channel_count;
	cgltf_size i;
	cgltf_float global_min = 0.0f;
	cgltf_float global_max = 0.0f;
	int have_range = 0;
	retime_map map;
	cgltf_data* copy = NULL;

	if (!result)
	{
		return cgltf_result_invalid_options;
	}
	*result = NULL;

	status = retime_validate_arguments(source, animation_index, source_times, output_times, count);
	if (status != cgltf_result_success)
	{
		return status;
	}
	status = retime_reject_compressed(source);
	if (status != cgltf_result_success)
	{
		return status;
	}

	animation = &source->animations[animation_index];

	/* The mapped span must stay inside the animation's combined time range. */
	for (i = 0; i < animation->samplers_count; ++i)
	{
		cgltf_float first, last;
		status = retime_read_input_bounds(animation->samplers[i].input, &first, &last);
		if (status != cgltf_result_success)
		{
			return status;
		}
		if (!isfinite(first) || !isfinite(last) || last < first)
		{
			return cgltf_result_invalid_gltf;
		}
		if (!have_range)
		{
			global_min = first;
			global_max = last;
			have_range = 1;
		}
		else
		{
			if (first < global_min)
			{
				global_min = first;
			}
			if (last > global_max)
			{
				global_max = last;
			}
		}
	}
	if (!have_range)
	{
		return cgltf_result_invalid_gltf;
	}
	if (source_times[0] < global_min || source_times[count - 1] > global_max)
	{
		return cgltf_result_invalid_options;
	}

	status = retime_check_targets(animation);
	if (status != cgltf_result_success)
	{
		return status;
	}

	map.source_times = source_times;
	map.output_times = output_times;
	map.count = count;
	map.source_begin = source_times[0];
	map.source_end = source_times[count - 1];

	channel_count = animation->channels_count;
	source_channels = (retime_src_channel*)calloc(
			(size_t)(channel_count ? channel_count : 1), sizeof(retime_src_channel));
	output_channels = (retime_out_channel*)calloc(
			(size_t)(channel_count ? channel_count : 1), sizeof(retime_out_channel));
	if (!source_channels || !output_channels)
	{
		free(source_channels);
		free(output_channels);
		return cgltf_result_out_of_memory;
	}

	for (i = 0; i < channel_count; ++i)
	{
		status = retime_read_src_channel(source, &animation->channels[i], &source_channels[i]);
		if (status != cgltf_result_success)
		{
			goto cleanup;
		}
		status = retime_build_out_channel(&source_channels[i], &map, &output_channels[i]);
		if (status != cgltf_result_success)
		{
			goto cleanup;
		}
	}

	status = retime_duplicate(source, &copy);
	if (status != cgltf_result_success)
	{
		goto cleanup;
	}
	status = retime_apply(copy, animation_index, output_channels, channel_count);
	if (status != cgltf_result_success)
	{
		cgltf_free(copy);
		copy = NULL;
		goto cleanup;
	}

	*result = copy;
	status = cgltf_result_success;

cleanup:
	for (i = 0; i < channel_count; ++i)
	{
		retime_free_src_channel(&source_channels[i]);
		retime_free_out_channel(&output_channels[i]);
	}
	free(source_channels);
	free(output_channels);
	return status;
}
