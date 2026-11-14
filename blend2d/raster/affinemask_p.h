// This file is part of Blend2D project <https://blend2d.com>
//
// See blend2d.h or LICENSE.md for license and copyright information
// SPDX-License-Identifier: Zlib

#ifndef BLEND2D_RASTER_AFFINEMASK_P_H_INCLUDED
#define BLEND2D_RASTER_AFFINEMASK_P_H_INCLUDED

#include <blend2d/core/geometry.h>
#include <blend2d/core/matrix_p.h>
#include <blend2d/pipeline/pipedefs_p.h>
#include <blend2d/pixelops/scalar_p.h>
#include <blend2d/raster/rasterdefs_p.h>
#include <blend2d/raster/workdata_p.h>
#include <blend2d/support/math_p.h>

//! \cond INTERNAL
//! \addtogroup blend2d_raster_engine_impl
//! \{

namespace bl::RasterEngine {

//! Data required to rasterize an A8 mask that is positioned through the context's final affine transform.
//!
//! The structure is allocated in the rendering batch allocator by the asynchronous rendering context, so it
//! outlives the render call and is only valid until the batch is finalized. The mask image impl is retained
//! separately by the render command that owns this data.
struct AffineMaskData {
  //! Rasterized coverage buffer, one byte per device pixel, row major, already multiplied with global/fill alpha.
  uint8_t* coverage;
  //! Pointer to the retained mask image pixels.
  const uint8_t* mask_pixels;
  //! Stride of the mask image in bytes.
  intptr_t mask_stride;
  //! Top-left corner of the selected mask area, in mask image coordinates.
  int mask_x;
  int mask_y;
  //! Size of the selected mask area.
  int mask_w;
  int mask_h;
  //! Device pixel box the coverage was rasterized into.
  BLBoxI box;
  //! Coefficients mapping a device pixel center (x, y) to mask selection-local coordinates:
  //!   u = c00*x + c01*y + c02
  //!   v = c10*x + c11*y + c12
  double c00, c01, c02;
  double c10, c11, c12;
  //! Combined global alpha and fill alpha [0, 255], baked into the coverage.
  uint32_t alpha;
};

namespace AffineMaskInternal {

//! Samples the selected A8 area at selection-local coordinates `u, v` using bilinear interpolation.
//!
//! Source pixel centers are at half-integer coordinates. Taps outside of the selected area contribute zero - the
//! footprint is never clamped to the edge and the last valid pixel is not repeated or read implicitly.
static BL_INLINE uint32_t sample_a8_bilinear(const AffineMaskData& md, double u, double v) noexcept {
  double fx = u - 0.5;
  double fy = v - 0.5;

  int ix = int(Math::floor(fx));
  int iy = int(Math::floor(fy));

  double tx = fx - double(ix);
  double ty = fy - double(iy);
  double ntx = 1.0 - tx;
  double nty = 1.0 - ty;

  auto load_tap = [&](int x, int y) noexcept -> uint32_t {
    if ((uint32_t(x) >= uint32_t(md.mask_w)) | (uint32_t(y) >= uint32_t(md.mask_h)))
      return 0u;
    return uint32_t(md.mask_pixels[intptr_t(md.mask_y + y) * md.mask_stride + intptr_t(md.mask_x + x)]);
  };

  uint32_t p00 = load_tap(ix,     iy);
  uint32_t p10 = load_tap(ix + 1, iy);
  uint32_t p01 = load_tap(ix,     iy + 1);
  uint32_t p11 = load_tap(ix + 1, iy + 1);

  double cov = double(p00) * (ntx * nty) + double(p10) * (tx * nty) +
               double(p01) * (ntx * ty)  + double(p11) * (tx * ty);

  int value = int(cov + 0.5);
  return uint32_t(bl_clamp(value, 0, 255));
}

} // {AffineMaskInternal}

//! Calculates the device pixel box touched by the transformed selection and the coefficients used to invert
//! the final transform per device pixel center.
//!
//! The box is expanded by one source pixel in mask space, so every device pixel whose bilinear footprint can
//! intersect the selected area is included. Pixels outside of this expanded region provably sample zero.
static BL_INLINE BLResult affine_mask_prepare(
    const BLMatrix2D& final_transform, const BLMatrix2D& inverse_transform,
    const BLPoint& origin, const BLRectI& mask_rect, const BLBox& clip_box_d, const BLBoxI& clip_box_i,
    AffineMaskData& out) noexcept {

  if (!Math::is_finite(origin) || !Math::is_finite(inverse_transform.m00, inverse_transform.m01,
                                                   inverse_transform.m10, inverse_transform.m11,
                                                   inverse_transform.m20, inverse_transform.m21))
    return bl_make_error(BL_ERROR_INVALID_VALUE);

  // Local corners expanded by one source pixel on every side.
  double lx[4] = { -1.0, double(mask_rect.w) + 1.0, -1.0, double(mask_rect.w) + 1.0 };
  double ly[4] = { -1.0, -1.0, double(mask_rect.h) + 1.0, double(mask_rect.h) + 1.0 };

  double min_x = Math::inf<double>();
  double min_y = Math::inf<double>();
  double max_x = -Math::inf<double>();
  double max_y = -Math::inf<double>();

  for (int k = 0; k < 4; k++) {
    BLPoint p = final_transform.map_point(origin.x + lx[k], origin.y + ly[k]);
    min_x = bl_min(min_x, p.x);
    min_y = bl_min(min_y, p.y);
    max_x = bl_max(max_x, p.x);
    max_y = bl_max(max_y, p.y);
  }

  min_x = bl_max(min_x, clip_box_d.x0);
  min_y = bl_max(min_y, clip_box_d.y0);
  max_x = bl_min(max_x, clip_box_d.x1);
  max_y = bl_min(max_y, clip_box_d.y1);

  if (!((min_x < max_x) & (min_y < max_y)))
    return BL_SUCCESS;

  // Select device pixels whose centers fall inside the (possibly fractional) clipped geometry bounds. A pixel
  // column x has center x + 0.5, so x is included when min <= x + 0.5 < max, i.e. ceil(min - 0.5) <= x <
  // ceil(max - 0.5). This applies fractional clipping precisely without storing an extra clip rectangle.
  int x0 = Math::ceil_to_int(min_x - 0.5);
  int y0 = Math::ceil_to_int(min_y - 0.5);
  int x1 = Math::ceil_to_int(max_x - 0.5);
  int y1 = Math::ceil_to_int(max_y - 0.5);

  x0 = bl_max(x0, clip_box_i.x0);
  y0 = bl_max(y0, clip_box_i.y0);
  x1 = bl_min(x1, clip_box_i.x1);
  y1 = bl_min(y1, clip_box_i.y1);

  if (!((x0 < x1) & (y0 < y1)))
    return BL_SUCCESS;

  // Inverse mapping of a device pixel center (x + 0.5, y + 0.5) to selection-local coordinates.
  const BLMatrix2D& inv = inverse_transform;
  out.c00 = inv.m00;
  out.c01 = inv.m10;
  out.c02 = inv.m20 + 0.5 * (inv.m00 + inv.m10) - origin.x;
  out.c10 = inv.m01;
  out.c11 = inv.m11;
  out.c12 = inv.m21 + 0.5 * (inv.m01 + inv.m11) - origin.y;

  out.box.reset(x0, y0, x1, y1);
  return BL_SUCCESS;
}

//! Rasterizes the whole coverage buffer of `md` and bakes the combined alpha into it.
static BL_INLINE void affine_mask_rasterize(AffineMaskData& md) noexcept {
  const int w = md.box.x1 - md.box.x0;
  const int h = md.box.y1 - md.box.y0;
  const uint32_t alpha = md.alpha;

  uint8_t* row = md.coverage;
  for (int y = 0; y < h; y++) {
    double device_y = double(md.box.y0 + y);
    double v = md.c10 * double(md.box.x0) + md.c11 * device_y + md.c12;
    double u = md.c00 * double(md.box.x0) + md.c01 * device_y + md.c02;

    for (int x = 0; x < w; x++) {
      uint32_t cov = AffineMaskInternal::sample_a8_bilinear(md, u, v);
      row[x] = uint8_t(PixelOps::Scalar::udiv255(cov * alpha));
      u += md.c00;
      v += md.c10;
    }
    row += w;
  }
}

//! Composites a horizontal range of already rasterized scanlines through the current fill pipeline.
//!
//! The coverage is passed as a variable A8 mask that already contains the global/fill alpha, so the pipeline is
//! always told to use `kVMaskA8WithGA` and the fill data alpha is set to opaque. Both the synchronous and the
//! asynchronous rendering paths use this function, which is what makes their results byte-identical.
static BL_INLINE BLResult affine_mask_composite_rows(
    Pipeline::ContextData& ctx_data, const Pipeline::DispatchData& dispatch_data,
    const void* fetch_data, const AffineMaskData& md, int y0, int y1) noexcept {

  if (y0 >= y1)
    return BL_SUCCESS;

  const int x0 = md.box.x0;
  const int x1 = md.box.x1;
  const int box_w = x1 - x0;

  Pipeline::FillFunc fill_func = dispatch_data.fill_func;
  if (!fill_func)
    return bl_make_error(BL_ERROR_NOT_IMPLEMENTED);

  const uint8_t* coverage_row = md.coverage + intptr_t(y0 - md.box.y0) * box_w;

  for (int y = y0; y < y1; y++) {
    Pipeline::MaskCommand mask_commands[2];
    mask_commands[0].init_vmask_a8_with_ga(uint32_t(x0), uint32_t(x1), coverage_row, 0);
    mask_commands[1].init_end();

    Pipeline::FillData fill_data;
    fill_data.init_mask_a(255u, x0, y, x1, y + 1, mask_commands);
    fill_func(&ctx_data, &fill_data, fetch_data);

    coverage_row += box_w;
  }

  return BL_SUCCESS;
}

} // {bl::RasterEngine}

//! \}
//! \endcond

#endif // BLEND2D_RASTER_AFFINEMASK_P_H_INCLUDED
