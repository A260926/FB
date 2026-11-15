#pragma once

#include "fwd.h"
#include "math/vec2.h"
#include "math/vec3.h"
#include "texture.h"

/**
 * A borrowed description of a thin surface that lets part of the already
 * rendered scene show through.
 *
 * `opacity` and every component of `tint` are in [0, 1].  `offset` is a pixel
 * offset; when `offset_map` is not NULL its red and green channels scale that
 * offset per fragment.  The material only stores the pointer, so the caller
 * keeps the structure alive for as long as the material is used.
 */
struct ScreenTransmission {
  float opacity;
  Vec3f tint;
  Vec2f offset;
  Texture *offset_map;
};

struct Material {
  Texture *texture;
  const ScreenTransmission *transmission;
};

int material_init(Material *this, Texture *texture);

/**
 * Binds or clears the borrowed screen-transmission description.
 *
 * Passing NULL turns transmission off.  Returns 0 on success.  On invalid
 * input the previous binding is left untouched and a non-zero value is
 * returned.
 */
int material_set_transmission(Material *this, const ScreenTransmission *transmission);
