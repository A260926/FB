//
//  closures.c
//  c-ray
//
//  Regression tests for the closure combination path: full directional
//  densities, mix/add combination rules, shared leaves, self-emission and
//  the path_trace transport of supported closure graphs.
//

#include <stdio.h>
#include <math.h>
#include <string.h>
#include <stdint.h>

#include <common/mempool.h>
#include <common/hashtable.h>
#include <datatypes/scene.h>
#include <datatypes/sphere.h>
#include <nodes/bsdfnode.h>
#include <nodes/textures/constant.h>
#include <nodes/valuenode.h>
#include <renderer/samplers/sampler.h>
#include <renderer/pathtrace.h>
#include <renderer/instance.h>
#include <accelerators/bvh.h>

#include "task_support/check.h"

// ---------------------------------------------------------------------------
// Shared fixtures
// ---------------------------------------------------------------------------

static struct node_storage storage;
static int storage_ready;

static void setup_storage(void) {
	if (storage_ready) return;
	storage.node_pool = newBlock(NULL, 4096);
	storage.node_table = newHashtable(compareNodes, &storage.node_pool);
	storage_ready = 1;
}

static const struct colorNode *tex(float r, float g, float b) {
	return newConstantTexture(&storage, (struct color){ r, g, b, 1.0f });
}

static const struct bsdfNode *diff(float r, float g, float b) {
	return newDiffuse(&storage, tex(r, g, b));
}

static const struct bsdfNode *trans(float r, float g, float b) {
	return newTranslucent(&storage, tex(r, g, b));
}

static const struct bsdfNode *iso(float r, float g, float b) {
	return newIsotropic(&storage, tex(r, g, b));
}

static const struct bsdfNode *emit(float r, float g, float b, float strength) {
	return newEmission(&storage, tex(r, g, b), newConstantValue(&storage, strength));
}

static struct hitRecord make_hit(struct vector normal) {
	static struct lightRay incident;
	incident = (struct lightRay){ .start = { 0.0f, 0.0f, 0.0f }, .direction = { 0.0f, 1.0f, 0.0f } };
	struct hitRecord hit = { 0 };
	hit.incident = &incident;
	hit.hitPoint = (struct vector){ 0.0f, 0.0f, 0.0f };
	hit.surfaceNormal = normal;
	return hit;
}

static int closef(float a, float b, float tol) {
	return fabsf(a - b) <= tol;
}

static int close3(struct color a, struct color b, float tol) {
	return closef(a.red, b.red, tol) && closef(a.green, b.green, tol) && closef(a.blue, b.blue, tol);
}

static int finite3(struct color c) {
	return isfinite(c.red) && isfinite(c.green) && isfinite(c.blue);
}

static int black3(struct color c) {
	return c.red == 0.0f && c.green == 0.0f && c.blue == 0.0f;
}

// Independent deterministic direction stream, deliberately not using the
// production sampler, so directional integration is cross-checked separately.
static uint32_t lcg_state;

static void lcg_seed(uint32_t seed) {
	lcg_state = seed;
}

static float lcg_unit(void) {
	lcg_state = lcg_state * 1664525u + 1013904223u;
	return (float)(lcg_state >> 8) * (1.0f / 16777216.0f);
}

static struct vector uniform_sphere(float u1, float u2) {
	const float z = 1.0f - 2.0f * u2;
	const float r = sqrtf(fmaxf(0.0f, 1.0f - z * z));
	const float phi = 2.0f * (float)PI * u1;
	return (struct vector){ r * cosf(phi), r * sinf(phi), z };
}

struct integ_result {
	float pdf;
	struct color response;
};

// Integrates pdf and response over the full unit sphere with a fixed batch
// of independent uniform directions (uniform sphere density 1/(4*pi)).
static struct integ_result integrate(const struct bsdfNode *node, const struct hitRecord *hit, int count) {
	sampler *s = sampler_new();
	sampler_init(s, Random, 0, 1, 1234);
	double pdf_sum = 0.0;
	double r_sum = 0.0, g_sum = 0.0, b_sum = 0.0;
	for (int i = 0; i < count; ++i) {
		const struct vector d = uniform_sphere(lcg_unit(), lcg_unit());
		const struct bsdfEval e = node->eval(node, s, hit, d);
		pdf_sum += e.pdf;
		r_sum += e.response.red;
		g_sum += e.response.green;
		b_sum += e.response.blue;
	}
	sampler_destroy(s);
	const float scale = 4.0f * (float)PI / (float)count;
	return (struct integ_result){
		.pdf = (float)(pdf_sum * scale),
		.response = { (float)(r_sum * scale), (float)(g_sum * scale), (float)(b_sum * scale), 1.0f }
	};
}

#define BATCH 16384
#define MC_TOL 0.08f

