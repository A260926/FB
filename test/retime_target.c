/*
 * retime_target.c - target tests for examples/cgltf_retime.c
 *
 * Linked by task_support/driver.c together with task_support/cgltf_impl.c and
 * examples/cgltf_retime.c.  This file defines neither main nor the cgltf
 * implementation macros.
 *
 * Trajectories are checked against an independent STEP / LINEAR / CUBICSPLINE
 * evaluator and an independent shortest-arc slerp.  The tolerance follows the
 * absolute + relative 2e-5 rule.
 */

#include "cgltf.h"
#include "cgltf_write.h"
#include "examples/cgltf_retime.h"
#include "task_support/check.h"

#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* JSON / binary builder                                               */
/* ------------------------------------------------------------------ */

typedef struct builder {
	unsigned char* bin;
	size_t bin_len;
	size_t bin_cap;
	char* views;
	size_t views_len;
	size_t views_cap;
	char* accs;
	size_t accs_len;
	size_t accs_cap;
	char* extra;
	size_t extra_len;
	size_t extra_cap;
	int n_views;
	int n_accessors;
} builder;

static void die(const char* what)
{
	fprintf(stderr, "retime_target: %s\n", what);
	exit(2);
}

static void sb_appendf(char** buf, size_t* len, size_t* cap, const char* fmt, ...)
{
	char tmp[4096];
	va_list ap;
	int n;
	va_start(ap, fmt);
	n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
	va_end(ap);
	if (n < 0)
	{
		die("vsnprintf");
	}
	if (*len + (size_t)n + 1 > *cap)
	{
		size_t want = *len + (size_t)n + 1;
		size_t next = *cap ? *cap : 1024;
		while (next < want)
		{
			next *= 2;
		}
		*buf = (char*)realloc(*buf, next);
		if (!*buf)
		{
			die("realloc");
		}
		*cap = next;
	}
	memcpy(*buf + *len, tmp, (size_t)n);
	*len += (size_t)n;
	(*buf)[*len] = '\0';
}

static void b_init(builder* b)
{
	memset(b, 0, sizeof(*b));
}

static void b_destroy(builder* b)
{
	free(b->bin);
	free(b->views);
	free(b->accs);
	free(b->extra);
	memset(b, 0, sizeof(*b));
}

static size_t b_bin_add(builder* b, const void* data, size_t bytes)
{
	size_t offset = b->bin_len;
	if (b->bin_len + bytes > b->bin_cap)
	{
		size_t next = b->bin_cap ? b->bin_cap : 4096;
		while (next < b->bin_len + bytes)
		{
			next *= 2;
		}
		b->bin = (unsigned char*)realloc(b->bin, next);
		if (!b->bin)
		{
			die("realloc bin");
		}
		b->bin_cap = next;
	}
	if (bytes)
	{
		memcpy(b->bin + b->bin_len, data, bytes);
	}
	b->bin_len += bytes;
	return offset;
}

static size_t b_bin_floats(builder* b, const float* values, size_t count)
{
	return b_bin_add(b, values, count * sizeof(float));
}

static size_t b_bin_u32(builder* b, const uint32_t* values, size_t count)
{
	return b_bin_add(b, values, count * sizeof(uint32_t));
}

static int b_view(builder* b, size_t offset, size_t length, size_t byte_stride)
{
	int index = b->n_views++;
	sb_appendf(&b->views, &b->views_len, &b->views_cap,
			"%s{\"buffer\":0,\"byteOffset\":%zu,\"byteLength\":%zu",
			index ? "," : "", offset, length);
	if (byte_stride)
	{
		sb_appendf(&b->views, &b->views_len, &b->views_cap,
				",\"byteStride\":%zu", byte_stride);
	}
	sb_appendf(&b->views, &b->views_len, &b->views_cap, "}");
	return index;
}

static int b_accessor_raw(builder* b, int view, size_t byte_offset, int count,
		const char* type, const char* extra)
{
	int index = b->n_accessors++;
	sb_appendf(&b->accs, &b->accs_len, &b->accs_cap,
			"%s{\"bufferView\":%d,\"byteOffset\":%zu,\"componentType\":5126,"
			"\"count\":%d,\"type\":\"%s\"%s}",
			index ? "," : "", view, byte_offset, count, type, extra ? extra : "");
	return index;
}

/*
 * Add `count` float elements with the given stride.  `src` must hold
 * count * stride_floats floats.  `offset_floats` floats of zero padding are
 * prepended and become the accessor byteOffset.
 */
static int b_accessor_f32(builder* b, const float* src, int count, int stride_floats,
		int offset_floats, const char* type, const char* extra)
{
	size_t base;
	float zero = 0.0f;
	int i;
	int view;
	if (stride_floats <= 0 || offset_floats < 0)
	{
		return -1;
	}
	base = b->bin_len;
	for (i = 0; i < offset_floats; ++i)
	{
		b_bin_floats(b, &zero, 1);
	}
	b_bin_floats(b, src, (size_t)count * (size_t)stride_floats);
	view = b_view(b, base, b->bin_len - base, (size_t)stride_floats * sizeof(float));
	return b_accessor_raw(b, view, (size_t)offset_floats * sizeof(float), count, type, extra);
}

static int b_times(builder* b, const float* times, int count)
{
	return b_accessor_f32(b, times, count, 1, 0, "SCALAR", NULL);
}

static int b_vec3s(builder* b, const float* values, int count)
{
	return b_accessor_f32(b, values, count, 3, 0, "VEC3", NULL);
}

static int b_vec4s(builder* b, const float* values, int count)
{
	return b_accessor_f32(b, values, count, 4, 0, "VEC4", NULL);
}

static int b_scalars(builder* b, const float* values, int count)
{
	return b_accessor_f32(b, values, count, 1, 0, "SCALAR", NULL);
}

/* Per key: [in, value, out]. */
static int b_cubic_vec3(builder* b, const float* blocks, int keys)
{
	return b_accessor_f32(b, blocks, keys * 3, 3, 0, "VEC3", NULL);
}

static int b_cubic_vec4(builder* b, const float* blocks, int keys)
{
	return b_accessor_f32(b, blocks, keys * 3, 4, 0, "VEC4", NULL);
}

/* Per key: [in(targets), value(targets), out(targets)], flattened SCALAR. */
static int b_cubic_weights(builder* b, const float* blocks, int keys, int targets)
{
	return b_accessor_f32(b, blocks, keys * targets * 3, 1, 0, "SCALAR", NULL);
}

static char* base64_encode(const unsigned char* data, size_t size)
{
	static const char alphabet[] =
		"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	char* out;
	size_t groups = (size + 2) / 3;
	size_t i;
	out = (char*)malloc(groups * 4 + 1);
	if (!out)
	{
		die("base64 alloc");
	}
	for (i = 0; i < size; i += 3)
	{
		size_t remain = size - i;
		unsigned int a = data[i];
		unsigned int b = remain > 1 ? data[i + 1] : 0;
		unsigned int c = remain > 2 ? data[i + 2] : 0;
		unsigned int triple = (a << 16) | (b << 8) | c;
		out[(i / 3) * 4 + 0] = alphabet[(triple >> 18) & 63];
		out[(i / 3) * 4 + 1] = alphabet[(triple >> 12) & 63];
		out[(i / 3) * 4 + 2] = remain > 1 ? alphabet[(triple >> 6) & 63] : '=';
		out[(i / 3) * 4 + 3] = remain > 2 ? alphabet[triple & 63] : '=';
	}
	out[groups * 4] = '\0';
	return out;
}

static char* b_finish(builder* b)
{
	char* uri = base64_encode(b->bin, b->bin_len);
	char* json = (char*)malloc(1);
	size_t len = 0, cap = 1;
	if (!json)
	{
		die("json alloc");
	}
	json[0] = '\0';
	sb_appendf(&json, &len, &cap,
			"{\"asset\":{\"version\":\"2.0\"},"
			"\"buffers\":[{\"byteLength\":%zu,\"uri\":\"data:application/octet-stream;base64,%s\"}],"
			"\"bufferViews\":[%s],\"accessors\":[%s]%s}",
			b->bin_len, uri, b->views ? b->views : "",
			b->accs ? b->accs : "", b->extra ? b->extra : "");
	free(uri);
	return json;
}

static cgltf_data* parse_json_asset(const char* json, int load)
{
	cgltf_options options;
	cgltf_data* data = NULL;
	cgltf_result result;
	memset(&options, 0, sizeof(options));
	result = cgltf_parse(&options, json, strlen(json), &data);
	if (result != cgltf_result_success)
	{
		return NULL;
	}
	if (load)
	{
		result = cgltf_load_buffers(&options, data, NULL);
		if (result != cgltf_result_success)
		{
			cgltf_free(data);
			return NULL;
		}
	}
	return data;
}

static cgltf_data* build_from_extra(builder* b)
{
	char* json = b_finish(b);
	cgltf_data* data = parse_json_asset(json, 1);
	free(json);
	return data;
}

/* ------------------------------------------------------------------ */
/* reference evaluation (independent of cgltf_retime)                  */
/* ------------------------------------------------------------------ */

typedef struct ref_chan {
	int path;
	int interp;
	int comps;
	size_t keys;
	float* times;
	float* values;      /* keys * comps */
	float* in_tangent;  /* keys * comps, cubic only */
	float* out_tangent; /* keys * comps, cubic only */
} ref_chan;

static int ref_comps(const cgltf_animation_channel* channel)
{
	switch (channel->target_path)
	{
	case cgltf_animation_path_type_translation:
	case cgltf_animation_path_type_scale:
		return 3;
	case cgltf_animation_path_type_rotation:
		return 4;
	case cgltf_animation_path_type_weights:
		if (channel->target_node && channel->target_node->mesh &&
				channel->target_node->mesh->primitives_count)
		{
			return (int)channel->target_node->mesh->primitives[0].targets_count;
		}
		return 0;
	default:
		return 0;
	}
}

static void ref_free(ref_chan* rc)
{
	free(rc->times);
	free(rc->values);
	free(rc->in_tangent);
	free(rc->out_tangent);
	memset(rc, 0, sizeof(*rc));
}

static int ref_init(ref_chan* rc, const cgltf_animation_channel* channel)
{
	cgltf_size input_floats;
	cgltf_size output_floats;
	float* input;
	float* output;
	int comps;
	int blocks;
	size_t i;
	int j;

	memset(rc, 0, sizeof(*rc));
	if (!channel || !channel->sampler || !channel->sampler->input ||
			!channel->sampler->output)
	{
		return 0;
	}
	comps = ref_comps(channel);
	if (comps <= 0)
	{
		return 0;
	}
	rc->path = (int)channel->target_path;
	rc->interp = (int)channel->sampler->interpolation;
	rc->comps = comps;
	rc->keys = channel->sampler->input->count;
	blocks = rc->interp == cgltf_interpolation_type_cubic_spline ? 3 : 1;

	input_floats = cgltf_accessor_unpack_floats(channel->sampler->input, NULL, 0);
	output_floats = cgltf_accessor_unpack_floats(channel->sampler->output, NULL, 0);
	if (input_floats != rc->keys ||
			output_floats != rc->keys * (cgltf_size)comps * (cgltf_size)blocks)
	{
		return 0;
	}
	input = (float*)malloc(sizeof(float) * input_floats);
	output = (float*)malloc(sizeof(float) * output_floats);
	if (!input || !output)
	{
		free(input);
		free(output);
		return 0;
	}
	if (cgltf_accessor_unpack_floats(channel->sampler->input, input, input_floats) != input_floats ||
			cgltf_accessor_unpack_floats(channel->sampler->output, output, output_floats) != output_floats)
	{
		free(input);
		free(output);
		return 0;
	}

	rc->times = (float*)malloc(sizeof(float) * rc->keys);
	rc->values = (float*)malloc(sizeof(float) * rc->keys * (size_t)comps);
	if (!rc->times || !rc->values)
	{
		free(input);
		free(output);
		ref_free(rc);
		return 0;
	}
	for (i = 0; i < rc->keys; ++i)
	{
		rc->times[i] = input[i];
	}
	if (blocks == 3)
	{
		rc->in_tangent = (float*)malloc(sizeof(float) * rc->keys * (size_t)comps);
		rc->out_tangent = (float*)malloc(sizeof(float) * rc->keys * (size_t)comps);
		if (!rc->in_tangent || !rc->out_tangent)
		{
			free(input);
			free(output);
			ref_free(rc);
			return 0;
		}
	}
	for (i = 0; i < rc->keys; ++i)
	{
		const float* block = output + i * (size_t)comps * (size_t)blocks;
		for (j = 0; j < comps; ++j)
		{
			if (blocks == 3)
			{
				rc->in_tangent[i * comps + j] = block[j];
				rc->values[i * comps + j] = block[comps + j];
				rc->out_tangent[i * comps + j] = block[2 * comps + j];
			}
			else
			{
				rc->values[i * comps + j] = block[j];
			}
		}
	}
	free(input);
	free(output);
	return 1;
}

