#pragma once

#include <cstdint>
#include <vector>

#include <glm/glm.hpp>

#include "Colour.h"

// Colour-space conversion + palette (median-cut colour quantization) utilities.
// SDL-free and unit-testable.

// Convert an RGB colour to HSV.
//   Input  `rgb`: components in [0, 1] (r, g, b).
//   Output `hsv`: h in [0, 360) degrees, s in [0, 1], v in [0, 1].
// For achromatic (grey) inputs hue is reported as 0.
glm::vec3 rgbToHsv(glm::vec3 rgb);

// Convert an HSV colour to RGB.
//   Input  `hsv`: h in [0, 360) degrees (wrapped if outside), s in [0, 1], v in [0, 1].
//   Output `rgb`: components in [0, 1] (r, g, b).
glm::vec3 hsvToRgb(glm::vec3 hsv);

// Build a representative colour palette from a set of pixels using median-cut
// colour quantization. `pixels` are packed 0xAARRGGBB (alpha ignored).
// Produces up to `paletteSize` colours (fewer if the image has fewer distinct
// colours). If `paletteSize` <= 0 or there are no pixels an empty palette is
// returned.
std::vector<Colour> buildPalette(const std::vector<uint32_t> &pixels, int paletteSize);

// Map each pixel (0xAARRGGBB) to the index of the nearest palette entry using
// squared Euclidean distance in RGB space. Returns one index per pixel. If the
// palette is empty, every index is 0.
std::vector<uint8_t> quantizeToPalette(const std::vector<uint32_t> &pixels, const std::vector<Colour> &palette);

// Expand an indexed image back to RGB and write it as a binary (P6) PPM.
// `indices` holds width*height palette indices in row-major order. Out-of-range
// indices and an empty palette are rendered as black.
void savePalettePPM(const char *path, int width, int height, const std::vector<uint8_t> &indices,
                    const std::vector<Colour> &palette);
