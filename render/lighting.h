#pragma once

#include "math/mat4.h"
#include "math/vec3.h"
#include <stddef.h>

/**
 * A point light attached to an Entity.
 *
 * `position` is in the local space of the entity the light is attached to, so
 * it follows that entity's full accumulated transform (including any scale).
 * `intensity` is a non-negative linear RGB colour and `range` is a positive
 * world-space distance; unlike the position, the range is not scaled.
 */
typedef struct PointLight {
  Vec3f position;
  Vec3f intensity;
  float range;
} PointLight;

/**
 * One node of the effective light list built while the entity tree is walked.
 *
 * The frame lives on entity_render's stack for as long as that node's own
 * renderable and its descendants are drawn. Object rendering only reads the
 * chain through the renderer, so one Object referenced by several entities is
 * shaded with the lights of whichever node is currently drawing it.
 */
typedef struct EntityLightFrame {
  const PointLight *lights;
  size_t count;
  Mat4 transform;
  const struct EntityLightFrame *parent;
} EntityLightFrame;

/**
 * Returns ambient plus the contribution of every light in `frame`'s chain.
 *
 * `normal` must be the unit world-space face normal and `point` the
 * world-space fragment position. The result is a linear RGB triple that the
 * caller multiplies with the surface colour channel by channel.
 */
Vec3f lighting_evaluate(const EntityLightFrame *frame, Vec3f normal,
                        Vec3f point, Vec3f ambient);
