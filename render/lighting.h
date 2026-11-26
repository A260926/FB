#pragma once

#include "fwd.h"
#include "math/mat4.h"
#include "math/vec3.h"
#include "pixel.h"

#include <stdbool.h>
#include <stddef.h>

/**
 * A point light attached to an Entity.
 *
 * `position` is expressed in the local space of the Entity the light is bound
 * to; it travels through that Entity's full accumulated transform, so node
 * scale moves it. `range` is a world-space distance and is never scaled.
 * `intensity` is a non-negative linear RGB multiplier.
 */
typedef struct PointLight {
  Vec3f position;
  Vec3f intensity;
  float range;
} PointLight;

/**
 * One node in the transient stack of lights active during a traversal.
 *
 * Each Entity render call owns its own node on the C stack and links it to the
 * chain stored on the Renderer. Keeping the chain per traversal rather than on
 * the Object means two nodes referencing the same Object never share a light
 * list. `LightChain` itself is typedef'd in fwd.h.
 */
struct LightChain {
  const PointLight *lights;
  size_t count;
  Mat4 world;
  const struct LightChain *parent;
};

/**
 * Sum ambient together with every active point light's contribution at the
 * world-space fragment position `p` with unit face normal `n`.
 *
 * The result is a linear RGB multiplier; it is deliberately not clamped here,
 * because clamping happens once, after it has been multiplied into the base
 * texel.
 */
Vec3f lighting_evaluate(const LightChain *chain, Vec3f n, Vec3f p,
                       Vec3f ambient);

/**
 * Multiply a base pixel by a linear RGB light and clamp/truncate every channel
 * to the byte range exactly once. Alpha is copied from the base pixel.
 */
Pixel lighting_shade_pixel(Vec3f light, Pixel base);

/**
 * Toggle point lighting on a renderer and set the linear ambient term.
 * Declared here as well as renderer.h so the lighting entry points have one
 * discoverable home; the two prototypes are identical. Returns 0 on success.
 */
int renderer_set_point_lighting(Renderer *renderer, bool enabled,
                                Vec3f ambient);
