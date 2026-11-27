// Task tests for transformed A8 mask fills (fill_mask with fractional origin / rotation / scale).
//
// Registered through register_task_tests(Registry&); this file has no main().

#include "registry.hpp"

#include <blend2d/blend2d.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace {

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static uint32_t get_pixel(const BLImage& img, int x, int y) {
  BLImageData d;
  img.get_data(&d);
  return reinterpret_cast<const uint32_t*>(static_cast<const uint8_t*>(d.pixel_data) + intptr_t(y) * d.stride)[x];
}

static BLImage make_a8(int w, int h, const std::vector<uint8_t>& values) {
  BLImage mask(w, h, BL_FORMAT_A8);
  BLImageData md;
  mask.make_mutable(&md);
  for (int y = 0; y < h; y++)
    for (int x = 0; x < w; x++)
      static_cast<uint8_t*>(md.pixel_data)[intptr_t(y) * md.stride + x] = values[size_t(y) * w + x];
  return mask;
}

static BLImage make_prgb(int w, int h, const std::vector<uint32_t>& values) {
  BLImage img(w, h, BL_FORMAT_PRGB32);
  BLImageData md;
  img.make_mutable(&md);
  for (int y = 0; y < h; y++)
    for (int x = 0; x < w; x++)
      reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(md.pixel_data) + intptr_t(y) * md.stride)[x] = values[size_t(y) * w + x];
  return img;
}

// Blend2D does not zero pixel storage of a newly created image, so tests that assume a transparent target must
// clear it explicitly.
static BLImage blank_prgb(int w, int h) {
  return make_prgb(w, h, std::vector<uint32_t>(size_t(w) * h, 0u));
}

static bool images_equal(const BLImage& a, const BLImage& b) {
  BLImageData da, db;
  a.get_data(&da);
  b.get_data(&db);
  if (da.size != db.size)
    return false;
  for (int y = 0; y < da.size.h; y++) {
    const uint8_t* ra = static_cast<const uint8_t*>(da.pixel_data) + intptr_t(y) * da.stride;
    const uint8_t* rb = static_cast<const uint8_t*>(db.pixel_data) + intptr_t(y) * db.stride;
    if (std::memcmp(ra, rb, size_t(da.size.w) * 4) != 0)
      return false;
  }
  return true;
}

static uint32_t channel_diff_max(const BLImage& a, const BLImage& b) {
  uint32_t worst = 0;
  BLImageData da, db;
  a.get_data(&da);
  b.get_data(&db);
  for (int y = 0; y < da.size.h; y++) {
    for (int x = 0; x < da.size.w; x++) {
      uint32_t pa = get_pixel(a, x, y);
      uint32_t pb = get_pixel(b, x, y);
      for (int k = 0; k < 4; k++) {
        worst = std::max(worst, uint32_t(std::abs(int((pa >> (k * 8)) & 0xff) - int((pb >> (k * 8)) & 0xff))));
      }
    }
  }
  return worst;
}

// Independent inverse-matrix bilinear coverage, mirroring the required sampling contract.
static uint32_t ref_coverage(const BLImage& mask, const BLRectI& area, const BLMatrix2D& inv, int dx, int dy) {
  double u = inv.m00 * (dx + 0.5) + inv.m10 * (dy + 0.5) + inv.m20;
  double v = inv.m01 * (dx + 0.5) + inv.m11 * (dy + 0.5) + inv.m21;

  double gu = u - 0.5;
  double gv = v - 0.5;
  double fx = std::floor(gu);
  double fy = std::floor(gv);

  int i0 = int(fx);
  int j0 = int(fy);
  double tx = gu - fx;
  double ty = gv - fy;

  BLImageData md;
  mask.get_data(&md);
  auto at = [&](int i, int j) -> double {
    if (i < area.x || i >= area.x + area.w || j < area.y || j >= area.y + area.h)
      return 0.0;
    return double(static_cast<const uint8_t*>(md.pixel_data)[intptr_t(j) * md.stride + i]);
  };

  double cov = at(i0, j0) * (1.0 - tx) * (1.0 - ty)
             + at(i0 + 1, j0) * tx * (1.0 - ty)
             + at(i0, j0 + 1) * (1.0 - tx) * ty
             + at(i0 + 1, j0 + 1) * tx * ty;

  if (cov <= 0.0) return 0u;
  if (cov >= 255.0) return 255u;
  return uint32_t(cov + 0.5);
}

// Builds the device->mask inverse for a fill_mask call: selection corner (area.x, area.y) lands on `origin`.
static BLMatrix2D make_inverse(const BLMatrix2D& final_transform, const BLPoint& origin, const BLRectI& area) {
  BLMatrix2D m2d(final_transform);
  m2d.translate(origin.x - double(area.x), origin.y - double(area.y));
  BLMatrix2D inv;
  BLResult r = BLMatrix2D::invert(inv, m2d);
  (void)r;
  return inv;
}

static inline uint32_t udiv255_d(double x) {
  if (x <= 0.0) return 0u;
  if (x >= 255.0) return 255u;
  return uint32_t(x + 0.5);
}

