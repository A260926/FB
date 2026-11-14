// Task tests for the transformed A8 mask path of BLContext::fill_mask().
//
// Registered through register_task_tests() in four groups:
//   - sampling:      independent inverse-matrix / bilinear reference vs the library
//   - styles:        current style, explicit RGBA32, gradient and pattern overloads and their coordinate spaces
//   - deferred:      asynchronous queuing with copy-on-write and released sources
//   - compatibility: legacy aligned bytes, invalid areas, clip/save/restore and row-end sentinels
//
// The file deliberately has no main(); task_support/main.cpp drives the registry.

#include "registry.hpp"

#include <blend2d/blend2d.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace {

// ----------------------------------------------------------------------------
// Shared helpers
// ----------------------------------------------------------------------------

constexpr uint32_t kColor = 0xffff0000u;   // Opaque red.
constexpr uint32_t kBg = 0xff102030u;      // Opaque bluish background.

struct Matrix {
  double m[6];
};

std::vector<uint32_t> snapshot(const BLImage& img) {
  BLImageData d{};
  img.get_data(&d);
  std::vector<uint32_t> px(size_t(img.width()) * size_t(img.height()));
  for (int y = 0; y < img.height(); ++y) {
    const uint32_t* row = reinterpret_cast<const uint32_t*>(
      static_cast<const uint8_t*>(d.pixel_data) + intptr_t(y) * d.stride);
    for (int x = 0; x < img.width(); ++x)
      px[size_t(y) * img.width() + x] = row[x];
  }
  return px;
}

uint32_t pixel_at(const BLImage& img, int x, int y) {
  BLImageData d{};
  img.get_data(&d);
  return reinterpret_cast<const uint32_t*>(
    static_cast<const uint8_t*>(d.pixel_data) + intptr_t(y) * d.stride)[x];
}

int clamp_u8(int v) { return v < 0 ? 0 : (v > 255 ? 255 : v); }
int round_u8(double v) { return clamp_u8(int(std::lround(v))); }
int div255(int v) { return round_u8(double(v) / 255.0); }

BLMatrix2D to_bl(const Matrix& m) {
  return BLMatrix2D(m.m[0], m.m[1], m.m[2], m.m[3], m.m[4], m.m[5]);
}

// Independent inverse of an affine matrix [m00 m01; m10 m11; m20 m21].
Matrix invert(const Matrix& m) {
  double det = m.m[0] * m.m[3] - m.m[1] * m.m[2];
  double id = 1.0 / det;
  Matrix r{};
  r.m[0] = m.m[3] * id;
  r.m[1] = -m.m[1] * id;
  r.m[2] = -m.m[2] * id;
  r.m[3] = m.m[0] * id;
  r.m[4] = -(r.m[0] * m.m[4] + r.m[2] * m.m[5]);
  r.m[5] = -(r.m[1] * m.m[4] + r.m[3] * m.m[5]);
  return r;
}

// Independent bilinear coverage for a solid-style fill, with zero contribution outside the selected area.
// Source pixel centers sit at selection-local half-integer coordinates; the footprint is never clamped to an
// edge and the last pixel is never repeated.
struct RefCase {
  int W, H;
  Matrix transform;
  BLPoint origin;
  BLRectI area;
  BLCompOp comp_op;
  int global_alpha;   // [0,255]
};

