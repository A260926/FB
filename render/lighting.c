#include "lighting.h"

#include "math/mat4.h"
#include "math/vec3.h"

#include <math.h>

Vec3f lighting_evaluate(const LightChain *chain, Vec3f normal, Vec3f point,
                        Vec3f ambient) {
  Vec3f color = ambient;

  for (const LightChain *link = chain; link != 0; link = link->parent) {
    for (size_t i = 0; i < link->count; ++i) {
      const PointLight *light = &link->lights[i];

      // The light position is local to the binding node; move it to world
      // space through that node's complete accumulated transform.
      Vec4f local = {light->position.x, light->position.y,
                     light->position.z, 1.0f};
      Mat4 world = link->world;
      Vec4f transformed = mat4MultiplyVec4(&local, &world);
      float inv_w = transformed.w != 0.0f ? 1.0f / transformed.w : 1.0f;
      Vec3f position = {transformed.x * inv_w, transformed.y * inv_w,
                        transformed.z * inv_w};

      Vec3f delta = vec3fsubV(position, point);
      float d = sqrtf(vec3Dot(delta, delta));

      // Distance must be strictly inside (0, range): at d == 0 the direction
      // is undefined and at d == range the falloff is already zero.
      if (!(d > 0.0f) || !(d < light->range))
        continue;

      Vec3f direction = vec3fmul(delta, 1.0f / d);
      float ndl = vec3Dot(normal, direction);
      if (ndl < 0.0f)
        ndl = 0.0f;

      float one_minus = 1.0f - d / light->range;
      float falloff = (one_minus * one_minus) / (1.0f + d * d);

      float k = ndl * falloff;
      color.x += light->intensity.x * k;
      color.y += light->intensity.y * k;
      color.z += light->intensity.z * k;
    }
  }

  return color;
}

Pixel lighting_shade_pixel(Vec3f light, Pixel base) {
  // One clamp, after the light sum has been multiplied into the base color.
  // Casting a non-negative in-range float to an unsigned byte truncates
  // toward zero, which is the required truncation.
  float r = light.x * (float)base.r;
  float g = light.y * (float)base.g;
  float b = light.z * (float)base.b;

  if (r < 0.0f)
    r = 0.0f;
  else if (r > 255.0f)
    r = 255.0f;
  if (g < 0.0f)
    g = 0.0f;
  else if (g > 255.0f)
    g = 255.0f;
  if (b < 0.0f)
    b = 0.0f;
  else if (b > 255.0f)
    b = 255.0f;

  uint8_t rb = (uint8_t)r;
  uint8_t gb = (uint8_t)g;
  uint8_t bb = (uint8_t)b;

#if defined(PINGO_PIXEL_RGBA8888) || defined(PINGO_PIXEL_BGRA8888)
  return pixelFromRGBA(rb, gb, bb, base.a);
#else
  // RGB888 carries no alpha and the grayscale formats collapse channels in
  // pixelFromRGBA; the task build uses BGRA8888.
  return pixelFromRGBA(rb, gb, bb, 255);
#endif
}