static void ref_normalize(const float* in, float* out)
{
	double x = in[0], y = in[1], z = in[2], w = in[3];
	double len = sqrt(x * x + y * y + z * z + w * w);
	if (len > 0.0)
	{
		out[0] = (float)(x / len);
		out[1] = (float)(y / len);
		out[2] = (float)(z / len);
		out[3] = (float)(w / len);
	}
	else
	{
		out[0] = out[1] = out[2] = 0.0f;
		out[3] = 1.0f;
	}
}

static void ref_slerp(const float* a, const float* b, double s, float* out)
{
	float qa[4], qb[4];
	double dot = 0.0, abs_dot, theta;
	int c;
	ref_normalize(a, qa);
	ref_normalize(b, qb);
	for (c = 0; c < 4; ++c)
	{
		dot += (double)qa[c] * qb[c];
	}
	abs_dot = dot < 0.0 ? -dot : dot;
	if (abs_dot > 0.9995)
	{
		float blend[4];
		for (c = 0; c < 4; ++c)
		{
			blend[c] = (float)((double)qa[c] + ((double)qb[c] - qa[c]) * s);
		}
		ref_normalize(blend, out);
		return;
	}
	theta = acos(abs_dot);
	{
		double wa = sin((1.0 - s) * theta) / sin(theta);
		double wb = sin(s * theta) / sin(theta);
		if (dot < 0.0)
		{
			wb = -wb;
		}
		for (c = 0; c < 4; ++c)
		{
			out[c] = (float)(wa * qa[c] + wb * qb[c]);
		}
	}
	ref_normalize(out, out);
}

static void ref_cubic(const ref_chan* rc, size_t seg, double s, float* out)
{
	double s2 = s * s, s3 = s2 * s;
	double h = (double)rc->times[seg + 1] - rc->times[seg];
	double p0 = 2.0 * s3 - 3.0 * s2 + 1.0;
	double m0 = s3 - 2.0 * s2 + s;
	double p1 = -2.0 * s3 + 3.0 * s2;
	double m1 = s3 - s2;
	int c;
	for (c = 0; c < rc->comps; ++c)
	{
		out[c] = (float)(p0 * rc->values[seg * rc->comps + c] +
				m0 * h * rc->out_tangent[seg * rc->comps + c] +
				p1 * rc->values[(seg + 1) * rc->comps + c] +
				m1 * h * rc->in_tangent[(seg + 1) * rc->comps + c]);
	}
}

static void ref_eval(const ref_chan* rc, double t, float* out)
{
	size_t i;
	if (rc->keys == 0)
	{
		memset(out, 0, sizeof(float) * (size_t)rc->comps);
		return;
	}
	if (t <= (double)rc->times[0])
	{
		memcpy(out, rc->values, sizeof(float) * (size_t)rc->comps);
		return;
	}
	if (t >= (double)rc->times[rc->keys - 1])
	{
		memcpy(out, rc->values + (rc->keys - 1) * rc->comps,
				sizeof(float) * (size_t)rc->comps);
		return;
	}
	for (i = 0; i + 1 < rc->keys; ++i)
	{
		if (t < (double)rc->times[i + 1])
		{
			break;
		}
	}
	{
		double t0 = rc->times[i], t1 = rc->times[i + 1];
		double s = (t1 > t0) ? (t - t0) / (t1 - t0) : 0.0;
		const float* v0 = rc->values + i * rc->comps;
		const float* v1 = rc->values + (i + 1) * rc->comps;
		int c;
		if (t == t0)
		{
			memcpy(out, v0, sizeof(float) * (size_t)rc->comps);
			return;
		}
		switch ((cgltf_interpolation_type)rc->interp)
		{
		case cgltf_interpolation_type_step:
			memcpy(out, v0, sizeof(float) * (size_t)rc->comps);
			break;
		case cgltf_interpolation_type_linear:
			if (rc->path == cgltf_animation_path_type_rotation && rc->comps == 4)
			{
				ref_slerp(v0, v1, s, out);
			}
			else
			{
				for (c = 0; c < rc->comps; ++c)
				{
					out[c] = (float)((double)v0[c] + ((double)v1[c] - v0[c]) * s);
				}
			}
			break;
		case cgltf_interpolation_type_cubic_spline:
			ref_cubic(rc, i, s, out);
			if (rc->path == cgltf_animation_path_type_rotation && rc->comps == 4)
			{
				ref_normalize(out, out);
			}
			break;
		default:
			memcpy(out, v0, sizeof(float) * (size_t)rc->comps);
			break;
		}
	}
}

/* Piecewise-linear time map. */
static double map_time(const float* st, const float* ot, size_t count, double t)
{
	size_t seg = 0;
	while (seg + 2 < count && (double)st[seg + 1] <= t)
	{
		++seg;
	}
	{
		double s0 = st[seg], s1 = st[seg + 1];
		double o0 = ot[seg], o1 = ot[seg + 1];
		if (s1 <= s0)
		{
			return o0;
		}
		return o0 + (t - s0) * (o1 - o0) / (s1 - s0);
	}
}

static double unmap_time(const float* st, const float* ot, size_t count, double o)
{
	size_t seg = 0;
	while (seg + 2 < count && (double)ot[seg + 1] <= o)
	{
		++seg;
	}
	{
		double s0 = st[seg], s1 = st[seg + 1];
		double o0 = ot[seg], o1 = ot[seg + 1];
		if (o1 <= o0)
		{
			return s0;
		}
		return s0 + (o - o0) * (s1 - s0) / (o1 - o0);
	}
}

static int close_enough(double a, double b)
{
	return fabs(a - b) <= 2e-5 + 2e-5 * fabs(b);
}

static const cgltf_animation_channel* find_channel(const cgltf_data* data,
		size_t animation_index, size_t node_index, cgltf_animation_path_type path)
{
	const cgltf_animation* animation;
	cgltf_size i;
	if (animation_index >= data->animations_count)
	{
		return NULL;
	}
	animation = &data->animations[animation_index];
	for (i = 0; i < animation->channels_count; ++i)
	{
		const cgltf_animation_channel* channel = &animation->channels[i];
		if (channel->target_path != path || !channel->target_node)
		{
			continue;
		}
		if (cgltf_node_index(data, channel->target_node) == node_index)
		{
			return channel;
		}
	}
	return NULL;
}

/* Compare a source channel with its counterpart at keys, knots and midpoints. */
static int compare_channel(const cgltf_data* source, size_t source_anim,
		size_t node_index, cgltf_animation_path_type path,
		const cgltf_data* result, size_t result_anim,
		const float* st, const float* ot, size_t count)
{
	const cgltf_animation_channel* sc =
			find_channel(source, source_anim, node_index, path);
	const cgltf_animation_channel* dc =
			find_channel(result, result_anim, node_index, path);
	ref_chan src, dst;
	float samples[128];
	size_t sample_count = 0;
	size_t i;
	int ok = 1;

	if (!sc || !dc || !ref_init(&src, sc) || !ref_init(&dst, dc))
	{
		fprintf(stderr, "  channel init failed node=%zu path=%d\n", node_index, (int)path);
		return 0;
	}

	for (i = 0; i < src.keys; ++i)
	{
		double t = src.times[i];
		if (t >= st[0] && t <= st[count - 1] &&
				sample_count < sizeof(samples) / sizeof(samples[0]))
		{
			samples[sample_count++] = (float)t;
		}
	}
	for (i = 0; i < count; ++i)
	{
		if (sample_count < sizeof(samples) / sizeof(samples[0]))
		{
			samples[sample_count++] = st[i];
		}
	}
	{
		size_t base = sample_count;
		for (i = 0; i + 1 < base; ++i)
		{
			float mid = (float)(((double)samples[i] + samples[i + 1]) / 2.0);
			if (sample_count < sizeof(samples) / sizeof(samples[0]))
			{
				samples[sample_count++] = mid;
			}
		}
	}

	for (i = 0; i < sample_count; ++i)
	{
		double s = samples[i];
		double o = map_time(st, ot, count, s);
		float a[8], b[8];
		int c;
		if (src.interp == cgltf_interpolation_type_step)
		{
			/* STEP is discontinuous at keys: evaluate just on both sides so
			 * that a one-ulp difference in the mapped key time cannot put the
			 * two assets on different sides of the step. */
			double span = (double)st[count - 1] - (double)st[0];
			double eps = span > 0.0 ? span * 1e-4 : 1e-4;
			double offsets[2];
			int pass;
			offsets[0] = -eps;
			offsets[1] = eps;
			for (pass = 0; pass < 2; ++pass)
			{
				double ss = s + offsets[pass];
				double oo;
				if (ss < (double)st[0] || ss > (double)st[count - 1])
				{
					/* Outside the selected span the result is constant at the
					 * span endpoint, which need not match the source's own
					 * extension, so only in-span samples are compared. */
					continue;
				}
				oo = map_time(st, ot, count, ss);
				ref_eval(&src, ss, a);
				ref_eval(&dst, oo, b);
				for (c = 0; c < src.comps; ++c)
				{
					if (!close_enough(a[c], b[c]))
					{
						fprintf(stderr, "  step mismatch t=%g comp=%d got=%g want=%g\n",
								ss, c, b[c], a[c]);
						ok = 0;
						break;
					}
				}
				if (!ok)
				{
					break;
				}
			}
			if (!ok)
			{
				fprintf(stderr, "  (channel node=%zu path=%d)\n", node_index, (int)path);
				break;
			}
			continue;
		}
		ref_eval(&src, s, a);
		ref_eval(&dst, o, b);
		if (src.path == cgltf_animation_path_type_rotation && src.comps == 4)
		{
			int direct = 1, negated = 1;
			for (c = 0; c < 4; ++c)
			{
				if (!close_enough(a[c], b[c]))
				{
					direct = 0;
				}
				if (!close_enough(a[c], -b[c]))
				{
					negated = 0;
				}
			}
			if (!direct && !negated)
			{
				fprintf(stderr, "  rotation mismatch at source time %g\n", s);
				ok = 0;
				break;
			}
		}
		else
		{
			for (c = 0; c < src.comps; ++c)
			{
				if (!close_enough(a[c], b[c]))
				{
					fprintf(stderr, "  mismatch t=%g comp=%d got=%g want=%g\n",
							s, c, b[c], a[c]);
					ok = 0;
					break;
				}
			}
			if (!ok)
			{
				fprintf(stderr, "  (channel node=%zu path=%d)\n", node_index, (int)path);
				break;
			}
		}
	}
	ref_free(&src);
	ref_free(&dst);
	return ok;
}

static int compare_animation(const cgltf_data* source, size_t source_anim,
		const cgltf_data* result, size_t result_anim,
		const float* st, const float* ot, size_t count)
{
	const cgltf_animation* animation = &source->animations[source_anim];
	cgltf_size i;
	int ok = 1;
	for (i = 0; i < animation->channels_count; ++i)
	{
		const cgltf_animation_channel* channel = &animation->channels[i];
		size_t node_index;
		if (!channel->target_node)
		{
			ok = 0;
			continue;
		}
		node_index = cgltf_node_index(source, channel->target_node);
		if (!compare_channel(source, source_anim, node_index, channel->target_path,
				result, result_anim, st, ot, count))
		{
			ok = 0;
		}
	}
	return ok;
}

/* ------------------------------------------------------------------ */
/* matrix / skinning helpers                                           */
/* ------------------------------------------------------------------ */

static void mat4_mul(const float* a, const float* b, float* out)
{
	int r, c;
	for (c = 0; c < 4; ++c)
	{
		for (r = 0; r < 4; ++r)
		{
			double sum = 0.0;
			int k;
			for (k = 0; k < 4; ++k)
			{
				sum += (double)a[k * 4 + r] * (double)b[c * 4 + k];
			}
			out[c * 4 + r] = (float)sum;
		}
	}
}

static void mat4_from_trs(const float* t, const float* r, const float* s, float* m)
{
	double x = r[0], y = r[1], z = r[2], w = r[3];
	double xx = x * x, yy = y * y, zz = z * z;
	double xy = x * y, xz = x * z, yz = y * z;
	double wx = w * x, wy = w * y, wz = w * z;
	m[0] = (float)((1.0 - 2.0 * (yy + zz)) * s[0]);
	m[1] = (float)((2.0 * (xy + wz)) * s[0]);
	m[2] = (float)((2.0 * (xz - wy)) * s[0]);
	m[3] = 0.0f;
	m[4] = (float)((2.0 * (xy - wz)) * s[1]);
	m[5] = (float)((1.0 - 2.0 * (xx + zz)) * s[1]);
	m[6] = (float)((2.0 * (yz + wx)) * s[1]);
	m[7] = 0.0f;
	m[8] = (float)((2.0 * (xz + wy)) * s[2]);
	m[9] = (float)((2.0 * (yz - wx)) * s[2]);
	m[10] = (float)((1.0 - 2.0 * (xx + yy)) * s[2]);
	m[11] = 0.0f;
	m[12] = t[0];
	m[13] = t[1];
	m[14] = t[2];
	m[15] = 1.0f;
}

