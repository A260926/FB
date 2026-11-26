//
//  bsdfnode.h
//  c-ray
//
//  Created by Valtteri Koskivuori on 29/11/2020.
//  Copyright © 2020-2024 Valtteri Koskivuori. All rights reserved.
//

#pragma once

#include <v.h>

#include "../datatypes/lightray.h"
#include "../../common/color.h"
#include "../../common/mempool.h"
#include "valuenode.h"
#include "vectornode.h"
#include "colornode.h"
#include "../datatypes/hitrecord.h"
#include "nodebase.h"

struct bsdfSample {
	struct lightRay out;
	float pdf;
	struct color weight;
	struct color emitted; // FIXME: Not really the right place for this
};

// Directional response of a closure graph. response already includes the
// projected cosine, and pdf is a density with respect to solid angle (sr^-1).
struct bsdfEval {
	struct color response;
	float pdf;
};

//TODO: Expand and refactor to match a standard bsdf signature with eval, sample and pdf
struct bsdfNode {
	struct nodeBase base;
	struct bsdfSample (*sample)(const struct bsdfNode *bsdf, sampler *sampler, const struct hitRecord *record);
	// These are present only when the entire graph consists of the continuously
	// composable closures: diffuse, translucent, isotropic, emission, mix, add.
	struct bsdfEval (*eval)(const struct bsdfNode *bsdf, sampler *sampler, const struct hitRecord *record, struct vector direction);
	struct color (*emission)(const struct bsdfNode *bsdf, sampler *sampler, const struct hitRecord *record);
};

static inline struct color bsdf_zero_emission(const struct bsdfNode *bsdf, sampler *sampler, const struct hitRecord *record) {
	(void)bsdf;
	(void)sampler;
	(void)record;
	return (struct color){ 0.0f, 0.0f, 0.0f, 0.0f };
}

static inline float bsdf_clamp_unit(float value) {
	return fminf(1.0f, fmaxf(0.0f, value));
}

// response / pdf, with a defined zero result for a zero-density direction.
static inline struct color bsdf_eval_weight(struct bsdfEval eval) {
	if (!(eval.pdf > 0.0f) || !isfinite(eval.pdf))
		return (struct color){ 0.0f, 0.0f, 0.0f, 0.0f };
	struct color weight = {
		eval.response.red / eval.pdf,
		eval.response.green / eval.pdf,
		eval.response.blue / eval.pdf,
		eval.response.alpha / eval.pdf
	};
	if (!isfinite(weight.red) || !isfinite(weight.green) ||
		!isfinite(weight.blue) || !isfinite(weight.alpha))
		return (struct color){ 0.0f, 0.0f, 0.0f, 0.0f };
	return weight;
}

struct bsdf_buffer {
	const struct bsdfNode **bsdfs;
	struct cr_shader_node **descriptions;
};

void bsdf_buffer_free(struct bsdf_buffer *b);

#include "shaders/diffuse.h"
#include "shaders/glass.h"
#include "shaders/metal.h"
#include "shaders/mix.h"
#include "shaders/plastic.h"
#include "shaders/transparent.h"
#include "shaders/add.h"
#include "shaders/background.h"
#include "shaders/emission.h"
#include "shaders/isotropic.h"
#include "shaders/translucent.h"

const struct bsdfNode *build_bsdf_node(struct cr_scene *s_ext, const struct cr_shader_node *desc);
