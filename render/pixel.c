#include "pixel.h"

#ifdef PINGO_PIXEL_UINT8

extern Pixel pixelRandom() { return (Pixel){(uint8_t)rand()}; }

uint8_t pixelToUInt8(Pixel *p) { return p->g; }

extern Pixel pixelFromUInt8(uint8_t g) { return (Pixel){g}; }

extern Pixel pixelMul(Pixel p, float f) { return (Pixel){p.g * f}; }

extern Pixel pixelFromRGBA(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
  return (Pixel){((r + g + b) / 3)};
}
#endif

#ifdef PINGO_PIXEL_RGB888
extern Pixel pixelRandom() {
  return (Pixel){(uint8_t)rand(), (uint8_t)rand(), (uint8_t)rand()};
}

extern Pixel pixelMul(Pixel p, float f) {
  return (Pixel){p.r * f, p.g * f, p.b * f};
}

extern Pixel pixelFromUInt8(uint8_t g) { return (Pixel){g, g, g}; }

extern uint8_t pixelToUInt8(Pixel *p) { return (p->r + p->g + p->b) / 3; }

extern Pixel pixelFromRGBA(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
  (void)a;
  return (Pixel){r, g, b};
}
#endif

#ifdef PINGO_PIXEL_RGBA8888
extern Pixel pixelRandom() {
  return (Pixel){(uint8_t)rand(), (uint8_t)rand(), (uint8_t)rand(), 255};
}

extern Pixel pixelFromUInt8(uint8_t g) { return (Pixel){g, g, g, 255}; }
extern uint8_t pixelToUInt8(Pixel *p) { return (p->r + p->g + p->b) / 3; }

extern Pixel pixelFromRGBA(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
  return (Pixel){r, g, b, a};
}

extern Pixel pixelMul(Pixel p, float f) {
  return (Pixel){p.r * f, p.g * f, p.b * f, p.a};
}

#endif

#ifdef PINGO_PIXEL_BGRA8888
extern Pixel pixelRandom() {
  return (Pixel){(uint8_t)rand(), (uint8_t)rand(), (uint8_t)rand(), 255};
}

extern Pixel pixelFromUInt8(uint8_t g) { return (Pixel){g, g, g, 255}; }

extern uint8_t pixelToUInt8(Pixel *p) { return (p->r + p->g + p->b) / 3; }

extern Pixel pixelFromRGBA(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
  return (Pixel){b, g, r, a};
}

extern Pixel pixelMul(Pixel p, float f) {
  return (Pixel){p.b * f, p.g * f, p.r * f, p.a};
}

#endif

// Shared by every pixel format: clamp to the byte range, then truncate. The
// cast alone would wrap a lit value above 255.
static uint8_t pixelChannel(float v) {
  if (!(v > 0.0f))
    return 0; // also maps NaN to zero
  if (v >= 255.0f)
    return 255;
  return (uint8_t)v;
}

Pixel pixelShadeRGB(Pixel p, float r, float g, float b) {
#ifdef PINGO_PIXEL_UINT8
  return (Pixel){pixelChannel(p.g * r)};
#elif defined(PINGO_PIXEL_RGB565)
  return (Pixel){pixelChannel(p.red * r), pixelChannel(p.green * g),
                 pixelChannel(p.blue * b)};
#elif defined(PINGO_PIXEL_RGB888)
  return (Pixel){pixelChannel(p.r * r), pixelChannel(p.g * g),
                 pixelChannel(p.b * b)};
#elif defined(PINGO_PIXEL_BGRA8888)
  return (Pixel){pixelChannel(p.b * b), pixelChannel(p.g * g),
                 pixelChannel(p.r * r), p.a};
#else
  return (Pixel){pixelChannel(p.r * r), pixelChannel(p.g * g),
                 pixelChannel(p.b * b), p.a};
#endif
}