static void animated_local(const cgltf_data* data, size_t animation_index,
		const cgltf_node* node, double t, float* m)
{
	float translation[3] = {0.0f, 0.0f, 0.0f};
	float rotation[4] = {0.0f, 0.0f, 0.0f, 1.0f};
	float scale[3] = {1.0f, 1.0f, 1.0f};
	const cgltf_animation_channel* channel;
	size_t node_index = cgltf_node_index(data, node);
	int c;

	if (node->has_matrix)
	{
		memcpy(m, node->matrix, sizeof(float) * 16);
		return;
	}
	if (node->has_translation)
	{
		memcpy(translation, node->translation, sizeof(translation));
	}
	if (node->has_rotation)
	{
		memcpy(rotation, node->rotation, sizeof(rotation));
	}
	if (node->has_scale)
	{
		memcpy(scale, node->scale, sizeof(scale));
	}

	channel = find_channel(data, animation_index, node_index,
			cgltf_animation_path_type_translation);
	if (channel)
	{
		ref_chan rc;
		if (ref_init(&rc, channel))
		{
			float value[4];
			ref_eval(&rc, t, value);
			for (c = 0; c < 3; ++c)
			{
				translation[c] = value[c];
			}
			ref_free(&rc);
		}
	}
	channel = find_channel(data, animation_index, node_index,
			cgltf_animation_path_type_rotation);
	if (channel)
	{
		ref_chan rc;
		if (ref_init(&rc, channel))
		{
			float value[4];
			ref_eval(&rc, t, value);
			ref_normalize(value, rotation);
			ref_free(&rc);
		}
	}
	channel = find_channel(data, animation_index, node_index,
			cgltf_animation_path_type_scale);
	if (channel)
	{
		ref_chan rc;
		if (ref_init(&rc, channel))
		{
			float value[3];
			ref_eval(&rc, t, value);
			for (c = 0; c < 3; ++c)
			{
				scale[c] = value[c];
			}
			ref_free(&rc);
		}
	}
	mat4_from_trs(translation, rotation, scale, m);
}

static void animated_world(const cgltf_data* data, size_t animation_index,
		const cgltf_node* node, double t, float* m)
{
	float local[16];
	animated_local(data, animation_index, node, t, local);
	if (node->parent)
	{
		float parent[16];
		animated_world(data, animation_index, node->parent, t, parent);
		mat4_mul(parent, local, m);
	}
	else
	{
		memcpy(m, local, sizeof(local));
	}
}

static void transform_point(const float* m, const float* p, float* out)
{
	double x = p[0], y = p[1], z = p[2], w = 1.0;
	out[0] = (float)(m[0] * x + m[4] * y + m[8] * z + m[12] * w);
	out[1] = (float)(m[1] * x + m[5] * y + m[9] * z + m[13] * w);
	out[2] = (float)(m[2] * x + m[6] * y + m[10] * z + m[14] * w);
	out[3] = (float)(m[3] * x + m[7] * y + m[11] * z + m[15] * w);
}

static void eval_morph_weights(const cgltf_data* data, size_t animation_index,
		const cgltf_node* node, double t, float* out, int targets)
{
	const cgltf_animation_channel* channel =
			find_channel(data, animation_index, cgltf_node_index(data, node),
					cgltf_animation_path_type_weights);
	int i;
	if (channel)
	{
		ref_chan rc;
		if (ref_init(&rc, channel))
		{
			ref_eval(&rc, t, out);
			ref_free(&rc);
			return;
		}
	}
	for (i = 0; i < targets; ++i)
	{
		out[i] = (node->weights && (cgltf_size)i < node->weights_count)
				? node->weights[i] : 0.0f;
	}
}

/* Skinned + morphed position of vertex 0 on the first skinned node. */
static int compute_skinned(const cgltf_data* data, size_t animation_index, double t,
		float out[3])
{
	cgltf_size node_index;
	const cgltf_node* node = NULL;
	const cgltf_mesh* mesh;
	const cgltf_primitive* prim;
	const cgltf_accessor* position_accessor = NULL;
	const cgltf_accessor* joints_accessor = NULL;
	const cgltf_accessor* weights_accessor = NULL;
	cgltf_size targets;
	float weights[8];
	float position[4] = {0.0f, 0.0f, 0.0f, 1.0f};
	float joints[4] = {0.0f, 0.0f, 0.0f, 0.0f};
	float skin_weights[4] = {0.0f, 0.0f, 0.0f, 0.0f};
	float acc[4] = {0.0f, 0.0f, 0.0f, 0.0f};
	cgltf_size m, a;
	int c;

	for (node_index = 0; node_index < data->nodes_count; ++node_index)
	{
		if (data->nodes[node_index].mesh && data->nodes[node_index].skin)
		{
			node = &data->nodes[node_index];
			break;
		}
	}
	if (!node || !node->mesh || !node->mesh->primitives_count)
	{
		return 0;
	}
	mesh = node->mesh;
	prim = &mesh->primitives[0];
	targets = prim->targets_count;
	if (targets > sizeof(weights) / sizeof(weights[0]))
	{
		return 0;
	}

	for (a = 0; a < prim->attributes_count; ++a)
	{
		if (strcmp(prim->attributes[a].name, "POSITION") == 0)
		{
			position_accessor = prim->attributes[a].data;
		}
		else if (strcmp(prim->attributes[a].name, "JOINTS_0") == 0)
		{
			joints_accessor = prim->attributes[a].data;
		}
		else if (strcmp(prim->attributes[a].name, "WEIGHTS_0") == 0)
		{
			weights_accessor = prim->attributes[a].data;
		}
	}
	if (!position_accessor || !joints_accessor || !weights_accessor)
	{
		return 0;
	}
	if (!cgltf_accessor_read_float(position_accessor, 0, position, 3) ||
			!cgltf_accessor_read_float(joints_accessor, 0, joints, 4) ||
			!cgltf_accessor_read_float(weights_accessor, 0, skin_weights, 4))
	{
		return 0;
	}

	eval_morph_weights(data, animation_index, node, t, weights, (int)targets);
	for (m = 0; m < targets; ++m)
	{
		const cgltf_accessor* delta_accessor = NULL;
		float delta[3] = {0.0f, 0.0f, 0.0f};
		for (a = 0; a < prim->targets[m].attributes_count; ++a)
		{
			if (strcmp(prim->targets[m].attributes[a].name, "POSITION") == 0)
			{
				delta_accessor = prim->targets[m].attributes[a].data;
			}
		}
		if (delta_accessor && cgltf_accessor_read_float(delta_accessor, 0, delta, 3))
		{
			for (c = 0; c < 3; ++c)
			{
				position[c] += weights[m] * delta[c];
			}
		}
	}

	for (c = 0; c < 4; ++c)
	{
		int joint_index = (int)joints[c];
		float joint_matrix[16];
		float ibm[16];
		float p1[4], p2[4];
		cgltf_size k;
		if (skin_weights[c] == 0.0f)
		{
			continue;
		}
		if ((cgltf_size)joint_index >= node->skin->joints_count ||
				!node->skin->inverse_bind_matrices)
		{
			return 0;
		}
		animated_world(data, animation_index, node->skin->joints[joint_index], t, joint_matrix);
		if (!cgltf_accessor_read_float(node->skin->inverse_bind_matrices,
				(cgltf_size)joint_index, ibm, 16))
		{
			return 0;
		}
		transform_point(ibm, position, p1);
		transform_point(joint_matrix, p1, p2);
		for (k = 0; k < 4; ++k)
		{
			acc[k] += skin_weights[c] * p2[k];
		}
	}
	out[0] = acc[0];
	out[1] = acc[1];
	out[2] = acc[2];
	return 1;
}

/* ------------------------------------------------------------------ */
/* cubic group                                                         */
/* ------------------------------------------------------------------ */

/*
 * Non-equidistant cubic keys on translation/scale/rotation/weights.  The map
 * knots are 0/1/2/3 with a different slope on each side.  Translation starts
 * after the span starts, scale and weights end before the span ends, so their
 * constant extensions are exercised.  The cubic rotation keys include every
 * map knot inside its open interval, satisfying the insertion restriction.
 */
static cgltf_data* build_cubic_asset(void)
{
	builder b;
	cgltf_data* data;
	float trans_t[4] = {0.5f, 1.2f, 2.2f, 3.0f};
	float scale_t[4] = {0.0f, 1.0f, 2.0f, 2.7f};
	float rot_t[4] = {0.5f, 1.0f, 2.0f, 3.0f};
	float weight_t[4] = {0.0f, 0.8f, 1.7f, 2.9f};
	float trans_blocks[4 * 9] = {
		0.1f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.5f, 1.0f, 0.0f,
		0.2f, -0.5f, 0.0f, 2.0f, 1.0f, 0.0f, -0.4f, 0.3f, 0.2f,
		0.6f, 0.2f, -0.1f, 1.0f, 2.0f, 1.0f, 0.0f, -0.6f, 0.4f,
		-0.3f, 0.4f, 0.1f, 3.0f, 0.5f, 2.0f, 0.2f, -0.2f, 0.0f,
	};
	float scale_blocks[4 * 9] = {
		0.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f, 0.2f, 0.0f, -0.1f,
		0.1f, 0.0f, 0.2f, 1.5f, 0.8f, 1.2f, -0.2f, 0.3f, 0.1f,
		-0.4f, 0.1f, 0.0f, 1.2f, 1.4f, 0.9f, 0.3f, -0.2f, 0.5f,
		0.0f, 0.2f, -0.3f, 0.8f, 1.0f, 1.1f, 0.0f, 0.0f, 0.0f,
	};
	/* Values are unit quaternions; tangents are free components. */
	float rot_blocks[4 * 12] = {
		0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.3f, 0.1f, 0.0f, 0.0f,
		0.1f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.70710678f, 0.70710678f, -0.2f, 0.0f, 0.1f, 0.0f,
		0.0f, 0.2f, 0.0f, 0.0f, 0.0f, 0.0f, 0.70710678f, 0.70710678f, 0.0f, -0.1f, 0.0f, 0.1f,
		0.0f, 0.0f, 0.0f, 0.1f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f,
	};
	float weight_blocks[4 * 6] = {
		0.0f, 0.0f, 0.0f, 0.0f, 0.2f, -0.1f,
		0.1f, 0.0f, 0.5f, 0.2f, 0.0f, 0.2f,
		0.0f, 0.1f, 1.0f, 0.5f, -0.1f, 0.0f,
		0.0f, 0.0f, 0.0f, 0.8f, 0.0f, 0.0f,
	};
	float positions[9] = {
		1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f,
	};
	float morph_a[9] = {
		0.1f, 0.0f, 0.0f, 0.0f, 0.1f, 0.0f, 0.0f, 0.0f, 0.1f,
	};
	float morph_b[9] = {
		0.0f, 0.2f, 0.0f, 0.2f, 0.0f, 0.0f, 0.0f, 0.0f, 0.2f,
	};
	int trans_in, trans_out, scale_in, scale_out, rot_in, rot_out;
	int weight_in, weight_out, position, target_a, target_b;

	b_init(&b);
	trans_in = b_times(&b, trans_t, 4);
	trans_out = b_cubic_vec3(&b, trans_blocks, 4);
	scale_in = b_times(&b, scale_t, 4);
	scale_out = b_cubic_vec3(&b, scale_blocks, 4);
	rot_in = b_times(&b, rot_t, 4);
	rot_out = b_cubic_vec4(&b, rot_blocks, 4);
	weight_in = b_times(&b, weight_t, 4);
	weight_out = b_cubic_weights(&b, weight_blocks, 4, 2);
	position = b_vec3s(&b, positions, 3);
	target_a = b_vec3s(&b, morph_a, 3);
	target_b = b_vec3s(&b, morph_b, 3);

	sb_appendf(&b.extra, &b.extra_len, &b.extra_cap,
			",\"nodes\":["
			"{\"mesh\":0},"
			"{\"translation\":[0,0,0]},"
			"{\"translation\":[0,0,0]},"
			"{\"rotation\":[0,0,0,1]}],"
			"\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":%d},"
			"\"targets\":[{\"POSITION\":%d},{\"POSITION\":%d}]}]}],"
			"\"scenes\":[{\"nodes\":[0,1,2,3]}],"
			"\"animations\":[{\"samplers\":["
			"{\"input\":%d,\"output\":%d,\"interpolation\":\"CUBICSPLINE\"},"
			"{\"input\":%d,\"output\":%d,\"interpolation\":\"CUBICSPLINE\"},"
			"{\"input\":%d,\"output\":%d,\"interpolation\":\"CUBICSPLINE\"},"
			"{\"input\":%d,\"output\":%d,\"interpolation\":\"CUBICSPLINE\"}],"
			"\"channels\":["
			"{\"sampler\":0,\"target\":{\"node\":1,\"path\":\"translation\"}},"
			"{\"sampler\":1,\"target\":{\"node\":2,\"path\":\"scale\"}},"
			"{\"sampler\":2,\"target\":{\"node\":3,\"path\":\"rotation\"}},"
			"{\"sampler\":3,\"target\":{\"node\":0,\"path\":\"weights\"}}]}]",
			position, target_a, target_b,
			trans_in, trans_out, scale_in, scale_out,
			rot_in, rot_out, weight_in, weight_out);

	data = build_from_extra(&b);
	b_destroy(&b);
	return data;
}