// Checks that, over a fixed sampled batch, sample()'s pdf and weight match
// the independent eval() query at the sampled outgoing direction.
static int sampled_matches_eval(const struct bsdfNode *node, const struct hitRecord *hit, int batches) {
	sampler *draw = sampler_new();
	sampler *query = sampler_new();
	sampler_init(draw, Random, 0, 1, 99);
	sampler_init(query, Random, 0, 1, 777);
	for (int i = 0; i < batches; ++i) {
		const struct bsdfSample sm = node->sample(node, draw, hit);
		if (!isfinite(sm.pdf) || !finite3(sm.weight) || !finite3(sm.emitted)) {
			sampler_destroy(draw);
			sampler_destroy(query);
			return 0;
		}
		const struct bsdfEval e = node->eval(node, query, hit, sm.out.direction);
		if (!closef(sm.pdf, e.pdf, 1e-5f)) {
			sampler_destroy(draw);
			sampler_destroy(query);
			return 0;
		}
		const struct color expect = e.pdf > 0.0f ? colorCoef(1.0f / e.pdf, e.response) : g_black_color;
		if (!close3(sm.weight, expect, 1e-5f)) {
			sampler_destroy(draw);
			sampler_destroy(query);
			return 0;
		}
	}
	sampler_destroy(draw);
	sampler_destroy(query);
	return 1;
}

// ---------------------------------------------------------------------------
// density group: leaf forms and full-sphere integration
// ---------------------------------------------------------------------------

static int test_leaf_forms(void) {
	setup_storage();
	const struct vector N = { 0.0f, 1.0f, 0.0f };
	const struct vector up = { 0.0f, 1.0f, 0.0f };
	const struct vector down = { 0.0f, -1.0f, 0.0f };
	struct hitRecord hit = make_hit(N);
	sampler *s = sampler_new();
	sampler_init(s, Random, 0, 1, 5);

	const struct color cd = { 0.2f, 0.3f, 0.4f, 1.0f };
	const struct bsdfNode *d = diff(cd.red, cd.green, cd.blue);

	// diffuse: color * max(N.d,0) / pi, pdf is the same density without color.
	struct bsdfEval e = d->eval(d, s, &hit, up);
	TASK_CHECK(closef(e.pdf, 1.0f / (float)PI, 1e-6f));
	TASK_CHECK(close3(e.response, colorCoef(1.0f / (float)PI, cd), 1e-6f));
	e = d->eval(d, s, &hit, down);
	TASK_CHECK(e.pdf == 0.0f && black3(e.response));

	// translucent: reflection domain flipped to the transmission hemisphere.
	const struct color ct = { 0.5f, 0.6f, 0.7f, 1.0f };
	const struct bsdfNode *t = trans(ct.red, ct.green, ct.blue);
	e = t->eval(t, s, &hit, down);
	TASK_CHECK(closef(e.pdf, 1.0f / (float)PI, 1e-6f));
	TASK_CHECK(close3(e.response, colorCoef(1.0f / (float)PI, ct), 1e-6f));
	e = t->eval(t, s, &hit, up);
	TASK_CHECK(e.pdf == 0.0f && black3(e.response));

	// isotropic: color / (4*pi) everywhere on the sphere.
	const struct color ci = { 0.8f, 0.1f, 0.2f, 1.0f };
	const struct bsdfNode *i = iso(ci.red, ci.green, ci.blue);
	const struct vector sideways = { 1.0f, 0.0f, 0.0f };
	e = i->eval(i, s, &hit, sideways);
	TASK_CHECK(closef(e.pdf, 1.0f / (4.0f * (float)PI), 1e-6f));
	TASK_CHECK(close3(e.response, colorCoef(1.0f / (4.0f * (float)PI), ci), 1e-6f));
	e = i->eval(i, s, &hit, up);
	TASK_CHECK(closef(e.pdf, 1.0f / (4.0f * (float)PI), 1e-6f));

	// emission: zero scattering response, diffuse-like cosine hemisphere pdf.
	const struct bsdfNode *em = emit(1.0f, 0.0f, 0.0f, 2.0f);
	e = em->eval(em, s, &hit, up);
	TASK_CHECK(black3(e.response) && closef(e.pdf, 1.0f / (float)PI, 1e-6f));
	e = em->eval(em, s, &hit, down);
	TASK_CHECK(black3(e.response) && e.pdf == 0.0f);
	const struct color own = em->emission(em, s, &hit);
	TASK_CHECK(close3(own, (struct color){ 2.0f, 0.0f, 0.0f, 1.0f }, 1e-6f));
	TASK_CHECK(black3(d->emission(d, s, &hit)));
	TASK_CHECK(black3(t->emission(t, s, &hit)));
	TASK_CHECK(black3(i->emission(i, s, &hit)));

	sampler_destroy(s);
	return 0;
}

