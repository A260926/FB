#include "task_support/check.h"

#include <math.h>
#include <stdint.h>

#include <common/color.h>
#include <common/hashtable.h>
#include <common/mempool.h>
#include <common/vector.h>
#include <accelerators/bvh.h>
#include <datatypes/scene.h>
#include <nodes/bsdfnode.h>
#include <nodes/textures/constant.h>
#include <nodes/valuenode.h>
#include <renderer/pathtrace.h>
#include <renderer/samplers/sampler.h>

#define TEST_PI 3.14159265358979323846f

struct kit {
	struct node_storage storage;
	sampler *rng;
};

static void kit_init(struct kit *kit, uint32_t seed) {
	kit->storage.node_pool = newBlock(NULL, 1 << 16);
	kit->storage.node_table = newHashtable(compareNodes, &kit->storage.node_pool);
	kit->rng = sampler_new();
	sampler_init(kit->rng, Random, 0, 1, seed);
}

static void kit_destroy(struct kit *kit) {
	sampler_destroy(kit->rng);
	destroyHashtable(kit->storage.node_table);
	destroyBlocks(kit->storage.node_pool);
}

static struct color rgb(float r, float g, float b) {
	return (struct color){ .red = r, .green = g, .blue = b, .alpha = 1.0f };
}

static const struct colorNode *color_node(struct kit *kit, float r, float g, float b) {
	return newConstantTexture(&kit->storage, rgb(r, g, b));
}

static const struct valueNode *value_node(struct kit *kit, float value) {
	return newConstantValue(&kit->storage, value);
}

static struct vector v3(float x, float y, float z) {
	return (struct vector){ x, y, z };
}

static int nearf(float a, float b, float tolerance) {
	return isfinite(a) && isfinite(b) && fabsf(a - b) <= tolerance;
}

static int near_rgb(struct color c, float r, float g, float b, float tolerance) {
	return nearf(c.red, r, tolerance) &&
		   nearf(c.green, g, tolerance) &&
		   nearf(c.blue, b, tolerance);
}

static struct hitRecord test_hit(struct lightRay *ray) {
	*ray = (struct lightRay){
		.start = v3(0.0f, 2.0f, 0.0f),
		.direction = v3(0.0f, -1.0f, 0.0f)
	};
	return (struct hitRecord){
		.incident = ray,
		.hitPoint = v3(0.0f, 0.0f, 0.0f),
		.surfaceNormal = v3(0.0f, 1.0f, 0.0f)
	};
}

enum closure_kind {
	CLOSURE_DIFFUSE,
	CLOSURE_TRANSLUCENT,
	CLOSURE_ISOTROPIC,
	CLOSURE_EMISSION
};

static float expected_leaf_pdf(enum closure_kind kind, struct vector normal, struct vector direction) {
	const float cosine = vec_dot(normal, direction);
	switch (kind) {
		case CLOSURE_DIFFUSE:
		case CLOSURE_EMISSION:
			return fmaxf(cosine, 0.0f) / TEST_PI;
		case CLOSURE_TRANSLUCENT:
			return fmaxf(-cosine, 0.0f) / TEST_PI;
		case CLOSURE_ISOTROPIC:
			return 1.0f / (4.0f * TEST_PI);
	}
	return 0.0f;
}

static int expected_leaf_response(enum closure_kind kind, struct color color,
								  struct vector normal, struct vector direction,
								  float *r, float *g, float *b) {
	const float pdf = expected_leaf_pdf(kind, normal, direction);
	if (kind == CLOSURE_EMISSION) {
		*r = *g = *b = 0.0f;
	} else {
		*r = color.red * pdf;
		*g = color.green * pdf;
		*b = color.blue * pdf;
	}
	return 1;
}

static int eval_close(const struct bsdfNode *node, sampler *rng, const struct hitRecord *hit,
					  struct vector direction, float r, float g, float b, float pdf,
					  float tolerance) {
	const struct bsdfEval got = node->eval(node, rng, hit, direction);
	return near_rgb(got.response, r, g, b, tolerance) &&
		   nearf(got.pdf, pdf, tolerance);
}

static int emission_close(const struct bsdfNode *node, sampler *rng, const struct hitRecord *hit,
						  float r, float g, float b, float tolerance) {
	const struct color got = node->emission(node, rng, hit);
	return near_rgb(got, r, g, b, tolerance);
}

static struct vector *test_directions(size_t *count) {
	static struct vector directions[] = {
		{ 0.0f,  1.0f,  0.0f},
		{ 0.0f, -1.0f,  0.0f},
		{ 1.0f,  0.0f,  0.0f},
		{ 0.0f,  0.0f,  1.0f},
		{ 0.0f,  0.0f, -1.0f},
		{ 0.0f,  0.707106781187f,  0.707106781187f},
		{ 0.707106781187f, -0.707106781187f, 0.0f},
		{ 0.267261241912f, 0.534522483825f, 0.801783725737f }
	};
	*count = sizeof(directions) / sizeof(directions[0]);
	return directions;
}

