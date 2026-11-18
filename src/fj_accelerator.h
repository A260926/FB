// Copyright (c) 2011-2020 Hiroshi Tsubokawa
// See LICENSE and README

#ifndef FJ_ACCERALATOR_H
#define FJ_ACCERALATOR_H

#include "fj_types.h"
#include "fj_box.h"

#include <vector>

namespace fj {

class Intersection;
class PrimitiveSet;
class Ray;

class Accelerator {
public:
  Accelerator();
  virtual ~Accelerator();

  Real GetBoundsPadding() const;
  const Box &GetBounds() const;
  const char *GetName() const;
  bool HasBuilt() const;

  void ComputeBounds();
  void SetPrimitiveSet(PrimitiveSet *primset);
  int Build();
  bool Intersect(const Ray &ray, Real time, Intersection *isect) const;

  const PrimitiveSet *GetPrimitiveSet() const { return primset_; }

  // Enumerates every primitive whose accelerator node or grid cell is a
  // candidate for the ray. Unlike Intersect(), the ray range is never shrunk
  // as candidates are found, so layers behind the nearest hit stay visible.
  // Each primitive id is reported at most once.
  void IntersectAllPrimitives(const Ray &ray, Real time,
      std::vector<int> *primitive_ids) const;

private:
  virtual int build() = 0;
  virtual bool intersect(const Ray &ray, Real time, Intersection *isect) const = 0;
  virtual const char *get_name() const = 0;
  virtual void intersect_all_primitives(const Ray &ray, Real time,
      std::vector<int> *primitive_ids) const;

  Box bounds_;
  bool has_built_;

  PrimitiveSet *primset_;
};

} // namespace xxx

#endif // FJ_XXX_H