static int test_leaf_integrals(void) {
	setup_storage();
	lcg_seed(0xabcdef01u);
	struct hitRecord hit = make_hit((struct vector){ 0.0f, 1.0f, 0.0f });

	const struct color cd = { 0.2f, 0.3f, 0.4f, 1.0f };
	struct integ_result r = integrate(diff(cd.red, cd.green, cd.blue), &hit, BATCH);
	TASK_CHECK(closef(r.pdf, 1.0f, MC_TOL));
	TASK_CHECK(close3(r.response, cd, MC_TOL));

	const struct color ct = { 0.5f, 0.6f, 0.7f, 1.0f };
	r = integrate(trans(ct.red, ct.green, ct.blue), &hit, BATCH);
	TASK_CHECK(closef(r.pdf, 1.0f, MC_TOL));
	TASK_CHECK(close3(r.response, ct, MC_TOL));

	const struct color ci = { 0.8f, 0.1f, 0.2f, 1.0f };
	r = integrate(iso(ci.red, ci.green, ci.blue), &hit, BATCH);
	TASK_CHECK(closef(r.pdf, 1.0f, MC_TOL));
	TASK_CHECK(close3(r.response, ci, MC_TOL));

	// Emission integrates as a normalized pdf with no scattering response.
	r = integrate(emit(1.0f, 1.0f, 1.0f, 3.0f), &hit, BATCH);
	TASK_CHECK(closef(r.pdf, 1.0f, MC_TOL));
	TASK_CHECK(black3(r.response));
	return 0;
}

static int test_combination_integrals(void) {
	setup_storage();
	lcg_seed(0x0badc0deu);
	struct hitRecord hit = make_hit((struct vector){ 0.0f, 1.0f, 0.0f });

	const struct bsdfNode *a = diff(0.2f, 0.3f, 0.4f);
	const struct bsdfNode *b = diff(0.5f, 0.1f, 0.7f);

	// add: pdf is the mean of the child densities, responses add.
	const struct bsdfNode *sum = newAdd(&storage, a, b);
	struct integ_result r = integrate(sum, &hit, BATCH);
	TASK_CHECK(closef(r.pdf, 1.0f, MC_TOL));
	TASK_CHECK(close3(r.response, colorAdd((struct color){ 0.2f, 0.3f, 0.4f, 1 }, (struct color){ 0.5f, 0.1f, 0.7f, 1 }), MC_TOL));

	// add(A, A): the shared closure contributes twice, density stays valid.
	const struct bsdfNode *doubled = newAdd(&storage, a, a);
	r = integrate(doubled, &hit, BATCH);
	TASK_CHECK(closef(r.pdf, 1.0f, MC_TOL));
	TASK_CHECK(close3(r.response, colorCoef(2.0f, (struct color){ 0.2f, 0.3f, 0.4f, 1 }), MC_TOL));

	// mix: factor-weighted density and response.
	const struct bsdfNode *mx = newMix(&storage, a, b, newConstantValue(&storage, 0.25f));
	r = integrate(mx, &hit, BATCH);
	TASK_CHECK(closef(r.pdf, 1.0f, MC_TOL));
	TASK_CHECK(close3(r.response, colorAdd(colorCoef(0.75f, (struct color){ 0.2f, 0.3f, 0.4f, 1 }),
	                                         colorCoef(0.25f, (struct color){ 0.5f, 0.1f, 0.7f, 1 })), MC_TOL));

	// Reflection/transmission domains combined keep a normalized full density.
	const struct bsdfNode *domains = newMix(&storage, diff(0.3f, 0.3f, 0.3f), trans(0.6f, 0.6f, 0.6f), newConstantValue(&storage, 0.5f));
	r = integrate(domains, &hit, BATCH);
	TASK_CHECK(closef(r.pdf, 1.0f, MC_TOL));
	TASK_CHECK(close3(r.response, (struct color){ 0.45f, 0.45f, 0.45f, 1.0f }, MC_TOL));

	// isotropic overlapping the diffuse hemisphere.
	const struct bsdfNode *overlap = newAdd(&storage, iso(0.4f, 0.4f, 0.4f), diff(0.2f, 0.2f, 0.2f));
	r = integrate(overlap, &hit, BATCH);
	TASK_CHECK(closef(r.pdf, 1.0f, MC_TOL));
	TASK_CHECK(close3(r.response, (struct color){ 0.6f, 0.6f, 0.6f, 1.0f }, MC_TOL));
	return 0;
}

// ---------------------------------------------------------------------------
// graphs group: combination rules, shared leaves and sampled consistency
// ---------------------------------------------------------------------------

