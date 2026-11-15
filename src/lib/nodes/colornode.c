//
//  texturenode.c
//  c-ray
//
//  Created by Valtteri Koskivuori on 30/11/2020.
//  Copyright © 2020-2025 Valtteri Koskivuori. All rights reserved.
//

#include <v.h>
#include <renderer/samplers/sampler.h>
#include <renderer/renderer.h>
#include <common/color.h>
#include <common/vector.h>
#include <common/loaders/textureloader.h>
#include <common/cr_string.h>
#include <datatypes/poly.h>
#include <datatypes/scene.h>
#include "bsdfnode.h"

#include "colornode.h"

// const struct colorNode *unknownTextureNode(const struct node_storage *s) {
// 	return newConstantTexture(s, g_black_color);
// }

struct decode_task_arg {
	char *path;
	struct texture_asset *asset;
};

void tex_decode_task(void *arg) {
	struct decode_task_arg *dt = (struct decode_task_arg *)arg;
	struct texture_asset *asset = dt->asset;
	v_timer timer = v_timer_start();
	file_data data = file_load(dt->path);
	// Decode into the raw slot of the shared asset. Each path is only
	// decoded once per scene; the raw data is published below and never
	// modified afterwards.
	int ret = data ? load_texture(dt->path, data, asset->t) : 1;
	if (!data) logr(warning, "Failed to load file '%s'\n", dt->path);
	long ms_decode = v_timer_get_ms(timer);
	v_mutex_lock(asset->mu);
	asset->decode_count++;
	asset->state = ret ? tex_asset_failed : tex_asset_ready;
	if (!ret && asset->linear_requested && !asset->linear_ready) {
		// A node asked for the linear view while the decode was queued
		// or running. Build it right here from the freshly published raw
		// data, so a worker thread never has to sit idle waiting for
		// another queued task to finish first.
		asset->linear = tex_linear_view(asset->t);
		asset->linear_ready = true;
	}
	v_mutex_release(asset->mu);
	long ms_total = v_timer_get_ms(timer);
	if (data) v_arr_free(data);
	char buf[3][64];
	logr(debug, "Async decode of '%s' took %s (%s dec, %s view)\n",
		dt->path,
		ms_to_readable(ms_total, buf[0]),
		ms_to_readable(ms_decode, buf[1]),
		ms_to_readable(ms_total - ms_decode, buf[2]));
	free(dt->path);
	free(dt);
}

// Find the shared image asset for `path`, creating it and queueing its
// decode if this is the first request for this path in this scene.
// Requests for the same path always share a single asset (and thus a
// single decode and a single copy of the raw pixels), regardless of the
// sampling options attached to each request.
static struct texture_asset *scene_image_asset(struct world *scene, const char *path) {
	for (size_t i = 0; i < v_arr_len(scene->textures); ++i) {
		if (stringEquals(scene->textures[i]->path, path))
			return scene->textures[i];
	}
	struct texture_asset *asset = calloc(1, sizeof(*asset));
	*asset = (struct texture_asset){
		.path = stringCopy(path),
		.t = tex_new(none, 0, 0, 0),
		.mu = v_mutex_create(),
		.state = tex_asset_pending,
	};
	v_arr_add(scene->textures, asset);
	struct decode_task_arg *arg = calloc(1, sizeof(*arg));
	*arg = (struct decode_task_arg){
		.path = stringCopy(path),
		.asset = asset
	};
	v_threadpool_enqueue(scene->bg_worker, tex_decode_task, arg);
	return asset;
}

