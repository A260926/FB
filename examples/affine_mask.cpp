// Affine mask example for Blend2D.
//
// This program does not load any image or other resource. It builds an A8 coverage mask procedurally, then
// paints it through BLContext::fill_mask() while the context carries a rotation and fractional translation.
// The result is written as a binary PPM (P6) file, which any image viewer can open.
//
// Build (static, no-JIT library, as configured by task_support/build.sh):
//   c++ -std=c++17 -O1 -DBL_STATIC -I. examples/affine_mask.cpp <path>/libblend2d.a -lpthread -o affine_mask
//
// Run:
//   ./affine_mask [output.ppm]

#include <blend2d/blend2d.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

constexpr int kWidth = 160;
constexpr int kHeight = 120;
constexpr int kMaskSize = 24;

// Generates a soft circular A8 mask: full coverage in the center fading to zero toward the edges, with a
// transparent border so the bilinear footprint has room to bleed into zero coverage.
BLImage make_radial_mask() {
  BLImage mask(kMaskSize, kMaskSize, BL_FORMAT_A8);
  BLImageData data{};
  if (mask.make_mutable(&data) != BL_SUCCESS)
    return mask;

  const double cx = (kMaskSize - 1) / 2.0;
  const double cy = (kMaskSize - 1) / 2.0;
  const double radius = kMaskSize / 2.0 - 1.5;

  for (int y = 0; y < kMaskSize; ++y) {
    uint8_t* row = static_cast<uint8_t*>(data.pixel_data) + intptr_t(y) * data.stride;
    for (int x = 0; x < kMaskSize; ++x) {
      double dx = x - cx;
      double dy = y - cy;
      double d = std::sqrt(dx * dx + dy * dy);
      double t = (radius - d) / 1.5;             // ~1.5 px wide anti-aliased edge
      t = t < 0.0 ? 0.0 : (t > 1.0 ? 1.0 : t);
      row[x] = static_cast<uint8_t>(std::lround(t * 255.0));
    }
  }
  return mask;
}

bool write_ppm(const char* file_name, const BLImage& image) {
  BLImageData data{};
  if (image.get_data(&data) != BL_SUCCESS)
    return false;

  std::FILE* f = std::fopen(file_name, "wb");
  if (!f)
    return false;

  std::fprintf(f, "P6\n%d %d\n255\n", image.width(), image.height());
  for (int y = 0; y < image.height(); ++y) {
    const uint32_t* src = reinterpret_cast<const uint32_t*>(
      static_cast<const uint8_t*>(data.pixel_data) + intptr_t(y) * data.stride);
    for (int x = 0; x < image.width(); ++x) {
      uint32_t px = src[x];                        // Premultiplied 0xAARRGGBB.
      uint8_t rgb[3] = {
        uint8_t((px >> 16) & 0xFF),
        uint8_t((px >> 8) & 0xFF),
        uint8_t(px & 0xFF)
      };
      std::fwrite(rgb, 1, 3, f);
    }
  }
  std::fclose(f);
  return true;
}

} // namespace

int main(int argc, char** argv) {
  const char* output = argc > 1 ? argv[1] : "affine_mask.ppm";

  BLImage image(kWidth, kHeight, BL_FORMAT_PRGB32);
  BLContext ctx(image);

  // Opaque dark background.
  ctx.set_comp_op(BL_COMP_OP_SRC_COPY);
  ctx.fill_all(BLRgba32(0xff141822));

  BLImage mask = make_radial_mask();

  // Paint the same mask several times. The mask origin maps the *top-left of the selected area* into the
  // transformed user space, while the brush keeps its own coordinate system. Fractional origins and the
  // rotation exercise the bilinear, inverse-transform sampling path.
  const uint32_t colors[] = {
    0xffe04040u,   // Red.
    0xff40c060u,   // Green.
    0xff4070e0u,   // Blue.
    0xffe0c040u    // Amber.
  };

  ctx.set_comp_op(BL_COMP_OP_SRC_OVER);
  for (int i = 0; i < 4; ++i) {
    ctx.save();
    // Rotate around the image center; each pass uses a slightly different angle.
    ctx.translate(kWidth / 2.0, kHeight / 2.0);
    ctx.rotate(0.35 * i);

    double half = kMaskSize / 2.0;
    BLPoint origin(-half + (i & 1) * 0.37, -half + (i & 1) * 0.61);  // Fractional on alternating passes.

    ctx.set_fill_style(BLRgba32(colors[i]));
    BLResult result = ctx.fill_mask(origin, mask);
    if (result != BL_SUCCESS) {
      std::fprintf(stderr, "fill_mask failed: %u\n", unsigned(result));
      return 1;
    }
    ctx.restore();
  }

  ctx.flush(BL_CONTEXT_FLUSH_SYNC);
  ctx.end();

  if (!write_ppm(output, image)) {
    std::fprintf(stderr, "failed to write %s\n", output);
    return 1;
  }

  std::printf("wrote %s (%dx%d PPM)\n", output, kWidth, kHeight);
  return 0;
}
