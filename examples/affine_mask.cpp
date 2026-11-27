// Affine A8 mask fill example.
//
// Generates an A8 coverage mask procedurally (no external images), places it with a fractional origin and a
// rotation through BLContext::fill_mask(), then writes the resulting PRGB32 target as a binary PPM (P6) file.
//
// Build (same flags as the rest of the task tree):
//   c++ -std=c++17 -O1 -DBL_STATIC -I. examples/affine_mask.cpp build/libblend2d.a -lpthread -o affine_mask
// Run:
//   ./affine_mask [output.ppm]

#include <blend2d/blend2d.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cmath>

namespace {

constexpr int kCanvasW = 96;
constexpr int kCanvasH = 96;
constexpr int kMaskW = 24;
constexpr int kMaskH = 18;

// Builds a soft radial coverage mask: 255 in the center, falling off toward the edges. The returned image is a
// regular BLImage in BL_FORMAT_A8, exactly the source type accepted by fill_mask().
BLImage make_radial_mask() {
  BLImage mask(kMaskW, kMaskH, BL_FORMAT_A8);
  BLImageData md;
  mask.make_mutable(&md);

  const double cx = (kMaskW - 1) * 0.5;
  const double cy = (kMaskH - 1) * 0.5;
  const double radius = 0.5 * std::sqrt(cx * cx + cy * cy);

  for (int y = 0; y < kMaskH; y++) {
    uint8_t* row = static_cast<uint8_t*>(md.pixel_data) + intptr_t(y) * md.stride;
    for (int x = 0; x < kMaskW; x++) {
      double dx = x - cx;
      double dy = y - cy;
      double t = std::sqrt(dx * dx + dy * dy) / radius;
      double coverage = 255.0 * (1.0 - std::min(1.0, t));
      row[x] = uint8_t(std::round(coverage));
    }
  }
  return mask;
}

// Writes a PRGB32 image as a binary PPM (P6), converting premultiplied pixels back to straight RGB on output.
bool write_ppm(const char* path, const BLImage& image) {
  BLImageData d;
  if (image.get_data(&d) != BL_SUCCESS)
    return false;

  std::FILE* f = std::fopen(path, "wb");
  if (!f)
    return false;

  std::fprintf(f, "P6\n%d %d\n255\n", d.size.w, d.size.h);
  for (int y = 0; y < d.size.h; y++) {
    const uint32_t* src = reinterpret_cast<const uint32_t*>(
      static_cast<const uint8_t*>(d.pixel_data) + intptr_t(y) * d.stride);
    for (int x = 0; x < d.size.w; x++) {
      uint32_t px = src[x];
      uint8_t bgra[3];
      uint32_t a = (px >> 24) & 0xff;
      if (a == 0) {
        bgra[0] = bgra[1] = bgra[2] = 0;
      }
      else {
        // Undo premultiplication so the PPM stores straight color.
        bgra[0] = uint8_t(std::min(255.0, std::round(double((px >> 0) & 0xff) * 255.0 / double(a))));
        bgra[1] = uint8_t(std::min(255.0, std::round(double((px >> 8) & 0xff) * 255.0 / double(a))));
        bgra[2] = uint8_t(std::min(255.0, std::round(double((px >> 16) & 0xff) * 255.0 / double(a))));
      }
      std::fwrite(bgra, 1, 3, f);
    }
  }
  std::fclose(f);
  return true;
}

} // namespace

int main(int argc, char** argv) {
  const char* out_path = argc > 1 ? argv[1] : "affine_mask.ppm";

  BLImage mask = make_radial_mask();

  // Render twice with independent transforms onto a checkerboard background so the SRC_OVER blending and the
  // fractional, rotated mask placement are both visible.
  BLImage canvas(kCanvasW, kCanvasH, BL_FORMAT_PRGB32);
  {
    BLContext ctx(canvas);

    // Checkerboard background.
    for (int y = 0; y < kCanvasH; y += 8) {
      for (int x = 0; x < kCanvasW; x += 8) {
        BLRgba32 color = ((x / 8 + y / 8) & 1) ? BLRgba32(0xff202830) : BLRgba32(0xff2c3640);
        ctx.fill_rect(BLRectI(x, y, 8, 8), color);
      }
    }

    // First mask: fractional origin with the identity transform.
    ctx.set_comp_op(BL_COMP_OP_SRC_OVER);
    ctx.set_fill_style(BLRgba32(0xffd04030));
    ctx.fill_mask(BLPoint(6.25, 8.75), mask);

    // Second mask: rotated about the canvas center with a fractional origin, exercising the affine branch.
    ctx.set_fill_style(BLRgba32(0xff30a0d0));
    ctx.translate(48.0, 48.0);
    ctx.rotate(0.6);
    ctx.fill_mask(BLPoint(-12.5, -9.0), mask);
  }

  if (!write_ppm(out_path, canvas)) {
    std::fprintf(stderr, "failed to write %s\n", out_path);
    return 1;
  }

  std::printf("wrote %s (%dx%d P6 PPM)\n", out_path, kCanvasW, kCanvasH);
  return 0;
}