// Reference combine of a style pixel and a destination pixel using coverage (already including global alpha when
// `ga_applied` is false). Style pixels are premultiplied.
static uint32_t ref_combine(uint32_t dst, uint32_t src, uint32_t m, BLCompOp comp) {
  double f = double(m) / 255.0;
  int da = int((dst >> 24) & 0xff), dr = int((dst >> 16) & 0xff), dg = int((dst >> 8) & 0xff), db = int(dst & 0xff);
  int sa = int((src >> 24) & 0xff), sr = int((src >> 16) & 0xff), sg = int((src >> 8) & 0xff), sb = int(src & 0xff);

  int oa, orr, og, ob;
  if (comp == BL_COMP_OP_SRC_COPY) {
    // D' = D*(1 - m) + S*m, interpolation between old target and source.
    oa = int(std::round(da * (1.0 - f) + sa * f));
    orr = int(std::round(dr * (1.0 - f) + sr * f));
    og = int(std::round(dg * (1.0 - f) + sg * f));
    ob = int(std::round(db * (1.0 - f) + sb * f));
  }
  else {
    // SRC_OVER with source scaled by mask m.
    double sf = f;
    oa = int(std::round(sa * sf + da * (1.0 - sf)));
    orr = int(std::round(sr * sf + dr * (1.0 - sf)));
    og = int(std::round(sg * sf + dg * (1.0 - sf)));
    ob = int(std::round(sb * sf + db * (1.0 - sf)));
  }
  auto cl = [](int v) -> uint32_t { return uint32_t(std::clamp(v, 0, 255)); };
  return (cl(oa) << 24) | (cl(orr) << 16) | (cl(og) << 8) | cl(ob);
}

// Renders a full-coverage style reference into a fresh image, using the same context transform and clip so that
// style coordinates match the masked rendering exactly.
template<typename StyleT>
static BLImage render_style_plate(int w, int h, const BLMatrix2D& transform, const BLRectI& clip,
                                  const StyleT& style, BLCompOp comp, double global_alpha) {
  BLImage plate(w, h, BL_FORMAT_PRGB32);
  BLContext c(plate);
  c.set_comp_op(comp);
  c.set_global_alpha(global_alpha);
  c.set_transform(transform);
  c.clip_to_rect(BLRect(double(clip.x), double(clip.y), double(clip.w), double(clip.h)));
  c.fill_rect(BLRect(0.0, 0.0, double(w), double(h)), style);
  c.end();
  return plate;
}

// Generic reference: combines background, style plate and per-pixel coverage.
static BLImage render_reference(const BLImage& background, const BLImage& plate, const BLImage& mask,
                                const BLRectI& area, const BLMatrix2D& inv, const BLRectI& box,
                                uint32_t style_alpha, BLCompOp comp) {
  BLImage ref;
  ref.create(background.size().w, background.size().h, BL_FORMAT_PRGB32);
  BLImageData rd;
  ref.make_mutable(&rd);
  int w = background.size().w, h = background.size().h;
  for (int y = 0; y < h; y++) {
    for (int x = 0; x < w; x++) {
      uint32_t v = get_pixel(background, x, y);
      if (x >= box.x && x < box.x + box.w && y >= box.y && y < box.y + box.h) {
        uint32_t cov = ref_coverage(mask, area, inv, x, y);
        uint32_t m = udiv255_d(double(cov) * double(style_alpha) / 255.0);
        v = ref_combine(v, get_pixel(plate, x, y), m, comp);
      }
      reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(rd.pixel_data) + intptr_t(y) * rd.stride)[x] = v;
    }
  }
  return ref;
}

// ---------------------------------------------------------------------------
// sampling group
// ---------------------------------------------------------------------------

void sampling_subarea(Case& c) {
  // 6x5 mask, selected 3x2 area at (2,1), nonzero pixels all around it.
  std::vector<uint8_t> values;
  for (int y = 0; y < 5; y++)
    for (int x = 0; x < 6; x++)
      values.push_back(uint8_t(15 + x * 17 + y * 29));
  BLImage mask = make_a8(6, 5, values);
  BLRectI area(2, 1, 3, 2);

  const int W = 28, H = 24;
  BLImage img = blank_prgb(W, H);
  BLContext ctx(img);
  ctx.set_fill_style(BLRgba32(0xffff0000));
  BLPoint origin(5.25, 4.75);
  BLResult r = ctx.fill_mask(origin, mask, area);
  c.check(r == BL_SUCCESS, "sub-area fractional fill_mask succeeds");
  ctx.end();

  BLMatrix2D inv = make_inverse(BLMatrix2D::make_identity(), origin, area);
  int outside_touched = 0;
  int mismatches = 0;
  for (int y = 0; y < H; y++) {
    for (int x = 0; x < W; x++) {
      uint32_t cov = ref_coverage(mask, area, inv, x, y);
      uint32_t got = get_pixel(img, x, y);
      if (cov == 0u) {
        if (got != 0u) outside_touched++;
      }
      else {
        uint32_t expect = (cov << 24) | (cov << 16); // premultiplied red
        uint32_t d = channel_diff_max(
          make_prgb(1, 1, {got}), make_prgb(1, 1, {expect}));
        if (d > 3) mismatches++;
      }
    }
  }
  c.check(outside_touched == 0, "zero-coverage and out-of-selection neighbors are not written");
  c.check(mismatches == 0, "sub-area bilinear coverage matches independent formula (tol 3)");
}

