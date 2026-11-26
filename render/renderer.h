#pragma once

#include "fwd.h"
#include "pixel.h"
#include "texture.h"
#include "math/vec3.h"
#include <stdbool.h>

struct Renderer {
  Renderable *root_renderable;

  Texture framebuffer;
  Pixel clear_color;
  bool clear;

  Mat4 camera_projection;
  Mat4 camera_view;

  Backend *backend;

  // Rendering optimizations
  bool enable_backface_culling;
  bool enable_frustum_culling;
  bool enable_early_z_test;

  // Opt-in per-fragment point lighting. Off by default, which keeps the
  // original face-shaded image bit-for-bit. `light_chain` is transient
  // traversal state owned by entity_render and is NULL outside a traversal.
  bool point_lighting_enabled;
  Vec3f point_lighting_ambient;
  const LightChain *light_chain;
};

extern int renderer_render(Renderer *);

extern int renderer_init(Renderer *, Vec2i size, Backend *backend);

extern int renderer_set_root_renderable(Renderer *renderer, Renderable *root);

// Optimization configuration functions
extern void renderer_enable_backface_culling(Renderer *renderer, bool enable);
extern void renderer_enable_frustum_culling(Renderer *renderer, bool enable);
extern void renderer_enable_early_z_test(Renderer *renderer, bool enable);
extern void renderer_set_all_optimizations(Renderer *renderer, bool enable);

// Point lighting configuration. `ambient` is added to every fragment's light
// sum even when no point light reaches it. Returns 0 on success.
extern int renderer_set_point_lighting(Renderer *renderer, bool enabled,
                                       Vec3f ambient);
