#pragma once

#include <ModelTriangle.h>
#include <vector>

// Generate a spectral (Tessendorf-style) ocean surface as a triangle mesh: an
// NxN grid over a `size`-wide square whose height field is synthesized from an
// oceanographic Phillips spectrum. Initial complex amplitudes
// h0(k) = (1/sqrt2)(xi_r + i xi_i) sqrt(Phillips(k)) are drawn once with a
// FIXED-seed Gaussian (deterministic, no time-based RNG), evolved to `time` via
// h(k,t) = h0(k) e^{i w t} + conj(h0(-k)) e^{-i w t} with the deep-water
// dispersion w = sqrt(g |k|), then inverse-transformed (direct inverse DFT) to
// the spatial height field. Vertices carry smooth finite-difference normals and
// a blue-tinted glass material, matching the Gerstner ocean's output convention.
std::vector<ModelTriangle> generateOceanFFT(int gridN, float size, float time);