static int density_leaf_pointwise(void) {
	struct kit kit;
	kit_init(&kit, 101);
	struct lightRay ray;
	struct hitRecord hit = test_hit(&ray);
	const struct vector n = hit.surfaceNormal;
	const struct color dc = rgb(0.20f, 0.30f, 0.40f);
	const struct color tc = rgb(0.50f, 0.25f, 0.10f);
	const struct color ic = rgb(0.10f, 0.40f, 0.80f);
	const struct color ec = rgb(1.00f, 0.25f, 0.00f);
	const struct bsdfNode *nodes[4] = {
		newDiffuse(&kit.storage, newConstantTexture(&kit.storage, dc)),
		newTranslucent(&kit.storage, newConstantTexture(&kit.storage, tc)),
		newIsotropic(&kit.storage, newConstantTexture(&kit.storage, ic)),
		newEmission(&kit.storage, newConstantTexture(&kit.storage, ec), value_node(&kit, 2.0f))
	};
	const struct color colors[4] = { dc, tc, ic, ec };
	const enum closure_kind kinds[4] = {
		CLOSURE_DIFFUSE, CLOSURE_TRANSLUCENT, CLOSURE_ISOTROPIC, CLOSURE_EMISSION
	};
	for (int i = 0; i < 4; ++i) {
		TASK_CHECK(nodes[i]->eval && nodes[i]->emission);
	}

	size_t direction_count;
	struct vector *directions = test_directions(&direction_count);
	for (size_t k = 0; k < direction_count; ++k) {
		for (int leaf = 0; leaf < 4; ++leaf) {
			float r, g, b;
			const float pdf = expected_leaf_pdf(kinds[leaf], n, directions[k]);
			expected_leaf_response(kinds[leaf], colors[leaf], n, directions[k], &r, &g, &b);
			TASK_CHECK(eval_close(nodes[leaf], kit.rng, &hit, directions[k],
								  r, g, b, pdf, 2.0e-6f));
		}
	}

	TASK_CHECK(emission_close(nodes[0], kit.rng, &hit, 0.0f, 0.0f, 0.0f, 0.0f));
	TASK_CHECK(emission_close(nodes[1], kit.rng, &hit, 0.0f, 0.0f, 0.0f, 0.0f));
	TASK_CHECK(emission_close(nodes[2], kit.rng, &hit, 0.0f, 0.0f, 0.0f, 0.0f));
	TASK_CHECK(emission_close(nodes[3], kit.rng, &hit, 2.0f, 0.5f, 0.0f, 1.0e-6f));
	kit_destroy(&kit);
	return 0;
}

struct integral {
	double pdf;
	double red, green, blue;
};

static struct integral integrate_over_sphere(const struct bsdfNode *node, sampler *rng,
											 const struct hitRecord *hit, int count) {
	struct integral result = { 0 };
	const double weight = 4.0 * (double)TEST_PI / (double)count;
	const double golden_angle = TEST_PI * (3.0 - sqrt(5.0));
	for (int i = 0; i < count; ++i) {
		const double y = 1.0 - 2.0 * ((double)i + 0.5) / (double)count;
		const double radius = sqrt(fmax(0.0, 1.0 - y * y));
		const double phi = golden_angle * (double)i;
		const struct vector direction = {
			(float)(cos(phi) * radius),
			(float)y,
			(float)(sin(phi) * radius)
		};
		const struct bsdfEval value = node->eval(node, rng, hit, direction);
		result.pdf += (double)value.pdf * weight;
		result.red += (double)value.response.red * weight;
		result.green += (double)value.response.green * weight;
		result.blue += (double)value.response.blue * weight;
	}
	return result;
}

static int close_double(double a, double b, double tolerance) {
	return isfinite(a) && isfinite(b) && fabs(a - b) <= tolerance;
}

