#pragma once

#include "array.h"
#include "lighting.h"
#include "renderable.h"
#include <stdbool.h>
#include <stddef.h>

/// Entity is a renderable associated to a transform which is renderable at a
/// position by the renderer
typedef struct {
  Renderable renderable;
  Renderable *entity_renderable;
  Mat4 transform;
  bool visible;
  Array children_entities;
  // Borrowed, like the children array. Empty lists are a NULL pointer and a
  // count of zero.
  const PointLight *point_lights;
  size_t point_lights_count;
} Entity;

extern int entity_init(Entity *this, Renderable *renderable, Mat4 transform);
extern int entity_init_children(Entity *this, Renderable *renderable,
                                Mat4 transform, Entity children[],
                                size_t children_count);

/**
 * Attaches a borrowed list of point lights to `this`.
 *
 * The lights are in the entity's local space and apply to the entity's own
 * model and to every visible descendant, which append their own lists on top.
 * Storing NULL, 0 clears the list.
 */
extern int entity_set_point_lights(Entity *this, const PointLight *lights,
                                   size_t count);