void sampling_fractional_translation(Case& c) {
  std::vector<uint8_t> values;
  for (int y = 0; y < 4; y++)
    for (int x = 0; x < 4; x++)
      values.push_back(uint8_t(x * 40 + y * 15 + 10));
  BLImage mask = make_a8(4, 4, values);
  BLRectI area(0, 0, 4, 4);

  const int W = 20, H = 20;
  BLImage img = blank_prgb(W, H);
  BLContext ctx(img);
  ctx.set_fill_style(BLRgba32(0xff00ff00));
  BLPoint origin(3.3, 2.8);
  c.check(ctx.fill_mask(origin, mask, area) == BL_SUCCESS, "fractional translation fill");
  ctx.end();

  BLMatrix2D inv = make_inverse(BLMatrix2D::make_identity(), origin, area);
  int mismatches = 0;
  for (int y = 0; y < H; y++)
    for (int x = 0; x < W; x++) {
      uint32_t cov = ref_coverage(mask, area, inv, x, y);
      uint32_t got = get_pixel(img, x, y);
      uint32_t expect = (cov << 24) | (cov << 8);
      if (channel_diff_max(make_prgb(1, 1, {got}), make_prgb(1, 1, {expect})) > 3)
        mismatches++;
    }
  c.check(mismatches == 0, "fractional translation matches inverse-matrix reference");
}

void sampling_rotation(Case& c) {
  std::vector<uint8_t> values;
  for (int y = 0; y < 5; y++)
    for (int x = 0; x < 5; x++)
      values.push_back(uint8_t(25 + x * 23 + y * 19));
  BLImage mask = make_a8(5, 5, values);
  BLRectI area(1, 1, 3, 3);

  const int W = 40, H = 40;
  BLMatrix2D transform;
  transform.reset(1, 0, 0, 1, 20.0, 20.0);
  transform.rotate(0.7);

  BLImage img = blank_prgb(W, H);
  BLContext ctx(img);
  ctx.set_transform(transform);
  ctx.set_fill_style(BLRgba32(0xff8040c0));
  BLPoint origin(-2.0, -1.5);
  c.check(ctx.fill_mask(origin, mask, area) == BL_SUCCESS, "rotated fill");
  ctx.end();

  BLMatrix2D inv = make_inverse(transform, origin, area);
  int mismatches = 0;
  for (int y = 0; y < H; y++)
    for (int x = 0; x < W; x++) {
      uint32_t cov = ref_coverage(mask, area, inv, x, y);
      uint32_t got = get_pixel(img, x, y);
      uint32_t expect = (cov << 24)
                      | (uint32_t(std::round(0x80 * cov / 255.0)) << 16)
                      | (uint32_t(std::round(0x40 * cov / 255.0)) << 8)
                      | uint32_t(std::round(0xc0 * cov / 255.0));
      if (channel_diff_max(make_prgb(1, 1, {got}), make_prgb(1, 1, {expect})) > 3)
        mismatches++;
    }
  c.check(mismatches == 0, "rotation coverage matches independent formula");
}

void sampling_reflection_shear_scale(Case& c) {
  std::vector<uint8_t> values;
  for (int y = 0; y < 3; y++)
    for (int x = 0; x < 4; x++)
      values.push_back(uint8_t(40 + x * 30 + y * 50));
  BLImage mask = make_a8(4, 3, values);
  BLRectI area(0, 0, 4, 3);

  const int W = 48, H = 36;

  // Reflection + non-uniform scale + shear combined.
  BLMatrix2D transform(-1.4, 0.35, 0.25, 0.8, 30.0, 6.0);

  BLImage img = blank_prgb(W, H);
  BLContext ctx(img);
  ctx.set_transform(transform);
  ctx.set_fill_style(BLRgba32(0xff20a0f0));
  BLPoint origin(1.0, 0.5);
  c.check(ctx.fill_mask(origin, mask, area) == BL_SUCCESS, "reflection/shear/scale fill");
  ctx.end();

  BLMatrix2D inv = make_inverse(transform, origin, area);
  int mismatches = 0;
  for (int y = 0; y < H; y++)
    for (int x = 0; x < W; x++) {
      uint32_t cov = ref_coverage(mask, area, inv, x, y);
      uint32_t got = get_pixel(img, x, y);
      uint32_t expect = (cov << 24)
                      | (uint32_t(std::round(0x20 * cov / 255.0)) << 16)
                      | (uint32_t(std::round(0xa0 * cov / 255.0)) << 8)
                      | uint32_t(std::round(0xf0 * cov / 255.0));
      if (channel_diff_max(make_prgb(1, 1, {got}), make_prgb(1, 1, {expect})) > 3)
        mismatches++;
    }
  c.check(mismatches == 0, "reflection, shear and non-uniform scale match reference");
}