static int test_sampled_consistency(void) {
	setup_storage();
	struct hitRecord hit = make_hit((struct vector){ 0.0f, 1.0f, 0.0f });

	TASK_CHECK(sampled_matches_eval(diff(0.2f, 0.7f, 0.4f), &hit, 256));
	TASK_CHECK(sampled_matches_eval(trans(0.3f, 0.5f, 0.9f), &hit, 256));
	TASK_CHECK(sampled_matches_eval(iso(0.6f, 0.2f, 0.1f), &hit, 256));

	const struct bsdfNode *a = diff(0.2f, 0.3f, 0.4f);
	const struct bsdfNode *b = diff(0.5f, 0.1f, 0.7f);
	TASK_CHECK(sampled_matches_eval(newAdd(&storage, a, b), &hit, 256));
	TASK_CHECK(sampled_matches_eval(newMix(&storage, a, b, newConstantValue(&storage, 0.3f)), &hit, 256));

	// Nested mix/add graph.
	const struct bsdfNode *nested =
		newAdd(&storage,
			newMix(&storage, a, trans(0.1f, 0.2f, 0.3f), newConstantValue(&storage, 0.4f)),
			newMix(&storage, iso(0.2f, 0.2f, 0.2f), emit(1.0f, 1.0f, 1.0f, 2.0f), newConstantValue(&storage, 0.6f)));
	TASK_CHECK(nested->eval != NULL && nested->emission != NULL);
	TASK_CHECK(sampled_matches_eval(nested, &hit, 256));
	return 0;
}

static int test_shared_and_same_leaves(void) {
	setup_storage();
	const struct vector N = { 0.0f, 1.0f, 0.0f };
	struct hitRecord hit = make_hit(N);
	sampler *s = sampler_new();
	sampler_init(s, Random, 0, 1, 11);

	const struct bsdfNode *shared = diff(0.2f, 0.4f, 0.6f);
	const struct bsdfNode *other = diff(0.1f, 0.1f, 0.1f);

	// The same leaf pointer is reached through both add paths and one mix arm.
	const struct bsdfNode *graph = newAdd(&storage, shared,
		newMix(&storage, shared, other, newConstantValue(&storage, 0.5f)));
	const struct bsdfEval e = graph->eval(graph, s, &hit, N);
	// response = r_shared + (0.5*r_shared + 0.5*r_other)
	const struct color expected = colorAdd(
		colorCoef(1.5f, colorCoef(1.0f / (float)PI, (struct color){ 0.2f, 0.4f, 0.6f, 1 })),
		colorCoef(0.5f, colorCoef(1.0f / (float)PI, (struct color){ 0.1f, 0.1f, 0.1f, 1 })));
	TASK_CHECK(close3(e.response, expected, 1e-6f));
	// Every component is diffuse, so at d=N the marginal pdf stays 1/pi.
	TASK_CHECK(closef(e.pdf, 1.0f / (float)PI, 1e-6f));
	TASK_CHECK(sampled_matches_eval(graph, &hit, 256));

	// add(A, A) doubles scattering and emission, no pointer deduplication.
	const struct bsdfNode *em = emit(1.0f, 0.5f, 0.25f, 2.0f);
	const struct bsdfNode *em2 = newAdd(&storage, em, em);
	TASK_CHECK(close3(em2->emission(em2, s, &hit), (struct color){ 4.0f, 2.0f, 1.0f, 1.0f }, 1e-6f));
	const struct bsdfNode *d2 = newAdd(&storage, shared, shared);
	const struct bsdfEval ed = d2->eval(d2, s, &hit, N);
	TASK_CHECK(close3(ed.response, colorCoef(2.0f / (float)PI, (struct color){ 0.2f, 0.4f, 0.6f, 1 }), 1e-6f));
	TASK_CHECK(closef(ed.pdf, 1.0f / (float)PI, 1e-6f));

	// mix(A, A) preserves the original closure, including its emission.
	const struct bsdfNode *ms = newMix(&storage, em, em, newConstantValue(&storage, 0.3f));
	TASK_CHECK(close3(ms->emission(ms, s, &hit), (struct color){ 2.0f, 1.0f, 0.5f, 1.0f }, 1e-6f));
	const struct bsdfNode *md = newMix(&storage, shared, shared, newConstantValue(&storage, 0.3f));
	const struct bsdfEval emd = md->eval(md, s, &hit, N);
	TASK_CHECK(close3(emd.response, colorCoef(1.0f / (float)PI, (struct color){ 0.2f, 0.4f, 0.6f, 1 }), 1e-6f));
	TASK_CHECK(closef(emd.pdf, 1.0f / (float)PI, 1e-6f));

	sampler_destroy(s);
	return 0;
}

