// Copyright (c) 2011-2020 Hiroshi Tsubokawa
// See LICENSE and README

#ifndef FJ_POINT_LIGHT_H
#define FJ_POINT_LIGHT_H

#include "fj_light.h"
#include "fj_deep_shadow.h"

namespace fj {

class PointLight : public Light {
public:
  PointLight();
  virtual ~PointLight();

  // Captures a six-face deep shadow map around the light. The context
  // provides the target object group and the sample time; the light position
  // comes from the same transform sample GetSamples() uses. Returns 0 on
  // success and -1 on invalid arguments or unsupported target geometry.
  bool BuildDeepShadowMap(const TraceContext &cxt, int resolution,
      Real maxDistance);
  void ClearDeepShadowMap();

  virtual bool LookupShadowVisibility(const TraceContext &cxt,
      const Vector &receiver, Real bias, Real *visibility) const;

private:
  virtual int get_sample_count() const;
  virtual void get_samples(LightSample *samples, int max_samples) const;
  virtual Color illuminate(const LightSample &sample, const Vector &Ps) const;
  virtual int preprocess();
  virtual void invalidate_shadow_cache();

  DeepShadowMap deep_shadow_;
};

} // namespace xxx

#endif // FJ_XXX_H