void sampling_filter_boundary(Case& c) {
  // 3x3 mask, selected single pixel region; the bilinear footprint crosses the selection edge and must
  // pick up zero contribution instead of clamping or repeating the edge pixel.
  std::vector<uint8_t> values = {
    200, 200, 200,
    200, 200, 200,
    200, 200, 200
  };
  BLImage mask = make_a8(3, 3, values);
  BLRectI area(1, 1, 1, 1); // only the center pixel is selected

  const int W = 12, H = 12;
  BLImage img = blank_prgb(W, H);
  BLContext ctx(img);
  ctx.set_fill_style(BLRgba32(0xffffffff));
  BLPoint origin(4.0, 4.0);
  c.check(ctx.fill_mask(origin, mask, area) == BL_SUCCESS, "boundary footprint fill");
  ctx.end();

  BLMatrix2D inv = make_inverse(BLMatrix2D::make_identity(), origin, area);
  int mismatches = 0;
  for (int y = 0; y < H; y++)
    for (int x = 0; x < W; x++) {
      uint32_t cov = ref_coverage(mask, area, inv, x, y);
      uint32_t got = get_pixel(img, x, y);
      uint32_t expect = cov ? ((cov << 24) | (cov << 16) | (cov << 8) | cov) : 0u;
      if (channel_diff_max(make_prgb(1, 1, {got}), make_prgb(1, 1, {expect})) > 3)
        mismatches++;
    }
  c.check(mismatches == 0, "footprint crossing selection edge uses zero outside contribution");

  // A pixel whose footprint is entirely beside the selected pixel must be zero.
  c.check(ref_coverage(mask, area, inv, 2, 4) == 0u, "neighboring footprint evaluates to zero");
}

void sampling_upscale_bounds(Case& c) {
  BLImage mask = make_a8(2, 2, {255, 255, 255, 255});
  BLRectI area(0, 0, 2, 2);

  const int W = 40, H = 40;
  BLMatrix2D transform = BLMatrix2D::make_scaling(4.0, 4.0);
  transform.post_translate(8.0, 8.0);

  BLImage img = blank_prgb(W, H);
  BLContext ctx(img);
  ctx.set_transform(transform);
  ctx.set_fill_style(BLRgba32(0xffffffff));
  BLPoint origin(1.0, 1.0);
  c.check(ctx.fill_mask(origin, mask, area) == BL_SUCCESS, "upscaled fill");
  ctx.end();

  BLMatrix2D inv = make_inverse(transform, origin, area);
  int mismatches = 0;
  int nonzero = 0;
  for (int y = 0; y < H; y++)
    for (int x = 0; x < W; x++) {
      uint32_t cov = ref_coverage(mask, area, inv, x, y);
      uint32_t got = get_pixel(img, x, y);
      if (cov) nonzero++;
      uint32_t expect = cov ? ((cov << 24) | (cov << 16) | (cov << 8) | cov) : 0u;
      if (channel_diff_max(make_prgb(1, 1, {got}), make_prgb(1, 1, {expect})) > 3)
        mismatches++;
    }
  c.check(nonzero > 20, "upscaled mask covers a multi-pixel device region");
  c.check(mismatches == 0, "upscale device box is not truncated at the boundary");
}

void sampling_fractional_clip(Case& c) {
  // Full-coverage mask placed at a fractional origin (so the transformed path is used); with an unaligned
  // rectangle clip the only attenuation must come from the pixel/clip overlap, proving the fractional clip is
  // applied inside the mask path rather than only via an integer box. The mask is large enough that the clip
  // boundary pixels lie deep inside it, where mask bilinear coverage is a constant 255.
  BLImage mask = make_a8(20, 20, std::vector<uint8_t>(400, 255));
  BLRectI area(0, 0, 20, 20);

  const int W = 16, H = 16;
  BLRect clip(3.25, 4.5, 7.0, 5.25); // fractional left and top edges
  BLImage img = blank_prgb(W, H);
  BLContext ctx(img);
  ctx.clip_to_rect(clip);
  ctx.set_fill_style(BLRgba32(0xffffffff));
  c.check(ctx.fill_mask(BLPoint(1.5, 1.25), mask, area) == BL_SUCCESS, "fractional clip fill succeeds");
  ctx.end();

  auto overlap = [](double p0, double p1, double c0, double c1) -> double {
    return std::max(0.0, std::min(p1, c1) - std::max(p0, c0));
  };

  int mismatches = 0;
  for (int y = 0; y < H; y++)
    for (int x = 0; x < W; x++) {
      double ox = overlap(double(x), double(x) + 1.0, clip.x, clip.x + clip.w);
      double oy = overlap(double(y), double(y) + 1.0, clip.y, clip.y + clip.h);
      uint32_t cov = udiv255_d(255.0 * ox * oy);
      uint32_t expect = cov ? ((cov << 24) | (cov << 16) | (cov << 8) | cov) : 0u;
      if (channel_diff_max(make_prgb(1, 1, {get_pixel(img, x, y)}), make_prgb(1, 1, {expect})) > 3)
        mismatches++;
    }
  c.check(mismatches == 0, "unaligned rect clip attenuates coverage by pixel overlap");

  // A pixel entirely outside the clip box must be untouched.
  c.check(get_pixel(img, 0, 0) == 0u, "fractional clip leaves outside pixels untouched");
}

// ---------------------------------------------------------------------------
// styles group
// ---------------------------------------------------------------------------