static int test_mix_factor_and_emission(void) {
	setup_storage();
	const struct vector N = { 0.0f, 1.0f, 0.0f };
	struct hitRecord hit = make_hit(N);
	sampler *s = sampler_new();
	sampler_init(s, Random, 0, 1, 13);

	const struct bsdfNode *a = diff(0.2f, 0.3f, 0.4f);
	const struct bsdfNode *b = diff(0.5f, 0.1f, 0.7f);

	// Finite out-of-range factors are clamped to [0,1].
	const struct bsdfNode *mix_hi = newMix(&storage, a, b, newConstantValue(&storage, 2.0f));
	const struct bsdfNode *mix_lo = newMix(&storage, a, b, newConstantValue(&storage, -1.0f));
	struct bsdfEval hi = mix_hi->eval(mix_hi, s, &hit, N);
	struct bsdfEval lo = mix_lo->eval(mix_lo, s, &hit, N);
	TASK_CHECK(close3(hi.response, colorCoef(1.0f / (float)PI, (struct color){ 0.5f, 0.1f, 0.7f, 1 }), 1e-6f));
	TASK_CHECK(close3(lo.response, colorCoef(1.0f / (float)PI, (struct color){ 0.2f, 0.3f, 0.4f, 1 }), 1e-6f));

	// Nested mixed emission is combined with the same weights.
	const struct bsdfNode *e1 = emit(1.0f, 0.0f, 0.0f, 2.0f); // (2, 0, 0)
	const struct bsdfNode *e2 = emit(0.0f, 1.0f, 0.0f, 3.0f); // (0, 3, 0)
	const struct bsdfNode *e3 = emit(0.0f, 0.0f, 1.0f, 1.0f); // (0, 0, 1)
	const struct bsdfNode *mixed = newMix(&storage, e1, e2, newConstantValue(&storage, 0.25f));
	const struct bsdfNode *total = newAdd(&storage, mixed, e3);
	TASK_CHECK(close3(total->emission(total, s, &hit), (struct color){ 1.5f, 0.75f, 1.0f, 1.0f }, 1e-6f));
	// Emission leaves carry no scattering response on their own.
	const struct bsdfNode *emit_sum = newAdd(&storage, e1, e2);
	const struct bsdfEval only_emit = emit_sum->eval(emit_sum, s, &hit, N);
	TASK_CHECK(black3(only_emit.response) && closef(only_emit.pdf, 1.0f / (float)PI, 1e-6f));

	// eval/emission queries do not draw scattering random numbers: after the
	// queries, this sampler must still match a freshly seeded reference stream.
	const struct bsdfNode *graph = newAdd(&storage, mixed, newMix(&storage, a, b, newConstantValue(&storage, 0.6f)));
	sampler *ref = sampler_new();
	sampler_init(ref, Random, 0, 1, 13);
	for (int k = 0; k < 32; ++k) {
		graph->eval(graph, s, &hit, uniform_sphere(lcg_unit(), lcg_unit()));
		graph->emission(graph, s, &hit);
	}
	TASK_CHECK(sampler_dimension(s) == sampler_dimension(ref));

	sampler_destroy(ref);
	sampler_destroy(s);
	return 0;
}

static int test_unsupported_leaves(void) {
	setup_storage();
	struct hitRecord hit = make_hit((struct vector){ 0.0f, 1.0f, 0.0f });
	sampler *s = sampler_new();
	sampler_init(s, Random, 0, 1, 17);

	// Out-of-range leaves provide neither callback and keep sample() working.
	const struct bsdfNode *metal = newMetal(&storage, tex(0.9f, 0.9f, 0.9f), newConstantValue(&storage, 0.0f));
	const struct bsdfNode *glass = newGlass(&storage, tex(1.0f, 1.0f, 1.0f), newConstantValue(&storage, 0.0f), newConstantValue(&storage, 1.45f));
	const struct bsdfNode *plastic = newPlastic(&storage, tex(0.8f, 0.2f, 0.2f), newConstantValue(&storage, 0.0f), newConstantValue(&storage, 1.45f));
	const struct bsdfNode *plain = newTransparent(&storage, tex(1.0f, 1.0f, 1.0f));
	TASK_CHECK(metal->eval == NULL && metal->emission == NULL);
	TASK_CHECK(glass->eval == NULL && glass->emission == NULL);
	TASK_CHECK(plastic->eval == NULL && plastic->emission == NULL);
	TASK_CHECK(plain->eval == NULL && plain->emission == NULL);

	// A single unsupported leaf anywhere in the graph disables both callbacks.
	const struct bsdfNode *bad_add = newAdd(&storage, metal, diff(0.2f, 0.2f, 0.2f));
	const struct bsdfNode *bad_mix = newMix(&storage, diff(0.2f, 0.2f, 0.2f), glass, newConstantValue(&storage, 0.5f));
	TASK_CHECK(bad_add->eval == NULL && bad_add->emission == NULL);
	TASK_CHECK(bad_mix->eval == NULL && bad_mix->emission == NULL);

	// The legacy branch rules still produce finite samples and weights.
	const struct bsdfSample sa = bad_add->sample(bad_add, s, &hit);
	const struct bsdfSample sm = bad_mix->sample(bad_mix, s, &hit);
	TASK_CHECK(isfinite(sa.pdf) && finite3(sa.weight));
	TASK_CHECK(isfinite(sm.pdf) && finite3(sm.weight));
	TASK_CHECK(close3(sa.weight, colorAdd((struct color){ 0.9f, 0.9f, 0.9f, 1 }, (struct color){ 0.2f, 0.2f, 0.2f, 1 }), 1e-6f));

	sampler_destroy(s);
	return 0;
}

