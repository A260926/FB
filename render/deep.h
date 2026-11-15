#pragma once

/**
 * Deep screen transmission.
 *
 * A normal render can only show the nearest surface per pixel.  A thin
 * surface using screen transmission needs the colour behind it, and when that
 * is another thin surface, the one behind that, and so on.  A DeepFrame keeps
 * every captured layer per pixel together with its already-evaluated colour
 * and transmission parameters, so sampling can descend through the layers
 * even after the scene's meshes, materials, textures and renderer are gone.
 */

#include "depth.h"
#include "fwd.h"
#include "math/vec2.h"
#include "math/vec3.h"
#include "pixel.h"
#include "texture.h"

/** Captures every layer of the renderer's current scene. */
int renderer_capture_deep(Renderer *renderer, DeepFrame **out);

/**
 * Samples the layer at integer pixel (x, y) closest to, but strictly in front
 * of, `upper`.  `upper` may be positive infinity.  The result is in linear RGB
 * in [0, 1] (transmission is not clamped while descending).  A pixel with no
 * such layer is black.
 */
int deep_frame_sample(const DeepFrame *frame, int x, int y, float upper,
                      Vec3f *out);

/**
 * Resolves a frame into an RGB texture and a 32-bit depth buffer.  The target
 * texture's size must match the frame.  Colour is clamped and quantized once;
 * alpha is 255.  Depth is the closest captured layer, or 0 when empty.
 */
int deep_frame_resolve(const DeepFrame *frame, Texture *target,
                       PingoDepth *depth);

/** Releases a frame.  Passing NULL is allowed. */
void deep_frame_destroy(DeepFrame *frame);

/** Captures, resolves into private buffers and publishes them to the backend. */
int renderer_render_deep(Renderer *renderer);
