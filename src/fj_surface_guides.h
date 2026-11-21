// Copyright (c) 2011-2020 Hiroshi Tsubokawa
// See LICENSE and README

#ifndef FJ_SURFACE_GUIDES_H
#define FJ_SURFACE_GUIDES_H

#include "fj_compatibility.h"
#include "fj_vector.h"
#include "fj_types.h"

#include <stdint.h>
#include <vector>

namespace fj {

// Per-sample surface guide record.
//
// Captured from the primary (camera) ray intersection while the regular
// shading for that same intersection is performed. Recursive rays
// (reflection/refraction/shadow/diffuse) never write this record, and no
// extra geometry queries or material evaluations are issued for it.
//
// A miss is represented by the zero record: id == 0 (reserved for the
// background), depth == 0 and normal == (0, 0, 0).
struct SurfaceGuideHit {
  SurfaceGuideHit() : id(0), depth(0), normal() {}

  uint32_t id;   // ObjectInstance guide id, 0 means background (no hit)
  Real depth;    // primary ray distance t_hit
  Vector normal; // normalized world space shading normal
};

// Per-pixel surface guide record produced by pixel reconstruction.
//
// id:       the guide id that collected the largest total filter weight
//           (background weight included). Ties are won by the smaller id.
// depth:    filter-weighted mean of the primary ray distances of the
//           winning id's samples. 0 when the winning id is 0.
// normal:   filter-weighted mean of the winning id's shading normals,
//           normalized at the end. (0, 0, 0) when the winning id is 0.
// coverage: total filter weight of non-background samples divided by the
//           total filter weight of all samples.
//
// When the sample set of a pixel is empty or the total weight is 0, all
// fields are 0.
struct SurfaceGuide {
  SurfaceGuide() : id(0), depth(0), normal(), coverage(0) {}

  uint32_t id;
  Real depth;
  Vector normal;
  Real coverage;
};

// Caller-owned destination of the surface guide AOVs.
//
// The buffer is handed to the renderer with Renderer::SetSurfaceGuides().
// Every RenderScene() call resets it to the current render resolution and
// fills the pixels inside the render region; pixels outside the render
// region are left as zeros. Volumes never contribute to these guides.
class FJ_API SurfaceGuideBuffer {
public:
  SurfaceGuideBuffer();
  ~SurfaceGuideBuffer();

  int GetWidth() const;
  int GetHeight() const;
  bool IsEmpty() const;

  // Read-only per-pixel access. Out-of-bounds queries return zeros.
  SurfaceGuide GetPixel(int x, int y) const;

  // The two functions below are used by the renderer during RenderScene().
  // They are not meant to be called while a render is running.

  // Resizes the buffer to (width, height) and clears every pixel to zeros.
  void Reset(int width, int height);
  void SetPixel(int x, int y, const SurfaceGuide &guide);

private:
  std::vector<SurfaceGuide> pixels_;
  int width_;
  int height_;
};

} // namespace xxx

#endif // FJ_XXX_H
