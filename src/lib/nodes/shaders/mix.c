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

static struct bsdfEval eval(const struct bsdfNode *bsdf, sampler *sampler, const struct hitRecord *record, struct vector direction) {
	const struct mixBsdf *self = (const struct mixBsdf *)bsdf;
	const float factor = bsdf_clamp_unit(self->factor->eval(self->factor, sampler, record));
	const struct bsdfEval A = self->A->eval(self->A, sampler, record, direction);
	const struct bsdfEval B = self->B->eval(self->B, sampler, record, direction);
	return (struct bsdfEval){
		.response = colorAdd(colorCoef(1.0f - factor, A.response), colorCoef(factor, B.response)),
		.pdf = (1.0f - factor) * A.pdf + factor * B.pdf
	};
}

static struct color emission(const struct bsdfNode *bsdf, sampler *sampler, const struct hitRecord *record) {
	const struct mixBsdf *self = (const struct mixBsdf *)bsdf;
	const float factor = bsdf_clamp_unit(self->factor->eval(self->factor, sampler, record));
	const struct color A = self->A->emission(self->A, sampler, record);
	const struct color B = self->B->emission(self->B, sampler, record);
	return colorAdd(colorCoef(1.0f - factor, A), colorCoef(factor, B));
}

static struct bsdfSample sampleSupported(const struct bsdfNode *bsdf, sampler *sampler, const struct hitRecord *record, float factor) {
	const struct mixBsdf *mixBsdf = (const struct mixBsdf *)bsdf;
	const struct bsdfNode *chosen = sampler_dimension(sampler) > factor ? mixBsdf->A : mixBsdf->B;
	struct bsdfSample result = chosen->sample(chosen, sampler, record);
	const struct bsdfEval fullEval = bsdf->eval(bsdf, sampler, record, result.out.direction);
	result.pdf = fullEval.pdf;
	result.weight = bsdf_eval_weight(fullEval);
	result.emitted = bsdf->emission(bsdf, sampler, record);
	return result;
}

static struct bsdfSample sample(const struct bsdfNode *bsdf, sampler *sampler, const struct hitRecord *record) {
	struct mixBsdf *mixBsdf = (struct mixBsdf *)bsdf;
	const float lerp = bsdf_clamp_unit(mixBsdf->factor->eval(mixBsdf->factor, sampler, record));
	if (bsdf->eval && bsdf->emission)
		return sampleSupported(bsdf, sampler, record, lerp);
	if (sampler_dimension(sampler) > lerp) {
		return mixBsdf->A->sample(mixBsdf->A, sampler, record);
	} else {
		return mixBsdf->B->sample(mixBsdf->B, sampler, record);
	}
}

const struct bsdfNode *newMix(const struct node_storage *s, const struct bsdfNode *A, const struct bsdfNode *B, const struct valueNode *factor) {
	if (A == B) {
		logr(debug, "A == B, pruning mix node.\n");
		return A;
	}
	const struct bsdfNode *a = A ? A : newDiffuse(s, newConstantTexture(s, g_black_color));
	const struct bsdfNode *b = B ? B : newDiffuse(s, newConstantTexture(s, g_black_color));
	const struct valueNode *f = factor ? factor : newConstantValue(s, 0.5f);
	const bool closures = a->eval && a->emission && b->eval && b->emission;
	HASH_CONS(s->node_table, hash, struct mixBsdf, {
		.A = a,
		.B = b,
		.factor = f,
		.bsdf = {
			.sample = sample,
			.eval = closures ? eval : NULL,
			.emission = closures ? emission : NULL,
			.base = { .compare = compare, .dump = dump }
		}
	});
}
