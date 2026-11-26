// row_restart.cc - example for xvc per CTU row CABAC restart (version 3.0).
//
// Generates five deterministic 128x128 8-bit I420 frames, encodes them with
// row_restart enabled (every CTU row becomes an independently initialized and
// terminated CABAC substream), decodes the resulting bitstream and writes:
//   row-restart.xvc - length prefixed NAL units (4 byte big endian length)
//   row-restart.yuv - decoded frames as tightly packed planar I420
//
// On success it prints: ROW_RESTART_EXAMPLE frames=5
//
// Build (from the repository root):
//   c++ -std=c++11 -O1 -Isrc examples/row_restart.cc \
//       build/src/libxvcenc.a build/src/libxvcdec.a -pthread -o row-example

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "xvc_dec_lib/xvcdec.h"
#include "xvc_enc_lib/xvcenc.h"

namespace {

void WriteBe32(std::ostream &out, uint32_t value) {
  unsigned char len[4] = {
    static_cast<unsigned char>((value >> 24) & 0xff),
    static_cast<unsigned char>((value >> 16) & 0xff),
    static_cast<unsigned char>((value >> 8) & 0xff),
    static_cast<unsigned char>(value & 0xff)
  };
  out.write(reinterpret_cast<const char*>(len), 4);
}

}  // namespace

