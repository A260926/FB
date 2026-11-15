#include "material.h"
#include "render/state.h"
#include <math.h>

static int transmission_valid(const ScreenTransmission *t) {
  if (!(t->opacity >= 0.0f && t->opacity <= 1.0f))
    return 0;

  if (!(t->tint.x >= 0.0f && t->tint.x <= 1.0f) ||
      !(t->tint.y >= 0.0f && t->tint.y <= 1.0f) ||
      !(t->tint.z >= 0.0f && t->tint.z <= 1.0f))
    return 0;

  if (!isfinite(t->offset.x) || !isfinite(t->offset.y))
    return 0;

  // The offset map is optional, but a pointer that is present has to name a
  // texture that can actually be sampled.
  if (t->offset_map != 0) {
    if (t->offset_map->frameBuffer == 0)
      return 0;
    if (t->offset_map->size.x <= 0 || t->offset_map->size.y <= 0)
      return 0;
  }

  return 1;
}

int material_init(Material *this, Texture *texture) {
  IF_NULL_RETURN(this, INIT_ERROR);

  // A freshly initialized material is fully opaque.
  this->transmission = 0;

  IF_NULL_RETURN(texture, INIT_ERROR);

  this->texture = texture;
  return OK;
}

int material_set_transmission(Material *this,
                              const ScreenTransmission *transmission) {
  IF_NULL_RETURN(this, SET_ERROR);

  if (transmission == 0) {
    this->transmission = 0;
    return OK;
  }

  if (!transmission_valid(transmission))
    return SET_ERROR;

  this->transmission = transmission;
  return OK;
}
