#pragma once

#include <Canvas.h>

// A self-contained bloom post-process that operates purely on a Canvas, so it
// needs no renderer internals. Bright regions bleed a soft glow into their
// surroundings: a bright-pass extracts the highlights, a Gaussian blur spreads
// their energy, and the result is added back on top of the original image.

// Apply bloom to the canvas in place. threshold is the luminance (on 0..1)
// above which a pixel counts as a highlight; radius is the Gaussian spread of
// the glow in pixels; intensity scales how much of the blurred highlight is
// added back. Pixels are clamped to 255 after compositing.
void applyBloom(Canvas &canvas, float threshold = 0.8f, float radius = 8.0f, float intensity = 0.6f);