// ---------------------------------------------------------------------------
// transport group: path_trace behavior on real scenes
// ---------------------------------------------------------------------------

struct scene_fixture {
	struct world world;
	struct bsdf_buffer bbuf;
	const struct bsdfNode *bsdfs[1];
	struct sphere *spheres;
	struct instance *instances;
};

static void build_scene(struct scene_fixture *f, const struct bsdfNode *surface, const struct bsdfNode *background, float radius) {
	memset(f, 0, sizeof(*f));
	f->bsdfs[0] = surface;
	f->bbuf.bsdfs = (const struct bsdfNode **)f->bsdfs;
	v_arr_add(f->spheres, ((struct sphere){ .radius = radius }));
	struct instance inst = new_sphere_instance(&f->spheres, 0, NULL, NULL);
	inst.bbuf = &f->bbuf;
	v_arr_add(f->instances, inst);
	f->world.spheres = f->spheres;
	f->world.instances = f->instances;
	f->world.topLevel = build_top_level_bvh(f->instances);
	f->world.background = background;
}

static void free_scene(struct scene_fixture *f) {
	destroy_bvh(f->world.topLevel);
	v_arr_free(f->spheres);
	v_arr_free(f->instances);
}

static const struct bsdfNode *black_background(void) {
	return newBackground(&storage, tex(0.0f, 0.0f, 0.0f), newConstantValue(&storage, 1.0f),
		newConstantVector(&storage, (struct vector){ 0.0f, 0.0f, 0.0f }), false);
}

static struct lightRay down_ray(void) {
	return (struct lightRay){ .start = { 0.0f, 2.0f, 0.0f }, .direction = { 0.0f, -1.0f, 0.0f }, .type = rt_camera };
}

// Reference random stream of a fresh sampler, used to count how many numbers
// a path_trace run actually consumed.
static void reference_stream(float *out, int count, uint32_t pixel) {
	sampler *s = sampler_new();
	sampler_init(s, Random, 0, 1, pixel);
	for (int i = 0; i < count; ++i) out[i] = sampler_dimension(s);
	sampler_destroy(s);
}

static int test_emission_cutoff(void) {
	setup_storage();
	struct scene_fixture f;
	// Pure emitter, bounce budget exhausted at the hit itself.
	build_scene(&f, emit(1.0f, 0.0f, 0.0f, 2.0f), black_background(), 1.0f);

	float ref[16];
	reference_stream(ref, 16, 31);
	sampler *s = sampler_new();
	sampler_init(s, Random, 0, 1, 31);
	const struct color result = path_trace(down_ray(), &f.world, 0, s);
	// Emission is added once and no scattering draw happens at the cutoff.
	TASK_CHECK(close3(result, (struct color){ 2.0f, 0.0f, 0.0f, 1.0f }, 1e-6f));
	TASK_CHECK(sampler_dimension(s) == ref[0]);
	sampler_destroy(s);
	free_scene(&f);
	return 0;
}

static int test_mixed_emission_cutoff(void) {
	setup_storage();
	struct scene_fixture f;
	const struct bsdfNode *surface =
		newMix(&storage, emit(1.0f, 0.0f, 0.0f, 1.0f), emit(0.0f, 1.0f, 0.0f, 1.0f), newConstantValue(&storage, 0.5f));
	build_scene(&f, surface, black_background(), 1.0f);

	float ref[16];
	reference_stream(ref, 16, 32);
	sampler *s = sampler_new();
	sampler_init(s, Random, 0, 1, 32);
	const struct color result = path_trace(down_ray(), &f.world, 0, s);
	// Independent emission query through the combiner, still zero random draws.
	TASK_CHECK(close3(result, (struct color){ 0.5f, 0.5f, 0.0f, 1.0f }, 1e-6f));
	TASK_CHECK(sampler_dimension(s) == ref[0]);
	sampler_destroy(s);
	free_scene(&f);
	return 0;
}

