// Color conversion contract for WebPDecoderOptions::yuv_conversion.
//
// This file intentionally has no main(): the fixed driver declared in
// support/target_main.c calls the three group functions and records every
// comparison through unit_check(). Reference values are computed
// independently from the unscaled decoded Y'CbCr planes; the decoder's own
// conversion entry points are never used to produce an expectation.
//
//   * matrix:    point-sampled Y'CbCr -> double-precision matrix -> round/clip.
//   * fancy:     independent re-implementation of the documented chroma
//                interpolation (phase, edge extension, integer rounding),
//                then the double-precision matrix.
//   * scaling:   the original rescaler primitives per plane (U/V go directly
//                from chroma size to the full target size), then the double
//                precision matrix. Alpha is rescaled separately and only
//                premultiplied for premultiplied modes.

#include <math.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "src/utils/rescaler_utils.h"
#include "src/webp/decode.h"
#include "src/webp/encode.h"

//------------------------------------------------------------------------------
// Generic helpers

static int g_sink;  // keeps the compiler from removing the comparisons

static int ExpectEq(int actual, int expected, const char *label) {
  char buf[192];
  (void)snprintf(buf, sizeof(buf), "%s (got %d, want %d)", label, actual,
                 expected);
  unit_check(actual == expected, buf);
  g_sink += actual;
  return actual == expected;
}

static int ExpectTrue(int cond, const char *label) {
  unit_check(cond, label);
  g_sink += cond;
  return cond;
}

static uint8_t Clamp8(int v) {
  return (uint8_t)((v < 0) ? 0 : (v > 255) ? 255 : v);
}

static int Bpp(WEBP_CSP_MODE mode) {
  return (mode == MODE_RGBA || mode == MODE_BGRA || mode == MODE_rgbA ||
          mode == MODE_bgrA)
             ? 4
             : 0;
}

static int IsBgra(WEBP_CSP_MODE mode) {
  return mode == MODE_BGRA || mode == MODE_bgrA;
}

static int Premultiplied(WEBP_CSP_MODE mode) {
  return mode == MODE_rgbA || mode == MODE_bgrA;
}

// Reads R,G,B,A in a layout-independent way.
static void GetPixel(const uint8_t *p, WEBP_CSP_MODE mode, int *r, int *g,
                     int *b, int *a) {
  if (IsBgra(mode)) {
    *b = p[0];
    *g = p[1];
    *r = p[2];
  } else {
    *r = p[0];
    *g = p[1];
    *b = p[2];
  }
  *a = p[3];
}

// Writes an RGBA-ordered pixel (the reference buffers always use RGBA order).
static void PutRgba(uint8_t *p, int r, int g, int b, int a) {
  p[0] = (uint8_t)r;
  p[1] = (uint8_t)g;
  p[2] = (uint8_t)b;
  p[3] = (uint8_t)a;
}

// The existing integer premultiply rule: (int)(x * a / 255.).
static int Premult(int x, int a) {
  if (a == 255) return x;
  return (int)(((uint32_t)x * (uint32_t)a * 32897u) >> 23);
}

//------------------------------------------------------------------------------
// Independent double-precision reference conversion

typedef struct {
  double kr, kb, kg;
} Matrix;

// Indexed by yuv_conversion; entry 0 (legacy) is never used here.
static const Matrix kMatrices[6] = {
    {-1.0, -1.0, -1.0},
    {0.2126, 0.0722, 0.7152},  // 1: BT.709 limited
    {0.2627, 0.0593, 0.6780},  // 2: BT.2020 limited
    {0.299, 0.114, 0.587},     // 3: BT.601 full
    {0.2126, 0.0722, 0.7152},  // 4: BT.709 full
    {0.2627, 0.0593, 0.6780},  // 5: BT.2020 full
};

static void RefConvert(int conv, int y, int u, int v, int *r, int *g, int *b) {
  const Matrix *const m = &kMatrices[conv];
  const int limited = (conv <= 2);
  double Y, cb, cr, rr, gg, bb;
  if (limited) {
    Y = 255.0 * (y - 16) / 219.0;
    cb = 255.0 * (u - 128.0) / 224.0;
    cr = 255.0 * (v - 128.0) / 224.0;
  } else {
    Y = y;
    cb = u - 128.0;
    cr = v - 128.0;
  }
  rr = Y + 2.0 * (1.0 - m->kr) * cr;
  bb = Y + 2.0 * (1.0 - m->kb) * cb;
  gg = Y - 2.0 * (1.0 - m->kr) * m->kr / m->kg * cr -
       2.0 * (1.0 - m->kb) * m->kb / m->kg * cb;
  *r = (int)Clamp8((int)floor(rr + 0.5));
  *g = (int)Clamp8((int)floor(gg + 0.5));
  *b = (int)Clamp8((int)floor(bb + 0.5));
}

//------------------------------------------------------------------------------
// Encoded fixtures

static uint32_t Le32(const uint8_t *p) {
  return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 |
         (uint32_t)p[3] << 24;
}

typedef struct {
  uint8_t *vp8;    // raw VP8 payload
  size_t vp8_size;
  uint8_t *alpha;  // raw ALPH payload, if any
  size_t alpha_size;
} Fixture;

// Splits a full WebP file into its VP8 and ALPH chunks.
static int Split(const WebPMemoryWriter *w, Fixture *f) {
  size_t pos = 12;
  f->vp8 = NULL;
  f->vp8_size = 0;
  f->alpha = NULL;
  f->alpha_size = 0;
  while (pos + 8 <= w->size) {
    const size_t n = Le32(w->mem + pos + 4);
    const uint8_t *tag = w->mem + pos;
    if (n > w->size - pos - 8) return 0;
    if (memcmp(tag, "VP8 ", 4) == 0) {
      f->vp8 = w->mem + pos + 8;
      f->vp8_size = n;
    } else if (memcmp(tag, "ALPH", 4) == 0) {
      f->alpha = w->mem + pos + 8;
      f->alpha_size = n;
    }
    pos += 8 + n + (n & 1);
  }
  return f->vp8 != NULL;
}

// Builds a lossy VP8 (YUV420 + alpha) file. variant 0 is a smooth non-uniform
// pattern; variant 1 deliberately contains limited-range black/white, values
// outside the legal range and saturated/out-of-gamut chroma.
static int MakeWebp(int width, int height, int variant,
                    WebPMemoryWriter *writer) {
  WebPConfig c;
  WebPPicture p;
  int x, y;
  if (!WebPConfigInit(&c) || !WebPPictureInit(&p)) return 0;
  c.quality = 90;
  c.method = 3;
  c.exact = 1;
  p.width = width;
  p.height = height;
  p.colorspace = WEBP_YUV420A;
  if (!WebPPictureAlloc(&p)) return 0;
  for (y = 0; y < height; ++y) {
    for (x = 0; x < width; ++x) {
      const int idx = y * p.y_stride + x;
      if (variant == 0) {
        p.y[idx] = (uint8_t)((x * 37 + y * 19 + 3) % 236 + 10);
      } else {
        p.y[idx] = (uint8_t)(((x + y) % 2) ? 235 : 16);
        if ((x + y) % 5 == 0) p.y[idx] = 250;  // beyond limited white
        if ((x * y) % 7 == 0) p.y[idx] = 4;    // below limited black
        if ((x * 3 + y) % 11 == 0) p.y[idx] = 128;
      }
      if (variant == 2) {
        // Large transparent block, to survive scaling: the left half is fully
        // transparent and the right half fully opaque.
        p.a[idx] = (uint8_t)((x < width / 2) ? 0 : 255);
      } else {
        p.a[idx] = (uint8_t)((x * 31 + y * 13 + variant * 7) % 256);
        if ((x + y) % 9 == 0) p.a[idx] = 0;  // transparent edges
        if ((x * 2 + y) % 13 == 0) p.a[idx] = 255;
      }
    }
  }
  for (y = 0; y < (height + 1) / 2; ++y) {
    for (x = 0; x < (width + 1) / 2; ++x) {
      const int idx = y * p.uv_stride + x;
      if (variant == 0) {
        p.u[idx] = (uint8_t)((x * 59 + y * 23 + 3) % 256);
        p.v[idx] = (uint8_t)((x * 29 + y * 67 + 11) % 256);
      } else {
        const int m = (x + y) % 4;
        p.u[idx] = (uint8_t)(m == 0 ? 3 : m == 1 ? 255 : (x * 61 + 90) % 256);
        p.v[idx] = (uint8_t)(m == 0 ? 252 : m == 1 ? 2 : (y * 43 + 70) % 256);
      }
    }
  }
  WebPMemoryWriterInit(writer);
  p.writer = WebPMemoryWrite;
  p.custom_ptr = writer;
  {
    const int ok = WebPEncode(&c, &p);
    WebPPictureFree(&p);
    return ok;
  }
}

