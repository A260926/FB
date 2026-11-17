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

// Directional query result for a fully supported closure graph.
// response is the directional response f*cos(theta), i.e. it already
// includes the projected cosine, and pdf is the matching directional
// density with the color factor removed. Both are defined per unit solid
// angle over the unit sphere; zero outside the closure's support domain.
struct bsdfEval {
	struct color response;
	float pdf;
};

// A bsdfNode always provides sample(). The eval and emission callbacks are
// only present when the whole graph is made of supported leaves (diffuse,
// translucent, isotropic, emission) combined with mix/add. Graphs containing
// any other leaf leave both callbacks NULL and keep relying on sample() alone,
// in which case no complete directional density is promised.
// eval() does not draw scattering random numbers and never stores transient
// hit state in the shared node.
struct bsdfNode {
	struct nodeBase base;
	struct bsdfSample (*sample)(const struct bsdfNode *bsdf, sampler *sampler, const struct hitRecord *record);
	struct bsdfEval (*eval)(const struct bsdfNode *bsdf, sampler *sampler, const struct hitRecord *record, struct vector direction);
	struct color (*emission)(const struct bsdfNode *bsdf, sampler *sampler, const struct hitRecord *record);
};

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