std::vector<uint32_t> reference_solid(const RefCase& c, const std::vector<uint8_t>& mask, int mw, int /*mh*/,
                                      uint32_t argb32, uint32_t bg) {
  Matrix inv = invert(c.transform);

  int sa = int((argb32 >> 24) & 255);
  int sr = int((argb32 >> 16) & 255);
  int sg = int((argb32 >> 8) & 255);
  int sb = int(argb32 & 255);
  // Premultiplied source.
  int src[4] = { div255(sr * sa), div255(sg * sa), div255(sb * sa), sa };

  std::vector<uint32_t> out(size_t(c.W) * c.H, bg);

  for (int y = 0; y < c.H; ++y) {
    for (int x = 0; x < c.W; ++x) {
      double dx = x + 0.5;
      double dy = y + 0.5;
      double u = inv.m[0] * dx + inv.m[2] * dy + inv.m[4] - c.origin.x;
      double v = inv.m[1] * dx + inv.m[3] * dy + inv.m[5] - c.origin.y;

      double fx = u - 0.5;
      double fy = v - 0.5;
      int ix = int(std::floor(fx));
      int iy = int(std::floor(fy));
      double tx = fx - ix;
      double ty = fy - iy;

      auto tap = [&](int lx, int ly) -> int {
        if (lx < 0 || ly < 0 || lx >= c.area.w || ly >= c.area.h)
          return 0;
        return mask[size_t(c.area.y + ly) * mw + (c.area.x + lx)];
      };

      double cov =
        tap(ix, iy) * (1.0 - tx) * (1.0 - ty) +
        tap(ix + 1, iy) * tx * (1.0 - ty) +
        tap(ix, iy + 1) * (1.0 - tx) * ty +
        tap(ix + 1, iy + 1) * tx * ty;

      int m = div255(round_u8(cov) * c.global_alpha);

      uint32_t d = out[size_t(y) * c.W + x];
      int dc[4] = { int((d >> 16) & 255), int((d >> 8) & 255), int(d & 255), int((d >> 24) & 255) };
      int o[4];

      if (c.comp_op == BL_COMP_OP_SRC_COPY) {
        for (int k = 0; k < 4; ++k)
          o[k] = div255(dc[k] * (255 - m) + src[k] * m);
      }
      else {
        int sm[4] = { div255(src[0] * m), div255(src[1] * m), div255(src[2] * m), div255(src[3] * m) };
        for (int k = 0; k < 4; ++k)
          o[k] = round_u8(sm[k] + double(div255(dc[k] * (255 - sm[3]))));
      }

      out[size_t(y) * c.W + x] =
        (uint32_t(clamp_u8(o[3])) << 24) | (uint32_t(clamp_u8(o[0])) << 16) |
        (uint32_t(clamp_u8(o[1])) << 8) | uint32_t(clamp_u8(o[2]));
    }
  }
  return out;
}

std::vector<uint8_t> make_mask(int mw, int mh) {
  std::vector<uint8_t> m(size_t(mw) * mh);
  for (int y = 0; y < mh; ++y)
    for (int x = 0; x < mw; ++x)
      m[size_t(y) * mw + x] = uint8_t((x * 37 + y * 53 + 11) & 255);
  return m;
}

BLImage to_a8_image(const std::vector<uint8_t>& mask, int mw, int mh) {
  BLImage img(mw, mh, BL_FORMAT_A8);
  BLImageData d{};
  img.make_mutable(&d);
  for (int y = 0; y < mh; ++y)
    std::memcpy(static_cast<uint8_t*>(d.pixel_data) + intptr_t(y) * d.stride,
                &mask[size_t(y) * mw], size_t(mw));
  return img;
}

// Renders one reference case through Blend2D. When `thread_count` is zero the context is synchronous.
BLImage render_solid(const RefCase& c, const std::vector<uint8_t>& mask, int mw, int mh,
                     uint32_t argb32, uint32_t bg, uint32_t thread_count) {
  BLImage img(c.W, c.H, BL_FORMAT_PRGB32);
  BLContextCreateInfo info{};
  info.thread_count = thread_count;
  BLContext ctx(img, info);

  ctx.set_transform(to_bl(c.transform));
  ctx.set_comp_op(BL_COMP_OP_SRC_COPY);
  ctx.fill_all(BLRgba32(bg));

  ctx.set_comp_op(c.comp_op);
  ctx.set_global_alpha(c.global_alpha / 255.0);
  ctx.set_fill_style(BLRgba32(argb32));

  BLImage mimg = to_a8_image(mask, mw, mh);
  BLResult r = ctx.fill_mask(c.origin, mimg, c.area);
  if (r != BL_SUCCESS)
    throw std::runtime_error("fill_mask failed");

  ctx.flush(BL_CONTEXT_FLUSH_SYNC);
  ctx.end();
  return img;
}

// Largest per-channel absolute difference between two images.
int worst_channel_diff(const std::vector<uint32_t>& a, const std::vector<uint32_t>& b) {
  int worst = 0;
  for (size_t i = 0; i < a.size(); ++i) {
    for (int sh = 0; sh < 32; sh += 8)
      worst = std::max(worst, std::abs(int((a[i] >> sh) & 255) - int((b[i] >> sh) & 255)));
  }
  return worst;
}