// Small lossless (VP8L) file, to check the option is ignored there.
static int MakeLossless(int width, int height, WebPMemoryWriter *writer) {
  WebPConfig c;
  WebPPicture p;
  int x, y;
  if (!WebPConfigInit(&c) || !WebPPictureInit(&p)) return 0;
  c.lossless = 1;
  c.quality = 90;
  c.exact = 1;
  p.width = width;
  p.height = height;
  p.use_argb = 1;
  if (!WebPPictureAlloc(&p)) return 0;
  for (y = 0; y < height; ++y) {
    for (x = 0; x < width; ++x) {
      p.argb[y * p.argb_stride + x] =
          0xff000000u | (uint32_t)((x * 13 + y * 7) & 0xff) << 16 |
          (uint32_t)((x * 3 + y * 11) & 0xff) << 8 |
          (uint32_t)((x * 5 + y * 17) & 0xff);
    }
  }
  WebPMemoryWriterInit(writer);
  p.writer = WebPMemoryWrite;
  p.custom_ptr = writer;
  {
    const int ok = WebPEncode(&c, &p);
    WebPPictureFree(&p);
    return ok;
  }
}

//------------------------------------------------------------------------------
// Planes and decoding helpers

typedef struct {
  int width, height;
  int y_stride, uv_stride, a_stride;
  uint8_t *y, *u, *v, *a;  // 'a' is NULL when no alpha plane is present
} Planes;

static void FreePlanes(Planes *p) {
  free(p->y);
  free(p->u);
  free(p->v);
  free(p->a);
  memset(p, 0, sizeof(*p));
}

static uint8_t *CopyPlane(const uint8_t *src, int stride, int w, int h) {
  uint8_t *dst = (uint8_t *)malloc((size_t)stride * h);
  int j;
  if (dst == NULL) return NULL;
  for (j = 0; j < h; ++j) {
    memcpy(dst + (size_t)j * stride, src + (size_t)j * stride, w);
  }
  return dst;
}

static int InitConfig(WebPDecoderConfig *c, WEBP_CSP_MODE mode, int conv) {
  if (!WebPInitDecoderConfig(c)) return 0;
  c->output.colorspace = mode;
  c->options.yuv_conversion = conv;
  return 1;
}

// Unscaled Y'CbCr planes (plus alpha when 'want_alpha'). This is the start of
// every reference pipeline.
static int DecodePlanes(const uint8_t *data, size_t size, int want_alpha,
                        Planes *out) {
  WebPDecoderConfig c;
  int uv_w;
  int uv_h;
  int ok;
  memset(out, 0, sizeof(*out));
  if (!InitConfig(&c, want_alpha ? MODE_YUVA : MODE_YUV, 0)) return 0;
  if (WebPDecode(data, size, &c) != VP8_STATUS_OK) return 0;
  out->width = c.output.width;
  out->height = c.output.height;
  out->y_stride = c.output.u.YUVA.y_stride;
  out->uv_stride = c.output.u.YUVA.u_stride;
  out->a_stride = c.output.u.YUVA.a_stride;
  uv_w = (out->width + 1) / 2;
  uv_h = (out->height + 1) / 2;
  out->y = CopyPlane(c.output.u.YUVA.y, out->y_stride, out->width, out->height);
  out->u = CopyPlane(c.output.u.YUVA.u, out->uv_stride, uv_w, uv_h);
  out->v = CopyPlane(c.output.u.YUVA.v, out->uv_stride, uv_w, uv_h);
  if (want_alpha && c.output.u.YUVA.a != NULL) {
    out->a = CopyPlane(c.output.u.YUVA.a, out->a_stride, out->width,
                       out->height);
  }
  ok = (out->y != NULL && out->u != NULL && out->v != NULL &&
        (!want_alpha || out->a != NULL));
  if (!ok) FreePlanes(out);
  WebPFreeDecBuffer(&c.output);
  return ok;
}

typedef struct {
  WEBP_CSP_MODE mode;
  int conv;
  int no_fancy;
  int use_cropping;
  int crop_left, crop_top, crop_width, crop_height;
  int use_scaling;
  int scaled_width, scaled_height;
  int flip;
} DecodeSpec;

// Full decode with the given options; returns a heap copy of the pixels.
static int DecodeFull(const uint8_t *data, size_t size, const DecodeSpec *s,
                      int *w, int *h, uint8_t **pixels, int *stride) {
  WebPDecoderConfig c;
  *pixels = NULL;
  if (!InitConfig(&c, s->mode, s->conv)) return 0;
  c.options.no_fancy_upsampling = s->no_fancy;
  c.options.use_cropping = s->use_cropping;
  c.options.crop_left = s->crop_left;
  c.options.crop_top = s->crop_top;
  c.options.crop_width = s->crop_width;
  c.options.crop_height = s->crop_height;
  c.options.use_scaling = s->use_scaling;
  c.options.scaled_width = s->scaled_width;
  c.options.scaled_height = s->scaled_height;
  c.options.flip = s->flip;
  if (WebPDecode(data, size, &c) != VP8_STATUS_OK) return 0;
  *w = c.output.width;
  *h = c.output.height;
  *stride = c.output.u.RGBA.stride;
  *pixels = (uint8_t *)malloc((size_t)*stride * *h);
  if (*pixels == NULL) {
    WebPFreeDecBuffer(&c.output);
    return 0;
  }
  memcpy(*pixels, c.output.u.RGBA.rgba, (size_t)*stride * *h);
  WebPFreeDecBuffer(&c.output);
  return 1;
}

//------------------------------------------------------------------------------
// Fancy upsampling reference.
//
// Same interpolation as the decoder's upsampler, written independently:
//   * output row 0 mirrors chroma row 0;
//   * pair (2k+1, 2k+2) uses chroma rows k and k+1;
//   * a lone last odd row (h even) mirrors chroma row h/2 - 1.
// This mirrors the documented phase and edge extension, with the same integer
// rounding of U and V, and only then applies the double-precision matrix.

static void RefStore(uint8_t *dst, int y, uint32_t uv, int conv) {
  int r, g, b;
  RefConvert(conv, y, (int)(uv & 0xff), (int)(uv >> 16), &r, &g, &b);
  PutRgba(dst, r, g, b, 255);
}

// Processes the pair (top, bottom) where top is row 2k+1 and bottom 2k+2.
static void RefPair(const uint8_t *top_y, const uint8_t *bottom_y,
                    const uint8_t *top_u, const uint8_t *top_v,
                    const uint8_t *cur_u, const uint8_t *cur_v,
                    uint8_t *top_dst, uint8_t *bottom_dst, int len, int conv) {
  const int last_pixel_pair = (len - 1) >> 1;
  uint32_t tl_uv = (uint32_t)top_u[0] | ((uint32_t)top_v[0] << 16);
  uint32_t l_uv = (uint32_t)cur_u[0] | ((uint32_t)cur_v[0] << 16);
  int x;
  {
    const uint32_t uv0 = (3 * tl_uv + l_uv + 0x00020002u) >> 2;
    RefStore(top_dst, top_y[0], uv0, conv);
  }
  if (bottom_y != NULL) {
    const uint32_t uv0 = (3 * l_uv + tl_uv + 0x00020002u) >> 2;
    RefStore(bottom_dst, bottom_y[0], uv0, conv);
  }
  for (x = 1; x <= last_pixel_pair; ++x) {
    const uint32_t t_uv = (uint32_t)top_u[x] | ((uint32_t)top_v[x] << 16);
    const uint32_t uv = (uint32_t)cur_u[x] | ((uint32_t)cur_v[x] << 16);
    const uint32_t avg = tl_uv + t_uv + l_uv + uv + 0x00080008u;
    const uint32_t diag_12 = (avg + 2 * (t_uv + l_uv)) >> 3;
    const uint32_t diag_03 = (avg + 2 * (tl_uv + uv)) >> 3;
    {
      const uint32_t uv0 = (diag_12 + tl_uv) >> 1;
      const uint32_t uv1 = (diag_03 + t_uv) >> 1;
      RefStore(top_dst + (2 * x - 1) * 4, top_y[2 * x - 1], uv0, conv);
      RefStore(top_dst + (2 * x - 0) * 4, top_y[2 * x - 0], uv1, conv);
    }
    if (bottom_y != NULL) {
      const uint32_t uv0 = (diag_03 + l_uv) >> 1;
      const uint32_t uv1 = (diag_12 + uv) >> 1;
      RefStore(bottom_dst + (2 * x - 1) * 4, bottom_y[2 * x - 1], uv0, conv);
      RefStore(bottom_dst + (2 * x + 0) * 4, bottom_y[2 * x + 0], uv1, conv);
    }
    tl_uv = t_uv;
    l_uv = uv;
  }
  if (!(len & 1)) {
    const uint32_t uv0 = (3 * tl_uv + l_uv + 0x00020002u) >> 2;
    RefStore(top_dst + (len - 1) * 4, top_y[len - 1], uv0, conv);
    if (bottom_y != NULL) {
      const uint32_t uv1 = (3 * l_uv + tl_uv + 0x00020002u) >> 2;
      RefStore(bottom_dst + (len - 1) * 4, bottom_y[len - 1], uv1, conv);
    }
  }
}

