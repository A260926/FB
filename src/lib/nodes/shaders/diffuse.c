//
//  diffuse.c
//  c-ray
//
//  Created by Valtteri Koskivuori on 30/11/2020.
//  Copyright © 2020-2025 Valtteri Koskivuori. All rights reserved.
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

#include "diffuse.h"

struct diffuseBsdf {
	struct bsdfNode bsdf;
	const struct colorNode *color;
};

static bool compare(const void *A, const void *B) {
	const struct diffuseBsdf *this = A;
	const struct diffuseBsdf *other = B;
	return this->color == other->color;
}

static uint32_t hash(const void *p) {
	const struct diffuseBsdf *this = p;
	uint32_t h = hashInit();
	h = hashBytes(h, &this->color, sizeof(this->color));
	return h;
}

static void dump(const void *node, char *dumpbuf, int bufsize) {
	struct diffuseBsdf *self = (struct diffuseBsdf *)node;
	char color[DUMPBUF_SIZE / 2] = "";
	if (self->color->base.dump) self->color->base.dump(self->color, color, sizeof(color));
	snprintf(dumpbuf, bufsize, "diffuseBsdf { color: %s }", color);
}

static float density(const struct hitRecord *record, struct vector direction) {
	return fmaxf(vec_dot(record->surfaceNormal, direction), 0.0f) / PI;
}

static struct bsdfEval eval(const struct bsdfNode *bsdf, sampler *sampler, const struct hitRecord *record, struct vector direction) {
	const struct diffuseBsdf *self = (const struct diffuseBsdf *)bsdf;
	const float pdf = density(record, direction);
	const struct color color = self->color->eval(self->color, sampler, record);
	return (struct bsdfEval){ .response = colorCoef(pdf, color), .pdf = pdf };
}

static struct bsdfSample sample(const struct bsdfNode *bsdf, sampler *sampler, const struct hitRecord *record) {
	struct diffuseBsdf *diffBsdf = (struct diffuseBsdf *)bsdf;
	const struct vector scatterDir = vec_normalize(vec_add(record->surfaceNormal, vec_on_unit_sphere(sampler)));
	const float pdf = density(record, scatterDir);
	return (struct bsdfSample){
		.out = { .start = record->hitPoint, .direction = scatterDir, .type = rt_reflection | rt_diffuse },
		.pdf = pdf,
		.weight = diffBsdf->color->eval(diffBsdf->color, sampler, record)
	};
}

const struct bsdfNode *newDiffuse(const struct node_storage *s, const struct colorNode *color) {
	HASH_CONS(s->node_table, hash, struct diffuseBsdf, {
		.color = color ? color : newConstantTexture(s, g_black_color),
		.bsdf = {
			.sample = sample,
			.eval = eval,
			.emission = bsdf_zero_emission,
			.base = { .compare = compare, .dump = dump }
		}
	});
}
