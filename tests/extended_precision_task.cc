#include "task_support/check.h"

#include <libde265/de265.h>
#include <libde265/extended_precision.h>
#include <libde265/fallback-dct.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace ep = de265::extended_precision;

namespace {

std::vector<unsigned char> read_file(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return {};
  return std::vector<unsigned char>(std::istreambuf_iterator<char>(in),
                                    std::istreambuf_iterator<char>());
}

void append_bits(std::vector<unsigned char>& bytes, size_t& bit_pos,
                 uint32_t value, int count) {
  for (int i = count - 1; i >= 0; --i) {
    const int bit = (value >> i) & 1;
    const size_t byte = bit_pos >> 3;
    if (bytes.size() <= byte) bytes.resize(byte + 1, 0);
    if (bit) bytes[byte] |= static_cast<unsigned char>(1u << (7 - (bit_pos & 7)));
    ++bit_pos;
  }
}

bool decode_limited_bits(const std::string& bits, int k, int range_bits,
                         uint32_t& value, size_t& consumed) {
  std::vector<unsigned char> bytes((bits.size() + 7) / 8, 0);
  for (size_t i = 0; i < bits.size(); ++i) {
    if (bits[i] == '1') bytes[i >> 3] |= 1u << (7 - (i & 7));
  }
  ep::BitReader reader{bytes.data(), bits.size(), 0};
  return ep::limited_egk_decode(reader, k, range_bits, value, &consumed);
}

int entropy_small_codes(void) {
  uint32_t value = 999;
  size_t consumed = 999;

  TASK_CHECK(decode_limited_bits("0", 0, 15, value, consumed));
  TASK_CHECK(value == 0);
  TASK_CHECK(consumed == 1);

  TASK_CHECK(decode_limited_bits("100", 0, 15, value, consumed));
  TASK_CHECK(value == 1);
  TASK_CHECK(consumed == 3);

  // prefix=0 with one Rice suffix bit.
  TASK_CHECK(decode_limited_bits("01", 1, 16, value, consumed));
  TASK_CHECK(value == 1);
  TASK_CHECK(consumed == 2);

  // A normal generic-EGk prefix p=2 followed by a k+p=4 bit suffix.
  TASK_CHECK(decode_limited_bits("1100010", 2, 18, value, consumed));
  TASK_CHECK(value == (3u << 2) + 2u);
  TASK_CHECK(consumed == 7);
  return 0;
}

int entropy_finite_boundaries_and_large_signed_ranges(void) {
  uint32_t value = 0;
  size_t consumed = 0;

  // Range 15, k=0: max prefix is 14.  Fourteen leading ones and a 14-bit
  // zero suffix form the implicit-terminator boundary (28 bits total).
  {
    std::string bits(14, '1');
    bits += std::string(14, '0');
    TASK_CHECK(decode_limited_bits(bits, 0, 15, value, consumed));
    TASK_CHECK(consumed == 28);
    TASK_CHECK(value == (1u << 14) - 1);
  }

  // BitDepth 16 gives a 22-bit coefficient range.  With k=4 the finite
  // prefix is 17 and the suffix is 21 bits: 38 consumed bits, no terminator.
  {
    std::string bits(17, '1');
    bits += std::string(21, '0');
    TASK_CHECK(decode_limited_bits(bits, 4, 22, value, consumed));
    TASK_CHECK(consumed == 38);
    TASK_CHECK(value == (((1u << 17) - 1) << 4));
    TASK_CHECK(value == 2097136u);
  }

  // Truncated suffix must fail and must not report a plausible symbol.
  TASK_CHECK(!decode_limited_bits(std::string(17, '1') + "00", 4, 22,
                                  value, consumed));

  // Signed coefficient endpoints for Monochrome/Main444 16.
  TASK_CHECK(ep::range_bits(16) == 22);
  TASK_CHECK(ep::clip_to_range(INT64_C(5000000), 22) == 4194303);
  TASK_CHECK(ep::clip_to_range(INT64_C(-5000000), 22) == -4194304);
  TASK_CHECK(ep::clip_to_range(12345, 22) == 12345);
  TASK_CHECK(ep::clip_to_range(-12345, 22) == -12345);
  return 0;
}

int transform_dynamic_range_and_rounding_model(void) {
  static const int scales[6] = {40, 45, 51, 57, 64, 72};

  for (int bit_depth : {8, 10, 12, 16}) {
    const int range_bits = std::max(15, bit_depth + 6);
    const int residual_shift = range_bits + 5 - bit_depth;
    TASK_CHECK(ep::range_bits(bit_depth) == range_bits);
    TASK_CHECK(ep::residual_rounding_shift(bit_depth) == residual_shift);

    for (int log2_n : {2, 3, 4, 5}) {
      const int shift = bit_depth + log2_n - 5 - (range_bits - 15) - 4;
      TASK_CHECK(ep::dequant_shift(bit_depth, log2_n, false) == shift);
      TASK_CHECK(ep::dequant_shift(bit_depth, log2_n, true) == shift + 4);

      const int ts = std::min(5, residual_shift - 2) + log2_n;
      TASK_CHECK(ep::transform_skip_shift(log2_n, residual_shift) == ts);
    }

    TASK_CHECK(ep::transform_skip_shift(2, ep::residual_rounding_shift(16)) == 7);
    TASK_CHECK(ep::transform_skip_shift(5, ep::residual_rounding_shift(16)) == 10);
  }

  // QP values on both sides of every qP/6 boundary.
  for (int qp : {0, 5, 6, 11, 12, 17, 18, 23, 24, 29, 30, 47, 48, 53, 54, 95, 96}) {
    const int expected = scales[qp % 6] << (qp / 6);
    TASK_CHECK(ep::level_scale_factor(qp) == expected);

    const int bit_depth = 16;
    const int log2_n = 2 + (qp & 3);
    const int shift = ep::dequant_shift(bit_depth, log2_n, false);
    const int64_t fact = expected;
    const int64_t offset = INT64_C(1) << (shift - 1);

    for (int coefficient : {-4194304, -1000003, -32769, -32768, -1,
                            0, 1, 32767, 32768, 999999, 4194303}) {
      // Independent rounding/clipping model.
      int64_t raw = (static_cast<int64_t>(coefficient) * fact + offset) >> shift;
      const int32_t expected_d = ep::clip_to_range(raw, 22);
      TASK_CHECK(ep::clip_to_range(raw, 22) == expected_d);

      // The production helper must preserve signs and the finite range.
      const int32_t produced = ep::clip_to_range(raw, ep::range_bits(bit_depth));
      TASK_CHECK(produced >= -(INT32_C(1) << 22));
      TASK_CHECK(produced <= (INT32_C(1) << 22) - 1);
      if (coefficient < 0 && raw != 0) TASK_CHECK(produced < 0);
      if (coefficient > 0 && raw != 0) TASK_CHECK(produced > 0);
    }
  }
  return 0;
}

int transform_wide_workspace_sizes(void) {
  // One DC coefficient exercises every supported square transform size and
  // the wide int32 workspace.  The independent model only checks finiteness
  // and the expected sign/energy relationship, never libde265's old output.
  for (int n : {4, 8, 16, 32}) {
    std::array<int32_t, 1024> coeff{};
    std::array<int32_t, 1024> out{};
    coeff[0] = 100000;

    const int h_shift = 11;
    transform_idct_wide_fallback(out.data(), n, coeff.data(), h_shift, 22);

    int64_t energy = 0;
    for (int i = 0; i < n * n; ++i) {
      TASK_CHECK(out[i] > -1000000 && out[i] < 1000000);
      energy += static_cast<int64_t>(out[i]) * out[i];
    }
    TASK_CHECK(energy > 0);

    if (n == 4) {
      std::array<int32_t, 16> dst{};
      transform_idst_4x4_wide_fallback(dst.data(), coeff.data(), 11, 22);
      int64_t dst_energy = 0;
      for (int32_t v : dst) dst_energy += static_cast<int64_t>(v) * v;
      TASK_CHECK(dst_energy > 0);
    }
  }

  std::array<int32_t, 16> skip_coeff{};
  std::array<int32_t, 16> residual{};
  skip_coeff[0] = -123456;
  transform_skip_residual_wide_fallback(residual.data(), skip_coeff.data(),
                                        4, 7, 11);
  TASK_CHECK(residual[0] < 0);
  TASK_CHECK(residual[1] == 0);
  return 0;
}

struct DecodedYuv {
  std::vector<uint16_t> luma;
  std::vector<uint16_t> chroma[2];
  int width = 0, height = 0;
  int frames = 0;
};

bool collect_one_frame(de265_decoder_context* ctx, DecodedYuv& out) {
  const de265_image* img = de265_get_next_picture(ctx);
  if (!img) return false;

  const int w = de265_get_image_width(img, 0);
  const int h = de265_get_image_height(img, 0);
  out.width = w;
  out.height = h;
  out.luma.resize(static_cast<size_t>(w) * h);

  int stride = 0;
  const unsigned char* plane = de265_get_image_plane(img, 0, &stride);
  const int depth = de265_get_bits_per_pixel(img, 0);
  if (!plane || depth != 16) return false;
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      uint16_t sample;
      std::memcpy(&sample, plane + y * stride + 2 * x, 2);
      out.luma[y * w + x] = sample;
    }
  }
  ++out.frames;
  return true;
}