static int density_leaf_integrals(void) {
	struct kit kit;
	kit_init(&kit, 102);
	struct lightRay ray;
	struct hitRecord hit = test_hit(&ray);
	const struct color dc = rgb(0.20f, 0.30f, 0.40f);
	const struct color tc = rgb(0.50f, 0.25f, 0.10f);
	const struct color ic = rgb(0.10f, 0.40f, 0.80f);
	const struct bsdfNode *diffuse = newDiffuse(&kit.storage, newConstantTexture(&kit.storage, dc));
	const struct bsdfNode *translucent = newTranslucent(&kit.storage, newConstantTexture(&kit.storage, tc));
	const struct bsdfNode *isotropic = newIsotropic(&kit.storage, newConstantTexture(&kit.storage, ic));
	const struct bsdfNode *emissive = newEmission(&kit.storage, color_node(&kit, 1.0f, 0.0f, 0.0f), value_node(&kit, 3.0f));

	const int samples = 16384;
	const struct integral di = integrate_over_sphere(diffuse, kit.rng, &hit, samples);
	const struct integral ti = integrate_over_sphere(translucent, kit.rng, &hit, samples);
	const struct integral ii = integrate_over_sphere(isotropic, kit.rng, &hit, samples);
	const struct integral ei = integrate_over_sphere(emissive, kit.rng, &hit, samples);

	TASK_CHECK(close_double(di.pdf, 1.0, 0.004));
	TASK_CHECK(close_double(di.red, dc.red, 0.004));
	TASK_CHECK(close_double(di.green, dc.green, 0.004));
	TASK_CHECK(close_double(di.blue, dc.blue, 0.004));

	TASK_CHECK(close_double(ti.pdf, 1.0, 0.004));
	TASK_CHECK(close_double(ti.red, tc.red, 0.004));
	TASK_CHECK(close_double(ti.green, tc.green, 0.004));
	TASK_CHECK(close_double(ti.blue, tc.blue, 0.004));

	TASK_CHECK(close_double(ii.pdf, 1.0, 1.0e-6));
	TASK_CHECK(close_double(ii.red, ic.red, 1.0e-6));
	TASK_CHECK(close_double(ii.green, ic.green, 1.0e-6));
	TASK_CHECK(close_double(ii.blue, ic.blue, 1.0e-6));

	TASK_CHECK(close_double(ei.pdf, 1.0, 0.004));
	TASK_CHECK(close_double(ei.red, 0.0, 1.0e-6));
	TASK_CHECK(close_double(ei.green, 0.0, 1.0e-6));
	TASK_CHECK(close_double(ei.blue, 0.0, 1.0e-6));
	kit_destroy(&kit);
	return 0;
}

static int density_query_consumes_no_samples(void) {
	struct kit kit;
	kit_init(&kit, 103);
	struct lightRay ray;
	struct hitRecord hit = test_hit(&ray);
	const struct bsdfNode *leaf = newDiffuse(&kit.storage, color_node(&kit, 0.2f, 0.3f, 0.4f));
	const struct bsdfNode *graph = newAdd(&kit.storage, leaf,
		newEmission(&kit.storage, color_node(&kit, 1.0f, 0.0f, 0.0f), value_node(&kit, 2.0f)));
	size_t count;
	struct vector *directions = test_directions(&count);
	for (size_t i = 0; i < count; ++i) {
		(void)graph->eval(graph, kit.rng, &hit, directions[i]);
		(void)graph->emission(graph, kit.rng, &hit);
	}
	const float after_queries = sampler_dimension(kit.rng);

	sampler *control = sampler_new();
	sampler_init(control, Random, 0, 1, 103);
	const float untouched_first = sampler_dimension(control);
	sampler_destroy(control);
	TASK_CHECK(after_queries == untouched_first);
	kit_destroy(&kit);
	return 0;
}

static int sample_batch_is_consistent(const struct bsdfNode *node, uint32_t seed, int batches) {
	sampler *rng = sampler_new();
	sampler_init(rng, Random, 0, 1, seed);
	struct lightRay ray;
	struct hitRecord hit = test_hit(&ray);
	int ok = 1;
	for (int i = 0; i < batches && ok; ++i) {
		const struct bsdfSample sample = node->sample(node, rng, &hit);
		const float length = sqrtf(vec_length_squared(sample.out.direction));
		ok = isfinite(sample.out.direction.x) && isfinite(sample.out.direction.y) &&
			 isfinite(sample.out.direction.z) && nearf(length, 1.0f, 5.0e-5f);
		ok = ok && isfinite(sample.pdf) && sample.pdf > 0.0f;
		ok = ok && isfinite(sample.weight.red) && sample.weight.red >= 0.0f &&
			 isfinite(sample.weight.green) && sample.weight.green >= 0.0f &&
			 isfinite(sample.weight.blue) && sample.weight.blue >= 0.0f;
		if (!ok)
			break;
		const struct bsdfEval full = node->eval(node, rng, &hit, sample.out.direction);
		const struct color emitted = node->emission(node, rng, &hit);
		ok = nearf(sample.pdf, full.pdf, 1.0e-6f);
		if (full.pdf > 0.0f) {
			ok = ok && nearf(sample.weight.red, full.response.red / full.pdf, 2.0e-5f);
			ok = ok && nearf(sample.weight.green, full.response.green / full.pdf, 2.0e-5f);
			ok = ok && nearf(sample.weight.blue, full.response.blue / full.pdf, 2.0e-5f);
		}
		ok = ok && nearf(sample.emitted.red, emitted.red, 1.0e-6f);
		ok = ok && nearf(sample.emitted.green, emitted.green, 1.0e-6f);
		ok = ok && nearf(sample.emitted.blue, emitted.blue, 1.0e-6f);
	}
	sampler_destroy(rng);
	return ok;
}

