// This file is part of Blend2D project <https://blend2d.com>
//
// See blend2d.h or LICENSE.md for license and copyright information
// SPDX-License-Identifier: Zlib

#ifndef BLEND2D_RASTER_MASKSAMPLER_P_H_INCLUDED
#define BLEND2D_RASTER_MASKSAMPLER_P_H_INCLUDED

#include <blend2d/core/image.h>
#include <blend2d/raster/rendercommand_p.h>

#include <algorithm>
#include <cmath>

//! \cond INTERNAL
//! \addtogroup blend2d_raster_engine_impl
//! \{

namespace bl::RasterEngine {
namespace MaskUSampler {

//! Samples the coverage of a single device pixel center `(dx + 0.5, dy + 0.5)` from a transformed A8 mask.
//!
//! The inverse mapping stored in `sd` converts the device pixel center into continuous mask coordinates, where
//! mask pixel `(i, j)` is centered at `(i + 0.5, j + 0.5)` and covers `[i, i + 1) x [j, j + 1)`. The mask is
//! bilinearly interpolated; mask pixels outside of the selected sub-rectangle contribute zero (the footprint is
//! never clamped to the edge and edge pixels are never repeated).
static BL_INLINE uint32_t sample_coverage(const BLImageImpl* mask_impl, const MaskUSourceData* sd, int dx, int dy) noexcept {
  double pxd = double(dx) + 0.5;
  double pyd = double(dy) + 0.5;

  double u = sd->inv_m00 * pxd + sd->inv_m10 * pyd + sd->inv_t20;
  double v = sd->inv_m01 * pxd + sd->inv_m11 * pyd + sd->inv_t21;

  // Mask pixel centers are at half-integers, shift the lattice to integers before interpolation.
  double gu = u - 0.5;
  double gv = v - 0.5;

  double fx = std::floor(gu);
  double fy = std::floor(gv);

  int64_t i0_64 = int64_t(fx);
  int64_t j0_64 = int64_t(fy);

  int32_t ax = sd->area_x;
  int32_t ay = sd->area_y;
  int32_t ax1 = ax + sd->area_w;
  int32_t ay1 = ay + sd->area_h;

  // Bilinear footprint [i0, i0 + 1] x [j0, j0 + 1] does not intersect the selected rectangle at all.
  if (i0_64 + 1 < int64_t(ax) || i0_64 >= int64_t(ax1) ||
      j0_64 + 1 < int64_t(ay) || j0_64 >= int64_t(ay1))
    return 0u;

  double tx = gu - fx;
  double ty = gv - fy;

  int i0 = int(i0_64);
  int j0 = int(j0_64);

  const uint8_t* pixels = static_cast<const uint8_t*>(mask_impl->pixel_data);
  intptr_t stride = mask_impl->stride;

  auto mask_pixel = [&](int i, int j) noexcept -> uint32_t {
    if (i < ax || i >= ax1 || j < ay || j >= ay1)
      return 0u;
    return uint32_t(pixels[intptr_t(j) * stride + intptr_t(i)]);
  };

  double w00 = (1.0 - tx) * (1.0 - ty);
  double w10 = tx * (1.0 - ty);
  double w01 = (1.0 - tx) * ty;
  double w11 = tx * ty;

  double cov = double(mask_pixel(i0, j0)) * w00 +
               double(mask_pixel(i0 + 1, j0)) * w10 +
               double(mask_pixel(i0, j0 + 1)) * w01 +
               double(mask_pixel(i0 + 1, j0 + 1)) * w11;

  // Rectangle clip coverage: overlap area of the device pixel square [dx, dx+1) x [dy, dy+1) with the clip box.
  double ox = std::max(0.0, std::min(double(dx) + 1.0, sd->clip_x1) - std::max(double(dx), sd->clip_x0));
  double oy = std::max(0.0, std::min(double(dy) + 1.0, sd->clip_y1) - std::max(double(dy), sd->clip_y0));
  cov *= ox * oy;

  if (cov <= 0.0)
    return 0u;
  if (cov >= 255.0)
    return 255u;
  return uint32_t(cov + 0.5);
}

//! Fills `dst` with the coverage of `w` device pixels of row `dy`, starting at device x coordinate `x0`.
static BL_INLINE void sample_scanline(uint8_t* dst, const BLImageImpl* mask_impl, const MaskUSourceData* sd,
                                      int x0, int dy, uint32_t w) noexcept {
  for (uint32_t k = 0; k < w; k++)
    dst[k] = uint8_t(sample_coverage(mask_impl, sd, x0 + int(k), dy));
}

} // {MaskUSampler}
} // {bl::RasterEngine}

//! \}
//! \endcond

#endif // BLEND2D_RASTER_MASKSAMPLER_P_H_INCLUDED
