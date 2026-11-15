#pragma once

#include <ModelTriangle.h>
#include <vector>

// Generate a Gerstner-wave ocean surface as a triangle mesh: an NxN grid over a
// `size`-wide square, displaced by a sum of directional Gerstner waves evaluated
// at `time` (so successive times animate the swell), with smooth per-vertex
// normals. The surface uses a blue-tinted glass material so the ray tracer gives
// Fresnel sky reflection + refraction, and the photon mapper focuses caustics
// through it.
std::vector<ModelTriangle> generateOcean(int gridN, float size, float time);