static int sampler_consumes_dimensions(const struct bsdfNode *node, uint32_t seed, int dimensions) {
	sampler *probe = sampler_new();
	sampler_init(probe, Random, 0, 1, seed);
	struct lightRay ray;
	struct hitRecord hit = test_hit(&ray);
	node->sample(node, probe, &hit);
	const float probe_next = sampler_dimension(probe);
	sampler_destroy(probe);

	sampler *control = sampler_new();
	sampler_init(control, Random, 0, 1, seed);
	for (int i = 0; i < dimensions; ++i)
		(void)sampler_dimension(control);
	const float control_next = sampler_dimension(control);
	sampler_destroy(control);
	return probe_next == control_next;
}

static void sample_mean_weight(const struct bsdfNode *node, uint32_t seed, int batches, double mean[3]) {
	sampler *rng = sampler_new();
	sampler_init(rng, Random, 0, 1, seed);
	struct lightRay ray;
	struct hitRecord hit = test_hit(&ray);
	double sum[3] = { 0.0, 0.0, 0.0 };
	for (int i = 0; i < batches; ++i) {
		const struct bsdfSample sample = node->sample(node, rng, &hit);
		sum[0] += sample.weight.red;
		sum[1] += sample.weight.green;
		sum[2] += sample.weight.blue;
	}
	sampler_destroy(rng);
	for (int i = 0; i < 3; ++i)
		mean[i] = sum[i] / (double)batches;
}

static int mean_close(const double mean[3], float r, float g, float b, double tolerance) {
	return close_double(mean[0], r, tolerance) &&
		   close_double(mean[1], g, tolerance) &&
		   close_double(mean[2], b, tolerance);
}