// ----------------------------------------------------------------------------
// sampling group
// ----------------------------------------------------------------------------

void sampling_cases(Case& c) {
  const int W = 24, H = 24, mw = 7, mh = 7;
  std::vector<uint8_t> mask = make_mask(mw, mh);

  // A border of saturated pixels surrounds a smaller selected area. The reference never reads those border
  // pixels, so matching it proves the selection (not the whole mask) is the sampling domain.
  for (int y = 0; y < mh; ++y) {
    for (int x = 0; x < mw; ++x) {
      if (x == 0 || y == 0 || x == mw - 1 || y == mh - 1)
        mask[size_t(y) * mw + x] = 255;
    }
  }

  struct Def { Matrix m; BLPoint origin; BLRectI area; BLCompOp op; int ga; const char* name; };
  Matrix identity{ {1, 0, 0, 1, 0, 0} };
  Matrix scale{ {1.5, 0, 0, 2.0, 0, 0} };
  Matrix reflect_x{ {-1.0, 0, 0, 1.0, 0, 0} };
  Matrix reflect_y{ {1.0, 0, 0, -1.0, 0, 0} };
  Matrix shear{ {1.2, 0.4, -0.5, 0.9, 5.0, 4.0} };
  Matrix rot = [] { Matrix r{ {1, 0, 0, 1, 0, 0} }; BLMatrix2D b = BLMatrix2D::make_rotation(0.6);
    std::memcpy(r.m, &b, sizeof(r.m)); r.m[4] = 11.0; r.m[5] = 11.0; return r; }();

  std::vector<Def> defs = {
    { identity,  BLPoint(1.5, 2.25), BLRectI(1, 1, 4, 4), BL_COMP_OP_SRC_OVER, 255, "fractional translation" },
    { identity,  BLPoint(2.25, 1.75), BLRectI(0, 0, 5, 4), BL_COMP_OP_SRC_COPY, 255, "fractional src_copy" },
    { identity,  BLPoint(0.5, 0.5), BLRectI(0, 0, 5, 5), BL_COMP_OP_SRC_OVER, 128, "fractional partial alpha" },
    { scale,     BLPoint(1.0, 0.5), BLRectI(1, 1, 4, 4), BL_COMP_OP_SRC_OVER, 255, "non-uniform scale" },
    { reflect_x, BLPoint(20.0, 2.0), BLRectI(0, 0, 5, 4), BL_COMP_OP_SRC_COPY, 200, "reflection x" },
    { reflect_y, BLPoint(2.0, 20.0), BLRectI(0, 0, 4, 5), BL_COMP_OP_SRC_OVER, 255, "reflection y" },
    { rot,       BLPoint(-4.0, -4.0), BLRectI(0, 0, 7, 7), BL_COMP_OP_SRC_OVER, 255, "rotation" },
    { shear,     BLPoint(-2.5, -2.0), BLRectI(1, 0, 4, 6), BL_COMP_OP_SRC_COPY, 170, "shear + partial alpha" },
  };

  for (const Def& d : defs) {
    RefCase rc{ W, H, d.m, d.origin, d.area, d.op, d.ga };
    std::vector<uint32_t> expect = reference_solid(rc, mask, mw, mh, kColor, kBg);

    BLImage sync = render_solid(rc, mask, mw, mh, kColor, kBg, 0);
    BLImage async = render_solid(rc, mask, mw, mh, kColor, kBg, 2);

    std::vector<uint32_t> got_sync = snapshot(sync);
    std::vector<uint32_t> got_async = snapshot(async);

    c.check(worst_channel_diff(expect, got_sync) <= 3, d.name);
    c.check(worst_channel_diff(expect, got_async) <= 3, d.name);
    // Synchronous and asynchronous results must be byte identical.
    c.check(got_sync == got_async, d.name);
  }

  // Explicit filter-boundary probe: a translated selection whose bilinear footprint straddles the left edge.
  // A fully covered interior pixel must read 255, while the column just outside must stay at the background.
  {
    BLImage img(10, 10, BL_FORMAT_PRGB32);
    BLContext ctx(img);
    ctx.set_comp_op(BL_COMP_OP_SRC_COPY);
    ctx.fill_all(BLRgba32(0));
    BLImage m(2, 1, BL_FORMAT_A8);
    BLImageData md{};
    m.make_mutable(&md);
    static_cast<uint8_t*>(md.pixel_data)[0] = 255;
    static_cast<uint8_t*>(md.pixel_data)[1] = 255;
    ctx.set_fill_style(BLRgba32(0xffff0000));
    ctx.fill_mask(BLPoint(2.5, 4.0), m);
    ctx.flush(BL_CONTEXT_FLUSH_SYNC);
    ctx.end();

    // Columns 3 and 4 are fully inside the footprint; column 2 only gets the half-weight left tap.
    c.check(pixel_at(img, 2, 4) == 0x80800000u, "footprint boundary half weight");
    c.check(pixel_at(img, 3, 4) == 0xffff0000u, "footprint interior full weight");
    c.check(pixel_at(img, 1, 4) == 0u, "outside footprint untouched");
  }
}

