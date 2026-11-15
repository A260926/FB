#ifndef TASK_PRODUCTION_H
#define TASK_PRODUCTION_H
#include "object.h"
/* Existing upstream externally linked functions, absent from render.h. */
float cast_ray(const struct Ray *, const v3, v3, uint32_t, struct Object *);
bool is_light_blocked(const struct Ray *, float, v3, const struct Object *);
#endif
