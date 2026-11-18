#include "lighting.h"

#include "math/mat4.h"
#include <math.h>

Vec3f lighting_evaluate(const EntityLightFrame *frame, Vec3f normal,
                        Vec3f point, Vec3f ambient) {
  Vec3f sum = ambient;

  for (const EntityLightFrame *f = frame; f != 0; f = f->parent) {
    // The matrix helpers take a mutable pointer, but only read it; the frame
    // is const, so work from a local copy rather than casting the const away.
    Mat4 transform = f->transform;

    for (size_t i = 0; i < f->count; i++) {
      const PointLight *light = &f->lights[i];

      // The light position is local to the entity, so the full accumulated
      // transform - translation, rotation and scale - carries it to world
      // space. The range stays a world distance and is not scaled with it.
      // A local copy of the position, too: mat4MultiplyVec3 writes through
      // its pointer and the light list is borrowed read-only storage.
      Vec3f local = light->position;
      Vec3f position = mat4MultiplyVec3(&local, &transform);
      Vec3f toFragment = vec3fsubV(position, point);
      float distance = vec3Dot(toFragment, toFragment);
      if (distance <= 0.0f)
        continue;
      distance = sqrtf(distance);

      if (!(distance > 0.0f && distance < light->range))
        continue;

      Vec3f direction = vec3fmul(toFragment, 1.0f / distance);
      float lambert = vec3Dot(normal, direction);
      if (lambert <= 0.0f)
        continue;

      // (1 - d/range)^2 / (1 + d*d): the window fades to zero at the range
      // boundary, the denominator keeps the near field from exploding.
      float window = 1.0f - distance / light->range;
      float weight = (window * window) / (1.0f + distance * distance);
      sum = vec3fsumV(sum, vec3fmul(light->intensity, lambert * weight));
    }
  }

  return sum;
}
