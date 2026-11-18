// Copyright (c) 2011-2020 Hiroshi Tsubokawa
// See LICENSE and README

#include "fj_accelerator.h"
#include "fj_primitive_set.h"
#include "fj_intersection.h"
#include "fj_multi_thread.h"
#include "fj_ray.h"

#include <algorithm>
#include <iostream>

namespace fj {

static const Real PADDING = .0001;

class NullPrimitiveSet : public PrimitiveSet {
public:
  NullPrimitiveSet() {}
  virtual ~NullPrimitiveSet() {}

private:
  virtual bool ray_intersect(Index prim_id, const Ray &ray,
      Real time, Intersection *isect) const { return false; }
  virtual void get_primitive_bounds(Index prim_id, Box *bounds) const { *bounds = Box(); }
  virtual void get_bounds(Box *bounds) const { *bounds = Box(); }
  virtual Index get_primitive_count() const { return 0; }
};

static NullPrimitiveSet null_primset;

// for critical session
static void build_accelerator_callback(void *data);

Accelerator::Accelerator() : bounds_(), has_built_(false), primset_(NULL)
{
  SetPrimitiveSet(NULL);
}

Accelerator::~Accelerator()
{
}

Real Accelerator::GetBoundsPadding() const
{
  return PADDING;
}

const Box &Accelerator::GetBounds() const
{
  return bounds_;
}

const char *Accelerator::GetName() const
{
  return get_name();
}

bool Accelerator::HasBuilt() const
{
  return has_built_;
}

void Accelerator::ComputeBounds()
{
  primset_->GetEntireBounds(&bounds_);
  bounds_.Expand(GetBoundsPadding());
}

void Accelerator::SetPrimitiveSet(PrimitiveSet *primset)
{
  if (primset == NULL) {
    primset_ = &null_primset;
  } else {
    primset_ = primset;
  }

  ComputeBounds();
}

int Accelerator::Build()
{
  if (HasBuilt()) { 
    return -1;
  }

  const int err = build();
  if (err) {
    return -1;
  }

  has_built_ = true;
  return 0;
}

bool Accelerator::Intersect(const Ray &ray, Real time, Intersection *isect) const
{
  Real boxhit_tmin = 0;
  Real boxhit_tmax = 0;

  // check intersection with overall bounds
  const bool hit = BoxRayIntersect(bounds_, ray.orig, ray.dir, ray.tmin, ray.tmax,
        &boxhit_tmin, &boxhit_tmax);

  if (!hit) {
    return false;
  }

  if (0) {
    // dynamic build is now disabled. build all accelerators before rendering
    MtCriticalSection((void *) this, build_accelerator_callback);
  }

  return intersect(ray, time, isect);
}

void Accelerator::IntersectAllPrimitives(const Ray &ray, Real time,
    std::vector<int> *primitive_ids) const
{
  Real boxhit_tmin = 0;
  Real boxhit_tmax = 0;

  // check intersection with overall bounds, exactly like Intersect()
  const bool hit = BoxRayIntersect(bounds_, ray.orig, ray.dir, ray.tmin, ray.tmax,
        &boxhit_tmin, &boxhit_tmax);

  if (!hit) {
    return;
  }

  intersect_all_primitives(ray, time, primitive_ids);

  // a primitive may sit in several grid cells, so de-duplicate the list
  std::sort(primitive_ids->begin(), primitive_ids->end());
  primitive_ids->erase(
      std::unique(primitive_ids->begin(), primitive_ids->end()),
      primitive_ids->end());
}

void Accelerator::intersect_all_primitives(const Ray &ray, Real time,
    std::vector<int> *primitive_ids) const
{
  // default fallback: only the nearest hit is available
  Intersection isect;
  if (intersect(ray, time, &isect) && isect.object != NULL) {
    primitive_ids->push_back(isect.prim_id);
  }
}

static void build_accelerator_callback(void *data)
{
  Accelerator *acc = reinterpret_cast<Accelerator *>(data);

  if (!acc->HasBuilt()) {
    std::cout << "\nbuilding accelerator ... : " << acc->GetName() << std::endl;
    acc->Build();
  }
}

} // namespace xxx