static void cubic_map(float* st, float* ot)
{
	st[0] = 0.0f; st[1] = 1.0f; st[2] = 2.0f; st[3] = 3.0f;
	ot[0] = 0.0f; ot[1] = 0.4f; ot[2] = 1.3f; ot[3] = 2.0f;
}

/* Snapshot the time/value bytes of every animation channel in an animation. */
typedef struct source_digest {
	cgltf_size samplers;
	cgltf_size channels;
	cgltf_accessor* input[8];
	cgltf_accessor* output[8];
	float input_data[8][8];
	float output_data[8][12];
} source_digest;

static int digest_animation(const cgltf_data* data, size_t animation_index,
		source_digest* digest)
{
	const cgltf_animation* animation;
	cgltf_size i;
	memset(digest, 0, sizeof(*digest));
	if (animation_index >= data->animations_count)
	{
		return 0;
	}
	animation = &data->animations[animation_index];
	digest->samplers = animation->samplers_count;
	digest->channels = animation->channels_count;
	if (animation->samplers_count > 8)
	{
		return 0;
	}
	for (i = 0; i < animation->samplers_count; ++i)
	{
		cgltf_size k, n;
		const cgltf_accessor* in = animation->samplers[i].input;
		const cgltf_accessor* out = animation->samplers[i].output;
		digest->input[i] = animation->samplers[i].input;
		digest->output[i] = animation->samplers[i].output;
		n = in->count < 8 ? in->count : 8;
		for (k = 0; k < n; ++k)
		{
			if (!cgltf_accessor_read_float(in, k, &digest->input_data[i][k], 1))
			{
				return 0;
			}
		}
		/* Compare the first output element in full; enough to detect writes. */
		if (out->count > 0 &&
				!cgltf_accessor_read_float(out, 0, digest->output_data[i],
						cgltf_num_components(out->type)))
		{
			return 0;
		}
	}
	return 1;
}

static int digest_equal(const source_digest* a, const source_digest* b)
{
	cgltf_size i;
	if (a->samplers != b->samplers || a->channels != b->channels)
	{
		return 0;
	}
	for (i = 0; i < a->samplers; ++i)
	{
		if (a->input[i] != b->input[i] || a->output[i] != b->output[i])
		{
			return 0;
		}
		if (memcmp(a->input_data[i], b->input_data[i], sizeof(a->input_data[i])) != 0 ||
				memcmp(a->output_data[i], b->output_data[i], sizeof(a->output_data[i])) != 0)
		{
			return 0;
		}
	}
	return 1;
}

static int case_cubic_trajectory(void)
{
	cgltf_data* source = build_cubic_asset();
	cgltf_data* result = NULL;
	source_digest before, after;
	float st[4], ot[4];
	cgltf_result status;

	TASK_CHECK(source != NULL);
	TASK_CHECK(cgltf_validate(source) == cgltf_result_success);
	TASK_CHECK(digest_animation(source, 0, &before));
	cubic_map(st, ot);
	status = cgltf_retime_animation(source, 0, st, ot, 4, &result);
	TASK_CHECK(status == cgltf_result_success);
	TASK_CHECK(result != NULL);
	TASK_CHECK(cgltf_validate(result) == cgltf_result_success);
	/* The source asset is left untouched. */
	TASK_CHECK(digest_animation(source, 0, &after));
	TASK_CHECK(digest_equal(&before, &after));
	TASK_CHECK(compare_animation(source, 0, result, 0, st, ot, 4));

	/* Not baked at a fixed rate: source keys plus map knots only. */
	{
		const cgltf_animation_channel* c;
		c = find_channel(result, 0, 1, cgltf_animation_path_type_translation);
		TASK_CHECK(c && c->sampler->input->count == 7);
		c = find_channel(result, 0, 2, cgltf_animation_path_type_scale);
		TASK_CHECK(c && c->sampler->input->count == 5);
		c = find_channel(result, 0, 3, cgltf_animation_path_type_rotation);
		TASK_CHECK(c && c->sampler->input->count == 5);
		c = find_channel(result, 0, 0, cgltf_animation_path_type_weights);
		TASK_CHECK(c && c->sampler->input->count == 7);
	}

	/* Weights stay a flattened SCALAR accessor: keys * targets elements. */
	{
		const cgltf_animation_channel* c =
				find_channel(result, 0, 0, cgltf_animation_path_type_weights);
		TASK_CHECK(c != NULL);
		TASK_CHECK(c->sampler->output->type == cgltf_type_scalar);
		/* 7 keys * 2 targets * 3 cubic blocks */
		TASK_CHECK(c->sampler->output->count == 7 * 2 * 3);
		TASK_CHECK(c->sampler->interpolation == cgltf_interpolation_type_cubic_spline);
	}

	/* Interpolation types are preserved. */
	TASK_CHECK(find_channel(result, 0, 3, cgltf_animation_path_type_rotation)->
			sampler->interpolation == cgltf_interpolation_type_cubic_spline);

	/* Pre-existing keys inside the span keep their mapped times. */
	{
		static const float trans_expected[7] = {
			0.0f, 0.2f, 0.4f, 0.58f, 1.3f, 1.44f, 2.0f
		};
		static const float scale_expected[5] = {
			0.0f, 0.4f, 1.3f, 1.79f, 2.0f
		};
		const cgltf_animation_channel* c;
		cgltf_size k;
		c = find_channel(result, 0, 1, cgltf_animation_path_type_translation);
		TASK_CHECK(c != NULL);
		for (k = 0; k < 7; ++k)
		{
			cgltf_float t;
			TASK_CHECK(cgltf_accessor_read_float(c->sampler->input, k, &t, 1));
			TASK_CHECK(close_enough(t, trans_expected[k]));
		}
		c = find_channel(result, 0, 2, cgltf_animation_path_type_scale);
		TASK_CHECK(c != NULL);
		for (k = 0; k < 5; ++k)
		{
			cgltf_float t;
			TASK_CHECK(cgltf_accessor_read_float(c->sampler->input, k, &t, 1));
			TASK_CHECK(close_enough(t, scale_expected[k]));
		}
	}

	cgltf_free(source);
	cgltf_free(result);
	return 0;
}

static int case_cubic_split_tangents(void)
{
	cgltf_data* source = build_cubic_asset();
	cgltf_data* result = NULL;
	float st[4], ot[4];
	cgltf_result status;
	const cgltf_animation_channel* src_rot;
	const cgltf_animation_channel* dst_rot;
	float src_in[2][4], src_out[2][4];
	float dst_in[2][4], dst_out[2][4];
	int c;

	TASK_CHECK(source != NULL);
	cubic_map(st, ot);
	status = cgltf_retime_animation(source, 0, st, ot, 4, &result);
	TASK_CHECK(status == cgltf_result_success);
	TASK_CHECK(result != NULL);

	src_rot = find_channel(source, 0, 3, cgltf_animation_path_type_rotation);
	dst_rot = find_channel(result, 0, 3, cgltf_animation_path_type_rotation);
	TASK_CHECK(src_rot && dst_rot);
	TASK_CHECK(src_rot->sampler->input->count == 4);
	TASK_CHECK(dst_rot->sampler->input->count == 5);

	/* Source keys 1.0 (index 1) and 2.0 (index 2) are interior map breaks. */
	for (c = 0; c < 2; ++c)
	{
		TASK_CHECK(cgltf_accessor_read_float(src_rot->sampler->output,
				(cgltf_size)(1 + c) * 3, src_in[c], 4));
		TASK_CHECK(cgltf_accessor_read_float(src_rot->sampler->output,
				(cgltf_size)(1 + c) * 3 + 2, src_out[c], 4));
	}
	/* Result keys are 0, 0.5, 1.0, 2.0, 3.0: indices 2 and 3. */
	TASK_CHECK(cgltf_accessor_read_float(dst_rot->sampler->output, 2 * 3, dst_in[0], 4));
	TASK_CHECK(cgltf_accessor_read_float(dst_rot->sampler->output, 2 * 3 + 2, dst_out[0], 4));
	TASK_CHECK(cgltf_accessor_read_float(dst_rot->sampler->output, 3 * 3, dst_in[1], 4));
	TASK_CHECK(cgltf_accessor_read_float(dst_rot->sampler->output, 3 * 3 + 2, dst_out[1], 4));

	{
		/* Segment dt_source/dt_output: [0,1] -> 2.5, [1,2] -> 1/0.9,
		 * [2,3] -> 1/0.7. */
		const double left1 = 2.5, right1 = 1.0 / 0.9;
		const double left2 = 1.0 / 0.9, right2 = 1.0 / 0.7;
		for (c = 0; c < 4; ++c)
		{
			TASK_CHECK(close_enough(dst_in[0][c], src_in[0][c] * left1));
			TASK_CHECK(close_enough(dst_out[0][c], src_out[0][c] * right1));
			TASK_CHECK(close_enough(dst_in[1][c], src_in[1][c] * left2));
			TASK_CHECK(close_enough(dst_out[1][c], src_out[1][c] * right2));
		}
	}

	/* Scale keys at source 1.0 and 2.0 also sit on map breaks; their cubic
	 * in/out tangents must be re-expressed separately per side.  Source keys
	 * 0, 1, 2, 2.7 map to 0, 0.4, 1.3, 1.79, with the knots 0, 1, 2, 3. */
	{
		const cgltf_animation_channel* src_scale =
				find_channel(source, 0, 2, cgltf_animation_path_type_scale);
		const cgltf_animation_channel* dst_scale =
				find_channel(result, 0, 2, cgltf_animation_path_type_scale);
		float s_in[2][3], s_out[2][3], d_in[2][3], d_out[2][3];
		const double left1 = 2.5, right1 = 1.0 / 0.9;
		const double left2 = 1.0 / 0.9, right2 = 1.0 / 0.7;
		TASK_CHECK(src_scale && dst_scale);
		TASK_CHECK(cgltf_accessor_read_float(src_scale->sampler->output, 1 * 3, s_in[0], 3));
		TASK_CHECK(cgltf_accessor_read_float(src_scale->sampler->output, 1 * 3 + 2, s_out[0], 3));
		TASK_CHECK(cgltf_accessor_read_float(src_scale->sampler->output, 2 * 3, s_in[1], 3));
		TASK_CHECK(cgltf_accessor_read_float(src_scale->sampler->output, 2 * 3 + 2, s_out[1], 3));
		TASK_CHECK(cgltf_accessor_read_float(dst_scale->sampler->output, 1 * 3, d_in[0], 3));
		TASK_CHECK(cgltf_accessor_read_float(dst_scale->sampler->output, 1 * 3 + 2, d_out[0], 3));
		TASK_CHECK(cgltf_accessor_read_float(dst_scale->sampler->output, 2 * 3, d_in[1], 3));
		TASK_CHECK(cgltf_accessor_read_float(dst_scale->sampler->output, 2 * 3 + 2, d_out[1], 3));
		for (c = 0; c < 3; ++c)
		{
			TASK_CHECK(close_enough(d_in[0][c], s_in[0][c] * left1));
			TASK_CHECK(close_enough(d_out[0][c], s_out[0][c] * right1));
			TASK_CHECK(close_enough(d_in[1][c], s_in[1][c] * left2));
			TASK_CHECK(close_enough(d_out[1][c], s_out[1][c] * right2));
		}
	}

	/* Every stored rotation value stays at unit length. */
	{
		cgltf_size key;
		for (key = 0; key < dst_rot->sampler->input->count; ++key)
		{
			float q[4];
			double len;
			TASK_CHECK(cgltf_accessor_read_float(dst_rot->sampler->output,
					key * 3 + 1, q, 4));
			len = sqrt((double)q[0] * q[0] + (double)q[1] * q[1] +
					(double)q[2] * q[2] + (double)q[3] * q[3]);
			TASK_CHECK(fabs(len - 1.0) < 1e-5);
		}
	}

	cgltf_free(source);
	cgltf_free(result);
	return 0;
}

/*
 * Strided input with padding and a sparse output override.  min/max on the
 * input are intentionally wrong.
 */
