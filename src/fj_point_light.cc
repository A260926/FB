// Copyright (c) 2011-2020 Hiroshi Tsubokawa
// See LICENSE and README

#include "fj_point_light.h"
#include "fj_shading.h"

namespace fj {

PointLight::PointLight()
{
}

PointLight::~PointLight()
{
}

int PointLight::get_sample_count() const
{
  return 1;
}

void PointLight::get_samples(LightSample *samples, int max_samples) const
{
  if (max_samples == 0)
    return;

  Transform transform_interp;
  // TODO time sampling
  const float time = 0;
  get_transform_sample(transform_interp, time);

  samples[0].P = transform_interp.translate;
  samples[0].N = Vector(0, 0, 0);
  samples[0].light = this;
}

Color PointLight::illuminate(const LightSample &sample, const Vector &Ps) const
{
  return GetIntensity() * GetColor();
}

int PointLight::preprocess()
{
  // does nothing
  return 0;
}

bool PointLight::BuildDeepShadowMap(const TraceContext &cxt, int resolution,
    Real maxDistance)
{
  if (resolution <= 0 || maxDistance <= 0) {
    return false;
  }

  // the light position comes from the same transform sample GetSamples uses
  Transform transform_interp;
  get_transform_sample(transform_interp, 0);
  const Vector light_pos = transform_interp.translate;

  DeepShadowMap cache;
  const int err = cache.Build(cxt, cxt.trace_target, light_pos,
      resolution, maxDistance);
  if (err) {
    return false;
  }

  deep_shadow_.Clear();
  deep_shadow_ = cache;
  return true;
}

void PointLight::ClearDeepShadowMap()
{
  deep_shadow_.Clear();
}

bool PointLight::LookupShadowVisibility(const TraceContext &cxt,
    const Vector &receiver, Real bias, Real *visibility) const
{
  if (!deep_shadow_.IsBuilt()) {
    return false;
  }

  // the cache is only valid for the group, time and capture distance it was
  // built with; any mismatch hands shading back to the regular shadow ray
  if (deep_shadow_.GetTarget() != cxt.trace_target ||
      deep_shadow_.GetTime() != cxt.time) {
    return false;
  }

  Transform transform_interp;
  get_transform_sample(transform_interp, 0);
  const Vector light_pos = transform_interp.translate;

  const Vector delta = receiver - light_pos;
  const Real distance = Length(delta);
  if (distance <= 0) {
    return false;
  }

  // only receivers inside the captured range are covered
  if (distance > deep_shadow_.GetMaxDistance()) {
    return false;
  }

  const Vector dir = delta / distance;
  *visibility = deep_shadow_.Lookup(dir, distance - bias);
  return true;
}

void PointLight::invalidate_shadow_cache()
{
  deep_shadow_.Clear();
}

} // namespace xxx
