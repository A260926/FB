//
//  emission.c
//  c-ray
//
//  Created by Valtteri on 2.1.2021.
//  Copyright © 2021-2025 Valtteri Koskivuori. All rights reserved.
//

#include <stdio.h>
#include <common/color.h>
#include <common/vector.h>
#include <common/hashtable.h>
#include <renderer/samplers/sampler.h>
#include <renderer/samplers/vec.h>
#include <datatypes/hitrecord.h>
#include <datatypes/scene.h>
#include "../colornode.h"
#include "../bsdfnode.h"

#include "emission.h"

struct emissiveBsdf {
	struct bsdfNode bsdf;
	const struct colorNode *color;
	const struct valueNode *strength;
};

static bool compare(const void *A, const void *B) {
	const struct emissiveBsdf *this = A;
	const struct emissiveBsdf *other = B;
	return this->color == other->color && this->strength == other->strength;
}

static uint32_t hash(const void *p) {
	const struct emissiveBsdf *this = p;
	uint32_t h = hashInit();
	h = hashBytes(h, &this->color, sizeof(this->color));
	h = hashBytes(h, &this->strength, sizeof(this->strength));
	return h;
}

static void dump(const void *node, char *dumpbuf, int bufsize) {
	struct emissiveBsdf *self = (struct emissiveBsdf *)node;
	char color[DUMPBUF_SIZE / 2] = "";
	char strength[DUMPBUF_SIZE / 2] = "";
	if (self->color->base.dump) self->color->base.dump(self->color, color, sizeof(color));
	if (self->strength->base.dump) self->strength->base.dump(self->strength, strength, sizeof(strength));
	snprintf(dumpbuf, bufsize, "emissiveBsdf { color: %s, strength: %s }", color, strength);
}

static struct bsdfSample sample(const struct bsdfNode *bsdf, sampler *sampler, const struct hitRecord *record) {
	struct emissiveBsdf *emitBsdf = (struct emissiveBsdf *)bsdf;
	const struct vector scatterDir = vec_normalize(vec_add(record->surfaceNormal, vec_on_unit_sphere(sampler)));
	const float cosine = max(vec_dot(record->surfaceNormal, scatterDir), 0.0f);
	return (struct bsdfSample){
		.out = { .start = record->hitPoint, .direction = scatterDir, .type = rt_reflection | rt_diffuse },
		// Emission scatters no light, so the scattering weight is zero.
		.pdf = cosine / PI,
		.weight = g_black_color,
		.emitted = colorCoef(emitBsdf->strength->eval(emitBsdf->strength, sampler, record), emitBsdf->color->eval(emitBsdf->color, sampler, record))
	};
}

static struct bsdfEval eval(const struct bsdfNode *bsdf, sampler *sampler, const struct hitRecord *record, struct vector direction) {
	(void)bsdf; (void)sampler;
	// No directional scattering response, but the cosine hemisphere proposal
	// is retained, so the pdf matches the diffuse leaf.
	return (struct bsdfEval){
		.response = g_black_color,
		.pdf = max(vec_dot(record->surfaceNormal, direction), 0.0f) / PI
	};
}

static struct color emission(const struct bsdfNode *bsdf, sampler *sampler, const struct hitRecord *record) {
	struct emissiveBsdf *emitBsdf = (struct emissiveBsdf *)bsdf;
	return colorCoef(emitBsdf->strength->eval(emitBsdf->strength, sampler, record), emitBsdf->color->eval(emitBsdf->color, sampler, record));
}

const struct bsdfNode *newEmission(const struct node_storage *s, const struct colorNode *color, const struct valueNode *strength) {
	HASH_CONS(s->node_table, hash, struct emissiveBsdf, {
		.color = color ? color : newConstantTexture(s, g_black_color),
		.strength = strength ? strength : newConstantValue(s, 1.0f),
		.bsdf = {
			.sample = sample,
			.eval = eval,
			.emission = emission,
			.base = { .compare = compare, .dump = dump }
		}
	});
}