static cgltf_data* build_sparse_asset(void)
{
	builder b;
	cgltf_data* data;
	/* With offset 1 float and stride 2, the real keys are 0, 1, 2. */
	float times[6] = {0.0f, 7.0f, 1.0f, 7.0f, 2.0f, 7.0f};
	float base[9] = {
		0.0f, 0.0f, 0.0f,
		2.0f, 0.0f, 0.0f,
		4.0f, 0.0f, 0.0f,
	};
	float sparse_value[3] = {9.0f, 9.0f, 9.0f};
	uint32_t sparse_index = 1;
	int in_acc, out_acc, base_view, idx_view, val_view;
	char extra[400];

	b_init(&b);
	in_acc = b_accessor_f32(&b, times, 3, 2, 1, "SCALAR",
			",\"min\":[-5],\"max\":[5]");
	base_view = b_view(&b, b.bin_len, 3 * 3 * sizeof(float), 0);
	b_bin_floats(&b, base, 9);
	idx_view = b_view(&b, b.bin_len, sizeof(uint32_t), 0);
	b_bin_u32(&b, &sparse_index, 1);
	val_view = b_view(&b, b.bin_len, 3 * sizeof(float), 0);
	b_bin_floats(&b, sparse_value, 3);
	snprintf(extra, sizeof(extra),
			",\"sparse\":{\"count\":1,\"indices\":{\"bufferView\":%d,"
			"\"byteOffset\":0,\"componentType\":5125},"
			"\"values\":{\"bufferView\":%d,\"byteOffset\":0}}",
			idx_view, val_view);
	out_acc = b_accessor_raw(&b, base_view, 0, 3, "VEC3", extra);

	sb_appendf(&b.extra, &b.extra_len, &b.extra_cap,
			",\"nodes\":[{\"translation\":[0,0,0]},{\"translation\":[0,0,0]}],"
			"\"scenes\":[{\"nodes\":[0,1]}],"
			"\"animations\":[{\"samplers\":[{\"input\":%d,\"output\":%d,"
			"\"interpolation\":\"LINEAR\"}],\"channels\":["
			"{\"sampler\":0,\"target\":{\"node\":1,\"path\":\"translation\"}}]}]",
			in_acc, out_acc);

	data = build_from_extra(&b);
	b_destroy(&b);
	return data;
}

static int case_cubic_sparse_stride(void)
{
	cgltf_data* source = build_sparse_asset();
	cgltf_data* result = NULL;
	float st[3] = {0.0f, 1.0f, 2.0f};
	float ot[3] = {0.0f, 0.4f, 1.0f};
	cgltf_result status;
	const cgltf_animation_channel* dst_ch;
	cgltf_float t;
	float value[3];

	TASK_CHECK(source != NULL);
	TASK_CHECK(source->accessors[0].stride == 2 * sizeof(float));
	TASK_CHECK(cgltf_validate(source) == cgltf_result_success);
	status = cgltf_retime_animation(source, 0, st, ot, 3, &result);
	TASK_CHECK(status == cgltf_result_success);
	TASK_CHECK(result != NULL);
	TASK_CHECK(compare_animation(source, 0, result, 0, st, ot, 3));

	dst_ch = find_channel(result, 0, 1, cgltf_animation_path_type_translation);
	TASK_CHECK(dst_ch != NULL);
	TASK_CHECK(cgltf_accessor_read_float(dst_ch->sampler->input, 1, &t, 1));
	TASK_CHECK(fabs((double)t - 0.4) < 1e-6);
	/* Sparse value 9 at source key 1 survives at the mapped key. */
	TASK_CHECK(cgltf_accessor_read_float(dst_ch->sampler->output, 1, value, 3));
	TASK_CHECK(fabs(value[0] - 9.0f) < 1e-5);
	/* The declared min/max [-5,5] were not used; the real last key is 2. */
	TASK_CHECK(cgltf_accessor_read_float(dst_ch->sampler->input, 2, &t, 1));
	TASK_CHECK(fabs((double)t - 1.0) < 1e-6);

	cgltf_free(source);
	cgltf_free(result);
	return 0;
}

/* ------------------------------------------------------------------ */
/* rotation_step group                                                 */
/* ------------------------------------------------------------------ */

/* Two LINEAR rotation channels (second with flipped signs) plus STEP data. */
static cgltf_data* build_linear_rotation_asset(float sign)
{
	builder b;
	cgltf_data* data;
	float times[2] = {0.0f, 2.0f};
	float rotation[8] = {
		0.0f, 0.0f, 0.0f, 1.0f,
		0.0f, 0.0f, 0.86602540f * sign, 0.5f * sign,
	};
	float other_times[2] = {0.0f, 2.0f};
	float other_values[6] = {0.0f, 0.0f, 0.0f, 1.0f, 1.0f, 0.0f};
	int in_acc, rot_acc, other_in, other_acc;

	b_init(&b);
	in_acc = b_times(&b, times, 2);
	rot_acc = b_vec4s(&b, rotation, 2);
	other_in = b_times(&b, other_times, 2);
	other_acc = b_vec3s(&b, other_values, 2);

	sb_appendf(&b.extra, &b.extra_len, &b.extra_cap,
			",\"nodes\":[{\"rotation\":[0,0,0,1]},{\"translation\":[0,0,0]}],"
			"\"scenes\":[{\"nodes\":[0,1]}],"
			"\"animations\":[{\"samplers\":["
			"{\"input\":%d,\"output\":%d,\"interpolation\":\"LINEAR\"},"
			"{\"input\":%d,\"output\":%d,\"interpolation\":\"LINEAR\"}],"
			"\"channels\":["
			"{\"sampler\":0,\"target\":{\"node\":0,\"path\":\"rotation\"}},"
			"{\"sampler\":1,\"target\":{\"node\":1,\"path\":\"translation\"}}]}]",
			in_acc, rot_acc, other_in, other_acc);

	data = build_from_extra(&b);
	b_destroy(&b);
	return data;
}

static int rotation_midpoint_case(float sign)
{
	cgltf_data* source = build_linear_rotation_asset(sign);
	cgltf_data* result = NULL;
	float st[3] = {0.0f, 1.0f, 2.0f};
	float ot[3] = {0.0f, 0.5f, 1.0f};
	cgltf_result status;
	const cgltf_animation_channel* dst;
	float q[4];
	float expected[4];
	float a[4] = {0.0f, 0.0f, 0.0f, 1.0f};
	float b[4] = {0.0f, 0.0f, 0.86602540f, 0.5f};
	int c;

	TASK_CHECK(source != NULL);
	status = cgltf_retime_animation(source, 0, st, ot, 3, &result);
	TASK_CHECK(status == cgltf_result_success);
	TASK_CHECK(result != NULL);
	TASK_CHECK(cgltf_validate(result) == cgltf_result_success);
	TASK_CHECK(compare_animation(source, 0, result, 0, st, ot, 3));

	dst = find_channel(result, 0, 0, cgltf_animation_path_type_rotation);
	TASK_CHECK(dst != NULL);
	TASK_CHECK(dst->sampler->input->count == 3);
	/* The inserted key sits at output 0.5, i.e. source 1.0. */
	{
		cgltf_float t;
		TASK_CHECK(cgltf_accessor_read_float(dst->sampler->input, 1, &t, 1));
		TASK_CHECK(fabs((double)t - 0.5) < 1e-6);
	}
	ref_slerp(a, b, 0.5, expected);
	TASK_CHECK(cgltf_accessor_read_float(dst->sampler->output, 1, q, 4));
	{
		int direct = 1, negated = 1;
		double len;
		for (c = 0; c < 4; ++c)
		{
			if (!close_enough(q[c], expected[c]))
			{
				direct = 0;
			}
			if (!close_enough(q[c], -expected[c]))
			{
				negated = 0;
			}
		}
		TASK_CHECK(direct || negated);
		len = sqrt((double)q[0] * q[0] + (double)q[1] * q[1] +
				(double)q[2] * q[2] + (double)q[3] * q[3]);
		TASK_CHECK(fabs(len - 1.0) < 1e-5);
	}
	cgltf_free(source);
	cgltf_free(result);
	return 0;
}

static int case_rotation_shortest_arc(void)
{
	TASK_CHECK(rotation_midpoint_case(1.0f) == 0);
	return rotation_midpoint_case(-1.0f);
}

/* Shared LINEAR sampler between translation and scale, plus a STEP channel. */
static cgltf_data* build_rotation_asset(void)
{
	builder b;
	cgltf_data* data;
	float shared_t[4] = {0.0f, 1.0f, 2.0f, 2.6f};
	float shared[12] = {
		0.0f, 0.0f, 0.0f,
		1.0f, 0.0f, 0.0f,
		2.0f, 1.0f, 0.0f,
		3.0f, 1.0f, 0.0f,
	};
	float scale[12] = {
		1.0f, 1.0f, 1.0f,
		2.0f, 0.5f, 1.0f,
		1.0f, 2.0f, 0.5f,
		2.0f, 2.0f, 2.0f,
	};
	float rot_t[4] = {0.0f, 1.0f, 2.0f, 3.0f};
	float rot[16] = {
		0.0f, 0.0f, 0.0f, 1.0f,
		0.0f, 0.0f, 0.0f, -1.0f,
		0.0f, 0.0f, 0.70710678f, 0.70710678f,
		0.5f, 0.5f, 0.5f, 0.5f,
	};
	float step_t[4] = {0.4f, 1.0f, 2.0f, 3.0f};
	float step[12] = {
		0.0f, 0.0f, 0.0f,
		1.0f, 2.0f, 0.0f,
		0.0f, 1.0f, 3.0f,
		4.0f, 0.0f, 0.0f,
	};
	int shared_in, shared_out, scale_out, rot_in, rot_out, step_in, step_out;

	b_init(&b);
	shared_in = b_times(&b, shared_t, 4);
	shared_out = b_vec3s(&b, shared, 4);
	scale_out = b_vec3s(&b, scale, 4);
	rot_in = b_times(&b, rot_t, 4);
	rot_out = b_vec4s(&b, rot, 4);
	step_in = b_times(&b, step_t, 4);
	step_out = b_vec3s(&b, step, 4);

	sb_appendf(&b.extra, &b.extra_len, &b.extra_cap,
			",\"nodes\":[{\"translation\":[0,0,0]},{\"rotation\":[0,0,0,1]},"
			"{\"translation\":[0,0,0]}],"
			"\"scenes\":[{\"nodes\":[0,1,2]}],"
			"\"animations\":[{\"samplers\":["
			"{\"input\":%d,\"output\":%d,\"interpolation\":\"LINEAR\"},"
			"{\"input\":%d,\"output\":%d,\"interpolation\":\"LINEAR\"},"
			"{\"input\":%d,\"output\":%d,\"interpolation\":\"LINEAR\"},"
			"{\"input\":%d,\"output\":%d,\"interpolation\":\"STEP\"}],"
			"\"channels\":["
			"{\"sampler\":0,\"target\":{\"node\":0,\"path\":\"translation\"}},"
			"{\"sampler\":0,\"target\":{\"node\":2,\"path\":\"translation\"}},"
			"{\"sampler\":1,\"target\":{\"node\":0,\"path\":\"scale\"}},"
			"{\"sampler\":2,\"target\":{\"node\":1,\"path\":\"rotation\"}},"
			"{\"sampler\":3,\"target\":{\"node\":2,\"path\":\"scale\"}}]}]",
			shared_in, shared_out, shared_in, scale_out,
			rot_in, rot_out, step_in, step_out);

	data = build_from_extra(&b);
	b_destroy(&b);
	return data;
}

static int case_rotation_trimmed_span(void)
{
	/* Map a sub-span [1.0, 3.0] to output [0.0, 1.0]; output_times[0] is 0
	 * while source_times[0] is not. */
	cgltf_data* source = build_rotation_asset();
	cgltf_data* result = NULL;
	float st[3] = {1.0f, 1.5f, 3.0f};
	float ot[3] = {0.0f, 0.2f, 1.0f};
	cgltf_result status;
	const cgltf_animation_channel* trans;
	const cgltf_animation_channel* scale;

	TASK_CHECK(source != NULL);
	status = cgltf_retime_animation(source, 0, st, ot, 3, &result);
	TASK_CHECK(status == cgltf_result_success);
	TASK_CHECK(result != NULL);
	TASK_CHECK(cgltf_validate(result) == cgltf_result_success);
	TASK_CHECK(compare_animation(source, 0, result, 0, st, ot, 3));

	/* The first output key is 0 and the trajectory starts at source 1.0. */
	trans = find_channel(result, 0, 0, cgltf_animation_path_type_translation);
	scale = find_channel(result, 0, 0, cgltf_animation_path_type_scale);
	TASK_CHECK(trans && scale);
	{
		cgltf_float t;
		float v[3];
		TASK_CHECK(cgltf_accessor_read_float(trans->sampler->input, 0, &t, 1));
		TASK_CHECK(t == 0.0f);
		TASK_CHECK(cgltf_accessor_read_float(trans->sampler->output, 0, v, 3));
		TASK_CHECK(close_enough(v[0], 1.0)); /* source value at source 1.0 */

		/* Source keys 1.0, 2.0 and 2.6 plus map knots 1.0, 1.5 and 3.0. */
		TASK_CHECK(trans->sampler->input->count == 5);
		TASK_CHECK(scale->sampler->input->count == 5);
	}

	cgltf_free(source);
	cgltf_free(result);
	return 0;
}

