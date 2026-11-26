// extended_precision - decode an HEVC Range Extensions stream using only the
// public libde265 C API and write concatenated planar samples.
//
// Usage:
//   extended_precision INPUT.h265 OUTPUT.yuv [worker-threads]
//
// Output layout:
//   * Each decoded frame is written completely before the next frame.
//   * Planes are written in Cb/Cr order for YUV; monochrome streams write one
//     plane.  For the fixtures, the order is Y then U then V.
//   * Every sample is written in the host's native 16-bit representation on
//     this target (the task fixtures and reference YUV use little-endian).
//   * Rows are written without padding: each output row is width*2 bytes for
//     10/12/16-bit samples and width byte for 8-bit samples.  The stride
//     returned by de265_get_image_plane() is a byte stride and may be larger
//     than the active row width, so it must not be copied contiguously as one
//     plane.  Copy row-by-row as below.
//
// Scope/limitations of this task's scalar extended-precision path:
//   * Monochrome 16 and Main 4:4:4 16 Intra are the validated configurations.
//   * Only extended_precision_processing_flag adds the wide coefficient and
//     transform path.  SCC tools, layered streams, and field coding are not
//     extended.  Streams without the flag continue to use the original paths.
//   * The output consists of normal clipped 16-bit (or source bit-depth)
//     reconstructed samples; there is no second display-only correction pass.

#include <libde265/de265.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace {

std::vector<unsigned char> read_file(const char* path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return {};
  return std::vector<unsigned char>(std::istreambuf_iterator<char>(in),
                                    std::istreambuf_iterator<char>());
}

struct Digest {
  uint64_t fnv = 1469598103934665603ULL;
  uint64_t samples = 0;
  int min_value = 65535;
  int max_value = 0;

  void add(unsigned value, int bytes) {
    for (int b = 0; b < bytes; ++b) {
      unsigned char byte = static_cast<unsigned char>((value >> (8 * b)) & 255);
      fnv = (fnv ^ byte) * 1099511628211ULL;
    }
    ++samples;
    min_value = std::min(min_value, static_cast<int>(value));
    max_value = std::max(max_value, static_cast<int>(value));
  }
};

int drain_pictures(de265_decoder_context* ctx, std::ofstream& out,
                    Digest& digest, int& frames) {
  while (const de265_image* img = de265_get_next_picture(ctx)) {
    const de265_chroma chroma = de265_get_chroma_format(img);
    const int planes = (chroma == de265_chroma_mono) ? 1 : 3;

    for (int plane = 0; plane < planes; ++plane) {
      int stride = 0;
      const unsigned char* row = de265_get_image_plane(img, plane, &stride);
      const int width = de265_get_image_width(img, plane);
      const int height = de265_get_image_height(img, plane);
      const int bit_depth = de265_get_bits_per_pixel(img, plane);
      if (!row || width <= 0 || height <= 0 || bit_depth < 8 || bit_depth > 16) {
        std::fprintf(stderr, "invalid plane %d\n", plane);
        return 20;
      }

      const int sample_bytes = (bit_depth > 8) ? 2 : 1;
      const int output_row_bytes = width * sample_bytes;

      for (int y = 0; y < height; ++y) {
        const unsigned char* src = row + static_cast<size_t>(y) * stride;
        out.write(reinterpret_cast<const char*>(src), output_row_bytes);
        if (!out) return 21;

        for (int x = 0; x < width; ++x) {
          unsigned value;
          if (sample_bytes == 2) {
            uint16_t sample;
            std::memcpy(&sample, src + 2 * x, sizeof(sample));
            value = sample;
          } else {
            value = src[x];
          }
          digest.add(value, sample_bytes);
        }
      }
    }

    ++frames;
  }
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 3 && argc != 4) {
    std::fprintf(stderr, "usage: %s INPUT.h265 OUTPUT.yuv [workers]\n", argv[0]);
    return 2;
  }

  const std::vector<unsigned char> bitstream = read_file(argv[1]);
  if (bitstream.empty()) {
    std::fprintf(stderr, "cannot read input bitstream: %s\n", argv[1]);
    return 3;
  }

  std::ofstream out(argv[2], std::ios::binary | std::ios::trunc);
  if (!out) {
    std::fprintf(stderr, "cannot open output YUV: %s\n", argv[2]);
    return 4;
  }

  de265_decoder_context* ctx = de265_new_decoder();
  if (!ctx) return 5;

  if (argc == 4) {
    const int workers = std::atoi(argv[3]);
    if (workers > 0) {
      de265_error err = de265_start_worker_threads(ctx, workers);
      if (!de265_isOK(err)) {
        std::fprintf(stderr, "cannot start %d workers: %s\n", workers,
                     de265_get_error_text(err));
        de265_free_decoder(ctx);
        return 6;
      }
    }
  }

  de265_error err = de265_push_data(ctx, bitstream.data(),
                                    static_cast<int>(bitstream.size()), 0, 0);
  if (!de265_isOK(err)) {
    std::fprintf(stderr, "push failed: %s\n", de265_get_error_text(err));
    de265_free_decoder(ctx);
    return 7;
  }

  err = de265_flush_data(ctx);
  if (!de265_isOK(err)) {
    std::fprintf(stderr, "flush failed: %s\n", de265_get_error_text(err));
    de265_free_decoder(ctx);
    return 8;
  }

  Digest digest;
  int frames = 0;
  int status = 0;

  for (int iterations = 0; iterations < 100000; ++iterations) {
    int more = 0;
    err = de265_decode(ctx, &more);

    status = drain_pictures(ctx, out, digest, frames);
    if (status) break;

    if (err == DE265_ERROR_WAITING_FOR_INPUT_DATA) break;
    if (err == DE265_ERROR_IMAGE_BUFFER_FULL) continue;
    if (!de265_isOK(err)) {
      std::fprintf(stderr, "decode error: %s\n", de265_get_error_text(err));
      status = 9;
      break;
    }
    if (!more) break;
  }

  out.close();
  de265_free_decoder(ctx);

  if (status) return status;
  if (frames == 0 || digest.samples == 0) {
    std::fprintf(stderr, "no decoded frames\n");
    return 10;
  }

  std::printf("frames=%d samples=%llu min=%d max=%d fnv1a=%016llx output=%s\n",
              frames, static_cast<unsigned long long>(digest.samples),
              digest.min_value, digest.max_value,
              static_cast<unsigned long long>(digest.fnv), argv[2]);
  return 0;
}