// Full fancy reference over a cropped (even-aligned) region; 'ref' is RGBA.
static int RefFancy(const Planes *p, int x0, int y0, int w, int h, int conv,
                    int check_alpha, int premultiply, uint8_t *ref,
                    int ref_stride) {
  const uint8_t *y = p->y + (size_t)y0 * p->y_stride + x0;
  const uint8_t *u = p->u + (size_t)(y0 >> 1) * p->uv_stride + (x0 >> 1);
  const uint8_t *v = p->v + (size_t)(y0 >> 1) * p->uv_stride + (x0 >> 1);
  int row;
  if (h <= 0 || w <= 0) return 0;
  // Row 0 is the decoder's first-line special case: the chroma of row 0 is
  // mirrored on top, so it is processed as the top row of the pair (0, 1).
  // When the very last row is odd, the decoder emits it later as the bottom
  // row of that same pair, which is why the reference builds it here too.
  RefPair(y, (h > 1) ? y + p->y_stride : NULL, u, v, u, v, ref,
          (h > 1) ? ref + ref_stride : NULL, w, conv);
  // Remaining pairs: top row r-1 and bottom row r (r = 2, 4, ...), using
  // chroma rows r/2 - 1 and r/2.
  for (row = 2; row < h; row += 2) {
    const int k = row >> 1;
    RefPair(y + (size_t)(row - 1) * p->y_stride,
            y + (size_t)row * p->y_stride, u + (size_t)(k - 1) * p->uv_stride,
            v + (size_t)(k - 1) * p->uv_stride, u + (size_t)k * p->uv_stride,
            v + (size_t)k * p->uv_stride,
            ref + (size_t)(row - 1) * ref_stride,
            ref + (size_t)row * ref_stride, w, conv);
  }
  // Even-height pictures have one last odd row (h - 1) that the decoder emits
  // by mirroring the last chroma row.
  if (!(h & 1)) {
    const int k = h / 2 - 1;
    RefPair(y + (size_t)(h - 1) * p->y_stride, NULL,
            u + (size_t)k * p->uv_stride, v + (size_t)k * p->uv_stride,
            u + (size_t)k * p->uv_stride, v + (size_t)k * p->uv_stride,
            ref + (size_t)(h - 1) * ref_stride, NULL, w, conv);
  }
  if (check_alpha && p->a != NULL) {
    int yy, xx;
    for (yy = 0; yy < h; ++yy) {
      for (xx = 0; xx < w; ++xx) {
        const int a = p->a[(size_t)(y0 + yy) * p->a_stride + x0 + xx];
        uint8_t *const dst = ref + (size_t)yy * ref_stride + (size_t)xx * 4;
        if (premultiply) {
          dst[0] = (uint8_t)Premult(dst[0], a);
          dst[1] = (uint8_t)Premult(dst[1], a);
          dst[2] = (uint8_t)Premult(dst[2], a);
        }
        dst[3] = (uint8_t)a;
      }
    }
  }
  return 1;
}

//------------------------------------------------------------------------------
// Scaling reference, using the original rescaler primitives per plane.

static int RescalePlaneRef(const uint8_t *src, int src_w, int src_h,
                           int src_stride, int dst_w, int dst_h,
                           uint8_t *dst) {
  WebPRescaler scaler;
  const uint64_t work_size =
      WebPRescalerWorkSize(src_w, src_h, dst_w, dst_h, 1);
  rescaler_t *const work = (rescaler_t *)malloc((size_t)work_size);
  int j = 0;
  int ok;
  if (work == NULL) return 0;
  if (!WebPRescalerInit(&scaler, src_w, src_h, dst, dst_w, dst_h, dst_w, 1,
                        work)) {
    free(work);
    return 0;
  }
  while (j < src_h) {
    j += WebPRescalerImport(&scaler, src_h - j,
                            src + (size_t)j * src_stride, src_stride);
    (void)WebPRescalerExport(&scaler);
  }
  (void)WebPRescalerExport(&scaler);
  ok = (scaler.dst_y == dst_h);
  free(work);
  return ok;
}

// Builds the expected RGBA image for a scaling spec out of the unscaled
// planes. Y/U/V are each rescaled independently, U/V going from chroma size
// straight to the full target size; alpha is rescaled on its own and applied
// only for premultiplied modes.
static int RefScaled(const Planes *p, const DecodeSpec *s, uint8_t *out,
                     int out_stride) {
  const int x0 = s->crop_left & ~1;
  const int y0 = s->crop_top & ~1;
  const int cw = s->use_cropping ? s->crop_width : p->width;
  const int ch = s->use_cropping ? s->crop_height : p->height;
  const int out_w = s->scaled_width;
  const int out_h = s->scaled_height;
  const int uv_in_w = (cw + 1) >> 1;
  const int uv_in_h = (ch + 1) >> 1;
  const int premultiply = Premultiplied(s->mode);
  uint8_t *ys = (uint8_t *)malloc((size_t)out_w * out_h);
  uint8_t *us = (uint8_t *)malloc((size_t)out_w * out_h);
  uint8_t *vs = (uint8_t *)malloc((size_t)out_w * out_h);
  uint8_t *as = (uint8_t *)malloc((size_t)out_w * out_h);
  int yy, xx, ok;
  if (!ys || !us || !vs || !as) {
    free(ys);
    free(us);
    free(vs);
    free(as);
    return 0;
  }
  ok = RescalePlaneRef(p->y + (size_t)y0 * p->y_stride + x0, cw, ch,
                       p->y_stride, out_w, out_h, ys) &&
       RescalePlaneRef(p->u + (size_t)(y0 >> 1) * p->uv_stride + (x0 >> 1),
                       uv_in_w, uv_in_h, p->uv_stride, out_w, out_h, us) &&
       RescalePlaneRef(p->v + (size_t)(y0 >> 1) * p->uv_stride + (x0 >> 1),
                       uv_in_w, uv_in_h, p->uv_stride, out_w, out_h, vs);
  if (ok && p->a != NULL) {
    ok = RescalePlaneRef(p->a + (size_t)y0 * p->a_stride + x0, cw, ch,
                         p->a_stride, out_w, out_h, as);
  }
  if (ok) {
    for (yy = 0; yy < out_h; ++yy) {
      for (xx = 0; xx < out_w; ++xx) {
        const int i = yy * out_w + xx;
        int r, g, b, a = p->a != NULL ? as[i] : 255;
        RefConvert(s->conv, ys[i], us[i], vs[i], &r, &g, &b);
        if (premultiply) {
          r = Premult(r, a);
          g = Premult(g, a);
          b = Premult(b, a);
        }
        PutRgba(out + (size_t)yy * out_stride + (size_t)xx * 4, r, g, b, a);
      }
    }
  }
  free(ys);
  free(us);
  free(vs);
  free(as);
  return ok;
}

//------------------------------------------------------------------------------
// Comparison helpers

// Compares actual pixels (in 'mode' layout) against an RGBA reference.
static int CompareImages(const uint8_t *actual, int actual_stride,
                         WEBP_CSP_MODE mode, const uint8_t *ref,
                         int ref_stride, int w, int h, int tol,
                         int check_alpha, int *max_err) {
  int y, x, ok = 1, local_max = 0;
  for (y = 0; y < h; ++y) {
    for (x = 0; x < w; ++x) {
      const uint8_t *a = actual + (size_t)y * actual_stride + (size_t)x * 4;
      const uint8_t *e = ref + (size_t)y * ref_stride + (size_t)x * 4;
      int ar, ag, ab, aa;
      GetPixel(a, mode, &ar, &ag, &ab, &aa);
      {
        const int dr = abs(ar - e[0]), dg = abs(ag - e[1]), db = abs(ab - e[2]);
        if (dr > local_max) local_max = dr;
        if (dg > local_max) local_max = dg;
        if (db > local_max) local_max = db;
        if (dr > tol || dg > tol || db > tol) ok = 0;
      }
      if (check_alpha && aa != e[3]) ok = 0;
    }
  }
  if (max_err != NULL) *max_err = local_max;
  return ok;
}

// Decodes with the given spec and checks against an RGBA reference.
static void CheckSpec(const uint8_t *data, size_t size, const DecodeSpec *s,
                      const uint8_t *ref, int ref_stride, int ref_w, int ref_h,
                      int check_alpha, const char *label) {
  uint8_t *pixels = NULL;
  int w = 0, h = 0, stride = 0;
  if (!DecodeFull(data, size, s, &w, &h, &pixels, &stride)) {
    char buf[192];
    (void)snprintf(buf, sizeof(buf), "%s: decode failed", label);
    unit_check(0, buf);
    return;
  }
  {
    char buf[192];
    (void)snprintf(buf, sizeof(buf), "%s: dimensions", label);
    ExpectTrue(w == ref_w && h == ref_h, buf);
    if (w == ref_w && h == ref_h) {
      int max_err = 0;
      (void)snprintf(buf, sizeof(buf), "%s: pixels", label);
      {
        const int cok =
            CompareImages(pixels, stride, s->mode, ref, ref_stride, w, h, 2,
                          check_alpha, &max_err);
        if (!cok) {
          fprintf(stderr, "%s: max channel error %d\n", label, max_err);
        }
        ExpectTrue(cok, buf);
      }
      g_sink += max_err;
    }
  }
  free(pixels);
}

//------------------------------------------------------------------------------
// test_matrix: numeric contract, supported layouts, rejection behavior.

