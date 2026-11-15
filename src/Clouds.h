#pragma once

#include <glm/glm.hpp>

// Procedural volumetric fractal clouds. SDL-free and unit-testable: depends only
// on the Perlin/fBm primitives in Noise.h and on glm.

// Fractal-noise (fBm) density field, remapped into [0, 1]. The sample point is
// advected by `time` so the field animates as if blown by a steady wind.
float cloudDensity(const glm::vec3 &p, float time);

// March the density field through a horizontal slab (y in [1, 3]). Transmittance
// is accumulated via Beer's-law extinction, with single-scatter in-scattering
// toward `sunDir`, composited over a sky-gradient background. Returns linear RGB
// in the [0, 255] range (not yet quantised to integers).
glm::vec3 raymarchClouds(const glm::vec3 &origin, const glm::vec3 &dir, const glm::vec3 &sunDir, float time);
