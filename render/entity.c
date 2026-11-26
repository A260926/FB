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

  // Push this node's borrowed lights onto the traversal chain. The chain node
  // lives on this C call's stack and is removed before returning, so lights
  // apply to this node's own renderable and every visible descendant only.
  LightChain chain;
  chain.lights = entity->point_lights;
  chain.count = entity->point_light_count;
  chain.world = new_transform;
  chain.parent = renderer->light_chain;
  renderer->light_chain = &chain;

  for (size_t i = 0; i < entity->children_entities.size; i++) {
    Entity *child_entity = &((Entity *)entity->children_entities.data)[i];
    child_entity->renderable.render(child_entity, new_transform, renderer);
  }

  Renderable *renderable = entity->entity_renderable;
  int result = renderable->render(renderable, new_transform, renderer);

  renderer->light_chain = chain.parent;
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
  this->point_light_count = 0;

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
  this->point_light_count = 0;

  array_init(&this->children_entities, children_count, children);

  return OK;
}

int entity_set_point_lights(Entity *entity, const PointLight *lights,
                            size_t count) {
  IF_NULL_RETURN(entity, SET_ERROR);

  // A zero-length list is always treated as empty, even if a non-NULL pointer
  // is supplied; a NULL pointer with a non-zero count is rejected.
  if (count == 0) {
    entity->point_lights = 0;
    entity->point_light_count = 0;
    return OK;
  }

  IF_NULL_RETURN(lights, SET_ERROR);

  entity->point_lights = lights;
  entity->point_light_count = count;

  return OK;
}
