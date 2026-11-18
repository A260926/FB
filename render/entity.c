#include "entity.h"
#include "math/mat4.h"
#include "render/array.h"
#include "renderer.h"
#include "state.h"
#include <stddef.h>

int entity_render(void *this, Mat4 transform, Renderer *renderer) {
  Entity *entity = this;
  IF_NULL_RETURN(entity, RENDER_ERROR);
  IF_NULL_RETURN(renderer, RENDER_ERROR);

  if (!entity->visible)
    return OK;

  Mat4 new_transform = mat4MultiplyM(&entity->transform, &transform);

  // The frame lives on this call's stack, so the chain a nested render sees is
  // exactly the path from the root to the node being drawn. It is rebuilt on
  // every visit: nothing is shared along the walk, and moving to a sibling
  // replaces the previous node's list instead of accumulating it.
  EntityLightFrame frame = {entity->point_lights, entity->point_lights_count,
                            new_transform, renderer->entity_lights};

  const EntityLightFrame *previous = renderer->entity_lights;
  renderer->entity_lights = &frame;

  for (size_t i = 0; i < entity->children_entities.size; i++) {
    Entity *child_entity = &((Entity *)entity->children_entities.data)[i];
    child_entity->renderable.render(child_entity, new_transform, renderer);
  }

  Renderable *renderable = entity->entity_renderable;

  int result = renderable->render(renderable, new_transform, renderer);

  renderer->entity_lights = previous;
  return result;
};

int entity_init(Entity *this, Renderable *renderable, Mat4 transform) {
  IF_NULL_RETURN(this, INIT_ERROR);
  IF_NULL_RETURN(renderable, INIT_ERROR);

  this->entity_renderable = renderable;
  this->renderable.render = &entity_render;
  this->transform = transform;
  this->visible = true;
  this->point_lights = 0;
  this->point_lights_count = 0;

  array_init(&this->children_entities, 0, 0);

  return OK;
}

int entity_init_children(Entity *this, Renderable *renderable, Mat4 transform,
                         Entity children[], size_t children_count) {
  IF_NULL_RETURN(this, INIT_ERROR);
  IF_NULL_RETURN(renderable, INIT_ERROR);

  this->entity_renderable = renderable;
  this->renderable.render = &entity_render;
  this->transform = transform;
  this->visible = true;
  this->point_lights = 0;
  this->point_lights_count = 0;

  array_init(&this->children_entities, children_count, children);

  return OK;
}

int entity_set_point_lights(Entity *this, const PointLight *lights,
                            size_t count) {
  IF_NULL_RETURN(this, SET_ERROR);
  if (lights == 0)
    count = 0;

  this->point_lights = lights;
  this->point_lights_count = count;

  return OK;
}