static int graphs_combination_math(void) {
	struct kit kit;
	kit_init(&kit, 201);
	struct lightRay ray;
	struct hitRecord hit = test_hit(&ray);
	const struct color da = rgb(0.20f, 0.30f, 0.40f);
	const struct color tb = rgb(0.50f, 0.25f, 0.10f);
	const struct color ic = rgb(0.10f, 0.40f, 0.80f);
	const struct color ec = rgb(1.00f, 0.20f, 0.00f);
	const struct bsdfNode *d = newDiffuse(&kit.storage, newConstantTexture(&kit.storage, da));
	const struct bsdfNode *t = newTranslucent(&kit.storage, newConstantTexture(&kit.storage, tb));
	const struct bsdfNode *iso = newIsotropic(&kit.storage, newConstantTexture(&kit.storage, ic));
	const struct bsdfNode *e = newEmission(&kit.storage, newConstantTexture(&kit.storage, ec), value_node(&kit, 2.0f));

	const float factor = 0.25f;
	const struct bsdfNode *mix_dt = newMix(&kit.storage, d, t, value_node(&kit, factor));
	const struct bsdfNode *add_dt = newAdd(&kit.storage, d, t);
	const struct bsdfNode *add_di = newAdd(&kit.storage, d, iso);
	const struct bsdfNode *add_dd = newAdd(&kit.storage, d, d);
	const struct bsdfNode *shared = newAdd(&kit.storage, newAdd(&kit.storage, d, t), d);
	const struct bsdfNode *mixed_emission = newMix(&kit.storage, e, d, value_node(&kit, factor));
	const struct bsdfNode *added_emission = newAdd(&kit.storage, e, d);
	const struct bsdfNode *nested = newAdd(&kit.storage,
		newMix(&kit.storage, e, d, value_node(&kit, 0.25f)),
		newMix(&kit.storage, t, iso, value_node(&kit, 0.75f)));

	const struct bsdfNode *all[] = {
		mix_dt, add_dt, add_di, add_dd, shared, mixed_emission, added_emission, nested
	};
	for (size_t i = 0; i < sizeof(all) / sizeof(all[0]); ++i)
		TASK_CHECK(all[i]->eval && all[i]->emission);
	TASK_CHECK(add_dd != d);
	TASK_CHECK(newAdd(&kit.storage, d, d) == add_dd); // hash-consing the identical graph is fine

	size_t dc;
	struct vector *dirs = test_directions(&dc);
	const struct vector n = hit.surfaceNormal;
	for (size_t k = 0; k < dc; ++k) {
		const struct vector dir = dirs[k];
		const float pd = expected_leaf_pdf(CLOSURE_DIFFUSE, n, dir);
		const float pt = expected_leaf_pdf(CLOSURE_TRANSLUCENT, n, dir);
		const float pi = expected_leaf_pdf(CLOSURE_ISOTROPIC, n, dir);
		float rd, gd, bd, rt, gt, bt, ri, gi, bi;
		expected_leaf_response(CLOSURE_DIFFUSE, da, n, dir, &rd, &gd, &bd);
		expected_leaf_response(CLOSURE_TRANSLUCENT, tb, n, dir, &rt, &gt, &bt);
		expected_leaf_response(CLOSURE_ISOTROPIC, ic, n, dir, &ri, &gi, &bi);

		TASK_CHECK(eval_close(mix_dt, kit.rng, &hit, dir,
			0.75f * rd + 0.25f * rt, 0.75f * gd + 0.25f * gt, 0.75f * bd + 0.25f * bt,
			0.75f * pd + 0.25f * pt, 2.0e-6f));
		TASK_CHECK(eval_close(add_dt, kit.rng, &hit, dir,
			rd + rt, gd + gt, bd + bt, 0.5f * (pd + pt), 2.0e-6f));
		TASK_CHECK(eval_close(add_di, kit.rng, &hit, dir,
			rd + ri, gd + gi, bd + bi, 0.5f * (pd + pi), 2.0e-6f));
		TASK_CHECK(eval_close(add_dd, kit.rng, &hit, dir,
			2.0f * rd, 2.0f * gd, 2.0f * bd, pd, 2.0e-6f));
		TASK_CHECK(eval_close(shared, kit.rng, &hit, dir,
			2.0f * rd + rt, 2.0f * gd + gt, 2.0f * bd + bt,
			0.75f * pd + 0.25f * pt, 2.0e-6f));
		TASK_CHECK(eval_close(nested, kit.rng, &hit, dir,
			0.75f * 0.0f + 0.25f * rd + 0.25f * rt + 0.75f * ri,
			0.25f * gd + 0.25f * gt + 0.75f * gi,
			0.25f * bd + 0.25f * bt + 0.75f * bi,
			0.5f * ((0.75f * pd + 0.25f * pd) + (0.25f * pt + 0.75f * pi)),
			3.0e-6f));
	}

	TASK_CHECK(emission_close(mixed_emission, kit.rng, &hit, 1.50f, 0.30f, 0.0f, 1.0e-6f));
	TASK_CHECK(emission_close(added_emission, kit.rng, &hit, 2.00f, 0.40f, 0.0f, 1.0e-6f));
	TASK_CHECK(emission_close(newAdd(&kit.storage, e, e), kit.rng, &hit, 4.00f, 0.80f, 0.0f, 1.0e-6f));
	TASK_CHECK(emission_close(nested, kit.rng, &hit, 1.50f, 0.30f, 0.0f, 1.0e-6f));

	TASK_CHECK(sample_batch_is_consistent(d, 301, 48));
	TASK_CHECK(sample_batch_is_consistent(t, 302, 48));
	TASK_CHECK(sample_batch_is_consistent(iso, 303, 48));
	TASK_CHECK(sample_batch_is_consistent(e, 304, 48));
	TASK_CHECK(sample_batch_is_consistent(mix_dt, 305, 64));
	TASK_CHECK(sample_batch_is_consistent(add_dt, 306, 64));
	TASK_CHECK(sample_batch_is_consistent(add_di, 310, 64));
	TASK_CHECK(sample_batch_is_consistent(add_dd, 307, 64));
	TASK_CHECK(sample_batch_is_consistent(shared, 308, 64));
	TASK_CHECK(sample_batch_is_consistent(nested, 309, 80));

	TASK_CHECK(sampler_consumes_dimensions(d, 401, 2));
	TASK_CHECK(sampler_consumes_dimensions(e, 402, 2));
	TASK_CHECK(sampler_consumes_dimensions(add_dt, 403, 3));
	TASK_CHECK(sampler_consumes_dimensions(mix_dt, 404, 3));
	kit_destroy(&kit);
	return 0;
}