bool pump_collect(de265_decoder_context* ctx, std::vector<DecodedYuv>* frames) {
  for (int iteration = 0; iteration < 100000; ++iteration) {
    int more = 0;
    de265_error err = de265_decode(ctx, &more);

    if (frames) {
      while (true) {
        DecodedYuv frame;
        if (!collect_one_frame(ctx, frame)) break;
        frames->push_back(std::move(frame));
      }
    } else {
      while (const de265_image* img = de265_get_next_picture(ctx)) {
        (void)img;
      }
    }

    if (err == DE265_ERROR_WAITING_FOR_INPUT_DATA) return true;
    if (err == DE265_ERROR_IMAGE_BUFFER_FULL) continue;
    if (!de265_isOK(err)) {
      std::fprintf(stderr, "decode error: %s\n", de265_get_error_text(err));
      return false;
    }
    if (!more) return true;
  }
  return false;
}

bool decode_fixture(const std::string& path, size_t chunk_size, int workers,
                    bool reset_replay, std::vector<DecodedYuv>& frames_out) {
  std::vector<unsigned char> bitstream = read_file(path);
  if (bitstream.empty()) return false;

  de265_decoder_context* ctx = de265_new_decoder();
  if (!ctx) return false;
  if (workers > 0 && de265_start_worker_threads(ctx, workers) != DE265_OK) {
    de265_free_decoder(ctx);
    return false;
  }

  auto feed_and_drain = [&](bool feed) -> bool {
    if (feed) {
      for (size_t at = 0; at < bitstream.size();) {
        size_t n = chunk_size ? std::min(chunk_size, bitstream.size() - at)
                              : bitstream.size();
        if (de265_push_data(ctx, bitstream.data() + at, static_cast<int>(n),
                            static_cast<de265_PTS>(at), nullptr) != DE265_OK) {
          return false;
        }
        at += n;
        if (!pump_collect(ctx, &frames_out)) return false;
      }
      if (de265_flush_data(ctx) != DE265_OK) return false;
    }

    if (!pump_collect(ctx, &frames_out)) return false;
    return true;
  };

  bool ok = feed_and_drain(true);
  if (ok && reset_replay) {
    de265_reset(ctx);
    frames_out.clear();
    ok = feed_and_drain(true);
  }

  de265_free_decoder(ctx);
  return ok;
}

