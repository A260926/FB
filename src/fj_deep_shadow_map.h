// Copyright (c) 2011-2020 Hiroshi Tsubokawa
// See LICENSE and README

#ifndef FJ_DEEP_SHADOW_MAP_H
#define FJ_DEEP_SHADOW_MAP_H

#include "fj_compatibility.h"
#include "fj_vector.h"
#include "fj_types.h"
#include <vector>
#include <stdint.h>

namespace fj {

class TraceContext;
class ObjectGroup;

// DeepShadowMap is a reusable transmittance cache for one point light.
// It captures the six cube directions around the light; every texel owns
// a staircase of accumulated transmittance as a function of radial
// distance from the light. The map owns its data: it keeps no references
// to Intersection or Shader objects used while building. When the scene
// changes, the caller is responsible for rebuilding it.
class FJ_API DeepShadowMap {
public:
  DeepShadowMap();
  ~DeepShadowMap();

  // Captures the scene visible from light_position. cxt.trace_target
  // selects the object group and cxt.time the sample time. Returns false
  // and leaves the map cleared when the target cannot be captured
  // (unsupported geometry, non-finite shader opacity, bad arguments).
  bool Build(const TraceContext &cxt, const Vector &light_position,
      int resolution, Real max_distance);
  void Clear();
  bool HasBuilt() const;

  // Reports the transmittance towards receiver when the group, time and
  // radial coverage match the build. Returns false without touching
  // visibility when the cache does not apply.
  bool Lookup(const TraceContext &cxt, const Vector &receiver,
      Real bias, Real *visibility) const;

  int GetResolution() const;
  Real GetMaxDistance() const;

private:
  struct Texel {
    Texel() : offset(0), count(0) {}
    Texel(uint32_t off, uint32_t cnt) : offset(off), count(cnt) {}
    uint32_t offset;
    uint32_t count;
  };

  Real eval_texel(int face, int x, int y, Real distance) const;

  // 6 * resolution * resolution texels, row-major per face
  std::vector<Texel> texels_;
  // breakpoints shared by all texels: distance[i] is where the
  // transmittance becomes transmission[i] (right-continuous)
  std::vector<Real> bp_distance_;
  std::vector<Real> bp_transmission_;

  Vector light_position_;
  const ObjectGroup *trace_target_;
  Real time_;
  Real max_distance_;
  int resolution_;
  bool has_built_;
};

} // namespace xxx

#endif // FJ_DEEP_SHADOW_MAP_H
