// Copyright (c) 2011-2020 Hiroshi Tsubokawa
// See LICENSE and README

#ifndef FJ_BVH_ACCELERATOR_H
#define FJ_BVH_ACCELERATOR_H

#include "fj_accelerator.h"
#include <vector>

namespace fj {

class BVHNode;

class BVHAccelerator : public Accelerator {
public:
  BVHAccelerator();
  ~BVHAccelerator();

public:
  virtual int build();
  virtual bool intersect(const Ray &ray, Real time, Intersection *isect) const;
  virtual void intersect_all(const Ray &ray, Real time,
      std::vector<Intersection> *hits) const;
  virtual const char *get_name() const;

  BVHNode *root;
};

} // namespace xxx

#endif // FJ_XXX_H