void styles_gradient(Case& c) {
  std::vector<uint8_t> values;
  for (int y = 0; y < 4; y++)
    for (int x = 0; x < 4; x++)
      values.push_back(uint8_t(30 + x * 45));
  BLImage mask = make_a8(4, 4, values);
  BLRectI area(0, 0, 4, 4);

  const int W = 36, H = 30;
  BLMatrix2D transform;
  transform.reset(1, 0, 0, 1, 4.0, 2.0);
  transform.rotate(0.45);

  BLLinearGradientValues gv(2.0, 3.0, 30.0, 22.0);
  BLGradient gradient(gv, BL_EXTEND_MODE_PAD);
  gradient.add_stop(0.0, BLRgba32(0xffff0000));
  gradient.add_stop(1.0, BLRgba32(0xff0000ff));

  BLImage background = make_prgb(W, H, std::vector<uint32_t>(W * H, 0xff101820u));
  BLImage img;
  img.create(W, H, BL_FORMAT_PRGB32);
  BLImageData bd;
  background.get_data(&bd);
  BLImageData id;
  img.make_mutable(&id);
  std::memcpy(id.pixel_data, bd.pixel_data, size_t(H) * id.stride);

  BLContext ctx(img);
  ctx.set_transform(transform);
  ctx.set_comp_op(BL_COMP_OP_SRC_OVER);
  BLPoint origin(3.5, 1.25);
  c.check(ctx.fill_mask(origin, mask, gradient) == BL_SUCCESS, "gradient mask fill");
  ctx.end();

  BLMatrix2D inv = make_inverse(transform, origin, area);
  BLImage plate = render_style_plate(W, H, transform, BLRectI(0, 0, W, H), gradient, BL_COMP_OP_SRC_OVER, 1.0);
  BLImage ref = render_reference(background, plate, mask, area, inv, BLRectI(0, 0, W, H), 255, BL_COMP_OP_SRC_OVER);
  c.check(channel_diff_max(img, ref) <= 3, "asymmetric gradient keeps its own coordinate matrix (tol 3)");
}

void styles_pattern(Case& c) {
  // Asymmetric pattern tile so any origin/mask offset leaking into the brush matrix is visible.
  std::vector<uint32_t> tile_values = {
    0xffff0000, 0xff00ff00, 0xff0000ff,
    0xffffff00, 0xffff00ffu, 0xff00ffffu
  };
  BLImage tile = make_prgb(3, 2, tile_values);
  BLPattern pattern(tile, BL_EXTEND_MODE_REPEAT);

  std::vector<uint8_t> values;
  for (int y = 0; y < 3; y++)
    for (int x = 0; x < 4; x++)
      values.push_back(uint8_t(20 + x * 50 + y * 30));
  BLImage mask = make_a8(4, 3, values);
  BLRectI area(0, 0, 4, 3);

  const int W = 32, H = 28;
  BLMatrix2D transform;
  transform.reset(1.2, 0.1, -0.15, 0.9, 5.0, 4.0);

  BLImage background = make_prgb(W, H, std::vector<uint32_t>(W * H, 0xff222222u));
  BLImage img;
  img.create(W, H, BL_FORMAT_PRGB32);
  BLImageData bd, id;
  background.get_data(&bd);
  img.make_mutable(&id);
  std::memcpy(id.pixel_data, bd.pixel_data, size_t(H) * id.stride);

  BLContext ctx(img);
  ctx.set_transform(transform);
  BLPoint origin(6.75, 3.25);
  c.check(ctx.fill_mask(origin, mask, pattern) == BL_SUCCESS, "pattern mask fill");
  ctx.end();

  BLMatrix2D inv = make_inverse(transform, origin, area);
  BLImage plate = render_style_plate(W, H, transform, BLRectI(0, 0, W, H), pattern, BL_COMP_OP_SRC_OVER, 1.0);
  BLImage ref = render_reference(background, plate, mask, area, inv, BLRectI(0, 0, W, H), 255, BL_COMP_OP_SRC_OVER);
  c.check(channel_diff_max(img, ref) <= 3, "asymmetric pattern keeps its own coordinate matrix (tol 3)");
}

void styles_srccopy_on_background(Case& c) {
  std::vector<uint8_t> values = {0, 64, 128, 200, 255, 100};
  BLImage mask = make_a8(3, 2, values);
  BLRectI area(0, 0, 3, 2);

  const int W = 14, H = 12;
  BLImage background = make_prgb(W, H, std::vector<uint32_t>(W * H, 0xff112233u));

  BLImage img;
  img.create(W, H, BL_FORMAT_PRGB32);
  BLImageData bd, id;
  background.get_data(&bd);
  img.make_mutable(&id);
  std::memcpy(id.pixel_data, bd.pixel_data, size_t(H) * id.stride);

  BLContext ctx(img);
  ctx.set_comp_op(BL_COMP_OP_SRC_COPY);
  ctx.set_global_alpha(0.75);
  BLPoint origin(4.5, 3.25);
  c.check(ctx.fill_mask(origin, mask, BLRgba32(0xffff0000)) == BL_SUCCESS, "SRC_COPY fractional mask on background");
  ctx.end();

  BLMatrix2D inv = make_inverse(BLMatrix2D::make_identity(), origin, area);
  // Plate renders the style at full global alpha; the 0.75 global alpha is applied exactly once through the
  // mask multiplier (style alpha 191) in render_reference.
  BLImage plate = render_style_plate(W, H, BLMatrix2D::make_identity(), BLRectI(0, 0, W, H), BLRgba32(0xffff0000),
                                     BL_COMP_OP_SRC_COPY, 1.0);
  // Style alpha with global alpha 0.75: di.alpha = round(255*0.75) = 191.
  BLImage ref = render_reference(background, plate, mask, area, inv, BLRectI(0, 0, W, H), 191, BL_COMP_OP_SRC_COPY);
  c.check(channel_diff_max(img, ref) <= 3, "partial SRC_COPY interpolates between target and source");

  // Fully outside the mask the background must be preserved exactly.
  c.check(get_pixel(img, 0, 0) == 0xff112233u, "SRC_COPY leaves uncovered background untouched");
}