static int case_rotation_shared_sampler(void)
{
	cgltf_data* source = build_rotation_asset();
	cgltf_data* result = NULL;
	float st[4] = {0.0f, 0.6f, 1.5f, 3.0f};
	float ot[4] = {0.0f, 0.2f, 0.8f, 1.2f};
	cgltf_result status;
	const cgltf_animation_channel* dst_rot;
	cgltf_size i;

	TASK_CHECK(source != NULL);
	TASK_CHECK(cgltf_validate(source) == cgltf_result_success);
	/* The source shares one sampler between two translation channels. */
	TASK_CHECK(source->animations[0].channels_count == 5);
	TASK_CHECK(source->animations[0].samplers_count == 4);
	status = cgltf_retime_animation(source, 0, st, ot, 4, &result);
	TASK_CHECK(status == cgltf_result_success);
	TASK_CHECK(result != NULL);
	TASK_CHECK(cgltf_validate(result) == cgltf_result_success);
	/* Shared samplers may be split; channel targets stay the same. */
	TASK_CHECK(result->animations[0].channels_count == 5);
	TASK_CHECK(result->animations[0].samplers_count == 5);
	TASK_CHECK(compare_animation(source, 0, result, 0, st, ot, 4));

	dst_rot = find_channel(result, 0, 1, cgltf_animation_path_type_rotation);
	TASK_CHECK(dst_rot != NULL);
	TASK_CHECK(dst_rot->sampler->interpolation == cgltf_interpolation_type_linear);
	for (i = 0; i < dst_rot->sampler->input->count; ++i)
	{
		float q[4];
		double len;
		TASK_CHECK(cgltf_accessor_read_float(dst_rot->sampler->output, i, q, 4));
		len = sqrt((double)q[0] * q[0] + (double)q[1] * q[1] +
				(double)q[2] * q[2] + (double)q[3] * q[3]);
		TASK_CHECK(fabs(len - 1.0) < 1e-5);
	}

	/* The sign-flipped pair denotes the same orientation; the inserted key
	 * at source 0.6 must be +-identity, not a half turn. */
	{
		cgltf_float t;
		float q[4];
		TASK_CHECK(cgltf_accessor_read_float(dst_rot->sampler->input, 1, &t, 1));
		TASK_CHECK(fabs((double)t - 0.2) < 1e-5);
		TASK_CHECK(cgltf_accessor_read_float(dst_rot->sampler->output, 1, q, 4));
		TASK_CHECK(fabs(q[0]) < 1e-4 && fabs(q[1]) < 1e-4 &&
				fabs(q[2]) < 1e-4 && fabs(fabs((double)q[3]) - 1.0) < 1e-4);
	}

	/* STEP interpolation is preserved. */
	{
		const cgltf_animation_channel* c =
				find_channel(result, 0, 2, cgltf_animation_path_type_scale);
		TASK_CHECK(c && c->sampler->interpolation == cgltf_interpolation_type_step);
	}

	cgltf_free(source);
	cgltf_free(result);
	return 0;
}

/* ------------------------------------------------------------------ */
/* asset group                                                         */
/* ------------------------------------------------------------------ */

static cgltf_data* build_skin_asset(void)
{
	builder b;
	cgltf_data* data;
	float times[3] = {0.0f, 1.0f, 2.0f};
	float joint0_keys[9] = {
		1.0f, 0.0f, 0.0f,
		2.0f, 3.0f, 0.0f,
		0.0f, 1.0f, 0.0f,
	};
	float weights_keys[6] = {
		0.0f, 0.0f,
		1.0f, 0.0f,
		0.25f, 0.75f,
	};
	float positions[9] = {
		1.0f, 0.0f, 0.0f,
		0.0f, 1.0f, 0.0f,
		0.0f, 0.0f, 1.0f,
	};
	float morph_a[9] = {
		0.1f, 0.0f, 0.0f,
		0.0f, 0.2f, 0.0f,
		0.0f, 0.0f, 0.3f,
	};
	float morph_b[9] = {
		0.0f, 0.1f, 0.0f,
		0.1f, 0.0f, 0.0f,
		0.0f, 0.0f, 0.2f,
	};
	float joints[12] = {
		0.0f, 1.0f, 0.0f, 0.0f,
		0.0f, 1.0f, 0.0f, 0.0f,
		1.0f, 0.0f, 0.0f, 0.0f,
	};
	float skin_weights[12] = {
		0.5f, 0.5f, 0.0f, 0.0f,
		0.5f, 0.5f, 0.0f, 0.0f,
		1.0f, 0.0f, 0.0f, 0.0f,
	};
	float ibm[32] = {
		1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, -1.0f, 0, 0, 1,
		1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, -1.0f, 0, 1,
	};
	float other_times[2] = {0.0f, 2.0f};
	float other_values[6] = {
		0.0f, 1.0f, 0.0f,
		0.0f, 4.0f, 0.0f,
	};
	int pos, morph_ai, morph_bi, joints_acc, sw_acc, ibm_acc;
	int in_acc, trans_out, w_out, other_in, other_out;

	b_init(&b);
	pos = b_vec3s(&b, positions, 3);
	morph_ai = b_vec3s(&b, morph_a, 3);
	morph_bi = b_vec3s(&b, morph_b, 3);
	joints_acc = b_vec4s(&b, joints, 3);
	sw_acc = b_vec4s(&b, skin_weights, 3);
	ibm_acc = b_accessor_f32(&b, ibm, 2, 16, 0, "MAT4", NULL);
	in_acc = b_times(&b, times, 3);
	trans_out = b_vec3s(&b, joint0_keys, 3);
	w_out = b_scalars(&b, weights_keys, 6);
	other_in = b_times(&b, other_times, 2);
	other_out = b_vec3s(&b, other_values, 2);

	sb_appendf(&b.extra, &b.extra_len, &b.extra_cap,
			",\"nodes\":["
			"{\"mesh\":0,\"skin\":0},"
			"{\"translation\":[1,0,0]},"
			"{\"translation\":[0,1,0],\"children\":[0]}],"
			"\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":%d,"
			"\"JOINTS_0\":%d,\"WEIGHTS_0\":%d},"
			"\"targets\":[{\"POSITION\":%d},{\"POSITION\":%d}]}]}],"
			"\"skins\":[{\"joints\":[1,2],\"skeleton\":2,\"inverseBindMatrices\":%d}],"
			"\"scenes\":[{\"nodes\":[2]}],"
			"\"animations\":["
			"{\"samplers\":["
			"{\"input\":%d,\"output\":%d,\"interpolation\":\"LINEAR\"},"
			"{\"input\":%d,\"output\":%d,\"interpolation\":\"LINEAR\"}],"
			"\"channels\":["
			"{\"sampler\":0,\"target\":{\"node\":1,\"path\":\"translation\"}},"
			"{\"sampler\":1,\"target\":{\"node\":0,\"path\":\"weights\"}}]},"
			"{\"samplers\":[{\"input\":%d,\"output\":%d,\"interpolation\":\"LINEAR\"}],"
			"\"channels\":[{\"sampler\":0,\"target\":{\"node\":2,\"path\":\"translation\"}}]}]",
			pos, joints_acc, sw_acc, morph_ai, morph_bi, ibm_acc,
			in_acc, trans_out, in_acc, w_out, other_in, other_out);

	data = build_from_extra(&b);
	b_destroy(&b);
	return data;
}

/* Two separate buffers: times in buffer 0, translation values in buffer 1. */
static cgltf_data* build_two_buffer_asset(void)
{
	float times[3] = {0.0f, 1.0f, 2.0f};
	float values[9] = {
		0.0f, 0.0f, 0.0f,
		1.0f, 2.0f, 0.0f,
		3.0f, 0.0f, 1.0f,
	};
	char* uri0 = base64_encode((const unsigned char*)times, sizeof(times));
	char* uri1 = base64_encode((const unsigned char*)values, sizeof(values));
	char json[4096];
	cgltf_data* data;

	snprintf(json, sizeof(json),
			"{\"asset\":{\"version\":\"2.0\"},"
			"\"buffers\":["
			"{\"byteLength\":%d,\"uri\":\"data:application/octet-stream;base64,%s\"},"
			"{\"byteLength\":%d,\"uri\":\"data:application/octet-stream;base64,%s\"}],"
			"\"bufferViews\":["
			"{\"buffer\":0,\"byteOffset\":0,\"byteLength\":%d},"
			"{\"buffer\":1,\"byteOffset\":0,\"byteLength\":%d}],"
			"\"accessors\":["
			"{\"bufferView\":0,\"componentType\":5126,\"count\":3,\"type\":\"SCALAR\"},"
			"{\"bufferView\":1,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\"}],"
			"\"nodes\":[{\"translation\":[0,0,0]},{\"translation\":[0,0,0]}],"
			"\"scenes\":[{\"nodes\":[0,1]}],"
			"\"animations\":[{\"samplers\":[{\"input\":0,\"output\":1,"
			"\"interpolation\":\"LINEAR\"}],\"channels\":["
			"{\"sampler\":0,\"target\":{\"node\":1,\"path\":\"translation\"}}]}]}",
			(int)sizeof(times), uri0, (int)sizeof(values), uri1,
			(int)sizeof(times), (int)sizeof(values));
	free(uri0);
	free(uri1);
	data = parse_json_asset(json, 1);
	return data;
}

static int case_asset_two_buffers(void)
{
	cgltf_data* source = build_two_buffer_asset();
	cgltf_data* result = NULL;
	cgltf_data* reloaded = NULL;
	cgltf_options options;
	float st[3] = {0.0f, 1.0f, 2.0f};
	float ot[3] = {0.0f, 0.3f, 1.0f};
	cgltf_result status;
	const char* path = ".task-build/retime_two_buffers.gltf";
	int i;
	float value[3];

	TASK_CHECK(source != NULL);
	TASK_CHECK(source->buffers_count == 2);
	TASK_CHECK(source->buffer_views[0].buffer == &source->buffers[0]);
	TASK_CHECK(source->buffer_views[1].buffer == &source->buffers[1]);
	TASK_CHECK(cgltf_validate(source) == cgltf_result_success);

	status = cgltf_retime_animation(source, 0, st, ot, 3, &result);
	TASK_CHECK(status == cgltf_result_success);
	TASK_CHECK(result != NULL);
	TASK_CHECK(cgltf_validate(result) == cgltf_result_success);
	/* The two original buffers survive next to the appended retimed buffer. */
	TASK_CHECK(result->buffers_count == 3);
	for (i = 0; i < 3; ++i)
	{
		TASK_CHECK(result->buffers[i].uri &&
				strncmp(result->buffers[i].uri, "data:", 5) == 0);
		TASK_CHECK(result->buffers[i].data != NULL);
	}
	TASK_CHECK(compare_animation(source, 0, result, 0, st, ot, 3));

	cgltf_free(source);
	source = NULL;

	memset(&options, 0, sizeof(options));
	TASK_CHECK(cgltf_write_file(&options, path, result) == cgltf_result_success);
	TASK_CHECK(cgltf_parse_file(&options, path, &reloaded) == cgltf_result_success);
	TASK_CHECK(cgltf_load_buffers(&options, reloaded, path) == cgltf_result_success);
	TASK_CHECK(cgltf_validate(reloaded) == cgltf_result_success);
	{
		const cgltf_animation_channel* c =
				find_channel(reloaded, 0, 1, cgltf_animation_path_type_translation);
		TASK_CHECK(c != NULL);
		TASK_CHECK(cgltf_accessor_read_float(c->sampler->output, 1, value, 3));
		/* Original key at source 1.0 -> output 0.3, value (1,2,0). */
		TASK_CHECK(close_enough(value[0], 1.0) && close_enough(value[1], 2.0) &&
				close_enough(value[2], 0.0));
	}

	cgltf_free(result);
	cgltf_free(reloaded);
	remove(path);
	return 0;
}