const struct colorNode *build_color_node(struct cr_scene *s_ext, const struct cr_color_node *desc) {
	if (!s_ext || !desc) return NULL;
	struct world *scene = (struct world *)s_ext;
	struct node_storage s = scene->storage;

	switch (desc->type) {
		case cr_cn_constant:
			return newConstantTexture(&s,
				(struct color){
					desc->arg.constant.r,
					desc->arg.constant.g,
					desc->arg.constant.b,
					desc->arg.constant.a
				});
		case cr_cn_image: {
			// FIXME: Hack, figure out a consistent way to deal with relative paths everywhere
			char *full = NULL;
			if (!stringStartsWith(scene->asset_path, desc->arg.image.full_path))
				full = stringConcat(scene->asset_path, desc->arg.image.full_path);
			char *path = full ? full : desc->arg.image.full_path;
			windowsFixPath(path);
			struct texture_asset *asset = scene_image_asset(scene, path);
			if (desc->arg.image.options & SRGB_TRANSFORM) {
				// Flag that a linear-light view of this source is needed.
				// If the raw data already finished decoding, build the
				// view right here on the calling thread; otherwise the
				// decode task picks this up when it publishes the source.
				// Either way every SRGB_TRANSFORM node for this path ends
				// up sampling the same cached view.
				v_mutex_lock(asset->mu);
				asset->linear_requested = true;
				if (asset->state == tex_asset_ready && !asset->linear_ready) {
					asset->linear = tex_linear_view(asset->t);
					asset->linear_ready = true;
				}
				v_mutex_release(asset->mu);
			}
			const struct colorNode *new = newImageTexture(&s, asset, desc->arg.image.options);
			if (full) free(full);
			return new;
		}
		case cr_cn_checkerboard:
			return newCheckerBoardTexture(&s,
				build_color_node(s_ext, desc->arg.checkerboard.a),
				build_color_node(s_ext, desc->arg.checkerboard.b),
				build_value_node(s_ext, desc->arg.checkerboard.scale));
		case cr_cn_blackbody:
			return newBlackbody(&s, build_value_node(s_ext, desc->arg.blackbody.degrees));
		case cr_cn_split:
			return newSplitValue(&s, build_value_node(s_ext, desc->arg.split.node));
		case cr_cn_rgb:
			return newCombineRGB(&s,
				build_value_node(s_ext, desc->arg.rgb.red),
				build_value_node(s_ext, desc->arg.rgb.green),
				build_value_node(s_ext, desc->arg.rgb.blue));
		case cr_cn_hsl:
			return newCombineHSL(&s,
				build_value_node(s_ext, desc->arg.hsl.H),
				build_value_node(s_ext, desc->arg.hsl.S),
				build_value_node(s_ext, desc->arg.hsl.L));
		case cr_cn_hsv:
			return newCombineHSV(&s,
				build_value_node(s_ext, desc->arg.hsv.H),
				build_value_node(s_ext, desc->arg.hsv.S),
				build_value_node(s_ext, desc->arg.hsv.V));
		case cr_cn_hsv_tform:
			return newHSVTransform(&s,
				build_color_node(s_ext, desc->arg.hsv_tform.tex),
				build_value_node(s_ext, desc->arg.hsv_tform.H),
				build_value_node(s_ext, desc->arg.hsv_tform.S),
				build_value_node(s_ext, desc->arg.hsv_tform.V),
				build_value_node(s_ext, desc->arg.hsv_tform.f));
		case cr_cn_vec_to_color:
			return newVecToColor(&s, build_vector_node(s_ext, desc->arg.vec_to_color.vec));
		case cr_cn_color_mix:
			return new_color_mix(&s,
				build_color_node(s_ext, desc->arg.color_mix.a),
				build_color_node(s_ext, desc->arg.color_mix.b),
				build_value_node(s_ext, desc->arg.color_mix.factor));
		case cr_cn_color_ramp:
			return new_color_ramp(&s,
				build_value_node(s_ext, desc->arg.color_ramp.factor),
				desc->arg.color_ramp.color_mode,
				desc->arg.color_ramp.interpolation,
				desc->arg.color_ramp.elements,
				desc->arg.color_ramp.element_count);
		default: // FIXME: default remove
			return NULL;
	};
	return NULL;
}