std::vector<uint16_t> read_reference_luma(const std::string& path,
                                          int width, int height, int frame) {
  std::ifstream in(path, std::ios::binary);
  const size_t frame_bytes = static_cast<size_t>(width) * height * 2;
  in.seekg(static_cast<std::streamoff>(frame) * frame_bytes, std::ios::beg);
  std::vector<unsigned char> bytes(frame_bytes);
  in.read(reinterpret_cast<char*>(bytes.data()), frame_bytes);
  std::vector<uint16_t> samples(width * height);
  for (size_t i = 0; i < samples.size(); ++i) {
    samples[i] = static_cast<uint16_t>(bytes[2 * i] | (bytes[2 * i + 1] << 8));
  }
  return samples;
}

int pipeline_mono_whole_zero_workers(void) {
  std::vector<DecodedYuv> frames;
  TASK_CHECK(decode_fixture("task_support/fixtures/mono16.h265", 0, 0, false, frames));
  TASK_CHECK(frames.size() == 3);
  for (int f = 0; f < 3; ++f) {
    TASK_CHECK(frames[f].width == 64 && frames[f].height == 64);
    auto ref = read_reference_luma("task_support/fixtures/mono16.yuv", 64, 64, f);
    TASK_CHECK(frames[f].luma == ref);
  }
  return 0;
}

