//
//  image.c
//  c-ray
//
//  Created by Valtteri Koskivuori on 06/12/2020.
//  Copyright © 2020-2022 Valtteri Koskivuori. All rights reserved.
//

#include <stdio.h>
#include <string.h>
#include <common/color.h>
#include <common/texture.h>
#include <common/mempool.h>
#include <common/hashtable.h>
#include <datatypes/poly.h>
#include <datatypes/hitrecord.h>
#include <datatypes/scene.h>
#include "../colornode.h"

#include "image.h"

struct imageTexture {
	struct colorNode node;
	const struct texture_asset *asset;
	uint8_t options;
};

// Pick the cached view of the shared source that matches this node's
// options. The raw pixels and the optional linear view live once per
// path in the texture_asset; nodes never own pixel data, they just
// sample the view their options describe.
static const struct texture *pick_view(const struct texture_asset *asset, uint8_t options) {
	if (!(options & SRGB_TRANSFORM)) return asset->t;
	if (!asset->linear_ready) return NULL; // Decode pending or failed -> pink fallback
	// A NULL linear view means the raw source already is linear float
	// data (e.g. an HDR environment map), so it is sampled directly.
	return asset->linear ? asset->linear : asset->t;
}

//Transform the intersection coordinates to the texture coordinate space
//And grab the color at that point. Texture mapping.
static struct color internalColor(const struct texture *tex, const struct hitRecord *isect, uint8_t options) {
	if (unlikely(!tex || !tex->data.byte_p)) return g_pink_color; // Async img decode fails -> we may get an empty texture
	//Get the color value at these XY coordinates
	struct color output;
	if (options & NO_BILINEAR) {
		float x = isect->uv.x * tex->width;
		float y = isect->uv.y * tex->height;
		output = tex_get_px(tex, x, y, false);
	} else {
		output = tex_get_px(tex, isect->uv.x, isect->uv.y, true);
	}

	return output;
}

static bool compare(const void *A, const void *B) {
	const struct imageTexture *this = A;
	const struct imageTexture *other = B;
	return this->asset == other->asset && this->options == other->options;
}

static uint32_t hash(const void *p) {
	const struct imageTexture *this = p;
	uint32_t h = hashInit();
	h = hashBytes(h, &this->asset, sizeof(this->asset));
	h = hashBytes(h, &this->options, sizeof(this->options));
	return h;
}

static void dump(const void *node, char *dumpbuf, int len) {
	struct imageTexture *self = (struct imageTexture *)node;
	const struct texture *tex = self->asset ? self->asset->t : NULL;
	//TODO: Consider having imageTexture have a func to dump this.
	if (!tex) {
		snprintf(dumpbuf, len, "imageTexture { tex: null }");
		return;
	}
	if (!tex->data.byte_p) {
		snprintf(dumpbuf, len, "imageTexture { tex: pending }");
		return;
	}
	snprintf(dumpbuf, len, "imageTexture { tex: { %zux%zu, %zu channels, %s, %s }, options: %s %s }",
		tex->width,
		tex->height,
		tex->channels,
		tex->colorspace == linear ? "linear" : "sRGB",
		tex->precision == char_p ? "8 bits/channel" : "32 bits/channel",
		self->options & SRGB_TRANSFORM ? "SRGB_TRANSFORM" : "",
		self->options & NO_BILINEAR ? "NO_BILINEAR" : "");
}

static struct color eval(const struct colorNode *node, sampler *sampler, const struct hitRecord *record) {
	(void)sampler;
	struct imageTexture *image = (struct imageTexture *)node;
	return internalColor(pick_view(image->asset, image->options), record, image->options);
}

const struct colorNode *newImageTexture(const struct node_storage *s, const struct texture_asset *asset, uint8_t options) {
	if (!asset) return NULL;
	HASH_CONS(s->node_table, hash, struct imageTexture, {
		.asset = asset,
		.options = options,
		.node = {
			.eval = eval,
			.base = { .compare = compare, .dump = dump }
		}
	});
}