static int graphs_fixed_batch_means(void) {
	struct kit kit;
	kit_init(&kit, 205);
	const struct color da = rgb(0.20f, 0.30f, 0.40f);
	const struct color tb = rgb(0.50f, 0.25f, 0.10f);
	const struct color ic = rgb(0.10f, 0.40f, 0.80f);
	const struct bsdfNode *d = newDiffuse(&kit.storage, newConstantTexture(&kit.storage, da));
	const struct bsdfNode *t = newTranslucent(&kit.storage, newConstantTexture(&kit.storage, tb));
	const struct bsdfNode *iso = newIsotropic(&kit.storage, newConstantTexture(&kit.storage, ic));
	const struct bsdfNode *e = newEmission(&kit.storage, color_node(&kit, 1.0f, 0.2f, 0.0f), value_node(&kit, 2.0f));
	const struct bsdfNode *mix_dt = newMix(&kit.storage, d, t, value_node(&kit, 0.25f));
	const struct bsdfNode *add_dt = newAdd(&kit.storage, d, t);
	const struct bsdfNode *add_di = newAdd(&kit.storage, d, iso);
	const struct bsdfNode *add_dd = newAdd(&kit.storage, d, d);
	const struct bsdfNode *shared = newAdd(&kit.storage, newAdd(&kit.storage, d, t), d);
	const struct bsdfNode *nested = newAdd(&kit.storage,
		newMix(&kit.storage, e, d, value_node(&kit, 0.25f)),
		newMix(&kit.storage, t, iso, value_node(&kit, 0.75f)));

	double mean[3];
	sample_mean_weight(d, 801, 8192, mean);
	TASK_CHECK(mean_close(mean, da.red, da.green, da.blue, 0.025));
	sample_mean_weight(t, 802, 8192, mean);
	TASK_CHECK(mean_close(mean, tb.red, tb.green, tb.blue, 0.025));
	sample_mean_weight(iso, 803, 8192, mean);
	TASK_CHECK(mean_close(mean, ic.red, ic.green, ic.blue, 0.025));
	sample_mean_weight(e, 804, 1024, mean);
	TASK_CHECK(mean_close(mean, 0.0f, 0.0f, 0.0f, 1.0e-7));
	sample_mean_weight(mix_dt, 805, 8192, mean);
	TASK_CHECK(mean_close(mean, 0.75f * da.red + 0.25f * tb.red,
		0.75f * da.green + 0.25f * tb.green,
		0.75f * da.blue + 0.25f * tb.blue, 0.035));
	sample_mean_weight(add_dt, 806, 16384, mean);
	TASK_CHECK(mean_close(mean, da.red + tb.red, da.green + tb.green, da.blue + tb.blue, 0.05));
	sample_mean_weight(add_di, 810, 16384, mean);
	TASK_CHECK(mean_close(mean, da.red + ic.red, da.green + ic.green, da.blue + ic.blue, 0.05));
	sample_mean_weight(add_dd, 807, 8192, mean);
	TASK_CHECK(mean_close(mean, 2.0f * da.red, 2.0f * da.green, 2.0f * da.blue, 0.05));
	sample_mean_weight(shared, 808, 16384, mean);
	TASK_CHECK(mean_close(mean, 2.0f * da.red + tb.red,
		2.0f * da.green + tb.green, 2.0f * da.blue + tb.blue, 0.06));
	sample_mean_weight(nested, 809, 16384, mean);
	TASK_CHECK(mean_close(mean,
		0.25f * da.red + 0.25f * tb.red + 0.75f * ic.red,
		0.25f * da.green + 0.25f * tb.green + 0.75f * ic.green,
		0.25f * da.blue + 0.25f * tb.blue + 0.75f * ic.blue, 0.06));
	kit_destroy(&kit);
	return 0;
}

static int graphs_mix_same_and_clamps(void) {
	struct kit kit;
	kit_init(&kit, 202);
	struct lightRay ray;
	struct hitRecord hit = test_hit(&ray);
	const struct color da = rgb(0.20f, 0.30f, 0.40f);
	const struct bsdfNode *d = newDiffuse(&kit.storage, newConstantTexture(&kit.storage, da));
	const struct bsdfNode *same = newMix(&kit.storage, d, d, value_node(&kit, 0.30f));
	const struct bsdfNode *clamped_low = newMix(&kit.storage, d, d, value_node(&kit, -2.0f));
	const struct bsdfNode *clamped_high = newMix(&kit.storage, d, d, value_node(&kit, 2.0f));
	TASK_CHECK(same == d); // Existing A == B mix pruning is value-preserving.
	TASK_CHECK(clamped_low == d && clamped_high == d);

	const struct bsdfNode *different = newMix(&kit.storage, d,
		newIsotropic(&kit.storage, color_node(&kit, 0.4f, 0.5f, 0.6f)),
		value_node(&kit, -1.0f));
	size_t dc;
	struct vector *dirs = test_directions(&dc);
	for (size_t i = 0; i < dc; ++i) {
		struct bsdfEval a = d->eval(d, kit.rng, &hit, dirs[i]);
		struct bsdfEval b = different->eval(different, kit.rng, &hit, dirs[i]);
		TASK_CHECK(nearf(b.response.red, a.response.red, 1.0e-6f));
		TASK_CHECK(nearf(b.pdf, a.pdf, 1.0e-6f));
	}
	TASK_CHECK(sample_batch_is_consistent(different, 501, 32));
	kit_destroy(&kit);
	return 0;
}