void styles_transparent_source(Case& c) {
  // Integer aligned 3x1 mask: coverage 0, 128 and 255. With a transparent SRC_COPY source the result must
  // interpolate between the old target and the transparent source, so only masked pixels are affected.
  BLImage mask = make_a8(3, 1, {0, 128, 255});

  const int W = 10, H = 10;
  BLImage background = make_prgb(W, H, std::vector<uint32_t>(W * H, 0xff334455u));
  BLImage img;
  img.create(W, H, BL_FORMAT_PRGB32);
  BLImageData bd, id;
  background.get_data(&bd);
  img.make_mutable(&id);
  std::memcpy(id.pixel_data, bd.pixel_data, size_t(H) * id.stride);

  BLContext ctx(img);
  ctx.set_comp_op(BL_COMP_OP_SRC_COPY);
  c.check(ctx.fill_mask(BLPointI(2, 2), mask, BLRgba32(0x00000000)) == BL_SUCCESS, "transparent source fill returns success");
  ctx.end();

  // Zero coverage: the target must be preserved exactly (no unmasked erase).
  c.check(get_pixel(img, 2, 2) == 0xff334455u, "transparent SRC_COPY keeps zero-coverage target intact");
  // Full coverage: interpolated all the way to the transparent source.
  c.check(get_pixel(img, 4, 2) == 0x00000000u, "transparent SRC_COPY full coverage reaches transparent source");
  // Partial coverage: each background channel is scaled by (255 - 128)/255, never clamped to a source color.
  uint32_t mid = get_pixel(img, 3, 2);
  auto scaled = [](uint32_t ch) -> uint32_t { return uint32_t(std::round(ch * 127.0 / 255.0)); };
  uint32_t expected = (scaled(0xff) << 24) | (scaled(0x33) << 16) | (scaled(0x44) << 8) | scaled(0x55);
  c.check(channel_diff_max(make_prgb(1, 1, {mid}), make_prgb(1, 1, {expected})) <= 3,
          "transparent SRC_COPY partial coverage interpolates toward transparency");
}

// ---------------------------------------------------------------------------
// deferred group
// ---------------------------------------------------------------------------

void deferred_cow_and_release(Case& c) {
  // Tall canvas so a single mask crosses multiple 64px-high render bands.
  const int W = 26, H = 140;
  BLRectI area(0, 0, 5, 4);
  std::vector<uint8_t> values;
  for (int y = 0; y < 4; y++)
    for (int x = 0; x < 5; x++)
      values.push_back(uint8_t(35 + x * 33 + y * 21));
  BLImage mask = make_a8(5, 4, values);
  BLImage snapshot(mask); // deep, independent copy of the call-time content

  BLImage background = make_prgb(W, H, std::vector<uint32_t>(W * H, 0xff101018u));

  // Queue every command (solid + a temporary pattern) before touching any source object.
  BLImage async_img;
  async_img.create(W, H, BL_FORMAT_PRGB32);
  BLImageData bd, id;
  background.get_data(&bd);
  async_img.make_mutable(&id);
  std::memcpy(id.pixel_data, bd.pixel_data, size_t(H) * id.stride);

  BLContextCreateInfo cci{};
  cci.thread_count = 2;
  BLContext ctx(async_img, cci);

  ctx.set_fill_style(BLRgba32(0xffd04030));
  for (int k = 0; k < 6; k++) {
    ctx.reset_transform();
    ctx.translate(2.0 + k * 1.5, 3.0 + k * 23.5); // spans bands at y ~64 and ~128
    ctx.rotate(0.15 * k);
    c.check(ctx.fill_mask(BLPoint(0.5 + 0.25 * k, 0.75), mask, area) == BL_SUCCESS, "queued solid mask fill");
  }

  BLPattern* transient_pattern = new BLPattern(
    make_prgb(2, 2, {0xffff0000, 0xff00ff00, 0xff0000ff, 0xffffffff}), BL_EXTEND_MODE_REPEAT);
  ctx.set_fill_style(*transient_pattern);
  ctx.reset_transform();
  c.check(ctx.fill_mask(BLPoint(7.25, 70.5), mask, area) == BL_SUCCESS, "queued pattern mask fill");

  // Everything is queued: now trigger copy-on-write, mutate the caller's shared image, and release the source
  // and the temporary style. Flushed pixels must still reflect the call-time content and queued states.
  BLImageData md;
  c.check(mask.make_mutable(&md) == BL_SUCCESS, "make_mutable triggers COW after queueing");
  std::memset(md.pixel_data, 0, size_t(mask.size().h) * md.stride);
  delete transient_pattern;
  mask.reset();

  ctx.end(); // flush here

  // Synchronous reference replays the identical sequence against the snapshot content.
  BLImage sync_img;
  sync_img.create(W, H, BL_FORMAT_PRGB32);
  BLImageData sid;
  sync_img.make_mutable(&sid);
  std::memcpy(sid.pixel_data, bd.pixel_data, size_t(H) * sid.stride);
  {
    BLContext s(sync_img);
    s.set_fill_style(BLRgba32(0xffd04030));
    for (int k = 0; k < 6; k++) {
      s.reset_transform();
      s.translate(2.0 + k * 1.5, 3.0 + k * 23.5);
      s.rotate(0.15 * k);
      s.fill_mask(BLPoint(0.5 + 0.25 * k, 0.75), snapshot, area);
    }
    BLPattern pat(make_prgb(2, 2, {0xffff0000, 0xff00ff00, 0xff0000ff, 0xffffffff}), BL_EXTEND_MODE_REPEAT);
    s.set_fill_style(pat);
    s.reset_transform();
    s.fill_mask(BLPoint(7.25, 70.5), snapshot, area);
  }

  c.check(images_equal(async_img, sync_img), "deferred COW/release pixels match synchronous reference");
}

