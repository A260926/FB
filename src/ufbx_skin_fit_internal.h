#ifndef UFBX_SKIN_FIT_INTERNAL_H
#define UFBX_SKIN_FIT_INTERNAL_H

#include "ufbx.h"

// Allocates `elem_size * count` bytes from the scene's own result buffer so the
// memory is released by the regular `ufbx_free_scene()` and stays valid for the
// whole lifetime of the scene. Implemented in ufbx.c as it needs the private
// scene implementation layout. Returns a non-NULL dummy pointer for count == 0
// and NULL on allocation failure.
void *ufbxi_skin_fit_scene_push(ufbx_scene *scene, size_t elem_size, size_t count);

#endif