static int test_scatter_draw_count(void) {
	setup_storage();
	struct scene_fixture f;
	build_scene(&f, diff(1.0f, 1.0f, 1.0f), black_background(), 1.0f);

	float ref[16];
	reference_stream(ref, 16, 33);
	sampler *s = sampler_new();
	sampler_init(s, Random, 0, 1, 33);
	// Bounce 0 scatters (two sphere-sampling dimensions), bounce 1 misses into
	// the black background, so exactly two numbers are consumed.
	const struct color result = path_trace(down_ray(), &f.world, 1, s);
	TASK_CHECK(black3(result));
	TASK_CHECK(sampler_dimension(s) == ref[2]);
	sampler_destroy(s);
	free_scene(&f);

	// A mix adds one branch dimension on top of the child's two.
	build_scene(&f, newMix(&storage, diff(1.0f, 1.0f, 1.0f), diff(0.5f, 0.5f, 0.5f), newConstantValue(&storage, 0.5f)), black_background(), 1.0f);
	reference_stream(ref, 16, 34);
	s = sampler_new();
	sampler_init(s, Random, 0, 1, 34);
	path_trace(down_ray(), &f.world, 1, s);
	TASK_CHECK(sampler_dimension(s) == ref[3]);
	sampler_destroy(s);
	free_scene(&f);
	return 0;
}

static int test_emission_once_per_hit(void) {
	setup_storage();
	struct scene_fixture f;
	// Two consecutive hit events on an add(emission, translucent): the path
	// integrator adds the self-emission once per hit, so two hits double it.
	const struct bsdfNode *surface = newAdd(&storage, emit(1.0f, 0.0f, 0.0f, 1.0f), trans(0.9f, 0.9f, 0.9f));
	struct hitRecord hit = make_hit((struct vector){ 0.0f, 1.0f, 0.0f });
	sampler *s = sampler_new();
	sampler_init(s, Random, 0, 1, 35);

	struct color radiance = g_black_color;
	// Each hit adds the graph emission exactly once at unit path throughput;
	// two hit events therefore accumulate two single-hit contributions.
	for (int bounce = 0; bounce < 2; ++bounce) {
		radiance = colorAdd(radiance, surface->emission(surface, s, &hit));
	}
	TASK_CHECK(close3(radiance, (struct color){ 2.0f, 0.0f, 0.0f, 1.0f }, 1e-6f));
	// One sample carries the single-hit emission, never a doubled value.
	const struct bsdfSample sm = surface->sample(surface, s, &hit);
	TASK_CHECK(close3(sm.emitted, (struct color){ 1.0f, 0.0f, 0.0f, 1.0f }, 1e-6f));
	TASK_CHECK(finite3(sm.weight) && isfinite(sm.pdf));
	sampler_destroy(s);

	// Real path_trace inside an enclosing sphere: the first hit either ends on
	// the emission leaf (one contribution) or transmits through the interior
	// and hits the wall again. In the transmission hemisphere only the
	// translucent child has density, so the graph pdf is half of the leaf pdf
	// and the correct marginal weight doubles the 0.9 albedo: the second
	// contribution is 1.8, for a total of 2.8. Both outcomes must occur.
	build_scene(&f, surface, black_background(), 10.0f);
	int ones = 0, continued = 0;
	for (uint32_t pixel = 100; pixel < 164; ++pixel) {
		sampler *b = sampler_new();
		sampler_init(b, Random, 0, 1, pixel);
		const struct color r = path_trace(down_ray(), &f.world, 1, b);
		TASK_CHECK(r.green == 0.0f && r.blue == 0.0f && finite3(r));
		if (closef(r.red, 1.0f, 1e-6f)) ++ones;
		else if (closef(r.red, 2.8f, 1e-6f)) ++continued; // 1.0 + (0.9 / 0.5) * 1.0
		else TASK_CHECK(0);
		sampler_destroy(b);
	}
	TASK_CHECK(ones > 0 && continued > 0);
	free_scene(&f);

	// Zero scattering response ends the path; a single hit still emits exactly
	// once instead of leaking through the sample path twice.
	build_scene(&f, emit(1.0f, 0.0f, 0.0f, 1.0f), black_background(), 1.0f);
	s = sampler_new();
	sampler_init(s, Random, 0, 1, 36);
	const struct color single = path_trace(down_ray(), &f.world, 5, s);
	TASK_CHECK(close3(single, (struct color){ 1.0f, 0.0f, 0.0f, 1.0f }, 1e-6f));
	sampler_destroy(s);
	free_scene(&f);
	return 0;
}