void deferred_style_and_transform_swap(Case& c) {
  const int W = 40, H = 40;
  BLRectI area(0, 0, 3, 3);
  BLImage mask = make_a8(3, 3, {90, 120, 150, 180, 210, 240, 60, 100, 200});
  BLImage background = make_prgb(W, H, std::vector<uint32_t>(W * H, 0u));

  auto sequence = [&](BLContext& ctx, const BLImage& m) {
    ctx.set_fill_style(BLRgba32(0xffff0000));
    ctx.reset_transform();
    ctx.translate(4.0, 4.0);
    ctx.fill_mask(BLPoint(1.25, 1.5), m, area);

    // Replace the brush and change the transform after the first call was queued; later commands must capture
    // the new state independently.
    ctx.set_fill_style(BLRgba32(0xff0000ff));
    ctx.reset_transform();
    ctx.translate(20.0, 18.0);
    ctx.rotate(-0.6);
    ctx.scale(1.4, 0.7);
    ctx.fill_mask(BLPoint(-2.0, 0.25), m, area);
  };

  BLImage sync_img, async_img;
  sync_img.create(W, H, BL_FORMAT_PRGB32);
  async_img.create(W, H, BL_FORMAT_PRGB32);
  BLImageData bd, sid, aid;
  background.get_data(&bd);
  sync_img.make_mutable(&sid);
  async_img.make_mutable(&aid);
  std::memcpy(sid.pixel_data, bd.pixel_data, size_t(H) * sid.stride);
  std::memcpy(aid.pixel_data, bd.pixel_data, size_t(H) * aid.stride);

  { BLContext s(sync_img); sequence(s, mask); }
  {
    BLContextCreateInfo cci{};
    cci.thread_count = 2;
    BLContext a(async_img, cci);
    sequence(a, mask);
  }
  c.check(images_equal(sync_img, async_img), "queued style/transform swaps render identically in sync and async");
}

// ---------------------------------------------------------------------------
// compatibility group
// ---------------------------------------------------------------------------

void compatibility_legacy_aligned(Case& c) {
  BLImage mask(3, 2, BL_FORMAT_A8);
  BLImageData md;
  mask.make_mutable(&md);
  for (int y = 0; y < 2; y++)
    for (int x = 0; x < 3; x++)
      static_cast<uint8_t*>(md.pixel_data)[intptr_t(y) * md.stride + x] = uint8_t(50 + x * 50);

  auto render = [&](uint32_t threads) {
    BLImage image(16, 16, BL_FORMAT_PRGB32);
    BLContextCreateInfo cci{};
    cci.thread_count = threads;
    BLContext ctx(image, cci);
    ctx.set_comp_op(BL_COMP_OP_SRC_COPY);
    ctx.fill_all(BLRgba32(0));
    ctx.set_fill_style(BLRgba32(0xffff0000));
    ctx.fill_mask(BLPointI(2, 2), mask);
    ctx.end();
    return image;
  };

  BLImage sync_img = render(0);
  BLImage async_img = render(2);

  auto row = [](const BLImage& img, int y) {
    BLImageData d;
    img.get_data(&d);
    return reinterpret_cast<const uint32_t*>(static_cast<const uint8_t*>(d.pixel_data) + intptr_t(y) * d.stride);
  };

  const uint32_t* sr = row(sync_img, 2);
  c.check(sr[2] == 0x32320000u && sr[4] == 0x96960000u, "legacy aligned integer mask bytes preserved");
  c.check(images_equal(sync_img, async_img), "legacy aligned mask identical between sync and async");
}

void compatibility_ga_srccopy_value(Case& c) {
  // A8=200, SRC_COPY red, global alpha 0.5 on transparent target -> 0x64640000.
  BLImage mask(1, 1, BL_FORMAT_A8);
  BLImageData md;
  mask.make_mutable(&md);
  static_cast<uint8_t*>(md.pixel_data)[0] = 200;

  auto render = [&](uint32_t threads) {
    BLImage img = blank_prgb(4, 4);
    BLContextCreateInfo cci{};
    cci.thread_count = threads;
    BLContext ctx(img, cci);
    ctx.set_comp_op(BL_COMP_OP_SRC_COPY);
    ctx.set_global_alpha(0.5);
    ctx.fill_mask(BLPointI(1, 1), mask, BLRgba32(0xffff0000));
    ctx.end();
    return img;
  };

  BLImage sync_img = render(0);
  BLImage async_img = render(2);
  c.check(get_pixel(sync_img, 1, 1) == 0x64640000u, "sync GA-corrected A8=200 SRC_COPY red is 0x64640000");
  c.check(get_pixel(async_img, 1, 1) == 0x64640000u, "async GA-corrected result matches sync exactly");
}