// ----------------------------------------------------------------------------
// styles group
// ----------------------------------------------------------------------------

// A plain fill of the whole target with a style; used as the device-coordinate color reference.
template<typename StyleT>
BLImage plain_style_fill(int W, int H, const StyleT& style) {
  BLImage img(W, H, BL_FORMAT_PRGB32);
  BLContext ctx(img);
  ctx.set_comp_op(BL_COMP_OP_SRC_COPY);
  ctx.set_fill_style(style);
  ctx.fill_all();
  ctx.flush(BL_CONTEXT_FLUSH_SYNC);
  ctx.end();
  return img;
}

void styles_cases(Case& c) {
  const int W = 16, H = 16;

  // Asymmetric horizontal gradient: color is a pure function of the device x coordinate.
  BLGradient gradient(BLLinearGradientValues(0, 0, W, 0));
  gradient.add_stop(0.0, BLRgba32(0xffff0000));
  gradient.add_stop(1.0, BLRgba32(0xff0000ff));

  // A 2x2 asymmetric tile used through a repeating pattern.
  BLImage tile(2, 2, BL_FORMAT_PRGB32);
  {
    BLImageData td{};
    tile.make_mutable(&td);
    uint32_t colors[4] = { 0xffff0000u, 0xff00ff00u, 0xff0000ffu, 0xffffffffu };
    for (int i = 0; i < 4; ++i)
      reinterpret_cast<uint32_t*>(td.pixel_data)[i] = colors[i];
  }
  BLPattern pattern(tile, BL_EXTEND_MODE_REPEAT);

  BLImage solid_mask(6, 6, BL_FORMAT_A8);
  {
    BLImageData md{};
    solid_mask.make_mutable(&md);
    for (int y = 0; y < 6; ++y)
      for (int x = 0; x < 6; ++x)
        static_cast<uint8_t*>(md.pixel_data)[intptr_t(y) * md.stride + x] = 255;
  }

  // --- Gradient keeps its own coordinate matrix under a fractional mask origin. ---
  {
    BLImage plain = plain_style_fill(W, H, gradient);

    BLImage img(W, H, BL_FORMAT_PRGB32);
    BLContext ctx(img);
    ctx.set_comp_op(BL_COMP_OP_SRC_COPY);
    ctx.fill_all(BLRgba32(0));
    ctx.set_comp_op(BL_COMP_OP_SRC_OVER);
    ctx.set_fill_style(gradient);
    // Explicit BLGradient overload through fill_mask(origin, mask, style).
    BLResult r = ctx.fill_mask(BLPoint(3.25, 4.75), solid_mask, gradient);
    c.check(r == BL_SUCCESS, "gradient overload accepts mask");
    ctx.flush(BL_CONTEXT_FLUSH_SYNC);
    ctx.end();

    // A fully covered interior pixel must carry the plain gradient color at the SAME device coordinate.
    c.check(pixel_at(img, 6, 7) == pixel_at(plain, 6, 7), "gradient color follows device coords");
    c.check(pixel_at(img, 0, 0) == 0u, "gradient leaves outside pixels untouched");
  }

  // --- Pattern keeps its own coordinate matrix under a fractional mask origin. ---
  {
    BLImage plain = plain_style_fill(W, H, pattern);

    BLImage img(W, H, BL_FORMAT_PRGB32);
    BLContext ctx(img);
    ctx.set_comp_op(BL_COMP_OP_SRC_COPY);
    ctx.fill_all(BLRgba32(0));
    ctx.set_comp_op(BL_COMP_OP_SRC_OVER);
    ctx.set_fill_style(pattern);
    BLResult r = ctx.fill_mask(BLPoint(2.25, 3.75), solid_mask, pattern);
    c.check(r == BL_SUCCESS, "pattern overload accepts mask");
    ctx.flush(BL_CONTEXT_FLUSH_SYNC);
    ctx.end();

    c.check(pixel_at(img, 4, 5) == pixel_at(plain, 4, 5), "pattern color follows device coords");
    c.check(pixel_at(img, 0, 0) == 0u, "pattern leaves outside pixels untouched");
  }

  // --- Explicit RGBA32 overload with SRC_COPY over an opaque background. ---
  {
    BLImage img(W, H, BL_FORMAT_PRGB32);
    BLContext ctx(img);
    ctx.set_comp_op(BL_COMP_OP_SRC_COPY);
    ctx.fill_all(BLRgba32(kBg));
    // SRC_COPY must interpolate between old target and source according to coverage.
    BLImage m(1, 1, BL_FORMAT_A8);
    BLImageData md{};
    m.make_mutable(&md);
    static_cast<uint8_t*>(md.pixel_data)[0] = 128;
    BLResult r = ctx.fill_mask(BLPoint(5, 5), m, BLRgba32(0xffff0000));
    c.check(r == BL_SUCCESS, "rgba32 overload accepts mask");
    ctx.flush(BL_CONTEXT_FLUSH_SYNC);
    ctx.end();

    uint32_t p = pixel_at(img, 5, 5);
    int rr = (p >> 16) & 255, gg = (p >> 8) & 255, bb = p & 255, aa = (p >> 24) & 255;
    // m = div255(128*255) = 128, so 127/255 old target blends with 128/255 red.
    // R=div255(16*127+255*128)=136, G=div255(32*127)=16, B=div255(48*127)=24, A=255.
    c.check(std::abs(aa - 255) <= 1 && std::abs(rr - 136) <= 2 && std::abs(gg - 16) <= 2 && std::abs(bb - 24) <= 2,
            "src_copy interpolates over background");
  }

  // --- A fully transparent source under SRC_OVER must leave the target unchanged (source contributes nothing). ---
  {
    BLImage img(W, H, BL_FORMAT_PRGB32);
    BLContext ctx(img);
    ctx.set_comp_op(BL_COMP_OP_SRC_COPY);
    ctx.fill_all(BLRgba32(kBg));
    ctx.set_comp_op(BL_COMP_OP_SRC_OVER);
    BLImage m(3, 3, BL_FORMAT_A8);
    BLImageData md{};
    m.make_mutable(&md);
    for (int i = 0; i < 9; ++i)
      static_cast<uint8_t*>(md.pixel_data)[i] = 255;
    ctx.fill_mask(BLPoint(2.5, 2.5), m, BLRgba32(0x00ff0000));
    ctx.flush(BL_CONTEXT_FLUSH_SYNC);
    ctx.end();
    c.check(pixel_at(img, 4, 4) == kBg, "transparent source leaves target intact");
  }

  // --- The implicit current-style overload routes through the same transformed path. ---
  {
    BLImage img(W, H, BL_FORMAT_PRGB32);
    BLContext ctx(img);
    ctx.set_comp_op(BL_COMP_OP_SRC_COPY);
    ctx.fill_all(BLRgba32(0));
    ctx.set_comp_op(BL_COMP_OP_SRC_OVER);
    ctx.set_fill_style(BLRgba32(0xff00ff00));
    BLResult r = ctx.fill_mask(BLPoint(1.5, 1.5), solid_mask);
    c.check(r == BL_SUCCESS, "implicit style overload accepts mask");
    ctx.flush(BL_CONTEXT_FLUSH_SYNC);
    ctx.end();
    c.check(pixel_at(img, 3, 3) == 0xff00ff00u, "implicit style renders through mask");
  }
}

