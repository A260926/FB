#pragma once

#include "fwd.h"

#include <stdint.h>
#include <stdlib.h>

// Define one of the available formats
// #define PINGO_PIXEL_UINT8
// #define PINGO_PIXEL_RGB565
// #define PINGO_PIXEL_RGBA8888
#define PINGO_PIXEL_BGRA8888
// #define PINGO_PIXEL_RGB888

// Formats definitions:
#ifdef PINGO_PIXEL_UINT8
struct Pixel {
  uint8_t g;
};
#define PIXELBLACK                                                             \
  (Pixel) { 0 }
#define PIXELWHITE                                                             \
  (Pixel) { 255 }
#endif

#ifdef PINGO_PIXEL_RGB565
struct Pixel {
  uint8_t red : 5;
  uint8_t green : 6;
  uint8_t blue : 5;
};
#define PIXELBLACK                                                             \
  (Pixel) { 0 }
#define PIXELWHITE                                                             \
  (Pixel) { 255 }
#endif

#ifdef PINGO_PIXEL_RGB888
struct Pixel {
  uint8_t r;
  uint8_t g;
  uint8_t b;
};

#define PIXELBLACK                                                             \
  (Pixel) { 0, 0, 0 }
#define PIXELWHITE                                                             \
  (Pixel) { 255, 255, 255 }
#endif

#ifdef PINGO_PIXEL_RGBA8888
struct Pixel {
  uint8_t r;
  uint8_t g;
  uint8_t b;
  uint8_t a;
};

#define PIXELBLACK                                                             \
  (Pixel) { 0, 0, 0, 255 }
#define PIXELWHITE                                                             \
  (Pixel) { 255, 255, 255, 255 }
#endif

#ifdef PINGO_PIXEL_BGRA8888
struct Pixel {
  uint8_t b;
  uint8_t g;
  uint8_t r;
  uint8_t a;
};

#define PIXELBLACK                                                             \
  (Pixel) { 0, 0, 0, 255 }
#define PIXELWHITE                                                             \
  (Pixel) { 255, 255, 255, 255 }
#endif

// Interface
extern Pixel pixelRandom();
extern Pixel pixelFromUInt8(uint8_t);
extern uint8_t pixelToUInt8(Pixel *);
extern Pixel pixelFromRGBA(uint8_t r, uint8_t g, uint8_t b, uint8_t a);
extern Pixel pixelMul(Pixel p, float f);

/**
 * Multiplies the colour channels of `p` by the linear RGB `r`, `g`, `b`,
 * clamps each result to the byte range and truncates. The alpha of `p` is
 * kept unchanged.
 *
 * Unlike pixelMul, which takes one factor and saturates only through the
 * destination type, this is what point lighting uses: the light sum may be
 * above 1, so the clamp has to happen before the cast.
 */
extern Pixel pixelShadeRGB(Pixel p, float r, float g, float b);