void compatibility_invalid_and_empty_area(Case& c) {
  BLImage mask(4, 4, BL_FORMAT_A8);
  BLImage img = blank_prgb(8, 8);
  BLContext ctx(img);

  c.check(ctx.fill_mask(BLPoint(1, 1), mask, BLRectI(2, 2, 5, 1)) == BL_ERROR_INVALID_VALUE, "out-of-range area rejected");
  c.check(ctx.fill_mask(BLPoint(1, 1), mask, BLRectI(-1, 0, 2, 2)) == BL_ERROR_INVALID_VALUE, "negative area origin rejected");

  BLResult empty = ctx.fill_mask(BLPoint(1, 1), mask, BLRectI(1, 1, 0, 2));
  c.check(empty == BL_SUCCESS, "empty selection succeeds and writes nothing");
  ctx.end();

  BLImageData d;
  img.get_data(&d);
  int touched = 0;
  for (int y = 0; y < 8; y++)
    for (int x = 0; x < 8; x++)
      touched += get_pixel(img, x, y) != 0u;
  c.check(touched == 0, "empty/fully-clipped mask writes no pixels");
}

void compatibility_clip_save_restore(Case& c) {
  BLImage mask(4, 4, BL_FORMAT_A8);
  BLImageData md;
  mask.make_mutable(&md);
  std::memset(md.pixel_data, 255, size_t(4) * md.stride);

  BLImage img = blank_prgb(20, 20);
  BLContext ctx(img);
  ctx.set_fill_style(BLRgba32(0xffff0000));

  ctx.save();
  ctx.clip_to_rect(BLRectI(5, 5, 6, 6));
  c.check(ctx.fill_mask(BLPointI(4, 4), mask) == BL_SUCCESS, "clipped aligned mask succeeds");
  ctx.restore();

  // After restore the clip is gone; a fractional mask outside the previous clip must write.
  c.check(ctx.fill_mask(BLPoint(14.5, 14.5), mask, BLRectI(0, 0, 2, 2)) == BL_SUCCESS, "post-restore fractional mask succeeds");
  ctx.end();

  c.check(get_pixel(img, 4, 4) == 0u, "pixel outside restored clip was not written");
  c.check(get_pixel(img, 5, 5) == 0xffff0000u, "pixel inside clip got full red");
  c.check(get_pixel(img, 15, 15) != 0u, "after restore the clip no longer applies");
}

void compatibility_row_sentinels(Case& c) {
  // External PRGB32 storage with extra padding per row; padding bytes are row-end sentinels and must never move.
  const int W = 8, H = 6, pad = 12;
  const intptr_t stride = W * 4 + pad;
  std::vector<uint8_t> storage(size_t(H) * stride);
  for (int y = 0; y < H; y++) {
    std::memset(storage.data() + intptr_t(y) * stride, 0, size_t(W) * 4);
    std::memset(storage.data() + intptr_t(y) * stride + W * 4, 0xCD, pad);
  }

  BLImage img;
  c.check(img.create_from_data(W, H, BL_FORMAT_PRGB32, storage.data(), stride, BL_DATA_ACCESS_RW) == BL_SUCCESS,
          "external stride image created");

  BLImage mask(4, 2, BL_FORMAT_A8);
  BLImageData md;
  mask.make_mutable(&md);
  std::memset(md.pixel_data, 255, size_t(2) * md.stride);

  {
    BLContext ctx(img);
    ctx.set_fill_style(BLRgba32(0xff00ff00));
    c.check(ctx.fill_mask(BLPointI(2, 1), mask) == BL_SUCCESS, "aligned mask on padded stride image");
    ctx.end();
  }

  int corrupted = 0;
  for (int y = 0; y < H; y++)
    for (int k = 0; k < pad; k++)
      corrupted += storage[intptr_t(y) * stride + W * 4 + k] != 0xCD;
  c.check(corrupted == 0, "row-end sentinel bytes beyond the stride are untouched");
  c.check(get_pixel(img, 2, 1) == 0xff00ff00u, "padded-stride image still renders correct pixels");
}

} // namespace

void register_task_tests(Registry& r) {
  r.add("sampling", "subarea", sampling_subarea);
  r.add("sampling", "fractional_translation", sampling_fractional_translation);
  r.add("sampling", "rotation", sampling_rotation);
  r.add("sampling", "reflection_shear_scale", sampling_reflection_shear_scale);
  r.add("sampling", "filter_boundary", sampling_filter_boundary);
  r.add("sampling", "upscale_bounds", sampling_upscale_bounds);
  r.add("sampling", "fractional_clip", sampling_fractional_clip);

  r.add("styles", "gradient", styles_gradient);
  r.add("styles", "pattern", styles_pattern);
  r.add("styles", "srccopy_on_background", styles_srccopy_on_background);
  r.add("styles", "transparent_source", styles_transparent_source);

  r.add("deferred", "cow_and_release", deferred_cow_and_release);
  r.add("deferred", "style_and_transform_swap", deferred_style_and_transform_swap);

  r.add("compatibility", "legacy_aligned", compatibility_legacy_aligned);
  r.add("compatibility", "ga_srccopy_value", compatibility_ga_srccopy_value);
  r.add("compatibility", "invalid_and_empty_area", compatibility_invalid_and_empty_area);
  r.add("compatibility", "clip_save_restore", compatibility_clip_save_restore);
  r.add("compatibility", "row_sentinels", compatibility_row_sentinels);
}
