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

static bool supported(const struct addBsdf *addBsdf) {
	return addBsdf->A->eval && addBsdf->A->emission && addBsdf->B->eval && addBsdf->B->emission;
}

static struct bsdfSample sample_legacy(const struct addBsdf *addBsdf, sampler *sampler, const struct hitRecord *record) {
	struct bsdfSample A = addBsdf->A->sample(addBsdf->A, sampler, record);
	struct bsdfSample B = addBsdf->B->sample(addBsdf->B, sampler, record);
	// Original branch rule for unsupported graphs: the direction comes from B.
	return (struct bsdfSample){.out = B.out, .weight = colorAdd(A.weight, B.weight)};
}

static struct bsdfSample sample(const struct bsdfNode *bsdf, sampler *sampler, const struct hitRecord *record) {
	struct addBsdf *addBsdf = (struct addBsdf *)bsdf;
	if (!supported(addBsdf)) return sample_legacy(addBsdf, sampler, record);

	// Propose one of the two child closures with equal probability.
	const struct bsdfNode *chosen = sampler_dimension(sampler) < 0.5f ? addBsdf->A : addBsdf->B;
	struct bsdfSample picked = chosen->sample(chosen, sampler, record);

	const struct bsdfEval ea = addBsdf->A->eval(addBsdf->A, sampler, record, picked.out.direction);
	const struct bsdfEval eb = addBsdf->B->eval(addBsdf->B, sampler, record, picked.out.direction);
	// Marginal density of the whole graph in the proposed direction.
	const float pdf = 0.5f * (ea.pdf + eb.pdf);
	// Responses of the two closures add; a shared child reached through both
	// paths contributes twice and is never deduplicated by pointer.
	const struct color response = colorAdd(ea.response, eb.response);

	const struct color emA = addBsdf->A->emission(addBsdf->A, sampler, record);
	const struct color emB = addBsdf->B->emission(addBsdf->B, sampler, record);
	return (struct bsdfSample){
		.out = picked.out,
		.pdf = pdf,
		.weight = pdf > 0.0f ? colorCoef(1.0f / pdf, response) : g_black_color,
		.emitted = colorAdd(emA, emB)
	};
}

static struct bsdfEval eval(const struct bsdfNode *bsdf, sampler *sampler, const struct hitRecord *record, struct vector direction) {
	struct addBsdf *addBsdf = (struct addBsdf *)bsdf;
	const struct bsdfEval ea = addBsdf->A->eval(addBsdf->A, sampler, record, direction);
	const struct bsdfEval eb = addBsdf->B->eval(addBsdf->B, sampler, record, direction);
	return (struct bsdfEval){
		.response = colorAdd(ea.response, eb.response),
		.pdf = 0.5f * (ea.pdf + eb.pdf)
	};
}

static struct color emission(const struct bsdfNode *bsdf, sampler *sampler, const struct hitRecord *record) {
	struct addBsdf *addBsdf = (struct addBsdf *)bsdf;
	return colorAdd(addBsdf->A->emission(addBsdf->A, sampler, record), addBsdf->B->emission(addBsdf->B, sampler, record));
}

const struct bsdfNode *newAdd(const struct node_storage *s, const struct bsdfNode *A, const struct bsdfNode *B) {
	// A == B is intentionally not pruned: add(A, A) has to scatter and emit
	// twice, and a shared child must count once per path it is used on.
	HASH_CONS(s->node_table, hash, struct addBsdf, {
		.A = A ? A : newDiffuse(s, newConstantTexture(s, g_black_color)),
		.B = B ? B : newDiffuse(s, newConstantTexture(s, g_black_color)),
		.bsdf = {
			.sample = sample,
			// A NULL child is replaced above by a supported diffuse.
			.eval = (!A || A->eval) && (!B || B->eval) ? eval : NULL,
			.emission = (!A || A->emission) && (!B || B->emission) ? emission : NULL,
			.base = { .compare = compare, .dump = dump }
		}
	});
}
