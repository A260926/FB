//
//  mix.c
//  c-ray
//
//  Created by Valtteri Koskivuori on 30/11/2020.
//  Copyright © 2020 Valtteri Koskivuori. All rights reserved.
//

#include <stdio.h>
#include <common/color.h>
#include <common/vector.h>
#include <common/hashtable.h>
#include <renderer/samplers/sampler.h>
#include <datatypes/scene.h>
#include "../bsdfnode.h"

#include "mix.h"

struct mixBsdf {
	struct bsdfNode bsdf;
	const struct bsdfNode *A;
	const struct bsdfNode *B;
	const struct valueNode *factor;
};

static bool compare(const void *A, const void *B) {
	struct mixBsdf *this = (struct mixBsdf *)A;
	struct mixBsdf *other = (struct mixBsdf *)B;
	return this->A == other->A && this->B == other->B && this->factor == other->factor;
}

static uint32_t hash(const void *p) {
	const struct mixBsdf *this = p;
	uint32_t h = hashInit();
	h = hashBytes(h, &this->A, sizeof(this->A));
	h = hashBytes(h, &this->B, sizeof(this->B));
	h = hashBytes(h, &this->factor, sizeof(this->factor));
	return h;
}

static void dump(const void *node, char *dumpbuf, int bufsize) {
	struct mixBsdf *self = (struct mixBsdf *)node;
	char A[DUMPBUF_SIZE / 2] = "";
	char B[DUMPBUF_SIZE / 2] = "";
	char factor[DUMPBUF_SIZE / 2] = "";
	if (self->A->base.dump) self->A->base.dump(self->A, A, sizeof(A));
	if (self->B->base.dump) self->B->base.dump(self->B, B, sizeof(B));
	if (self->factor->base.dump) self->factor->base.dump(self->factor, factor, sizeof(factor));
	snprintf(dumpbuf, bufsize, "mixBsdf { A: %s, B: %s, factor: %s }", A, B, factor);
}

static bool supported(const struct mixBsdf *mixBsdf) {
	return mixBsdf->A->eval && mixBsdf->A->emission && mixBsdf->B->eval && mixBsdf->B->emission;
}

static struct bsdfSample sample_legacy(const struct mixBsdf *mixBsdf, sampler *sampler, const struct hitRecord *record, const float lerp) {
	// Original branch rule: one child direction is chosen and returned as-is.
	if (sampler_dimension(sampler) > lerp) {
		return mixBsdf->A->sample(mixBsdf->A, sampler, record);
	} else {
		return mixBsdf->B->sample(mixBsdf->B, sampler, record);
	}
}

static struct bsdfSample sample(const struct bsdfNode *bsdf, sampler *sampler, const struct hitRecord *record) {
	struct mixBsdf *mixBsdf = (struct mixBsdf *)bsdf;
	const float lerp = clamp(mixBsdf->factor->eval(mixBsdf->factor, sampler, record), 0.0f, 1.0f);
	if (!supported(mixBsdf)) return sample_legacy(mixBsdf, sampler, record, lerp);

	// Only one child direction is drawn, but the returned pdf is the full
	// graph's marginal density in that direction.
	const struct bsdfNode *chosen = sampler_dimension(sampler) > lerp ? mixBsdf->A : mixBsdf->B;
	struct bsdfSample picked = chosen->sample(chosen, sampler, record);

	const struct bsdfEval ea = mixBsdf->A->eval(mixBsdf->A, sampler, record, picked.out.direction);
	const struct bsdfEval eb = mixBsdf->B->eval(mixBsdf->B, sampler, record, picked.out.direction);
	const float pdf = (1.0f - lerp) * ea.pdf + lerp * eb.pdf;
	const struct color response = colorAdd(colorCoef(1.0f - lerp, ea.response), colorCoef(lerp, eb.response));

	const struct color emA = mixBsdf->A->emission(mixBsdf->A, sampler, record);
	const struct color emB = mixBsdf->B->emission(mixBsdf->B, sampler, record);
	return (struct bsdfSample){
		.out = picked.out,
		.pdf = pdf,
		.weight = pdf > 0.0f ? colorCoef(1.0f / pdf, response) : g_black_color,
		.emitted = colorAdd(colorCoef(1.0f - lerp, emA), colorCoef(lerp, emB))
	};
}

static struct bsdfEval eval(const struct bsdfNode *bsdf, sampler *sampler, const struct hitRecord *record, struct vector direction) {
	struct mixBsdf *mixBsdf = (struct mixBsdf *)bsdf;
	const float lerp = clamp(mixBsdf->factor->eval(mixBsdf->factor, sampler, record), 0.0f, 1.0f);
	const struct bsdfEval ea = mixBsdf->A->eval(mixBsdf->A, sampler, record, direction);
	const struct bsdfEval eb = mixBsdf->B->eval(mixBsdf->B, sampler, record, direction);
	return (struct bsdfEval){
		.response = colorAdd(colorCoef(1.0f - lerp, ea.response), colorCoef(lerp, eb.response)),
		.pdf = (1.0f - lerp) * ea.pdf + lerp * eb.pdf
	};
}

static struct color emission(const struct bsdfNode *bsdf, sampler *sampler, const struct hitRecord *record) {
	struct mixBsdf *mixBsdf = (struct mixBsdf *)bsdf;
	const float lerp = clamp(mixBsdf->factor->eval(mixBsdf->factor, sampler, record), 0.0f, 1.0f);
	const struct color emA = mixBsdf->A->emission(mixBsdf->A, sampler, record);
	const struct color emB = mixBsdf->B->emission(mixBsdf->B, sampler, record);
	return colorAdd(colorCoef(1.0f - lerp, emA), colorCoef(lerp, emB));
}

const struct bsdfNode *newMix(const struct node_storage *s, const struct bsdfNode *A, const struct bsdfNode *B, const struct valueNode *factor) {
	// A == B is intentionally not pruned: the graph structure has to survive
	// so that mix(A, A) keeps evaluating as the unchanged closure.
	HASH_CONS(s->node_table, hash, struct mixBsdf, {
		.A = A ? A : newDiffuse(s, newConstantTexture(s, g_black_color)),
		.B = B ? B : newDiffuse(s, newConstantTexture(s, g_black_color)),
		.factor = factor ? factor : newConstantValue(s, 0.5f),
		.bsdf = {
			.sample = sample,
			// A NULL child is replaced above by a supported diffuse, so only an
			// explicitly unsupported child suppresses the callbacks.
			.eval = (!A || A->eval) && (!B || B->eval) ? eval : NULL,
			.emission = (!A || A->emission) && (!B || B->emission) ? emission : NULL,
			.base = { .compare = compare, .dump = dump }
		}
	});
}
