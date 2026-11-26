// Copyright (c) 2011-2020 Hiroshi Tsubokawa
// See LICENSE and README

#ifndef FJ_POINT_LIGHT_H
#define FJ_POINT_LIGHT_H

#include "fj_deep_shadow_map.h"
#include "fj_light.h"

namespace fj {

class PointLight : public Light {
public:
  PointLight();
  virtual ~PointLight();

  // Captures a deep shadow map around this light for the trace target
  // and time in cxt. The light position is the existing GetSamples
  // position. Returns false when the target cannot be captured.
  bool BuildDeepShadowMap(const TraceContext &cxt, int resolution, Real maxDistance);
  void ClearDeepShadowMap();
  bool HasDeepShadowMap() const;

  virtual bool LookupShadowVisibility(const TraceContext &cxt,
      const Vector &receiver, Real bias, Real *visibility) const;

private:
  virtual int get_sample_count() const;
  virtual void get_samples(LightSample *samples, int max_samples) const;
  virtual Color illuminate(const LightSample &sample, const Vector &Ps) const;
  virtual int preprocess();
  virtual void transform_changed();

  DeepShadowMap deep_shadow_map_;
};

} // namespace xxx

#endif // FJ_XXX_H
