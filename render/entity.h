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

  // Borrowed list of point lights expressed in this Entity's local space.
  // NULL/0 means the Entity carries no lights; the pointed-to array must
  // outlive every render that uses it.
  const PointLight *point_lights;
  size_t point_light_count;
} Entity;

extern int entity_init(Entity *this, Renderable *renderable, Mat4 transform);

extern int entity_init_children(Entity *this, Renderable *renderable,
                                Mat4 transform, Entity children[],
                                size_t children_count);

// Bind a borrowed array of `count` point lights to this Entity. Pass NULL, 0
// for an empty list. Returns 0 on success.
extern int entity_set_point_lights(Entity *entity, const PointLight *lights,
                                   size_t count);
