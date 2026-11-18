// Copyright (c) 2011-2020 Hiroshi Tsubokawa
// See LICENSE and README

#ifndef FJ_DEEP_SHADOW_H
#define FJ_DEEP_SHADOW_H

#include "fj_compatibility.h"
#include "fj_types.h"
#include "fj_vector.h"

#include <vector>

namespace fj {

class TraceContext;
class ObjectGroup;

// One right-continuous breakpoint of a transmittance staircase.
// T(d) equals transmittance for every distance d >= distance, until the
// next breakpoint. Layers hit at identical radial distance are merged into
// a single breakpoint carrying the product of all their factors.
struct DeepShadowBreakpoint {
  DeepShadowBreakpoint() : distance(0), transmittance(1) {}
  DeepShadowBreakpoint(Real dist, Real trans) : distance(dist), transmittance(trans) {}
  ~DeepShadowBreakpoint() {}

  Real distance;
  Real transmittance;
};

// Six-face cube map of per-texel transmittance staircases captured around a
// point light. The cache owns every piece of its data and keeps no reference
// to the geometry or shaders it was built from, so a changed scene simply
// requires a rebuild by the caller.
//
// Face order is +X, -X, +Y, -Y, +Z, -Z. Coordinate and filtering rules live
// in task_support/cube_contract.md.
class FJ_API DeepShadowMap {
public:
  DeepShadowMap();
  ~DeepShadowMap();

  // Builds the cache by casting 6*resolution*resolution rays from light_pos
  // through target at cxt.time. Returns 0 on success and -1 when the target
  // contains anything other than triangle mesh surfaces (curves, point
  // clouds or volumes are explicitly rejected).
  int Build(const TraceContext &cxt, const ObjectGroup *target,
      const Vector &light_pos, int resolution, Real max_distance);
  void Clear();

  bool IsBuilt() const { return built_; }
  int GetResolution() const { return resolution_; }
  Real GetMaxDistance() const { return max_distance_; }
  Real GetTime() const { return time_; }
  const ObjectGroup *GetTarget() const { return target_; }

  // Transmittance at radial distance d along the given receiver direction,
  // using the four-neighbour cross-face filtering of cube_contract.md.
  // Each neighbour evaluates its own staircase at d before the four
  // transmittances are bilinearly weighted.
  Real Lookup(const Vector &receiver_dir, Real distance_minus_bias) const;

  // Staircase of one stored texel. Only real breakpoints are stored; the
  // implicit T=1 value before the first breakpoint is supplied on lookup.
  const std::vector<DeepShadowBreakpoint> &GetStaircase(int face, int x, int y) const;

private:
  // Right-continuous step of one staircase: T(d) for breakpoints[index-1]
  // .distance <= d < breakpoints[index].distance.
  static Real staircase_transmittance(
      const std::vector<DeepShadowBreakpoint> &staircase, Real d);

  // Cube mapping helpers (see task_support/cube_contract.md).
  static Vector texel_direction(int face, int x, int y, int resolution);
  static void direction_to_face_uv(const Vector &dir,
      int *face, Real *u, Real *v);

  int resolution_;
  Real max_distance_;
  Real time_;
  const ObjectGroup *target_;
  bool built_;

  // 6 * resolution * resolution staircases, index face*N*N + y*N + x.
  std::vector<std::vector<DeepShadowBreakpoint> > staircases_;
};

} // namespace xxx

#endif // FJ_XXX_H
