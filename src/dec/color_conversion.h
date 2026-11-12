// Copyright 2026 Google Inc. All Rights Reserved.
//
// Use of this source code is governed by a BSD-style license
// that can be found in the COPYING file in the root of the source
// tree. An additional intellectual property rights grant can be found
// in the file PATENTS. All contributing project authors may
// be found in the AUTHORS file in the root of the source tree.
// -----------------------------------------------------------------------------
//
// Explicit Y'CbCr matrix / range selection for lossy VP8 -> RGB conversion.
//
// The samples in the WebP bitstream carry no usable color metadata: they are
// just Y'CbCr values. The decoder's historical behavior is a BT.601 limited
// conversion. When the caller knows out-of-band that the file was produced
// with another matrix or range (for instance a pipeline that explicitly
// signalled BT.709), it can ask for the corresponding interpretation through
// WebPDecoderOptions::yuv_conversion. This only changes how the existing
// samples are interpreted; no bitstream metadata is read, written or guessed.

#ifndef WEBP_DEC_COLOR_CONVERSION_H_
#define WEBP_DEC_COLOR_CONVERSION_H_

#include "src/dsp/dsp.h"
#include "src/webp/decode.h"
#include "src/webp/types.h"

WEBP_ASSUME_UNSAFE_INDEXABLE_ABI

#ifdef __cplusplus
extern "C" {
#endif

// Converts one triple of 8-bit Y'CbCr samples into a packed 4-byte pixel.
// 'dst' receives the channel order of the target colorspace, with alpha set
// to 0xff; alpha, if any, is dispatched separately by the caller.
typedef void (*WebPColorPixelFunc)(uint8_t y, uint8_t u, uint8_t v,
                                   uint8_t* dst);

// Line-pair upsampler with a pluggable per-pixel conversion. It performs
// exactly the same chroma interpolation (phase, edge extension and integer
// rounding) as the regular fancy upsampler.
typedef void (*WebPColorLinePairFunc)(
    const uint8_t* WEBP_RESTRICT top_y,
    const uint8_t* WEBP_RESTRICT bottom_y,
    const uint8_t* WEBP_RESTRICT top_u, const uint8_t* WEBP_RESTRICT top_v,
    const uint8_t* WEBP_RESTRICT cur_u, const uint8_t* WEBP_RESTRICT cur_v,
    uint8_t* WEBP_RESTRICT top_dst, uint8_t* WEBP_RESTRICT bottom_dst, int len,
    WebPColorPixelFunc convert);

// Returns true if 'mode' is one of the four packed RGB(A) modes that support
// an explicit non-zero conversion (MODE_RGBA, MODE_BGRA, MODE_rgbA,
// MODE_bgrA).
int WebPIsSupportedColorConversionOutput(WEBP_CSP_MODE mode);

// Validates 'options->yuv_conversion' for the given bitstream and output mode.
// Out-of-range values are always rejected. A non-zero value is only rejected
// for lossy VP8 output to an unsupported packed RGB layout; YUV/YUVA output
// and lossless VP8L keep their historical behavior.
VP8StatusCode WebPValidateColorConversionOptions(
    const WebPDecoderOptions* options, int is_lossless,
    WEBP_CSP_MODE output_mode);

// Per-row point sampler for the requested conversion (conv in [1..5]).
// Returns NULL if the combination is unsupported.
WebPSamplerRowFunc WebPGetColorSamplerRow(WEBP_CSP_MODE mode, int conv);

// YUV444 -> RGB converter for the requested conversion (conv in [1..5]).
// Returns NULL if the combination is unsupported.
WebPYUV444Converter WebPGetColorYuv444Converter(WEBP_CSP_MODE mode, int conv);

// Pixel converter for the requested conversion (conv in [1..5]).
WebPColorPixelFunc WebPGetColorPixelFunc(WEBP_CSP_MODE mode, int conv);

// Fancy line-pair upsampler for 'mode', or NULL if unsupported.
WebPColorLinePairFunc WebPGetColorLinePair(WEBP_CSP_MODE mode);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // WEBP_DEC_COLOR_CONVERSION_H_