static int case_asset_roundtrip(void)
{
	cgltf_data* source = build_skin_asset();
	cgltf_data* result = NULL;
	cgltf_data* reloaded = NULL;
	cgltf_options options;
	float st[3] = {0.0f, 0.5f, 2.0f};
	float ot[3] = {0.0f, 0.2f, 1.2f};
	double sample_out[7] = {0.0, 0.05, 0.2, 0.5, 0.9, 1.2, 1.1};
	float expected[7][3];
	cgltf_result status;
	int i, c;
	const char* path = ".task-build/retime_asset.gltf";

	TASK_CHECK(source != NULL);
	TASK_CHECK(cgltf_validate(source) == cgltf_result_success);
	TASK_CHECK(source->animations_count == 2);
	TASK_CHECK(source->skins_count == 1 && source->skins[0].joints_count == 2);

	for (i = 0; i < 7; ++i)
	{
		double s = unmap_time(st, ot, 3, sample_out[i]);
		TASK_CHECK(compute_skinned(source, 0, s, expected[i]));
	}

	status = cgltf_retime_animation(source, 0, st, ot, 3, &result);
	TASK_CHECK(status == cgltf_result_success);
	TASK_CHECK(result != NULL);
	TASK_CHECK(cgltf_validate(result) == cgltf_result_success);

	/* Node hierarchy, node indices, skin and animation count are preserved. */
	TASK_CHECK(result->nodes_count == source->nodes_count);
	TASK_CHECK(result->animations_count == 2);
	TASK_CHECK(cgltf_node_index(result, result->skins[0].joints[0]) == 1);
	TASK_CHECK(cgltf_node_index(result, result->skins[0].joints[1]) == 2);
	TASK_CHECK(result->skins[0].joints[0] >= result->nodes &&
			result->skins[0].joints[0] < result->nodes + result->nodes_count);
	TASK_CHECK(result->skins[0].inverse_bind_matrices >= result->accessors &&
			result->skins[0].inverse_bind_matrices <
					result->accessors + result->accessors_count);
	{
		float m[16];
		TASK_CHECK(cgltf_accessor_read_float(result->skins[0].inverse_bind_matrices, 0, m, 16));
		TASK_CHECK(m[12] == -1.0f && m[13] == 0.0f);
		TASK_CHECK(cgltf_accessor_read_float(result->skins[0].inverse_bind_matrices, 1, m, 16));
		TASK_CHECK(m[12] == 0.0f && m[13] == -1.0f);
	}

	/* Every result buffer owns embedded bytes. */
	for (i = 0; i < (int)result->buffers_count; ++i)
	{
		TASK_CHECK(result->buffers[i].uri &&
				strncmp(result->buffers[i].uri, "data:", 5) == 0);
		TASK_CHECK(result->buffers[i].data != NULL);
	}

	/* Meshes, morph targets and every node's local transform are preserved. */
	{
		cgltf_size n;
		TASK_CHECK(result->meshes_count == source->meshes_count);
		TASK_CHECK(result->meshes[0].primitives_count ==
				source->meshes[0].primitives_count);
		TASK_CHECK(result->meshes[0].primitives[0].targets_count == 2);
		TASK_CHECK(result->meshes[0].primitives[0].attributes_count ==
				source->meshes[0].primitives[0].attributes_count);
		for (n = 0; n < source->meshes[0].primitives[0].attributes_count; ++n)
		{
			cgltf_size k;
			const cgltf_accessor* a =
					source->meshes[0].primitives[0].attributes[n].data;
			const cgltf_accessor* b =
					result->meshes[0].primitives[0].attributes[n].data;
			TASK_CHECK(b->count == a->count);
			for (k = 0; k < a->count; ++k)
			{
				float va[4], vb[4];
				cgltf_size comps = cgltf_num_components(a->type);
				TASK_CHECK(cgltf_accessor_read_float(a, k, va, comps));
				TASK_CHECK(cgltf_accessor_read_float(b, k, vb, comps));
				TASK_CHECK(memcmp(va, vb, sizeof(float) * comps) == 0);
			}
		}
		for (n = 0; n < source->nodes_count; ++n)
		{
			TASK_CHECK(result->nodes[n].has_translation ==
					source->nodes[n].has_translation);
			TASK_CHECK(result->nodes[n].has_rotation ==
					source->nodes[n].has_rotation);
			TASK_CHECK(result->nodes[n].has_matrix == source->nodes[n].has_matrix);
			TASK_CHECK(memcmp(result->nodes[n].translation,
					source->nodes[n].translation, sizeof(source->nodes[n].translation)) == 0);
			TASK_CHECK(memcmp(result->nodes[n].rotation,
					source->nodes[n].rotation, sizeof(source->nodes[n].rotation)) == 0);
		}
	}

	TASK_CHECK(compare_animation(source, 0, result, 0, st, ot, 3));

	/* The unselected animation is untouched and still usable. */
	{
		const cgltf_animation_channel* ch =
				find_channel(result, 1, 2, cgltf_animation_path_type_translation);
		ref_chan rc;
		float value[3];
		TASK_CHECK(ch != NULL);
		TASK_CHECK(ref_init(&rc, ch));
		ref_eval(&rc, 1.0, value);
		TASK_CHECK(close_enough(value[0], 0.0) && close_enough(value[1], 2.5) &&
				close_enough(value[2], 0.0));
		ref_free(&rc);
	}

	/* Source and result agree on a real skinned + morphed point. */
	for (i = 0; i < 7; ++i)
	{
		double s = unmap_time(st, ot, 3, sample_out[i]);
		float pa[3], pb[3];
		TASK_CHECK(compute_skinned(source, 0, s, pa));
		TASK_CHECK(compute_skinned(result, 0, sample_out[i], pb));
		for (c = 0; c < 3; ++c)
		{
			TASK_CHECK(close_enough(pb[c], pa[c]));
		}
	}

	cgltf_free(source);
	source = NULL;

	/* The result is independent of the source. */
	for (i = 0; i < 7; ++i)
	{
		float p[3];
		TASK_CHECK(compute_skinned(result, 0, sample_out[i], p));
		for (c = 0; c < 3; ++c)
		{
			TASK_CHECK(close_enough(p[c], expected[i][c]));
		}
	}

	/* Write, reparse, reload and validate without the original buffers. */
	memset(&options, 0, sizeof(options));
	TASK_CHECK(cgltf_write_file(&options, path, result) == cgltf_result_success);
	TASK_CHECK(cgltf_parse_file(&options, path, &reloaded) == cgltf_result_success);
	TASK_CHECK(reloaded != NULL);
	TASK_CHECK(cgltf_load_buffers(&options, reloaded, path) == cgltf_result_success);
	TASK_CHECK(cgltf_validate(reloaded) == cgltf_result_success);
	TASK_CHECK(reloaded->animations_count == 2);
	TASK_CHECK(reloaded->buffers_count == result->buffers_count);
	for (i = 0; i < (int)reloaded->buffers_count; ++i)
	{
		TASK_CHECK(reloaded->buffers[i].uri &&
				strncmp(reloaded->buffers[i].uri, "data:", 5) == 0);
	}
	TASK_CHECK(find_channel(reloaded, 1, 2, cgltf_animation_path_type_translation) != NULL);
	TASK_CHECK(find_channel(reloaded, 0, 1, cgltf_animation_path_type_translation) != NULL);
	TASK_CHECK(find_channel(reloaded, 0, 0, cgltf_animation_path_type_weights) != NULL);
	for (i = 0; i < 7; ++i)
	{
		float p[3];
		TASK_CHECK(compute_skinned(reloaded, 0, sample_out[i], p));
		for (c = 0; c < 3; ++c)
		{
			TASK_CHECK(close_enough(p[c], expected[i][c]));
		}
	}

	cgltf_free(result);
	cgltf_free(reloaded);
	remove(path);
	return 0;
}

/* ------------------------------------------------------------------ */
/* invalid group                                                       */
/* ------------------------------------------------------------------ */

/* Valid translation + rotation channels; optional broken variants. */
static cgltf_data* build_simple_animation_asset(int duplicate_channels,
		int wrong_output_type)
{
	builder b;
	cgltf_data* data;
	float times[2] = {0.0f, 1.0f};
	float translation[6] = {0.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f};
	float rotation[8] = {0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f, 0.0f};
	float translation4[8] = {0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f, 0.0f};
	int in_acc, trans_acc, rot_acc;

	b_init(&b);
	in_acc = b_times(&b, times, 2);
	trans_acc = wrong_output_type
			? b_vec4s(&b, translation4, 2)
			: b_vec3s(&b, translation, 2);
	rot_acc = b_vec4s(&b, rotation, 2);

	sb_appendf(&b.extra, &b.extra_len, &b.extra_cap,
			",\"nodes\":[{\"translation\":[0,0,0]},{\"translation\":[0,0,0],"
			"\"rotation\":[0,0,0,1]}],"
			"\"scenes\":[{\"nodes\":[0,1]}],"
			"\"animations\":[{\"samplers\":["
			"{\"input\":%d,\"output\":%d,\"interpolation\":\"LINEAR\"},"
			"{\"input\":%d,\"output\":%d,\"interpolation\":\"LINEAR\"}],"
			"\"channels\":["
			"{\"sampler\":0,\"target\":{\"node\":1,\"path\":\"translation\"}},"
			"%s"
			"{\"sampler\":1,\"target\":{\"node\":1,\"path\":\"rotation\"}}]}]",
			in_acc, trans_acc, in_acc, rot_acc,
			duplicate_channels
				? "{\"sampler\":0,\"target\":{\"node\":1,\"path\":\"translation\"}},"
				: "");

	data = build_from_extra(&b);
	b_destroy(&b);
	return data;
}

/* Cubic rotation keys {0.5, 1.0, 1.5}; translation covers [0, 2]. */
static cgltf_data* build_cubic_rotation_asset(void)
{
	builder b;
	cgltf_data* data;
	float rot_t[3] = {0.5f, 1.0f, 1.5f};
	float blocks[3 * 12] = {
		0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.1f, 0.0f, 0.0f, 0.0f,
		0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.70710678f, 0.70710678f, 0.0f, 0.0f, 0.1f, 0.0f,
		0.0f, 0.1f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f,
	};
	float trans_t[2] = {0.0f, 2.0f};
	float trans_v[6] = {0.0f, 0.0f, 0.0f, 2.0f, 0.0f, 0.0f};
	int rot_in, rot_out, trans_in, trans_out;

	b_init(&b);
	rot_in = b_times(&b, rot_t, 3);
	rot_out = b_cubic_vec4(&b, blocks, 3);
	trans_in = b_times(&b, trans_t, 2);
	trans_out = b_vec3s(&b, trans_v, 2);

	sb_appendf(&b.extra, &b.extra_len, &b.extra_cap,
			",\"nodes\":[{\"rotation\":[0,0,0,1]},{\"translation\":[0,0,0]}],"
			"\"scenes\":[{\"nodes\":[0,1]}],"
			"\"animations\":[{\"samplers\":["
			"{\"input\":%d,\"output\":%d,\"interpolation\":\"CUBICSPLINE\"},"
			"{\"input\":%d,\"output\":%d,\"interpolation\":\"LINEAR\"}],"
			"\"channels\":["
			"{\"sampler\":0,\"target\":{\"node\":0,\"path\":\"rotation\"}},"
			"{\"sampler\":1,\"target\":{\"node\":1,\"path\":\"translation\"}}]}]",
			rot_in, rot_out, trans_in, trans_out);

	data = build_from_extra(&b);
	b_destroy(&b);
	return data;
}

