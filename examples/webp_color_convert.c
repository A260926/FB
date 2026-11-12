// Copyright 2026 Google Inc. All Rights Reserved.
//
// Use of this source code is governed by a BSD-style license
// that can be found in the COPYING file in the root of the source
// tree. An additional intellectual property rights grant can be found
// in the file PATENTS. All contributing project authors may
// be found in the AUTHORS file in the root of the source tree.
// -----------------------------------------------------------------------------
//
// Example: decode a lossy WebP with an explicitly selected Y'CbCr matrix and
// range, for cases where the producer of the file signalled those parameters
// out of band (the file itself carries no usable color metadata).
//
// Usage: webp_color_convert <in.webp> <out.pgm> [conversion]
//   0: BT.601 limited (decoder default)   1: BT.709 limited
//   2: BT.2020 limited                    3: BT.601 full
//   4: BT.709 full                        5: BT.2020 full
// Writes a binary PGM (grayscale) preview of the red channel, or prints the
// first few pixels when no output path is given.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "webp/decode.h"

static int ReadFile(const char* const path, uint8_t** const data_out,
                    size_t* const size_out) {
  FILE* const f = fopen(path, "rb");
  long size;
  uint8_t* data;
  if (f == NULL) return 0;
  if (fseek(f, 0, SEEK_END) != 0) {
    fclose(f);
    return 0;
  }
  size = ftell(f);
  if (size < 0 || fseek(f, 0, SEEK_SET) != 0) {
    fclose(f);
    return 0;
  }
  data = (uint8_t*)malloc((size_t)size);
  if (data != NULL && fread(data, 1, (size_t)size, f) != (size_t)size) {
    free(data);
    data = NULL;
  }
  fclose(f);
  if (data == NULL) return 0;
  *data_out = data;
  *size_out = (size_t)size;
  return 1;
}

static int WritePgm(const char* const path, const uint8_t* const rgba,
                    int width, int height, int stride) {
  FILE* const f = fopen(path, "wb");
  int y;
  if (f == NULL) return 0;
  fprintf(f, "P5\n%d %d\n255\n", width, height);
  for (y = 0; y < height; ++y) {
    int x;
    for (x = 0; x < width; ++x) {
      // Binary PGM stores one sample; use the red channel as a preview.
      fputc(rgba[(size_t)y * stride + 4 * x + 0], f);
    }
  }
  fclose(f);
  return 1;
}

int main(int argc, char* argv[]) {
  uint8_t* data = NULL;
  size_t data_size = 0;
  int width = 0, height = 0;
  int conv = 0;
  WebPDecoderConfig config;
  VP8StatusCode status;
  const char* out_path = NULL;

  if (argc < 2 || argc > 4) {
    fprintf(stderr,
            "Usage: %s <in.webp> [out.pgm] [conversion 0..5]\n", argv[0]);
    return 1;
  }
  if (argc == 3) out_path = argv[2];
  if (argc == 4) {
    out_path = argv[2];
    conv = atoi(argv[3]);
  }
  if (!ReadFile(argv[1], &data, &data_size)) {
    fprintf(stderr, "Cannot read %s\n", argv[1]);
    return 1;
  }
  if (!WebPInitDecoderConfig(&config)) {
    fprintf(stderr, "WebPInitDecoderConfig failed\n");
    free(data);
    return 1;
  }

  // Select the interpretation of the Y'CbCr samples signalled out of band.
  // 0 keeps the decoder's historical BT.601 limited conversion.
  config.options.yuv_conversion = conv;
  // Only these four packed layouts support a non-zero conversion.
  config.output.colorspace = MODE_RGBA;

  status = WebPDecode(data, data_size, &config);
  if (status != VP8_STATUS_OK) {
    fprintf(stderr, "WebPDecode failed: status %d\n", (int)status);
    free(data);
    return 1;
  }
  width = config.output.width;
  height = config.output.height;
  {
    const WebPRGBABuffer* const rgba = &config.output.u.RGBA;
    printf("decoded %dx%d, conversion=%d (see config.options.yuv_conversion)\n",
           width, height, conv);
    {
      int i;
      for (i = 0; i < 4 && i < width; ++i) {
        const uint8_t* const px = rgba->rgba + 4 * i;
        printf("  pixel %d: R=%u G=%u B=%u A=%u\n", i, px[0], px[1], px[2],
               px[3]);
      }
    }
    if (out_path != NULL) {
      if (!WritePgm(out_path, rgba->rgba, width, height, rgba->stride)) {
        fprintf(stderr, "Cannot write %s\n", out_path);
        WebPFreeDecBuffer(&config.output);
        free(data);
        return 1;
      }
      printf("wrote %s\n", out_path);
    }
  }

  WebPFreeDecBuffer(&config.output);
  free(data);
  return 0;
}