static int test_zero_response_terminates(void) {
	setup_storage();
	struct scene_fixture f;
	const struct bsdfNode *surface = newAdd(&storage, emit(1.0f, 0.0f, 0.0f, 2.0f), diff(0.0f, 0.0f, 0.0f));
	build_scene(&f, surface, black_background(), 1.0f);

	sampler *s = sampler_new();
	sampler_init(s, Random, 0, 1, 37);
	const struct color result = path_trace(down_ray(), &f.world, 2, s);
	// Emission survives, the black scattering closure terminates without NaN.
	TASK_CHECK(close3(result, (struct color){ 2.0f, 0.0f, 0.0f, 1.0f }, 1e-6f));
	sampler_destroy(s);
	free_scene(&f);
	return 0;
}

static int test_background_miss(void) {
	setup_storage();
	struct scene_fixture f;
	const struct bsdfNode *bg = newBackground(&storage, tex(0.1f, 0.2f, 0.3f), newConstantValue(&storage, 1.0f),
		newConstantVector(&storage, (struct vector){ 0.0f, 0.0f, 0.0f }), false);
	build_scene(&f, diff(1.0f, 1.0f, 1.0f), bg, 1.0f);

	float ref[16];
	reference_stream(ref, 16, 38);
	sampler *s = sampler_new();
	sampler_init(s, Random, 0, 1, 38);
	const struct lightRay miss = { .start = { 0.0f, 2.0f, 0.0f }, .direction = { 1.0f, 0.0f, 0.0f }, .type = rt_camera };
	const struct color result = path_trace(miss, &f.world, 0, s);
	TASK_CHECK(close3(result, (struct color){ 0.1f, 0.2f, 0.3f, 1.0f }, 1e-6f));
	// Missing into a constant background consumes no random numbers.
	TASK_CHECK(sampler_dimension(s) == ref[0]);
	sampler_destroy(s);
	free_scene(&f);
	return 0;
}

static int test_legacy_metal_glass(void) {
	setup_storage();
	struct scene_fixture f;

	// Metal keeps its old path: no callbacks, no randoms at roughness zero.
	const struct bsdfNode *metal = newMetal(&storage, tex(0.9f, 0.9f, 0.9f), newConstantValue(&storage, 0.0f));
	build_scene(&f, metal, black_background(), 1.0f);
	float ref[16];
	reference_stream(ref, 16, 39);
	sampler *s = sampler_new();
	sampler_init(s, Random, 0, 1, 39);
	TASK_CHECK(black3(path_trace(down_ray(), &f.world, 0, s)));
	TASK_CHECK(sampler_dimension(s) == ref[0]);
	sampler_destroy(s);
	free_scene(&f);

	// Glass keeps its old path: even at the cutoff it draws its fresnel branch.
	const struct bsdfNode *glass = newGlass(&storage, tex(1.0f, 1.0f, 1.0f), newConstantValue(&storage, 0.0f), newConstantValue(&storage, 1.45f));
	build_scene(&f, glass, black_background(), 1.0f);
	reference_stream(ref, 16, 40);
	s = sampler_new();
	sampler_init(s, Random, 0, 1, 40);
	TASK_CHECK(black3(path_trace(down_ray(), &f.world, 0, s)));
	TASK_CHECK(sampler_dimension(s) == ref[1]);
	sampler_destroy(s);
	free_scene(&f);

	// An unsupported combined graph still transports through sample/emitted.
	const struct bsdfNode *legacy = newAdd(&storage, metal, diff(0.2f, 0.2f, 0.2f));
	build_scene(&f, legacy, black_background(), 1.0f);
	s = sampler_new();
	sampler_init(s, Random, 0, 1, 41);
	const struct color result = path_trace(down_ray(), &f.world, 0, s);
	TASK_CHECK(black3(result) && finite3(result));
	sampler_destroy(s);
	free_scene(&f);
	return 0;
}

const struct task_case task_cases[] = {
	{ "density", "leaf_forms", test_leaf_forms },
	{ "density", "leaf_integrals", test_leaf_integrals },
	{ "density", "combination_integrals", test_combination_integrals },
	{ "graphs", "sampled_consistency", test_sampled_consistency },
	{ "graphs", "shared_and_same_leaves", test_shared_and_same_leaves },
	{ "graphs", "mix_factor_and_emission", test_mix_factor_and_emission },
	{ "graphs", "unsupported_leaves", test_unsupported_leaves },
	{ "transport", "emission_cutoff", test_emission_cutoff },
	{ "transport", "mixed_emission_cutoff", test_mixed_emission_cutoff },
	{ "transport", "scatter_draw_count", test_scatter_draw_count },
	{ "transport", "emission_once_per_hit", test_emission_once_per_hit },
	{ "transport", "zero_response_terminates", test_zero_response_terminates },
	{ "transport", "background_miss", test_background_miss },
	{ "transport", "legacy_metal_glass", test_legacy_metal_glass },
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
