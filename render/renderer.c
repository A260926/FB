#include "renderer.h"
#include "backend.h"
#include "depth.h"
#include "math/mat4.h"
#include "pixel.h"
#include "render/state.h"
#include <stdio.h>
#include <string.h>

int renderer_init(Renderer *r, Vec2i size, Backend *backend) {
  r->root_renderable = 0;
  r->clear = 1;
  r->clear_color = PIXELBLACK;
  r->backend = backend;
  r->backend->init(r, r->backend, (Vec4i){0, 0, 0, 0});

  // Initialize optimizations (enable by default for better performance)
  r->enable_backface_culling = true;
  r->enable_frustum_culling = false; // Disabled by default (can have overhead)
  r->enable_early_z_test = true;

  // Point lighting is opt-in; an initialized renderer keeps the old image.
  r->point_lighting_enabled = false;
  r->point_lighting_ambient = (Vec3f){0.0f, 0.0f, 0.0f};
  r->light_chain = 0;

  int e = 0;
  e = texture_init(&r->framebuffer, size, backend->getFrameBuffer(r, backend));
  if (e)
    return e;

  return 0;
}

int renderer_render(Renderer *r) {
  Backend *be = r->backend;

  // Always start a traversal with no inherited lights: changing the root
  // between frames or reusing a Renderer must not leak a previous tree's
  // chain (entity_render restores the chain before returning anyway).
  r->light_chain = 0;

  int pixels = r->framebuffer.size.x * r->framebuffer.size.y;
  memset(be->getZetaBuffer(r, be), 0, pixels * sizeof(PingoDepth));

  be->beforeRender(r, be);

  // get current framebuffe from Backend
  r->framebuffer.frameBuffer = be->getFrameBuffer(r, be);

  // Clear draw buffer before rendering
  if (r->clear) {
    memset(be->getFrameBuffer(r, be), 0, pixels * sizeof(Pixel));
  }

  r->root_renderable->render(r->root_renderable, mat4Identity(), r);

  be->afterRender(r, be);

  return 0;
}

int renderer_set_root_renderable(Renderer *renderer, Renderable *root) {
  IF_NULL_RETURN(renderer, SET_ERROR);
  IF_NULL_RETURN(root, SET_ERROR);

  renderer->root_renderable = root;
  return 0;
}

// Optimization configuration functions
void renderer_enable_backface_culling(Renderer *renderer, bool enable) {
  if (renderer) {
    renderer->enable_backface_culling = enable;
  }
}

void renderer_enable_frustum_culling(Renderer *renderer, bool enable) {
  if (renderer) {
    renderer->enable_frustum_culling = enable;
  }
}

void renderer_enable_early_z_test(Renderer *renderer, bool enable) {
  if (renderer) {
    renderer->enable_early_z_test = enable;
  }
}

void renderer_set_all_optimizations(Renderer *renderer, bool enable) {
  if (renderer) {
    renderer->enable_backface_culling = enable;
    renderer->enable_frustum_culling = enable;
    renderer->enable_early_z_test = enable;
  }
}

int renderer_set_point_lighting(Renderer *renderer, bool enabled,
                                Vec3f ambient) {
  IF_NULL_RETURN(renderer, SET_ERROR);

  renderer->point_lighting_enabled = enabled;
  renderer->point_lighting_ambient = ambient;

  return OK;
}