// ----------------------------------------------------------------------------
// deferred group
// ----------------------------------------------------------------------------

void deferred_cases(Case& c) {
  // Tall target so the queued masks span several rendering bands even with two worker threads.
  const int W = 28, H = 56, mw = 6, mh = 6;
  std::vector<uint8_t> mask_bytes = make_mask(mw, mh);

  auto queue_draws = [&](BLContext& ctx, BLImage& mask_img) {
    ctx.set_comp_op(BL_COMP_OP_SRC_COPY);
    ctx.fill_all(BLRgba32(0));
    ctx.set_comp_op(BL_COMP_OP_SRC_OVER);
    ctx.set_fill_style(BLRgba32(0xff0088ff));
    // Five overlapping, fractional-origin draws that together cross multiple bands.
    for (int k = 0; k < 5; ++k)
      ctx.fill_mask(BLPoint(2.0 + k * 3.5, 3.0 + k * 9.5), mask_img);
  };

  // Synchronous reference pixels.
  BLImage ref(W, H, BL_FORMAT_PRGB32);
  {
    BLImage mask_img = to_a8_image(mask_bytes, mw, mh);
    BLContext ctx(ref);
    queue_draws(ctx, mask_img);
    ctx.flush(BL_CONTEXT_FLUSH_SYNC);
    ctx.end();
  }
  std::vector<uint32_t> expect = snapshot(ref);

  // Asynchronous: share the mask impl, queue the work, then force copy-on-write and release everything.
  BLImage out(W, H, BL_FORMAT_PRGB32);
  {
    BLImage mask_img = to_a8_image(mask_bytes, mw, mh);
    BLImage shared = mask_img;  // Weak copy; both now reference the same impl.

    BLContextCreateInfo info{};
    info.thread_count = 2;
    BLContext ctx(out, info);
    queue_draws(ctx, shared);

    // Mutating the caller's image must detach it (copy-on-write); queued commands keep their own reference.
    BLImageData cow{};
    c.check(shared.make_mutable(&cow) == BL_SUCCESS, "make_mutable triggers copy-on-write");
    static_cast<uint8_t*>(cow.pixel_data)[0] = 7;

    // Release every caller-held reference and replace brush / transform before flushing.
    shared.reset();
    mask_img.reset();

    BLGradient other(BLLinearGradientValues(0, 0, double(W), 0));
    other.add_stop(0.0, BLRgba32(0xff112233));
    other.add_stop(1.0, BLRgba32(0xff445566));
    ctx.set_fill_style(other);
    ctx.translate(120.0, 120.0);

    ctx.flush(BL_CONTEXT_FLUSH_SYNC);
    ctx.end();
  }

  // Compare actual pixels, not just the call return value.
  c.check(snapshot(out) == expect, "deferred pixels match synchronous reference");

  // A second async run on a fresh target with a temporary style that is destroyed before flush.
  BLImage out2(W, H, BL_FORMAT_PRGB32);
  {
    BLImage mask_img = to_a8_image(mask_bytes, mw, mh);
    BLContextCreateInfo info{};
    info.thread_count = 2;
    BLContext ctx(out2, info);
    ctx.set_comp_op(BL_COMP_OP_SRC_COPY);
    ctx.fill_all(BLRgba32(0));
    ctx.set_comp_op(BL_COMP_OP_SRC_OVER);
    {
      BLGradient tmp(BLLinearGradientValues(0, 0, 10, 10));
      tmp.add_stop(0.0, BLRgba32(0xff0088ff));
      tmp.add_stop(1.0, BLRgba32(0xff0088ff));
      ctx.set_fill_style(tmp);  // `tmp` dies at block end; the context must retain what it needs.
    }
    for (int k = 0; k < 5; ++k)
      ctx.fill_mask(BLPoint(2.0 + k * 3.5, 3.0 + k * 9.5), mask_img);
    ctx.flush(BL_CONTEXT_FLUSH_SYNC);
    ctx.end();
  }
  // Solid blue gradient, so the queued region must still contain blue premultiplied pixels.
  bool saw_blue = false;
  for (uint32_t px : snapshot(out2))
    saw_blue |= ((px >> 24) & 255) != 0;
  c.check(saw_blue, "temporary style retained until flush");
}

