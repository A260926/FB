#pragma once

// QMC.h
// Quasi-Monte-Carlo low-discrepancy sequences for less-noisy, faster
// converging sampling than plain uniform random numbers.
//
// These sequences (Halton, Hammersley, Sobol) fill the unit square/interval
// more evenly than pseudo-random points, which reduces variance in
// Monte-Carlo estimators such as those used in path tracing, ambient
// occlusion, soft shadows and area-light sampling.
//
// Everything here is dependency free: it only needs glm, <cmath> and <cstdint>.

#include <glm/glm.hpp>
#include <cstdint>

// van der Corput radical inverse of "index" in the given (prime) base.
// Returns a value in [0, 1). This mirrors the fractional digits of "index"
// written in the chosen base about the radix point, producing a well
// stratified 1D low-discrepancy sequence.
float radicalInverse(uint32_t base, uint32_t index);

// Halton sequence value for a given dimension and sample index.
// Dimension 0 uses base 2, dimension 1 uses base 3, dimension 2 uses base 5,
// and so on through the first ~16 primes. Higher dimensions fall back
// gracefully (they reuse the last known prime) rather than failing.
float halton(int dimension, uint32_t index);

// Hammersley 2D point for sample "index" out of a known total "count".
// Returns (index / count, radicalInverse base 2 of index). The Hammersley
// set is even lower discrepancy than Halton but requires knowing the total
// sample count up front.
glm::vec2 hammersley(uint32_t index, uint32_t count);

// Base-2 Sobol sequence, first dimension only. "scramble" is an optional
// XOR scramble seed (Owen-style random digit scrambling approximation) that
// lets each pixel/thread use a decorrelated but still low-discrepancy set.
// Returns a value in [0, 1).
float sobol1D(uint32_t index, uint32_t scramble = 0);

// Base-2 Sobol sequence, first two dimensions, as a 2D point in [0, 1)^2.
// "scramble" seeds both dimensions (the second dimension derives an
// independent scramble from it). This is the common form for 2D sampling.
glm::vec2 sobol2D(uint32_t index, uint32_t scramble = 0);

// Map a unit-square sample u in [0, 1)^2 to a cosine-weighted hemisphere
// direction oriented about "normal". Cosine weighting matches the diffuse
// BRDF importance so that returned directions are denser near the normal.
glm::vec3 cosineHemisphere(glm::vec2 u, const glm::vec3 &normal);

// Map a unit-square sample u in [0, 1)^2 to a point uniformly distributed on
// the unit disk, using the concentric (Shirley) mapping which preserves
// stratification better than the naive polar mapping.
glm::vec2 uniformDisk(glm::vec2 u);