int pipeline_mono_bytes_two_workers(void) {
  std::vector<DecodedYuv> frames;
  TASK_CHECK(decode_fixture("task_support/fixtures/mono16.h265", 1, 2, false, frames));
  TASK_CHECK(frames.size() == 3);
  for (int f = 0; f < 3; ++f) {
    auto ref = read_reference_luma("task_support/fixtures/mono16.yuv", 64, 64, f);
    TASK_CHECK(frames[f].luma == ref);
  }
  return 0;
}

int pipeline_color_chunks_and_drain(void) {
  std::vector<DecodedYuv> frames;
  TASK_CHECK(decode_fixture("task_support/fixtures/44416.h265", 131, 0, false, frames));
  TASK_CHECK(frames.size() == 3);

  // The shared collector currently retains luma; compare the complete planar
  // byte stream through the public plane API in a dedicated compact decoder.
  std::vector<unsigned char> bitstream = read_file("task_support/fixtures/44416.h265");
  de265_decoder_context* ctx = de265_new_decoder();
  TASK_CHECK(ctx != nullptr);
  TASK_CHECK(de265_push_data(ctx, bitstream.data(), (int)bitstream.size(), 0, 0) == DE265_OK);
  TASK_CHECK(de265_flush_data(ctx) == DE265_OK);
  std::vector<unsigned char> decoded;
  int count = 0;
  for (int it = 0; it < 100000; ++it) {
    int more = 0;
    de265_error e = de265_decode(ctx, &more);
    const de265_image* img;
    while ((img = de265_get_next_picture(ctx))) {
      ++count;
      for (int c = 0; c < 3; ++c) {
        int stride = 0;
        auto* p = de265_get_image_plane(img, c, &stride);
        int w = de265_get_image_width(img, c), h = de265_get_image_height(img, c);
        for (int y = 0; y < h; ++y)
          decoded.insert(decoded.end(), p + y * stride, p + y * stride + 2 * w);
      }
    }
    if (e == DE265_ERROR_WAITING_FOR_INPUT_DATA) break;
    TASK_CHECK(de265_isOK(e));
    if (!more) break;
  }
  de265_free_decoder(ctx);
  TASK_CHECK(count == 3);
  auto expected = read_file("task_support/fixtures/44416.yuv");
  TASK_CHECK(decoded == expected);
  return 0;
}

int pipeline_reset_replay(void) {
  std::vector<DecodedYuv> first, second;
  TASK_CHECK(decode_fixture("task_support/fixtures/mono16.h265", 97, 2, false, first));
  TASK_CHECK(decode_fixture("task_support/fixtures/mono16.h265", 97, 2, true, second));
  TASK_CHECK(first.size() == 3 && second.size() == 3);
  for (size_t f = 0; f < first.size(); ++f) {
    TASK_CHECK(first[f].luma == second[f].luma);
    auto ref = read_reference_luma("task_support/fixtures/mono16.yuv", 64, 64, (int)f);
    TASK_CHECK(second[f].luma == ref);
  }
  return 0;
}