void test_matrix(void) {
  WebPMemoryWriter w;
  Planes p;
  const WEBP_CSP_MODE modes[4] = {MODE_RGBA, MODE_BGRA, MODE_rgbA, MODE_bgrA};
  int conv, m;

  if (!MakeWebp(29, 21, 1, &w) || !DecodePlanes(w.mem, w.size, 1, &p)) {
    unit_check(0, "test_matrix: fixture/planes");
    return;
  }

  // 1) Every matrix and supported layout, point-sampled so the chroma sample
  //    is unambiguous, compared to the double-precision reference.
  for (conv = 1; conv <= 5; ++conv) {
    for (m = 0; m < 4; ++m) {
      DecodeSpec s;
      uint8_t *pixels = NULL;
      int dw = 0, dh = 0, stride = 0, y, x, max_err = 0;
      char label[192];
      memset(&s, 0, sizeof(s));
      s.mode = modes[m];
      s.conv = conv;
      s.no_fancy = 1;
      if (!DecodeFull(w.mem, w.size, &s, &dw, &dh, &pixels, &stride)) {
        unit_check(0, "test_matrix: decode with conversion");
        continue;
      }
      (void)snprintf(label, sizeof(label),
                     "test_matrix dims conv=%d mode=%d", conv, (int)s.mode);
      ExpectEq(dw, p.width, label);
      ExpectEq(dh, p.height, label);
      for (y = 0; y < dh; ++y) {
        for (x = 0; x < dw; ++x) {
          const uint8_t *a = pixels + (size_t)y * stride + (size_t)x * 4;
          int ar, ag, ab, aa, er, eg, eb;
          RefConvert(conv, p.y[(size_t)y * p.y_stride + x],
                     p.u[(size_t)(y >> 1) * p.uv_stride + (x >> 1)],
                     p.v[(size_t)(y >> 1) * p.uv_stride + (x >> 1)], &er, &eg,
                     &eb);
          GetPixel(a, s.mode, &ar, &ag, &ab, &aa);
          if (Premultiplied(s.mode)) {
            // The decoder premultiplies after conversion for the *A modes.
            er = Premult(er, p.a[(size_t)y * p.a_stride + x]);
            eg = Premult(eg, p.a[(size_t)y * p.a_stride + x]);
            eb = Premult(eb, p.a[(size_t)y * p.a_stride + x]);
          }
          if (abs(ar - er) > max_err) max_err = abs(ar - er);
          if (abs(ag - eg) > max_err) max_err = abs(ag - eg);
          if (abs(ab - eb) > max_err) max_err = abs(ab - eb);
          if (aa != p.a[(size_t)y * p.a_stride + x]) max_err = 100;
          g_sink += aa;
        }
      }
      (void)snprintf(label, sizeof(label),
                     "test_matrix tolerance conv=%d mode=%d", conv,
                     (int)s.mode);
      ExpectTrue(max_err <= 2, label);
      free(pixels);
    }
  }

  // 2) limited/full and different matrices must really differ on this
  //    saturated fixture.
  {
    uint8_t *a = NULL, *b = NULL;
    int aw = 0, ah = 0, as = 0, bw = 0, bh = 0, bs = 0;
    DecodeSpec s;
    int differ = 0, y, x, n = 0;
    memset(&s, 0, sizeof(s));
    s.mode = MODE_RGBA;
    s.conv = 1;
    s.no_fancy = 1;
    ExpectTrue(DecodeFull(w.mem, w.size, &s, &aw, &ah, &a, &as),
               "test_matrix: limited decode");
    s.conv = 3;
    ExpectTrue(DecodeFull(w.mem, w.size, &s, &bw, &bh, &b, &bs),
               "test_matrix: full decode");
    ExpectEq(aw, bw, "test_matrix: same width");
    ExpectEq(ah, bh, "test_matrix: same height");
    if (a != NULL && b != NULL) {
      for (y = 0; y < ah; ++y) {
        for (x = 0; x < aw; ++x) {
          const uint8_t *pa = a + (size_t)y * as + (size_t)x * 4;
          const uint8_t *pb = b + (size_t)y * bs + (size_t)x * 4;
          ++n;
          if (pa[0] != pb[0] || pa[1] != pb[1] || pa[2] != pb[2]) ++differ;
        }
      }
    }
    ExpectTrue(differ > n / 3,
               "test_matrix: limited and full differ on saturated fixture");
    free(a);
    free(b);
  }

  // 3) Black/white and out-of-range chroma: clipping happens after the
  //    conversion, never before it.
  {
    int r, g, b;
    RefConvert(1, 16, 128, 128, &r, &g, &b);  // limited black
    unit_check(r == 0 && g == 0 && b == 0, "test_matrix: limited black");
    RefConvert(3, 0, 128, 128, &r, &g, &b);  // full black
    unit_check(r == 0 && g == 0 && b == 0, "test_matrix: full black");
    RefConvert(1, 235, 128, 128, &r, &g, &b);  // limited white
    unit_check(r == 255 && g == 255 && b == 255, "test_matrix: limited white");
    RefConvert(3, 255, 128, 128, &r, &g, &b);  // full white
    unit_check(r == 255 && g == 255 && b == 255, "test_matrix: full white");
    RefConvert(3, 128, 255, 255, &r, &g, &b);  // saturated full red/blue
    unit_check(r == 255 && b == 255, "test_matrix: saturated full red/blue");
    // Limited-range black with out-of-range chroma (709). If the chroma were
    // clipped to the legal [16..240] range before conversion, the result would
    // be a dull gray; converting first and clipping per channel gives these
    // saturated values.
    RefConvert(1, 16, 3, 252, &r, &g, &b);
    unit_check(r == 222 && g == 0 && b == 0,
               "test_matrix: out-of-range chroma, positive Cr");
    RefConvert(1, 235, 252, 3, &r, &g, &b);
    unit_check(r == 31 && g == 255 && b == 255,
               "test_matrix: out-of-range chroma, negative Cr");
  }

  // 4) conv == 0 keeps the historical bytes, also via the simple one-shot API.
  {
    DecodeSpec s;
    uint8_t *legacy = NULL;
    int lw = 0, lh = 0, ls = 0;
    int sw = 0, sh = 0;
    uint8_t *simple;
    memset(&s, 0, sizeof(s));
    s.mode = MODE_RGBA;
    s.conv = 0;
    ExpectTrue(DecodeFull(w.mem, w.size, &s, &lw, &lh, &legacy, &ls),
               "test_matrix: conv 0 decode");
    simple = WebPDecodeRGBA(w.mem, w.size, &sw, &sh);
    ExpectTrue(simple != NULL, "test_matrix: WebPDecodeRGBA");
    if (simple != NULL && legacy != NULL) {
      int identical = (sw == lw && sh == lh);
      int y;
      for (y = 0; identical && y < lh; ++y) {
        if (memcmp(legacy + (size_t)y * ls, simple + (size_t)y * sw * 4,
                   (size_t)sw * 4) != 0) {
          identical = 0;
        }
      }
      ExpectTrue(identical, "test_matrix: conv 0 keeps legacy bytes");
      WebPFree(simple);
    }
    free(legacy);
  }

  // 5) Rejection rules, before any output pixel (or padding) is written.
  {
    WebPDecoderConfig c;
    uint8_t guard[64 * 64 * 4 + 64];
    const int bad_conv[2] = {6, -1};
    int i;
    for (i = 0; i < 2; ++i) {
      memset(guard, 0xa5, sizeof(guard));
      if (!InitConfig(&c, MODE_RGBA, bad_conv[i])) continue;
      c.output.is_external_memory = 1;
      c.output.u.RGBA.rgba = guard;
      c.output.u.RGBA.stride = 64 * 4;
      c.output.u.RGBA.size = 64 * 64 * 4;
      ExpectEq(WebPDecode(w.mem, w.size, &c), VP8_STATUS_INVALID_PARAM,
               "test_matrix: out-of-range rejected (lossy RGBA)");
      ExpectTrue(guard[0] == 0xa5 && guard[sizeof(guard) - 1] == 0xa5,
                 "test_matrix: out-of-range wrote no pixels");
      WebPFreeDecBuffer(&c.output);
    }
    // Out-of-range is rejected for lossless input as well.
    {
      WebPMemoryWriter lw;
      if (MakeLossless(15, 11, &lw)) {
        memset(guard, 0xa5, sizeof(guard));
        if (InitConfig(&c, MODE_RGBA, 6)) {
          c.output.is_external_memory = 1;
          c.output.u.RGBA.rgba = guard;
          c.output.u.RGBA.stride = 64 * 4;
          c.output.u.RGBA.size = 64 * 64 * 4;
          ExpectEq(WebPDecode(lw.mem, lw.size, &c), VP8_STATUS_INVALID_PARAM,
                   "test_matrix: out-of-range rejected (lossless)");
          ExpectTrue(guard[0] == 0xa5,
                     "test_matrix: lossless out-of-range wrote no pixels");
          WebPFreeDecBuffer(&c.output);
        }
        WebPMemoryWriterClear(&lw);
      }
    }
    // Out-of-range is rejected for YUV output too.
    {
      memset(guard, 0xa5, sizeof(guard));
      if (InitConfig(&c, MODE_YUVA, 7)) {
        c.output.is_external_memory = 1;
        c.output.u.YUVA.y = guard;
        c.output.u.YUVA.y_stride = 64;
        c.output.u.YUVA.y_size = 64 * 64;
        c.output.u.YUVA.u = guard;
        c.output.u.YUVA.u_stride = 32;
        c.output.u.YUVA.u_size = 32 * 32;
        c.output.u.YUVA.v = guard;
        c.output.u.YUVA.v_stride = 32;
        c.output.u.YUVA.v_size = 32 * 32;
        c.output.u.YUVA.a = guard;
        c.output.u.YUVA.a_stride = 64;
        c.output.u.YUVA.a_size = 64 * 64;
        ExpectEq(WebPDecode(w.mem, w.size, &c), VP8_STATUS_INVALID_PARAM,
                 "test_matrix: out-of-range rejected (YUV)");
        ExpectTrue(guard[0] == 0xa5,
                   "test_matrix: YUV out-of-range wrote no pixels");
        WebPFreeDecBuffer(&c.output);
      }
    }
    // Unsupported packed layouts with a non-zero selection on lossy VP8.
    {
      const WEBP_CSP_MODE bad_modes[4] = {MODE_RGB, MODE_BGR, MODE_RGB_565,
                                          MODE_RGBA_4444};
      int k;
      for (k = 0; k < 4; ++k) {
        memset(guard, 0xa5, sizeof(guard));
        if (!InitConfig(&c, bad_modes[k], 1)) continue;
        c.output.is_external_memory = 1;
        c.output.u.RGBA.rgba = guard;
        c.output.u.RGBA.stride = 64 * 4;
        c.output.u.RGBA.size = 64 * 64 * 4;
        ExpectEq(WebPDecode(w.mem, w.size, &c), VP8_STATUS_INVALID_PARAM,
                 "test_matrix: unsupported layout rejected");
        ExpectTrue(guard[0] == 0xa5 && guard[sizeof(guard) - 1] == 0xa5,
                   "test_matrix: unsupported layout wrote no pixels");
        WebPFreeDecBuffer(&c.output);
      }
    }
    // WebPValidateDecoderConfig agrees with these checks.
    {
      WebPDecoderConfig v;
      if (InitConfig(&v, MODE_RGBA, 6)) {
        ExpectEq(WebPValidateDecoderConfig(&v), 0,
                 "test_matrix: validate rejects out-of-range");
      }
      if (InitConfig(&v, MODE_RGB, 2)) {
        v.input.format = 1;
        ExpectEq(WebPValidateDecoderConfig(&v), 0,
                 "test_matrix: validate rejects unsupported layout");
      }
      if (InitConfig(&v, MODE_YUVA, 2)) {
        v.input.format = 1;
        ExpectEq(WebPValidateDecoderConfig(&v), 1,
                 "test_matrix: validate accepts YUV output");
      }
      if (InitConfig(&v, MODE_RGBA, 5)) {
        v.input.format = 2;
        ExpectEq(WebPValidateDecoderConfig(&v), 1,
                 "test_matrix: validate accepts lossless");
      }
      if (InitConfig(&v, MODE_RGBA, 5)) {
        v.input.format = 1;
        ExpectEq(WebPValidateDecoderConfig(&v), 1,
                 "test_matrix: validate accepts supported layout");
      }
    }
    // Incremental decoding rejects the same configurations, before pixels:
    // out-of-range values are refused at creation, unsupported layouts for
    // lossy VP8 once the headers are known.
    {
      WebPDecoderConfig ic;
      WebPIDecoder *d1 = NULL, *d2 = NULL;
      uint8_t guarded[64 * 64 * 4];
      if (InitConfig(&ic, MODE_RGBA, 8)) {
        memset(guarded, 0xa5, sizeof(guarded));
        ic.output.is_external_memory = 1;
        ic.output.u.RGBA.rgba = guarded;
        ic.output.u.RGBA.stride = 64 * 4;
        ic.output.u.RGBA.size = 64 * 64 * 4;
        d1 = WebPIDecode(NULL, 0, &ic);
        ExpectTrue(d1 == NULL,
                   "test_matrix: incremental out-of-range at creation");
        ExpectTrue(guarded[0] == 0xa5,
                   "test_matrix: incremental rejection wrote no pixels");
        if (d1 != NULL) WebPIDelete(d1);
      }
      if (InitConfig(&ic, MODE_RGB_565, 2)) {
        size_t f = 0;
        int rejected = 0;
        memset(guarded, 0xa5, sizeof(guarded));
        ic.output.is_external_memory = 1;
        ic.output.u.RGBA.rgba = guarded;
        ic.output.u.RGBA.stride = 64 * 4;
        ic.output.u.RGBA.size = 64 * 64 * 4;
        ic.options.use_scaling = 0;
        d2 = WebPIDecode(NULL, 0, &ic);
        ExpectTrue(d2 != NULL, "test_matrix: incremental bad layout created");
        if (d2 != NULL) {
          while (f < w.size) {
            const size_t take = (w.size - f > 101) ? 101 : w.size - f;
            const VP8StatusCode st = WebPIAppend(d2, w.mem + f, take);
            f += take;
            if (st == VP8_STATUS_INVALID_PARAM) {
              rejected = 1;
              break;
            }
            if (st != VP8_STATUS_OK && st != VP8_STATUS_SUSPENDED) break;
            if (st == VP8_STATUS_OK) break;
          }
          ExpectTrue(rejected,
                     "test_matrix: incremental unsupported layout rejected");
          ExpectTrue(guarded[0] == 0xa5 && guarded[sizeof(guarded) - 1] == 0xa5,
                     "test_matrix: incremental layout rejection wrote no pixels");
          WebPIDelete(d2);
        }
      }
    }
  }

  // 6) YUV/YUVA output and lossless VP8L are unchanged by a legal selection.
  {
    WebPDecoderConfig a, b2;
    if (InitConfig(&a, MODE_YUVA, 0) && InitConfig(&b2, MODE_YUVA, 4)) {
      int y;
      ExpectTrue(WebPDecode(w.mem, w.size, &b2) == VP8_STATUS_OK,
                 "test_matrix: YUVA accepts non-zero selection");
      ExpectTrue(WebPDecode(w.mem, w.size, &a) == VP8_STATUS_OK,
                 "test_matrix: YUVA legacy decode");
      ExpectTrue(a.output.width == b2.output.width &&
                     a.output.height == b2.output.height &&
                     a.output.u.YUVA.y_stride == b2.output.u.YUVA.y_stride &&
                     a.output.u.YUVA.u_stride == b2.output.u.YUVA.u_stride,
                 "test_matrix: YUVA layout equal");
      {
        int identical = 1;
        for (y = 0; y < a.output.height && identical; ++y) {
          if (memcmp(a.output.u.YUVA.y + (size_t)y * a.output.u.YUVA.y_stride,
                     b2.output.u.YUVA.y + (size_t)y * b2.output.u.YUVA.y_stride,
                     a.output.width) != 0) {
            identical = 0;
          }
        }
        ExpectTrue(identical, "test_matrix: YUVA bytes unchanged by selection");
      }
      WebPFreeDecBuffer(&a.output);
      WebPFreeDecBuffer(&b2.output);
    }
    {
      WebPMemoryWriter lw;
      if (MakeLossless(15, 11, &lw)) {
        WebPDecoderConfig l0, l5;
        uint8_t *p0 = NULL, *p5 = NULL;
        int w0 = 0, h0 = 0, s0 = 0, w5 = 0, h5 = 0, s5 = 0;
        if (InitConfig(&l0, MODE_RGBA, 0) &&
            WebPDecode(lw.mem, lw.size, &l0) == VP8_STATUS_OK) {
          w0 = l0.output.width;
          h0 = l0.output.height;
          s0 = l0.output.u.RGBA.stride;
          p0 = CopyPlane(l0.output.u.RGBA.rgba, s0, w0 * 4, h0);
          WebPFreeDecBuffer(&l0.output);
        }
        if (InitConfig(&l5, MODE_RGBA, 5) &&
            WebPDecode(lw.mem, lw.size, &l5) == VP8_STATUS_OK) {
          w5 = l5.output.width;
          h5 = l5.output.height;
          s5 = l5.output.u.RGBA.stride;
          p5 = CopyPlane(l5.output.u.RGBA.rgba, s5, w5 * 4, h5);
          WebPFreeDecBuffer(&l5.output);
        }
        ExpectTrue(p0 != NULL && p5 != NULL && w0 == w5 && h0 == h5 &&
                       memcmp(p0, p5, (size_t)s0 * h0) == 0,
                   "test_matrix: lossless VP8L unchanged");
        free(p0);
        free(p5);
        WebPMemoryWriterClear(&lw);
      }
    }
  }

  FreePlanes(&p);
  WebPMemoryWriterClear(&w);
  (void)Split;
  (void)Bpp;
}