static int graphs_unsupported_mask_and_legacy_sample(void) {
	struct kit kit;
	kit_init(&kit, 203);
	struct lightRay ray;
	struct hitRecord hit = test_hit(&ray);
	const struct bsdfNode *d = newDiffuse(&kit.storage, color_node(&kit, 0.2f, 0.3f, 0.4f));
	const struct bsdfNode *metal = newMetal(&kit.storage, color_node(&kit, 0.7f, 0.7f, 0.7f), value_node(&kit, 0.0f));
	const struct bsdfNode *glass = newGlass(&kit.storage, color_node(&kit, 0.8f, 0.9f, 1.0f), value_node(&kit, 0.0f), value_node(&kit, 1.45f));
	const struct bsdfNode *plastic = newPlastic(&kit.storage, color_node(&kit, 0.6f, 0.2f, 0.1f), value_node(&kit, 0.0f), value_node(&kit, 1.45f));
	const struct bsdfNode *emissive = newEmission(&kit.storage, color_node(&kit, 1.0f, 0.25f, 0.0f), value_node(&kit, 2.0f));
	TASK_CHECK(!metal->eval && !metal->emission);
	TASK_CHECK(!glass->eval && !glass->emission);
	TASK_CHECK(!plastic->eval && !plastic->emission);

	const struct bsdfNode *bad_mix = newMix(&kit.storage, d, metal, value_node(&kit, 0.5f));
	const struct bsdfNode *bad_add = newAdd(&kit.storage, d, glass);
	const struct bsdfNode *bad_add_emission = newAdd(&kit.storage, emissive, metal);
	TASK_CHECK(!bad_mix->eval && !bad_mix->emission);
	TASK_CHECK(!bad_add->eval && !bad_add->emission);
	TASK_CHECK(!bad_add_emission->eval && !bad_add_emission->emission);
	TASK_CHECK(d->eval && d->emission);

	struct bsdfSample mix_sample = bad_mix->sample(bad_mix, kit.rng, &hit);
	struct bsdfSample add_sample = bad_add->sample(bad_add, kit.rng, &hit);
	struct bsdfSample add_emission_sample = bad_add_emission->sample(bad_add_emission, kit.rng, &hit);
	TASK_CHECK(isfinite(mix_sample.weight.red) && isfinite(mix_sample.out.direction.y));
	TASK_CHECK(isfinite(add_sample.weight.red) && isfinite(add_sample.out.direction.y));
	TASK_CHECK(near_rgb(add_emission_sample.emitted, 2.0f, 0.5f, 0.0f, 1.0e-6f));
	TASK_CHECK(sampler_consumes_dimensions(bad_add_emission, 205, 2));
	TASK_CHECK(sampler_consumes_dimensions(glass, 204, 1));
	kit_destroy(&kit);
	return 0;
}

static struct color trace_sphere(const struct bsdfNode *material, struct node_storage *storage,
								 sampler *rng, int max_bounces) {
	struct sphere *spheres = NULL;
	struct sphere sphere_value = { .radius = 1.0f, .rayOffset = 0.0f };
	v_arr_add(spheres, sphere_value);
	struct instance sphere_instance = new_sphere_instance(&spheres, 0, NULL, NULL);

	const struct bsdfNode *bsdfs[1] = { material };
	struct bsdf_buffer bbuf = { .bsdfs = bsdfs, .descriptions = NULL };
	sphere_instance.bbuf = &bbuf;

	struct instance *instances = NULL;
	v_arr_add(instances, sphere_instance);
	struct bvh *top_level = build_top_level_bvh(instances);

	struct world world = { 0 };
	world.spheres = spheres;
	world.instances = instances;
	world.topLevel = top_level;
	world.storage = *storage;
	world.background = newBackground(storage,
		newConstantTexture(storage, (struct color){ 0.0f, 0.0f, 0.0f, 1.0f }),
		newConstantValue(storage, 1.0f), NULL, false);

	struct lightRay camera_ray = {
		.start = { 0.0f, 0.0f, 3.0f },
		.direction = { 0.0f, 0.0f, -1.0f }
	};
	struct color result = path_trace(camera_ray, &world, max_bounces, rng);

	destroy_bvh(top_level);
	v_arr_free(instances);
	v_arr_free(spheres);
	return result;
}

static int next_random_equals_control(uint32_t seed, int already_consumed, float observed_next) {
	sampler *control = sampler_new();
	sampler_init(control, Random, 0, 1, seed);
	for (int i = 0; i < already_consumed; ++i)
		(void)sampler_dimension(control);
	const float expected_next = sampler_dimension(control);
	sampler_destroy(control);
	return observed_next == expected_next;
}

static int transport_emission_cutoff(void) {
	struct kit kit;
	kit_init(&kit, 601);
	const struct bsdfNode *emissive = newEmission(&kit.storage, color_node(&kit, 1.0f, 0.25f, 0.0f), value_node(&kit, 2.0f));
	sampler *rng = sampler_new();
	sampler_init(rng, Random, 0, 1, 701);
	struct color cutoff = trace_sphere(emissive, &kit.storage, rng, 0);
	const float next_after_cutoff = sampler_dimension(rng);
	sampler_destroy(rng);

	TASK_CHECK(near_rgb(cutoff, 2.0f, 0.5f, 0.0f, 1.0e-6f));
	TASK_CHECK(next_random_equals_control(701, 0, next_after_cutoff));

	rng = sampler_new();
	sampler_init(rng, Random, 0, 1, 702);
	struct color one_bounce = trace_sphere(emissive, &kit.storage, rng, 1);
	const float next_after_bounce = sampler_dimension(rng);
	sampler_destroy(rng);
	TASK_CHECK(near_rgb(one_bounce, 2.0f, 0.5f, 0.0f, 1.0e-6f));
	TASK_CHECK(next_random_equals_control(702, 2, next_after_bounce));
	kit_destroy(&kit);
	return 0;
}