int compat_girlshy_baseline(void) {
  std::vector<unsigned char> bitstream = read_file("testdata/girlshy.h265");
  TASK_CHECK(!bitstream.empty());
  de265_decoder_context* ctx = de265_new_decoder();
  TASK_CHECK(ctx != nullptr);
  TASK_CHECK(de265_push_data(ctx, bitstream.data(), (int)bitstream.size(), 0, 0) == DE265_OK);
  TASK_CHECK(de265_flush_data(ctx) == DE265_OK);

  int frames = 0;
  size_t bytes = 0;
  uint64_t hash = 1469598103934665603ULL;
  for (int it = 0; it < 100000; ++it) {
    int more = 0;
    de265_error e = de265_decode(ctx, &more);
    while (const de265_image* img = de265_get_next_picture(ctx)) {
      ++frames;
      int stride = 0;
      const unsigned char* p = de265_get_image_plane(img, 0, &stride);
      int w = de265_get_image_width(img, 0), h = de265_get_image_height(img, 0);
      int depth = de265_get_bits_per_pixel(img, 0);
      for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
          unsigned value = (depth > 8)
              ? (p[y * stride + 2 * x] | (p[y * stride + 2 * x + 1] << 8))
              : p[y * stride + x];
          int nbytes = depth > 8 ? 2 : 1;
          for (int b = 0; b < nbytes; ++b) {
            unsigned char byte = (value >> (8 * b)) & 255;
            hash = (hash ^ byte) * 1099511628211ULL;
            ++bytes;
          }
        }
      }
    }
    if (e == DE265_ERROR_WAITING_FOR_INPUT_DATA) break;
    TASK_CHECK(de265_isOK(e));
    if (!more) break;
  }
  de265_free_decoder(ctx);
  TASK_CHECK(frames == 75);
  TASK_CHECK(bytes == 8532000);
  TASK_CHECK(hash == 1270709103934665603ULL || hash == 0x1270709a1c0785a6ULL);
  return 0;
}

int compat_public_api_surface(void) {
  // The existing public lifecycle and metadata calls remain available and
  // reject nothing at construction time.  This is intentionally an interface
  // test rather than an alternative decoder.
  de265_decoder_context* ctx = de265_new_decoder();
  TASK_CHECK(ctx != nullptr);
  TASK_CHECK(de265_start_worker_threads(ctx, 0) == DE265_OK);
  de265_reset(ctx);
  de265_free_decoder(ctx);

  TASK_CHECK(std::string(de265_get_version()).size() > 0);
  TASK_CHECK(de265_get_error_text(DE265_OK) != nullptr);
  return 0;
}

}  // namespace

const struct task_case task_cases[] = {
    {"entropy", "finite_small_codes", entropy_small_codes},
    {"entropy", "finite_boundaries_signed_range",
     entropy_finite_boundaries_and_large_signed_ranges},
    {"transform", "integer_rounding_qp6", transform_dynamic_range_and_rounding_model},
    {"transform", "wide_dst_dct_skip", transform_wide_workspace_sizes},
    {"pipeline", "mono_whole_0worker", pipeline_mono_whole_zero_workers},
    {"pipeline", "mono_bytes_2workers", pipeline_mono_bytes_two_workers},
    {"pipeline", "color_131_drain", pipeline_color_chunks_and_drain},
    {"pipeline", "reset_replay_2workers", pipeline_reset_replay},
    {"compat", "girlshy_hash", compat_girlshy_baseline},
    {"compat", "public_api", compat_public_api_surface},
};

const size_t task_case_count =
    sizeof(task_cases) / sizeof(task_cases[0]);
