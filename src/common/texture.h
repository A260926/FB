//
//  texture.h
//  c-ray
//
//  Created by Valtteri Koskivuori on 09/04/2019.
//  Copyright © 2019-2022 Valtteri Koskivuori. All rights reserved.
//

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <v.h>
#include "color.h"

enum colorspace {
	linear,
	sRGB
};

enum precision {
	char_p,
	float_p,
	none
};

struct texture {
	enum colorspace colorspace;
	enum precision precision;
	union {
		unsigned char *byte_p; //For 24/32bit
		float *float_p; //For hdr
	} data;
	size_t channels;
	size_t width;
	size_t height;
};

// Lifecycle of a decoded image source, shared by every node that
// references the same path within a scene.
enum tex_asset_state {
	tex_asset_pending = 0, // Decode queued, no data published yet
	tex_asset_ready,       // Raw data published, immutable from here on
	tex_asset_failed       // Decode failed, views fall back to pink, no retries
};

// Per-scene, per-path image asset. Owns the raw decoded pixels and,
// on demand, a single cached linear-light view of those pixels.
// The asset itself is heap-allocated and owned by the scene texture
// cache; nodes only keep const pointers to it.
struct texture_asset {
	char *path;           // Owned, normalized full path this asset was requested with
	struct texture *t;    // Owned raw source data, published once by the decode task
	struct texture *linear; // Owned cached linear view, generated on demand. NULL if not
	                        // generated yet, or if the raw source already is linear float
	                        // data (in which case the view aliases `t`).
	v_mutex *mu;          // Guards state, decode_count, linear_requested, linear_ready,
	                      // and the `linear` pointer while it is being published.
	enum tex_asset_state state;
	unsigned decode_count;   // How many times a decode was actually performed (<= 1)
	bool linear_requested;   // Some node asked for the SRGB_TRANSFORM view
	bool linear_ready;       // Linear view generation ran (view may still alias `t`)
};

//FIXME: These are opposite states, which is kinda confusing.
#define SRGB_TRANSFORM 0x01
#define NO_BILINEAR    0x02

struct texture *tex_new(enum precision p, size_t width, size_t height, size_t channels);

void tex_set_px(struct texture *t, struct color c, size_t x, size_t y);

/// Get a color value for a given pixel in a texture
/// @remarks When filtered == false, pass in the integer coordinates, otherwise pass in a 0.0f->1.0f coefficient
struct color tex_get_px(const struct texture *t, float x, float y, bool filtered);

/// Convert texture from sRGB to linear color space
/// @remarks The texture data will be modified directly.
/// @param t Texture to convert
void tex_from_srgb(struct texture *t);

/// Convert texture from linear color space to sRGB.
/// @remarks The texture data will be modified directly.
/// @param t Texture to convert
void tex_to_srgb(struct texture *t);

/// Create a cached linear-light view of a texture.
/// @remarks Does not modify `t`. Returns a new float precision texture with
///          RGB channels converted through colorFromSRGB() when `t` is marked
///          sRGB, or copied as-is when already linear. Alpha is preserved.
///          Returns NULL when `t` already is linear float data (HDR), in which
///          case callers should just sample `t` directly.
/// @param t Source texture to view
struct texture *tex_linear_view(const struct texture *t);

bool tex_uses_alpha(const struct texture *t);

void tex_clear(struct texture *t);

/// Deallocate a given texture
/// @param tex Texture to deallocate
void tex_destroy(struct texture *tex);
