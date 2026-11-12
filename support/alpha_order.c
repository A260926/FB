#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include "src/webp/decode.h"
#include "src/webp/encode.h"
static uint32_t le32(const uint8_t *p) {
  return (uint32_t)p[0] | (uint32_t)p[1]<<8 | (uint32_t)p[2]<<16 | (uint32_t)p[3]<<24;
}
int verify_alpha_order(const WebPMemoryWriter *w) {
  const uint8_t *vp8 = NULL;
  size_t vp8_size = 0;
  unsigned pixels = 0, alpha_different = 0, rgb_different = 0;
  const int widths[] = {19, 7, 25}, heights[] = {17, 5, 23};
  for (size_t pos = 12; pos + 8 <= w->size;) {
    const size_t n = le32(w->mem + pos + 4);
    if (n > w->size - pos - 8) return 10;
    if (w->mem[pos]=='V' && w->mem[pos+1]=='P' && w->mem[pos+2]=='8' && w->mem[pos+3]==' ') {
      vp8 = w->mem + pos + 8; vp8_size = n; break;
    }
    pos += 8 + n + (n & 1);
  }
  if (!vp8) return 11;
  for (unsigned k = 0; k < 3; ++k) {
    WebPDecoderConfig with_alpha, without_alpha;
    if (!WebPInitDecoderConfig(&with_alpha) || !WebPInitDecoderConfig(&without_alpha)) return 12;
    with_alpha.output.colorspace = without_alpha.output.colorspace = MODE_RGBA;
    if (k) {
      with_alpha.options.use_scaling = without_alpha.options.use_scaling = 1;
      with_alpha.options.scaled_width = without_alpha.options.scaled_width = widths[k];
      with_alpha.options.scaled_height = without_alpha.options.scaled_height = heights[k];
    }
    if (WebPDecode(w->mem, w->size, &with_alpha) != VP8_STATUS_OK ||
        WebPDecode(vp8, vp8_size, &without_alpha) != VP8_STATUS_OK) return 13;
    if (with_alpha.output.width != widths[k] || with_alpha.output.height != heights[k] ||
        without_alpha.output.width != widths[k] || without_alpha.output.height != heights[k]) return 14;
    for (int y=0; y<heights[k]; ++y) for (int x=0; x<widths[k]; ++x) {
      const uint8_t *a = with_alpha.output.u.RGBA.rgba + (ptrdiff_t)y*with_alpha.output.u.RGBA.stride + 4*x;
      const uint8_t *b = without_alpha.output.u.RGBA.rgba + (ptrdiff_t)y*without_alpha.output.u.RGBA.stride + 4*x;
      ++pixels; alpha_different += a[3] != b[3];
      for (int c=0; c<3; ++c) rgb_different += a[c] != b[c];
    }
    WebPFreeDecBuffer(&with_alpha.output); WebPFreeDecBuffer(&without_alpha.output);
  }
  printf("SAME_VP8_WITH_WITHOUT_ALPHA pixels=%u alpha_different=%u rgb_different=%u\n",pixels,alpha_different,rgb_different);
  return pixels != 933 || alpha_different != 933 || rgb_different != 0;
}
