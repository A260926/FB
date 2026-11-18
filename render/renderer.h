#pragma once

#include "fwd.h"
#include "lighting.h"
#include "math/vec3.h"
#include "pixel.h"
#include "texture.h"
#include <stdbool.h>

struct Renderer {
  Renderable *root_renderable;

  Texture framebuffer;
  Pixel clear_color;
  bool clear;

  Mat4 camera_projection;
  Mat4 camera_view;

  Backend *backend;

  // Point lighting: off by default, so a Renderer initialized here keeps
  // producing the same image as before. When enabled, `point_light_ambient`
  // is added to the contribution of every attached light.
  bool point_lighting_enabled;
  Vec3f point_light_ambient;

  // Current node of the entity tree being walked, or NULL outside a walk.
  // Entity rendering pushes and pops it; nothing here owns the list.
  const struct EntityLightFrame *entity_lights;

  // Rendering optimizations
  bool enable_backface_culling;
  bool enable_frustum_culling;
  bool enable_early_z_test;
};

extern int renderer_render(Renderer *);

extern int renderer_init(Renderer *, Vec2i size, Backend *backend);

extern int renderer_set_root_renderable(Renderer *renderer, Renderable *root);

/**
 * Turns per-fragment point lighting on or off.
 *
 * `ambient` is a linear RGB value added to every shaded fragment. It is only
 * read when lighting is enabled; while disabled the renderer uses the
 * original whole-face brightness and ignores any attached lights.
 */
extern int renderer_set_point_lighting(Renderer *renderer, bool enabled,
                                       Vec3f ambient);

// Optimization configuration functions
extern void renderer_enable_backface_culling(Renderer *renderer, bool enable);
extern void renderer_enable_frustum_culling(Renderer *renderer, bool enable);
extern void renderer_enable_early_z_test(Renderer *renderer, bool enable);
extern void renderer_set_all_optimizations(Renderer *renderer, bool enable);
