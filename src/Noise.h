#pragma once

#include <glm/glm.hpp>

// Classic Perlin noise. `perlin` returns roughly [-1, 1]; `fractalNoise` sums
// octaves (fBm) and returns roughly [0, 1]. SDL-free and unit-testable.
float perlin(float x, float y, float z);
float fractalNoise(const glm::vec3 &p, int octaves);

// Analytical-derivative variants. These return the noise value together with its
// exact gradient (computed analytically from the quintic fade and the trilinear
// interpolation, not by finite differences).
//
// `perlinNoiseD` returns the Perlin value in .x and the gradient
// (d/dx, d/dy, d/dz) in .y, .z, .w. The value matches `perlin` at every point.
//
// `fractalNoiseD` returns the fBm value in .x (matching `fractalNoise`, i.e.
// mapped to roughly [0, 1]) and the analytical gradient of that value in
// .y, .z, .w, accumulating each octave's gradient scaled by its frequency
// (chain rule) and amplitude.
glm::vec4 perlinNoiseD(const glm::vec3 &p);
glm::vec4 fractalNoiseD(const glm::vec3 &p, int octaves);

// Value noise. Unlike Perlin/gradient noise (which hashes lattice points to
// gradient vectors), value noise hashes each integer lattice point to a
// pseudo-random VALUE in [0, 1] and trilinearly interpolates those values using
// the same quintic fade (6t^5 - 15t^4 + 10t^3). The result is blockier/softer
// than gradient noise. `valueNoise` returns [0, 1]; `fractalValueNoise` sums
// octaves (fBm) and also returns [0, 1], matching fractalNoise's normalisation.
float valueNoise(const glm::vec3 &p);
float fractalValueNoise(const glm::vec3 &p, int octaves);