// ----------------------------------------------------------------------------
// compatibility group
// ----------------------------------------------------------------------------

void compatibility_cases(Case& c) {
  // Legacy integer, pixel aligned mask keeps the exact synchronous byte result.
  {
    BLImage img(16, 16, BL_FORMAT_PRGB32);
    BLImage mask(3, 2, BL_FORMAT_A8);
    BLImageData md{};
    mask.make_mutable(&md);
    for (int y = 0; y < 2; ++y)
      for (int x = 0; x < 3; ++x)
        static_cast<uint8_t*>(md.pixel_data)[intptr_t(y) * md.stride + x] = uint8_t(50 + x * 50);

    BLContext ctx(img);
    ctx.set_comp_op(BL_COMP_OP_SRC_COPY);
    ctx.fill_all(BLRgba32(0));
    ctx.set_fill_style(BLRgba32(0xffff0000));
    c.check(ctx.fill_mask(BLPointI(2, 2), mask) == BL_SUCCESS, "legacy aligned fill enqueues");
    ctx.flush(BL_CONTEXT_FLUSH_SYNC);
    ctx.end();

    c.check(pixel_at(img, 2, 2) == 0x32320000u, "legacy aligned first pixel");
    c.check(pixel_at(img, 4, 2) == 0x96960000u, "legacy aligned third pixel");
  }

  // Required exact value: A8=200, SRC_COPY red, global alpha 0.5 on a transparent target -> 0x64640000.
  {
    BLImage img(8, 8, BL_FORMAT_PRGB32);
    BLImage mask(1, 1, BL_FORMAT_A8);
    BLImageData md{};
    mask.make_mutable(&md);
    static_cast<uint8_t*>(md.pixel_data)[0] = 200;

    BLContext ctx(img);
    ctx.set_comp_op(BL_COMP_OP_SRC_COPY);
    ctx.fill_all(BLRgba32(0));
    ctx.set_global_alpha(0.5);
    ctx.set_fill_style(BLRgba32(0xffff0000));
    ctx.fill_mask(BLPointI(3, 3), mask);
    ctx.flush(BL_CONTEXT_FLUSH_SYNC);
    ctx.end();

    c.check(pixel_at(img, 3, 3) == 0x64640000u, "global alpha half exact value");
  }

  // The corrected async aligned path must be byte identical to the synchronous path under partial alpha.
  {
    BLImage sync(12, 12, BL_FORMAT_PRGB32), async(12, 12, BL_FORMAT_PRGB32);
    for (int mode = 0; mode < 2; ++mode) {
      BLImage& img = mode ? async : sync;
      BLContextCreateInfo info{};
      info.thread_count = mode ? 2u : 0u;
      BLContext ctx(img, info);

      BLImage mask(4, 4, BL_FORMAT_A8);
      BLImageData md{};
      mask.make_mutable(&md);
      for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 4; ++x)
          static_cast<uint8_t*>(md.pixel_data)[intptr_t(y) * md.stride + x] = uint8_t(30 + x * 40 + y * 17);

      ctx.set_comp_op(BL_COMP_OP_SRC_COPY);
      ctx.fill_all(BLRgba32(0xff202020));
      ctx.set_global_alpha(0.375);
      ctx.set_fill_style(BLRgba32(0xffc04080));
      ctx.fill_mask(BLPointI(2, 2), mask);
      ctx.flush(BL_CONTEXT_FLUSH_SYNC);
      ctx.end();
    }
    c.check(snapshot(sync) == snapshot(async), "aligned sync/async identical under partial alpha");
  }

  // Invalid mask_area keeps the existing error contract.
  {
    BLImage img(8, 8, BL_FORMAT_PRGB32);
    BLContext ctx(img);
    BLImage mask(3, 3, BL_FORMAT_A8);
    BLRectI bad(2, 0, 5, 3);  // x + w exceeds the mask width.
    c.check(ctx.fill_mask(BLPoint(0, 0), mask, bad) == BL_ERROR_INVALID_VALUE, "invalid area rejected");
    ctx.end();
  }

  // Empty area and a fully clipped mask both succeed and write nothing.
  {
    BLImage img(8, 8, BL_FORMAT_PRGB32);
    BLContext ctx(img);
    ctx.set_comp_op(BL_COMP_OP_SRC_COPY);
    ctx.fill_all(BLRgba32(kBg));

    BLImage mask(3, 3, BL_FORMAT_A8);
    BLImageData md{};
    mask.make_mutable(&md);
    for (int i = 0; i < 9; ++i)
      static_cast<uint8_t*>(md.pixel_data)[i] = 255;

    c.check(ctx.fill_mask(BLPoint(1.5, 1.5), mask, BLRectI(0, 0, 0, 3)) == BL_SUCCESS, "empty area succeeds");
    c.check(pixel_at(img, 2, 2) == kBg, "empty area writes nothing");

    ctx.clip_to_rect(BLRect(100, 100, 8, 8));
    c.check(ctx.fill_mask(BLPoint(0.5, 0.5), mask) == BL_SUCCESS, "fully clipped mask succeeds");
    c.check(pixel_at(img, 1, 1) == kBg, "fully clipped mask writes nothing");
    ctx.end();
  }

  // save()/restore() restores transform and clip around transformed mask draws.
  {
    BLImage img(12, 12, BL_FORMAT_PRGB32);
    BLContext ctx(img);
    ctx.set_comp_op(BL_COMP_OP_SRC_COPY);
    ctx.fill_all(BLRgba32(0));

    BLImage mask(2, 2, BL_FORMAT_A8);
    BLImageData md{};
    mask.make_mutable(&md);
    for (int i = 0; i < 4; ++i)
      static_cast<uint8_t*>(md.pixel_data)[i] = 255;

    ctx.save();
    ctx.clip_to_rect(BLRectI(0, 0, 6, 6));
    ctx.rotate(0.5);
    ctx.set_fill_style(BLRgba32(0xff00ff00));
    ctx.fill_mask(BLPoint(1.25, 1.25), mask);
    ctx.restore();

    // After restore, an identity-translation aligned mask lands at its literal device position.
    ctx.set_fill_style(BLRgba32(0xffff0000));
    ctx.fill_mask(BLPointI(8, 8), mask);
    ctx.flush(BL_CONTEXT_FLUSH_SYNC);
    ctx.end();

    c.check(pixel_at(img, 8, 8) == 0xffff0000u, "restore returns to identity placement");
    c.check(pixel_at(img, 9, 9) == 0xffff0000u, "restored aligned mask covered");
  }

  // Row-end sentinels: external target storage with guard bytes after every row must be untouched.
  {
    const int W = 9, H = 7, guard = 4;
    const int stride = W * 4 + guard;
    std::vector<uint8_t> buffer(size_t(stride) * H);
    std::memset(buffer.data(), 0, buffer.size());
    const uint32_t sentinel = 0xa5a5a5a5u;
    for (int y = 0; y < H; ++y)
      std::memcpy(buffer.data() + intptr_t(y) * stride + W * 4, &sentinel, guard);

    BLImage img;
    BLResult cr = img.create_from_data(W, H, BL_FORMAT_PRGB32, buffer.data(), stride, BL_DATA_ACCESS_RW, nullptr, nullptr);
    c.check(cr == BL_SUCCESS, "external target created");

    BLContext ctx(img);
    BLImage mask(4, 4, BL_FORMAT_A8);
    BLImageData md{};
    mask.make_mutable(&md);
    for (int i = 0; i < 16; ++i)
      static_cast<uint8_t*>(md.pixel_data)[i] = uint8_t(60 + i * 11);

    ctx.set_comp_op(BL_COMP_OP_SRC_COPY);
    ctx.set_fill_style(BLRgba32(0xff112233));
    ctx.fill_mask(BLPointI(1, 1), mask);             // Aligned, reaches the last target column.
    ctx.fill_mask(BLPoint(4.5, 2.5), mask);          // Fractional, exercises the affine rasterizer.
    ctx.flush(BL_CONTEXT_FLUSH_SYNC);
    ctx.end();

    bool sentinels_intact = true;
    for (int y = 0; y < H; ++y) {
      uint32_t got;
      std::memcpy(&got, buffer.data() + intptr_t(y) * stride + W * 4, guard);
      sentinels_intact &= (got == sentinel);
    }
    c.check(sentinels_intact, "row-end sentinels preserved");
  }
}

} // namespace

void register_task_tests(Registry& registry) {
  registry.add("sampling", "inverse-bilinear", sampling_cases);
  registry.add("styles", "coordinate-spaces", styles_cases);
  registry.add("deferred", "cow-and-release", deferred_cases);
  registry.add("compatibility", "legacy-and-invariants", compatibility_cases);
}