//------------------------------------------------------------------------------
// test_rows: geometry, fancy/point/scaling pipelines and incremental rows.

// A single row-geometry case: optional crop, optional scaling, given mode,
// conversion, and fancy/point. The expected image is built independently.
static void RowCase(const uint8_t *data, size_t size, const Planes *planes,
                    int conv, WEBP_CSP_MODE mode, int no_fancy, int crop,
                    int cl, int ct, int cw, int ch, int scale, int sw, int sh,
                    int with_alpha, const char *label) {
  DecodeSpec s;
  uint8_t *ref;
  int out_w, out_h, ostride, ok;
  memset(&s, 0, sizeof(s));
  s.mode = mode;
  s.conv = conv;
  s.no_fancy = no_fancy;
  s.use_cropping = crop;
  s.crop_left = cl;
  s.crop_top = ct;
  s.crop_width = crop ? cw : planes->width;
  s.crop_height = crop ? ch : planes->height;
  s.use_scaling = scale;
  s.scaled_width = sw;
  s.scaled_height = sh;
  if (scale) {
    out_w = sw;
    out_h = sh;
  } else {
    out_w = crop ? cw : planes->width;
    out_h = crop ? ch : planes->height;
  }
  ostride = out_w * 4;
  ref = (uint8_t *)malloc((size_t)ostride * out_h);
  if (ref == NULL) {
    unit_check(0, "test_rows: reference allocation");
    return;
  }
  if (scale) {
    ok = RefScaled(planes, &s, ref, ostride);
  } else if (no_fancy) {
    int y, x;
    ok = 1;
    for (y = 0; y < out_h && ok; ++y) {
      for (x = 0; x < out_w; ++x) {
        const int cy = (ct & ~1) + y;
        const int cx = (cl & ~1) + x;
        int r, g, b, a = 255;
        RefConvert(conv, planes->y[(size_t)cy * planes->y_stride + cx],
                   planes->u[(size_t)(cy >> 1) * planes->uv_stride + (cx >> 1)],
                   planes->v[(size_t)(cy >> 1) * planes->uv_stride + (cx >> 1)],
                   &r, &g, &b);
        if (planes->a != NULL) {
          a = planes->a[(size_t)cy * planes->a_stride + cx];
        }
        if (Premultiplied(mode)) {
          r = Premult(r, a);
          g = Premult(g, a);
          b = Premult(b, a);
        }
        PutRgba(ref + (size_t)y * ostride + (size_t)x * 4, r, g, b, a);
      }
    }
  } else {
    ok = RefFancy(planes, cl & ~1, ct & ~1, out_w, out_h, conv, with_alpha,
                  Premultiplied(mode), ref, ostride);
  }
  if (!ok) {
    unit_check(0, "test_rows: reference build");
    free(ref);
    return;
  }
  CheckSpec(data, size, &s, ref, ostride, out_w, out_h, with_alpha, label);
  free(ref);
}

