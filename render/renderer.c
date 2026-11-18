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

  // Lighting is opt-in: this is what keeps a renderer that never calls
  // renderer_set_point_lighting() on the original face-shaded path.
  r->point_lighting_enabled = false;
  r->point_light_ambient = (Vec3f){0, 0, 0};
  r->entity_lights = 0;

  // Initialize optimizations (enable by default for better performance)
  r->enable_backface_culling = true;
  r->enable_frustum_culling = false; // Disabled by default (can have overhead)
  r->enable_early_z_test = true;

  int e = 0;
  e = texture_init(&r->framebuffer, size, backend->getFrameBuffer(r, backend));
  if (e)
    return e;

  return 0;
}

int renderer_render(Renderer *r) {
  Backend *be = r->backend;

  int pixels = r->framebuffer.size.x * r->framebuffer.size.y;
  memset(be->getZetaBuffer(r, be), 0, pixels * sizeof(PingoDepth));

  be->beforeRender(r, be);

  // get current framebuffe from Backend
  r->framebuffer.frameBuffer = be->getFrameBuffer(r, be);

  // Clear draw buffer before rendering
  if (r->clear) {
    memset(be->getFrameBuffer(r, be), 0, pixels * sizeof(Pixel));
  }

  // A previous walk always unwinds its frames; resetting here guarantees a
  // fresh root even if a renderable bailed out mid-tree in an earlier frame.
  r->entity_lights = 0;
  r->root_renderable->render(r->root_renderable, mat4Identity(), r);
  r->entity_lights = 0;

  be->afterRender(r, be);

  return 0;
}

int renderer_set_root_renderable(Renderer *renderer, Renderable *root) {
  IF_NULL_RETURN(renderer, SET_ERROR);
  IF_NULL_RETURN(root, SET_ERROR);

  renderer->root_renderable = root;
  return 0;
}

int renderer_set_point_lighting(Renderer *renderer, bool enabled,
                                Vec3f ambient) {
  IF_NULL_RETURN(renderer, SET_ERROR);

  renderer->point_lighting_enabled = enabled;
  renderer->point_light_ambient = ambient;
  return OK;
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
