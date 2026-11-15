#pragma once

#include "array.h"
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
} Entity;

extern int entity_init(Entity *this, Renderable *renderable, Mat4 transform);

extern int entity_init_children(Entity *this, Renderable *renderable,
                                Mat4 transform, Entity children[],
                                size_t children_count);