void test_rows(void) {
  WebPMemoryWriter w, odd;
  Planes p, po;
  if (!MakeWebp(19, 17, 0, &w) || !DecodePlanes(w.mem, w.size, 1, &p)) {
    unit_check(0, "test_rows: fixture/planes");
    return;
  }

  // 1) Fancy upsampling (the default), point sampling, and their differences,
  //    plus odd image sizes.
  RowCase(w.mem, w.size, &p, 2, MODE_RGBA, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1,
          "rows fancy 19x17 conv2");
  RowCase(w.mem, w.size, &p, 3, MODE_RGBA, 1, 0, 0, 0, 0, 0, 0, 0, 0, 1,
          "rows point 19x17 conv3");

  // 2) Cropping (snapped to even), including crop then scale up/down.
  RowCase(w.mem, w.size, &p, 4, MODE_BGRA, 0, 1, 2, 4, 12, 10, 0, 0, 0, 1,
          "rows fancy crop 12x10 conv4");
  RowCase(w.mem, w.size, &p, 1, MODE_RGBA, 1, 1, 3, 2, 11, 9, 1, 16, 12, 1,
          "rows crop then scale-up conv1");
  RowCase(w.mem, w.size, &p, 2, MODE_RGBA, 1, 1, 0, 0, 19, 17, 1, 9, 7, 1,
          "rows scale-down 19x17->9x7 conv2");
  RowCase(w.mem, w.size, &p, 5, MODE_bgrA, 1, 1, 2, 2, 14, 12, 1, 5, 4, 1,
          "rows premultiplied downscale conv5");

  // 3) Odd sizes exercise the fancy edge cases (odd width, odd height) and
  //    all three pipelines.
  if (MakeWebp(13, 9, 0, &odd) && DecodePlanes(odd.mem, odd.size, 1, &po)) {
    RowCase(odd.mem, odd.size, &po, 3, MODE_RGBA, 0, 0, 0, 0, 0, 0, 0, 0, 0,
            1, "rows fancy odd 13x9 conv3");
    RowCase(odd.mem, odd.size, &po, 4, MODE_BGRA, 1, 0, 0, 0, 0, 0, 0, 0, 0,
            1, "rows point odd 13x9 conv4");
    RowCase(odd.mem, odd.size, &po, 2, MODE_RGBA, 1, 0, 0, 0, 0, 0, 1, 7, 5,
            1, "rows scale-down 13x9->7x5 conv2");
    RowCase(odd.mem, odd.size, &po, 5, MODE_BGRA, 1, 0, 0, 0, 0, 0, 1, 21, 17,
            1, "rows scale-up 13x9->21x17 conv5");
    RowCase(odd.mem, odd.size, &po, 1, MODE_RGBA, 1, 1, 2, 2, 10, 6, 1, 23, 13,
            1, "rows crop then scale-up odd target conv1");
    RowCase(odd.mem, odd.size, &po, 2, MODE_rgbA, 1, 0, 0, 0, 0, 0, 1, 9, 5,
            1, "rows premultiplied scaling conv2");

    RowCase(odd.mem, odd.size, &po, 3, MODE_bgrA, 0, 0, 0, 0, 0, 0, 0, 0, 0,
            1, "rows premultiplied fancy conv3");
    FreePlanes(&po);
    WebPMemoryWriterClear(&odd);
  } else {
    unit_check(0, "test_rows: odd fixture");
  }

  // 4) Non-premultiplied output keeps the converted color where alpha is 0.
  {
    int found = 0, nonzero = 0, y, x;
    uint8_t *pixels = NULL;
    int dw = 0, dh = 0, stride = 0;
    DecodeSpec s;
    memset(&s, 0, sizeof(s));
    s.mode = MODE_RGBA;
    s.conv = 2;
    if (DecodeFull(w.mem, w.size, &s, &dw, &dh, &pixels, &stride)) {
      for (y = 0; y < dh; ++y) {
        for (x = 0; x < dw; ++x) {
          const uint8_t *a = pixels + (size_t)y * stride + (size_t)x * 4;
          if (a[3] == 0) {
            ++found;
            if (a[0] != 0 || a[1] != 0 || a[2] != 0) ++nonzero;
          }
        }
      }
    }
    free(pixels);
    ExpectTrue(found > 0, "rows: transparent pixels exist");
    ExpectTrue(nonzero > 0, "rows: color retained at alpha==0");
  }

  // 5) Vertical flip keeps the same rows in reverse order.
  {
    uint8_t *plain = NULL, *flipped = NULL;
    int pw = 0, ph = 0, ps = 0, fw = 0, fh = 0, fs = 0;
    int y, ok = 1;
    DecodeSpec s;
    memset(&s, 0, sizeof(s));
    s.mode = MODE_BGRA;
    s.conv = 4;
    ExpectTrue(DecodeFull(w.mem, w.size, &s, &pw, &ph, &plain, &ps),
               "rows: no-flip decode");
    s.flip = 1;
    ExpectTrue(DecodeFull(w.mem, w.size, &s, &fw, &fh, &flipped, &fs),
               "rows: flip decode");
    ExpectTrue(pw == fw && ph == fh, "rows: flip dims");
    if (plain != NULL && flipped != NULL) {
      for (y = 0; ok && y < ph; ++y) {
        const uint8_t *a = plain + (size_t)y * ps;
        const uint8_t *b = flipped + (size_t)(ph - 1 - y) * fs;
        if (memcmp(a, b, (size_t)pw * 4) != 0) ok = 0;
      }
    }
    ExpectTrue(ok, "rows: flip reverses rows");
    free(plain);
    free(flipped);
  }

  // 6) A negative output stride behaves like a vertical flip.
  {
    WebPDecoderConfig c;
    uint8_t plain[19 * 17 * 4];
    uint8_t external[19 * 17 * 4 * 2];
    uint8_t *pixels = NULL;
    int pw = 0, ph = 0, ps = 0, y, identical = 1;
    DecodeSpec s;
    memset(&s, 0, sizeof(s));
    s.mode = MODE_RGBA;
    s.conv = 2;
    if (!DecodeFull(w.mem, w.size, &s, &pw, &ph, &pixels, &ps)) {
      unit_check(0, "rows: negative stride base decode");
    } else {
      memcpy(plain, pixels, sizeof(plain));
      free(pixels);
      ExpectEq(pw, 19, "rows: negative stride width");
      ExpectEq(ph, 17, "rows: negative stride height");
      memset(external, 0x5a, sizeof(external));
      if (InitConfig(&c, MODE_RGBA, 2)) {
        c.output.is_external_memory = 1;
        c.output.u.RGBA.stride = -19 * 4;
        c.output.u.RGBA.size = sizeof(external);
        c.output.u.RGBA.rgba = external + 16 * 19 * 4;  // last row first
        c.output.width = 19;
        c.output.height = 17;
        ExpectEq(WebPDecode(w.mem, w.size, &c), VP8_STATUS_OK,
                 "rows: negative stride decode");
        {
          const uint8_t *base = external + 16 * 19 * 4;
          for (y = 0; identical && y < 17; ++y) {
            if (memcmp(base + (ptrdiff_t)y * (-19 * 4),
                       plain + (size_t)y * 19 * 4, 19 * 4) != 0) {
              identical = 0;
            }
          }
        }
        ExpectTrue(identical, "rows: negative stride rows flipped");
        WebPFreeDecBuffer(&c.output);
      }
    }
  }

  // 7) Alpha is rescaled independently and written to the matching output
  //    rows; plain RGBA keeps the converted color even where the scaled alpha
  //    is 0. A fixture with a large transparent block makes this survive the
  //    downscale.
  {
    WebPMemoryWriter edge;
    if (MakeWebp(19, 17, 2, &edge)) {
      DecodeSpec s;
      uint8_t *pixels = NULL;
      int dw = 0, dh = 0, stride = 0;
      int zero_alpha = 0, zero_with_color = 0, y, x;
      memset(&s, 0, sizeof(s));
      s.mode = MODE_RGBA;
      s.conv = 3;
      s.use_scaling = 1;
      s.scaled_width = 11;
      s.scaled_height = 9;
      if (DecodeFull(edge.mem, edge.size, &s, &dw, &dh, &pixels, &stride)) {
        for (y = 0; y < dh; ++y) {
          for (x = 0; x < dw; ++x) {
            const uint8_t *a = pixels + (size_t)y * stride + (size_t)x * 4;
            if (a[3] == 0) {
              ++zero_alpha;
              if (a[0] || a[1] || a[2]) ++zero_with_color;
            }
          }
        }
      }
      free(pixels);
      ExpectTrue(zero_alpha > 0, "rows: scaled transparent pixels");
      ExpectTrue(zero_with_color > 0,
                 "rows: scaled RGBA keeps color at alpha==0");
      WebPMemoryWriterClear(&edge);
    }
  }

  // 8) Incremental decoding: every published row must match the final image,
  //    including across the fancy one-row delay; the final image must match
  //    the up-front decode.
  {
    DecodeSpec s;
    uint8_t *full = NULL;
    int fw = 0, fh = 0, fs = 0;
    WebPDecoderConfig c;
    WebPIDecoder *idec;
    size_t fed = 0;
    int ok = 1, saw_partial = 0, saw_published = 0;
    int status_ok = 1;
    memset(&s, 0, sizeof(s));
    s.mode = MODE_RGBA;
    s.conv = 3;  // fancy is on by default
    if (!DecodeFull(w.mem, w.size, &s, &fw, &fh, &full, &fs)) {
      unit_check(0, "rows: incremental reference decode");
      goto done_incremental;
    }
    if (!InitConfig(&c, MODE_RGBA, 3)) {
      unit_check(0, "rows: incremental config");
      goto done_incremental;
    }
    idec = WebPIDecode(NULL, 0, &c);
    ExpectTrue(idec != NULL, "rows: WebPIDecode");
    if (idec == NULL) goto done_incremental;
    while (fed < w.size) {
      size_t take = w.size - fed;
      VP8StatusCode status;
      int last_y = 0;
      uint8_t *published;
      const size_t chunk = (fed < 20) ? 2 : 31;
      if (take > chunk) take = chunk;
      status = WebPIAppend(idec, w.mem + fed, take);
      fed += take;
      if (status != VP8_STATUS_OK && status != VP8_STATUS_SUSPENDED) {
        status_ok = 0;
        break;
      }
      published = WebPIDecGetRGB(idec, &last_y, NULL, NULL, NULL);
      if (published != NULL && last_y > 0 && last_y <= fh) {
        const int as = c.output.u.RGBA.stride;
        int y;
        for (y = 0; y < last_y; ++y) {
          if (memcmp(published + (size_t)y * as, full + (size_t)y * fs,
                     (size_t)fw * 4) != 0) {
            ok = 0;
          }
        }
        if (last_y < fh) saw_partial = 1;
        saw_published = 1;
      }
      if (status == VP8_STATUS_OK) break;
    }
    ExpectTrue(status_ok, "rows: incremental append status");
    ExpectTrue(ok, "rows: incremental published rows match final");
    ExpectTrue(saw_partial && saw_published,
               "rows: incremental saw partial rows before the end");
    ExpectTrue(fed == w.size, "rows: incremental fed all bytes");
    {
      const WebPDecBuffer *area = WebPIDecodedArea(idec, NULL, NULL, NULL, NULL);
      ExpectTrue(area != NULL, "rows: WebPIDecodedArea");
      if (area != NULL) {
        ExpectEq(area->width, fw, "rows: incremental final width");
      }
    }
    WebPIDelete(idec);
done_incremental:
    free(full);
  }

  // 9) YUV/YUVA scaling keeps its historical behavior (including the luma
  //    premultiply rule) when a legal selection is present.
  {
    WebPDecoderConfig c0, c5;
    int identical = 1, y;
    if (InitConfig(&c0, MODE_YUVA, 0) && InitConfig(&c5, MODE_YUVA, 5)) {
      c0.options.use_scaling = 1;
      c0.options.scaled_width = 11;
      c0.options.scaled_height = 9;
      c5.options.use_scaling = 1;
      c5.options.scaled_width = 11;
      c5.options.scaled_height = 9;
      if (WebPDecode(w.mem, w.size, &c0) == VP8_STATUS_OK &&
          WebPDecode(w.mem, w.size, &c5) == VP8_STATUS_OK) {
        ExpectEq(c5.output.width, 11, "rows: YUVA scaled width");
        ExpectEq(c5.output.height, 9, "rows: YUVA scaled height");
        ExpectTrue(c0.output.width == c5.output.width &&
                       c0.output.height == c5.output.height &&
                       c0.output.u.YUVA.y_stride == c5.output.u.YUVA.y_stride &&
                       c0.output.u.YUVA.u_stride == c5.output.u.YUVA.u_stride &&
                       c0.output.u.YUVA.a_stride == c5.output.u.YUVA.a_stride,
                   "rows: YUVA scaled layout equal");
        for (y = 0; y < c5.output.height && identical; ++y) {
          const int st = c0.output.u.YUVA.y_stride;
          if (memcmp(c0.output.u.YUVA.y + (size_t)y * st,
                     c5.output.u.YUVA.y + (size_t)y * st, 11) != 0) {
            identical = 0;
          }
        }
        ExpectTrue(identical, "rows: YUVA scaling unchanged by selection");
      } else {
        unit_check(0, "rows: YUVA scaling decode");
      }
      WebPFreeDecBuffer(&c0.output);
      WebPFreeDecBuffer(&c5.output);
    }
  }

  FreePlanes(&p);
  WebPMemoryWriterClear(&w);
  (void)Split;
}