static int case_invalid_arguments(void)
{
	cgltf_data* source = build_simple_animation_asset(0, 0);
	cgltf_data* result;
	float st[3] = {0.0f, 0.5f, 1.0f};
	float ot[3] = {0.0f, 0.25f, 1.0f};
	float bad[3];

	TASK_CHECK(source != NULL);
	TASK_CHECK(cgltf_validate(source) == cgltf_result_success);

	result = (cgltf_data*)0x1;
	TASK_CHECK(cgltf_retime_animation(source, 0, st, ot, 1, &result) ==
			cgltf_result_invalid_options);
	TASK_CHECK(result == NULL);

	result = (cgltf_data*)0x1;
	TASK_CHECK(cgltf_retime_animation(source, 0, NULL, ot, 3, &result) ==
			cgltf_result_invalid_options);
	TASK_CHECK(result == NULL);

	result = (cgltf_data*)0x1;
	TASK_CHECK(cgltf_retime_animation(source, 0, st, NULL, 3, &result) ==
			cgltf_result_invalid_options);
	TASK_CHECK(result == NULL);

	bad[0] = st[0]; bad[1] = NAN; bad[2] = st[2];
	result = (cgltf_data*)0x1;
	TASK_CHECK(cgltf_retime_animation(source, 0, bad, ot, 3, &result) ==
			cgltf_result_invalid_options);
	TASK_CHECK(result == NULL);

	bad[1] = INFINITY;
	result = (cgltf_data*)0x1;
	TASK_CHECK(cgltf_retime_animation(source, 0, st, bad, 3, &result) ==
			cgltf_result_invalid_options);
	TASK_CHECK(result == NULL);

	bad[0] = 0.1f; bad[1] = ot[1]; bad[2] = ot[2];
	result = (cgltf_data*)0x1;
	TASK_CHECK(cgltf_retime_animation(source, 0, st, bad, 3, &result) ==
			cgltf_result_invalid_options);
	TASK_CHECK(result == NULL);

	bad[0] = st[0]; bad[1] = st[1]; bad[2] = st[1];
	result = (cgltf_data*)0x1;
	TASK_CHECK(cgltf_retime_animation(source, 0, bad, ot, 3, &result) ==
			cgltf_result_invalid_options);
	TASK_CHECK(result == NULL);

	bad[0] = ot[0]; bad[1] = ot[2]; bad[2] = ot[1];
	result = (cgltf_data*)0x1;
	TASK_CHECK(cgltf_retime_animation(source, 0, st, bad, 3, &result) ==
			cgltf_result_invalid_options);
	TASK_CHECK(result == NULL);

	bad[0] = -0.1f; bad[1] = 0.5f; bad[2] = 1.0f;
	result = (cgltf_data*)0x1;
	TASK_CHECK(cgltf_retime_animation(source, 0, bad, ot, 3, &result) ==
			cgltf_result_invalid_options);
	TASK_CHECK(result == NULL);

	bad[0] = 0.0f; bad[1] = 0.5f; bad[2] = 1.1f;
	result = (cgltf_data*)0x1;
	TASK_CHECK(cgltf_retime_animation(source, 0, bad, ot, 3, &result) ==
			cgltf_result_invalid_options);
	TASK_CHECK(result == NULL);

	result = (cgltf_data*)0x1;
	TASK_CHECK(cgltf_retime_animation(source, 5, st, ot, 3, &result) ==
			cgltf_result_invalid_options);
	TASK_CHECK(result == NULL);

	TASK_CHECK(cgltf_retime_animation(source, 0, st, ot, 3, NULL) ==
			cgltf_result_invalid_options);

	/* A valid call still succeeds after all rejections. */
	result = NULL;
	TASK_CHECK(cgltf_retime_animation(source, 0, st, ot, 3, &result) ==
			cgltf_result_success);
	TASK_CHECK(result != NULL);
	TASK_CHECK(cgltf_validate(result) == cgltf_result_success);
	TASK_CHECK(compare_animation(source, 0, result, 0, st, ot, 3));
	cgltf_free(result);
	cgltf_free(source);
	return 0;
}

static int case_invalid_shapes(void)
{
	cgltf_data* source;
	cgltf_data* result;
	float st[2] = {0.0f, 1.0f};
	float ot[2] = {0.0f, 1.0f};

	/* Duplicate target attribute. */
	source = build_simple_animation_asset(1, 0);
	TASK_CHECK(source != NULL);
	result = (cgltf_data*)0x1;
	TASK_CHECK(cgltf_retime_animation(source, 0, st, ot, 2, &result) ==
			cgltf_result_invalid_gltf);
	TASK_CHECK(result == NULL);
	cgltf_free(source);

	/* Translation output is not VEC3. */
	source = build_simple_animation_asset(0, 1);
	TASK_CHECK(source != NULL);
	result = (cgltf_data*)0x1;
	TASK_CHECK(cgltf_retime_animation(source, 0, st, ot, 2, &result) ==
			cgltf_result_invalid_gltf);
	TASK_CHECK(result == NULL);
	cgltf_free(source);

	/* Non-increasing key sequence. */
	{
		builder b;
		float bad_times[2] = {1.0f, 0.5f};
		float values[6] = {0.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f};
		float good_times[2] = {0.0f, 1.0f};
		int in_bad, out_bad, in_good;
		b_init(&b);
		in_bad = b_times(&b, bad_times, 2);
		out_bad = b_vec3s(&b, values, 2);
		in_good = b_times(&b, good_times, 2);
		sb_appendf(&b.extra, &b.extra_len, &b.extra_cap,
				",\"nodes\":[{\"translation\":[0,0,0]},{\"translation\":[0,0,0]}],"
				"\"scenes\":[{\"nodes\":[0,1]}],"
				"\"animations\":[{\"samplers\":["
				"{\"input\":%d,\"output\":%d,\"interpolation\":\"LINEAR\"},"
				"{\"input\":%d,\"output\":%d,\"interpolation\":\"LINEAR\"}],"
				"\"channels\":["
				"{\"sampler\":0,\"target\":{\"node\":1,\"path\":\"translation\"}},"
				"{\"sampler\":1,\"target\":{\"node\":0,\"path\":\"translation\"}}]}]",
				in_bad, out_bad, in_good, out_bad);
		source = build_from_extra(&b);
		b_destroy(&b);
		TASK_CHECK(source != NULL);
		result = (cgltf_data*)0x1;
		TASK_CHECK(cgltf_retime_animation(source, 0, st, ot, 2, &result) !=
				cgltf_result_success);
		TASK_CHECK(result == NULL);
		cgltf_free(source);
	}

	/* Output element count does not match keys * components. */
	{
		builder b;
		float times[2] = {0.0f, 1.0f};
		float values[3] = {0.0f, 0.0f, 0.0f}; /* only one VEC3 */
		int in_acc, out_acc;
		b_init(&b);
		in_acc = b_times(&b, times, 2);
		out_acc = b_vec3s(&b, values, 1);
		sb_appendf(&b.extra, &b.extra_len, &b.extra_cap,
				",\"nodes\":[{\"translation\":[0,0,0]}],"
				"\"scenes\":[{\"nodes\":[0]}],"
				"\"animations\":[{\"samplers\":[{\"input\":%d,\"output\":%d,"
				"\"interpolation\":\"LINEAR\"}],\"channels\":["
				"{\"sampler\":0,\"target\":{\"node\":0,\"path\":\"translation\"}}]}]",
				in_acc, out_acc);
		source = build_from_extra(&b);
		b_destroy(&b);
		TASK_CHECK(source != NULL);
		result = (cgltf_data*)0x1;
		TASK_CHECK(cgltf_retime_animation(source, 0, st, ot, 2, &result) ==
				cgltf_result_invalid_gltf);
		TASK_CHECK(result == NULL);
		cgltf_free(source);
	}

	return 0;
}

static int case_invalid_unloaded(void)
{
	cgltf_data* source = build_simple_animation_asset(0, 0);
	cgltf_options options;
	float st[2] = {0.0f, 1.0f};
	float ot[2] = {0.0f, 1.0f};
	cgltf_data* result = (cgltf_data*)0x1;

	TASK_CHECK(source != NULL);
	memset(&options, 0, sizeof(options));
	{
		cgltf_size size = cgltf_write(&options, NULL, 0, source);
		char* json = (char*)malloc(size);
		cgltf_data* unloaded = NULL;
		TASK_CHECK(json != NULL);
		TASK_CHECK(cgltf_write(&options, json, size, source) == size);
		TASK_CHECK(cgltf_parse(&options, json, size - 1, &unloaded) ==
				cgltf_result_success);
		free(json);
		TASK_CHECK(unloaded != NULL);
		/* Buffers were never loaded: reading the key data must fail. */
		TASK_CHECK(cgltf_retime_animation(unloaded, 0, st, ot, 2, &result) ==
				cgltf_result_data_too_short);
		TASK_CHECK(result == NULL);
		cgltf_free(unloaded);
	}
	cgltf_free(source);
	return 0;
}

static int case_invalid_cubic_rotation(void)
{
	cgltf_data* source = build_cubic_rotation_asset();
	cgltf_data* result;
	float st_bad[3] = {0.0f, 0.75f, 2.0f}; /* 0.75 is inside (0.5, 1.0) */
	float st_ok[4] = {0.0f, 0.5f, 1.5f, 2.0f}; /* knots at keys/endpoints */
	float ot_bad[3] = {0.0f, 0.25f, 1.0f};
	float ot_ok[4] = {0.0f, 0.25f, 1.0f, 1.4f};

	TASK_CHECK(source != NULL);
	TASK_CHECK(cgltf_validate(source) == cgltf_result_success);

	result = (cgltf_data*)0x1;
	TASK_CHECK(cgltf_retime_animation(source, 0, st_bad, ot_bad, 3, &result) ==
			cgltf_result_invalid_options);
	TASK_CHECK(result == NULL);

	/* Outside the sampler's own key span the channel extends constantly, so
	 * a map knot there is allowed even for cubic rotations. */
	result = NULL;
	TASK_CHECK(cgltf_retime_animation(source, 0, st_ok, ot_ok, 4, &result) ==
			cgltf_result_success);
	TASK_CHECK(result != NULL);
	TASK_CHECK(cgltf_validate(result) == cgltf_result_success);
	TASK_CHECK(compare_animation(source, 0, result, 0, st_ok, ot_ok, 4));

	/* The rotation channel keeps its cubic interpolation across the span. */
	{
		const cgltf_animation_channel* c =
				find_channel(result, 0, 0, cgltf_animation_path_type_rotation);
		TASK_CHECK(c && c->sampler->interpolation == cgltf_interpolation_type_cubic_spline);
		/* keys: 0 (extension), 0.5, 1.0, 1.5, 2.0 (extension) */
		TASK_CHECK(c->sampler->input->count == 5);
	}

	cgltf_free(result);
	cgltf_free(source);
	return 0;
}

static int case_invalid_compressed(void)
{
	cgltf_data* source = build_simple_animation_asset(0, 0);
	cgltf_data* result;
	float st[2] = {0.0f, 1.0f};
	float ot[2] = {0.0f, 1.0f};

	TASK_CHECK(source != NULL);
	TASK_CHECK(source->buffer_views_count > 0);
	source->buffer_views[0].has_meshopt_compression = 1;
	result = (cgltf_data*)0x1;
	TASK_CHECK(cgltf_retime_animation(source, 0, st, ot, 2, &result) ==
			cgltf_result_unknown_format);
	TASK_CHECK(result == NULL);
	source->buffer_views[0].has_meshopt_compression = 0;

	/* A compressed-buffer marker on a buffer view is rejected as well. */
	source->buffer_views[0].has_meshopt_compression = 1;
	{
		cgltf_data* again = NULL;
		TASK_CHECK(cgltf_retime_animation(source, 0, st, ot, 2, &again) ==
				cgltf_result_unknown_format);
		TASK_CHECK(again == NULL);
	}
	source->buffer_views[0].has_meshopt_compression = 0;

	/* Draco compressed primitives are rejected. */
	{
		builder b;
		cgltf_data* draco;
		float times[2] = {0.0f, 1.0f};
		float values[6] = {0.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f};
		int in_acc, out_acc, pos;
		b_init(&b);
		in_acc = b_times(&b, times, 2);
		out_acc = b_vec3s(&b, values, 2);
		pos = b_vec3s(&b, values, 2);
		sb_appendf(&b.extra, &b.extra_len, &b.extra_cap,
				",\"nodes\":[{\"mesh\":0},{\"translation\":[0,0,0]}],"
				"\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":%d},"
				"\"extensions\":{\"KHR_draco_mesh_compression\":{"
				"\"bufferView\":0,\"attributes\":{\"POSITION\":0}}}}]}],"
				"\"scenes\":[{\"nodes\":[0,1]}],"
				"\"animations\":[{\"samplers\":[{\"input\":%d,\"output\":%d,"
				"\"interpolation\":\"LINEAR\"}],\"channels\":["
				"{\"sampler\":0,\"target\":{\"node\":1,\"path\":\"translation\"}}]}]",
				pos, in_acc, out_acc);
		draco = build_from_extra(&b);
		b_destroy(&b);
		TASK_CHECK(draco != NULL);
		result = (cgltf_data*)0x1;
		TASK_CHECK(cgltf_retime_animation(draco, 0, st, ot, 2, &result) ==
				cgltf_result_unknown_format);
		TASK_CHECK(result == NULL);
		cgltf_free(draco);
	}

	cgltf_free(source);
	return 0;
}

/* ------------------------------------------------------------------ */
/* registration                                                        */
/* ------------------------------------------------------------------ */

const struct task_case task_cases[] = {
	{"cubic", "trajectory", case_cubic_trajectory},
	{"cubic", "split_tangents", case_cubic_split_tangents},
	{"cubic", "sparse_stride", case_cubic_sparse_stride},
	{"rotation_step", "shortest_arc", case_rotation_shortest_arc},
	{"rotation_step", "shared_sampler", case_rotation_shared_sampler},
	{"rotation_step", "trimmed_span", case_rotation_trimmed_span},
	{"asset", "roundtrip_skin_morph", case_asset_roundtrip},
	{"asset", "two_buffers", case_asset_two_buffers},
	{"invalid", "arguments", case_invalid_arguments},
	{"invalid", "shapes", case_invalid_shapes},
	{"invalid", "unloaded_buffers", case_invalid_unloaded},
	{"invalid", "cubic_rotation_restriction", case_invalid_cubic_rotation},
	{"invalid", "compressed", case_invalid_compressed},
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