static int transport_graph_random_use(void) {
	struct kit kit;
	kit_init(&kit, 602);
	const struct bsdfNode *d = newDiffuse(&kit.storage, color_node(&kit, 0.2f, 0.3f, 0.4f));
	const struct bsdfNode *e = newEmission(&kit.storage, color_node(&kit, 1.0f, 0.0f, 0.0f), value_node(&kit, 1.0f));
	const struct bsdfNode *add = newAdd(&kit.storage, e, d);
	const struct bsdfNode *mix = newMix(&kit.storage, e, d, value_node(&kit, 0.5f));

	sampler *add_rng = sampler_new();
	sampler_init(add_rng, Random, 0, 1, 703);
	struct color add_cutoff = trace_sphere(add, &kit.storage, add_rng, 0);
	const float add_next = sampler_dimension(add_rng);
	sampler_destroy(add_rng);
	TASK_CHECK(near_rgb(add_cutoff, 1.0f, 0.0f, 0.0f, 1.0e-6f));
	TASK_CHECK(next_random_equals_control(703, 0, add_next));

	add_rng = sampler_new();
	sampler_init(add_rng, Random, 0, 1, 704);
	(void)trace_sphere(add, &kit.storage, add_rng, 1);
	const float add_bounce_next = sampler_dimension(add_rng);
	sampler_destroy(add_rng);
	TASK_CHECK(next_random_equals_control(704, 3, add_bounce_next));

	sampler *mix_rng = sampler_new();
	sampler_init(mix_rng, Random, 0, 1, 705);
	(void)trace_sphere(mix, &kit.storage, mix_rng, 1);
	const float mix_bounce_next = sampler_dimension(mix_rng);
	sampler_destroy(mix_rng);
	TASK_CHECK(next_random_equals_control(705, 3, mix_bounce_next));
	kit_destroy(&kit);
	return 0;
}

static int transport_glass_legacy_control(void) {
	struct kit kit;
	kit_init(&kit, 603);
	const struct bsdfNode *glass = newGlass(&kit.storage,
		color_node(&kit, 0.8f, 0.9f, 1.0f), value_node(&kit, 0.0f), value_node(&kit, 1.45f));
	TASK_CHECK(!glass->eval && !glass->emission);

	sampler *rng = sampler_new();
	sampler_init(rng, Random, 0, 1, 706);
	struct color cutoff = trace_sphere(glass, &kit.storage, rng, 0);
	const float next = sampler_dimension(rng);
	sampler_destroy(rng);
	TASK_CHECK(near_rgb(cutoff, 0.0f, 0.0f, 0.0f, 1.0e-6f));
	// The unsupported path still calls sample() at max_bounces; smooth glass
	// consumes its one branch dimension there.
	TASK_CHECK(next_random_equals_control(706, 1, next));

	struct lightRay ray;
	struct hitRecord hit = test_hit(&ray);
	sampler *direct = sampler_new();
	sampler_init(direct, Random, 0, 1, 707);
	struct bsdfSample old_sample = glass->sample(glass, direct, &hit);
	TASK_CHECK(isfinite(old_sample.out.direction.x) && isfinite(old_sample.out.direction.y) &&
			   isfinite(old_sample.out.direction.z));
	TASK_CHECK(near_rgb(old_sample.weight, 0.8f, 0.9f, 1.0f, 1.0e-6f));
	sampler_destroy(direct);
	kit_destroy(&kit);
	return 0;
}

const struct task_case task_cases[] = {
	{ "density", "leaf_pointwise", density_leaf_pointwise },
	{ "density", "leaf_integrals", density_leaf_integrals },
	{ "density", "query_consumes_no_samples", density_query_consumes_no_samples },
	{ "graphs", "combination_math", graphs_combination_math },
	{ "graphs", "fixed_batch_means", graphs_fixed_batch_means },
	{ "graphs", "mix_same_and_clamps", graphs_mix_same_and_clamps },
	{ "graphs", "unsupported_mask_and_legacy_sample", graphs_unsupported_mask_and_legacy_sample },
	{ "transport", "emission_cutoff", transport_emission_cutoff },
	{ "transport", "graph_random_use", transport_graph_random_use },
	{ "transport", "glass_legacy_control", transport_glass_legacy_control }
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