//------------------------------------------------------------------------------
// test_isolation: independent instances and concurrent decoding.

typedef struct {
  const uint8_t *data;
  size_t size;
  const uint8_t *expected;  // RGBA rows of the same conversion
  int expected_stride;
  int width, height;
  int conv;
  int iterations;
  int ok;
} ThreadJob;

static void *DecodeThread(void *arg) {
  ThreadJob *const job = (ThreadJob *)arg;
  int i, ok = 1;
  for (i = 0; i < job->iterations && ok; ++i) {
    WebPDecoderConfig c;
    if (!InitConfig(&c, MODE_RGBA, job->conv) ||
        WebPDecode(job->data, job->size, &c) != VP8_STATUS_OK) {
      ok = 0;
      break;
    }
    if (c.output.width != job->width || c.output.height != job->height) {
      ok = 0;
    } else {
      int y, x, max_err = 0;
      for (y = 0; y < job->height && ok; ++y) {
        for (x = 0; x < job->width; ++x) {
          const uint8_t *pa =
              c.output.u.RGBA.rgba + (size_t)y * c.output.u.RGBA.stride +
              4 * x;
          const uint8_t *pe = job->expected + (size_t)y * job->expected_stride +
                              4 * x;
          int ar, ag, ab, aa;
          GetPixel(pa, MODE_RGBA, &ar, &ag, &ab, &aa);
          if (abs(ar - pe[0]) > 2 || abs(ag - pe[1]) > 2 ||
              abs(ab - pe[2]) > 2 || aa != pe[3]) {
            ok = 0;
          }
          if (abs(ar - pe[0]) > max_err) max_err = abs(ar - pe[0]);
        }
      }
      g_sink += max_err;
    }
    WebPFreeDecBuffer(&c.output);
  }
  job->ok = ok;
  return NULL;
}