int main() {
  const int kWidth = 128;
  const int kHeight = 128;
  const int kFrames = 5;

  const xvc_encoder_api *enc_api = xvc_encoder_api_get();
  const xvc_decoder_api *dec_api = xvc_decoder_api_get();

  xvc_encoder_parameters *param = enc_api->parameters_create();
  if (!param || enc_api->parameters_set_default(param) != XVC_ENC_OK) {
    std::cerr << "encoder parameters init failed\n";
    return 1;
  }
  param->width = kWidth;
  param->height = kHeight;
  param->input_bitdepth = 8;
  param->internal_bitdepth = 8;
  param->chroma_format = XVC_ENC_CHROMA_FORMAT_420;
  param->speed_mode = 2;
  param->qp = 32;
  param->sub_gop_length = 4;
  param->max_keypic_distance = 64;
  param->simd_mask = 0;
  param->threads = 0;

  // New public setter: default is off, here explicitly enabled.
  if (xvc_encoder_set_row_restart(param, 1) != XVC_ENC_OK) {
    std::cerr << "xvc_encoder_set_row_restart failed\n";
    return 1;
  }
  if (enc_api->parameters_check(param) != XVC_ENC_OK) {
    std::cerr << "encoder parameter check failed\n";
    return 1;
  }

  xvc_encoder *encoder = enc_api->encoder_create(param);
  if (!encoder) {
    std::cerr << "encoder create failed\n";
    return 1;
  }

  xvc_decoder_parameters *dec_param = dec_api->parameters_create();
  dec_api->parameters_set_default(dec_param);
  dec_param->output_bitdepth = 8;
  dec_param->threads = 0;
  dec_param->simd_mask = 0;
  xvc_decoder *decoder = dec_api->decoder_create(dec_param);
  if (!decoder) {
    std::cerr << "decoder create failed\n";
    return 1;
  }
  xvc_decoded_picture *out_pic = dec_api->picture_create(decoder);

  xvc_enc_pic_buffer *rec_pic = enc_api->picture_create(encoder);

  std::ofstream bitstream("row-restart.xvc", std::ios::binary);
  std::ofstream raw_yuv("row-restart.yuv", std::ios::binary);
  if (!bitstream || !raw_yuv) {
    std::cerr << "cannot open output files\n";
    return 1;
  }

  int decoded_frames = 0;
  bool ok = true;

  auto drain = [&]() {
    xvc_dec_return_code rc;
    while ((rc = dec_api->decoder_get_picture(decoder, out_pic))
           == XVC_DEC_OK) {
      if (out_pic->stats.width != kWidth ||
          out_pic->stats.height != kHeight ||
          out_pic->stats.bitdepth != 8 ||
          !out_pic->stats.conforming) {
        std::cerr << "unexpected decoded picture stats\n";
        ok = false;
        return;
      }
      // Tightly packed I420: full luma then half resolution chroma planes.
      for (int c = 0; c < 3; ++c) {
        int w = c == 0 ? kWidth : kWidth / 2;
        int h = c == 0 ? kHeight : kHeight / 2;
        const uint8_t *src =
          reinterpret_cast<const uint8_t*>(out_pic->planes[c]);
        for (int y = 0; y < h; ++y) {
          raw_yuv.write(reinterpret_cast<const char*>(
                          src + static_cast<size_t>(y) * out_pic->stride[c]),
                        w);
        }
      }
      ++decoded_frames;
    }
    if (rc != XVC_DEC_NO_DECODED_PIC) {
      std::cerr << "decoder drain error " << rc << "\n";
      ok = false;
    }
  };

  auto take_nals = [&](xvc_enc_nal_unit *nals, int count) {
    for (int i = 0; i < count; ++i) {
      if (!nals[i].bytes || nals[i].size == 0) {
        std::cerr << "empty nal unit\n";
        ok = false;
        return;
      }
      WriteBe32(bitstream, static_cast<uint32_t>(nals[i].size));
      bitstream.write(reinterpret_cast<const char*>(nals[i].bytes),
                      nals[i].size);
      if (dec_api->decoder_decode_nal(decoder, nals[i].bytes, nals[i].size,
                                      nals[i].user_data) != XVC_DEC_OK) {
        std::cerr << "decode_nal failed\n";
        ok = false;
        return;
      }
      drain();
    }
  };

  const size_t frame_bytes =
    static_cast<size_t>(kWidth) * kHeight * 3 / 2;
  std::vector<uint8_t> frame(frame_bytes);

  for (int f = 0; f < kFrames && ok; ++f) {
    // Deterministic mixed content: structured gradient plus fine texture.
    for (size_t k = 0; k < frame.size(); ++k) {
      frame[k] = static_cast<uint8_t>(
        (k * 13 + (k / kWidth) * 17 + f * 23 + (k % 19) * f) & 0xff);
    }
    xvc_enc_nal_unit *nals = nullptr;
    int nal_count = 0;
    if (enc_api->encoder_encode(encoder, frame.data(), &nals, &nal_count,
                                rec_pic) != XVC_ENC_OK) {
      std::cerr << "encoder_encode failed\n";
      return 1;
    }
    take_nals(nals, nal_count);
  }

  // Full encoder flush (B frame reordering) and decoder flush.
  bool flush_done = false;
  for (int i = 0; i < 64 && ok; ++i) {
    xvc_enc_nal_unit *nals = nullptr;
    int nal_count = 0;
    xvc_enc_return_code rc =
      enc_api->encoder_flush(encoder, &nals, &nal_count, rec_pic);
    if (rc != XVC_ENC_OK && rc != XVC_ENC_NO_MORE_OUTPUT) {
      std::cerr << "encoder_flush failed\n";
      return 1;
    }
    take_nals(nals, nal_count);
    if (rc == XVC_ENC_NO_MORE_OUTPUT) {
      flush_done = true;
      break;
    }
  }
  if (!flush_done) {
    std::cerr << "encoder did not flush completely\n";
    return 1;
  }
  if (dec_api->decoder_flush(decoder) != XVC_DEC_OK) {
    std::cerr << "decoder_flush failed\n";
    return 1;
  }
  drain();

  int corrupted = 0;
  if (dec_api->decoder_check_conformance(decoder, &corrupted) != XVC_DEC_OK ||
      corrupted != 0) {
    std::cerr << "conformance check failed\n";
    return 1;
  }
  if (!ok || decoded_frames != kFrames) {
    std::cerr << "decoded frame count " << decoded_frames << "\n";
    return 1;
  }
  bitstream.close();
  raw_yuv.close();
  if (!bitstream || !raw_yuv) {
    std::cerr << "failed writing output files\n";
    return 1;
  }

  dec_api->picture_destroy(out_pic);
  enc_api->picture_destroy(rec_pic);
  dec_api->decoder_destroy(decoder);
  enc_api->encoder_destroy(encoder);
  dec_api->parameters_destroy(dec_param);
  enc_api->parameters_destroy(param);

  std::cout << "ROW_RESTART_EXAMPLE frames=" << kFrames << "\n";
  return 0;
}
