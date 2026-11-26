//
//  add.c
//  c-ray
//
//  Created by Valtteri Koskivuori on 17/12/2020.
//  Copyright © 2020-2025 Valtteri Koskivuori. All rights reserved.
//

#include <stdio.h>
#include <common/color.h>
#include <common/vector.h>
#include <common/hashtable.h>
#include <renderer/samplers/sampler.h>
#include "../colornode.h"
#include <datatypes/scene.h>
#include "../bsdfnode.h"

#include "add.h"

struct addBsdf {
	struct bsdfNode bsdf;
	const struct bsdfNode *A;
	const struct bsdfNode *B;
};

static bool compare(const void *A, const void *B) {
	struct addBsdf *this = (struct addBsdf *)A;
	struct addBsdf *other = (struct addBsdf *)B;
	return this->A == other->A && this->B == other->B;
}

static uint32_t hash(const void *p) {
	const struct addBsdf *this = p;
	uint32_t h = hashInit();
	h = hashBytes(h, &this->A, sizeof(this->A));
	h = hashBytes(h, &this->B, sizeof(this->B));
	return h;
}

static void dump(const void *node, char *dumpbuf, int bufsize) {
	struct addBsdf *self = (struct addBsdf *)node;
	char A[DUMPBUF_SIZE / 2] = "";
	char B[DUMPBUF_SIZE / 2] = "";
	if (self->A->base.dump) self->A->base.dump(self->A, A, sizeof(A));
	if (self->B->base.dump) self->B->base.dump(self->B, B, sizeof(B));
	snprintf(dumpbuf, bufsize, "addBsdf { A: %s, B: %s }", A, B);
}

static struct bsdfEval eval(const struct bsdfNode *bsdf, sampler *sampler, const struct hitRecord *record, struct vector direction) {
	const struct addBsdf *self = (const struct addBsdf *)bsdf;
	struct bsdfEval A = self->A->eval(self->A, sampler, record, direction);
	struct bsdfEval B = self->B->eval(self->B, sampler, record, direction);
	return (struct bsdfEval){
		.response = colorAdd(A.response, B.response),
		.pdf = 0.5f * (A.pdf + B.pdf)
	};
}

static struct color emission(const struct bsdfNode *bsdf, sampler *sampler, const struct hitRecord *record) {
	const struct addBsdf *self = (const struct addBsdf *)bsdf;
	return colorAdd(self->A->emission(self->A, sampler, record),
					self->B->emission(self->B, sampler, record));
}

static struct bsdfSample sampleSupported(const struct bsdfNode *bsdf, sampler *sampler, const struct hitRecord *record) {
	const struct addBsdf *self = (const struct addBsdf *)bsdf;
	const struct bsdfNode *chosen = sampler_dimension(sampler) < 0.5f ? self->A : self->B;
	struct bsdfSample result = chosen->sample(chosen, sampler, record);
	const struct bsdfEval fullEval = eval(bsdf, sampler, record, result.out.direction);
	result.pdf = fullEval.pdf;
	result.weight = bsdf_eval_weight(fullEval);
	result.emitted = emission(bsdf, sampler, record);
	return result;
}

static struct bsdfSample sample(const struct bsdfNode *bsdf, sampler *sampler, const struct hitRecord *record) {
	struct addBsdf *mixBsdf = (struct addBsdf *)bsdf;
	if (bsdf->eval && bsdf->emission)
		return sampleSupported(bsdf, sampler, record);
	struct bsdfSample A = mixBsdf->A->sample(mixBsdf->A, sampler, record);
	struct bsdfSample B = mixBsdf->B->sample(mixBsdf->B, sampler, record);
	// TODO: Do we just add the outgoing vertices together or what...?
	// TODO: Find out if we want to even keep this node around.
	// FIXME: Hackery. We're using B.out here, so our fake Principled Shader graph works
	// as expected. This will be resolved once this shader system is rewritten to be correct,
	// we're not supposed to compute the out direction here.
	// Cycles does the add with OSL shading closures, instead of at this stage, so we'd have to
	// do something similar to that, probably.
	return (struct bsdfSample){
		.out = B.out,
		.pdf = 0.0f,
		.weight = colorAdd(A.weight, B.weight),
		.emitted = colorAdd(A.emitted, B.emitted)
	};
}

const struct bsdfNode *newAdd(const struct node_storage *s, const struct bsdfNode *A, const struct bsdfNode *B) {
	const struct bsdfNode *a = A ? A : newDiffuse(s, newConstantTexture(s, g_black_color));
	const struct bsdfNode *b = B ? B : newDiffuse(s, newConstantTexture(s, g_black_color));
	const bool closures = a->eval && a->emission && b->eval && b->emission;
	HASH_CONS(s->node_table, hash, struct addBsdf, {
		.A = a,
		.B = b,
		.bsdf = {
			.sample = sample,
			.eval = closures ? eval : NULL,
			.emission = closures ? emission : NULL,
			.base = { .compare = compare, .dump = dump }
		}
	});
}