void test_isolation(void) {
  WebPMemoryWriter w;
  uint8_t *full1 = NULL, *full4 = NULL;
  int w1 = 0, h1 = 0, s1 = 0, w4 = 0, h4 = 0, s4 = 0;
  Planes p;
  DecodeSpec ds1, ds4;
  WebPDecoderConfig c1, c4;
  WebPIDecoder *i1 = NULL, *i4 = NULL;
  int ok = 1, have_refs = 0;

  if (!MakeWebp(23, 19, 1, &w) || !DecodePlanes(w.mem, w.size, 0, &p)) {
    unit_check(0, "isolation: fixture");
    return;
  }
  memset(&ds1, 0, sizeof(ds1));
  ds1.mode = MODE_RGBA;
  ds1.conv = 1;  // fancy upsampling kept (default)
  memset(&ds4, 0, sizeof(ds4));
  ds4.mode = MODE_RGBA;
  ds4.conv = 4;
  have_refs = DecodeFull(w.mem, w.size, &ds1, &w1, &h1, &full1, &s1) &&
              DecodeFull(w.mem, w.size, &ds4, &w4, &h4, &full4, &s4);
  ExpectTrue(have_refs, "isolation: reference decodes");
  ExpectTrue(full1 != NULL && full4 != NULL && w1 == w4 && h1 == h4,
             "isolation: reference dims");

  // 1) Two incremental decoders with different matrices, fed byte by byte in
  //    interleaved order. Published rows are checked as they appear and the
  //    final images afterwards: no cross-talk is allowed.
  if (!InitConfig(&c1, MODE_RGBA, 1) || !InitConfig(&c4, MODE_RGBA, 4)) {
    unit_check(0, "isolation: configs");
    goto cleanup;
  }
  ExpectTrue(InitConfig(&c1, MODE_RGBA, 1) && InitConfig(&c4, MODE_RGBA, 4),
             "isolation: configs re-init");
  i1 = WebPIDecode(NULL, 0, &c1);
  i4 = WebPIDecode(NULL, 0, &c4);
  ExpectTrue(i1 != NULL && i4 != NULL, "isolation: decoders created");
  if (i1 == NULL || i4 == NULL) goto cleanup;
  {
    size_t fed = 0;
    while (fed < w.size) {
      const uint8_t byte1 = w.mem[fed];
      const uint8_t byte4 = w.mem[fed];
      const VP8StatusCode s1s = WebPIAppend(i1, &byte1, 1);
      const VP8StatusCode s4s = WebPIAppend(i4, &byte4, 1);
      ++fed;
      if ((s1s != VP8_STATUS_OK && s1s != VP8_STATUS_SUSPENDED) ||
          (s4s != VP8_STATUS_OK && s4s != VP8_STATUS_SUSPENDED)) {
        ok = 0;
        break;
      }
      {
        int ly1 = 0, ly4 = 0, y;
        uint8_t *pub1 = WebPIDecGetRGB(i1, &ly1, NULL, NULL, NULL);
        uint8_t *pub4 = WebPIDecGetRGB(i4, &ly4, NULL, NULL, NULL);
        const int st1 = c1.output.u.RGBA.stride;
        const int st4 = c4.output.u.RGBA.stride;
        if (pub1 != NULL && ly1 > 0 && ly1 <= h1) {
          for (y = 0; y < ly1; ++y) {
            if (memcmp(pub1 + (size_t)y * st1, full1 + (size_t)y * s1,
                       (size_t)w1 * 4) != 0) {
              ok = 0;
            }
          }
        }
        if (pub4 != NULL && ly4 > 0 && ly4 <= h4) {
          for (y = 0; y < ly4; ++y) {
            if (memcmp(pub4 + (size_t)y * st4, full4 + (size_t)y * s4,
                       (size_t)w4 * 4) != 0) {
              ok = 0;
            }
          }
        }
      }
      if (s1s == VP8_STATUS_OK && s4s == VP8_STATUS_OK) break;
    }
    ExpectTrue(ok, "isolation: interleaved incremental rows");
  }
  {
    int y, same1 = 1, same4 = 1;
    const int st1 = c1.output.u.RGBA.stride;
    const int st4 = c4.output.u.RGBA.stride;
    ExpectTrue(c1.output.width == w1 && c1.output.height == h1,
               "isolation: conv1 final dims");
    ExpectTrue(c4.output.width == w4 && c4.output.height == h4,
               "isolation: conv4 final dims");
    ExpectTrue(st1 == s1 && st4 == s4, "isolation: final strides");
    for (y = 0; y < h1 && same1; ++y) {
      if (memcmp(c1.output.u.RGBA.rgba + (size_t)y * st1,
                 full1 + (size_t)y * s1, (size_t)w1 * 4) != 0) {
        same1 = 0;
      }
    }
    for (y = 0; y < h4 && same4; ++y) {
      if (memcmp(c4.output.u.RGBA.rgba + (size_t)y * st4,
                 full4 + (size_t)y * s4, (size_t)w4 * 4) != 0) {
        same4 = 0;
      }
    }
    ExpectTrue(same1, "isolation: conv1 final image unchanged");
    ExpectTrue(same4, "isolation: conv4 final image unchanged");
    ExpectTrue(memcmp(full1, full4, (size_t)s1 * h1) != 0,
               "isolation: different matrices give different images");
  }

  // 2) Concurrent full decodes, each with its own matrix.
  {
    ThreadJob job1, job4;
    pthread_t t1, t4;
    job1.data = w.mem;
    job1.size = w.size;
    job1.expected = full1;
    job1.expected_stride = s1;
    job1.width = w1;
    job1.height = h1;
    job1.conv = 1;
    job1.iterations = 5;
    job1.ok = 0;
    job4 = job1;
    job4.expected = full4;
    job4.expected_stride = s4;
    job4.width = w4;
    job4.height = h4;
    job4.conv = 4;
    ExpectTrue(pthread_create(&t1, NULL, DecodeThread, &job1) == 0,
               "isolation: thread 1 created");
    ExpectTrue(pthread_create(&t4, NULL, DecodeThread, &job4) == 0,
               "isolation: thread 2 created");
    (void)pthread_join(t1, NULL);
    (void)pthread_join(t4, NULL);
    ExpectTrue(job1.ok && job4.ok, "isolation: concurrent decodes stable");
    ExpectTrue(memcmp(full1, full4, (size_t)s1 * h1) != 0 ||
                   w1 * h1 == 0,
               "isolation: concurrent matrices differ");
  }

  // 3) A decoder keeps the selection it was created with; mutating the
  //    config afterwards has no effect (no mid-stream option changes).
  {
    WebPIDecoder *im;
    WebPDecoderConfig cm;
    if (InitConfig(&cm, MODE_RGBA, 5)) {
      im = WebPIDecode(NULL, 0, &cm);
      ExpectTrue(im != NULL, "isolation: fixed-selection decoder");
      if (im != NULL) {
        size_t f = 0;
        int stable = 1, y;
        int started = 0;
        while (f < w.size) {
          const size_t take = (w.size - f > 23) ? 23 : w.size - f;
          const VP8StatusCode st = WebPIAppend(im, w.mem + f, take);
          int ly = 0;
          f += take;
          if (st != VP8_STATUS_OK && st != VP8_STATUS_SUSPENDED) {
            stable = 0;
            break;
          }
          if (!started && WebPIDecGetRGB(im, &ly, NULL, NULL, NULL) != NULL &&
              ly > 0) {
            // The conversion has been set up and rows published; changing the
            // option now must have no effect on the rest of this instance.
            cm.options.yuv_conversion = 1;
            started = 1;
          }
          if (st == VP8_STATUS_OK) break;
        }
        ExpectTrue(started, "isolation: mid-stream mutation attempted");
        if (!started) cm.options.yuv_conversion = 1;  // fall back comparison
        {
          DecodeSpec d5;
          uint8_t *f5 = NULL;
          int fw = 0, fh = 0, fst = 0;
          memset(&d5, 0, sizeof(d5));
          d5.mode = MODE_RGBA;
          d5.conv = 5;
          if (stable && DecodeFull(w.mem, w.size, &d5, &fw, &fh, &f5, &fst)) {
            const int st = cm.output.u.RGBA.stride;
            ExpectTrue(cm.output.width == fw && cm.output.height == fh &&
                           st == fst,
                       "isolation: fixed-selection dims");
            for (y = 0; y < fh && stable; ++y) {
              if (memcmp(cm.output.u.RGBA.rgba + (size_t)y * st,
                         f5 + (size_t)y * fst, (size_t)fw * 4) != 0) {
                stable = 0;
              }
            }
            ExpectTrue(stable, "isolation: selection fixed at creation");
          } else {
            unit_check(0, "isolation: fixed-selection decode");
          }
          free(f5);
        }
        WebPIDelete(im);
      }
    }
  }

cleanup:
  if (i1 != NULL) WebPIDelete(i1);
  if (i4 != NULL) WebPIDelete(i4);
  WebPFreeDecBuffer(&c1.output);
  WebPFreeDecBuffer(&c4.output);
  FreePlanes(&p);
  WebPMemoryWriterClear(&w);
  free(full1);
  free(full4);
  (void)Split;
}
